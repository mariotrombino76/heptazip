// 7zExtract.cpp

#include "StdAfx.h"

#include "../../../../C/7zCrc.h"

#include "../../../Common/ComTry.h"

#include "../../Common/ProgressUtils.h"

#include "7zDecode.h"
#include "7zHandler.h"

#if !defined(Z7_ST)
#include <atomic>
#include <deque>
#include <memory>
#include <vector>

#include "../../../../C/Alloc.h"
#include "../../Common/StreamUtils.h"
#include "7zExtractMt.h"
#endif

// EXTERN_g_ExternalCodecs

namespace NArchive {
namespace N7z {

Z7_CLASS_IMP_COM_1(
  CFolderOutStream
  , ISequentialOutStream
  /* , ICompressGetSubStreamSize */
)
  CMyComPtr<ISequentialOutStream> _stream;
public:
  bool TestMode;
  bool CheckCrc;
private:
  bool _fileIsOpen;
  bool _calcCrc;
  UInt32 _crc;
  UInt64 _rem;

  const UInt32 *_indexes;
  // unsigned _startIndex;
  unsigned _numFiles;
  unsigned _fileIndex;

  HRESULT OpenFile(bool isCorrupted = false);
  HRESULT CloseFile_and_SetResult(Int32 res);
  HRESULT CloseFile();
  HRESULT ProcessEmptyFiles();

public:
  const CDbEx *_db;
  CMyComPtr<IArchiveExtractCallback> ExtractCallback;

  bool ExtraWriteWasCut;

  CFolderOutStream():
      TestMode(false),
      CheckCrc(true)
      {}

  HRESULT Init(unsigned startIndex, const UInt32 *indexes, unsigned numFiles);
  HRESULT FlushCorrupted(Int32 callbackOperationResult);

