// 7zExtractMt.cpp
// 7zip-modern: building blocks for parallel extraction of 7z folders.
// See docs/design/0001-estrazione-parallela-folder-7z.md

#include "StdAfx.h"

#include <string.h>

#include "../../../../C/CpuArch.h"

#include "7zExtractMt.h"
#include "7zHeader.h"

namespace NArchive {
namespace N7z {

// ---------------------------------------------------------------------------
// Memory estimation
// ---------------------------------------------------------------------------

/* Fixed overhead for each coder: input/output buffers of the decoder wrappers
   (LzmaDecoder / Lzma2Decoder use 1 MiB input buffer and 1 MiB output step),
   stream binders of the mixer, and objects. */
static const UInt64 kCoderOverhead = (UInt64)2 << 20;

static const UInt64 kMem_BZip2   = (UInt64)16 << 20; // ~7.5 MiB per block, rounded up
static const UInt64 kMem_Deflate = (UInt64)1 << 20;  // 64 KiB window + Huffman tables
static const UInt64 kMem_BCJ2    = (UInt64)8 << 20;  // input buffers of 4 streams

/* Zstandard, 7z method of 7-Zip-zstd (registered in ZstdCodecRegister.cpp).
   The 7-Zip decoder (C/ZstdDec.c) allocates MIN(content size, window size) of the
   frame. The properties don't contain the window: we use the largest window of
   the standard levels (2^27, level 22). Frames with bigger windows (long mode,
   custom window) and without content size can use more memory than estimated.
   The 7-Zip-modern encoder stores the content size only for folders up to
   128 KiB (compressed in one call); bigger frames use the window of the level,
   at most 2^27. */
static const UInt64 kMem_Zstd = (UInt64)1 << 27;

// LZMA_LIT_SIZE and LZMA probability model size (C/LzmaDec.c), with 16-bit probs
static UInt64 GetLzmaProbsMemory(unsigned lc, unsigned lp)
{
  return ((UInt64)1846 + ((UInt64)0x300 << (lc + lp))) * 2;
}

bool EstimateCoderDecodeMemory(CMethodId methodId, const Byte *props, size_t propsSize, UInt64 &memUsage)
{
  memUsage = 0;
  UInt64 mem = 0;

  if (methodId == k_FLOAT)
  {
    // input buffer (largest encoded block) + output buffer, see FloatCoder.cpp
    if (propsSize != 1 || props[0] < 16 || props[0] > 22)
      return false;
    memUsage = ((UInt64)2 << props[0]) + 16 + kCoderOverhead;
    return true;
  }
  if (methodId > (UInt32)0xFFFFFFFF)
    return false;

  switch ((UInt32)methodId)
  {
    case k_LZMA:
    {
      if (propsSize < 5)
        return false;
      unsigned d = props[0];
      if (d >= 9 * 5 * 5)
        return false;
      const unsigned lc = d % 9;
      d /= 9;
      const unsigned lp = d % 5;
      UInt32 dictSize = GetUi32(props + 1);
      if (dictSize < (1 << 12))
        dictSize = (1 << 12);
      mem = (UInt64)dictSize + GetLzmaProbsMemory(lc, lp);
      break;
    }

    case k_LZMA2:
    {
      if (propsSize < 1)
        return false;
      const unsigned p = props[0];
      if (p > 40)
        return false;
      const UInt64 dictSize = (p == 40) ?
          (UInt64)0xFFFFFFFF :
          (UInt64)(((UInt32)2 | (p & 1)) << (p / 2 + 11));
      // LZMA2 limits (lc + lp <= 4)
      mem = dictSize + GetLzmaProbsMemory(4, 0);
      break;
    }

    case k_PPMD:
    {
      if (propsSize < 5)
        return false;
      mem = GetUi32(props + 1);
      break;
    }

    case k_BZip2:
      mem = kMem_BZip2;
      break;

    case k_Deflate:
    case k_Deflate64:
      mem = kMem_Deflate;
      break;

    case k_BCJ2:
      mem = kMem_BCJ2;
      break;

    case k_ZSTD_7zZstd:
      mem = kMem_Zstd;
      break;

    case k_Copy:
    case k_AES:
      break;

    default:
      if (!IsFilterMethod(methodId))
        return false;
      break;
  }

  memUsage = mem + kCoderOverhead;
  return true;
}


bool EstimateFolderDecodeMemory(const CFolder &folder, UInt64 &memUsage)
{
  memUsage = 0;
  UInt64 sum = 0;
  for (unsigned i = 0; i < folder.Coders.Size(); i++)
  {
    const CCoderInfo &coder = folder.Coders[i];
    UInt64 mem;
    if (!EstimateCoderDecodeMemory(coder.MethodID, coder.Props, coder.Props.Size(), mem))
      return false;
    sum += mem;
  }
  memUsage = sum;
  return true;
}


// ---------------------------------------------------------------------------
// CFolderBufOutStream
// ---------------------------------------------------------------------------

Z7_COM7F_IMF(CFolderBufOutStream::Write(const void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (size == 0)
    return S_OK;

  const size_t rem = _size - _pos;
  const size_t cur = (size <= rem) ? (size_t)size : rem;
  if (cur != 0)
  {
    memcpy(_buf + _pos, data, cur);
    _pos += cur;
  }
  if (processedSize)
    *processedSize = (UInt32)cur;
  if (cur != size)
  {
    // same as CFolderOutStream::Write(), when there are no more requested files
    WritingWasCut = true;
    return k_My_HRESULT_WritingWasCut;
  }
  return S_OK;
}


// ---------------------------------------------------------------------------
// CBufInStreamAt
// ---------------------------------------------------------------------------

Z7_COM7F_IMF(CBufInStreamAt::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (size == 0)
    return S_OK;
  if (_pos < _base)
    return E_FAIL; // the decoder reads only inside the packed streams of the folder
  const UInt64 offset = _pos - _base;
  if (offset >= _size)
    return _endRes;
  size_t rem = _size - (size_t)offset;
  if (rem > size)
    rem = (size_t)size;
  memcpy(data, _data + (size_t)offset, rem);
  _pos += rem;
  if (processedSize)
    *processedSize = (UInt32)rem;
  return S_OK;
}

Z7_COM7F_IMF(CBufInStreamAt::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: offset += (Int64)_pos; break;
    // the real end of the archive stream is unknown here: we use the end of the data
    case STREAM_SEEK_END: offset += (Int64)(_base + _size); break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (offset < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  _pos = (UInt64)offset;
  if (newPosition)
    *newPosition = (UInt64)offset;
  return S_OK;
}


#ifndef Z7_NO_CRYPTO

// ---------------------------------------------------------------------------
// CCachedPassword
// ---------------------------------------------------------------------------

HRESULT CCachedPassword::Fetch(ICryptoGetTextPassword *callback)
{
  _fetched = true;
  _password.Wipe_and_Empty();
  if (!callback)
  {
    // 7zDecode.cpp returns E_NOTIMPL for encrypted folders without password callback
    _res = E_NOTIMPL;
    return _res;
  }
  CMyComBSTR_Wipe bstr;
  _res = callback->CryptoGetTextPassword(&bstr);
  if (_res == S_OK && bstr)
    static_cast<UString &>(_password) = (LPCOLESTR)bstr; // as in 7zDecode.cpp
  return _res;
}

Z7_COM7F_IMF(CCachedPassword::CryptoGetTextPassword(BSTR *password))
{
  *password = NULL;
  if (!_fetched)
    return E_FAIL; // programming error: Fetch() must be called first
  if (_res != S_OK)
    return _res;
  return StringToBstr(_password, password);
}

#endif

}}
