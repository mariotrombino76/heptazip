// DeflateEncoder.cpp

#include "StdAfx.h"

#include "../../../C/Alloc.h"
#include "../../../C/HuffEnc.h"

#include "../../Common/ComTry.h"

#include "../Common/CWrappers.h"
#include "../Common/StreamObjects.h"
#include "../Common/StreamUtils.h"
#ifdef Z7_DEFLATE_LIBDEFLATE
#include "../../../third_party/libdeflate/libdeflate.h"
#endif
#ifndef Z7_ST
#include "../Common/MtTaskPool.h"
#endif

#include "DeflateEncoder.h"

#undef NO_INLINE

#ifdef _MSC_VER
#define NO_INLINE Z7_NO_INLINE
#else
#define NO_INLINE
#endif

#define MAX_HUF_LEN_12  12

namespace NCompress {
namespace NDeflate {
namespace NEncoder {

static const unsigned k_CodeValue_Len_Is_Literal_Flag = 1u << 15;

static const unsigned kNumDivPassesMax = 10; // [0, 16); ratio/speed/ram tradeoff; use big value for better compression ratio.
static const unsigned kNumTables = 1u << kNumDivPassesMax;

static const UInt32 kFixedHuffmanCodeBlockSizeMax = (1 << 8); // [0, (1 << 32)); ratio/speed tradeoff; use big value for better compression ratio.
static const UInt32 kDivideCodeBlockSizeMin = (1 << 7); // [1, (1 << 32)); ratio/speed tradeoff; use small value for better compression ratio.
static const UInt32 kDivideBlockSizeMin = (1 << 6); // [1, (1 << 32)); ratio/speed tradeoff; use small value for better compression ratio.

static const UInt32 kMaxUncompressedBlockSize = ((1 << 16) - 1) * 1; // [1, (1 << 32))
static const UInt32 kMatchArraySize = kMaxUncompressedBlockSize * 10; // [kMatchMaxLen * 2, (1 << 32))
static const UInt32 kMatchArrayLimit = kMatchArraySize - kMatchMaxLen * 4 * sizeof(UInt16);
static const UInt32 kBlockUncompressedSizeThreshold = kMaxUncompressedBlockSize -
    kMatchMaxLen - kNumOpts;

// static const unsigned kMaxCodeBitLength = 11;
static const unsigned kMaxLevelBitLength = 7;

static const Byte kNoLiteralStatPrice = 11;
static const Byte kNoLenStatPrice = 11;
static const Byte kNoPosStatPrice = 6;

MY_ALIGN(64)
static Byte g_LenSlots[kNumLenSymbolsMax];

#define kNumLogBits 9    // do not change it
MY_ALIGN(64)
static Byte g_FastPos[1 << kNumLogBits];

class CFastPosInit
{
public:
  CFastPosInit()
  {
    unsigned i;
    for (i = 0; i < kNumLenSlots; i++)
    {
      unsigned c = kLenStart32[i];
      const unsigned j = 1u << kLenDirectBits32[i];
      for (unsigned k = 0; k < j; k++, c++)
        g_LenSlots[c] = (Byte)i;
    }
    
    const unsigned kFastSlots = kNumLogBits * 2;
    unsigned c = 0;
    for (Byte slotFast = 0; slotFast < kFastSlots; slotFast++)
    {
      const unsigned k = 1u << kDistDirectBits[slotFast];
      for (unsigned j = 0; j < k; j++, c++)
        g_FastPos[c] = slotFast;
    }
  }
};

static CFastPosInit g_FastPosInit;

inline unsigned GetPosSlot(UInt32 pos)
{
  /*
  if (pos < 0x200)
    return g_FastPos[pos];
  return g_FastPos[pos >> 8] + 16;
  */
  // const unsigned zz = (pos < ((UInt32)1 << (kNumLogBits))) ? 0 : 8;
  /*
  const unsigned zz = (kNumLogBits - 1) &
      ((UInt32)0 - (((((UInt32)1 << kNumLogBits) - 1) - pos) >> 31));
  */
  const unsigned zz = (kNumLogBits - 1) &
      (((((UInt32)1 << kNumLogBits) - 1) - pos) >> (31 - 3));
  return g_FastPos[pos >> zz] + (zz * 2);
}


void CEncProps::Normalize()
{
  int level = Level;
  if (level < 0) level = 5;
  Level = level;
  if (algo < 0) algo = (level < 5 ? 0 : 1);
  /* Heptazip: -mx1..-mx4 were the same profile (fb 32, mc 32, greedy parsing). The fast
     mode now does lazy matching, with a longer search at each level (docs/design/0010;
     silesia, 20 threads: -mx1 is as fast as before with a better ratio, -mx3 is faster
     than pigz -6 with a better ratio, -mx4 has the ratio of pigz -9 at 1.8 times its
     speed). With libdeflate (design 0012) this coder is used only for Deflate64 and for
     explicit properties. */
  if (algo == 0 && level < 5)
  {
    static const Byte kFastFb[5] = { 32, 32, 32, 64, 128 };
    static const Byte kFastMc[5] = { 10, 10, 24, 48, 128 };
    if (fb < 0) fb = kFastFb[level];
    if (mc == 0) mc = kFastMc[level];
  }
  if (fb < 0) fb = (level < 7 ? 32 : (level < 9 ? 64 : 128));
  if (btMode < 0) btMode = (algo == 0 ? 0 : 1);
  if (mc == 0) mc = (16 + ((unsigned)fb >> 1));
  if (numPasses == (UInt32)(Int32)-1) numPasses = (level < 7 ? 1 : (level < 9 ? 3 : 10));
}

void CCoder::SetProps(const CEncProps *props2)
{
  CEncProps props = *props2;
  props.Normalize();

  m_MatchFinderCycles = props.mc;
  {
    unsigned fb = (unsigned)props.fb;
    if (fb < kMatchMinLen)
      fb = kMatchMinLen;
    if (fb > m_MatchMaxLen)
      fb = m_MatchMaxLen;
    m_NumFastBytes = fb;
  }
  _fastMode = (props.algo == 0);
  _btMode = (props.btMode != 0);

  m_NumDivPasses = props.numPasses;
  if (m_NumDivPasses == 0)
    m_NumDivPasses = 1;
  if (m_NumDivPasses == 1)
    m_NumPasses = 1;
  else if (m_NumDivPasses <= kNumDivPassesMax)
    m_NumPasses = 2;
  else
  {
    m_NumPasses = 2 + (m_NumDivPasses - kNumDivPassesMax);
    m_NumDivPasses = kNumDivPassesMax;
  }
}

/* Heptazip: the chunks are coded by libdeflate (design 0012), at the level with a better
   ratio than the coder of 7-Zip at the same -mx (silesia, 1 MiB chunks: -mx1 32.78%
   instead of 32.98%, -mx3 31.85% / 32.14%, -mx5 30.61% / 30.99%, -mx9 30.53% / 30.56%),
   with less CPU time (2 to 5 times less; -mx5 about the same). The coder of 7-Zip codes
   Deflate64, and the properties set explicitly (-mfb, -mmc, -mpass, -ma). */
static int GetLibLevel(const CEncProps &p, bool deflate64Mode)
{
  #ifdef Z7_DEFLATE_LIBDEFLATE
  if (deflate64Mode || p.algo >= 0 || p.fb >= 0 || p.btMode >= 0 || p.mc != 0
      || p.numPasses != (UInt32)(Int32)-1)
    return 0;
  static const Byte kLibLevels[10] = { 4, 4, 5, 6, 7, 10, 10, 11, 11, 12 };
  int level = p.Level;
  if (level < 0)
    level = 5;
  if (level > 9)
    level = 9;
  return kLibLevels[level];
  #else
  UNUSED_VAR(p)
  UNUSED_VAR(deflate64Mode)
  return 0;
  #endif
}

CCoder::CCoder(bool deflate64Mode):
  m_Values(NULL),
  m_OnePosMatchesMemory(NULL),
  m_DistanceMemory(NULL),
  m_Created(false),
  m_Deflate64Mode(deflate64Mode),
  m_Tables(NULL),
  _numThreads(1),
  _chunkEncoder(NULL),
  _libLevel(0)
{
  m_MatchMaxLen = deflate64Mode ? kMatchMaxLen64 : kMatchMaxLen32;
  m_NumLenCombinations = deflate64Mode ? kNumLenSymbols64 : kNumLenSymbols32;
  m_LenStart = deflate64Mode ? kLenStart64 : kLenStart32;
  m_LenDirectBits = deflate64Mode ? kLenDirectBits64 : kLenDirectBits32;
  {
    CEncProps props;
    SetProps(&props);
    _libLevel = GetLibLevel(props, deflate64Mode);
  }
  MatchFinder_Construct(&_lzInWindow);
}

HRESULT CCoder::Create()
{
  // COM_TRY_BEGIN
  if (!m_Values)
  {
    m_Values = (CCodeValue *)MyAlloc(kMaxUncompressedBlockSize * sizeof(CCodeValue));
    if (!m_Values)
      return E_OUTOFMEMORY;
  }
  if (!m_Tables)
  {
    m_Tables = (CTables *)MyAlloc(kNumTables * sizeof(CTables));
    if (!m_Tables)
      return E_OUTOFMEMORY;
  }

  if (m_IsMultiPass)
  {
    if (!m_OnePosMatchesMemory)
    {
      m_OnePosMatchesMemory = (UInt16 *)::MidAlloc(kMatchArraySize * sizeof(UInt16));
      if (!m_OnePosMatchesMemory)
        return E_OUTOFMEMORY;
    }
  }
  else
  {
    if (!m_DistanceMemory)
    {
      m_DistanceMemory = (UInt16 *)MyAlloc((kMatchMaxLen + 2) * 2 * sizeof(UInt16));
      if (!m_DistanceMemory)
        return E_OUTOFMEMORY;
      m_MatchDistances = m_DistanceMemory;
    }
  }

  if (!m_Created)
  {
    _lzInWindow.btMode = (Byte)(_btMode ? 1 : 0);
    _lzInWindow.numHashBytes = 3;
    _lzInWindow.numHashBytes_Min = 3;
    if (!MatchFinder_Create(&_lzInWindow,
        m_Deflate64Mode ? kHistorySize64 : kHistorySize32,
        kNumOpts + kMaxUncompressedBlockSize,
        m_NumFastBytes, m_MatchMaxLen - m_NumFastBytes, &g_AlignedAlloc))
      return E_OUTOFMEMORY;
    if (!m_OutStream.Create(1 << 20))
      return E_OUTOFMEMORY;
  }
  if (m_MatchFinderCycles != 0)
    _lzInWindow.cutValue = m_MatchFinderCycles;
  m_Created = true;
  return S_OK;
  // COM_TRY_END
}

HRESULT CCoder::BaseSetEncoderProperties2(const PROPID *propIDs, const PROPVARIANT *coderProps, UInt32 numProps)
{
  CEncProps props;
  UInt32 numThreads = 1;
  for (UInt32 i = 0; i < numProps; i++)
  {
    const PROPVARIANT &prop = coderProps[i];
    PROPID propID = propIDs[i];
    if (propID >= NCoderPropID::kReduceSize)
      continue;
    if (prop.vt != VT_UI4)
      return E_INVALIDARG;
    UInt32 v = (UInt32)prop.ulVal;
    switch (propID)
    {
      case NCoderPropID::kNumPasses: props.numPasses = v; break;
      case NCoderPropID::kNumFastBytes: props.fb = (int)v; break;
      case NCoderPropID::kMatchFinderCycles: props.mc = v; break;
      case NCoderPropID::kAlgorithm: props.algo = (int)v; break;
      case NCoderPropID::kLevel: props.Level = (int)v; break;
      case NCoderPropID::kNumThreads: numThreads = v; break;
      default: return E_INVALIDARG;
    }
  }
  SetProps(&props);
  _props = props;
  _libLevel = GetLibLevel(props, m_Deflate64Mode);
  _numThreads = numThreads;
  // the coders of the chunks are created again with the new properties
  DeleteChunkEncoder();
  return S_OK;
}
  
void CCoder::Free()
{
  ::MidFree(m_OnePosMatchesMemory); m_OnePosMatchesMemory = NULL;
  ::MyFree(m_DistanceMemory); m_DistanceMemory = NULL;
  ::MyFree(m_Values); m_Values = NULL;
  ::MyFree(m_Tables); m_Tables = NULL;
}

CCoder::~CCoder()
{
  DeleteChunkEncoder();
  Free();
  MatchFinder_Free(&_lzInWindow, &g_AlignedAlloc);
}

NO_INLINE void CCoder::GetMatches()
{
  if (m_IsMultiPass)
  {
    m_MatchDistances = m_OnePosMatchesMemory + m_Pos;
    if (m_SecondPass)
    {
      m_Pos += *m_MatchDistances + 1;
      return;
    }
  }

  UInt32 distanceTmp[kMatchMaxLen * 2 + 3];
  
  const size_t numPairs = (size_t)((_btMode ?
      Bt3Zip_MatchFinder_GetMatches(&_lzInWindow, distanceTmp):
      Hc3Zip_MatchFinder_GetMatches(&_lzInWindow, distanceTmp)) - distanceTmp);

  UInt16 *matchDistances = m_MatchDistances;
  *matchDistances++ = (UInt16)numPairs;
   
  if (numPairs != 0)
  {
    size_t i;
    for (i = 0; i < numPairs; i += 2)
    {
      matchDistances[0] = (UInt16)distanceTmp[i];
      matchDistances[1] = (UInt16)distanceTmp[(size_t)i + 1];
      matchDistances += 2;
    }
    UInt32 len = distanceTmp[(size_t)numPairs - 2];
    if (len == m_NumFastBytes && m_NumFastBytes != m_MatchMaxLen)
    {
      UInt32 numAvail = Inline_MatchFinder_GetNumAvailableBytes(&_lzInWindow) + 1;
      const Byte *pby = Inline_MatchFinder_GetPointerToCurrentPos(&_lzInWindow) - 1;
      const Byte *pby2 = pby - (distanceTmp[(size_t)numPairs - 1] + 1);
      if (numAvail > m_MatchMaxLen)
        numAvail = m_MatchMaxLen;
      for (; len < numAvail && pby[len] == pby2[len]; len++);
      matchDistances[-2] = (UInt16)len;
    }
  }
  if (m_IsMultiPass)
    m_Pos += (UInt32)numPairs + 1;
  if (!m_SecondPass)
    m_AdditionalOffset++;
}

void CCoder::MovePos(UInt32 num)
{
  if (!m_SecondPass && num > 0)
  {
    if (_btMode)
      Bt3Zip_MatchFinder_Skip(&_lzInWindow, num);
    else
      Hc3Zip_MatchFinder_Skip(&_lzInWindow, num);
    m_AdditionalOffset += num;
  }
}

static const UInt32 kIfinityPrice = 0xFFFFFFF;

static const unsigned kNumPassesFirstBlock = 4;  // Heptazip: see CodeStream()

NO_INLINE UInt32 CCoder::Backward(UInt32 &backRes, UInt32 cur)
{
  m_OptimumEndIndex = cur;
  UInt32 posMem = m_Optimum[cur].PosPrev;
  UInt16 backMem = m_Optimum[cur].BackPrev;
  do
  {
    UInt32 posPrev = posMem;
    UInt16 backCur = backMem;
    backMem = m_Optimum[posPrev].BackPrev;
    posMem = m_Optimum[posPrev].PosPrev;
    m_Optimum[posPrev].BackPrev = backCur;
    m_Optimum[posPrev].PosPrev = (UInt16)cur;
    cur = posPrev;
  }
  while (cur > 0);
  backRes = m_Optimum[0].BackPrev;
  m_OptimumCurrentIndex = m_Optimum[0].PosPrev;
  return m_OptimumCurrentIndex;
}

NO_INLINE UInt32 CCoder::GetOptimal(UInt32 &backRes)
{
  if (m_OptimumEndIndex != m_OptimumCurrentIndex)
  {
    UInt32 len = m_Optimum[m_OptimumCurrentIndex].PosPrev - m_OptimumCurrentIndex;
    backRes = m_Optimum[m_OptimumCurrentIndex].BackPrev;
    m_OptimumCurrentIndex = m_Optimum[m_OptimumCurrentIndex].PosPrev;
    return len;
  }
  m_OptimumCurrentIndex = m_OptimumEndIndex = 0;
  
  GetMatches();

  UInt32 lenEnd;
  {
    const UInt32 numDistancePairs = m_MatchDistances[0];
    if (numDistancePairs == 0)
      return 1;
    const UInt16 *matchDistances = m_MatchDistances + 1;
    lenEnd = matchDistances[(size_t)numDistancePairs - 2];
    
    if (lenEnd > m_NumFastBytes)
    {
      backRes = matchDistances[(size_t)numDistancePairs - 1];
      MovePos(lenEnd - 1);
      return lenEnd;
    }

    m_Optimum[1].Price = m_LiteralPrices[*(Inline_MatchFinder_GetPointerToCurrentPos(&_lzInWindow) - m_AdditionalOffset)];
    m_Optimum[1].PosPrev = 0;
    
    m_Optimum[2].Price = kIfinityPrice;
    m_Optimum[2].PosPrev = 1;
    
    UInt32 offs = 0;
  
    for (UInt32 i = kMatchMinLen; i <= lenEnd; i++)
    {
      UInt32 distance = matchDistances[(size_t)offs + 1];
      m_Optimum[i].PosPrev = 0;
      m_Optimum[i].BackPrev = (UInt16)distance;
      m_Optimum[i].Price = m_LenPrices[(size_t)i - kMatchMinLen] + m_PosPrices[GetPosSlot(distance)];
      if (i == matchDistances[offs])
        offs += 2;
    }
  }

  UInt32 cur = 0;

  for (;;)
  {
    ++cur;
    if (cur == lenEnd || cur == kNumOptsBase || m_Pos >= kMatchArrayLimit)
      return Backward(backRes, cur);
    GetMatches();
    const UInt16 *matchDistances = m_MatchDistances + 1;
    const UInt32 numDistancePairs = m_MatchDistances[0];
    UInt32 newLen = 0;
    if (numDistancePairs != 0)
    {
      newLen = matchDistances[(size_t)numDistancePairs - 2];
      if (newLen > m_NumFastBytes)
      {
        UInt32 len = Backward(backRes, cur);
        m_Optimum[cur].BackPrev = matchDistances[(size_t)numDistancePairs - 1];
        m_OptimumEndIndex = cur + newLen;
        m_Optimum[cur].PosPrev = (UInt16)m_OptimumEndIndex;
        MovePos(newLen - 1);
        return len;
      }
    }
    UInt32 curPrice = m_Optimum[cur].Price;
    {
      const UInt32 curAnd1Price = curPrice + m_LiteralPrices[*(Inline_MatchFinder_GetPointerToCurrentPos(&_lzInWindow) + cur - m_AdditionalOffset)];
      COptimal &optimum = m_Optimum[(size_t)cur + 1];
      if (curAnd1Price < optimum.Price)
      {
        optimum.Price = curAnd1Price;
        optimum.PosPrev = (UInt16)cur;
      }
    }
    if (numDistancePairs == 0)
      continue;
    while (lenEnd < cur + newLen)
      m_Optimum[++lenEnd].Price = kIfinityPrice;
    UInt32 offs = 0;
    UInt32 distance = matchDistances[(size_t)offs + 1];
    curPrice += m_PosPrices[GetPosSlot(distance)];
    for (UInt32 lenTest = kMatchMinLen; ; lenTest++)
    {
      UInt32 curAndLenPrice = curPrice + m_LenPrices[(size_t)lenTest - kMatchMinLen];
      COptimal &optimum = m_Optimum[cur + lenTest];
      if (curAndLenPrice < optimum.Price)
      {
        optimum.Price = curAndLenPrice;
        optimum.PosPrev = (UInt16)cur;
        optimum.BackPrev = (UInt16)distance;
      }
      if (lenTest == matchDistances[offs])
      {
        offs += 2;
        if (offs == numDistancePairs)
          break;
        curPrice -= m_PosPrices[GetPosSlot(distance)];
        distance = matchDistances[(size_t)offs + 1];
        curPrice += m_PosPrices[GetPosSlot(distance)];
      }
    }
  }
}

/* Heptazip: the fast mode does lazy matching (docs/design/0010), as zlib's deflate_slow():
   the match at the current position is coded only if the next position has no better
   match; else the current byte is a literal, and the match of the next position is
   compared in the same way with the following one. "Better" is decided as in zstd's
   lazy parser, by the gain of a match: 4 per byte of length minus the extra bits of the
   distance; the next match must have kNextBias more.
   Also from zlib: a match of 3 bytes at a distance over 4096 is not used (it costs more
   than 3 literals), and after a match of kGoodLen bytes the next position is searched
   with a quarter of the chain.
   The match of the next position is pending while (m_OptimumEndIndex !=
   m_OptimumCurrentIndex): the fast mode does not use the optimal parser, and TryBlock()
   does not end a block before the pending match. */
static const UInt32 kTooFar = 4096;     // (backRes >= kTooFar): distance > 4096
static const UInt32 kGoodLen = 8;
static const Int32 kNextBias = 2;
static const Int32 kShorterBias = 2;    // a shorter match at the same position

static inline Int32 MatchGain(UInt32 len, UInt32 backRes)
{
  return (Int32)(len * 4) - (Int32)kDistDirectBits[GetPosSlot(backRes)];
}

UInt32 CCoder::GetOptimalFast(UInt32 &backRes)
{
  UInt32 len;
  if (m_OptimumEndIndex != m_OptimumCurrentIndex)
  {
    m_OptimumEndIndex = m_OptimumCurrentIndex;
    len = _nextLen;
    backRes = _nextBack;
  }
  else
  {
    GetMatches();
    const UInt32 numPairs = m_MatchDistances[0];
    if (numPairs == 0)
      return 1;
    len = m_MatchDistances[(size_t)numPairs - 1];
    backRes = m_MatchDistances[numPairs];
    // the matches are sorted by length and distance: a shorter one can be closer
    Int32 gain = MatchGain(len, backRes);
    for (UInt32 i = 1; i < numPairs - 2; i += 2)
    {
      const UInt32 len2 = m_MatchDistances[i];
      const UInt32 back2 = m_MatchDistances[(size_t)i + 1];
      const Int32 gain2 = MatchGain(len2, back2) - kShorterBias;
      if (gain < gain2)
      {
        gain = gain2;
        len = len2;
        backRes = back2;
      }
    }
    if (len == kMatchMinLen && backRes >= kTooFar)
      return 1;
  }

  /* the bytes after the current position: the match finder is m_AdditionalOffset bytes
     ahead (in the second pass of a block it is at the end of the block, and the
     matches come from m_OnePosMatchesMemory: the decisions must be the same) */
  if (len < m_MatchMaxLen
      && Inline_MatchFinder_GetNumAvailableBytes(&_lzInWindow) + m_AdditionalOffset - 1 > len)
  {
    if (len >= kGoodLen)
    {
      const UInt32 cutValue = _lzInWindow.cutValue;
      _lzInWindow.cutValue = (cutValue >> 2) + 1;
      GetMatches();
      _lzInWindow.cutValue = cutValue;
    }
    else
      GetMatches();
    const UInt32 numPairs = m_MatchDistances[0];
    if (numPairs != 0)
    {
      const UInt32 lenNext = m_MatchDistances[(size_t)numPairs - 1];
      const UInt32 backNext = m_MatchDistances[numPairs];
      if (MatchGain(lenNext, backNext) > MatchGain(len, backRes) + kNextBias)
      {
        _nextLen = (UInt16)lenNext;
        _nextBack = (UInt16)backNext;
        m_OptimumEndIndex = m_OptimumCurrentIndex + 1;
        return 1;
      }
    }
    MovePos(len - 2);
    return len;
  }
  MovePos(len - 1);
  return len;
}

void CTables::InitStructures()
{
  UInt32 i;
  for (i = 0; i < 256; i++)
    litLenLevels[i] = 8;
  litLenLevels[i++] = 13;
  for (;i < kFixedMainTableSize; i++)
    litLenLevels[i] = 5;
  for (i = 0; i < kFixedDistTableSize; i++)
    distLevels[i] = 5;
}

NO_INLINE void CCoder::LevelTableDummy(const Byte *levels, unsigned numLevels, UInt32 *freqs)
{
  unsigned prevLen = 0xFF;
  unsigned nextLen = levels[0];
  unsigned count = 0;
  unsigned maxCount = 7;
  unsigned minCount = 4;
  
  if (nextLen == 0)
  {
    maxCount = 138;
    minCount = 3;
  }
  
  for (unsigned n = 0; n < numLevels; n++)
  {
    unsigned curLen = nextLen;
    nextLen = (n < numLevels - 1) ? levels[(size_t)n + 1] : 0xFF;
    count++;
    if (count < maxCount && curLen == nextLen)
      continue;
    
    if (count < minCount)
      freqs[curLen] += (UInt32)count;
    else if (curLen != 0)
    {
      if (curLen != prevLen)
      {
        freqs[curLen]++;
        count--;
      }
      freqs[kTableLevelRepNumber]++;
    }
    else if (count <= 10)
      freqs[kTableLevel0Number]++;
    else
      freqs[kTableLevel0Number2]++;

    count = 0;
    prevLen = curLen;
    
    if (nextLen == 0)
    {
      maxCount = 138;
      minCount = 3;
    }
    else if (curLen == nextLen)
    {
      maxCount = 6;
      minCount = 3;
    }
    else
    {
      maxCount = 7;
      minCount = 4;
    }
  }
}

NO_INLINE void CCoder::WriteBits(UInt32 value, unsigned numBits)
{
  m_OutStream.WriteBits(value, numBits);
}

#define WRITE_HF2(codes, lens, i) m_OutStream.WriteBits(codes[i], lens[i])
#define WRITE_HF2_NO_INLINE(codes, lens, i)   WriteBits(codes[i], lens[i])
#define WRITE_HF(i) WriteBits(codes[i], lens[i])

NO_INLINE void CCoder::LevelTableCode(const Byte *levels, unsigned numLevels, const Byte *lens, const UInt32 *codes)
{
  unsigned prevLen = 0xFF;
  unsigned nextLen = levels[0];
  unsigned count = 0;
  unsigned maxCount = 7;
  unsigned minCount = 4;
  
  if (nextLen == 0)
  {
    maxCount = 138;
    minCount = 3;
  }
  
  for (unsigned n = 0; n < numLevels; n++)
  {
    unsigned curLen = nextLen;
    nextLen = (n < numLevels - 1) ? levels[(size_t)n + 1] : 0xFF;
    count++;
    if (count < maxCount && curLen == nextLen)
      continue;
    
    if (count < minCount)
      for (unsigned i = 0; i < count; i++)
        WRITE_HF(curLen);
    else if (curLen != 0)
    {
      if (curLen != prevLen)
      {
        WRITE_HF(curLen);
        count--;
      }
      WRITE_HF(kTableLevelRepNumber);
      WriteBits(count - 3, 2);
    }
    else if (count <= 10)
    {
      WRITE_HF(kTableLevel0Number);
      WriteBits(count - 3, 3);
    }
    else
    {
      WRITE_HF(kTableLevel0Number2);
      WriteBits(count - 11, 7);
    }

    count = 0;
    prevLen = curLen;
    
    if (nextLen == 0)
    {
      maxCount = 138;
      minCount = 3;
    }
    else if (curLen == nextLen)
    {
      maxCount = 6;
      minCount = 3;
    }
    else
    {
      maxCount = 7;
      minCount = 4;
    }
  }
}

NO_INLINE void CCoder::MakeTables(unsigned maxHuffLen)
{
  Huffman_Generate(mainFreqs, mainCodes, m_NewLevels.litLenLevels, kFixedMainTableSize, maxHuffLen);
  Huffman_Generate(distFreqs, distCodes, m_NewLevels.distLevels, kDistTableSize64, maxHuffLen);
}

static NO_INLINE UInt32 Huffman_GetPrice(const UInt32 *freqs, const Byte *lens, UInt32 num)
{
  UInt32 price = 0;
  UInt32 i;
  for (i = 0; i < num; i++)
    price += lens[i] * freqs[i];
  return price;
}

static NO_INLINE UInt32 Huffman_GetPrice_Spec(
    const UInt32 *freqs, const Byte *lens, UInt32 num,
    const Byte *extraBits, UInt32 extraBase)
{
  return
    Huffman_GetPrice(freqs, lens, num) +
    Huffman_GetPrice(freqs + extraBase, extraBits, num - extraBase);
}

NO_INLINE UInt32 CCoder::GetLzBlockPrice() const
{
  return
    Huffman_GetPrice_Spec(mainFreqs, m_NewLevels.litLenLevels,
        kFixedMainTableSize, m_LenDirectBits, kSymbolMatch) +
    Huffman_GetPrice_Spec(distFreqs, m_NewLevels.distLevels,
        kDistTableSize64, kDistDirectBits, 0);
}

NO_INLINE void CCoder::TryBlock()
{
  memset(mainFreqs, 0, sizeof(mainFreqs));
  memset(distFreqs, 0, sizeof(distFreqs));

  m_ValueIndex = 0;
  UInt32 blockSize = BlockSizeRes;
  BlockSizeRes = 0;
  for (;;)
  {
    if (m_OptimumCurrentIndex == m_OptimumEndIndex)
    {
      if (m_Pos >= kMatchArrayLimit
          || BlockSizeRes >= blockSize
          || (!m_SecondPass && ((Inline_MatchFinder_GetNumAvailableBytes(&_lzInWindow) == 0) || m_ValueIndex >= m_ValueBlockSize)))
        break;
    }
    UInt32 pos;
    UInt32 len;
    if (_fastMode)
      len = GetOptimalFast(pos);
    else
      len = GetOptimal(pos);
    CCodeValue &codeValue = m_Values[m_ValueIndex++];
    if (len >= kMatchMinLen)
    {
      const UInt32 newLen = len - kMatchMinLen;
      codeValue.Len = (UInt16)newLen;
      mainFreqs[kSymbolMatch + (size_t)g_LenSlots[newLen]]++;
      codeValue.Pos = (UInt16)pos;
      distFreqs[GetPosSlot(pos)]++;
    }
    else
    {
      const unsigned b = *(Inline_MatchFinder_GetPointerToCurrentPos(&_lzInWindow) - m_AdditionalOffset);
      mainFreqs[b]++;
      codeValue.Len = k_CodeValue_Len_Is_Literal_Flag;
      codeValue.Pos = (UInt16)b;
    }
    m_AdditionalOffset -= len;
    BlockSizeRes += len;
  }
  mainFreqs[kSymbolEndOfBlock]++;
  m_AdditionalOffset += BlockSizeRes;
  m_SecondPass = true;
}

NO_INLINE void CCoder::SetPrices(const CLevels &levels)
{
  if (_fastMode)
    return;
  UInt32 i;
  for (i = 0; i < 256; i++)
  {
    Byte price = levels.litLenLevels[i];
    m_LiteralPrices[i] = ((price != 0) ? price : kNoLiteralStatPrice);
  }
  
  for (i = 0; i < m_NumLenCombinations; i++)
  {
    UInt32 slot = g_LenSlots[i];
    Byte price = levels.litLenLevels[kSymbolMatch + (size_t)slot];
    m_LenPrices[i] = (Byte)(((price != 0) ? price : kNoLenStatPrice) + m_LenDirectBits[slot]);
  }
  
  for (i = 0; i < kDistTableSize64; i++)
  {
    Byte price = levels.distLevels[i];
    m_PosPrices[i] = (Byte)(((price != 0) ? price: kNoPosStatPrice) + kDistDirectBits[i]);
  }
}

#if MAX_HUF_LEN_12 > 12
// Huffman_ReverseBits() now supports 12-bits values only.
#error Stop_Compiling_Bad_MAX_HUF_LEN_12
#endif
static NO_INLINE void Huffman_ReverseBits(UInt32 *codes, const Byte *lens, UInt32 num)
{
  const Byte * const lens_lim = lens + num;
  do
  {
    // we should change constants, if lens[*] can be larger than 12.
    UInt32 x = *codes;
    x = ((x & (0x555     )) << 2) + (x & (0xAAA     ));
    x = ((x & (0x333 << 1)) << 4) | (x & (0xCCC << 1));
    x = ((x & (0xF0F << 3)) << 8) | (x & (0x0F0 << 3));
    // we can use (x) instead of (x & (0xFF << 7)), if we support garabage data after (*lens) bits.
    *codes++ = (((x & (0xFF << 7)) << 16) | x) >> (*lens ^ 31);
  }
  while (++lens != lens_lim);
}

NO_INLINE void CCoder::WriteBlock()
{
  Huffman_ReverseBits(mainCodes, m_NewLevels.litLenLevels, kFixedMainTableSize);
  Huffman_ReverseBits(distCodes, m_NewLevels.distLevels, kDistTableSize64);

  CCodeValue *values = m_Values;
  const CCodeValue * const values_lim = values + m_ValueIndex;

  if (values != values_lim)
  do
  {
    const UInt32 len = values->Len;
    const UInt32 dist = values->Pos;
    if (len == k_CodeValue_Len_Is_Literal_Flag)
      WRITE_HF2(mainCodes, m_NewLevels.litLenLevels, dist);
    else
    {
      const unsigned lenSlot = g_LenSlots[len];
      WRITE_HF2(mainCodes, m_NewLevels.litLenLevels, kSymbolMatch + lenSlot);
      m_OutStream.WriteBits(len - m_LenStart[lenSlot], m_LenDirectBits[lenSlot]);
      const unsigned posSlot = GetPosSlot(dist);
      WRITE_HF2(distCodes, m_NewLevels.distLevels, posSlot);
      m_OutStream.WriteBits(dist - kDistStart[posSlot], kDistDirectBits[posSlot]);
    }
  }
  while (++values != values_lim);
  WRITE_HF2_NO_INLINE(mainCodes, m_NewLevels.litLenLevels, kSymbolEndOfBlock);
}

static UInt32 GetStorePrice(UInt32 blockSize, unsigned bitPosition)
{
  UInt32 price = 0;
  do
  {
    UInt32 nextBitPosition = (bitPosition + kFinalBlockFieldSize + kBlockTypeFieldSize) & 7;
    unsigned numBitsForAlign = nextBitPosition > 0 ? (8 - nextBitPosition): 0;
    UInt32 curBlockSize = (blockSize < (1 << 16)) ? blockSize : (1 << 16) - 1;
    price += kFinalBlockFieldSize + kBlockTypeFieldSize + numBitsForAlign + (2 + 2) * 8 + curBlockSize * 8;
    bitPosition = 0;
    blockSize -= curBlockSize;
  }
  while (blockSize != 0);
  return price;
}

void CCoder::WriteStoreBlock(UInt32 blockSize, UInt32 additionalOffset, bool finalBlock)
{
  do
  {
    UInt32 curBlockSize = (blockSize < (1 << 16)) ? blockSize : (1 << 16) - 1;
    blockSize -= curBlockSize;
    WriteBits((finalBlock && (blockSize == 0) ? NFinalBlockField::kFinalBlock: NFinalBlockField::kNotFinalBlock), kFinalBlockFieldSize);
    WriteBits(NBlockType::kStored, kBlockTypeFieldSize);
    m_OutStream.FlushByte();
    WriteBits((UInt16)curBlockSize, kStoredBlockLengthFieldSize);
    WriteBits((UInt16)~curBlockSize, kStoredBlockLengthFieldSize);
    const Byte *data = Inline_MatchFinder_GetPointerToCurrentPos(&_lzInWindow)- additionalOffset;
    m_OutStream.WriteBytes(data, curBlockSize);
    additionalOffset -= curBlockSize;
  }
  while (blockSize != 0);
}

NO_INLINE UInt32 CCoder::TryDynBlock(unsigned tableIndex, UInt32 numPasses)
{
  CTables &t = m_Tables[tableIndex];
  BlockSizeRes = t.BlockSizeRes;
  UInt32 posTemp = t.m_Pos;
  SetPrices(t);

  for (UInt32 p = 0; p < numPasses; p++)
  {
    m_Pos = posTemp;
    TryBlock();
    const unsigned numHuffBits =
        m_ValueIndex > 18000 ? MAX_HUF_LEN_12 :
        m_ValueIndex >  7000 ? 11 :
        m_ValueIndex >  2000 ? 10 : 9;
    MakeTables(numHuffBits);
    SetPrices(m_NewLevels);
  }

  (CLevels &)t = m_NewLevels;

  m_NumLitLenLevels = kMainTableSize;
  while (m_NumLitLenLevels > kNumLitLenCodesMin && m_NewLevels.litLenLevels[(size_t)m_NumLitLenLevels - 1] == 0)
    m_NumLitLenLevels--;
  
  m_NumDistLevels = kDistTableSize64;
  while (m_NumDistLevels > kNumDistCodesMin && m_NewLevels.distLevels[(size_t)m_NumDistLevels - 1] == 0)
    m_NumDistLevels--;
  
  UInt32 levelFreqs[kLevelTableSize];
  memset(levelFreqs, 0, sizeof(levelFreqs));

  LevelTableDummy(m_NewLevels.litLenLevels, m_NumLitLenLevels, levelFreqs);
  LevelTableDummy(m_NewLevels.distLevels, m_NumDistLevels, levelFreqs);
  
  Huffman_Generate(levelFreqs, levelCodes, levelLens, kLevelTableSize, kMaxLevelBitLength);
  
  m_NumLevelCodes = kNumLevelCodesMin;
  for (UInt32 i = 0; i < kLevelTableSize; i++)
  {
    Byte level = levelLens[kCodeLengthAlphabetOrder[i]];
    if (level > 0 && i >= m_NumLevelCodes)
      m_NumLevelCodes = i + 1;
    m_LevelLevels[i] = level;
  }
  
  return GetLzBlockPrice() +
      Huffman_GetPrice_Spec(levelFreqs, levelLens, kLevelTableSize, kLevelDirectBits, kTableDirectLevels) +
      kNumLenCodesFieldSize + kNumDistCodesFieldSize + kNumLevelCodesFieldSize +
      m_NumLevelCodes * kLevelFieldSize + kFinalBlockFieldSize + kBlockTypeFieldSize;
}

NO_INLINE UInt32 CCoder::TryFixedBlock(unsigned tableIndex)
{
  CTables &t = m_Tables[tableIndex];
  BlockSizeRes = t.BlockSizeRes;
  m_Pos = t.m_Pos;
  m_NewLevels.SetFixedLevels();
  SetPrices(m_NewLevels);
  TryBlock();
  return kFinalBlockFieldSize + kBlockTypeFieldSize + GetLzBlockPrice();
}

NO_INLINE UInt32 CCoder::GetBlockPrice(unsigned tableIndex, unsigned numDivPasses)
{
  CTables &t = m_Tables[tableIndex];
  t.StaticMode = false;
  UInt32 price = TryDynBlock(tableIndex, m_NumPasses);
  t.BlockSizeRes = BlockSizeRes;
  UInt32 numValues = m_ValueIndex;
  UInt32 posTemp = m_Pos;
  UInt32 additionalOffsetEnd = m_AdditionalOffset;
  
  if (m_CheckStatic && m_ValueIndex <= kFixedHuffmanCodeBlockSizeMax)
  {
    const UInt32 fixedPrice = TryFixedBlock(tableIndex);
    t.StaticMode = (fixedPrice < price);
    if (t.StaticMode)
      price = fixedPrice;
  }

  const UInt32 storePrice = GetStorePrice(BlockSizeRes, 0); // bitPosition
  t.StoreMode = (storePrice <= price);
  if (t.StoreMode)
    price = storePrice;

  t.UseSubBlocks = false;

  if (numDivPasses > 1 && numValues >= kDivideCodeBlockSizeMin)
  {
    CTables &t0 = m_Tables[(tableIndex << 1)];
    (CLevels &)t0 = t;
    t0.BlockSizeRes = t.BlockSizeRes >> 1;
    t0.m_Pos = t.m_Pos;
    UInt32 subPrice = GetBlockPrice((tableIndex << 1), numDivPasses - 1);

    UInt32 blockSize2 = t.BlockSizeRes - t0.BlockSizeRes;
    if (t0.BlockSizeRes >= kDivideBlockSizeMin && blockSize2 >= kDivideBlockSizeMin)
    {
      CTables &t1 = m_Tables[(tableIndex << 1) + 1];
      (CLevels &)t1 = t;
      t1.BlockSizeRes = blockSize2;
      t1.m_Pos = m_Pos;
      m_AdditionalOffset -= t0.BlockSizeRes;
      subPrice += GetBlockPrice((tableIndex << 1) + 1, numDivPasses - 1);
      t.UseSubBlocks = (subPrice < price);
      if (t.UseSubBlocks)
        price = subPrice;
    }
  }
  
  m_AdditionalOffset = additionalOffsetEnd;
  m_Pos = posTemp;
  return price;
}

void CCoder::CodeBlock(unsigned tableIndex, bool finalBlock)
{
  CTables &t = m_Tables[tableIndex];
  if (t.UseSubBlocks)
  {
    CodeBlock((tableIndex << 1), false);
    CodeBlock((tableIndex << 1) + 1, finalBlock);
  }
  else
  {
    if (t.StoreMode)
      WriteStoreBlock(t.BlockSizeRes, m_AdditionalOffset, finalBlock);
    else
    {
      WriteBits((finalBlock ? NFinalBlockField::kFinalBlock: NFinalBlockField::kNotFinalBlock), kFinalBlockFieldSize);
      if (t.StaticMode)
      {
        WriteBits(NBlockType::kFixedHuffman, kBlockTypeFieldSize);
        TryFixedBlock(tableIndex);
        unsigned i;
        const unsigned kMaxStaticHuffLen = 9;
        for (i = 0; i < kFixedMainTableSize; i++)
          mainFreqs[i] = (UInt32)1 << (kMaxStaticHuffLen - m_NewLevels.litLenLevels[i]);
        for (i = 0; i < kFixedDistTableSize; i++)
          distFreqs[i] = (UInt32)1 << (kMaxStaticHuffLen - m_NewLevels.distLevels[i]);
        MakeTables(kMaxStaticHuffLen);
      }
      else
      {
        if (m_NumDivPasses > 1 || m_CheckStatic)
          TryDynBlock(tableIndex, 1);
        WriteBits(NBlockType::kDynamicHuffman, kBlockTypeFieldSize);
        WriteBits(m_NumLitLenLevels - kNumLitLenCodesMin, kNumLenCodesFieldSize);
        WriteBits(m_NumDistLevels - kNumDistCodesMin, kNumDistCodesFieldSize);
        WriteBits(m_NumLevelCodes - kNumLevelCodesMin, kNumLevelCodesFieldSize);
        
        for (UInt32 i = 0; i < m_NumLevelCodes; i++)
          WriteBits(m_LevelLevels[i], kLevelFieldSize);
        
        Huffman_ReverseBits(levelCodes, levelLens, kLevelTableSize);
        LevelTableCode(m_NewLevels.litLenLevels, m_NumLitLenLevels, levelLens, levelCodes);
        LevelTableCode(m_NewLevels.distLevels, m_NumDistLevels, levelLens, levelCodes);
      }
      WriteBlock();
    }
    m_AdditionalOffset -= t.BlockSizeRes;
  }
}


HRESULT CCoder::CodeStream(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    UInt32 prefixSize, bool finalChunk, ICompressProgressInfo *progress)
{
  m_CheckStatic = (m_NumPasses != 1 || m_NumDivPasses != 1);
  m_IsMultiPass = (m_CheckStatic || (m_NumPasses != 1 || m_NumDivPasses != 1));
  /* Heptazip: the optimal parser prices a block with the Huffman codes of the previous
     block. A chunk after the first one would start with the default codes: its first
     block is parsed again with its own codes (kNumPassesFirstBlock passes; silesia -mx5,
     1 MiB chunks: the loss against a single stream goes from 0.17% to 0.09%, same time).
     The passes need the cache of the matches (m_IsMultiPass). */
  const unsigned numPasses = m_NumPasses;
  const bool warmUp = (prefixSize != 0 && !_fastMode && m_NumPasses < kNumPassesFirstBlock);
  if (warmUp)
    m_IsMultiPass = true;

  /* we can set stream mode before MatchFinder_Create
    if default MatchFinder mode was not STREAM_MODE) */
  // MatchFinder_SET_STREAM_MODE(&_lzInWindow);

  CSeqInStreamWrap _seqInStream;
  _seqInStream.Init(inStream);
  MatchFinder_SET_STREAM(&_lzInWindow, &_seqInStream.vt)

  RINOK(Create())

  m_ValueBlockSize = (7 << 10) + (1 << 12) * m_NumDivPasses;

  UInt64 nowPos = 0;

  MatchFinder_Init(&_lzInWindow);
  // Heptazip: the history before the chunk goes into the match finder, but is not coded.
  if (prefixSize != 0)
  {
    if (_btMode)
      Bt3Zip_MatchFinder_Skip(&_lzInWindow, prefixSize);
    else
      Hc3Zip_MatchFinder_Skip(&_lzInWindow, prefixSize);
  }
  m_OutStream.SetStream(outStream);
  m_OutStream.Init();

  m_OptimumEndIndex = m_OptimumCurrentIndex = 0;

  CTables &t = m_Tables[1];
  t.m_Pos = 0;
  t.InitStructures();

  m_AdditionalOffset = 0;
  do
  {
    t.BlockSizeRes = kBlockUncompressedSizeThreshold;
    m_SecondPass = false;
    if (warmUp)
      m_NumPasses = (nowPos == 0 ? kNumPassesFirstBlock : numPasses);
    GetBlockPrice(1, m_NumDivPasses);
    CodeBlock(1, finalChunk && Inline_MatchFinder_GetNumAvailableBytes(&_lzInWindow) == 0);
    nowPos += m_Tables[1].BlockSizeRes;
    if (progress != NULL)
    {
      UInt64 packSize = m_OutStream.GetProcessedSize();
      RINOK(progress->SetRatioInfo(&nowPos, &packSize))
    }
  }
  while (Inline_MatchFinder_GetNumAvailableBytes(&_lzInWindow) != 0);

  m_NumPasses = numPasses;

  // Heptazip: an empty stored block ends the chunk on a byte boundary (as zlib's
  // Z_SYNC_FLUSH): the chunks are concatenated as bytes.
  if (!finalChunk)
    WriteStoreBlock(0, 0, false);
  
  if (_seqInStream.Res != S_OK)
    return _seqInStream.Res;

  if (_lzInWindow.result != SZ_OK)
    return SResToHRESULT(_lzInWindow.result);
  return m_OutStream.Flush();
}

// ---------- Heptazip: chunked and parallel coding (design 0009) ----------
//
// The input is cut in chunks of kChunkSize bytes, coded independently by a pool of
// coders. Each coder gets the last kHistorySize bytes of the previous chunk as history:
// the matches can reach back as in a single coder. All the chunks but the final one end
// with an empty stored block, so the output is one Deflate stream, readable by any
// decoder. The size of the chunks does not depend on the number of threads: the output
// is the same for every -mmt. An input up to kChunkSize is coded as by 7-Zip.

static const size_t kChunkSize = (size_t)1 << 20;
static const unsigned kNumThreadsMax = 64;

#ifndef Z7_ST
#define Z7_DEFLATE_TASK_BASE  : public CMtTask
#define Z7_DEFLATE_TASK_OVERRIDE  Z7_override
#else
#define Z7_DEFLATE_TASK_BASE
#define Z7_DEFLATE_TASK_OVERRIDE
#endif

class CChunkSlot Z7_DEFLATE_TASK_BASE
{
public:
  CByteBuffer In;          // history (end of the previous chunk), then the data
  size_t PrefixSize;
  size_t DataSize;
  bool Final;
  HRESULT Res;
  CMyComPtr2_Create<ISequentialOutStream, CDynBufSeqOutStream> Out;
  CRecordVector<CCoder *> *Coders;
  #ifdef Z7_DEFLATE_LIBDEFLATE
  CRecordVector<libdeflate_compressor *> *LibCoders;  // if libdeflate codes the chunks
  CByteBuffer LibOut;
  size_t LibOutSize;
  #endif

