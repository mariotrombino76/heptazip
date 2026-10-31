// MtTaskPool.h
// 7zip-modern: pool of worker threads that run tasks in FIFO order.
// Used by the parallel extraction (design 0001) and compression (design 0003)
// of 7z folders.

#ifndef ZIP7_INC_MT_TASK_POOL_H
#define ZIP7_INC_MT_TASK_POOL_H

#ifndef Z7_ST

#include "../../Common/MyTypes.h"
#include "../../Common/MyVector.h"
#include "../../Common/MyWindows.h"
#include "../../Windows/Synchronization.h"
#include "../../Windows/Thread.h"

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

#endif