  bool WasWritingFinished() const { return _numFiles == 0; }
};


HRESULT CFolderOutStream::Init(unsigned startIndex, const UInt32 *indexes, unsigned numFiles)
{
  // _startIndex = startIndex;
  _fileIndex = startIndex;
  _indexes = indexes;
  _numFiles = numFiles;
  
  _fileIsOpen = false;
  ExtraWriteWasCut = false;
  
  return ProcessEmptyFiles();
}

HRESULT CFolderOutStream::OpenFile(bool isCorrupted)
{
  const CFileItem &fi = _db->Files[_fileIndex];
  const UInt32 nextFileIndex = (_indexes ? *_indexes : _fileIndex);
  Int32 askMode = (_fileIndex == nextFileIndex) ? TestMode ?
      NExtract::NAskMode::kTest :
      NExtract::NAskMode::kExtract :
      NExtract::NAskMode::kSkip;

  if (isCorrupted
      && askMode == NExtract::NAskMode::kExtract
      && !_db->IsItemAnti(_fileIndex)
      && !fi.IsDir)
    askMode = NExtract::NAskMode::kTest;
  
  CMyComPtr<ISequentialOutStream> realOutStream;
  RINOK(ExtractCallback->GetStream(_fileIndex, &realOutStream, askMode))
  
  _stream = realOutStream;
  _crc = CRC_INIT_VAL;
  _calcCrc = (CheckCrc && fi.CrcDefined && !fi.IsDir);

  _fileIsOpen = true;
  _rem = fi.Size;
  
  if (askMode == NExtract::NAskMode::kExtract
      && !realOutStream
      && !_db->IsItemAnti(_fileIndex)
      && !fi.IsDir)
    askMode = NExtract::NAskMode::kSkip;
  return ExtractCallback->PrepareOperation(askMode);
}

HRESULT CFolderOutStream::CloseFile_and_SetResult(Int32 res)
{
  _stream.Release();
  _fileIsOpen = false;
  
  if (!_indexes)
    _numFiles--;
  else if (*_indexes == _fileIndex)
  {
    _indexes++;
    _numFiles--;
  }

  _fileIndex++;
  return ExtractCallback->SetOperationResult(res);
}

HRESULT CFolderOutStream::CloseFile()
{
  const CFileItem &fi = _db->Files[_fileIndex];
  return CloseFile_and_SetResult((!_calcCrc || fi.Crc == CRC_GET_DIGEST(_crc)) ?
      NExtract::NOperationResult::kOK :
      NExtract::NOperationResult::kCRCError);
}

HRESULT CFolderOutStream::ProcessEmptyFiles()
{
  while (_numFiles != 0 && _db->Files[_fileIndex].Size == 0)
  {
    RINOK(OpenFile())
    RINOK(CloseFile())
  }
  return S_OK;
}

Z7_COM7F_IMF(CFolderOutStream::Write(const void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  
  while (size != 0)
  {
    if (_fileIsOpen)
    {
      UInt32 cur = (size < _rem ? size : (UInt32)_rem);
      if (_calcCrc)
      {
        const UInt32 k_Step = (UInt32)1 << 20;
        if (cur > k_Step)
          cur = k_Step;
      }
      HRESULT result = S_OK;
      if (_stream)
        result = _stream->Write(data, cur, &cur);
      if (_calcCrc)
        _crc = CrcUpdate(_crc, data, cur);
      if (processedSize)
        *processedSize += cur;
      data = (const Byte *)data + cur;
      size -= cur;
      _rem -= cur;
      if (_rem == 0)
      {
        RINOK(CloseFile())
        RINOK(ProcessEmptyFiles())
      }
      RINOK(result)
      if (cur == 0)
        break;
      continue;
    }
  
    RINOK(ProcessEmptyFiles())
    if (_numFiles == 0)
    {
      // we support partial extracting
      /*
      if (processedSize)
        *processedSize += size;
      break;
      */
      ExtraWriteWasCut = true;
      // return S_FALSE;
      return k_My_HRESULT_WritingWasCut;
    }
    RINOK(OpenFile())
  }
  
  return S_OK;
}

HRESULT CFolderOutStream::FlushCorrupted(Int32 callbackOperationResult)
{
  while (_numFiles != 0)
  {
    if (_fileIsOpen)
    {
      RINOK(CloseFile_and_SetResult(callbackOperationResult))
    }
    else
    {
      RINOK(OpenFile(true))
    }
  }
  return S_OK;
}

/*
Z7_COM7F_IMF(CFolderOutStream::GetSubStreamSize(UInt64 subStream, UInt64 *value))
{
  *value = 0;
  // const unsigned numFiles_Original = _numFiles + _fileIndex - _startIndex;
  const unsigned numFiles_Original = _numFiles;
  if (subStream >= numFiles_Original)
    return S_FALSE; // E_FAIL;
  *value = _db->Files[_startIndex + (unsigned)subStream].Size;
  return S_OK;
}
*/


/* Heptazip: the planning of one extraction step and the handling of the
   folder decoding result were moved out of CHandler::Extract() into the two
   functions below, without behavior changes, so that the parallel folder
   extraction (docs/design/0001-estrazione-parallela-folder-7z.md) can reuse
   exactly the same logic. */

/* One step of extraction: a run of consecutive requested items that belong to
   the same folder (or one item without folder: empty file, dir, anti-item). */
struct CExtractJob
{
  UInt32 NumItems;        // number of requested items processed by this step
  UInt32 StartFileIndex;  // first file passed to CFolderOutStream::Init()
  CNum FolderIndex;       // kNumNoIndex, if the item has no data stream
  UInt64 UnpackSize;      // bytes to unpack: up to the end of the last requested file
  UInt64 PackSize;
};

static void GetExtractJob(const CDbEx &db,
    const UInt32 *indices, UInt32 numItems, bool allFilesMode,
    UInt32 i, CExtractJob &job)
{
  job.UnpackSize = 0;
  job.PackSize = 0;

  UInt32 fileIndex = allFilesMode ? i : indices[i];
  const CNum folderIndex = db.FileIndexToFolderIndexMap[fileIndex];

  UInt32 numSolidFiles = 1;

  if (folderIndex != kNumNoIndex)
  {
    job.PackSize = db.GetFolderFullPackSize(folderIndex);
    UInt32 nextFile = fileIndex + 1;
    fileIndex = db.FolderStartFileIndex[folderIndex];
    UInt32 k;

    for (k = i + 1; k < numItems; k++)
    {
      const UInt32 fileIndex2 = allFilesMode ? k : indices[k];
      if (db.FileIndexToFolderIndexMap[fileIndex2] != folderIndex
          || fileIndex2 < nextFile)
        break;
      nextFile = fileIndex2 + 1;
    }

    numSolidFiles = k - i;

    for (k = fileIndex; k < nextFile; k++)
      job.UnpackSize += db.Files[k].Size;
  }

  job.NumItems = numSolidFiles;
  job.StartFileIndex = fileIndex;
  job.FolderIndex = folderIndex;
}

/* Processes the result of CDecoder::Decode() for one folder.
   Returns S_OK, if extraction must continue with the next step. */
static HRESULT ProcessFolderDecodeResult(
    CFolderOutStream *folderOutStream,
    IArchiveExtractCallbackMessage2 *callbackMessage,
    CNum folderIndex, HRESULT result, bool dataAfterEnd_Error)
{
  if (result == S_FALSE || result == E_NOTIMPL || dataAfterEnd_Error)
  {
    const bool wasFinished = folderOutStream->WasWritingFinished();

    int resOp = NExtract::NOperationResult::kDataError;

    if (result != S_FALSE)
    {
      if (result == E_NOTIMPL)
        resOp = NExtract::NOperationResult::kUnsupportedMethod;
      else if (wasFinished && dataAfterEnd_Error)
        resOp = NExtract::NOperationResult::kDataAfterEnd;
    }

    RINOK(folderOutStream->FlushCorrupted(resOp))

    if (wasFinished)
    {
      // we don't show error, if it's after required files
      if (/* !folderOutStream->ExtraWriteWasCut && */ callbackMessage)
      {
        RINOK(callbackMessage->ReportExtractResult(NEventIndexType::kBlockIndex, folderIndex, resOp))
      }
    }
    return S_OK;
  }

  if (result != S_OK)
    return result;

  return folderOutStream->FlushCorrupted(NExtract::NOperationResult::kDataError);
}


/* State shared by the extraction steps of one CHandler::Extract() call. */
struct CExtractContext
{
  #ifdef Z7_EXTERNAL_CODECS
  const CExternalCodecs *ExternalCodecs;
  #endif
  IInStream *InStream;
  const CDbEx *Db;
  const UInt32 *Indices;
  bool AllFilesMode;
  IArchiveExtractCallback *ExtractCallback;
  IArchiveExtractCallbackMessage2 *CallbackMessage;
  CFolderOutStream *FolderOutStream;
  ISequentialOutStream *OutStream; // FolderOutStream
  ICompressProgressInfo *Progress;
  #if !defined(Z7_ST)
  UInt32 NumThreads;
  UInt64 MemUsage;
  #endif
};

/* Prepares CFolderOutStream for the step that starts at requested item (i).
   (needDecode) is false, if all requested items of the step were processed
   already (empty files, dirs). */
static HRESULT InitJobOutput(const CExtractContext &ctx, const CExtractJob &job, UInt32 i, bool &needDecode)
{
  needDecode = false;
  RINOK(ctx.FolderOutStream->Init(job.StartFileIndex,
      ctx.AllFilesMode ? NULL : ctx.Indices + i,
      job.NumItems))

  if (ctx.FolderOutStream->WasWritingFinished())
  {
    // for debug: to test zero size stream unpacking
    // if (folderIndex == kNumNoIndex)  // enable this check for debug
    return S_OK;
  }

  if (job.FolderIndex == kNumNoIndex)
    return E_FAIL;
  needDecode = true;
  return S_OK;
}

/* Decodes the folder of the step directly to CFolderOutStream. */
static HRESULT DecodeJobInline(const CExtractContext &ctx, CDecoder &decoder, const CExtractJob &job)
{
  #ifdef Z7_EXTERNAL_CODECS
  const CExternalCodecs *_externalCodecs = ctx.ExternalCodecs; // for EXTERNAL_CODECS_LOC_VARS
  #endif

  #ifndef Z7_NO_CRYPTO
  CMyComPtr<ICryptoGetTextPassword> getTextPassword;
  if (ctx.ExtractCallback)
    ctx.ExtractCallback->QueryInterface(IID_ICryptoGetTextPassword, (void **)&getTextPassword);
  #endif

  try
  {
    #ifndef Z7_NO_CRYPTO
      bool isEncrypted = false;
      bool passwordIsDefined = false;
      UString_Wipe password;
    #endif

    bool dataAfterEnd_Error = false;

    const HRESULT result = decoder.Decode(
        EXTERNAL_CODECS_LOC_VARS
        ctx.InStream,
        ctx.Db->ArcInfo.DataStartPosition,
        *ctx.Db, job.FolderIndex,
        &job.UnpackSize,

        ctx.OutStream,
        ctx.Progress,
        NULL // *inStreamMainRes
        , dataAfterEnd_Error

        Z7_7Z_DECODER_CRYPRO_VARS
        #if !defined(Z7_ST)
          , true, ctx.NumThreads, ctx.MemUsage
        #endif
        );

    return ProcessFolderDecodeResult(ctx.FolderOutStream, ctx.CallbackMessage,
        job.FolderIndex, result, dataAfterEnd_Error);
  }
  catch(...)
  {
    RINOK(ctx.FolderOutStream->FlushCorrupted(NExtract::NOperationResult::kDataError))
    // continue;
    // return E_FAIL;
    throw;
  }
}


#if !defined(Z7_ST)

/* ---------------------------------------------------------------------------
   Heptazip: parallel extraction of folders.
   See docs/design/0001-estrazione-parallela-folder-7z.md

   Worker threads decode folders ahead into memory buffers. The extraction
   thread processes the steps in the original order: for each step it calls
   InitJobOutput() and then "replays" the decoded data to CFolderOutStream,
   and processes the decoding result with ProcessFolderDecodeResult().
   So the sequence of IArchiveExtractCallback calls is the same as in
   sequential extraction. Large folders, folders with unknown methods, and
   steps without folder are processed inline in the extraction thread.
--------------------------------------------------------------------------- */

class CParallelExtract;

/* A folder decoded ahead by a worker thread. */
class CFolderDecodeTask Z7_final: public CMtTask
{
  Z7_CLASS_NO_COPY(CFolderDecodeTask)
public:
  CParallelExtract *Owner;
  CExtractJob Job;
  UInt32 FirstItem;       // index of the first requested item of the step
  bool IsParallel;        // the step can be decoded ahead by a worker
  bool IsAdmitted;        // submitted to the pool
  bool IsEncrypted;
  UInt64 Cost;            // reserved in CMemoryBudget
  UInt64 PackStartPos;    // position of the first packed stream in archive stream

  Byte *PackBuf;
  Byte *OutBuf;

  // results: written by the worker before (Finished), read after it
  HRESULT DecodeRes;
  bool DataAfterEnd_Error;
  bool DecodeException;
  size_t OutSize;

  // progress: written by the worker, read by the extraction thread
  std::atomic<UInt64> InProcessed;
  std::atomic<UInt64> OutProcessed;
  std::atomic<bool> Finished;

  CFolderDecodeTask():
      Owner(NULL), FirstItem(0), IsParallel(false), IsAdmitted(false),
      IsEncrypted(false), Cost(0), PackStartPos(0),
      PackBuf(NULL), OutBuf(NULL),
      DecodeRes(S_OK), DataAfterEnd_Error(false), DecodeException(false), OutSize(0),
      InProcessed(0), OutProcessed(0), Finished(false)
      {}
  ~CFolderDecodeTask() { FreeBuffers(); }

  void FreeBuffers()
  {
    MyFree(PackBuf);  PackBuf = NULL;
    MyFree(OutBuf);   OutBuf = NULL;
  }

  bool AllocBuffers()
  {
    PackBuf = (Byte *)MyAlloc((size_t)Job.PackSize);
    OutBuf = (Byte *)MyAlloc((size_t)Job.UnpackSize);
    return (PackBuf || Job.PackSize == 0) && (OutBuf || Job.UnpackSize == 0);
  }

  void Run(unsigned workerIndex) Z7_override;
};


/* Progress of a task: stores the processed sizes and handles cancellation. */
Z7_CLASS_IMP_COM_1(
  CFolderDecodeProgress
  , ICompressProgressInfo
)
public:
  CFolderDecodeTask *Task;
};


class CParallelExtract
{
  Z7_CLASS_NO_COPY(CParallelExtract)
public:
  const CExtractContext &Ctx;
  bool UseMixerMT;

