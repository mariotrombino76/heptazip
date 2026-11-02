// DeflateDecoder.cpp

#include "StdAfx.h"

#include "DeflateDecoder.h"

namespace NCompress {
namespace NDeflate {
namespace NDecoder {

CCoder::CCoder(bool deflate64Mode):
    _deflateNSIS(false),
    _deflate64Mode(deflate64Mode),
    _keepHistory(false),
    _needFinishInput(false),
    _needInitInStream(true),
    _outSizeDefined(false),
    _outStartPos(0)
    {}

UInt32 CCoder::ReadBits(unsigned numBits)
{
  return m_InBitStream.ReadBits(numBits);
}

Byte CCoder::ReadAlignedByte()
{
  return m_InBitStream.ReadAlignedByte();
}

bool CCoder::DecodeLevels(Byte *levels, unsigned numSymbols)
{
  unsigned i = 0;
  
  do
  {
    unsigned sym = m_LevelDecoder.Decode(&m_InBitStream);
    if (sym < kTableDirectLevels)
      levels[i++] = (Byte)sym;
    else
    {
      if (sym >= kLevelTableSize)
        return false;
      
      unsigned num;
      unsigned numBits;
      Byte symbol;
      
      if (sym == kTableLevelRepNumber)
      {
        if (i == 0)
          return false;
        numBits = 2;
        num = 0;
        symbol = levels[(size_t)i - 1];
      }
      else
      {
        sym -= kTableLevel0Number;
        sym <<= 2;
        numBits = 3 + (unsigned)sym;
        num = ((unsigned)sym << 1);
        symbol = 0;
      }
      
      num += i + 3 + ReadBits(numBits);
      if (num > numSymbols)
        return false;
      do
        levels[i++] = symbol;
      while (i < num);
    }
  }
  while (i < numSymbols);
  
  return true;
}

/* ---------- Tensor7: fast decoder (docs/design/0011) ----------

   DecodeFast() decodes the symbols of a Huffman block as libdeflate does: a 64-bit bit
   buffer refilled 8 bytes at a time without branches, tables indexed by the next bits
   (LSB first) with the length or distance base and the number of extra bits in the
   entry, and matches copied 8 bytes at a time. It reads the bytes of the buffer of
   m_InBitStream and writes in the buffer of m_OutWindowStream directly, so it runs only
   far from their limits; everything else (the limits, the end of the input, the copies
   that cross the end of the window, the symbols and distances that are not valid) stops
   it before the symbol, and the code of 7-Zip decodes that symbol: the result for any
   input, valid or not, is the same as without DecodeFast(). */

/* entry of a table: bits 0-4: bits of the code in this table (for a length or a
   distance: plus its extra bits, and the bits of the code are in bits 8-12, as in
   libdeflate: one shift for both); bit 6: literal; bit 7: other than a literal, a length
   or a distance (kind in bits 13-14); bits 16-31: value (literal, base of length or
   distance, subtable). In (symEntries) of BuildFastTable(), bits 8-12 of a length or a
   distance are the number of extra bits. */
static const UInt32 kFlagLit = 1 << 6;
static const UInt32 kFlagExc = 1 << 7;
static const unsigned kExcSlow = 0;   // not handled by DecodeFast()
static const unsigned kExcEob = 1;
static const unsigned kExcSub = 2;

#define FAST_LIT(v)  (kFlagLit | ((UInt32)(v) << 16))
#define FAST_LEN(extra, base)  (((UInt32)(extra) << 8) | ((UInt32)(base) << 16))
#define FAST_EXC(kind, value)  (kFlagExc | ((UInt32)(kind) << 13) | ((UInt32)(value) << 16))
#define FAST_IS_SUB(e)  (((e) & (kFlagExc | (3u << 13))) == (kFlagExc | ((UInt32)kExcSub << 13)))
#define FAST_IS_EOB(e)  (((e) & (3u << 13)) == ((UInt32)kExcEob << 13))

static const int kFastStop = 0;  // near a limit: continue with the code of 7-Zip
static const int kFastEob = 1;   // end of block
static const int kFastSlow = 2;  // the next symbol must be decoded by the code of 7-Zip

static const UInt32 kWindowSize = (UInt32)1 << 20;
static const UInt32 kFastOutMin = 1 << 9;  // DecodeFast() starts only with this space
static const size_t kFastInMin = 1 << 6;
static const UInt32 kFastCopyOver = 32;    // the copies write up to 31 bytes after a match

static inline UInt32 MakeEntry(UInt32 e, unsigned codeBits)
{
  if (e & (kFlagLit | kFlagExc))
    return e | codeBits;
  const unsigned extra = (e >> 8) & 31;
  return (e & ~((UInt32)31 << 8)) | ((UInt32)codeBits << 8) | (codeBits + extra);
}

/* the canonical Huffman code of (lens), as 7-Zip's NHuffman::CDecoder::Build() that
   accepted it before (no over-subscribed code; the codes that are not used stay kExcSlow) */
static void BuildFastTable(UInt32 *table, unsigned mainBits, const Byte *lens,
    const UInt32 *symEntries, unsigned numSyms)
{
  const unsigned subBits = kNumHuffmanBits - mainBits;
  const UInt32 kSlowEntry = FAST_EXC(kExcSlow, 0);
  unsigned counts[kNumHuffmanBits + 1];
  UInt32 next[kNumHuffmanBits + 1];
  unsigned i;
  for (i = 0; i <= kNumHuffmanBits; i++)
    counts[i] = 0;
  for (i = 0; i < numSyms; i++)
    counts[lens[i]]++;
  counts[0] = 0;
  {
    UInt32 code = 0;
    for (i = 1; i <= kNumHuffmanBits; i++)
    {
      code = (code + counts[i - 1]) << 1;
      next[i] = code;
    }
  }
  const UInt32 mainSize = (UInt32)1 << mainBits;
  for (i = 0; i < mainSize; i++)
    table[i] = kSlowEntry;
  UInt32 subPos = mainSize;
  for (unsigned sym = 0; sym < numSyms; sym++)
  {
    const unsigned len = lens[sym];
    if (len == 0)
      continue;
    UInt32 c = next[len]++;
    UInt32 rev = 0;
    for (unsigned k = 0; k < len; k++)
    {
      rev = (rev << 1) | (c & 1);
      c >>= 1;
    }
    const UInt32 e = symEntries[sym];
    if (len <= mainBits)
    {
      const UInt32 e2 = MakeEntry(e, len);
      for (UInt32 j = rev; j < mainSize; j += (UInt32)1 << len)
        table[j] = e2;
      continue;
    }
    UInt32 t = table[rev & (mainSize - 1)];
    if (!FAST_IS_SUB(t))
    {
      t = FAST_EXC(kExcSub, subPos) | mainBits;
      table[rev & (mainSize - 1)] = t;
      for (UInt32 j = 0; j < ((UInt32)1 << subBits); j++)
        table[subPos + j] = kSlowEntry;
      subPos += (UInt32)1 << subBits;
    }
    UInt32 *sub = table + (t >> 16);
    const unsigned len2 = len - mainBits;
    const UInt32 e2 = MakeEntry(e, len2);
    for (UInt32 j = rev >> mainBits; j < ((UInt32)1 << subBits); j += (UInt32)1 << len2)
      sub[j] = e2;
  }
}

void CCoder::BuildFastTables(const Byte *litLenLevels, const Byte *distLevels)
{
  UInt32 syms[kFixedMainTableSize];
  unsigned i;
  for (i = 0; i < 256; i++)
    syms[i] = FAST_LIT(i);
  syms[kSymbolEndOfBlock] = FAST_EXC(kExcEob, 0);
  const Byte *lenStart = _deflate64Mode ? kLenStart64 : kLenStart32;
  const Byte *lenBits = _deflate64Mode ? kLenDirectBits64 : kLenDirectBits32;
  for (i = kSymbolMatch; i < kFixedMainTableSize; i++)
  {
    const unsigned s = i - kSymbolMatch;
    // the symbols 286 and 287 are left to the code of 7-Zip
    syms[i] = (i < kMainTableSize) ?
        FAST_LEN(lenBits[s], lenStart[s] + kMatchMinLen) :
        FAST_EXC(kExcSlow, 0);
  }
  BuildFastTable(_fastLit, kFastLitBits, litLenLevels, syms, kFixedMainTableSize);
  for (i = 0; i < kFixedDistTableSize; i++)
    syms[i] = (i < _numDistLevels) ?
        FAST_LEN(kDistDirectBits[i], kDistStart[i]) :
        FAST_EXC(kExcSlow, 0);
  BuildFastTable(_fastDist, kFastDistBits, distLevels, syms, kFixedDistTableSize);
}


int CCoder::DecodeFast(UInt32 &curSize)
{
  const UInt32 histLimit = _deflate64Mode ? kHistorySize64 : kHistorySize32;
  UInt32 pos, limitPos;
  Byte * const buf = m_OutWindowStream.Fast_GetBuf(pos, limitPos);
  UInt32 outRem = limitPos - pos;
  if (outRem < kFastOutMin || curSize < kFastOutMin
      || m_OutWindowStream.GetBufSize() < histLimit + kFastOutMin)
    return kFastStop;
  outRem -= kFastCopyOver;
  if (outRem > curSize)
    outRem = curSize;

  CInBuffer &inBuf = m_InBitStream.GetInByte();
  const Byte *inLim;
  const Byte *in = inBuf.Fast_GetPtr(inLim);
  /* the check is before GetNormalBits(): near the end of the input the bit decoder must
     read ahead as in the code of 7-Zip alone, that detects the end of the input by the
     number of bytes read after it (ExtraBitsWereRead_Fast()) */
  if ((size_t)(inLim - in) < kFastInMin)
    return kFastStop;
  unsigned n;
  UInt64 bb = m_InBitStream.GetNormalBits(n);
  in = inBuf.Fast_GetPtr(inLim);
  inLim -= 8;
  NBitl::CDecoder<CInBuffer>::CState state;
  m_InBitStream.SaveState(state);
  const unsigned numBits0 = n;
  const Byte * const in0 = in;

  Byte * const out0 = buf + pos;
  Byte *out = out0;
  Byte * const outLim = out0 + outRem;
  const UInt32 bufSize = m_OutWindowStream.GetBufSize();
  const bool overDict = m_OutWindowStream.IsOverDict();
  int res;
  // the entry of the next symbol in the main table, looked up before the refill
  // if the bits are there (it does not wait for the load of the refill)
  UInt32 e = _fastLit[(size_t)bb & ((1u << kFastLitBits) - 1)];

  for (;;)
  {
    // (outLim - 2): room for the 3 literals of an iteration
    if (in > inLim || out >= outLim - 2)
    {
      res = kFastStop;
      break;
    }
    // refill: (n) becomes 56..63
    {
      const unsigned n0 = n;
      bb |= GetUi64(in) << n;
      in += (63 - n) >> 3;
      n |= 56;
      if (n0 < kFastLitBits)
        e = _fastLit[(size_t)bb & ((1u << kFastLitBits) - 1)];
    }

    // the bits are consumed in (b, k), and committed in (bb, n) after the symbol
    UInt64 b = bb;
    unsigned k = n;
    if (FAST_IS_SUB(e))
    {
      b >>= kFastLitBits;
      k -= kFastLitBits;
      e = _fastLit[(e >> 16) + ((size_t)b & ((1u << (kNumHuffmanBits - kFastLitBits)) - 1))];
    }
    if (e & kFlagLit)
    {
      b >>= (e & 31);
      k -= (e & 31);
      *out++ = (Byte)(e >> 16);
      /* up to 2 more literals of the main table without a refill: here (k >= 56 - 15),
         and a literal has up to 15 bits */
      e = _fastLit[(size_t)b & ((1u << kFastLitBits) - 1)];
      if (e & kFlagLit)
      {
        b >>= (e & 31);
        k -= (e & 31);
        *out++ = (Byte)(e >> 16);
        e = _fastLit[(size_t)b & ((1u << kFastLitBits) - 1)];
        if (e & kFlagLit)
        {
          b >>= (e & 31);
          k -= (e & 31);
          *out++ = (Byte)(e >> 16);
          e = _fastLit[(size_t)b & ((1u << kFastLitBits) - 1)];
        }
      }
      bb = b;
      n = k;
      continue;
    }
    if (e & kFlagExc)
    {
      res = kFastSlow;
      if (FAST_IS_EOB(e))
      {
        bb = b >> (e & 31);
        n = k - (e & 31);
        res = kFastEob;
      }
      break;
    }
    UInt32 len;
    {
      const unsigned total = e & 31;
      // the code and the extra bits, then those of the distance (Deflate64: 16 extra bits)
      if (k < total + kNumHuffmanBits + 14)
      {
        res = kFastSlow;
        break;
      }
      len = (e >> 16) + (UInt32)((b & (((UInt64)1 << total) - 1)) >> ((e >> 8) & 31));
      b >>= total;
      k -= total;
    }
    e = _fastDist[(size_t)b & ((1u << kFastDistBits) - 1)];
    if (FAST_IS_SUB(e))
    {
      b >>= kFastDistBits;
      k -= kFastDistBits;
      e = _fastDist[(e >> 16) + ((size_t)b & ((1u << (kNumHuffmanBits - kFastDistBits)) - 1))];
    }
    if (e & kFlagExc)
    {
      res = kFastSlow;
      break;
    }
    UInt32 dist; // distance - 1
    {
      const unsigned total = e & 31;
      dist = (e >> 16) + (UInt32)((b & (((UInt64)1 << total) - 1)) >> ((e >> 8) & 31));
      b >>= total;
      k -= total;
    }
    if (dist >= histLimit || len > (size_t)(outLim - out))
    {
      res = kFastSlow;
      break;
    }
    const UInt32 outPos = (UInt32)(out - buf);
    if (dist < outPos)
    {
      const Byte *src = out - dist - 1;
      Byte *dest = out;
      Byte * const lim = out + len;
      if (dist >= 7)
      {
        // most matches are short: 32 bytes without a loop
        SetUi64(dest, GetUi64(src))
        SetUi64(dest + 8, GetUi64(src + 8))
        SetUi64(dest + 16, GetUi64(src + 16))
        SetUi64(dest + 24, GetUi64(src + 24))
        dest += 32;
        src += 32;
        while (dest < lim)
        {
          SetUi64(dest, GetUi64(src))
          dest += 8;
          src += 8;
        }
      }
      else if (dist == 0)
      {
        const UInt64 v = (UInt64)*src * 0x0101010101010101;
        do
        {
          SetUi64(dest, v)
          dest += 8;
        }
        while (dest < lim);
      }
      else
        do
          *dest++ = *src++;
        while (dest < lim);
    }
    else
    {
      // the match starts before the start of the window buffer, at its end
      if (!overDict || dist >= bufSize)
      {
        res = kFastSlow;
        break;
      }
      UInt32 srcPos = outPos - dist - 1 + bufSize;
      for (UInt32 i = 0; i < len; i++)
      {
        out[i] = buf[srcPos];
        if (++srcPos == bufSize)
          srcPos = 0;
      }
    }
    out += len;
    bb = b;
    n = k;
    e = _fastLit[(size_t)bb & ((1u << kFastLitBits) - 1)];
  }

  curSize -= (UInt32)(out - out0);
  m_OutWindowStream.Fast_SetPos((UInt32)(out - buf));

  // the (n) bits of (bb) were not used: the last (n) bits of the bytes read
  const size_t numRead = (size_t)(in - in0);
  if (numRead * 8 >= n)
  {
    in -= n >> 3;
    inBuf.Fast_SetPtr(in);
    const unsigned r = n & 7;
    m_InBitStream.SetLastBits(r ? in[-1] : (Byte)0, r);
  }
  else
  {
    // some bits of the bit buffer of m_InBitStream were not used
    inBuf.Fast_SetPtr(in0);
    m_InBitStream.RestoreState(state);
    for (UInt32 used = numBits0 + (UInt32)numRead * 8 - n; used != 0;)
    {
      const unsigned num = used > 16 ? 16 : (unsigned)used;
      m_InBitStream.ReadBits(num);
      used -= num;
    }
  }
  return res;
}


#define RIF(x) { if (!(x)) return false; }

bool CCoder::ReadTables(void)
{
  m_FinalBlock = (ReadBits(kFinalBlockFieldSize) == NFinalBlockField::kFinalBlock);
  if (m_InBitStream.ExtraBitsWereRead())
    return false;
  const UInt32 blockType = ReadBits(kBlockTypeFieldSize);
  if (blockType > NBlockType::kDynamicHuffman)
    return false;
  if (m_InBitStream.ExtraBitsWereRead())
    return false;

  if (blockType == NBlockType::kStored)
  {
    m_StoredMode = true;
    m_InBitStream.AlignToByte();
    m_StoredBlockSize = ReadAligned_UInt16(); // ReadBits(kStoredBlockLengthFieldSize)
    if (_deflateNSIS)
      return true;
    return (m_StoredBlockSize == (UInt16)~ReadAligned_UInt16());
  }

  m_StoredMode = false;

  CLevels levels;
  if (blockType == NBlockType::kFixedHuffman)
  {
    levels.SetFixedLevels();
    _numDistLevels = _deflate64Mode ? kDistTableSize64 : kDistTableSize32;
  }
  else
  {
    const unsigned numLitLenLevels = ReadBits(kNumLenCodesFieldSize) + kNumLitLenCodesMin;
    _numDistLevels = (unsigned)ReadBits(kNumDistCodesFieldSize) + kNumDistCodesMin;
    const unsigned numLevelCodes = ReadBits(kNumLevelCodesFieldSize) + kNumLevelCodesMin;

    if (!_deflate64Mode)
      if (_numDistLevels > kDistTableSize32)
        return false;
    
    const unsigned kLevelTableSize_aligned4 = kLevelTableSize + 1;
    Byte levelLevels[kLevelTableSize_aligned4];
    memset (levelLevels, 0, sizeof(levelLevels));
    unsigned i = 0;
    do
      levelLevels[kCodeLengthAlphabetOrder[i++]] = (Byte)ReadBits(kLevelFieldSize);
    while (i != numLevelCodes);
    
    if (m_InBitStream.ExtraBitsWereRead())
      return false;

    RIF(m_LevelDecoder.Build(levelLevels, false)) // full
    
    Byte tmpLevels[kFixedMainTableSize + kFixedDistTableSize];
    if (!DecodeLevels(tmpLevels, numLitLenLevels + _numDistLevels))
      return false;
    
    if (m_InBitStream.ExtraBitsWereRead())
      return false;

    levels.SubClear();
    memcpy(levels.litLenLevels, tmpLevels, numLitLenLevels);
    memcpy(levels.distLevels, tmpLevels + numLitLenLevels, _numDistLevels);
  }
  RIF(m_MainDecoder.Build(levels.litLenLevels))
  RIF(m_DistDecoder.Build(levels.distLevels))
  BuildFastTables(levels.litLenLevels, levels.distLevels);
  return true;
}


HRESULT CCoder::InitInStream(bool needInit)
{
  if (needInit)
  {
    // for HDD-Windows:
    // (1 << 15) - best for reading only prefetch
    // (1 << 22) - best for real reading / writing
    if (!m_InBitStream.Create(1 << 20))
      return E_OUTOFMEMORY;
    m_InBitStream.Init();
    _needInitInStream = false;
  }
  return S_OK;
}


HRESULT CCoder::CodeSpec(UInt32 curSize, bool finishInputStream, UInt32 inputProgressLimit)
{
  if (_remainLen == kLenIdFinished)
    return S_OK;
  
  if (_remainLen == kLenIdNeedInit)
  {
    if (!_keepHistory)
      // Tensor7: a window larger than the history: DecodeFast() needs room, and the
      // output stream gets larger writes
      if (!m_OutWindowStream.Create(kWindowSize))
        return E_OUTOFMEMORY;
    RINOK(InitInStream(_needInitInStream))
    m_OutWindowStream.Init(_keepHistory);
  
    m_FinalBlock = false;
    _remainLen = 0;
    _needReadTable = true;
  }

  // _remainLen >= 0
  while (_remainLen && curSize)
  {
    _remainLen--;
    const Byte b = m_OutWindowStream.GetByte(_rep0);
    m_OutWindowStream.PutByte(b);
    curSize--;
  }

  UInt64 inputStart = 0;
  if (inputProgressLimit != 0)
    inputStart = m_InBitStream.GetProcessedSize();

  while (curSize || finishInputStream)
  {
    if (m_InBitStream.ExtraBitsWereRead())
      return S_FALSE;

    if (_needReadTable)
    {
      if (m_FinalBlock)
      {
        _remainLen = kLenIdFinished;
        break;
      }
 
      if (inputProgressLimit != 0)
        if (m_InBitStream.GetProcessedSize() - inputStart >= inputProgressLimit)
          return S_OK;
      
      if (!ReadTables())
        return S_FALSE;
      if (m_InBitStream.ExtraBitsWereRead())
        return S_FALSE;
      _needReadTable = false;
    }

    if (m_StoredMode)
    {
      if (finishInputStream && curSize == 0 && m_StoredBlockSize != 0)
        return S_FALSE;
      /* NSIS version contains some bits in bitl bits buffer.
         So we must read some first bytes via ReadAlignedByte */
      UInt32 num = m_StoredBlockSize;
      if (num > curSize)
          num = curSize;
      m_StoredBlockSize -= num;
      curSize -= num;
      for (; num && m_InBitStream.ThereAreDataInBitsBuffer(); num--)
        m_OutWindowStream.PutByte(ReadAlignedByte());
      if (num)
      {
#if 1
        // fast code
        do
        {
          size_t a;
          Byte *buf = m_OutWindowStream.GetOutBuffer(a);
          // a != 0
          if (a > num)
              a = num;
          // a != 0
          a = m_InBitStream.ReadDirectBytesPart(buf, a);
          if (a == 0)
            return S_FALSE;
          m_OutWindowStream.SkipWrittenBytes(a);
          num -= (UInt32)a;
        }
        while (num);
#else
        // slow code:
        do
          m_OutWindowStream.PutByte(m_InBitStream.ReadDirectByte());
        while (--num);
#endif
      }
      _needReadTable = (m_StoredBlockSize == 0);
      continue;
    }
    
    while (curSize)
    {
      {
        // Tensor7: fast decoder, far from the limits of the buffers
        const int r = DecodeFast(curSize);
        if (r == kFastEob)
        {
          _needReadTable = true;
          break;
        }
        if (curSize == 0)
          break;
      }
      if (m_InBitStream.ExtraBitsWereRead_Fast())
        return S_FALSE;
      unsigned sym;
#if 0
      sym = m_MainDecoder.Decode(&m_InBitStream);
#else
      Z7_HUFF_DECODE_CHECK(sym, &m_MainDecoder, kNumHuffmanBits, kNumTableBits_Main, &m_InBitStream, { return S_FALSE; })
#endif

      if (sym < 0x100)
      {
        m_OutWindowStream.PutByte((Byte)sym);
        curSize--;
        continue;
      }
      if (sym == kSymbolEndOfBlock)
      {
        _needReadTable = true;
        break;
      }
#if 0
      if (sym >= kMainTableSize)
        return S_FALSE;
#endif
      {
        sym -= kSymbolMatch;
        UInt32 len;
        {
          unsigned numBits;
          if (_deflate64Mode)
          {
            len = kLenStart64[sym];
            numBits = kLenDirectBits64[sym];
          }
          else
          {
            len = kLenStart32[sym];
            numBits = kLenDirectBits32[sym];
          }
          len += kMatchMinLen + m_InBitStream.ReadBits(numBits);
        }
       
#if 0
        sym = m_DistDecoder.Decode(&m_InBitStream);
        if (sym >= _numDistLevels)
          return S_FALSE;
#else
        Z7_HUFF_DECODE_CHECK(sym, &m_DistDecoder, kNumHuffmanBits, kNumTableBits_Dist, &m_InBitStream, { return S_FALSE; })
#endif

#if 1
        sym = kDistStart[sym] + m_InBitStream.ReadBits(kDistDirectBits[sym]);
        // Tensor7: the window can be larger than the history (it was rejected by CopyBlock())
        if (sym >= (_deflate64Mode ? kHistorySize64 : kHistorySize32))
          return S_FALSE;
#else
        if (sym >= 4)
        {
          // sym &= 31;
          const unsigned numDirectBits = (sym - 2) >> 1;
          sym = (2u | (sym & 1)) << numDirectBits;
          sym += m_InBitStream.ReadBits(numDirectBits);
        }
#endif
        UInt32 locLen = len;
        if (locLen > curSize)
          locLen = (UInt32)curSize;
        if (!m_OutWindowStream.CopyBlock(sym, locLen))
          return S_FALSE;
        curSize -= locLen;
        len -= locLen;
        if (len != 0)
        {
          _remainLen = (Int32)len;
          _rep0 = sym;
          break;
        }
      }
    }
    
    if (finishInputStream && curSize == 0)
    {
      if (m_MainDecoder.Decode(&m_InBitStream) != kSymbolEndOfBlock)
        return S_FALSE;
      _needReadTable = true;
    }
  }

  if (m_InBitStream.ExtraBitsWereRead())
    return S_FALSE;

  return S_OK;
}


#ifdef Z7_NO_EXCEPTIONS

#define DEFLATE_TRY_BEGIN
#define DEFLATE_TRY_END(res)

#else

#define DEFLATE_TRY_BEGIN try {
#define DEFLATE_TRY_END(res) } \
  catch(const CSystemException &e) { res = e.ErrorCode; } \
  catch(...) { res = S_FALSE; }

