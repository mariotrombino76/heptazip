// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// SlotPipeline.h
// Heptazip: a ring of slots coded by a pool of threads and consumed in order.
//
// Used by the block codecs (Deflate chunks, brotli frames): the caller fills
// the slots in order, submits them, waits for the oldest one and writes its
// output, so the output is the same for any number of threads. With one
// thread (or Z7_ST) the slot is coded in the calling thread.

#ifndef ZIP7_INC_SLOT_PIPELINE_H
#define ZIP7_INC_SLOT_PIPELINE_H

#include "../../Common/MyTypes.h"
#include "../../Common/MyVector.h"
#include "../../Common/MyWindows.h"

#ifndef Z7_ST
#include "MtTaskPool.h"
#define Z7_SLOT_TASK_BASE  : public CMtTask
#define Z7_SLOT_TASK_OVERRIDE  Z7_override
#else
#define Z7_SLOT_TASK_BASE
#define Z7_SLOT_TASK_OVERRIDE
#endif

/* TSlot: class TSlot Z7_SLOT_TASK_BASE, with
     void Run(unsigned workerIndex) Z7_SLOT_TASK_OVERRIDE;  // no exception may leave it
     HRESULT Res;                                          // result of Run() */
template <class TSlot>
class CSlotPipeline
{
  Z7_CLASS_NO_COPY(CSlotPipeline)
public:
  static const unsigned kNumThreadsMax = 64;

  CObjectVector<TSlot> Slots;
  #ifndef Z7_ST
  CMtTaskPool Pool;
  #endif
  unsigned NumThreads;

  CSlotPipeline(): NumThreads(1) {}
  ~CSlotPipeline()
  {
    #ifndef Z7_ST
    Pool.Stop();
    #endif
  }

  /* Clamps the number of threads and creates the slots: the slots in work and a
     few more, read in advance or waiting to be written in order (measured with
     Deflate on silesia, 20 threads: as fast as 2 slots per thread with less
     memory; with (numThreads + 2) slots 6% slower). The slots are kept between
     calls. */
  void Init(unsigned numThreads)
  {
    if (numThreads < 1)
      numThreads = 1;
    if (numThreads > kNumThreadsMax)
      numThreads = kNumThreadsMax;
    #ifdef Z7_ST
    numThreads = 1;
    #endif
    NumThreads = numThreads;
    const unsigned numSlots = (numThreads == 1) ? 1 : numThreads + numThreads / 2 + 1;
    while (Slots.Size() < numSlots)
      Slots.AddNew();
  }

  unsigned NumSlots() const { return Slots.Size(); }

  HRESULT Start()
  {
    #ifndef Z7_ST
    if (NumThreads > 1)
      return Pool.Start(NumThreads);
    #endif
    return S_OK;
  }

  HRESULT Submit(TSlot &s)
  {
    #ifndef Z7_ST
    if (NumThreads > 1)
      return Pool.Submit(&s);
    #endif
    s.Run(0);
    return S_OK;
  }

  /* Waits for the slot and returns the result of its Run(). */
  HRESULT Wait(TSlot &s)
  {
    #ifndef Z7_ST
    if (NumThreads > 1)
    {
      const WRes wres = s.Done.Lock();
      if (wres != 0)
        return HRESULT_FROM_WIN32(wres);
      if (s.ExceptionWasCaught)
        return E_FAIL;
    }
    #endif
    return s.Res;
  }

  void Stop()
  {
    #ifndef Z7_ST
    Pool.Stop();
    #endif
  }
};

#endif