  CChunkSlot(): PrefixSize(0), DataSize(0), Final(false), Res(S_OK), Coders(NULL)
    #ifdef Z7_DEFLATE_LIBDEFLATE
      , LibCoders(NULL), LibOutSize(0)
    #endif
    {}
  void Run(unsigned workerIndex) Z7_DEFLATE_TASK_OVERRIDE;
  const Byte *GetOut(size_t &size) const
  {
    #ifdef Z7_DEFLATE_LIBDEFLATE
    if (LibCoders)
    {
      size = LibOutSize;
      return LibOut;
    }
    #endif
    size = Out->GetSize();
    return Out->GetBuffer();
  }
};

void CChunkSlot::Run(unsigned workerIndex)
{
  #ifdef Z7_DEFLATE_LIBDEFLATE
  if (LibCoders)
  {
    LibOutSize = libdeflate_deflate_compress_chunk((*LibCoders)[workerIndex],
        In, PrefixSize, DataSize, Final ? 1 : 0, LibOut, LibOut.Size());
    // 0: the output buffer is too small (it has the size of the bound)
    Res = (LibOutSize != 0) ? S_OK : E_FAIL;
    return;
  }
  #endif
  // no exception may leave Run()
  try
  {
    CMyComPtr2_Create<ISequentialInStream, CBufInStream> inStream;
    inStream->Init(In, PrefixSize + DataSize);
    Out->Init();
    Res = (*Coders)[workerIndex]->CodeStream(inStream, Out, (UInt32)PrefixSize, Final, NULL);
  }
  catch (const COutBufferException &e) { Res = e.ErrorCode; }
  catch (...) { Res = E_OUTOFMEMORY; }
}

class CChunkEncoder
{
  Z7_CLASS_NO_COPY(CChunkEncoder)
public:
  CObjectVector<CChunkSlot> Slots;
  CRecordVector<CCoder *> Coders;   // one per thread
  #ifdef Z7_DEFLATE_LIBDEFLATE
  CRecordVector<libdeflate_compressor *> LibCoders;   // one per thread
  #endif
  #ifndef Z7_ST
  CMtTaskPool Pool;
  #endif

