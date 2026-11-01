// FloatCoder.cpp
// 7zip-modern: "FLOAT" method of 7z (design 0008).

#include "StdAfx.h"

#include <string.h>

#include "../../../C/FloatPlane.h"

#include "../Common/StreamUtils.h"

#include "FloatCoder.h"

namespace NCompress {
namespace NFloat {

// ---------- Slots ----------

void CSlot::Run(unsigned /* workerIndex */)
{
  if (Encode)
    OutLen = FloatPlane_EncodeBlock(In, InLen, Out);
  else
  {
    size_t used = 0;
    Res = FloatPlane_DecodeBlock(In, InLen, Out, BlockSize, &used);
    if (Res == SZ_OK && used != InLen)   // InLen came from FloatPlane_GetBlockSize()
      Res = SZ_ERROR_DATA;
  }
}

HRESULT CSlotRing::Init(UInt32 numThreads, size_t inSize, size_t outSize, bool encode)
{
  Stop();
  if (numThreads < 1)
    numThreads = 1;
  #ifdef Z7_ST
  numThreads = 1;
  #endif
  _numThreads = numThreads;
  // 2 slots per thread: one in work, one being read or written by the caller
  const unsigned numSlots = numThreads == 1 ? 1 : (unsigned)numThreads * 2;
  while (_slots.Size() > numSlots)
    _slots.DeleteBack();
  while (_slots.Size() < numSlots)
    _slots.AddNew();
  for (unsigned i = 0; i < numSlots; i++)
  {
    CSlot &s = _slots[i];
    s.In.AllocAtLeast(inSize);
    s.Out.AllocAtLeast(outSize);
    s.Encode = encode;
    s.InLen = s.OutLen = 0;
    s.Res = SZ_OK;
  }
  #ifndef Z7_ST
  if (numThreads > 1)
    return _pool.Start(numThreads);
  #endif
  return S_OK;
}

HRESULT CSlotRing::Submit(unsigned i)
{
  #ifndef Z7_ST
  if (_numThreads > 1)
    return _pool.Submit(&_slots[i]);
  #endif
  _slots[i].Run(0);
  return S_OK;
}

HRESULT CSlotRing::Wait(unsigned i)
{
  #ifndef Z7_ST
  if (_numThreads > 1)
  {
    CSlot &s = _slots[i];
    const WRes wres = s.Done.Lock();
    if (wres != 0)
      return HRESULT_FROM_WIN32(wres);
    if (s.ExceptionWasCaught)
      return E_FAIL;
  }
  #else
  UNUSED_VAR(i)
  #endif
  return S_OK;
}

void CSlotRing::Stop()
{
  #ifndef Z7_ST
  _pool.Stop();
  #endif
}


// ---------- Decoder ----------

static HRESULT SResToHRESULT_Float(int res)
{
  switch (res)
  {
    case SZ_OK: return S_OK;
    case SZ_ERROR_MEM: return E_OUTOFMEMORY;
    case SZ_ERROR_PARAM: return E_INVALIDARG;
    default: return S_FALSE;
  }
}

Z7_COM7F_IMF(CDecoder::SetDecoderProperties2(const Byte *props, UInt32 size))
{
  if (size != 1 || props[0] < kBlockLogMin || props[0] > kBlockLogMax)
    return E_NOTIMPL;
  _blockLog = props[0];
  return S_OK;
}

#ifndef Z7_ST
Z7_COM7F_IMF(CDecoder::SetNumberOfThreads(UInt32 numThreads))
{
  if (numThreads < 1)
    numThreads = 1;
  if (numThreads > kNumThreadsMax)
    numThreads = kNumThreadsMax;
  _numThreads = numThreads;
  return S_OK;
}
#endif

/* The calling thread reads the stream into _inBuf, finds each block with
   FloatPlane_GetBlockSize() and copies it to a free slot; the slots decode in
   parallel; the caller writes their outputs in order. The input buffer holds the
   largest encoded block (stored: block + 5 bytes). */
HRESULT CDecoder::Code2(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 *outSize, ICompressProgressInfo *progress)
{
  const size_t blockSize = (size_t)1 << _blockLog;
  const size_t inCap = FLOAT_PLANE_ENCODED_MAX(blockSize);
  _inBuf.AllocAtLeast(inCap);
  RINOK(_ring.Init(_numThreads, inCap, blockSize, false))
  const unsigned numSlots = _ring.NumSlots();

  size_t inPos = 0, inLim = 0;
  bool inEnd = false;
  bool streamEnd = false;     // no more blocks will be submitted
  HRESULT endRes = S_OK;      // result once the submitted blocks are written
  UInt64 inProcessed = 0, outProcessed = 0;
  UInt64 outQueued = 0;       // bytes of the submitted blocks
  unsigned next = 0, numInWork = 0;

  for (;;)
  {
    // 1. Fill free slots with the next blocks.
    while (!streamEnd && numInWork < numSlots)
    {
      if (outSize && outQueued == *outSize)
      {
        streamEnd = true;
        break;
      }
      size_t encSize = 0;
      SRes res = SZ_ERROR_INPUT_EOF;
      if (inLim != inPos)
        res = FloatPlane_GetBlockSize(_inBuf + inPos, inLim - inPos, &encSize);
      if (res == SZ_OK && encSize > inLim - inPos)
        res = SZ_ERROR_INPUT_EOF;
      if (res == SZ_ERROR_INPUT_EOF)
      {
        if (inEnd)
        {
          streamEnd = true;
          // the end: after a whole block without outSize, else the data are truncated
          if (inLim != inPos || outSize)
            endRes = S_FALSE;
          break;
        }
        if (encSize > inCap)
        {
          streamEnd = true;
          endRes = S_FALSE;   // a block larger than the props allow
          break;
        }
        memmove(_inBuf, _inBuf + inPos, inLim - inPos);
        inLim -= inPos;
        inPos = 0;
        size_t rem = inCap - inLim;
        RINOK(ReadStream(inStream, _inBuf + inLim, &rem))
        if (rem == 0)
          inEnd = true;
        inLim += rem;
        continue;
      }
      size_t outLen = 0;
      if (res == SZ_OK)
        res = FloatPlane_GetBlockOutLen(_inBuf + inPos, inLim - inPos, &outLen);
      if (res != SZ_OK || outLen > blockSize || (outSize && outLen > *outSize - outQueued))
      {
        streamEnd = true;
        endRes = S_FALSE;
        break;
      }
      const unsigned k = (next + numInWork) % numSlots;
      CSlot &s = _ring.Slot(k);
      memcpy(s.In, _inBuf + inPos, encSize);
      s.InLen = encSize;
      s.OutLen = outLen;
      s.BlockSize = blockSize;
      inPos += encSize;
      outQueued += outLen;
      numInWork++;
      const HRESULT hres = _ring.Submit(k);
      if (hres != S_OK)
      {
        _ring.Stop();
        return hres;
      }
    }

    // 2. Write the oldest block.
    if (numInWork == 0)
      return endRes;
    CSlot &s = _ring.Slot(next);
    HRESULT hres = _ring.Wait(next);
    if (hres == S_OK)
      hres = SResToHRESULT_Float(s.Res);
    if (hres == S_OK)
      hres = WriteStream(outStream, s.Out, s.OutLen);
    if (hres == S_OK)
    {
      inProcessed += s.InLen;
      outProcessed += s.OutLen;
      if (progress)
        hres = progress->SetRatioInfo(&inProcessed, &outProcessed);
    }
    if (hres != S_OK)
    {
      _ring.Stop();
      return hres;
    }
    next = (next + 1) % numSlots;
    numInWork--;
  }
}

Z7_COM7F_IMF(CDecoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 * /* inSize */, const UInt64 *outSize, ICompressProgressInfo *progress))
{
  const HRESULT res = Code2(inStream, outStream, outSize, progress);
  _ring.Stop();
  return res;
}


#ifndef Z7_EXTRACT_ONLY

// ---------- Encoder ----------

Z7_COM7F_IMF(CEncoder::SetCoderProperties(const PROPID *propIDs, const PROPVARIANT *coderProps, UInt32 numProps))
{
  unsigned blockLog = kBlockLogDefault;
  UInt32 numThreads = 1;
  for (UInt32 i = 0; i < numProps; i++)
  {
    const PROPVARIANT &prop = coderProps[i];
    switch (propIDs[i])
    {
      case NCoderPropID::kBlockSize:
      {
        UInt64 v;
        if (prop.vt == VT_UI4)
          v = prop.ulVal;
        else if (prop.vt == VT_UI8)
          v = prop.uhVal.QuadPart;
        else
          return E_INVALIDARG;
        // power of 2 in [64 KiB, 4 MiB]
        unsigned k;
        for (k = kBlockLogMin; k <= kBlockLogMax; k++)
          if (v == ((UInt64)1 << k))
            break;
        if (k > kBlockLogMax)
          return E_INVALIDARG;
        blockLog = k;
        break;
      }
      case NCoderPropID::kNumThreads:
        if (prop.vt != VT_UI4)
          return E_INVALIDARG;
        numThreads = prop.ulVal;
        if (numThreads < 1)
          numThreads = 1;
        if (numThreads > kNumThreadsMax)
          numThreads = kNumThreadsMax;
        break;
      // generic properties that 7-Zip passes to every coder: not used
      case NCoderPropID::kLevel:
      case NCoderPropID::kAffinity:
      case NCoderPropID::kNumThreadGroups:
      case NCoderPropID::kReduceSize:
      case NCoderPropID::kExpectedDataSize:
        break;
      default:
        return E_INVALIDARG;
    }
  }
  _blockLog = blockLog;
  _numThreads = numThreads;
  return S_OK;
}

Z7_COM7F_IMF(CEncoder::WriteCoderProperties(ISequentialOutStream *outStream))
{
  const Byte prop = (Byte)_blockLog;
  return WriteStream(outStream, &prop, 1);
}

/* The calling thread reads blocks into free slots and writes the encoded blocks in
   order; the slots encode in parallel. The output does not depend on the number of
   threads. */
HRESULT CEncoder::Code2(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    ICompressProgressInfo *progress)
{
  const size_t blockSize = (size_t)1 << _blockLog;
  RINOK(_ring.Init(_numThreads, blockSize, FLOAT_PLANE_ENCODED_MAX(blockSize), true))
  const unsigned numSlots = _ring.NumSlots();
  bool inEnd = false;
  UInt64 inProcessed = 0, outProcessed = 0;
  unsigned next = 0, numInWork = 0;

  for (;;)
  {
    while (!inEnd && numInWork < numSlots)
    {
      const unsigned k = (next + numInWork) % numSlots;
      CSlot &s = _ring.Slot(k);
      size_t len = blockSize;
      const HRESULT hres = ReadStream(inStream, s.In, &len);
      if (hres != S_OK)
      {
        _ring.Stop();
        return hres;
      }
      if (len != blockSize)
        inEnd = true;
      if (len == 0)
        break;
      s.InLen = len;
      numInWork++;
      const HRESULT hres2 = _ring.Submit(k);
      if (hres2 != S_OK)
      {
        _ring.Stop();
        return hres2;
      }
    }
    if (numInWork == 0)
      return S_OK;
    CSlot &s = _ring.Slot(next);
    HRESULT hres = _ring.Wait(next);
    if (hres == S_OK)
      hres = WriteStream(outStream, s.Out, s.OutLen);
    if (hres == S_OK)
    {
      inProcessed += s.InLen;
      outProcessed += s.OutLen;
      if (progress)
        hres = progress->SetRatioInfo(&inProcessed, &outProcessed);
    }
    if (hres != S_OK)
    {
      _ring.Stop();
      return hres;
    }
    next = (next + 1) % numSlots;
    numInWork--;
  }
}

Z7_COM7F_IMF(CEncoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 * /* inSize */, const UInt64 * /* outSize */, ICompressProgressInfo *progress))
{
  const HRESULT res = Code2(inStream, outStream, progress);
  _ring.Stop();
  return res;
}

#endif

}}
