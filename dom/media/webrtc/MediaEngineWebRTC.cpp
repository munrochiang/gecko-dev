/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et ft=cpp : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsIPrefService.h"
#include "nsIPrefBranch.h"
#include "CamerasUtils.h"

#include "CSFLog.h"
#include "prenv.h"

#include "mozilla/Logging.h"

static mozilla::LogModule*
GetUserMediaLog()
{
  static mozilla::LazyLogModule sLog("GetUserMedia");
  return sLog;
}

#include "MediaEngineWebRTC.h"
#include "ImageContainer.h"
#include "nsIComponentRegistrar.h"
#include "MediaEngineTabVideoSource.h"
#include "MediaEngineRemoteVideoSource.h"
#include "CamerasChild.h"
#include "nsITabSource.h"
#include "MediaTrackConstraints.h"

#ifdef MOZ_WIDGET_ANDROID
#include "AndroidJNIWrapper.h"
#include "AndroidBridge.h"
#endif

#if defined(MOZ_B2G_CAMERA) && defined(MOZ_WIDGET_GONK)
#include "MediaEngineGonkVideoSource.h"
#include <binder/IServiceManager.h>
using namespace android;
#endif

#undef LOG
#define LOG(args) MOZ_LOG(GetUserMediaLog(), mozilla::LogLevel::Debug, args)

