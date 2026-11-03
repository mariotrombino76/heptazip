// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// MtTaskPool.cpp
// Heptazip: see MtTaskPool.h

#include "StdAfx.h"

#ifndef Z7_ST

#include "MtTaskPool.h"

// ---------------------------------------------------------------------------
// CMtTaskPool
// ---------------------------------------------------------------------------

using namespace NWindows::NSynchronization;

THREAD_FUNC_DECL CMtTaskPool::WorkerThread(void *p)
{
  CWorker *w = (CWorker *)p;
  w->Pool->WorkerLoop(w->Index);
  return THREAD_FUNC_RET_ZERO;
}

void CMtTaskPool::WorkerLoop(unsigned workerIndex)
{
  CWorker &w = _workers[workerIndex];
  for (;;)
  {
    CMtTask *task = NULL;
    {
      CCriticalSectionLock lock(_cs);
      if (_exit)
        return;
      if (_queueHead != _queue.Size())
      {
        task = _queue[_queueHead++];
        if (_queueHead == _queue.Size())
        {
          _queue.Clear();
          _queueHead = 0;
        }
      }
      else
        _idle.Add(workerIndex);
    }
    if (!task)
    {
      // Submit() sets (Task) and then (Wake); Stop() sets (_exit) and then (Wake)
      if (w.Wake.Lock() != 0)
        return;
      CCriticalSectionLock lock(_cs);
      if (_exit)
        return;
      task = w.Task;
      w.Task = NULL;
      if (!task)
        continue;
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
  _exit = false;
  _idle.Clear();
  _queue.Clear();
  _queueHead = 0;
  unsigned i;
  // all the workers (and their events) exist before the first thread runs
  for (i = 0; i < numThreads; i++)
  {
    CWorker &w = _workers.AddNew();
    w.Pool = this;
    w.Index = i;
    w.Task = NULL;
    const WRes wres = w.Wake.CreateIfNotCreated_Reset();
    if (wres != 0)
    {
      _workers.Clear();
      return HRESULT_FROM_WIN32(wres);
    }
  }
  for (i = 0; i < numThreads; i++)
  {
    const WRes wres = _workers[i].Thread.Create(WorkerThread, &_workers[i]);
    if (wres != 0)
    {
      Stop();   // only the created threads are waited for
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
  CWorker *w = NULL;
  {
    CCriticalSectionLock lock(_cs);
    if (_exit || _workers.IsEmpty())
      return E_FAIL;
    if (!_idle.IsEmpty())
    {
      w = &_workers[_idle.Back()];
      _idle.DeleteBack();
      w->Task = task;
    }
    else
      _queue.Add(task);
  }
  if (w)
  {
    const WRes wres = w->Wake.Set();
    if (wres != 0)
      return HRESULT_FROM_WIN32(wres);
  }
  return S_OK;
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
    _idle.Clear();
  }
  // wake up all workers: each one sees (_exit) and returns
  FOR_VECTOR (i, _workers)
    _workers[i].Wake.Set();
  FOR_VECTOR (i, _workers)
    if (_workers[i].Thread.IsCreated())
      _workers[i].Thread.Wait_Close();
  _workers.Clear();
}

#endif // Z7_ST
