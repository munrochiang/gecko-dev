/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef IMAGECAPTURE_H
#define IMAGECAPTURE_H

#include "mozilla/DOMEventTargetHelper.h"
#include "mozilla/dom/ImageCaptureBinding.h"
#include "mozilla/Logging.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ImageBitmap.h"
#include "MediaEngine.h"

namespace mozilla {

#ifndef IC_LOG
LogModule* GetICLog();
#define IC_LOG(...) MOZ_LOG(GetICLog(), mozilla::LogLevel::Debug, (__VA_ARGS__))
#endif

namespace dom {

class Blob;
class MediaStreamTrack;

/**
 *  Implementation of https://dvcs.w3.org/hg/dap/raw-file/default/media-stream-
 *  capture/ImageCapture.html.
 *  The ImageCapture accepts a MediaStreamTrack as input source. The image will
 *  be sent back as a JPG format via Blob event.
 *
 *  All the functions in ImageCapture are run in main thread.
 *
 *  There are two ways to capture image, MediaEngineSource and MediaStreamGraph.
 *  When the implementation of MediaEngineSource supports TakePhoto() in platform
 *  like B2G, it uses the platform camera to grab image. Otherwise, it falls back
 *  to the MediaStreamGraph way.
 */

class ImageCapture final : public DOMEventTargetHelper
{
public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(ImageCapture, DOMEventTargetHelper)

  // WebIDL members.
  already_AddRefed<dom::Promise> TakePhoto(ErrorResult& aResult, ImageCaptureOutputFormat aFormat);
  already_AddRefed<dom::Promise> TakePhoto(ErrorResult& aResult);
  already_AddRefed<dom::Promise> GrabFrame(ErrorResult& aResult);

  // The MediaStream passed into the constructor.
  MediaStreamTrack* GetVideoStreamTrack() const;

  // nsWrapperCache member
  JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override
  {
    return ImageCaptureBinding::Wrap(aCx, this, aGivenProto);
  }

  // ImageCapture class members
  nsPIDOMWindow* GetParentObject() { return GetOwner(); }

  static already_AddRefed<ImageCapture> Constructor(const GlobalObject& aGlobal,
                                                    MediaStreamTrack& aTrack,
                                                    ErrorResult& aRv);

  ImageCapture(MediaStreamTrack* aMediaStreamTrack, nsPIDOMWindow* aOwnerWindow);

  // Post a Blob event to script.
  nsresult PostBlobEvent(Blob* aBlob);
  nsresult PostImageBitmapEvent(ImageBitmap* aImageBitmap);

  // Post an error event to script.
  // aErrorCode should be one of error codes defined in ImageCaptureError.h.
  // aReason is the nsresult which maps to a error string in dom/base/domerr.msg.
  nsresult PostErrorEvent(uint16_t aErrorCode, nsresult aReason = NS_OK, dom::Promise* aPromise = nullptr);

  bool CheckPrincipal();

protected:
  virtual ~ImageCapture();
  already_AddRefed<dom::Promise> CreatePromise(ErrorResult& aRv);
  void AbortPromise(RefPtr<dom::Promise>& aPromise);

  // Capture image by MediaEngine. If it's not support taking photo, this function
  // should return NS_ERROR_NOT_IMPLEMENTED.
  nsresult TakePhotoByMediaEngine(ImageCaptureOutputFormat aFormat);

  RefPtr<MediaStreamTrack> mMediaStreamTrack;
  RefPtr<dom::Promise> mTakePhotoPromise;
};

} // namespace dom
} // namespace mozilla

#endif // IMAGECAPTURE_H