namespace mozilla {

// statics from AudioInputCubeb
nsTArray<int>* AudioInputCubeb::mDeviceIndexes;
nsTArray<nsCString>* AudioInputCubeb::mDeviceNames;
cubeb_device_collection* AudioInputCubeb::mDevices = nullptr;
bool AudioInputCubeb::mAnyInUse = false;

MediaEngineWebRTC::MediaEngineWebRTC(MediaEnginePrefs &aPrefs)
  : mMutex("mozilla::MediaEngineWebRTC"),
    mVoiceEngine(nullptr),
    mAudioInput(nullptr),
    mAudioEngineInit(false),
    mFullDuplex(aPrefs.mFullDuplex)
{
#ifndef MOZ_B2G_CAMERA
  nsCOMPtr<nsIComponentRegistrar> compMgr;
  NS_GetComponentRegistrar(getter_AddRefs(compMgr));
  if (compMgr) {
    compMgr->IsContractIDRegistered(NS_TABSOURCESERVICE_CONTRACTID, &mHasTabVideoSource);
  }
#else
#ifdef MOZ_WIDGET_GONK
  AsyncLatencyLogger::Get()->AddRef();
#endif
#endif
  // XXX
  gFarendObserver = new AudioOutputObserver();

  NS_NewNamedThread("AudioGUM", getter_AddRefs(mThread));
  MOZ_ASSERT(mThread);
}

#if defined(MOZ_B2G_CAMERA) && defined(MOZ_WIDGET_GONK)
void
MediaEngineWebRTC::EnsureCameraService()
{
  sp<IServiceManager> sm(defaultServiceManager());
  MOZ_ASSERT(sm.get());
  sp<IBinder> binder = sm->getService(String16("media.camera"));
  MOZ_ASSERT(binder.get());
  // Intentionally let android ref count > 0 to avoid destroying object.
  // (Lifecycle of this object will be managed by ourselves.)
  this->incStrong(this);
  __android_log_print(ANDROID_LOG_ERROR, "MediaEngineWebRTC", "linkToDeath(): %p-%p", binder.get(), this);
  binder->linkToDeath(this);
  mCameraService = interface_cast<ICameraService>(binder);
}

nsresult
MediaEngineWebRTC::GetNumberOfCameras(size_t& aDeviceCount)
{
  aDeviceCount = mCameraService->getNumberOfCameras();
  return NS_OK;
}

nsresult
MediaEngineWebRTC::GetCameraInfo(const size_t aDeviceNum, nsCString& aName, int& aOrientation)
{
  CameraInfo info;

  if (mCameraService->getCameraInfo(aDeviceNum, &info) != OK) {
    return NS_ERROR_FAILURE;
  }
  __android_log_print(ANDROID_LOG_ERROR, "MediaEngineWebRTC", "camera#%d facing:%d orientation:%d", aDeviceNum, info.facing, info.orientation);

  switch (info.facing) {
    case CAMERA_FACING_BACK:
      aName.AssignLiteral("back");
      break;

    case CAMERA_FACING_FRONT:
      aName.AssignLiteral("front");
      break;

    default:
      aName.AssignLiteral("extra-camera-");
      aName.AppendInt(aDeviceNum);
      break;
  }
  aOrientation = info.orientation;

  return NS_OK;
}

static PLDHashOperator
NotifyCameraServiceDied(const nsStringHashKey::KeyType aKey,
                        RefPtr<MediaEngineVideoSource>& aValue,
                        void* aUnused)
{
  MediaEngineGonkVideoSource* src = static_cast<MediaEngineGonkVideoSource*>(aValue.get());
  src->OnServiceDied();
  return PL_DHASH_NEXT;
}

void
MediaEngineWebRTC::binderDied(const android::wp<android::IBinder>& /*who*/)
{
  MutexAutoLock lock(mMutex);
  mVideoSources.Enumerate(NotifyCameraServiceDied, nullptr);
  mCameraService = nullptr;
}
#endif

void
MediaEngineWebRTC::EnumerateVideoDevices(dom::MediaSourceEnum aMediaSource,
                                         nsTArray<RefPtr<MediaEngineVideoSource> >* aVSources)
{
  // We spawn threads to handle gUM runnables, so we must protect the member vars
  MutexAutoLock lock(mMutex);

#if defined(MOZ_B2G_CAMERA) && defined(MOZ_WIDGET_GONK)
  if (aMediaSource != dom::MediaSourceEnum::Camera) {
    // only supports camera sources
    return;
  }

  /**
   * We still enumerate every time, in case a new device was plugged in since
   * the last call. TODO: Verify that WebRTC actually does deal with hotplugging
   * new devices (with or without new engine creation) and accordingly adjust.
   * Enumeration is not neccessary if GIPS reports the same set of devices
   * for a given instance of the engine. Likewise, if a device was plugged out,
   * mVideoSources must be updated.
   */

  EnsureCameraService();

  size_t num = 0;
  nsresult rv = GetNumberOfCameras(num);

  if (num <= 0 || rv != NS_OK) {
    return;
  }

  for (size_t i = 0; i < num; i++) {
    nsCString cameraName;
    int cameraAngle;
    rv = GetCameraInfo(i, cameraName, cameraAngle);

    if (NS_OK == rv) {
      RefPtr<MediaEngineVideoSource> vSource;
      NS_ConvertUTF8toUTF16 uuid(cameraName);
      if (mVideoSources.Get(uuid, getter_AddRefs(vSource))) {
	// We've already seen this device, just append.
	aVSources->AppendElement(vSource.get());
      } else {
	vSource = new MediaEngineGonkVideoSource(mCameraService, i, cameraName, cameraAngle);
	mVideoSources.Put(uuid, vSource); // Hashtable takes ownership.
	aVSources->AppendElement(vSource);
      }
    }
  }

  return;
#else
  mozilla::camera::CaptureEngine capEngine = mozilla::camera::InvalidEngine;

#ifdef MOZ_WIDGET_ANDROID
  // get the JVM
  JavaVM* jvm;
  JNIEnv* const env = jni::GetEnvForThread();
  MOZ_ALWAYS_TRUE(!env->GetJavaVM(&jvm));

  if (webrtc::VideoEngine::SetAndroidObjects(jvm) != 0) {
    LOG(("VieCapture:SetAndroidObjects Failed"));
    return;
  }
#endif

  switch (aMediaSource) {
    case dom::MediaSourceEnum::Window:
      capEngine = mozilla::camera::WinEngine;
      break;
    case dom::MediaSourceEnum::Application:
      capEngine = mozilla::camera::AppEngine;
      break;
    case dom::MediaSourceEnum::Screen:
      capEngine = mozilla::camera::ScreenEngine;
      break;
    case dom::MediaSourceEnum::Browser:
      capEngine = mozilla::camera::BrowserEngine;
      break;
    case dom::MediaSourceEnum::Camera:
      capEngine = mozilla::camera::CameraEngine;
      break;
    default:
      // BOOM
      MOZ_CRASH("No valid video engine");
      break;
  }

  /**
   * We still enumerate every time, in case a new device was plugged in since
   * the last call. TODO: Verify that WebRTC actually does deal with hotplugging
   * new devices (with or without new engine creation) and accordingly adjust.
   * Enumeration is not neccessary if GIPS reports the same set of devices
   * for a given instance of the engine. Likewise, if a device was plugged out,
   * mVideoSources must be updated.
   */
  int num;
  num = mozilla::camera::GetChildAndCall(
    &mozilla::camera::CamerasChild::NumberOfCaptureDevices,
    capEngine);
  if (num <= 0) {
    return;
  }

  for (int i = 0; i < num; i++) {
    char deviceName[MediaEngineSource::kMaxDeviceNameLength];
    char uniqueId[MediaEngineSource::kMaxUniqueIdLength];

    // paranoia
    deviceName[0] = '\0';
    uniqueId[0] = '\0';
    int error;

    error =  mozilla::camera::GetChildAndCall(
      &mozilla::camera::CamerasChild::GetCaptureDevice,
      capEngine,
      i, deviceName,
      sizeof(deviceName), uniqueId,
      sizeof(uniqueId));
    if (error) {
      LOG(("camera:GetCaptureDevice: Failed %d", error ));
      continue;
    }
#ifdef DEBUG
    LOG(("  Capture Device Index %d, Name %s", i, deviceName));

    webrtc::CaptureCapability cap;
    int numCaps = mozilla::camera::GetChildAndCall(
      &mozilla::camera::CamerasChild::NumberOfCapabilities,
      capEngine,
      uniqueId);
    LOG(("Number of Capabilities %d", numCaps));
    for (int j = 0; j < numCaps; j++) {
      if (mozilla::camera::GetChildAndCall(
            &mozilla::camera::CamerasChild::GetCaptureCapability,
            capEngine,
            uniqueId,
            j, cap) != 0) {
       break;
      }
      LOG(("type=%d width=%d height=%d maxFPS=%d",
           cap.rawType, cap.width, cap.height, cap.maxFPS ));
    }
#endif

    if (uniqueId[0] == '\0') {
      // In case a device doesn't set uniqueId!
      strncpy(uniqueId, deviceName, sizeof(uniqueId));
      uniqueId[sizeof(uniqueId)-1] = '\0'; // strncpy isn't safe
    }

    RefPtr<MediaEngineVideoSource> vSource;
    NS_ConvertUTF8toUTF16 uuid(uniqueId);
    if (mVideoSources.Get(uuid, getter_AddRefs(vSource))) {
      // We've already seen this device, just refresh and append.
      static_cast<MediaEngineRemoteVideoSource*>(vSource.get())->Refresh(i);
      aVSources->AppendElement(vSource.get());
    } else {
      vSource = new MediaEngineRemoteVideoSource(i, capEngine, aMediaSource);
      mVideoSources.Put(uuid, vSource); // Hashtable takes ownership.
      aVSources->AppendElement(vSource);
    }
  }

  if (mHasTabVideoSource || dom::MediaSourceEnum::Browser == aMediaSource) {
    aVSources->AppendElement(new MediaEngineTabVideoSource());
  }
#endif
}

void
MediaEngineWebRTC::EnumerateAudioDevices(dom::MediaSourceEnum aMediaSource,
                                         nsTArray<RefPtr<MediaEngineAudioSource> >* aASources)
{
  ScopedCustomReleasePtr<webrtc::VoEBase> ptrVoEBase;
  // We spawn threads to handle gUM runnables, so we must protect the member vars
  MutexAutoLock lock(mMutex);

  if (aMediaSource == dom::MediaSourceEnum::AudioCapture) {
    RefPtr<MediaEngineWebRTCAudioCaptureSource> audioCaptureSource =
      new MediaEngineWebRTCAudioCaptureSource(nullptr);
    aASources->AppendElement(audioCaptureSource);
    return;
  }

#ifdef MOZ_WIDGET_ANDROID
  jobject context = mozilla::AndroidBridge::Bridge()->GetGlobalContextRef();

  // get the JVM
  JavaVM* jvm;
  JNIEnv* const env = jni::GetEnvForThread();
  MOZ_ALWAYS_TRUE(!env->GetJavaVM(&jvm));

  if (webrtc::VoiceEngine::SetAndroidObjects(jvm, (void*)context) != 0) {
    LOG(("VoiceEngine:SetAndroidObjects Failed"));
    return;
  }
#endif

  if (!mVoiceEngine) {
    mVoiceEngine = webrtc::VoiceEngine::Create();
    if (!mVoiceEngine) {
      return;
    }
  }

  ptrVoEBase = webrtc::VoEBase::GetInterface(mVoiceEngine);
  if (!ptrVoEBase) {
    return;
  }

  if (!mAudioEngineInit) {
    if (ptrVoEBase->Init() < 0) {
      return;
    }
    mAudioEngineInit = true;
  }

  if (!mAudioInput) {
    if (mFullDuplex) {
      // The platform_supports_full_duplex.
      mAudioInput = new mozilla::AudioInputCubeb(mVoiceEngine);
    } else {
      mAudioInput = new mozilla::AudioInputWebRTC(mVoiceEngine);
    }
  }

  int nDevices = 0;
  mAudioInput->GetNumOfRecordingDevices(nDevices);
  int i;
#if defined(MOZ_WIDGET_ANDROID) || defined(MOZ_WIDGET_GONK)
  i = 0; // Bug 1037025 - let the OS handle defaulting for now on android/b2g
#else
  // -1 is "default communications device" depending on OS in webrtc.org code
  i = -1;
#endif
  for (; i < nDevices; i++) {
    // We use constants here because GetRecordingDeviceName takes char[128].
    char deviceName[128];
    char uniqueId[128];
    // paranoia; jingle doesn't bother with this
    deviceName[0] = '\0';
    uniqueId[0] = '\0';

    int error = mAudioInput->GetRecordingDeviceName(i, deviceName, uniqueId);
    if (error) {
      LOG((" VoEHardware:GetRecordingDeviceName: Failed %d", error));
      continue;
    }

    if (uniqueId[0] == '\0') {
      // Mac and Linux don't set uniqueId!
      MOZ_ASSERT(sizeof(deviceName) == sizeof(uniqueId)); // total paranoia
      strcpy(uniqueId, deviceName); // safe given assert and initialization/error-check
    }

    RefPtr<MediaEngineAudioSource> aSource;
    NS_ConvertUTF8toUTF16 uuid(uniqueId);
    if (mAudioSources.Get(uuid, getter_AddRefs(aSource))) {
      // We've already seen this device, just append.
      aASources->AppendElement(aSource.get());
    } else {
      AudioInput* audioinput = mAudioInput;
      if (mFullDuplex) {
        // The platform_supports_full_duplex.

        // For cubeb, it has state (the selected ID)
        // XXX just use the uniqueID for cubeb and support it everywhere, and get rid of this
        // XXX Small window where the device list/index could change!
        audioinput = new mozilla::AudioInputCubeb(mVoiceEngine, i);
      }
      aSource = new MediaEngineWebRTCMicrophoneSource(mThread, mVoiceEngine, audioinput,
                                                      i, deviceName, uniqueId);
      mAudioSources.Put(uuid, aSource); // Hashtable takes ownership.
      aASources->AppendElement(aSource);
    }
  }
}

void
MediaEngineWebRTC::Shutdown()
{
  // This is likely paranoia
  MutexAutoLock lock(mMutex);

  LOG(("%s", __FUNCTION__));
  // Shutdown all the sources, since we may have dangling references to the
  // sources in nsDOMUserMediaStreams waiting for GC/CC
  for (auto iter = mVideoSources.Iter(); !iter.Done(); iter.Next()) {
    MediaEngineVideoSource* source = iter.UserData();
    if (source) {
      source->Shutdown();
    }
  }
  for (auto iter = mAudioSources.Iter(); !iter.Done(); iter.Next()) {
    MediaEngineAudioSource* source = iter.UserData();
    if (source) {
      source->Shutdown();
    }
  }
  mVideoSources.Clear();
  mAudioSources.Clear();

  if (mVoiceEngine) {
    mVoiceEngine->SetTraceCallback(nullptr);
    webrtc::VoiceEngine::Delete(mVoiceEngine);
  }

  mVoiceEngine = nullptr;

  mozilla::camera::Shutdown();
  AudioInputCubeb::CleanupGlobalData();

  if (mThread) {
    mThread->Shutdown();
    mThread = nullptr;
  }
}

}