  /* Sliding window of planned steps, in order: front() is the step that is
     processed now. A step is planned when it enters the window, and its task
     (with buffers) is freed when it leaves the window. So the memory doesn't
     depend on the number of items in archive.
     It's a member (not a local of Run()): the destructor stops the workers
     before the tasks are freed. */
  std::deque< std::unique_ptr<CFolderDecodeTask> > Window;

  CMtTaskPool Pool;
  std::vector< std::unique_ptr<CDecoder> > Decoders; // one per worker thread

  NWindows::NSynchronization::CCriticalSection InStreamLock;
  NWindows::NSynchronization::CAutoResetEvent Wakeup; // a task has finished or progressed
  std::atomic<bool> Abort;
  std::atomic<CFolderDecodeTask *> HeadTask;

  #ifndef Z7_NO_CRYPTO
  CCachedPassword *PasswordSpec;
  CMyComPtr<ICryptoGetTextPassword> Password;
  #endif

  CParallelExtract(const CExtractContext &ctx, bool useMixerMT):
      Ctx(ctx), UseMixerMT(useMixerMT), Abort(false), HeadTask(NULL)
  {
    #ifndef Z7_NO_CRYPTO
    PasswordSpec = new CCachedPassword;
    Password = PasswordSpec;
    #endif
  }