  CChunkEncoder() {}
  ~CChunkEncoder()
  {
    #ifndef Z7_ST
    Pool.Stop();
    #endif
    FOR_VECTOR (i, Coders)
      delete Coders[i];
    #ifdef Z7_DEFLATE_LIBDEFLATE
    FOR_VECTOR (k, LibCoders)
      libdeflate_free_compressor(LibCoders[k]);
    #endif
  }
  HRESULT Code(CCoder &main, ISequentialInStream *inStream, ISequentialOutStream *outStream,
      ICompressProgressInfo *progress);
private:
  HRESULT Code2(CCoder &main, ISequentialInStream *inStream, ISequentialOutStream *outStream,
      ICompressProgressInfo *progress, unsigned numThreads);
};

void CCoder::DeleteChunkEncoder()
{
  delete _chunkEncoder;
  _chunkEncoder = NULL;
}

HRESULT CChunkEncoder::Code(CCoder &main, ISequentialInStream *inStream, ISequentialOutStream *outStream,
    ICompressProgressInfo *progress)
{
  unsigned numThreads = main._numThreads;
  if (numThreads < 1)
    numThreads = 1;
  if (numThreads > kNumThreadsMax)
    numThreads = kNumThreadsMax;
  #ifdef Z7_ST
  numThreads = 1;
  #endif
  HRESULT res;
  // the threads are stopped in any case (BaseCode() catches the exceptions)
  try { res = Code2(main, inStream, outStream, progress, numThreads); }
  catch (...)
  {
    #ifndef Z7_ST
    Pool.Stop();
    #endif
    throw;
  }
  #ifndef Z7_ST
  Pool.Stop();
  #endif
  return res;
}

HRESULT CChunkEncoder::Code2(CCoder &main, ISequentialInStream *inStream, ISequentialOutStream *outStream,
    ICompressProgressInfo *progress, unsigned numThreads)
{
  const size_t history = main.m_Deflate64Mode ? kHistorySize64 : kHistorySize32;
  // The chunks in work and a few more, read in advance or waiting to be written in order
  // (silesia, 20 threads: as fast as 2 slots per thread, 96 MiB instead of 110 MiB;
  // with (numThreads + 2) slots 6% slower)
  const unsigned numSlots = (numThreads == 1) ? 1 : numThreads + numThreads / 2 + 1;
  while (Slots.Size() < numSlots)
    Slots.AddNew();
  #ifdef Z7_DEFLATE_LIBDEFLATE
  const bool useLib = (main._libLevel != 0);
  if (useLib)
  {
    const unsigned numCoders = numThreads;
    while (LibCoders.Size() < numCoders)
    {
      libdeflate_compressor *c = libdeflate_alloc_compressor(main._libLevel);
      if (!c)
        return E_OUTOFMEMORY;
      LibCoders.Add(c);
    }
  }
  #endif
  for (unsigned i = 0; i < numSlots; i++)
  {
    CChunkSlot &s = Slots[i];
    s.In.AllocAtLeast(history + kChunkSize);
    s.Coders = &Coders;
    #ifdef Z7_DEFLATE_LIBDEFLATE
    s.LibCoders = useLib ? &LibCoders : NULL;
    if (useLib)
      // + the empty stored block of a chunk that is not final
      s.LibOut.AllocAtLeast(libdeflate_deflate_compress_bound(LibCoders[0], kChunkSize) + 16);
    #endif
  }

  // The first chunk. If the input ends there, it is coded here as a whole.
  {
    CChunkSlot &s = Slots[0];
    size_t len = kChunkSize;
    RINOK(ReadStream(inStream, s.In, &len))
    if (len < kChunkSize)
    {
      #ifdef Z7_DEFLATE_LIBDEFLATE
      if (useLib)
      {
        const size_t size = libdeflate_deflate_compress(LibCoders[0], s.In, len, s.LibOut, s.LibOut.Size());
        if (size == 0)
          return E_FAIL;
        RINOK(WriteStream(outStream, s.LibOut, size))
        if (progress)
        {
          const UInt64 inSize = len, outSize = size;
          RINOK(progress->SetRatioInfo(&inSize, &outSize))
        }
        return S_OK;
      }
      #endif
      CMyComPtr2_Create<ISequentialInStream, CBufInStream> bufStream;
      bufStream->Init(s.In, len);
      return main.CodeStream(bufStream, outStream, 0, true, progress);
    }
    s.PrefixSize = 0;
    s.DataSize = len;
    s.Final = false;
  }

  #ifdef Z7_DEFLATE_LIBDEFLATE
  if (!useLib)
  #endif
  while (Coders.Size() < numThreads)
  {
    CCoder *c = new CCoder(main.m_Deflate64Mode);
    Coders.Add(c);
    c->SetProps(&main._props);
  }
  #ifndef Z7_ST
  if (numThreads > 1)
  {
    RINOK(Pool.Start(numThreads))
  }
  #endif

  UInt64 inProcessed = 0, outProcessed = 0;
  unsigned next = 0, numInWork = 0;
  unsigned last = 0;       // the slot read last
  bool inEnd = false;
  bool finalWritten = false;
  bool first = true;

  for (;;)
  {
    while (!inEnd && numInWork < numSlots)
    {
      const unsigned k = (next + numInWork) % numSlots;
      CChunkSlot &s = Slots[k];
      if (first)
        first = false;   // already read
      else
      {
        // the history: the end of the previous chunk (full, so longer than history)
        const CChunkSlot &prev = Slots[last];
        memmove(s.In, prev.In + prev.PrefixSize + prev.DataSize - history, history);
        size_t len = kChunkSize;
        RINOK(ReadStream(inStream, s.In + history, &len))
        if (len == 0)
        {
          inEnd = true;
          break;
        }
        s.PrefixSize = history;
        s.DataSize = len;
        s.Final = (len < kChunkSize);
        if (s.Final)
          inEnd = true;
      }
      last = k;
      numInWork++;
      #ifndef Z7_ST
      if (numThreads > 1)
      {
        RINOK(Pool.Submit(&s))
      }
      else
      #endif
        s.Run(0);
    }

    if (numInWork == 0)
      break;
    CChunkSlot &s = Slots[next];
    #ifndef Z7_ST
    if (numThreads > 1)
    {
      const WRes wres = s.Done.Lock();
      if (wres != 0)
        return HRESULT_FROM_WIN32(wres);
      if (s.ExceptionWasCaught)
        return E_FAIL;
    }
    #endif
    RINOK(s.Res)
    size_t outSize;
    const Byte *outData = s.GetOut(outSize);
    RINOK(WriteStream(outStream, outData, outSize))
    finalWritten = s.Final;
    inProcessed += s.DataSize;
    outProcessed += outSize;
    if (progress)
    {
      RINOK(progress->SetRatioInfo(&inProcessed, &outProcessed))
    }
    next = (next + 1) % numSlots;
    numInWork--;
  }

  if (!finalWritten)
  {
    // The input ended with a full chunk: an empty final block with the fixed codes
    // (BFINAL = 1, BTYPE = 01, end of block), as zlib.
    static const Byte kFinalEmptyBlock[2] = { 3, 0 };
    RINOK(WriteStream(outStream, kFinalEmptyBlock, sizeof(kFinalEmptyBlock)))
  }
  return S_OK;
}

HRESULT CCoder::CodeReal(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 * /* inSize */ , const UInt64 * /* outSize */ , ICompressProgressInfo *progress)
{
  if (!_chunkEncoder)
    _chunkEncoder = new CChunkEncoder;
  return _chunkEncoder->Code(*this, inStream, outStream, progress);
}

HRESULT CCoder::BaseCode(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress)
{
  try { return CodeReal(inStream, outStream, inSize, outSize, progress); }
  catch(const COutBufferException &e) { return e.ErrorCode; }
  catch(...) { return E_FAIL; }
}

Z7_COM7F_IMF(CCOMCoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress))
  { return BaseCode(inStream, outStream, inSize, outSize, progress); }

Z7_COM7F_IMF(CCOMCoder::SetCoderProperties(const PROPID *propIDs, const PROPVARIANT *props, UInt32 numProps))
  { return BaseSetEncoderProperties2(propIDs, props, numProps); }

Z7_COM7F_IMF(CCOMCoder64::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress))
  { return BaseCode(inStream, outStream, inSize, outSize, progress); }

Z7_COM7F_IMF(CCOMCoder64::SetCoderProperties(const PROPID *propIDs, const PROPVARIANT *props, UInt32 numProps))
  { return BaseSetEncoderProperties2(propIDs, props, numProps); }

}}}
