/* FloatPlaneLz.c -- zstd for the blocks of the FLOAT method that are not floats (mode 2)
7zip-modern (design 0008) : Public domain */

#include "Precomp.h"

#include "Alloc.h"
#include "FloatPlaneLz.h"
#include "ZstdDec.h"

#ifdef Z7_ZSTD_LIB_ENCODER
#include "../third_party/zstd/lib/zstd.h"

/* level 3: the zstd default, ~400 MB/s; such blocks are a small part of model files */
#define kZstdLevel  3

static size_t Lz_Compress(void *ctx, const Byte *src, size_t srcLen, Byte *dest, size_t destCap)
{
  CFloatPlaneZstd *p = (CFloatPlaneZstd *)ctx;
  ZSTD_CCtx *cctx;
  size_t res;
  if (!p->cctx)
  {
    p->cctx = ZSTD_createCCtx();
    if (!p->cctx)
      return 0;
  }
  cctx = (ZSTD_CCtx *)p->cctx;
  ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
  if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, kZstdLevel))
      || ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0)))
    return 0;
  res = ZSTD_compress2(cctx, dest, destCap, src, srcLen);
  return ZSTD_isError(res) ? 0 : res;
}
#endif

/* Exactly one data frame that fills dest is accepted. */
static SRes Lz_Decompress(void *ctx, const Byte *src, size_t srcLen, Byte *dest, size_t destLen)
{
  CFloatPlaneZstd *p = (CFloatPlaneZstd *)ctx;
  CZstdDecHandle dec;
  CZstdDecState st;
  if (!p->dec)
  {
    p->dec = ZstdDec_Create(&g_AlignedAlloc, &g_BigAlloc);
    if (!p->dec)
      return SZ_ERROR_MEM;
  }
  dec = (CZstdDecHandle)p->dec;
  ZstdDec_Init(dec);
  ZstdDecState_Clear(&st);
  st.inBuf = src;
  st.inPos = 0;
  st.inLim = srcLen;
  st.outBuf_fromCaller = dest;
  st.outBufSize_fromCaller = destLen;
  st.outSize = destLen;
  st.outSize_Defined = True;
  for (;;)
  {
    const size_t inPos = st.inPos;
    const size_t winPos = st.winPos;
    const SRes res = ZstdDec_Decode(dec, &st);
    if (res != SZ_OK)
      return res;
    if (st.status == ZSTD_STATUS_FINISHED_FRAME)
      break;
    if (st.status == ZSTD_STATUS_NEEDS_MORE_INPUT
        || st.status == ZSTD_STATUS_OUT_REACHED
        || (st.inPos == inPos && st.winPos == winPos))
      return SZ_ERROR_DATA;   /* truncated, too long, or no progress */
  }
  if (st.inPos != srcLen || st.winPos != destLen
      || st.info.num_DataFrames != 1 || st.info.num_SkipFrames != 0)
    return SZ_ERROR_DATA;
  return SZ_OK;
}

void FloatPlaneZstd_Construct(CFloatPlaneZstd *p)
{
  p->cctx = NULL;
  p->dec = NULL;
  p->Lz.ctx = p;
  #ifdef Z7_ZSTD_LIB_ENCODER
  p->Lz.Compress = Lz_Compress;
  #else
  p->Lz.Compress = NULL;
  #endif
  p->Lz.Decompress = Lz_Decompress;
}

void FloatPlaneZstd_Free(CFloatPlaneZstd *p)
{
  #ifdef Z7_ZSTD_LIB_ENCODER
  if (p->cctx)
    ZSTD_freeCCtx((ZSTD_CCtx *)p->cctx);
  #endif
  if (p->dec)
    ZstdDec_Destroy((CZstdDecHandle)p->dec);
  p->cctx = NULL;
  p->dec = NULL;
}
