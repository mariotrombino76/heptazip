// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// BrotliCoder.cpp
// Heptazip: brotli for the 7z format, see BrotliCoder.h and design 0014.

#include "StdAfx.h"

#include "../../../C/CpuArch.h"

#include "../../../third_party/brotli/c/include/brotli/decode.h"
#include "../../../third_party/brotli/c/include/brotli/encode.h"

#include "../../Common/MyBuffer.h"
#include "../../Windows/System.h"

#include "../Common/StreamUtils.h"

#ifndef Z7_ST
#include "../Common/MtTaskPool.h"
#endif

#include "BrotliCoder.h"

namespace NCompress {
namespace NBrotli {

static const UInt32 kMagicSkippable = 0x184D2A50;
static const UInt32 kMagicBr = 0x5242;   // "BR"
static const unsigned kHeaderSize = 16;
static const unsigned kNumThreadsMax = 64;
/* a frame of another encoder can be larger than ours (11 MiB): limits against
   corrupted headers */
static const UInt32 kFramePackMax = (UInt32)1 << 30;
static const size_t kFrameUnpackMax = (size_t)1 << 30;

/* size of the input of a frame: as 7-Zip-zstd (brotli-mt: 1 MiB * level) up to level 9;
   32 MiB for levels 10 and 11, where the frames cost more (silesia, q11: 23.90% with
   11 MiB frames, 23.66% with 32 MiB, 23.55% with 64 MiB, 23.48% as one stream).
   The decoders read frames of any size (the size is in the header). */
static size_t GetFrameSize(unsigned level)
{
  if (level >= 10)
    return (size_t)32 << 20;
  return (size_t)(level ? level : 1) << 20;
}

/* memory of the encoder for one thread: the buffers of a frame and the encoder of
   brotli (measured with lgwin 24: q11 ~230 MB for 32 MiB, q9 ~92 MB for 32 MiB) */
static UInt64 GetEncoderMemory(unsigned level, size_t frameSize)
{
  const UInt64 buffers = (UInt64)frameSize * 3;
  if (level >= 10)
    return buffers + ((UInt64)200 << 20);
  if (level >= 5)
    return buffers + (UInt64)frameSize + ((UInt64)24 << 20);
  return buffers + ((UInt64)8 << 20);
}

/* the window: as 7-Zip-zstd (24 bits, the maximum of brotli), but not larger than the
   frame (less memory for the decoder; the format is the same) */
static unsigned GetWindowLog(size_t frameSize)
{
  unsigned w = BROTLI_MIN_WINDOW_BITS;
  while (w < BROTLI_MAX_WINDOW_BITS && ((size_t)1 << w) - 16 < frameSize)
    w++;
  return w;
}

#ifndef Z7_ST
#define Z7_BROTLI_TASK_BASE  : public CMtTask
#define Z7_BROTLI_TASK_OVERRIDE  Z7_override
#else
#define Z7_BROTLI_TASK_BASE
#define Z7_BROTLI_TASK_OVERRIDE
#endif

/* one frame: the encoder compresses (In) to (Out = header + brotli stream), the
   decoder decompresses (In = brotli stream) to (Out) */
class CSlot Z7_BROTLI_TASK_BASE
{
public:
  CByteBuffer In;
  size_t InSize;
  CByteBuffer Out;
  size_t OutSize;
  HRESULT Res;
  bool Encode;
  unsigned Level;
  unsigned WindowLog;
  size_t FrameSize;   // encoder: frame size of the level, for the header
  UInt32 Hint;        // decoder: 64 KiB blocks of the output, from the header