  // catch(const CInBufferException &e)  { res = e.ErrorCode; }
  // catch(const CLzOutWindowException &e)  { res = e.ErrorCode; }

#endif


HRESULT CCoder::CodeReal(ISequentialOutStream *outStream, ICompressProgressInfo *progress)
{
  HRESULT res;
  
  DEFLATE_TRY_BEGIN
  
  m_OutWindowStream.SetStream(outStream);
  CCoderReleaser flusher(this);

  const UInt64 inStart = _needInitInStream ? 0 : m_InBitStream.GetProcessedSize();

  for (;;)
  {
    const UInt32 kInputProgressLimit = 1 << 21;
    UInt32 curSize = 1 << 20;
    bool finishInputStream = false;
    if (_outSizeDefined)
    {
      const UInt64 rem = _outSize - GetOutProcessedCur();
      if (curSize >= rem)
      {
        curSize = (UInt32)rem;
        if (_needFinishInput)
          finishInputStream = true;
        else if (curSize == 0)
          break;
      }
    }
    
    RINOK(CodeSpec(curSize, finishInputStream, progress ? kInputProgressLimit : 0))
    
    if (_remainLen == kLenIdFinished)
      break;

    if (progress)
    {
      const UInt64 inSize = m_InBitStream.GetProcessedSize() - inStart;
      const UInt64 nowPos64 = GetOutProcessedCur();
      RINOK(progress->SetRatioInfo(&inSize, &nowPos64))
    }
  }
  
  flusher.NeedFlush = false;
  res = Flush();
  if (res == S_OK && _remainLen != kLenIdNeedInit && InputEofError())
    return S_FALSE;
  
  DEFLATE_TRY_END(res)
  
  return res;
}


Z7_COM7F_IMF(CCoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 * /* inSize */, const UInt64 *outSize, ICompressProgressInfo *progress))
{
  SetInStream(inStream);
  SetOutStreamSize(outSize);
  const HRESULT res = CodeReal(outStream, progress);
  ReleaseInStream();
  /*
  if (res == S_OK)
    if (_needFinishInput && inSize && *inSize != m_InBitStream.GetProcessedSize())
      res = S_FALSE;
  */
  return res;
}


Z7_COM7F_IMF(CCoder::SetFinishMode(UInt32 finishMode))
{
  Set_NeedFinishInput(finishMode != 0);
  return S_OK;
}


Z7_COM7F_IMF(CCoder::GetInStreamProcessedSize(UInt64 *value))
{
  *value = m_InBitStream.GetStreamSize();
  return S_OK;
}


Z7_COM7F_IMF(CCoder::ReadUnusedFromInBuf(void *data, UInt32 size, UInt32 *processedSize))
{
  AlignToByte();
  UInt32 i = 0;
  {
    for (i = 0; i < size; i++)
    {
      if (!m_InBitStream.ReadAlignedByte_FromBuf(((Byte *)data)[i]))
        break;
    }
  }
  if (processedSize)
    *processedSize = i;
  return S_OK;
}


Z7_COM7F_IMF(CCoder::SetInStream(ISequentialInStream *inStream))
{
  m_InStreamRef = inStream;
  m_InBitStream.SetStream(inStream);
  return S_OK;
}


Z7_COM7F_IMF(CCoder::ReleaseInStream())
{
  m_InStreamRef.Release();
  m_InBitStream.ClearStreamPtr();
  return S_OK;
}


void CCoder::SetOutStreamSizeResume(const UInt64 *outSize)
{
  _outSizeDefined = (outSize != NULL);
  _outSize = 0;
  if (_outSizeDefined)
    _outSize = *outSize;
  m_OutWindowStream.Init(_keepHistory);
  _outStartPos = m_OutWindowStream.GetProcessedSize();
  _remainLen = kLenIdNeedInit;
}


Z7_COM7F_IMF(CCoder::SetOutStreamSize(const UInt64 *outSize))
{
  /*
    18.06:
    We want to support GetInputProcessedSize() before CCoder::Read()
    So we call m_InBitStream.Init() even before buffer allocations
    m_InBitStream.Init() just sets variables to default values
    But later we will call m_InBitStream.Init() again with real buffer pointers
  */
  m_InBitStream.Init();
  _needInitInStream = true;
  SetOutStreamSizeResume(outSize);
  return S_OK;
}


#ifndef Z7_NO_READ_FROM_CODER

Z7_COM7F_IMF(CCoder::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  const UInt64 outPos = GetOutProcessedCur();

  bool finishInputStream = false;
  if (_outSizeDefined)
  {
    const UInt64 rem = _outSize - outPos;
    if (size >= rem)
    {
      size = (UInt32)rem;
      if (_needFinishInput)
        finishInputStream = true;
    }
  }
  if (!finishInputStream && size == 0)
    return S_OK;

  HRESULT res;
  DEFLATE_TRY_BEGIN
  m_OutWindowStream.SetMemStream((Byte *)data);
  res = CodeSpec(size, finishInputStream);
  DEFLATE_TRY_END(res)
  {
    const HRESULT res2 = Flush();
    if (res2 != S_OK)
      res = res2;
  }
  if (processedSize)
    *processedSize = (UInt32)(GetOutProcessedCur() - outPos);
  m_OutWindowStream.SetMemStream(NULL);
  return res;
}

#endif


HRESULT CCoder::CodeResume(ISequentialOutStream *outStream, const UInt64 *outSize, ICompressProgressInfo *progress)
{
  SetOutStreamSizeResume(outSize);
  return CodeReal(outStream, progress);
}

}}}
