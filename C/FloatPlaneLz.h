/* FloatPlaneLz.h -- zstd for the blocks of the FLOAT method that are not floats (mode 2)
7zip-modern (design 0008) : Public domain

Encoder: zstd reference library (third_party/zstd), only if this file is compiled with
Z7_ZSTD_LIB_ENCODER; otherwise Lz.Compress is NULL and the encoder never uses mode 2.
Decoder: the zstd decoder of 7-Zip (C/ZstdDec.c), in the mode that decodes into the
caller's buffer: nothing is written beyond the block and no window is allocated,
whatever the frame declares. One CFloatPlaneZstd per thread. */

#ifndef ZIP7_INC_FLOAT_PLANE_LZ_H
#define ZIP7_INC_FLOAT_PLANE_LZ_H

#include "FloatPlane.h"

EXTERN_C_BEGIN

typedef struct
{
  void *cctx;   /* ZSTD_CCtx *, created on first use */
  void *dec;    /* CZstdDecHandle, created on first use */
  CFloatPlaneLz Lz;
} CFloatPlaneZstd;

void FloatPlaneZstd_Construct(CFloatPlaneZstd *p);
void FloatPlaneZstd_Free(CFloatPlaneZstd *p);

EXTERN_C_END

#endif
