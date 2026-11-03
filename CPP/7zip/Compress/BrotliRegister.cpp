// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// BrotliRegister.cpp
// Heptazip: registers brotli as a codec for the 7z format (design 0014).
// Method ID 0x4F71102 and name "BROTLI", as in 7-Zip-zstd.

#include "StdAfx.h"

#include "../Common/RegisterCodec.h"

#include "BrotliCoder.h"

namespace NCompress {
namespace NBrotli {

REGISTER_CODEC_CREATE(CreateDec, CDecoder)

#ifndef Z7_EXTRACT_ONLY
REGISTER_CODEC_CREATE(CreateEnc, CEncoder)
#else
#define CreateEnc NULL
#endif

REGISTER_CODEC_2(Brotli, CreateDec, CreateEnc, 0x4F71102, "BROTLI")

}}
