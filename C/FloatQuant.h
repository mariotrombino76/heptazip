/* SPDX-License-Identifier: BSD-2-Clause
   Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip) */
/* FloatQuant.h -- context model for the quantized structs of GGUF files (FLOAT mode 4)
Heptazip (design 0008)
Internal interface of FloatPlane.c: the block format is described in FloatPlane.h. */

#ifndef ZIP7_INC_FLOAT_QUANT_H
#define ZIP7_INC_FLOAT_QUANT_H

#include "7zTypes.h"

EXTERN_C_BEGIN

#define FLOAT_QUANT_MODE        4
#define FLOAT_QUANT_MAX_FIELDS  5
#define FLOAT_QUANT_MAX_TAB     4096   /* contexts * symbols of a field */

/* The model of a block: the frequency tables (as levels) of each field. */
typedef struct
{
  unsigned qtype, phase;
  size_t n, tail;
  size_t total;   /* estimated size of the block */
  size_t tabBytes[FLOAT_QUANT_MAX_FIELDS];
  Byte levels[FLOAT_QUANT_MAX_FIELDS][FLOAT_QUANT_MAX_TAB];
} CFloatQuantPlan;

/* qtype: index of the struct type (0 = Q4_0, 1 = Q8_0, 2 = Q4_K, 3 = Q5_K, 4 = Q6_K,
   5 = Q2_K, 6 = Q3_K).
   Returns 1 and the plan, or 0 if the type is unknown or there is no struct after
   phase. */
int FloatQuant_Plan(const Byte *src, size_t srcLen, unsigned qtype, unsigned phase, CFloatQuantPlan *plan);

/* Writes the block (mode 4) planned by FloatQuant_Plan() to dest (destCap bytes):
   returns its size, or 0 if it does not fit. */
size_t FloatQuant_Write(const Byte *src, size_t srcLen, Byte *dest, size_t destCap, const CFloatQuantPlan *plan);

/* As FloatPlane_GetBlockSize() / FloatPlane_DecodeBlock() for a mode 4 block, after mode
   and outLen. */
SRes FloatQuant_GetBlockSize(const Byte *src, size_t srcLen, size_t outLen, size_t *blockSize);
/* simd: 0 = the scalar decoder only (tests) */
SRes FloatQuant_Decode(const Byte *src, size_t srcLen, Byte *dest, size_t outLen, size_t *srcUsed, int simd);

EXTERN_C_END

#endif
