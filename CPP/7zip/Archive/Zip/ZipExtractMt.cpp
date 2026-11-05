// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// ZipExtractMt.cpp
// Heptazip: parallel extraction of the items of a zip archive, see ZipExtractMt.h and
// docs/design/0017-estrazione-zip-parallela.md.

#include "StdAfx.h"

#ifndef Z7_ST

#include <atomic>
#include <deque>
#include <memory>
#include <vector>

#include "../../../../C/Alloc.h"

#include "../../../Windows/Synchronization.h"

#include "../../Common/MtTaskPool.h"
#include "../../Common/StreamUtils.h"

#include "../7z/7zExtractMt.h"   // CMemoryBudget, CSharedInStream, CBufInStreamAt, CCachedPassword

#include "ZipDecoder.h"
#include "ZipExtractMt.h"

namespace NArchive {
namespace NZip {

/* the external codecs of the handler, from the context (as EXTERNAL_CODECS_LOC_VARS) */
#ifdef Z7_EXTERNAL_CODECS
#define EXTERNAL_CODECS_LOC_VARS_OF(ctx) (ctx)._externalCodecs,
#else
#define EXTERNAL_CODECS_LOC_VARS_OF(ctx)
#endif

using N7z::CMemoryBudget;
using N7z::CSharedInStream;
using N7z::CBufInStreamAt;
#ifndef Z7_NO_CRYPTO
using N7z::CCachedPassword;
#endif

/* The decoder of an item needs about this much beyond its output: the window of
   Deflate/Deflate64 (1 MiB, design 0011) and the tables, the block of BZip2. For the
   methods not listed the memory is not known from the headers (LZMA and PPMd have their
   parameters in the data, zstd and xz their own frames) and the item is decoded inline. */
static bool GetDecoderMemory(unsigned method, UInt64 &mem)
{
  switch (method)
  {
    case NFileHeader::NCompressionMethod::kStore:     mem = 0; return true;
    case NFileHeader::NCompressionMethod::kShrink:
    case NFileHeader::NCompressionMethod::kImplode:   mem = (UInt64)1 << 20; return true;
    case NFileHeader::NCompressionMethod::kDeflate:
    case NFileHeader::NCompressionMethod::kDeflate64: mem = (UInt64)2 << 20; return true;
    case NFileHeader::NCompressionMethod::kBZip2:     mem = (UInt64)8 << 20; return true;
    default: return false;
  }
}


/* Output of the decoder of a worker: a buffer of the declared size of the item. If the
   decoder writes more (a corrupted item, or a wrong size in the headers), the data would
   go to the real stream in the sequential extraction and what follows depends on the
   stream: the worker gives up (Overflow) and the item is decoded again inline. */
Z7_CLASS_IMP_COM_1(
  CItemBufOutStream
  , ISequentialOutStream
)
  Byte *_buf;
  size_t _size;
  size_t _pos;
public:
  bool Overflow;
  CItemBufOutStream(): _buf(NULL), _size(0), _pos(0), Overflow(false) {}
  void Init(Byte *buf, size_t size) { _buf = buf; _size = size; _pos = 0; Overflow = false; }
  size_t GetPos() const { return _pos; }
};

Z7_COM7F_IMF(CItemBufOutStream::Write(const void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (size == 0)
    return S_OK;
  if (size > _size - _pos)
  {
    Overflow = true;
    return E_FAIL;
  }
  memcpy(_buf + _pos, data, size);
  _pos += size;
  if (processedSize)
    *processedSize = size;
  return S_OK;
}


class CParallelZipExtract;

/* An item decoded ahead by a worker thread. */
class CItemDecodeTask Z7_final: public CMtTask
{
  Z7_CLASS_NO_COPY(CItemDecodeTask)
public:
  CParallelZipExtract *Owner;
  UInt32 Pos;             // position in the list of the requested items
  UInt32 Index;           // index of the item in the archive
  CItemEx Item;           // with the local header read (if it could be)
  // what the planning found, replayed in order by the extraction thread
  bool IsLocalOffsetOK;
  HRESULT LocalRes;       // Read_LocalItem_After_CdItem(): S_OK, S_FALSE (headers) or an error
  bool IsAvail;
  bool HeadersError;
  // parallel decoding
  bool IsParallel;        // can be decoded ahead by a worker
  bool IsAdmitted;        // submitted to the pool
  bool WaitInline;        // no buffers for it: no admission past it until it's processed
  bool IsEncrypted;
  bool CostReserved;      // (Cost) is reserved in the memory budget until the replay
  UInt64 Cost;            // reserved in the memory budget
  UInt64 PackPos;         // position of the packed data in the archive stream
  Byte *PackBuf;
  Byte *OutBuf;
  size_t PackLoaded;
  HRESULT PackReadRes;
  // results: written by the worker before (Finished), read after it
  HRESULT DecodeRes;
  Int32 OpRes;
  size_t OutSize;
  bool NeedInline;        // the worker could not reproduce the sequential decoding exactly
  // progress: written by the worker, read by the extraction thread
  std::atomic<UInt64> InProcessed;
  std::atomic<UInt64> OutProcessed;
  std::atomic<bool> Finished;

  CItemDecodeTask():
      Owner(NULL), Pos(0), Index(0),
      IsLocalOffsetOK(true), LocalRes(S_OK), IsAvail(true), HeadersError(false),
      IsParallel(false), IsAdmitted(false), WaitInline(false), IsEncrypted(false), CostReserved(false),
      Cost(0), PackPos(0), PackBuf(NULL), OutBuf(NULL), PackLoaded(0), PackReadRes(S_OK),
      DecodeRes(S_OK), OpRes(NExtract::NOperationResult::kDataError), OutSize(0), NeedInline(false),
      InProcessed(0), OutProcessed(0), Finished(false)
      {}
  ~CItemDecodeTask() Z7_override { FreeBuffers(); }
  void FreeBuffers()
  {
    MyFree(PackBuf);  PackBuf = NULL;
    MyFree(OutBuf);   OutBuf = NULL;
  }
  bool AllocBuffers()
  {
    PackBuf = (Byte *)MyAlloc((size_t)Item.PackSize);
    OutBuf = (Byte *)MyAlloc((size_t)Item.Size);
    return (PackBuf || Item.PackSize == 0) && (OutBuf || Item.Size == 0);
  }
  void Run(unsigned workerIndex) Z7_override;
  /* the decoding itself, with the decoder (workerIndex) of the owner */
  void Decode(unsigned workerIndex);
};


/* Progress of a task: stores the processed sizes and handles cancellation. */
Z7_CLASS_IMP_COM_1(
  CItemDecodeProgress
  , ICompressProgressInfo
)
public:
  CItemDecodeTask *Task;
  CItemDecodeProgress(): Task(NULL) {}
};


class CParallelZipExtract
{
  Z7_CLASS_NO_COPY(CParallelZipExtract)
public:
  CZipExtractCtx &Ctx;
  CZipDecoder &InlineDecoder;
  /* The requested items planned and not yet replayed, in order: front() is the item
     processed now. A member (not a local of Run()): the destructor stops the workers
     before the tasks are freed. */
  std::deque< std::unique_ptr<CItemDecodeTask> > Window;
  CMtTaskPool Pool;
  std::vector< std::unique_ptr<CZipDecoder> > Decoders; // one per worker thread
  CSharedInStream Shared;   // the archive stream, read by the workers under a lock
  NWindows::NSynchronization::CAutoResetEvent Wakeup; // a task has finished or progressed
  std::atomic<bool> Abort;
  std::atomic<CItemDecodeTask *> HeadTask;
  std::atomic<unsigned> NumRunning;  // tasks submitted and not finished: they read the archive stream
  // admission state (Run and AdmitAhead)
  CMemoryBudget Budget;
  unsigned MaxInFlight;
  unsigned NumInFlight;     // admitted and not yet replayed
  size_t AdmitPos;          // Window[AdmitPos] is the first item not considered for admission
  CMyComPtr<ICryptoGetTextPassword> GetTextPassword; // of the callback, for the password request
  #ifndef Z7_NO_CRYPTO
  CCachedPassword *PasswordSpec;
  CMyComPtr<ICryptoGetTextPassword> Password;
  #endif

