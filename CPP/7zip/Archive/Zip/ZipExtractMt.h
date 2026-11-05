// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// ZipExtractMt.h
// Heptazip: parallel extraction of the items of a zip archive (issue #86, design 0017).
//
// The items of a zip are independent streams: worker threads read the packed data of an
// item into memory (under the lock of the archive stream) and decode it into a memory
// buffer with their own CZipDecoder; the extraction thread then replays, in the original
// order, the same callbacks as the sequential loop of CHandler::Extract() (GetStream,
// PrepareOperation, the writes, SetOperationResult), so what the caller observes is the
// same. The same scheme as the parallel extraction of the 7z folders (design 0001), whose
// building blocks (CMemoryBudget, CSharedInStream, CBufInStreamAt, CCachedPassword, the
// task pool) are reused.
//
// What is not decoded ahead goes through the sequential code with the real streams:
// multi-volume archives (the whole archive), items bigger than
// CMemoryBudget::kMaxParallelFolderUnpackSize, methods whose memory is not known from the
// headers (LZMA, PPMd, zstd, xz, LZFSE: the decoder of a big item may also be
// multithreaded itself), big unencrypted items that are stored or nearly so (they decode
// at the speed of a memory copy: a worker would only add two copies), items with an
// unavailable or unreadable local header, and any item whose decoding in the worker
// could not be reproduced exactly (an exception, or an output longer than the declared
// size): these are decoded again inline.

#ifndef ZIP7_INC_ZIP_EXTRACT_MT_H
#define ZIP7_INC_ZIP_EXTRACT_MT_H

#include "../../../Common/MyCom.h"

#include "../../IStream.h"
#include "../IArchive.h"

#include "../../Common/CreateCoder.h"
#include "../../Common/ProgressUtils.h"

#include "ZipIn.h"

namespace NArchive {
namespace NZip {

class CZipDecoder;

/* What Extract() knows: the archive, the items, the callback and the properties. */
struct CZipExtractCtx
{
  DECL_EXTERNAL_CODECS_LOC_VARS_DECL
  CInArchive &Archive;
  const CObjectVector<CItemEx> &Items;
  IArchiveExtractCallback *Callback;
  CLocalProgress *Lps;
  const UInt32 *Indices;     // NULL: all the items
  UInt32 NumItems;
  bool TestMode;
  UInt32 NumThreads;
  UInt64 MemUsage;

  CZipExtractCtx(
      DECL_EXTERNAL_CODECS_LOC_VARS
      CInArchive &archive, const CObjectVector<CItemEx> &items):
      #ifdef Z7_EXTERNAL_CODECS
      _externalCodecs(_externalCodecs),
      #endif
      Archive(archive), Items(items), Callback(NULL), Lps(NULL), Indices(NULL), NumItems(0),
      TestMode(false), NumThreads(1), MemUsage(0) {}

  UInt32 ItemIndex(UInt32 i) const { return Indices ? Indices[i] : i; }
};

#ifndef Z7_ST
/* Extracts all the requested items with worker threads where possible.
   Returns what Extract() must return. (wasUsed) = false: nothing was done (the
   parallel path was not applicable, e.g. a multi-volume archive or one thread), and
   the caller must run the sequential loop. */
HRESULT ExtractItemsParallel(CZipExtractCtx &ctx, CZipDecoder &inlineDecoder, bool &wasUsed);
#endif

}}

#endif