  CSlot(): InSize(0), OutSize(0), Res(S_OK), Encode(true), Level(0), WindowLog(24), FrameSize(0), Hint(0) {}
  void Run(unsigned workerIndex) Z7_BROTLI_TASK_OVERRIDE;
private:
  void Compress();
  void Decompress();
};

void CSlot::Run(unsigned /* workerIndex */)
{
  // no exception may leave Run()
  try
  {
    if (Encode)
      Compress();
    else
      Decompress();
  }
  catch (...) { Res = E_OUTOFMEMORY; }
}

void CSlot::Compress()
{
  const size_t bound = BrotliEncoderMaxCompressedSize(InSize);
  if (bound == 0)
  {
    Res = E_FAIL;
    return;
  }
  Out.AllocAtLeast(kHeaderSize + bound);
  size_t size = bound;
  if (!BrotliEncoderCompress((int)Level, (int)WindowLog, BROTLI_MODE_GENERIC,
      InSize, In, &size, Out + kHeaderSize))
  {
    Res = E_FAIL;
    return;
  }
  Byte *h = Out;
  SetUi32(h, kMagicSkippable)
  SetUi32(h + 4, 8)
  SetUi32(h + 8, (UInt32)size)
  SetUi16(h + 12, (UInt16)kMagicBr)
  // as brotli-mt: the 64 KiB blocks of the frame (rounded up for a shorter frame)
  const UInt32 hint = (FrameSize > InSize) ?
      (UInt32)(InSize >> 16) + 1 : (UInt32)(FrameSize >> 16);
  SetUi16(h + 14, (UInt16)hint)
  OutSize = kHeaderSize + size;
  Res = S_OK;
}

void CSlot::Decompress()
{
  BrotliDecoderState *s = BrotliDecoderCreateInstance(NULL, NULL, NULL);
  if (!s)
  {
    Res = E_OUTOFMEMORY;
    return;
  }
  size_t cap = (size_t)Hint << 16;
  if (cap < ((size_t)1 << 16))
    cap = (size_t)1 << 16;
  if (cap > ((size_t)1 << 26))
    cap = (size_t)1 << 26;   // the hint is not trusted: grow on demand
  Out.AllocAtLeast(cap);
  size_t availIn = InSize;
  const uint8_t *nextIn = In;
  size_t total = 0;
  HRESULT res = S_FALSE;
  for (;;)
  {
    size_t availOut = Out.Size() - total;
    uint8_t *nextOut = Out + total;
    const BrotliDecoderResult r = BrotliDecoderDecompressStream(s,
        &availIn, &nextIn, &availOut, &nextOut, NULL);
    total = (size_t)(nextOut - (uint8_t *)Out);
    if (r == BROTLI_DECODER_RESULT_SUCCESS)
    {
      // the frame must be exactly the brotli stream
      res = (availIn == 0) ? S_OK : S_FALSE;
      break;
    }
    if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT)
    {
      if (Out.Size() >= kFrameUnpackMax)
        break;
      CByteBuffer b(Out.Size() * 2);
      memcpy(b, Out, total);
      Out.CopyFrom(b, b.Size());
      continue;
    }
    // NEEDS_MORE_INPUT: the frame is cut; ERROR: corrupted data
    break;
  }
  BrotliDecoderDestroyInstance(s);
  OutSize = total;
  Res = res;
}


// ---------- the frames in order, with a pool of threads ----------

class CPipeline
{
  Z7_CLASS_NO_COPY(CPipeline)
public:
  CObjectVector<CSlot> Slots;
  #ifndef Z7_ST
  CMtTaskPool Pool;
  #endif
  unsigned NumThreads;

