/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ImageCapture.h"
#include "mozilla/dom/BlobEvent.h"
#include "mozilla/dom/DOMException.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/ImageCaptureError.h"
#include "mozilla/dom/ImageCaptureErrorEvent.h"
#include "mozilla/dom/ImageCaptureErrorEventBinding.h"
#include "mozilla/dom/MediaStreamTrack.h"
#include "nsIDocument.h"
#include "CaptureTask.h"

namespace mozilla {

LogModule* GetICLog()
{
  static LazyLogModule log("ImageCapture");
  return log;
}

namespace dom {

NS_IMPL_CYCLE_COLLECTION_INHERITED(ImageCapture, DOMEventTargetHelper,
                                   mMediaStreamTrack, mTakePhotoPromise)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(ImageCapture)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_ADDREF_INHERITED(ImageCapture, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(ImageCapture, DOMEventTargetHelper)

ImageCapture::ImageCapture(MediaStreamTrack* aMediaStreamTrack,
                           nsPIDOMWindow* aOwnerWindow)
  : DOMEventTargetHelper(aOwnerWindow)
{
  MOZ_ASSERT(aOwnerWindow);
  MOZ_ASSERT(aMediaStreamTrack);

  mMediaStreamTrack = aMediaStreamTrack;
}

ImageCapture::~ImageCapture()
{
  MOZ_ASSERT(NS_IsMainThread());
  AbortPromise(mTakePhotoPromise);
}

already_AddRefed<ImageCapture>
ImageCapture::Constructor(const GlobalObject& aGlobal,
                          MediaStreamTrack& aTrack,
                          ErrorResult& aRv)
{
  nsCOMPtr<nsPIDOMWindow> win = do_QueryInterface(aGlobal.GetAsSupports());
  if (!win) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<ImageCapture> object = new ImageCapture(&aTrack, win);

  return object.forget();
}

MediaStreamTrack*
ImageCapture::GetVideoStreamTrack() const
{
  return mMediaStreamTrack;
}

nsresult
ImageCapture::TakePhotoByMediaEngine(ImageCaptureOutputFormat aFormat)
{
  // Callback for TakPhoto(), it also monitor the principal. If principal
  // changes, it returns PHOTO_ERROR with security error.
  class TakePhotoCallback : public MediaEngineSource::PhotoCallback,
                            public DOMMediaStream::PrincipalChangeObserver
  {
  public:
    TakePhotoCallback(DOMMediaStream* aStream, ImageCapture* aImageCapture)
      : mStream(aStream)
      , mImageCapture(aImageCapture)
      , mPrincipalChanged(false)
    {
      MOZ_ASSERT(NS_IsMainThread());
      mStream->AddPrincipalChangeObserver(this);
    }

    void PrincipalChanged(DOMMediaStream* aMediaStream) override
    {
      mPrincipalChanged = true;
    }

    nsresult PhotoComplete(already_AddRefed<Blob> aBlob) override
    {
      RefPtr<Blob> blob = aBlob;

      if (mPrincipalChanged) {
        return PhotoError(NS_ERROR_DOM_SECURITY_ERR);
      }
      return mImageCapture->PostBlobEvent(blob);
    }

    nsresult FrameComplete(already_AddRefed<dom::ImageBitmap> aImageBitmap) override
    {
      RefPtr<dom::ImageBitmap> imagebitmap = aImageBitmap;

      if (mPrincipalChanged) {
        return FrameError(NS_ERROR_DOM_SECURITY_ERR);
      }
      return mImageCapture->PostImageBitmapEvent(imagebitmap);
    }

    nsresult PhotoError(nsresult aRv) override
    {
      return mImageCapture->PostErrorEvent(ImageCaptureError::PHOTO_ERROR, aRv);
    }

    nsresult FrameError(nsresult aRv) override
    {
      return mImageCapture->PostErrorEvent(ImageCaptureError::FRAME_ERROR, aRv);
    }

  protected:
    ~TakePhotoCallback()
    {
      MOZ_ASSERT(NS_IsMainThread());
      mStream->RemovePrincipalChangeObserver(this);
    }

    RefPtr<DOMMediaStream> mStream;
    RefPtr<ImageCapture> mImageCapture;
    bool mPrincipalChanged;
  };

  RefPtr<DOMMediaStream> domStream = mMediaStreamTrack->GetStream();
  DOMLocalMediaStream* domLocalStream = domStream->AsDOMLocalMediaStream();
  if (domLocalStream) {
    RefPtr<MediaEngineSource> mediaEngine =
      domLocalStream->GetMediaEngine(mMediaStreamTrack->GetTrackID());
    RefPtr<MediaEngineSource::PhotoCallback> callback =
      new TakePhotoCallback(domStream, this);
    return mediaEngine->TakePhoto(callback, aFormat);
  }

  return NS_ERROR_NOT_IMPLEMENTED;
}

already_AddRefed<Promise>
ImageCapture::TakePhoto(ErrorResult& aResult, ImageCaptureOutputFormat aFormat)
{
  RefPtr<Promise> promise = CreatePromise(aResult);
  if (aResult.Failed()) {
    return nullptr;
  }

  // According to spec, MediaStreamTrack.readyState must be "live"; however
  // gecko doesn't implement it yet (bug 910249). Instead of readyState, we
  // check MediaStreamTrack.enable before bug 910249 is fixed.
  // The error code should be INVALID_TRACK, but spec doesn't define it in
  // ImageCaptureError. So it returns PHOTO_ERROR here before spec updates.
  if (mTakePhotoPromise || !mMediaStreamTrack->Enabled()) {
    PostErrorEvent(ImageCaptureError::PHOTO_ERROR, NS_ERROR_FAILURE, promise);
    return promise.forget();
  }

  mTakePhotoPromise = promise;

  // Try if MediaEngine supports taking photo.
  nsresult rv = TakePhotoByMediaEngine(aFormat);

  // It falls back to MediaStreamGraph image capture if MediaEngine doesn't
  // support TakePhoto().
  if (rv == NS_ERROR_NOT_IMPLEMENTED) {
    IC_LOG("MediaEngine doesn't support TakePhoto(), it falls back to MediaStreamGraph.");
    RefPtr<CaptureTask> task =
      new CaptureTask(this, mMediaStreamTrack->GetTrackID(), aFormat);

    // It adds itself into MediaStreamGraph, so ImageCapture doesn't need to hold
    // the reference.
    task->AttachStream();
  }
  return promise.forget();
}

already_AddRefed<Promise>
ImageCapture::TakePhoto(ErrorResult& aResult)
{
  return TakePhoto(aResult, ImageCaptureOutputFormat::JPEG);
}

already_AddRefed<Promise>
ImageCapture::GrabFrame(ErrorResult& aResult)
{
  return TakePhoto(aResult, ImageCaptureOutputFormat::YUV);
}

nsresult
ImageCapture::PostBlobEvent(Blob* aBlob)
{
  MOZ_ASSERT(NS_IsMainThread());
  if (!CheckPrincipal()) {
    // Media is not same-origin, don't allow the data out.
    return PostErrorEvent(ImageCaptureError::PHOTO_ERROR, NS_ERROR_DOM_SECURITY_ERR);
  }

  RefPtr<Promise> promise = mTakePhotoPromise.forget();
  if (promise) {
    promise->MaybeResolve(aBlob);
    return NS_OK;
  } else {
    return NS_ERROR_FAILURE;
  }
}

nsresult
ImageCapture::PostImageBitmapEvent(ImageBitmap* aImageBitmap)
{
  MOZ_ASSERT(NS_IsMainThread());
  if (!CheckPrincipal()) {
    // Media is not same-origin, don't allow the data out.
    return PostErrorEvent(ImageCaptureError::FRAME_ERROR, NS_ERROR_DOM_SECURITY_ERR);
  }

  RefPtr<Promise> promise = mTakePhotoPromise.forget();
  if (promise) {
    promise->MaybeResolve(aImageBitmap);
     return NS_OK;
  } else {
    return NS_ERROR_FAILURE;
  }
}

nsresult
ImageCapture::PostErrorEvent(uint16_t aErrorCode, nsresult aReason, Promise* aPromise)
{
  MOZ_ASSERT(NS_IsMainThread());
  nsresult rv = CheckInnerWindowCorrectness();
  NS_ENSURE_SUCCESS(rv, rv);

  if (!aPromise)
    aPromise = mTakePhotoPromise;

  nsString errorMsg;
  if (NS_FAILED(aReason)) {
    nsCString name, message;
    rv = NS_GetNameAndMessageForDOMNSResult(aReason, name, message);
    if (NS_SUCCEEDED(rv)) {
      CopyASCIItoUTF16(message, errorMsg);
    }
  }

  RefPtr<ImageCaptureError> error =
    new ImageCaptureError(this, aErrorCode, errorMsg);

  if (aPromise) {
    aPromise->MaybeReject(error);
    return NS_OK;
  } else {
    return NS_ERROR_FAILURE;
  }
}

bool
ImageCapture::CheckPrincipal()
{
  MOZ_ASSERT(NS_IsMainThread());

  RefPtr<DOMMediaStream> ms = mMediaStreamTrack->GetStream();
  if (!ms) {
    return false;
  }
  nsCOMPtr<nsIPrincipal> principal = ms->GetPrincipal();

  if (!GetOwner()) {
    return false;
  }
  nsCOMPtr<nsIDocument> doc = GetOwner()->GetExtantDoc();
  if (!doc || !principal) {
    return false;
  }

  bool subsumes;
  if (NS_FAILED(doc->NodePrincipal()->Subsumes(principal, &subsumes))) {
    return false;
  }

  return subsumes;
}

already_AddRefed<Promise>
ImageCapture::CreatePromise(ErrorResult& aRv)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetOwner());
  MOZ_ASSERT(global);
  return Promise::Create(global, aRv);
}

void
ImageCapture::AbortPromise(RefPtr<Promise>& aPromise)
{
  RefPtr<Promise> promise = aPromise.forget();
  if (promise) {
    promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  }
}
} // namespace dom
} // namespace mozilla
