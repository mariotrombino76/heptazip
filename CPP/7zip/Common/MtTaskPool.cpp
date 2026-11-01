// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
// MtTaskPool.cpp
// Tensor7: see MtTaskPool.h

#include "StdAfx.h"

#ifndef Z7_ST

#include "MtTaskPool.h"

// ---------------------------------------------------------------------------
// CMtTaskPool
// ---------------------------------------------------------------------------

using namespace NWindows::NSynchronization;

static const UInt32 kMaxQueued = (UInt32)1 << 30;

THREAD_FUNC_DECL CMtTaskPool::WorkerThread(void *p)
{
  CWorker *w = (CWorker *)p;
  w->Pool->WorkerLoop(w->Index);
  return THREAD_FUNC_RET_ZERO;
}

void CMtTaskPool::WorkerLoop(unsigned workerIndex)
{
  for (;;)
  {
    if (_numQueued.Lock() != 0)
      return;
    CMtTask *task;
    {
      CCriticalSectionLock lock(_cs);
      if (_exit)
        return;
      task = _queue[_queueHead++];
      if (_queueHead == _queue.Size())
      {
        _queue.Clear();
        _queueHead = 0;
      }
    }
    try
    {
      task->Run(workerIndex);
    }
    catch(...)
    {
      task->ExceptionWasCaught = true;
    }
    task->Done.Set();
  }
}

HRESULT CMtTaskPool::Start(unsigned numThreads)
{
  if (!_workers.IsEmpty() || numThreads == 0)
    return E_FAIL;
  {
    // OptCreateInit(): Start() can be called again after Stop()
    const WRes wres = _numQueued.OptCreateInit(0, kMaxQueued);
    if (wres != 0)
      return HRESULT_FROM_WIN32(wres);
  }
  _exit = false;
  for (unsigned i = 0; i < numThreads; i++)
  {
    CWorker &w = _workers.AddNew();
    w.Pool = this;
    w.Index = i;
    const WRes wres = w.Thread.Create(WorkerThread, &w);
    if (wres != 0)
    {
      _workers.DeleteBack();
      Stop();
      return HRESULT_FROM_WIN32(wres);
    }
  }
  return S_OK;
}

HRESULT CMtTaskPool::Submit(CMtTask *task)
{
  {
    const WRes wres = task->Done.CreateIfNotCreated_Reset();
    if (wres != 0)
      return HRESULT_FROM_WIN32(wres);
  }
  task->ExceptionWasCaught = false;
  {
    CCriticalSectionLock lock(_cs);
    if (_exit || _workers.IsEmpty())
      return E_FAIL;
    _queue.Add(task);
  }
  const WRes wres = _numQueued.Release();
  return wres == 0 ? S_OK : HRESULT_FROM_WIN32(wres);
}

void CMtTaskPool::Stop()
{
  if (_workers.IsEmpty())
    return;
  {
    CCriticalSectionLock lock(_cs);
    _exit = true;
    _queue.Clear();
    _queueHead = 0;
  }
  // wake up all workers: each one sees (_exit) and returns
  _numQueued.Release(_workers.Size());
  FOR_VECTOR (i, _workers)
    _workers[i].Thread.Wait_Close();
  _workers.Clear();
}

#endif // Z7_ST
