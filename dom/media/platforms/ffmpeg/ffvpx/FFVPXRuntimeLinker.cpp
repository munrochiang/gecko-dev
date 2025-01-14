/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FFVPXRuntimeLinker.h"
#include "FFmpegLibWrapper.h"
#include "FFmpegLog.h"
#include "nsIFile.h"
#include "nsXPCOMPrivate.h" // for XUL_DLL
#include "prmem.h"
#include "prlink.h"

namespace mozilla
{

template <int V> class FFmpegDecoderModule
{
public:
  static already_AddRefed<PlatformDecoderModule> Create(FFmpegLibWrapper*);
};

static FFmpegLibWrapper sFFVPXLib;

FFVPXRuntimeLinker::LinkStatus FFVPXRuntimeLinker::sLinkStatus =
  LinkStatus_INIT;

static PRLibrary*
MozAVLink(const char* aName)
{
  PRLibSpec lspec;
  lspec.type = PR_LibSpec_Pathname;
  lspec.value.pathname = aName;
  return PR_LoadLibraryWithFlags(lspec, PR_LD_NOW | PR_LD_LOCAL);
}

/* static */ bool
FFVPXRuntimeLinker::Init()
{
  if (sLinkStatus) {
    return sLinkStatus == LinkStatus_SUCCEEDED;
  }

  MOZ_ASSERT(NS_IsMainThread());

  // We retrieve the path of the XUL library as this is where mozavcodec and
  // mozavutil libs are located.
  char* path =
    PR_GetLibraryFilePathname(XUL_DLL, (PRFuncPtr)&FFVPXRuntimeLinker::Init);
  if (!path) {
    return false;
  }
  nsCOMPtr<nsIFile> xulFile = do_CreateInstance(NS_LOCAL_FILE_CONTRACTID);
  if (!xulFile ||
      NS_FAILED(xulFile->InitWithNativePath(nsDependentCString(path)))) {
    PR_Free(path);
    return false;
  }
  PR_Free(path);

  nsCOMPtr<nsIFile> rootDir;
  if (NS_FAILED(xulFile->GetParent(getter_AddRefs(rootDir))) || !rootDir) {
    return false;
  }
  nsAutoCString rootPath;
  if (NS_FAILED(rootDir->GetNativePath(rootPath))) {
    return false;
  }

  char* libname = NULL;
  /* Get the platform-dependent library name of the module */
  libname = PR_GetLibraryName(rootPath.get(), "mozavutil");
  if (!libname) {
    return false;
  }
  sFFVPXLib.mAVUtilLib = MozAVLink(libname);
  PR_FreeLibraryName(libname);
  libname = PR_GetLibraryName(rootPath.get(), "mozavcodec");
  if (libname) {
    sFFVPXLib.mAVCodecLib = MozAVLink(libname);
    PR_FreeLibraryName(libname);
  }
  if (sFFVPXLib.Link()) {
    sLinkStatus = LinkStatus_SUCCEEDED;
    return true;
  }
  sLinkStatus = LinkStatus_FAILED;

  return false;
}

/* static */ already_AddRefed<PlatformDecoderModule>
FFVPXRuntimeLinker::CreateDecoderModule()
{
  if (!Init()) {
    return nullptr;
  }
  return FFmpegDecoderModule<FFVPX_VERSION>::Create(&sFFVPXLib);
}

} // namespace mozilla