  CPipeline(unsigned numThreads): NumThreads(numThreads)
  {
    if (NumThreads < 1) NumThreads = 1;
    if (NumThreads > kNumThreadsMax) NumThreads = kNumThreadsMax;
    #ifdef Z7_ST
    NumThreads = 1;
    #endif
    // the frames in work and a few more, read in advance or waiting to be written
    const unsigned numSlots = (NumThreads == 1) ? 1 : NumThreads + NumThreads / 2 + 1;
    for (unsigned i = 0; i < numSlots; i++)
      Slots.AddNew();
  }
  ~CPipeline()
  {
    #ifndef Z7_ST
    Pool.Stop();
    #endif
  }
  HRESULT Start()
  {
    #ifndef Z7_ST
    if (NumThreads > 1)
      return Pool.Start(NumThreads);
    #endif
    return S_OK;
  }
  HRESULT Submit(CSlot &s)
  {
    #ifndef Z7_ST
    if (NumThreads > 1)
      return Pool.Submit(&s);
    #endif
    s.Run(0);
    return S_OK;
  }
  HRESULT Wait(CSlot &s)
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
};


// ---------- encoder ----------

CEncoder::CEncoder():
  _level(kLevelDefault),
  _numThreads(1)
  {}

Z7_COM7F_IMF(CEncoder::SetCoderProperties(const PROPID *propIDs, const PROPVARIANT *coderProps, UInt32 numProps))
{
  _level = kLevelDefault;
  for (UInt32 i = 0; i < numProps; i++)
  {
    const PROPVARIANT &prop = coderProps[i];
    const PROPID propID = propIDs[i];
    if (propID >= NCoderPropID::kReduceSize)
      continue;
    if (prop.vt != VT_UI4)
      return E_INVALIDARG;
    const UInt32 v = prop.ulVal;
    switch (propID)
    {
      case NCoderPropID::kLevel:
        _level = (v > kLevelMax) ? kLevelMax : v;
        break;
      case NCoderPropID::kNumThreads:
        _numThreads = v;
        break;
      default:
        break;
    }
  }
  return S_OK;
}

Z7_COM7F_IMF(CEncoder::WriteCoderProperties(ISequentialOutStream *outStream))
{
  // as 7-Zip-zstd: version of brotli (major, minor) and level
  const UInt32 ver = BrotliEncoderVersion();
  Byte props[kPropsSize];
  props[0] = (Byte)(ver >> 24);
  props[1] = (Byte)((ver >> 12) & 0xFFF);
  props[2] = (Byte)_level;
  return WriteStream(outStream, props, kPropsSize);
}

#ifndef Z7_ST
Z7_COM7F_IMF(CEncoder::SetNumberOfThreads(UInt32 numThreads))
{
  _numThreads = numThreads;
  return S_OK;
}
#endif

Z7_COM7F_IMF(CEncoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 * /* inSize */, const UInt64 * /* outSize */, ICompressProgressInfo *progress))
{
  try
  {
    const size_t frameSize = GetFrameSize(_level);
    const unsigned windowLog = GetWindowLog(frameSize);
    UInt32 numThreads = _numThreads;
    {
      // at most half of the RAM for the encoders (q11: ~250 MB per thread)
      size_t ram = 0;
      if (NWindows::NSystem::GetRamSize(ram) && ram != 0)
      {
        const UInt64 perThread = GetEncoderMemory(_level, frameSize);
        const UInt64 maxThreads = (ram / 2) / perThread;
        if (numThreads > maxThreads)
          numThreads = (maxThreads == 0) ? 1 : (UInt32)maxThreads;
      }
    }
    CPipeline p(numThreads);
    const unsigned numSlots = p.Slots.Size();
    for (unsigned i = 0; i < numSlots; i++)
    {
      CSlot &s = p.Slots[i];
      s.In.AllocAtLeast(frameSize);
      s.Encode = true;
      s.Level = _level;
      s.WindowLog = windowLog;
      s.FrameSize = frameSize;
    }
    RINOK(p.Start())
    UInt64 inProcessed = 0, outProcessed = 0;
    unsigned next = 0, numInWork = 0;
    bool inEnd = false;
    for (;;)
    {
      while (!inEnd && numInWork < numSlots)
      {
        CSlot &s = p.Slots[(next + numInWork) % numSlots];
        size_t len = frameSize;
        RINOK(ReadStream(inStream, s.In, &len))
        if (len == 0)
        {
          inEnd = true;
          break;
        }
        if (len < frameSize)
          inEnd = true;
        s.InSize = len;
        numInWork++;
        RINOK(p.Submit(s))
      }
      if (numInWork == 0)
        break;
      CSlot &s = p.Slots[next];
      RINOK(p.Wait(s))
      RINOK(WriteStream(outStream, s.Out, s.OutSize))
      inProcessed += s.InSize;
      outProcessed += s.OutSize;
      if (progress)
      {
        RINOK(progress->SetRatioInfo(&inProcessed, &outProcessed))
      }
      next = (next + 1) % numSlots;
      numInWork--;
    }
    return S_OK;
  }
  catch (...) { return E_OUTOFMEMORY; }
}


// ---------- decoder ----------

CDecoder::CDecoder(): _numThreads(1) {}

Z7_COM7F_IMF(CDecoder::SetDecoderProperties2(const Byte * /* props */, UInt32 size))
{
  // the properties (versions, level) are informative; 7-Zip-zstd writes 3 bytes
  return (size >= kPropsSize && size <= 5) ? S_OK : E_NOTIMPL;
}

#ifndef Z7_ST
Z7_COM7F_IMF(CDecoder::SetNumberOfThreads(UInt32 numThreads))
{
  _numThreads = numThreads;
  return S_OK;
}
#endif

/* a single brotli stream without frames (the .br format of a single thread), the first
   (numPre) bytes of it already read in (pre) */
