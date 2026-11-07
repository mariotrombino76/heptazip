// IsoOut.h
// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
//
// Writer of ISO 9660 images with Joliet and Rock Ridge (design note 0019).
// The whole layout is computed in memory first, then the image is written
// sequentially: no seeking on the output stream.

#ifndef ZIP7_INC_ARCHIVE_ISO_OUT_H
#define ZIP7_INC_ARCHIVE_ISO_OUT_H

#include "../../../Common/MyBuffer.h"
#include "../../../Common/MyString.h"
#include "../../../Common/MyVector.h"

#include "../../IStream.h"
#include "../IArchive.h"

namespace NArchive {
namespace NIso {

// one entry given by the update callback (or taken from the archive being rewritten)
struct COutItem
{
  UString Path;       // relative, '/' separated, no trailing slash
  AString SymLink;    // UTF-8 target of a symbolic link, empty otherwise
  UInt64 Size;        // 0 for directories and links
  UInt64 MTime;       // FILETIME (100 ns since 1601), 0 = not set
  UInt32 Mode;        // POSIX mode (type bits included), 0 = not set
  bool IsDir;
  bool NewData;       // data from the update callback (IndexInClient), else from the old archive (IndexInArc)
  UInt32 IndexInClient;
  UInt32 IndexInArc;

  COutItem(): Size(0), MTime(0), Mode(0), IsDir(false), NewData(false), IndexInClient(0), IndexInArc(0) {}
};

struct COutOptions
{
  AString VolumeId;   // d-characters, at most 32; empty = "ISOIMAGE"
  bool Joliet;
  bool RockRidge;
  UInt64 Now;         // FILETIME of the creation, for the descriptors and the implicit directories

  COutOptions(): Joliet(true), RockRidge(true), Now(0) {}
};

// Provides the data of the items: the callback for new data, the open archive for the
// rest (a rewrite of an existing image).
struct IOutDataSource
{
  virtual HRESULT GetStream(const COutItem &item, ISequentialInStream **stream) = 0;
  virtual HRESULT ReportResult(const COutItem &item, bool changed) = 0;
  virtual ~IOutDataSource() {}
};

HRESULT WriteIsoImage(ISequentialOutStream *outStream,
    const CObjectVector<COutItem> &items, const COutOptions &options,
    IOutDataSource &source, IArchiveUpdateCallback *progress);

}}

#endif
