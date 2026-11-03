// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// FloatCoder.h
// Heptazip: "FLOAT" method of 7z, lossless coder for arrays of floating-point
// numbers such as the weights of AI models (design 0008, block format in
// C/FloatPlane.h). The encoder chooses the float type (BF16, FP16, FP32) and the
// alignment for each block, so any data can be compressed; other data are stored.
//
// Coder properties: 1 byte, log2 of the block size (16 .. 22).
//
// Blocks are independent: with N threads, a ring of 2*N slots keeps N blocks in work
// while the calling thread reads the next blocks and writes the finished ones, in order.
// With 1 thread each block is processed in the calling thread (same code path).

#ifndef ZIP7_INC_COMPRESS_FLOAT_CODER_H
#define ZIP7_INC_COMPRESS_FLOAT_CODER_H

#include "../../../C/FloatPlaneLz.h"

#include "../../Common/MyBuffer.h"
#include "../../Common/MyCom.h"
#include "../../Common/MyVector.h"

#ifndef Z7_ST
#include "../Common/MtTaskPool.h"
#endif

#include "../ICoder.h"

namespace NCompress {
namespace NFloat {

const unsigned kBlockLogMin = 16;
const unsigned kBlockLogMax = 22;
const unsigned kBlockLogDefault = 20;
const UInt32 kNumThreadsMax = 64;

#ifndef Z7_ST
#define Z7_FLOAT_TASK_BASE  : public CMtTask
#define Z7_FLOAT_TASK_OVERRIDE  Z7_override
#else
#define Z7_FLOAT_TASK_BASE
#define Z7_FLOAT_TASK_OVERRIDE
#endif

// zstd for the blocks that are not floats (mode 2 of the format, C/FloatPlaneLz.h).
// One per slot: the slots run in parallel.
class CLzCoder
{
  Z7_CLASS_NO_COPY(CLzCoder)
public:
  CFloatPlaneZstd Zstd;
  CLzCoder();
  ~CLzCoder();
};

// One block in work: input, output and the result of Run().
class CSlot Z7_FLOAT_TASK_BASE
{
public:
  CByteBuffer In;
  CByteBuffer Out;
  size_t InLen;
  size_t OutLen;    // decoder: known from the block header before Run()
  size_t BlockSize;
  bool Encode;
  int Res;          // SRes of the decoder
  UInt64 DecodeNs;  // decoder: time of FloatPlane_DecodeBlock() in Run()
  CLzCoder LzCoder;

  void Run(unsigned workerIndex) Z7_FLOAT_TASK_OVERRIDE;
};

// The ring of slots shared by encoder and decoder.
class CSlotRing
{
  Z7_CLASS_NO_COPY(CSlotRing)
  CObjectVector<CSlot> _slots;
  #ifndef Z7_ST
  CMtTaskPool _pool;
  #endif
  UInt32 _numThreads;
public:
  CSlotRing(): _numThreads(1) {}
  // creates the slots and starts the threads (none for 1 thread)
  HRESULT Init(UInt32 numThreads, size_t inSize, size_t outSize, bool encode);
  unsigned NumSlots() const { return _slots.Size(); }
  CSlot &Slot(unsigned i) { return _slots[i]; }
  HRESULT Submit(unsigned i);       // runs the slot (now with 1 thread)
  HRESULT Wait(unsigned i);         // waits for the slot submitted last
  void Stop();                      // waits for the running slots, discards the others
};

class CDecoder Z7_final:
  public ICompressCoder,
  public ICompressSetDecoderProperties2,
  #ifndef Z7_ST
  public ICompressSetCoderMt,
  #endif
  public CMyUnknownImp
{
  Z7_COM_QI_BEGIN2(ICompressCoder)
  Z7_COM_QI_ENTRY(ICompressSetDecoderProperties2)
  #ifndef Z7_ST
  Z7_COM_QI_ENTRY(ICompressSetCoderMt)
  #endif
  Z7_COM_QI_END
  Z7_COM_ADDREF_RELEASE

  Z7_IFACE_COM7_IMP(ICompressCoder)
  Z7_IFACE_COM7_IMP(ICompressSetDecoderProperties2)
  #ifndef Z7_ST
  Z7_IFACE_COM7_IMP(ICompressSetCoderMt)
  #endif

  unsigned _blockLog;
  UInt32 _numThreads;
  CByteBuffer _inBuf;
  CSlotRing _ring;
  HRESULT Code2(ISequentialInStream *inStream, ISequentialOutStream *outStream,
      const UInt64 *outSize, ICompressProgressInfo *progress);
public:
  CDecoder(): _blockLog(kBlockLogDefault), _numThreads(1) {}
};

#ifndef Z7_EXTRACT_ONLY

class CEncoder Z7_final:
  public ICompressCoder,
  public ICompressSetCoderProperties,
  public ICompressWriteCoderProperties,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_3(ICompressCoder, ICompressSetCoderProperties, ICompressWriteCoderProperties)

  unsigned _blockLog;
  UInt32 _numThreads;
  CSlotRing _ring;
  HRESULT Code2(ISequentialInStream *inStream, ISequentialOutStream *outStream,
      ICompressProgressInfo *progress);
public:
  CEncoder(): _blockLog(kBlockLogDefault), _numThreads(1) {}
};

#endif

}}

#endif
