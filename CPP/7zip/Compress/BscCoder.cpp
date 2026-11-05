// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// BscCoder.cpp
// Heptazip: the "BWT" method of the 7z format, see BscCoder.h and design 0016.

#include "StdAfx.h"

#include "../../../C/CpuArch.h"

#include "libbsc/libbsc.h"

#include "../../Common/MyBuffer.h"
#include "../../Windows/System.h"

#include "../Common/StreamUtils.h"

#include "../Common/SlotPipeline.h"

#include "BscCoder.h"

namespace NCompress {
namespace NBwt {

static const unsigned kHeaderSize = LIBBSC_HEADER_SIZE;   // 28
// a block of data is at most 1 GiB (libbsc works on int sizes and needs 5 x the block):
// a header that declares more is corrupted and must not make the decoder allocate it
static const UInt32 kBlockMax = (UInt32)1 << 30;
static const UInt32 kBlockMin = (UInt32)1 << 16;

/* the features of libbsc: no OpenMP (the parallelism is per block, in the pipeline);
   FASTMODE as in the default of bsc */
static const int kFeatures = LIBBSC_FEATURE_FASTMODE;

/* the tables of libbsc are initialized once; a local static is initialized once even
   with several threads (C++11) */
static bool BscInit()
{
  static const int res = bsc_init(kFeatures);
  return res == LIBBSC_NO_ERROR;
}

/* the block size and the coder of a level (measured on Silesia, docs/design/0016 §4):
   the ratio gains with the block up to 16 MiB (8 MiB static 22.85%, 16 MiB 22.68%,
   32 and 64 MiB 22.68%) and with the adaptive QLFC coder (16 MiB: 22.41%); smaller
   blocks give more parallelism in decompression (2 MiB: 379 MB/s, 16 MiB: 117-188 MB/s
   with 20 threads) and cost less memory (7 x block per thread). Bigger blocks only
   help on big homogeneous texts: levels 6-9 use 32 and 64 MiB (128 and 256 MiB were
   worse on Silesia, whose files are smaller). */
static UInt32 GetBlockSize(unsigned level)
{
  if (level <= 1) return (UInt32)1 << 21;    //  2 MiB
  if (level <= 2) return (UInt32)1 << 22;    //  4 MiB
  if (level <= 3) return (UInt32)1 << 23;    //  8 MiB
  if (level <= 5) return (UInt32)1 << 24;    // 16 MiB (4, 5)
  if (level <= 6) return (UInt32)1 << 25;    // 32 MiB
  return (UInt32)1 << 26;                    // 64 MiB (7, 8, 9)
}

static int GetCoder(unsigned level)
{
  if (level <= 2) return LIBBSC_CODER_QLFC_FAST;
  if (level <= 4) return LIBBSC_CODER_QLFC_STATIC;
  return LIBBSC_CODER_QLFC_ADAPTIVE;
}

/* LZP before the BWT, as the default of bsc (hash of 2^15 entries, matches of at
   least 72 bytes): cheap, and it shortens the long repetitions that the BWT handles
   badly */
static const int kLzpHashSize = LIBBSC_DEFAULT_LZPHASHSIZE;
static const int kLzpMinLen = LIBBSC_DEFAULT_LZPMINLEN;

/* memory of one slot: the input and output buffers (2 x block + header) and the work
   memory of libbsc (about 5 x block for the BWT, as its documentation says) */
static UInt64 GetSlotMemory(UInt64 blockSize)
{
  return blockSize * 7 + ((UInt64)16 << 20);
}


/* one block: the encoder compresses (In) to (Out = libbsc header + data), the decoder
   decompresses (In = header + data) to (Out) */
class CSlot Z7_SLOT_TASK_BASE
{
public:
  CByteBuffer In;
  size_t InSize;
  CByteBuffer Out;
  size_t OutSize;
  HRESULT Res;
  bool Encode;
  int Coder;
  size_t DataSize;    // decoder: uncompressed size from the header