  /* Stops the workers on any exit path, before the tasks are freed. */
  ~CParallelExtract()
  {
    Abort = true;
    Pool.Stop();
  }

  void PlanStep(UInt32 numItems, UInt32 i, const CMemoryBudget &budget, CFolderDecodeTask &task);
  HRESULT ReadPackedData(CFolderDecodeTask &task, size_t &size, HRESULT &readRes);
  HRESULT Run(UInt32 numItems, CDecoder &inlineDecoder, CLocalProgress *lps, bool &wasUsed);
};


Z7_COM7F_IMF(CFolderDecodeProgress::SetRatioInfo(const UInt64 *inSize, const UInt64 *outSize))
{
  if (inSize)
    Task->InProcessed = *inSize;
  if (outSize)
    Task->OutProcessed = *outSize;
  CParallelExtract *owner = Task->Owner;
  if (owner->Abort)
    return E_ABORT;
  if (owner->HeadTask == Task)
    owner->Wakeup.Set(); // the extraction thread reports progress of the head task
  return S_OK;
}


/* Reads the packed streams of the folder (they are contiguous in the archive)
   with one sequential read under lock: also good for HDD.
   (readRes) is the result of reading: if it's an error, the decoder will get
   it at the end of the data, as it would get it from the archive stream. */
HRESULT CParallelExtract::ReadPackedData(CFolderDecodeTask &task, size_t &size, HRESULT &readRes)
{
  NWindows::NSynchronization::CCriticalSectionLock lock(InStreamLock);
  size = (size_t)task.Job.PackSize;
  readRes = InStream_SeekSet(Ctx.InStream, task.PackStartPos);
  if (readRes != S_OK)
  {
    size = 0;
    return S_OK;
  }
  readRes = ReadStream(Ctx.InStream, task.PackBuf, &size);
  return S_OK;
}


void CFolderDecodeTask::Run(unsigned workerIndex)
{
  CParallelExtract &owner = *Owner;
  const CExtractContext &ctx = owner.Ctx;

  try
  {
    size_t packSize = 0;
    HRESULT readRes = S_OK;
    owner.ReadPackedData(*this, packSize, readRes);

    CBufInStreamAt *inStreamSpec = new CBufInStreamAt;
    CMyComPtr<IInStream> inStream = inStreamSpec;
    inStreamSpec->Init(PackBuf, packSize, PackStartPos, readRes);

    CFolderBufOutStream *outStreamSpec = new CFolderBufOutStream;
    CMyComPtr<ISequentialOutStream> outStream = outStreamSpec;
    outStreamSpec->Init(OutBuf, (size_t)Job.UnpackSize);

    CFolderDecodeProgress *progressSpec = new CFolderDecodeProgress;
    CMyComPtr<ICompressProgressInfo> progress = progressSpec;
    progressSpec->Task = this;

    #ifdef Z7_EXTERNAL_CODECS
    const CExternalCodecs *_externalCodecs = ctx.ExternalCodecs; // for EXTERNAL_CODECS_LOC_VARS
    #endif

    #ifndef Z7_NO_CRYPTO
    ICryptoGetTextPassword *getTextPassword = owner.Password;
    bool isEncrypted = false;
    bool passwordIsDefined = false;
    UString_Wipe password;
    #endif

    DataAfterEnd_Error = false;
    try
    {
      DecodeRes = owner.Decoders[workerIndex]->Decode(
          EXTERNAL_CODECS_LOC_VARS
          inStream,
          ctx.Db->ArcInfo.DataStartPosition,
          *ctx.Db, Job.FolderIndex,
          &Job.UnpackSize,
          outStream,
          progress,
          NULL // *inStreamMainRes
          , DataAfterEnd_Error
          Z7_7Z_DECODER_CRYPRO_VARS
          , true, 1, ctx.MemUsage // the parallelism is between folders
          );
    }
    catch(...)
    {
      // CHandler::Extract() converts any exception to E_OUTOFMEMORY (COM_TRY_END)
      DecodeException = true;
    }
    OutSize = outStreamSpec->GetPos();
  }
  catch(...)
  {
    DecodeException = true;
  }

  Finished = true;
  owner.Wakeup.Set();
}


/* Plans the step that starts at requested item (i): the job and whether it
   can be decoded ahead by a worker (known methods, not a "large" job). */
void CParallelExtract::PlanStep(UInt32 numItems, UInt32 i, const CMemoryBudget &budget, CFolderDecodeTask &task)
{
  const CDbEx &db = *Ctx.Db;
  task.Owner = this;
  task.FirstItem = i;
  GetExtractJob(db, Ctx.Indices, numItems, Ctx.AllFilesMode, i, task.Job);
  task.IsParallel = false;
  task.IsEncrypted = false;
  task.Cost = 0;
  task.PackStartPos = 0;

  const CNum folderIndex = task.Job.FolderIndex;
  if (folderIndex == kNumNoIndex
      || task.Job.PackSize != (size_t)task.Job.PackSize
      || task.Job.UnpackSize != (size_t)task.Job.UnpackSize)
    return;

  CFolderEx folder;
  db.ParseFolderEx(folderIndex, folder);
  UInt64 decoderMem;
  if (!folder.IsDecodingSupported()
      || !EstimateFolderDecodeMemory(folder, decoderMem))
    return;

  for (unsigned k = 0; k < folder.Coders.Size(); k++)
    if (folder.Coders[k].MethodID == k_AES)
      task.IsEncrypted = true;

  const UInt64 kTaskOverhead = (UInt64)1 << 20;
  task.Cost = task.Job.PackSize + task.Job.UnpackSize + decoderMem + kTaskOverhead;
  task.PackStartPos = db.GetFolderStreamPos(folderIndex, 0);
  // overflow check of the sum, then the "large job" and "big folder" rules
  task.IsParallel = (task.Cost >= decoderMem
      && !budget.IsLargeJob(task.Cost)
      && !CMemoryBudget::IsBigFolder(task.Job.UnpackSize));
}


/* (wasUsed) is false, if the parallel path is not suitable: then nothing was
   done, and the caller must use the sequential loop. */
HRESULT CParallelExtract::Run(UInt32 numItems, CDecoder &inlineDecoder, CLocalProgress *lps, bool &wasUsed)
{
  wasUsed = false;

  CMemoryBudget budget;
  budget.Init(Ctx.MemUsage);

  // ---------- is it worth it? count parallel steps, up to the number of threads ----------
  unsigned numParallel = 0;
  {
    CFolderDecodeTask temp;
    for (UInt32 i = 0; i < numItems && numParallel < Ctx.NumThreads;)
    {
      PlanStep(numItems, i, budget, temp);
      i += temp.Job.NumItems;
      if (temp.IsParallel)
        numParallel++;
    }
  }
  if (numParallel < 2)
    return S_OK;

  // ---------- start ----------
  if (Wakeup.Create() != 0)
    return S_OK; // sequential path

  const unsigned numWorkers = numParallel; // <= Ctx.NumThreads
  for (unsigned k = 0; k < numWorkers; k++)
    Decoders.push_back(std::unique_ptr<CDecoder>(new CDecoder(UseMixerMT)));
  if (Pool.Start(numWorkers) != S_OK)
    return S_OK; // no threads: sequential path
  wasUsed = true;

  // Bounds of the window: buffered folders (memory, latency) and planned steps.
  const unsigned kMaxInFlightPerWorker = 4;
  const unsigned maxInFlight = numWorkers * kMaxInFlightPerWorker;
  const size_t maxWindow = (size_t)maxInFlight * 4 + 16;
  unsigned numInFlight = 0;
  size_t admitPos = 0;     // Window[admitPos] is the first step not considered for admission
  UInt32 planItem = 0;     // first requested item of the next step to plan

  #ifndef Z7_NO_CRYPTO
  CMyComPtr<ICryptoGetTextPassword> getTextPassword;
  if (Ctx.ExtractCallback)
    Ctx.ExtractCallback->QueryInterface(IID_ICryptoGetTextPassword, (void **)&getTextPassword);
  #endif

  UInt64 curPacked = 0, curUnpacked = 0;

  for (UInt32 i = 0;; lps->OutSize += curUnpacked, lps->InSize += curPacked)
  {
    RINOK(lps->SetCur())

    if (i >= numItems)
      break;

    // ---------- plan and admit ahead (in order; never past a step processed inline) ----------
    while (numInFlight < maxInFlight)
    {
      if (admitPos == Window.size())
      {
        if (planItem >= numItems || Window.size() >= maxWindow)
          break;
        std::unique_ptr<CFolderDecodeTask> planned(new CFolderDecodeTask);
        PlanStep(numItems, planItem, budget, *planned);
        planItem += planned->Job.NumItems;
        Window.push_back(std::move(planned));
      }
      CFolderDecodeTask &task = *Window[admitPos];
      if (!task.IsParallel)
      {
        if (task.Job.FolderIndex != kNumNoIndex)
          break; // inline folder: no admission past it until it's processed
        admitPos++; // step without folder: nothing to decode
        continue;
      }
      if (!budget.CanReserve(task.Cost))
        break;
      if (!task.AllocBuffers())
      {
        // not enough memory now: the step will be processed inline
        task.FreeBuffers();
        task.IsParallel = false;
        break;
      }
      #ifndef Z7_NO_CRYPTO
      if (task.IsEncrypted && !PasswordSpec->WasFetched())
        PasswordSpec->Fetch(getTextPassword); // the UI is asked from this thread only
      #endif
      budget.Reserve(task.Cost);
      RINOK(Pool.Submit(&task))
      task.IsAdmitted = true;
      numInFlight++;
      admitPos++;
    }

    // ---------- process the step at the front of the window ----------
    if (Window.empty())
    {
      // the window is limited and all its steps were processed: plan the next step
      std::unique_ptr<CFolderDecodeTask> planned(new CFolderDecodeTask);
      PlanStep(numItems, planItem, budget, *planned);
      planItem += planned->Job.NumItems;
      Window.push_back(std::move(planned));
    }
    CFolderDecodeTask &task = *Window.front();
    if (task.FirstItem != i)
      return E_FAIL; // internal error: the window is out of sync
    const CExtractJob &job = task.Job;
    curUnpacked = job.UnpackSize;
    curPacked = job.PackSize;

    bool needDecode;
    {
      const HRESULT initRes = InitJobOutput(Ctx, job, i, needDecode);
      i += job.NumItems;
      RINOK(initRes) // ~CParallelExtract() stops the workers
    }

    if (task.IsAdmitted)
    {
      // wait for the task, reporting the progress of this (head) task
      HeadTask = &task;
      while (!task.Finished)
      {
        Wakeup.Lock();
        if (!task.Finished && needDecode)
        {
          const UInt64 inSize = task.InProcessed;
          const UInt64 outSize = task.OutProcessed;
          const HRESULT res = Ctx.Progress->SetRatioInfo(&inSize, &outSize); // == lps
          if (res != S_OK)
            return res; // ~CParallelExtract() stops the workers
        }
      }
      HeadTask = NULL;
      task.Done.Lock(); // the pool has finished with the task
      numInFlight--;
      budget.Release(task.Cost);
    }

    HRESULT stepRes = S_OK;

    if (!needDecode)
    {
    }
    else if (!task.IsAdmitted)
      stepRes = DecodeJobInline(Ctx, inlineDecoder, job);
    else
    {
      // ---------- replay the decoded data in the original order ----------
      HRESULT result = task.DecodeRes;
      {
        const Byte *data = task.OutBuf;
        size_t rem = task.OutSize;
        while (rem != 0)
        {
          const UInt32 kStep = (UInt32)1 << 20;
          const UInt32 cur = (rem < kStep) ? (UInt32)rem : kStep;
          UInt32 processed = 0;
          const HRESULT res = Ctx.OutStream->Write(data, cur, &processed);
          if (res != S_OK)
          {
            // in sequential extraction the decoder returns the error of the output stream
            result = res;
            break;
          }
          if (processed == 0)
            break;
          data += processed;
          rem -= processed;
        }
      }

      if (task.DecodeException && result == task.DecodeRes)
      {
        // same as the catch(...) block of DecodeJobInline()
        RINOK(Ctx.FolderOutStream->FlushCorrupted(NExtract::NOperationResult::kDataError))
        return E_OUTOFMEMORY;
      }
      stepRes = ProcessFolderDecodeResult(Ctx.FolderOutStream, Ctx.CallbackMessage,
          job.FolderIndex, result, task.DataAfterEnd_Error && result == task.DecodeRes);
    }

    // the step leaves the window: its task and buffers are freed
    Window.pop_front();
    if (admitPos != 0)
      admitPos--;

    RINOK(stepRes)
  }

  return S_OK;
}

#endif // !defined(Z7_ST)


Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testModeSpec, IArchiveExtractCallback *extractCallbackSpec))
{
  // for GCC
  // CFolderOutStream *folderOutStream = new CFolderOutStream;
  // CMyComPtr<ISequentialOutStream> outStream(folderOutStream);

  COM_TRY_BEGIN
  
  CMyComPtr<IArchiveExtractCallback> extractCallback = extractCallbackSpec;
  
  UInt64 importantTotalUnpacked = 0;

  // numItems = (UInt32)(Int32)-1;

  const bool allFilesMode = (numItems == (UInt32)(Int32)-1);
  if (allFilesMode)
    numItems = _db.Files.Size();

  if (numItems == 0)
    return S_OK;

  {
    CNum prevFolder = kNumNoIndex;
    UInt32 nextFile = 0;
    
    UInt32 i;
    
    for (i = 0; i < numItems; i++)
    {
      const UInt32 fileIndex = allFilesMode ? i : indices[i];
      const CNum folderIndex = _db.FileIndexToFolderIndexMap[fileIndex];
      if (folderIndex == kNumNoIndex)
        continue;
      if (folderIndex != prevFolder || fileIndex < nextFile)
        nextFile = _db.FolderStartFileIndex[folderIndex];
      for (CNum index = nextFile; index <= fileIndex; index++)
        importantTotalUnpacked += _db.Files[index].Size;
      nextFile = fileIndex + 1;
      prevFolder = folderIndex;
    }
  }

  RINOK(extractCallback->SetTotal(importantTotalUnpacked))

  CMyComPtr2_Create<ICompressProgressInfo, CLocalProgress> lps;
  lps->Init(extractCallback, false);

  const bool useMixerMT =
    #if !defined(USE_MIXER_MT)
      false
    #elif !defined(USE_MIXER_ST)
      true
    #elif !defined(Z7_7Z_SET_PROPERTIES)
      #ifdef Z7_ST
        false
      #else
        true
      #endif
    #else
      _useMultiThreadMixer
    #endif
    ;
  CDecoder decoder(useMixerMT);

  UInt64 curPacked, curUnpacked;

  CMyComPtr<IArchiveExtractCallbackMessage2> callbackMessage;
  extractCallback.QueryInterface(IID_IArchiveExtractCallbackMessage2, &callbackMessage);

  CFolderOutStream *folderOutStream = new CFolderOutStream;
  CMyComPtr<ISequentialOutStream> outStream(folderOutStream);

  folderOutStream->_db = &_db;
  folderOutStream->ExtractCallback = extractCallback;
  folderOutStream->TestMode = (testModeSpec != 0);
  folderOutStream->CheckCrc = (_crcSize != 0);

  CExtractContext ctx;
  #ifdef Z7_EXTERNAL_CODECS
  ctx.ExternalCodecs = EXTERNAL_CODECS_VARS2;
  #endif
  ctx.InStream = _inStream;
  ctx.Db = &_db;
  ctx.Indices = indices;
  ctx.AllFilesMode = allFilesMode;
  ctx.ExtractCallback = extractCallback;
  ctx.CallbackMessage = callbackMessage;
  ctx.FolderOutStream = folderOutStream;
  ctx.OutStream = outStream;
  ctx.Progress = lps;
  #if !defined(Z7_ST)
  ctx.NumThreads = _numThreads;
  ctx.MemUsage = _memUsage_Decompress;

  // Heptazip: parallel extraction of folders (-mpfd)
  if (_parallelFolderDecoding && _numThreads > 1)
  {
    CParallelExtract parallelExtract(ctx, useMixerMT);
    bool wasUsed = false;
    const HRESULT res = parallelExtract.Run(numItems, decoder, lps.ClsPtr(), wasUsed);
    if (wasUsed)
      return res;
  }
  #endif

  for (UInt32 i = 0;; lps->OutSize += curUnpacked, lps->InSize += curPacked)
  {
    RINOK(lps->SetCur())

    if (i >= numItems)
      break;

    CExtractJob job;
    GetExtractJob(_db, indices, numItems, allFilesMode, i, job);
    curUnpacked = job.UnpackSize;
    curPacked = job.PackSize;

    bool needDecode;
    {
      const HRESULT result = InitJobOutput(ctx, job, i, needDecode);
      i += job.NumItems;
      RINOK(result)
    }
    if (!needDecode)
      continue;

    RINOK(DecodeJobInline(ctx, decoder, job))
  }

  return S_OK;

  COM_TRY_END
}

}}
