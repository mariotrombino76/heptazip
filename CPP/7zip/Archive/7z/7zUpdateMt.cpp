// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
// 7zUpdateMt.cpp
// Tensor7: see 7zUpdateMt.h and docs/design/0003-compressione-parallela-folder-7z.md

#include "StdAfx.h"

#include <string.h>

#ifdef Z7_ZSTD_LIB_ENCODER
#include "../../Compress/ZstdLibEncoder.h"
#endif

#include "7zHeader.h"
#include "7zUpdateMt.h"

namespace NArchive {
namespace N7z {

// ---------------------------------------------------------------------------
// Cost of the compression of one folder
// ---------------------------------------------------------------------------

static const UInt32 kFilterMemory = (UInt32)1 << 20;     // buffers of a filter coder, AES
static const UInt32 kBZip2ThreadMemory = (UInt32)14 << 20; // CThreadInfo::Alloc(): sized for 900 KB blocks
static const UInt32 kDeflateMemory = (UInt32)8 << 20;    // match finder, match arrays, tables
static const UInt32 kPpmdExtraMemory = (UInt32)1 << 20;  // range coder, stream buffers

static UInt64 DivRoundUp(UInt64 a, UInt64 b)
{
  return a / b + (a % b != 0 ? 1 : 0);
}

/* Sets the dictionary size in (props), replacing the property if it exists. */
static void SetDictSize(CMethodProps &props, UInt64 dictSize)
{
  const int i = props.FindProp(NCoderPropID::kDictionarySize);
  if (i >= 0)
    props.Props[(unsigned)i].Value = (UInt32)dictSize;
  else
    props.AddProp32(NCoderPropID::kDictionarySize, (UInt32)dictSize);
}


/* LZMA and LZMA2: as LzmaEncProps_Normalize() / Lzma2EncProps_Normalize(). */
static bool Estimate_Lzma(const CMethodFull &m, bool isLzma2,
    UInt64 reduceSize, UInt64 folderSize, CFolderEncodeCost &cost)
{
  UInt64 reduceForDict = reduceSize;
  const UInt64 explicitBlockSize = isLzma2 ? m.GetProp_BlockSize(NCoderPropID::kBlockSize) : 0;
  if (explicitBlockSize != 0 && explicitBlockSize < reduceForDict)
    reduceForDict = explicitBlockSize;

  UInt64 dictSize = m.Get_Lzma_DicSize();
  {
    // LzmaEncProps_Normalize(): the dictionary is reduced to the size of the data
    UInt64 v = reduceForDict;
    const UInt32 kReduceMin = (UInt32)1 << 12;
    if (v < kReduceMin)
      v = kReduceMin;
    if (dictSize > v)
      dictSize = v;
  }

  CMethodProps props = m;
  SetDictSize(props, dictSize);

  // threads of one LZMA encoder: 2 with the multithreaded (bt) match finder
  UInt32 lzmaThreads = props.Get_Lzma_NumThreads();
  if (!props.Get_Lzma_MatchFinder_IsBt())
    lzmaThreads = 1;

  if (!isLzma2)
  {
    cost.NumThreads = lzmaThreads;
    cost.EncoderMemory = props.Get_Lzma_MemUsage(true);
    return true;
  }

  // LZMA2: kNumThreads is the total of threads (numTotalThreads)
  const int numThreadsProp = m.Get_NumThreads();
  const UInt32 numTotal = numThreadsProp > 0 ? (UInt32)numThreadsProp : 1;
  UInt32 blockThreads = numTotal / lzmaThreads;
  if (blockThreads == 0)
  {
    lzmaThreads = 1;
    blockThreads = numTotal;
  }

  const UInt64 blockSize = props.Get_Xz_BlockSize();

  /* Lzma2EncProps_Normalize() reduces the block threads with the size of the
     data of the whole operation (reduceSize), not of the folder: with more than
     one, the folder goes through MtCoder (a buffer of blockSize per block). */
  UInt64 blockThreadsReduced = blockThreads;
  if (reduceSize != (UInt64)(Int64)-1)
  {
    const UInt64 numBlocks = DivRoundUp(reduceSize, blockSize);
    if (blockThreadsReduced > numBlocks)
      blockThreadsReduced = numBlocks;
  }

  if (blockThreads <= 1 || blockThreadsReduced <= 1)
  {
    // one stream (LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID or one block)
    cost.NumThreads = lzmaThreads;
    cost.EncoderMemory = props.Get_Lzma_MemUsage(true);
    return true;
  }

  // MtCoder: the blocks of this folder
  UInt64 used = DivRoundUp(folderSize, blockSize);
  if (used == 0)
    used = 1;
  if (used > blockThreadsReduced)
    used = blockThreadsReduced;

  // as in CHandler::SetMainMethod() (7zHandlerOut.cpp) for the memory limit
  UInt64 size = used * (props.Get_Lzma_MemUsage(false) + blockSize);
  UInt64 numPackChunks = used + (used / 8) + 1;
  if (blockSize < ((UInt32)1 << 26)) numPackChunks++;
  if (blockSize < ((UInt32)1 << 24)) numPackChunks++;
  if (blockSize < ((UInt32)1 << 22)) numPackChunks++;
  size += numPackChunks * blockSize;

  cost.NumThreads = (UInt32)(lzmaThreads * used);
  cost.EncoderMemory = size;
  return true;
}


static bool Estimate_Ppmd(const CMethodFull &m, UInt64 reduceSize, CFolderEncodeCost &cost)
{
  /* The model is reduced to the size of the data, with the same rule as
     NCompress::NPpmd::CEncProps::Normalize() (PpmdEncoder.cpp). It's not called:
     7zUpdate must not depend on the PPMd encoder (builds without it). */
  UInt64 memSize = m.Get_Ppmd_MemSize();
  if (reduceSize < (UInt32)(Int32)-1)
  {
    const unsigned kMult = 16;
    if (memSize / kMult > reduceSize)
    {
      for (unsigned i = 16; i < 32; i++)
      {
        const UInt32 mem = (UInt32)1 << i;
        if (reduceSize <= mem / kMult)
        {
          if (memSize > mem)
            memSize = mem;
          break;
        }
      }
    }
  }
  cost.NumThreads = 1;
  cost.EncoderMemory = memSize + kPpmdExtraMemory;
  return true;
}


static bool Estimate_BZip2(const CMethodFull &m, UInt64 folderSize, CFolderEncodeCost &cost)
{
  bool fixedNumber;
  const UInt32 numThreads = m.Get_BZip2_NumThreads(fixedNumber);
  UInt64 used = DivRoundUp(folderSize, m.Get_BZip2_BlockSize());
  if (used == 0)
    used = 1;
  if (used > numThreads)
    used = numThreads;
  cost.NumThreads = (UInt32)used;
  cost.EncoderMemory = used * kBZip2ThreadMemory;
  return true;
}


#ifdef Z7_ZSTD_LIB_ENCODER

static bool Estimate_Zstd(const CMethodFull &m, UInt64 folderSize, CFolderEncodeCost &cost)
{
  int levelProp = -1;
  {
    const int i = m.FindProp(NCoderPropID::kLevel);
    if (i >= 0)
    {
      const NWindows::NCOM::CPropVariant &v = m.Props[(unsigned)i].Value;
      if (v.vt != VT_UI4)
        return false;
      levelProp = (int)v.ulVal;
    }
  }
  UInt64 windowSize = 0;
  if (!m.Get_DicSize(windowSize))
    windowSize = 0;

  NCompress::NZstdLib::CEncoderEstimate e;
  NCompress::NZstdLib::EstimateEncoder(levelProp, windowSize, e);

  // NZstdLib::CEncoder: zstd workers only with more than 1 thread
  const int numThreadsProp = m.Get_NumThreads();
  const UInt32 numThreads = numThreadsProp > 0 ? (UInt32)numThreadsProp : 1;
  if (numThreads <= 1)
  {
    cost.NumThreads = 1;
    cost.EncoderMemory = e.CCtxMemory;
    return true;
  }
  UInt64 used = DivRoundUp(folderSize, e.JobSize);
  if (used == 0)
    used = 1;
  if (used > numThreads)
    used = numThreads;
  cost.NumThreads = (UInt32)used;
  // per busy worker: a context and the job buffers (input, output)
  cost.EncoderMemory = used * (e.CCtxMemory + 2 * e.JobSize);
  return true;
}

#endif


bool EstimateFolderEncodeCost(const CCompressionMethodMode &mode,
    UInt64 reduceSize, UInt64 folderSize, CFolderEncodeCost &cost)
{
  cost.NumThreads = 1;
  cost.EncoderMemory = 0;
  if (mode.Methods.IsEmpty())
    return false;
  bool thereIsMainCoder = false;
  FOR_VECTOR (i, mode.Methods)
  {
    const CMethodFull &m = mode.Methods[i];
    if (!m.IsSimpleCoder())
      return false;  // BCJ2 and other coders with several streams
    CFolderEncodeCost c;
    c.NumThreads = 0;
    c.EncoderMemory = kFilterMemory;
    bool isMainCoder = true;
    switch (m.Id)
    {
      case k_LZMA:  if (!Estimate_Lzma(m, false, reduceSize, folderSize, c)) return false; break;
      case k_LZMA2: if (!Estimate_Lzma(m, true, reduceSize, folderSize, c)) return false; break;
      case k_PPMD:  if (!Estimate_Ppmd(m, reduceSize, c)) return false; break;
      case k_BZip2: if (!Estimate_BZip2(m, folderSize, c)) return false; break;
      case k_Deflate:
      case k_Deflate64:
        c.NumThreads = 1;
        c.EncoderMemory = kDeflateMemory;
        break;
     #ifdef Z7_ZSTD_LIB_ENCODER
      case k_ZSTD_7zZstd: if (!Estimate_Zstd(m, folderSize, c)) return false; break;
     #endif
      case k_FLOAT:
      {
        // one thread; input and output buffer of one block (FloatCoder.cpp)
        UInt64 blockSize = (UInt64)1 << 20;
        const int i = m.FindProp(NCoderPropID::kBlockSize);
        if (i >= 0)
        {
          const NWindows::NCOM::CPropVariant &v = m.Props[(unsigned)i].Value;
          if (v.vt == VT_UI4)
            blockSize = v.ulVal;
          else if (v.vt == VT_UI8)
            blockSize = v.uhVal.QuadPart;
          else
            return false;
          if (blockSize > ((UInt64)1 << 22))
            return false;
        }
        // NFloat::CEncoder: with N > 1 threads, 2 * N slots (FloatCoder.h), each with
        // an input and an output buffer of one block; 1 slot with 1 thread
        const int numThreadsProp = m.Get_NumThreads();
        UInt32 numThreads = numThreadsProp > 0 ? (UInt32)numThreadsProp : 1;
        if (numThreads > 64)
          numThreads = 64;
        const UInt64 numSlots = numThreads == 1 ? 1 : 2 * (UInt64)numThreads;
        c.NumThreads = numThreads;
        c.EncoderMemory = numSlots * (2 * blockSize + 16);
        break;
      }
      default:
        // IsFilterMethod() includes BCJ2, rejected above (4 streams)
        if (!IsFilterMethod(m.Id))
          return false;  // Copy (nothing to gain), unknown and external methods
        isMainCoder = false;
    }
    if (isMainCoder)
      thereIsMainCoder = true;
    if (cost.NumThreads < c.NumThreads)
      cost.NumThreads = c.NumThreads;
    cost.EncoderMemory += c.EncoderMemory;
  }
  if (!thereIsMainCoder)
    return false;  // only filters: inexpensive
  if (mode.PasswordIsDefined)
    cost.EncoderMemory += kFilterMemory;  // AES coder, added by CEncoder
  return true;
}


// ---------------------------------------------------------------------------
// CFolderReplayInStream
// ---------------------------------------------------------------------------

Z7_COM7F_IMF(CFolderReplayInStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (size == 0)
    return S_OK;

  // files that were read, and files without data (empty, or not opened)
  while (_fileRem == 0 && _fileIndex < _numFiles)
    _fileRem = _fileSizes[_fileIndex++];

  const size_t avail = _size - _pos;
  if (avail == 0)
  {
    if (_fileRem != 0)
      return E_FAIL;  // the sizes of the files don't match the data
    if (_rest)
      return _rest->Read(data, size, processedSize);
    return S_OK;
  }

  UInt32 cur = size;
  if (cur > kMaxReadSize)
    cur = kMaxReadSize;
  // (_fileRem == 0) here: data after the last completed file (a file being read)
  if (_fileRem != 0 && cur > _fileRem)
    cur = (UInt32)_fileRem;
  if (cur > avail)
    cur = (UInt32)avail;

  memcpy(data, _data + _pos, cur);
  _pos += cur;
  if (_fileRem != 0)
    _fileRem -= cur;
  if (processedSize)
    *processedSize = cur;
  return S_OK;
}

}}