static HRESULT DecodeRaw(const Byte *pre, size_t numPre, ISequentialInStream *inStream,
    ISequentialOutStream *outStream, UInt64 &outTotal, ICompressProgressInfo *progress)
{
  BrotliDecoderState *s = BrotliDecoderCreateInstance(NULL, NULL, NULL);
  if (!s)
    return E_OUTOFMEMORY;
  const size_t kBufSize = (size_t)1 << 20;
  CByteBuffer inBuf(kBufSize), outBuf(kBufSize);
  memcpy(inBuf, pre, numPre);
  size_t availIn = numPre;
  const uint8_t *nextIn = inBuf;
  bool inEnd = false;
  UInt64 inTotal = numPre;
  HRESULT res = S_OK;
  for (;;)
  {
    size_t availOut = kBufSize;
    uint8_t *nextOut = outBuf;
    const BrotliDecoderResult r = BrotliDecoderDecompressStream(s,
        &availIn, &nextIn, &availOut, &nextOut, NULL);
    const size_t produced = (size_t)(nextOut - (uint8_t *)outBuf);
    if (produced)
    {
      res = WriteStream(outStream, outBuf, produced);
      if (res != S_OK)
        break;
      outTotal += produced;
      if (progress)
      {
        res = progress->SetRatioInfo(&inTotal, &outTotal);
        if (res != S_OK)
          break;
      }
    }
    if (r == BROTLI_DECODER_RESULT_SUCCESS)
    {
      // data after the stream
      if (availIn != 0)
        res = S_FALSE;
      else
      {
        Byte b;
        size_t one = 1;
        res = ReadStream(inStream, &b, &one);
        if (res == S_OK && one != 0)
          res = S_FALSE;
      }
      break;
    }
    if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT)
      continue;
    if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && !inEnd)
    {
      size_t len = kBufSize;
      res = ReadStream(inStream, inBuf, &len);
      if (res != S_OK)
        break;
      if (len == 0)
        inEnd = true;
      inTotal += len;
      availIn = len;
      nextIn = inBuf;
      if (len != 0)
        continue;
    }
    res = S_FALSE;
    break;
  }
  BrotliDecoderDestroyInstance(s);
  return res;
}

Z7_COM7F_IMF(CDecoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 * /* inSize */, const UInt64 *outSize, ICompressProgressInfo *progress))
{
  try
  {
    UInt64 outTotal = 0;
    CPipeline p(_numThreads);
    const unsigned numSlots = p.Slots.Size();
    for (unsigned i = 0; i < numSlots; i++)
      p.Slots[i].Encode = false;
    bool started = false;
    UInt64 inProcessed = 0;
    unsigned next = 0, numInWork = 0;
    bool inEnd = false;
    bool first = true;
    HRESULT readRes = S_OK;   // the error of the input, after the frames before it
    for (;;)
    {
      while (!inEnd && numInWork < numSlots)
      {
        Byte h[kHeaderSize];
        size_t len = kHeaderSize;
        readRes = ReadStream(inStream, h, &len);
        if (readRes != S_OK || len == 0)
        {
          inEnd = true;
          break;
        }
        if (len < kHeaderSize || GetUi32(h) != kMagicSkippable)
        {
          if (first && (len < 4 || GetUi32(h) != kMagicSkippable))
          {
            // not the format of 7-Zip-zstd: a single brotli stream
            RINOK(DecodeRaw(h, len, inStream, outStream, outTotal, progress))
            if (outSize && outTotal != *outSize)
              return S_FALSE;
            return S_OK;
          }
          readRes = S_FALSE;
          inEnd = true;
          break;
        }
        if (GetUi32(h + 4) != 8 || GetUi16(h + 12) != kMagicBr)
        {
          readRes = S_FALSE;
          inEnd = true;
          break;
        }
        first = false;
        const UInt32 packSize = GetUi32(h + 8);
        if (packSize > kFramePackMax)
        {
          readRes = S_FALSE;
          inEnd = true;
          break;
        }
        CSlot &s = p.Slots[(next + numInWork) % numSlots];
        s.In.AllocAtLeast(packSize ? packSize : 1);
        size_t got = packSize;
        readRes = ReadStream(inStream, s.In, &got);
        if (readRes == S_OK && got != packSize)
          readRes = S_FALSE;
        if (readRes != S_OK)
        {
          inEnd = true;
          break;
        }
        inProcessed += kHeaderSize + packSize;
        s.InSize = packSize;
        s.Hint = GetUi16(h + 14);
        if (!started)
        {
          RINOK(p.Start())
          started = true;
        }
        numInWork++;
        RINOK(p.Submit(s))
      }
      if (numInWork == 0)
        break;
      CSlot &s = p.Slots[next];
      RINOK(p.Wait(s))
      if (outSize && outTotal + s.OutSize > *outSize)
        return S_FALSE;
      RINOK(WriteStream(outStream, s.Out, s.OutSize))
      outTotal += s.OutSize;
      if (progress)
      {
        RINOK(progress->SetRatioInfo(&inProcessed, &outTotal))
      }
      next = (next + 1) % numSlots;
      numInWork--;
    }
    RINOK(readRes)
    if (outSize && outTotal != *outSize)
      return S_FALSE;
    return S_OK;
  }
  catch (...) { return E_OUTOFMEMORY; }
}

}}
