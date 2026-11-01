// FloatCoder.cpp
// 7zip-modern: "FLOAT" method of 7z (design 0008).

#include "StdAfx.h"

#include <string.h>

#include "../../../C/FloatPlane.h"

#include "../Common/StreamUtils.h"

#include "FloatCoder.h"

namespace NCompress {
namespace NFloat {

static HRESULT SResToHRESULT_Float(SRes res)
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


/* The input buffer holds the largest encoded block (stored: block + 5 bytes); after
   each block the unused bytes move to the start and the buffer is filled again. */
Z7_COM7F_IMF(CDecoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 * /* inSize */, const UInt64 *outSize, ICompressProgressInfo *progress))
{
  const size_t blockSize = (size_t)1 << _blockLog;
  const size_t inCap = FLOAT_PLANE_ENCODED_MAX(blockSize);
  _inBuf.AllocAtLeast(inCap);
  _outBuf.AllocAtLeast(blockSize);

  size_t inPos = 0, inLim = 0;
  bool inEnd = false;
  UInt64 inProcessed = 0, outProcessed = 0;

  for (;;)
  {
    if (outSize && outProcessed == *outSize)
      return S_OK;

    if (inPos == inLim && inEnd)
      return outSize ? S_FALSE : S_OK;   // without outSize the stream ends after a block

    size_t used = 0;
    SRes res = SZ_ERROR_INPUT_EOF;
    if (inLim != inPos)
      res = FloatPlane_DecodeBlock(_inBuf + inPos, inLim - inPos, _outBuf, blockSize, &used);

    if (res == SZ_ERROR_INPUT_EOF)
    {
      if (inEnd || (inPos == 0 && inLim == inCap))
        return S_FALSE;   // truncated, or a block larger than the props allow
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
    if (res != SZ_OK)
      return SResToHRESULT_Float(res);

    size_t outLen = 0;
    FloatPlane_GetBlockOutLen(_inBuf + inPos, inLim - inPos, &outLen);
    if (outSize && outLen > *outSize - outProcessed)
      return S_FALSE;
    RINOK(WriteStream(outStream, _outBuf, outLen))
    inPos += used;
    inProcessed += used;
    outProcessed += outLen;
    if (progress)
    {
      RINOK(progress->SetRatioInfo(&inProcessed, &outProcessed))
    }
  }
}


#ifndef Z7_EXTRACT_ONLY

Z7_COM7F_IMF(CEncoder::SetCoderProperties(const PROPID *propIDs, const PROPVARIANT *coderProps, UInt32 numProps))
{
  unsigned blockLog = kBlockLogDefault;
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
      // generic properties that 7-Zip passes to every coder: not used
      case NCoderPropID::kLevel:
      case NCoderPropID::kNumThreads:
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
  return S_OK;
}


Z7_COM7F_IMF(CEncoder::WriteCoderProperties(ISequentialOutStream *outStream))
{
  const Byte prop = (Byte)_blockLog;
  return WriteStream(outStream, &prop, 1);
}


Z7_COM7F_IMF(CEncoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 * /* inSize */, const UInt64 * /* outSize */, ICompressProgressInfo *progress))
{
  const size_t blockSize = (size_t)1 << _blockLog;
  _inBuf.AllocAtLeast(blockSize);
  _outBuf.AllocAtLeast(FLOAT_PLANE_ENCODED_MAX(blockSize));
  UInt64 inProcessed = 0, outProcessed = 0;

  for (;;)
  {
    size_t len = blockSize;
    RINOK(ReadStream(inStream, _inBuf, &len))
    if (len == 0)
      return S_OK;
    const size_t encLen = FloatPlane_EncodeBlock(_inBuf, len, _outBuf);
    RINOK(WriteStream(outStream, _outBuf, encLen))
    inProcessed += len;
    outProcessed += encLen;
    if (progress)
    {
      RINOK(progress->SetRatioInfo(&inProcessed, &outProcessed))
    }
    if (len != blockSize)
      return S_OK;
  }
}

#endif

}}
