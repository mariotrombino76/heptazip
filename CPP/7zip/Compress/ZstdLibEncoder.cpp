// ZstdLibEncoder.cpp
// 7zip-modern: Zstandard encoder based on the reference library (third_party/zstd).

#include "StdAfx.h"

#include "../../../third_party/zstd/lib/zstd.h"

#include "../../../C/Alloc.h"

#include "../Common/StreamUtils.h"

#include "ZstdLibEncoder.h"

namespace NCompress {
namespace NZstdLib {

static const int kDefaultLevel = 3; // ZSTD_CLEVEL_DEFAULT
static const UInt32 kNumThreadsMax = 256;

CEncoder::CEncoder():
    _level(kDefaultLevel),
    _windowLog(0),
    _numThreads(1)
  {}


// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

Z7_COM7F_IMF(CEncoder::SetCoderProperties(const PROPID *propIDs, const PROPVARIANT *coderProps, UInt32 numProps))
{
  int level = kDefaultLevel;
  unsigned windowLog = 0;

  for (UInt32 i = 0; i < numProps; i++)
  {
    const PROPVARIANT &prop = coderProps[i];
    const PROPID propID = propIDs[i];

    switch (propID)
    {
      // not used by this encoder
      case NCoderPropID::kAffinity:
      case NCoderPropID::kNumThreadGroups:
      case NCoderPropID::kReduceSize:
      case NCoderPropID::kExpectedDataSize:
        continue;
      default: break;
    }

    if (prop.vt != VT_UI4)
      return E_INVALIDARG;
    const UInt32 v = prop.ulVal;

    switch (propID)
    {
      case NCoderPropID::kLevel:
      {
        // -mx0 is mapped to the Copy method by the 7z handler; here 0 means default
        if (v == 0)
          level = kDefaultLevel;
        else
        {
          const UInt32 maxLevel = (UInt32)ZSTD_maxCLevel();
          level = (int)(v > maxLevel ? maxLevel : v);
        }
        break;
      }
      case NCoderPropID::kDictionarySize:
      {
        // window size: rounded up to a power of 2, in the range supported by zstd
        // (stable API: ZSTD_WINDOWLOG_MIN/MAX are in the experimental section)
        const ZSTD_bounds bounds = ZSTD_cParam_getBounds(ZSTD_c_windowLog);
        if (ZSTD_isError(bounds.error))
          return E_FAIL;
        unsigned log = (unsigned)bounds.lowerBound;
        while ((int)log < bounds.upperBound && ((UInt64)1 << log) < v)
          log++;
        windowLog = log;
        break;
      }
      case NCoderPropID::kNumThreads:
      {
        #ifndef Z7_ST
        RINOK(SetNumberOfThreads(v))
        #endif
        break;
      }
      default:
        return E_INVALIDARG;
    }
  }

  _level = level;
  _windowLog = windowLog;
  return S_OK;
}


#ifndef Z7_ST
Z7_COM7F_IMF(CEncoder::SetNumberOfThreads(UInt32 numThreads))
{
  if (numThreads < 1)
    numThreads = 1;
  if (numThreads > kNumThreadsMax)
    numThreads = kNumThreadsMax;
  _numThreads = numThreads;
  return S_OK;
}
#endif


/* Same layout as 7-Zip-zstd: { version major, version minor, level, 0, 0 }.
   The 7-Zip decoder ignores the properties (the frame header has all the data). */
Z7_COM7F_IMF(CEncoder::WriteCoderProperties(ISequentialOutStream *outStream))
{
  Byte props[kPropsSize];
  props[0] = (Byte)ZSTD_VERSION_MAJOR;
  props[1] = (Byte)ZSTD_VERSION_MINOR;
  props[2] = (Byte)_level;
  props[3] = 0;
  props[4] = 0;
  return WriteStream(outStream, props, kPropsSize);
}


// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

namespace {

/* RAII owner of a compression context. */
class CCCtx
{
  Z7_CLASS_NO_COPY(CCCtx)
public:
  ZSTD_CCtx *Ctx;
  CCCtx(): Ctx(ZSTD_createCCtx()) {}
  ~CCCtx() { ZSTD_freeCCtx(Ctx); }
};

/* RAII owner of a buffer from MyAlloc(). */
class CBuf
{
  Z7_CLASS_NO_COPY(CBuf)
public:
  Byte *Data;
  explicit CBuf(size_t size): Data((Byte *)MyAlloc(size)) {}
  ~CBuf() { MyFree(Data); }
};

HRESULT ZstdErrorToHRESULT(size_t code)
{
  switch (ZSTD_getErrorCode(code))
  {
    case ZSTD_error_memory_allocation:
    case ZSTD_error_workSpace_tooSmall:
      return E_OUTOFMEMORY;
    case ZSTD_error_parameter_unsupported:
    case ZSTD_error_parameter_outOfBound:
      return E_INVALIDARG;
    default:
      return E_FAIL;
  }
}

#define RINOK_ZSTD(x) { const size_t _r_ = (x); if (ZSTD_isError(_r_)) return ZstdErrorToHRESULT(_r_); }

} // namespace


Z7_COM7F_IMF(CEncoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 *inSize, const UInt64 * /* outSize */, ICompressProgressInfo *progress))
{
  CCCtx cctx;
  if (!cctx.Ctx)
    return E_OUTOFMEMORY;

  RINOK_ZSTD(ZSTD_CCtx_setParameter(cctx.Ctx, ZSTD_c_compressionLevel, _level))
  if (_windowLog != 0)
    RINOK_ZSTD(ZSTD_CCtx_setParameter(cctx.Ctx, ZSTD_c_windowLog, (int)_windowLog))
  // 7z stores the CRC of each file: the frame checksum would be redundant
  RINOK_ZSTD(ZSTD_CCtx_setParameter(cctx.Ctx, ZSTD_c_checksumFlag, 0))
  #ifndef Z7_ST
  if (_numThreads > 1)
  {
    // ZSTD_MULTITHREAD: the frame is compressed by (_numThreads) workers
    RINOK_ZSTD(ZSTD_CCtx_setParameter(cctx.Ctx, ZSTD_c_nbWorkers, (int)_numThreads))
  }
  #endif
  if (inSize)
    RINOK_ZSTD(ZSTD_CCtx_setPledgedSrcSize(cctx.Ctx, *inSize))

  const size_t inBufSize = ZSTD_CStreamInSize();
  const size_t outBufSize = ZSTD_CStreamOutSize();
  CBuf inBuf(inBufSize);
  CBuf outBuf(outBufSize);
  if (!inBuf.Data || !outBuf.Data)
    return E_OUTOFMEMORY;

  UInt64 inProcessed = 0;
  UInt64 outProcessed = 0;
  bool inFinished = false;

  for (;;)
  {
    size_t inLen = 0;
    if (!inFinished)
    {
      inLen = inBufSize;
      RINOK(ReadStream(inStream, inBuf.Data, &inLen))
      if (inLen < inBufSize)
        inFinished = true;
      inProcessed += inLen;
    }

    const ZSTD_EndDirective mode = inFinished ? ZSTD_e_end : ZSTD_e_continue;
    ZSTD_inBuffer in = { inBuf.Data, inLen, 0 };

    for (;;)
    {
      ZSTD_outBuffer out = { outBuf.Data, outBufSize, 0 };
      const size_t remaining = ZSTD_compressStream2(cctx.Ctx, &out, &in, mode);
      if (ZSTD_isError(remaining))
        return ZstdErrorToHRESULT(remaining);
      if (out.pos != 0)
      {
        RINOK(WriteStream(outStream, outBuf.Data, out.pos))
        outProcessed += out.pos;
      }
      // continue mode: until the input is consumed; end mode: until the frame is flushed
      if (mode == ZSTD_e_end ? (remaining == 0) : (in.pos == in.size))
        break;
    }

    if (progress)
    {
      RINOK(progress->SetRatioInfo(&inProcessed, &outProcessed))
    }
    if (inFinished)
      return S_OK;
  }
}

}}