  CParallelZipExtract(CZipExtractCtx &ctx, CZipDecoder &inlineDecoder):
      Ctx(ctx), InlineDecoder(inlineDecoder), Abort(false), HeadTask(NULL), NumRunning(0),
      MaxInFlight(0), NumInFlight(0), AdmitPos(0)
  {
    Shared.Init(ctx.Archive.GetReadStream());
    #ifndef Z7_NO_CRYPTO
    PasswordSpec = new CCachedPassword;
    Password = PasswordSpec;
    #endif
  }
  /* Stops the workers on any exit path, before the tasks are freed. */
  ~CParallelZipExtract()
  {
    Abort = true;
    Pool.Stop();
  }

  void Plan(UInt32 i, CItemDecodeTask &task);
  HRESULT WaitWorkersIdle(CLocalProgress *lps, CItemDecodeTask *head);
  HRESULT ReplayItem(CItemDecodeTask &task, UInt64 &curUnpacked, UInt64 &curPacked);
  HRESULT DecodeInline(const CItemDecodeTask &task, ISequentialOutStream *realOutStream, Int32 &opRes);
  HRESULT FetchPasswordIfNeeded(const CItemDecodeTask &task);
  HRESULT AdmitAhead();
  HRESULT Run(bool &wasUsed);
};


Z7_COM7F_IMF(CItemDecodeProgress::SetRatioInfo(const UInt64 *inSize, const UInt64 *outSize))
{
  if (inSize)
    Task->InProcessed = *inSize;
  if (outSize)
    Task->OutProcessed = *outSize;
  CParallelZipExtract *owner = Task->Owner;
  if (owner->Abort)
    return E_ABORT;
  if (owner->HeadTask == Task)
    owner->Wakeup.Set(); // the extraction thread reports the progress of the head task
  return S_OK;
}


/* Worker: reads the packed data of the item (one read under the lock) and decodes it
   into the output buffer with the decoder of this worker. The result is what the
   sequential loop would have got from CZipDecoder::Decode(), unless something could not
   be reproduced (NeedInline). */
void CItemDecodeTask::Run(unsigned workerIndex)
{
  Decode(workerIndex);
  Owner->NumRunning--;
  Owner->Wakeup.Set();
}

void CItemDecodeTask::Decode(unsigned workerIndex)
{
  CParallelZipExtract *owner = Owner;
  try
  {
    // ---------- the packed data ----------
    {
      size_t size = (size_t)Item.PackSize;
      PackReadRes = owner->Shared.ReadFullAt(PackPos, PackBuf, size);
      PackLoaded = size;
    }
    if (owner->Abort)
    {
      DecodeRes = E_ABORT;
    }
    else
    {
      CMyComPtr2_Create<ISequentialInStream, CBufInStreamAt> packStream;
      /* if the read stopped before PackSize (end of the archive or a read error), the
         decoder gets the same data and the same result as from the archive stream */
      packStream->Init(PackBuf, PackLoaded, 0, PackReadRes);
      CMyComPtr2_Create<ISequentialOutStream, CItemBufOutStream> outStream;
      outStream->Init(OutBuf, (size_t)Item.Size);
      CMyComPtr2_Create<ICompressProgressInfo, CItemDecodeProgress> progress;
      progress->Task = this;
      /* the password provider was set in the extraction thread (Run()): the reference
         count of the COM objects of 7-Zip is not atomic */
      CZipDecoder &decoder = *owner->Decoders[workerIndex];
      Int32 opRes = NExtract::NOperationResult::kDataError;
      DecodeRes = decoder.Decode(
          EXTERNAL_CODECS_LOC_VARS_OF(owner->Ctx)
          owner->Ctx.Archive, Item, outStream, NULL, progress,
          1 /* threads: the parallelism is between the items */, owner->Ctx.MemUsage,
          opRes, packStream);
      OpRes = opRes;
      OutSize = outStream->GetPos();
      if (outStream->Overflow)
        NeedInline = true;
    }
  }
  catch (...)
  {
    NeedInline = true;
  }
  Finished = true;
}


/* Planning of one requested item: what the sequential loop does before PrepareOperation
   (except the callbacks), and whether a worker can decode it. It reads the archive
   stream (the local header): only while no worker runs. */
void CParallelZipExtract::Plan(UInt32 i, CItemDecodeTask &task)
{
  task.Owner = this;
  task.Pos = i;
  task.Index = Ctx.ItemIndex(i);
  task.Item = Ctx.Items[task.Index];
  CItemEx &item = task.Item;
  task.IsLocalOffsetOK = Ctx.Archive.IsLocalOffsetOK(item);
  task.IsParallel = false;
  if (!task.IsLocalOffsetOK)
    return;
  if (!item.FromLocal)
  {
    task.LocalRes = Ctx.Archive.Read_LocalItem_After_CdItem(item, task.IsAvail, task.HeadersError);
    if (task.LocalRes != S_OK)
      return;
  }
  if (item.IsDir())
    return;
  if (item.IsStrongEncrypted())
    return; // its own header handling with the real stream: inline
  unsigned method = item.Method;
  if (method == NFileHeader::NCompressionMethod::kWzAES)
  {
    // WinZip AES: the compression method is in the extra field (the same id is LZFSE)
    CWzAesExtra aesField;
    if (!item.GetMainExtra().GetWzAes(aesField) || !item.IsEncrypted())
      return;
    method = aesField.Method;
  }
  UInt64 decoderMem;
  if (!GetDecoderMemory(method, decoderMem))
    return;
  if (CMemoryBudget::IsBigFolder(item.Size) || item.Size >= ((UInt64)1 << 32) || item.PackSize >= ((UInt64)1 << 32))
    return;
  if (Ctx.Archive.UseDisk_in_SingleVol && item.Disk != Ctx.Archive.EcdVolIndex)
    return; // GetItemStream() gives no stream: kUnavailable, inline
  task.IsEncrypted = item.IsEncrypted();
  if (!task.IsEncrypted && item.Size > ((UInt64)1 << 20))
  {
    /* a big item that is stored, or compressed by less than 5% (the deflate encoders
       emit stored blocks for incompressible data), decodes at the speed of a memory
       copy: a worker would gain nothing and spend two copies of its size in memory
       (measured: 50 MB at 98.7%, 14 ms in one thread, 42 ms through a worker). It
       stays on the sequential path, with the real streams. An encrypted item is
       decrypted at a few hundred MB/s: worth a worker whatever its method. */
    if (method == NFileHeader::NCompressionMethod::kStore || item.PackSize >= item.Size - item.Size / 20)
      return;
  }
  task.PackPos = (UInt64)((Int64)item.GetDataPosition() + Ctx.Archive.ArcInfo.Base);
  task.Cost = item.PackSize + item.Size + decoderMem + ((UInt64)1 << 16);
  task.IsParallel = true;
}


/* The password is asked (once) from this thread before the first encrypted item is
   decoded in memory: the user interface must not be called from the workers. */
HRESULT CParallelZipExtract::FetchPasswordIfNeeded(const CItemDecodeTask &task)
{
  #ifndef Z7_NO_CRYPTO
  if (task.IsEncrypted && !PasswordSpec->WasFetched())
    return PasswordSpec->Fetch(GetTextPassword);
  #else
  UNUSED_VAR(task)
  #endif
  return S_OK;
}


/* Submits to the pool the items of the window, in order, as far as the memory budget
   and the in-flight limit allow. Called whenever the extraction thread is free: before
   it processes the head, and each time a worker finishes while it waits for the head,
   so that the workers never starve behind a long item. */
HRESULT CParallelZipExtract::AdmitAhead()
{
  while (NumInFlight < MaxInFlight && AdmitPos < Window.size())
  {
    CItemDecodeTask &task = *Window[AdmitPos];
    if (!task.IsParallel)
    {
      if (task.WaitInline)
        break; // no memory for its buffers: no admission past it until it's processed
      AdmitPos++;
      continue;
    }
    if (!Budget.CanReserve(task.Cost))
      break;
    if (!task.AllocBuffers())
    {
      task.FreeBuffers();
      task.IsParallel = false;
      task.WaitInline = true;
      break;
    }
    RINOK(FetchPasswordIfNeeded(task))
    Budget.Reserve(task.Cost);
    task.CostReserved = true;
    NumRunning++;
    if (Pool.Submit(&task) != S_OK)
    {
      NumRunning--;
      return E_FAIL;
    }
    task.IsAdmitted = true;
    NumInFlight++;
    AdmitPos++;
  }
  return S_OK;
}


/* Waits until no worker reads the archive stream (all the submitted tasks finished),
   reporting the progress of (head) meanwhile: before the archive is read in this thread
   (planning, inline decoding). */
HRESULT CParallelZipExtract::WaitWorkersIdle(CLocalProgress *lps, CItemDecodeTask *head)
{
  HeadTask = head;
  while (NumRunning != 0)
  {
    Wakeup.Lock();
    if (head && !head->Finished)
    {
      const UInt64 inSize = head->InProcessed;
      const UInt64 outSize = head->OutProcessed;
      ICompressProgressInfo *progress = lps;
      const HRESULT res = progress->SetRatioInfo(&inSize, &outSize);
      if (res != S_OK)
        return res; // ~CParallelZipExtract() stops the workers
    }
  }
  HeadTask = NULL;
  return S_OK;
}


/* The item is decoded here, with the real streams: the code of the sequential loop. */
HRESULT CParallelZipExtract::DecodeInline(const CItemDecodeTask &task, ISequentialOutStream *realOutStream, Int32 &opRes)
{
  return InlineDecoder.Decode(
      EXTERNAL_CODECS_LOC_VARS_OF(Ctx)
      Ctx.Archive, task.Item, realOutStream, Ctx.Callback, Ctx.Lps,
      Ctx.NumThreads, Ctx.MemUsage,
      opRes);
}


/* The callbacks of one item, in the order of the sequential loop of CHandler::Extract():
   GetStream, (PrepareOperation, SetOperationResult) for the special cases, else
   PrepareOperation, the data (from the buffer of the worker, or decoded here) and
   SetOperationResult. */
HRESULT CParallelZipExtract::ReplayItem(CItemDecodeTask &task, UInt64 &curUnpacked, UInt64 &curPacked)
{
  IArchiveExtractCallback *extractCallback = Ctx.Callback;
  const CItemEx &item = task.Item;
  curUnpacked = item.Size;
  curPacked = item.PackSize;
  const bool skip = !task.IsLocalOffsetOK && !item.IsDir();
  const Int32 askMode = skip ?
      NExtract::NAskMode::kSkip : Ctx.TestMode ?
      NExtract::NAskMode::kTest :
      NExtract::NAskMode::kExtract;
  CMyComPtr<ISequentialOutStream> realOutStream;
  RINOK(extractCallback->GetStream(task.Index, &realOutStream, askMode))

  if (!task.IsLocalOffsetOK)
  {
    RINOK(extractCallback->PrepareOperation(askMode))
    realOutStream.Release();
    return extractCallback->SetOperationResult(NExtract::NOperationResult::kUnavailable);
  }
  if (task.LocalRes != S_OK)
  {
    if (task.LocalRes == S_FALSE)
    {
      if (item.IsDir() || realOutStream || Ctx.TestMode)
      {
        RINOK(extractCallback->PrepareOperation(askMode))
        realOutStream.Release();
        RINOK(extractCallback->SetOperationResult(
            task.IsAvail ?
              NExtract::NOperationResult::kHeadersError :
              NExtract::NOperationResult::kUnavailable))
      }
      return S_OK;
    }
    return task.LocalRes;
  }
  if (item.IsDir())
  {
    RINOK(extractCallback->PrepareOperation(askMode))
    realOutStream.Release();
    return extractCallback->SetOperationResult(NExtract::NOperationResult::kOK);
  }
  if (!Ctx.TestMode && !realOutStream)
    return S_OK;

  RINOK(extractCallback->PrepareOperation(askMode))

  Int32 opRes;
  if (task.Finished && !task.NeedInline)
  {
    // ---------- the data decoded by the worker, in the original order ----------
    RINOK(task.DecodeRes) // what the sequential loop does with the result of Decode()
    opRes = task.OpRes;
    if (realOutStream)
    {
      const Byte *data = task.OutBuf;
      size_t rem = task.OutSize;
      while (rem != 0)
      {
        const UInt32 kStep = (UInt32)1 << 20;
        const UInt32 cur = (rem < kStep) ? (UInt32)rem : kStep;
        RINOK(WriteStream(realOutStream, data, cur))
        data += cur;
        rem -= cur;
      }
    }
  }
  else
  {
    // the real archive stream: no worker may be reading it
    RINOK(WaitWorkersIdle(Ctx.Lps, NULL))
    RINOK(DecodeInline(task, realOutStream, opRes))
  }
  if (opRes == NExtract::NOperationResult::kOK && task.HeadersError)
    opRes = NExtract::NOperationResult::kHeadersError;
  realOutStream.Release();
  return extractCallback->SetOperationResult(opRes);
}


HRESULT CParallelZipExtract::Run(bool &wasUsed)
{
  wasUsed = false;
  const UInt32 numItems = Ctx.NumItems;
  Budget.Init(Ctx.MemUsage);
  /* Items admitted (buffers allocated, in the pool) and not yet replayed: the memory
     budget is the real limit; the count keeps the workers fed past a long item that is
     the head (1285 items of 86 KB on average: with 4 per worker the workers starved
     behind a 50 MB item, 0.39 s; with 64, 0.21 s). */
  const unsigned kMaxInFlightPerWorker = 64;
  /* the items are planned in batches (the local headers are read from the archive
     stream, so only while no worker runs): the batch is the window. A task is a copy
     of the item (a few hundred bytes): the window can be large, and a large window
     means few drains of the pipeline at the batch boundaries. */
  const size_t maxWindow = (size_t)Ctx.NumThreads * 512 + 512;
  if (Wakeup.Create() != 0)
    return S_OK; // sequential path

  // ---------- the first batch: is it worth it? ----------
  UInt32 planItem = 0;     // the next requested item to plan
  unsigned numParallel = 0;
  while (planItem < numItems && Window.size() < maxWindow)
  {
    Window.push_back(std::unique_ptr<CItemDecodeTask>(new CItemDecodeTask));
    Plan(planItem, *Window.back());
    planItem++;
    if (Window.back()->IsParallel)
      numParallel++;
  }
  if (numParallel < 2)
  {
    /* the local headers were read: the sequential loop reads them again (it seeks
       before every read: the stream position doesn't matter) */
    Window.clear();
    return S_OK;
  }

  // ---------- start ----------
  const unsigned numWorkers = (numParallel < Ctx.NumThreads) ? numParallel : Ctx.NumThreads;
  // one decoder per worker
  for (unsigned k = 0; k < numWorkers; k++)
  {
    Decoders.push_back(std::unique_ptr<CZipDecoder>(new CZipDecoder));
    #ifndef Z7_NO_CRYPTO
    /* here, in one thread: the reference count of the COM objects of 7-Zip is not
       atomic, the workers must not AddRef/Release the shared password object */
    Decoders.back()->SetPasswordProvider(Password);
    #endif
  }
  if (Pool.Start(numWorkers) != S_OK)
  {
    Window.clear();
    return S_OK; // no threads: sequential path
  }
  wasUsed = true;
  MaxInFlight = numWorkers * kMaxInFlightPerWorker;
  #ifndef Z7_NO_CRYPTO
  if (Ctx.Callback)
    Ctx.Callback->QueryInterface(IID_ICryptoGetTextPassword, (void **)&GetTextPassword);
  #endif
  CLocalProgress *lps = Ctx.Lps;
  UInt64 curPacked = 0, curUnpacked = 0;
  for (UInt32 i = 0;; i++, lps->OutSize += curUnpacked, lps->InSize += curPacked)
  {
    RINOK(lps->SetCur())
    if (i >= numItems)
      break;
    // ---------- the next batch, when the window is empty and no worker runs ----------
    if (Window.empty())
    {
      RINOK(WaitWorkersIdle(lps, NULL))
      while (planItem < numItems && Window.size() < maxWindow)
      {
        Window.push_back(std::unique_ptr<CItemDecodeTask>(new CItemDecodeTask));
        Plan(planItem, *Window.back());
        planItem++;
      }
      AdmitPos = 0;
    }
    // ---------- admit ahead (in order, within the batch) ----------
    RINOK(AdmitAhead())
    // ---------- process the item at the front of the window ----------
    CItemDecodeTask &task = *Window.front();
    if (task.Pos != i)
      return E_FAIL; // internal error: the window is out of sync
    if (task.IsAdmitted)
    {
      /* wait for the task, reporting the progress of this (head) task; a worker that
         finishes another task wakes this thread too: it admits the next items */
      HeadTask = &task;
      while (!task.Finished)
      {
        Wakeup.Lock();
        if (!task.Finished)
        {
          const UInt64 inSize = task.InProcessed;
          const UInt64 outSize = task.OutProcessed;
          ICompressProgressInfo *progress = lps;
          const HRESULT res = progress->SetRatioInfo(&inSize, &outSize);
          if (res != S_OK)
            return res; // ~CParallelZipExtract() stops the workers
          RINOK(AdmitAhead())
        }
      }
      HeadTask = NULL;
      task.Done.Lock(); // the pool has finished with the task
      NumInFlight--;
    }
    if (task.CostReserved)
    {
      Budget.Release(task.Cost);
      task.CostReserved = false;
    }
    RINOK(ReplayItem(task, curUnpacked, curPacked))
    if (AdmitPos != 0)
      AdmitPos--;
    Window.pop_front();
  }
  return S_OK;
}


HRESULT ExtractItemsParallel(CZipExtractCtx &ctx, CZipDecoder &inlineDecoder, bool &wasUsed)
{
  wasUsed = false;
  if (ctx.NumThreads < 2 || ctx.NumItems < 2 || ctx.Archive.IsMultiVol || !ctx.Archive.GetReadStream())
    return S_OK;
  CParallelZipExtract pe(ctx, inlineDecoder);
  return pe.Run(wasUsed);
}

}}

#endif // Z7_ST
