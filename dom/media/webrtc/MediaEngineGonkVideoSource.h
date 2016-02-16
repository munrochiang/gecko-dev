/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MediaEngineGonkVideoSource_h_
#define MediaEngineGonkVideoSource_h_

#ifndef MOZ_B2G_CAMERA
#error MediaEngineGonkVideoSource is only available when MOZ_B2G_CAMERA is defined.
#endif

#include "CameraControlListener.h"
#include "MediaEngineCameraVideoSource.h"
#include "GonkNativeWindow.h"

#include "mozilla/Hal.h"
#include "mozilla/ReentrantMonitor.h"
#include "mozilla/dom/File.h"
#include "mozilla/layers/TextureClientRecycleAllocator.h"
#include "GonkCameraSource.h"

#pragma GCC visibility push(default)
#include <camera/camera2/ICameraDeviceCallbacks.h>
#include <camera/camera2/ICameraDeviceUser.h>
#include <gui/Surface.h>
#include <camera/CameraMetadata.h>
#include <gui/CpuConsumer.h>
#pragma GCC visibility pop

namespace android {
class MOZ_EXPORT MediaBuffer;
}

namespace mozilla {

/**
 * The B2G implementation of the MediaEngine interface.
 *
 * On B2G platform, member data may accessed from different thread after construction:
 *
 * MediaThread:
 * mState, mImage, mWidth, mHeight, mCapability, mPrefs, mDeviceName, mUniqueId, mInitDone,
 * mSources, mImageContainer, mSources, mState, mImage, mLastCapture.
 *
 * CameraThread:
 * mDOMCameraControl, mCaptureIndex, mCameraThread, mWindowId, mCameraManager,
 * mNativeCameraControl, mPreviewStream, mState, mLastCapture, mWidth, mHeight
 *
 * Where mWidth, mHeight, mImage, mPhotoCallbacks, mRotation, mCameraAngle and
 * mBackCamera are protected by mMonitor (in parent MediaEngineCameraVideoSource)
 * mState, mLastCapture is protected by mCallbackMonitor
 * Other variable is accessed only from single thread
 */
class MediaEngineGonkVideoSource : public MediaEngineCameraVideoSource
                                 , public mozilla::hal::ScreenConfigurationObserver
                                 , public CameraControlListener
#if ANDROID_VERSION >= 21
                                 , public android::GonkNativeWindowNewFrameCallback
                                 , public android::CpuConsumer::FrameAvailableListener
#endif
{
public:
  NS_DECL_ISUPPORTS_INHERITED

  MediaEngineGonkVideoSource(const android::sp<android::ICameraService>& aService,
    int aIndex, nsCString& aDeviceName, int aCameraAngle = 0)
    : MediaEngineCameraVideoSource(aIndex, "GonkCamera.Monitor")
    , mCallbackMonitor("GonkCamera.CallbackMonitor")
    , mCameraControl(nullptr)
    , mRotation(0)
    , mCameraAngle(aCameraAngle)
    , mBackCamera(false)
    , mOrientationChanged(true) // Correct the orientation at first time takePhoto.
    , mService(aService)
    , mDeviceCharacteristics(nullptr)
    , mPreviewStreamId(-1)
    , mJpegCaptureStreamId(-1)
    , mYCbCrCaptureStreamId(-1)
    , mPreviewRequestId(-1)
    , mCaptureRequestId(-1)
    {
      Init(aDeviceName);
    }

  nsresult Allocate(const dom::MediaTrackConstraints &aConstraints,
                    const MediaEnginePrefs &aPrefs,
                    const nsString& aDeviceId) override;
  nsresult Deallocate() override;
  nsresult Start(SourceMediaStream* aStream, TrackID aID) override;
  nsresult Stop(SourceMediaStream* aSource, TrackID aID) override;
  nsresult Restart(const dom::MediaTrackConstraints& aConstraints,
                   const MediaEnginePrefs &aPrefs,
                   const nsString& aDeviceId) override;
  void NotifyPull(MediaStreamGraph* aGraph,
                  SourceMediaStream* aSource,
                  TrackID aId,
                  StreamTime aDesiredTime) override;
  dom::MediaSourceEnum GetMediaSource() const override {
    return dom::MediaSourceEnum::Camera;
  }

  void OnHardwareStateChange(HardwareState aState, nsresult aReason) override;
  void GetRotation();
  bool OnNewPreviewFrame(layers::Image* aImage, uint32_t aWidth, uint32_t aHeight) override;
  void OnUserError(UserContext aContext, nsresult aError) override;
  void OnTakePictureComplete(const uint8_t* aData, uint32_t aLength, const nsAString& aMimeType) override;

  void AllocImpl();
  void DeallocImpl();
  void StartImpl(webrtc::CaptureCapability aCapability);
  void StopImpl();
  uint32_t ConvertPixelFormatToFOURCC(int aFormat);
  void RotateImage(layers::Image* aImage, uint32_t aWidth, uint32_t aHeight);
  void Notify(const mozilla::hal::ScreenConfiguration& aConfiguration);

  nsresult TakePhoto(PhotoCallback* aCallback, ImageCaptureOutputFormat aFormat) override;

  // It sets the correct photo orientation via camera parameter according to
  // current screen orientation.
  nsresult UpdatePhotoOrientation();

  // It adds aBuffer to current preview image and sends this image to MediaStreamDirectListener
  // via AppendToTrack(). Due to MediaBuffer is limited resource, it will clear
  // image's MediaBuffer by calling GonkCameraImage::ClearMediaBuffer() before leaving
  // this function.
  nsresult OnNewMediaBufferFrame(android::MediaBuffer* aBuffer);

  virtual void OnServiceDied();
#if ANDROID_VERSION >= 21
  virtual void OnNewFrame() override; // GonkNativeWindowNewFrameCallback
  void onFrameAvailable(const android::BufferItem& item); // CpuConsumer listener implementation
  void onJpegAvailable();
  void onYCbCrAvailable();
#endif

protected:
  ~MediaEngineGonkVideoSource()
  {
    Shutdown();
  }
  // Initialize the needed Video engine interfaces.
  void Init(nsCString& aDeviceName);
  void Shutdown();
  size_t NumCapabilities() override;
  // Initialize the recording frame (MediaBuffer) callback and Gonk camera.
  // MediaBuffer will transfers to MediaStreamGraph via AppendToTrack.
  nsresult InitDirectMediaBuffer();

  mozilla::ReentrantMonitor mCallbackMonitor; // Monitor for camera callback handling
  // This is only modified on MainThread (AllocImpl and DeallocImpl)
  RefPtr<ICameraControl> mCameraControl;
  RefPtr<dom::File> mLastCapture;

  android::sp<android::GonkCameraSource> mCameraSource;

  // These are protected by mMonitor in parent class
  nsTArray<RefPtr<PhotoCallback>> mPhotoCallbacks;
  int mRotation;
  int mCameraAngle; // See dom/base/ScreenOrientation.h
  bool mBackCamera;
  bool mOrientationChanged; // True when screen rotates.

  RefPtr<layers::TextureClientRecycleAllocator> mTextureClientAllocator;

  android::sp<android::ICameraService> mService;
  android::sp<android::ICameraDeviceUser> mDeviceUser;
  android::sp<android::ICameraDeviceCallbacks> mDeviceCallbacks;
  nsAutoPtr<android::CameraMetadata> mDeviceCharacteristics;
  android::sp<android::GonkNativeWindow> mPreviewWindow;
  android::sp<android::Surface> mPreviewSurface;
  android::sp<android::Surface> mJpegCaptureSurface;
  android::sp<android::Surface> mYCbCrCaptureSurface;
  int mPreviewStreamId;
  int mJpegCaptureStreamId;
  int mYCbCrCaptureStreamId;
  int mPreviewRequestId;
  int mCaptureRequestId;
  bool mSupportApi2;
  ImageCaptureOutputFormat mFormat;
  android::sp<android::CpuConsumer> mJpegCaptureConsumer;
  android::sp<android::CpuConsumer> mYCbCrCaptureConsumer;
#if ANDROID_VERSION >= 21
  android::sp<MediaEngineGonkVideoSource> mkungFuthis;
#endif
  enum {
    NO_STREAM = -1
  };
  size_t findJpegSize(uint8_t* jpegBuffer, size_t maxSize);
};

} // namespace mozilla

#endif // MediaEngineGonkVideoSource_h_
