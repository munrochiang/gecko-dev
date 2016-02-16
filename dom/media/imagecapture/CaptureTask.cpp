/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CaptureTask.h"
#include "mozilla/dom/ImageCapture.h"
#include "mozilla/dom/ImageCaptureError.h"
#include "mozilla/dom/ImageEncoder.h"
#include "mozilla/dom/MediaStreamTrack.h"
#include "mozilla/dom/ImageBitmap.h"
#include "gfxUtils.h"
#include "nsThreadUtils.h"

namespace mozilla {

nsresult
CaptureTask::BlobComplete(already_AddRefed<dom::Blob> aBlob, nsresult aRv)
{
  MOZ_ASSERT(NS_IsMainThread());

  DetachStream();

  nsresult rv;
  RefPtr<dom::Blob> blob(aBlob);

  // We have to set the parent because the blob has been generated with a valid one.
  if (blob) {
    blob = dom::Blob::Create(mImageCapture->GetParentObject(), blob->Impl());
  }

  if (mPrincipalChanged) {
    aRv = NS_ERROR_DOM_SECURITY_ERR;
    IC_LOG("MediaStream principal should not change during TakePhoto().");
  }

  if (NS_SUCCEEDED(aRv)) {
    rv = mImageCapture->PostBlobEvent(blob);
  } else {
    rv = mImageCapture->PostErrorEvent(dom::ImageCaptureError::PHOTO_ERROR, aRv);
  }

  // Ensure ImageCapture dereference on main thread here because the TakePhoto()
  // sequences stopped here.
  mImageCapture = nullptr;

  return rv;
}

nsresult
CaptureTask::ImageBitmapComplete(already_AddRefed<dom::ImageBitmap> aImageBitmap, nsresult aRv)
{
  MOZ_ASSERT(NS_IsMainThread());

  DetachStream();

  nsresult rv;
  RefPtr<dom::ImageBitmap> imagebitmap(aImageBitmap);

  if (mPrincipalChanged) {
    aRv = NS_ERROR_DOM_SECURITY_ERR;
    IC_LOG("MediaStream principal should not change during GrabFrame().");
  }

  if (NS_SUCCEEDED(aRv)) {
    rv = mImageCapture->PostImageBitmapEvent(imagebitmap);
  } else {
    rv = mImageCapture->PostErrorEvent(dom::ImageCaptureError::FRAME_ERROR, aRv);
  }

  // Ensure ImageCapture dereference on main thread here because the TakePhoto()
  // sequences stopped here.
  mImageCapture = nullptr;

  return rv;
}

void
CaptureTask::AttachStream()
{
  MOZ_ASSERT(NS_IsMainThread());

  RefPtr<dom::MediaStreamTrack> track = mImageCapture->GetVideoStreamTrack();

  RefPtr<DOMMediaStream> domStream = track->GetStream();
  domStream->AddPrincipalChangeObserver(this);

  RefPtr<MediaStream> stream = domStream->GetPlaybackStream();
  stream->AddListener(this);
}

void
CaptureTask::DetachStream()
{
  MOZ_ASSERT(NS_IsMainThread());

  RefPtr<dom::MediaStreamTrack> track = mImageCapture->GetVideoStreamTrack();

  RefPtr<DOMMediaStream> domStream = track->GetStream();
  domStream->RemovePrincipalChangeObserver(this);

  RefPtr<MediaStream> stream = domStream->GetPlaybackStream();
  stream->RemoveListener(this);
}

void
CaptureTask::PrincipalChanged(DOMMediaStream* aMediaStream)
{
  MOZ_ASSERT(NS_IsMainThread());
  mPrincipalChanged = true;
}

void
CaptureTask::NotifyQueuedTrackChanges(MediaStreamGraph* aGraph, TrackID aID,
                                      StreamTime aTrackOffset,
                                      uint32_t aTrackEvents,
                                      const MediaSegment& aQueuedMedia,
                                      MediaStream* aInputStream,
                                      TrackID aInputTrackID)
{
  if (mImageGrabbedOrTrackEnd) {
    return;
  }

  if (aTrackEvents == MediaStreamListener::TRACK_EVENT_ENDED) {
    PostTrackEndEvent();
    return;
  }

  // Callback for encoding complete, it calls on main thread.
  class EncodeComplete : public dom::EncodeCompleteCallback
  {
  public:
    explicit EncodeComplete(CaptureTask* aTask) : mTask(aTask) {}

    nsresult ReceiveBlob(already_AddRefed<dom::Blob> aBlob) override
    {
      RefPtr<dom::Blob> blob(aBlob);
      mTask->BlobComplete(blob.forget(), NS_OK);
      mTask = nullptr;
      return NS_OK;
    }

  protected:
    RefPtr<CaptureTask> mTask;
  };

  class ImageBitmapCompleteRunnable : public nsRunnable
  {
  public:
    explicit ImageBitmapCompleteRunnable(CaptureTask* aTask, already_AddRefed<layers::Image> aImage)
      : mTask(aTask)
      , mImage(aImage) {
    }

    NS_IMETHOD Run()
    {
      RefPtr<dom::ImageBitmap> imagebitmap = new dom::ImageBitmap(nullptr, mImage);
      mTask->ImageBitmapComplete(imagebitmap.forget(), NS_OK);
      mTask = nullptr;
      return NS_OK;
    }

  protected:
    RefPtr<CaptureTask> mTask;
    RefPtr<layers::Image> mImage;
  };

  if (aQueuedMedia.GetType() == MediaSegment::VIDEO && mTrackID == aID) {
    VideoSegment* video =
      const_cast<VideoSegment*> (static_cast<const VideoSegment*>(&aQueuedMedia));
    VideoSegment::ChunkIterator iter(*video);
    while (!iter.IsEnded()) {
      VideoChunk chunk = *iter;
      // Extract the first valid video frame.
      VideoFrame frame;
      if (!chunk.IsNull()) {
        RefPtr<layers::Image> image;
        if (chunk.mFrame.GetForceBlack()) {
          // Create a black image.
          image = VideoFrame::CreateBlackImage(chunk.mFrame.GetIntrinsicSize());
        } else {
          image = chunk.mFrame.GetImage();
        }
        MOZ_ASSERT(image);
        mImageGrabbedOrTrackEnd = true;

        if (ImageCaptureOutputFormat::JPEG == mFormat) {
          // Encode image.
          nsresult rv;
          nsAutoString type(NS_LITERAL_STRING("image/jpeg"));
          nsAutoString options;
          rv = dom::ImageEncoder::ExtractDataFromLayersImageAsync(
                                    type,
                                    options,
                                    false,
                                    image,
                                    new EncodeComplete(this));
          if (NS_FAILED(rv)) {
            PostTrackEndEvent();
          }
          return;
        } else if (ImageCaptureOutputFormat::YUV == mFormat) {
          NS_DispatchToMainThread(new ImageBitmapCompleteRunnable(this, image.forget()));
        }
      }
      iter.Next();
    }
  }
}

void
CaptureTask::NotifyEvent(MediaStreamGraph* aGraph, MediaStreamGraphEvent aEvent)
{
  if (((aEvent == EVENT_FINISHED) || (aEvent == EVENT_REMOVED)) &&
      !mImageGrabbedOrTrackEnd) {
    PostTrackEndEvent();
  }
}

void
CaptureTask::PostTrackEndEvent()
{
  mImageGrabbedOrTrackEnd = true;

  // Got track end or finish event, stop the task.
  class TrackEndRunnable : public nsRunnable
  {
  public:
    explicit TrackEndRunnable(CaptureTask* aTask, ImageCaptureOutputFormat aFormat)
      : mTask(aTask)
      , mFormat(aFormat) {}

    NS_IMETHOD Run()
    {
      if (ImageCaptureOutputFormat::JPEG == mFormat) {
        mTask->BlobComplete(nullptr, NS_ERROR_FAILURE);
      } else if (ImageCaptureOutputFormat::YUV == mFormat) {
        mTask->ImageBitmapComplete(nullptr, NS_ERROR_FAILURE);
      }
      mTask = nullptr;
      return NS_OK;
    }

  protected:
    RefPtr<CaptureTask> mTask;
    ImageCaptureOutputFormat mFormat;
  };

  IC_LOG("Got MediaStream track removed or finished event.");
  NS_DispatchToMainThread(new TrackEndRunnable(this, mFormat));
}

} // namespace mozilla