  CSlot(): InSize(0), OutSize(0), Res(S_OK), Encode(true), Coder(LIBBSC_CODER_QLFC_STATIC), DataSize(0) {}
  void Run(unsigned workerIndex) Z7_SLOT_TASK_OVERRIDE;
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
  Out.AllocAtLeast(InSize + kHeaderSize);
  int r = bsc_compress(In, Out, (int)InSize, kLzpHashSize, kLzpMinLen,
      LIBBSC_BLOCKSORTER_BWT, Coder, kFeatures);
  if (r == LIBBSC_NOT_COMPRESSIBLE)
    r = bsc_store(In, Out, (int)InSize, kFeatures);
  if (r < 0)
  {
    Res = (r == LIBBSC_NOT_ENOUGH_MEMORY) ? E_OUTOFMEMORY : E_FAIL;
    return;
  }
  OutSize = (size_t)r;
  Res = S_OK;
}

void CSlot::Decompress()
{
  Out.AllocAtLeast(DataSize ? DataSize : 1);
  const int r = bsc_decompress(In, (int)InSize, Out, (int)DataSize, kFeatures);
  if (r != LIBBSC_NO_ERROR)
  {
    // corrupted data (also the checksums of libbsc), or memory
    Res = (r == LIBBSC_NOT_ENOUGH_MEMORY) ? E_OUTOFMEMORY : S_FALSE;
    return;
  }
  OutSize = DataSize;
  Res = S_OK;
}


// ---------- the blocks in order, with a pool of threads ----------

typedef CSlotPipeline<CSlot> CPipeline;

/* at most half of the RAM for the slots */
static UInt32 LimitThreadsByMemory(UInt32 numThreads, UInt64 blockSize)
{
  size_t ram = 0;
  if (NWindows::NSystem::GetRamSize(ram) && ram != 0)
  {
    const UInt64 perThread = GetSlotMemory(blockSize);
    const UInt64 maxThreads = (ram / 2) / perThread;
    if (numThreads > maxThreads)
      numThreads = (maxThreads == 0) ? 1 : (UInt32)maxThreads;
  }
  return numThreads;
}


// ---------- encoder ----------

CEncoder::CEncoder():
  _level(kLevelDefault),
  _blockSize(0),
  _coder(-1),
  _numThreads(1)
  {}

Z7_COM7F_IMF(CEncoder::SetCoderProperties(const PROPID *propIDs, const PROPVARIANT *coderProps, UInt32 numProps))
{
  _level = kLevelDefault;
  _blockSize = 0;
  _coder = -1;
  for (UInt32 i = 0; i < numProps; i++)
  {
    const PROPVARIANT &prop = coderProps[i];
    const PROPID propID = propIDs[i];
    if (propID >= NCoderPropID::kReduceSize)
      continue;
    // the block is the "dictionary" of a BWT: the d property (the Dictionary of the
    // Windows dialog, "0d=") sets it too
    if (propID == NCoderPropID::kBlockSize || propID == NCoderPropID::kDictionarySize)
    {
      UInt64 v;
      if (prop.vt == VT_UI4) v = prop.ulVal;
      else if (prop.vt == VT_UI8) v = prop.uhVal.QuadPart;
      else return E_INVALIDARG;
      if (v < kBlockMin) v = kBlockMin;
      if (v > kBlockMax) v = kBlockMax;
      _blockSize = v;
      continue;
    }
    if (prop.vt != VT_UI4)
      return E_INVALIDARG;
    const UInt32 v = prop.ulVal;
    switch (propID)
    {
      case NCoderPropID::kLevel:
        _level = (v > kLevelMax) ? kLevelMax : v;
        break;
      case NCoderPropID::kAlgorithm:
        if (v > 2)
          return E_INVALIDARG;
        _coder = (v == 0) ? LIBBSC_CODER_QLFC_FAST : (v == 1 ? LIBBSC_CODER_QLFC_STATIC : LIBBSC_CODER_QLFC_ADAPTIVE);
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
  Byte props[kPropsSize];
  props[0] = kFormatVersion;
  props[1] = (Byte)_level;
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
    if (!BscInit())
      return E_FAIL;
    const size_t blockSize = (size_t)(_blockSize ? _blockSize : GetBlockSize(_level));
    const int coder = (_coder >= 0) ? _coder : GetCoder(_level);
    const UInt32 numThreads = LimitThreadsByMemory(_numThreads, blockSize);
    CPipeline p;
    p.Init(numThreads);
    const unsigned numSlots = p.Slots.Size();
    for (unsigned i = 0; i < numSlots; i++)
    {
      CSlot &s = p.Slots[i];
      s.In.AllocAtLeast(blockSize);
      s.Encode = true;
      s.Coder = coder;
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
        size_t len = blockSize;
        RINOK(ReadStream(inStream, s.In, &len))
        if (len == 0)
        {
          inEnd = true;
          break;
        }
        if (len < blockSize)
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

Z7_COM7F_IMF(CDecoder::SetDecoderProperties2(const Byte *props, UInt32 size))
{
  // the format version must be known; the level is informative
  if (size < 1 || size > 4)
    return E_NOTIMPL;
  return (props[0] == kFormatVersion) ? S_OK : E_NOTIMPL;
}

#ifndef Z7_ST
Z7_COM7F_IMF(CDecoder::SetNumberOfThreads(UInt32 numThreads))
{
  _numThreads = numThreads;
  return S_OK;
}
#endif

Z7_COM7F_IMF(CDecoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress))
{
  try
  {
    if (!BscInit())
      return E_FAIL;
    UInt64 outTotal = 0;
    UInt64 inProcessed = 0;
    /* the block size is known only from the headers: the first header is read before
       the pipeline is sized, so that the threads (and the slots, a block each) stay
       within the memory for blocks of this size */
    Byte h[kHeaderSize];
    bool haveHeader = false;
    int blockSize0 = 0, dataSize0 = 0;
    HRESULT readRes = S_OK;   // the error of the input, after the blocks before it
    {
      size_t len = kHeaderSize;
      readRes = ReadStream(inStream, h, &len);
      if (readRes != S_OK)
        return readRes;
      if (len != 0)
      {
        if (len < kHeaderSize
            || bsc_block_info(h, (int)kHeaderSize, &blockSize0, &dataSize0, kFeatures) != LIBBSC_NO_ERROR)
          return S_FALSE;
        haveHeader = true;
      }
    }
    if (!haveHeader)
    {
      // an empty stream
      if (outSize && *outSize != 0)
        return S_FALSE;
      return S_OK;
    }
    CPipeline p;
    p.Init(LimitThreadsByMemory(_numThreads, (UInt64)(UInt32)(dataSize0 > 0 ? dataSize0 : 1)));
    const unsigned numSlots = p.Slots.Size();
    for (unsigned i = 0; i < numSlots; i++)
      p.Slots[i].Encode = false;
    RINOK(p.Start())
    unsigned next = 0, numInWork = 0;
    bool inEnd = false;
    for (;;)
    {
      while (!inEnd && numInWork < numSlots)
      {
        int blockSize, dataSize;
        if (haveHeader)
        {
          haveHeader = false;
          blockSize = blockSize0;
          dataSize = dataSize0;
        }
        else
        {
          size_t len = kHeaderSize;
          readRes = ReadStream(inStream, h, &len);
          if (readRes != S_OK || len == 0)
          {
            inEnd = true;
            break;
          }
          blockSize = dataSize = 0;
          if (len < kHeaderSize
              || bsc_block_info(h, (int)kHeaderSize, &blockSize, &dataSize, kFeatures) != LIBBSC_NO_ERROR)
          {
            readRes = S_FALSE;
            inEnd = true;
            break;
          }
        }
        if (blockSize < (int)kHeaderSize || dataSize < 0
            || (UInt32)blockSize > kBlockMax + kHeaderSize || (UInt32)dataSize > kBlockMax
            || (inSize && *inSize - inProcessed < (UInt64)(UInt32)blockSize))
        {
          readRes = S_FALSE;
          inEnd = true;
          break;
        }
        CSlot &s = p.Slots[(next + numInWork) % numSlots];
        s.In.AllocAtLeast((size_t)blockSize);
        memcpy(s.In, h, kHeaderSize);
        size_t got = (size_t)blockSize - kHeaderSize;
        readRes = ReadStream(inStream, s.In + kHeaderSize, &got);
        if (readRes == S_OK && got != (size_t)blockSize - kHeaderSize)
          readRes = S_FALSE;
        if (readRes != S_OK)
        {
          inEnd = true;
          break;
        }
        inProcessed += (UInt32)blockSize;
        s.InSize = (size_t)blockSize;
        s.DataSize = (size_t)dataSize;
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
