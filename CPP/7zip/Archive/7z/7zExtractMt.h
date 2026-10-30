// 7zExtractMt.h
// 7zip-modern: building blocks for parallel extraction of 7z folders.
// See docs/design/0001-estrazione-parallela-folder-7z.md

#ifndef ZIP7_INC_7Z_EXTRACT_MT_H
#define ZIP7_INC_7Z_EXTRACT_MT_H

#include "../../../Common/MyCom.h"
#include "../../../Common/MyString.h"
#include "../../../Common/MyVector.h"

#ifndef Z7_ST
#include "../../../Windows/Synchronization.h"
#include "../../../Windows/Thread.h"
#endif

#include "../../IStream.h"

#ifndef Z7_NO_CRYPTO
#include "../../IPassword.h"
#endif

#include "7zItem.h"

namespace NArchive {
namespace N7z {

// ---------------------------------------------------------------------------
// Memory estimation
// ---------------------------------------------------------------------------

/* Estimates the memory used by the decoder of one coder (dictionary, model,
   internal buffers), when the decoder runs single-threaded.
   Returns false for unknown methods or malformed properties: the caller must
   not decode such folder in parallel (it's processed by the sequential path). */
bool EstimateCoderDecodeMemory(CMethodId methodId, const Byte *props, size_t propsSize, UInt64 &memUsage);

/* Sum of EstimateCoderDecodeMemory() for all coders of the folder. */
bool EstimateFolderDecodeMemory(const CFolder &folder, UInt64 &memUsage);


// ---------------------------------------------------------------------------
// Memory budget for admission of parallel jobs.
// Used only by the extraction thread: no synchronization.
// ---------------------------------------------------------------------------

class CMemoryBudget
{
  UInt64 _limit;
  UInt64 _used;
public:
  /* A job is "large" if its cost is more than (limit / kLargeJobDivisor).
     Large jobs are not decoded ahead into memory. */
  static const unsigned kLargeJobDivisor = 4;

  CMemoryBudget(): _limit(0), _used(0) {}
  void Init(UInt64 limit) { _limit = limit; _used = 0; }

  UInt64 GetLimit() const { return _limit; }
  UInt64 GetUsed() const { return _used; }

  bool IsLargeJob(UInt64 cost) const { return cost > _limit / kLargeJobDivisor; }
  bool CanReserve(UInt64 cost) const { return cost <= _limit - _used; }
  void Reserve(UInt64 cost) { _used += cost; }
  void Release(UInt64 cost) { _used -= cost; }
};


// ---------------------------------------------------------------------------
// Output stream that stores decoded folder data in a memory buffer.
//
// When the buffer is full, Write() stores the bytes that fit and returns
// k_My_HRESULT_WritingWasCut: the same result and (*processedSize) that
// CFolderOutStream::Write() returns when data exceeds the requested files.
// So the decoder and the mixer behave exactly as in sequential extraction.
// ---------------------------------------------------------------------------

Z7_CLASS_IMP_COM_1(
  CFolderBufOutStream
  , ISequentialOutStream
)
  Byte *_buf;
  size_t _size;
  size_t _pos;
public:
  bool WritingWasCut;

  CFolderBufOutStream(): _buf(NULL), _size(0), _pos(0), WritingWasCut(false) {}
  void Init(Byte *buf, size_t size)
  {
    _buf = buf;
    _size = size;
    _pos = 0;
    WritingWasCut = false;
  }
  size_t GetPos() const { return _pos; }
};


#ifndef Z7_NO_CRYPTO

// ---------------------------------------------------------------------------
// Password provider for worker threads.
//
// The extraction thread calls Fetch() once, before the first encrypted folder
// is decoded: so the user interface is asked from the extraction thread only.
// After that the object is read-only and can be used from any thread.
// ---------------------------------------------------------------------------

Z7_CLASS_IMP_COM_1(
  CCachedPassword
  , ICryptoGetTextPassword
)
  bool _fetched;
  HRESULT _res;
  UString_Wipe _password;
public:
  CCachedPassword(): _fetched(false), _res(S_OK) {}

  bool WasFetched() const { return _fetched; }

  /* Asks the password from (callback) and stores the result.
     Returns the result of callback->CryptoGetTextPassword(). */
  HRESULT Fetch(ICryptoGetTextPassword *callback);
};

#endif


#ifndef Z7_ST

// ---------------------------------------------------------------------------
// Pool of worker threads that execute tasks in submission (FIFO) order.
//
// The caller waits for the completion of each task with (task->Done).
// The pool doesn't own tasks: a task must stay alive until (Done) is set,
// or until Stop() returned.
// ---------------------------------------------------------------------------

class CMtTask
{
  Z7_CLASS_NO_COPY(CMtTask)
public:
  NWindows::NSynchronization::CManualResetEvent Done;
  bool ExceptionWasCaught; // Run() has thrown an exception (it must not)

  CMtTask(): ExceptionWasCaught(false) {}
  virtual ~CMtTask() {}

  /* Executed in a worker thread.
     (workerIndex) < number of threads: it allows per-worker state. */
  virtual void Run(unsigned workerIndex) = 0;
};


class CMtTaskPool
{
  Z7_CLASS_NO_COPY(CMtTaskPool)

  struct CWorker
  {
    NWindows::CThread Thread;
    CMtTaskPool *Pool;
    unsigned Index;
  };

  CObjectVector<CWorker> _workers;
  NWindows::NSynchronization::CCriticalSection _cs;
  NWindows::NSynchronization::CSemaphore _numQueued;
  CRecordVector<CMtTask *> _queue;   // protected by _cs
  unsigned _queueHead;               // protected by _cs
  bool _exit;                        // protected by _cs

  static THREAD_FUNC_DECL WorkerThread(void *p);
  void WorkerLoop(unsigned workerIndex);

public:
  CMtTaskPool(): _queueHead(0), _exit(false) {}
  ~CMtTaskPool() { Stop(); }

  /* Creates (numThreads) worker threads. Can be called again after Stop().
     If a thread can't be created, the already created threads are stopped. */
  HRESULT Start(unsigned numThreads);
  unsigned GetNumThreads() const { return _workers.Size(); }

  /* Queues the task. (task->Done) is created/reset here.
     Returns E_FAIL, if the pool was not started or was stopped. */
  HRESULT Submit(CMtTask *task);

  /* Waits for the end of the tasks that are running now and terminates the
     threads. Queued tasks that were not started are discarded: their (Done)
     will not be set. Can be called several times. */
  void Stop();
};

#endif // Z7_ST

}}

#endif
