// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// ZstdCodecRegister.cpp
// Heptazip: registers Zstandard as a codec for the 7z format.
//
// Method ID 0x4F71101 and name "ZSTD", as in 7-Zip-zstd, so archives are
// compatible in both directions.
// - Decoder: the 7-Zip decoder (C/ZstdDec.c), always available.
// - Encoder: reference library (third_party/zstd), only if the build defines
//   Z7_ZSTD_LIB_ENCODER (CMake option SEVENZIP_ZSTD_ENCODER).

#include "StdAfx.h"

#include "../Common/RegisterCodec.h"

#include "ZstdDecoder.h"

#if !defined(Z7_EXTRACT_ONLY) && defined(Z7_ZSTD_LIB_ENCODER)
#include "ZstdLibEncoder.h"
#endif

namespace NCompress {
namespace NZstdLib {

REGISTER_CODEC_CREATE(CreateDec, NCompress::NZstd::CDecoder)

#if !defined(Z7_EXTRACT_ONLY) && defined(Z7_ZSTD_LIB_ENCODER)
REGISTER_CODEC_CREATE(CreateEnc, CEncoder)
#else
#define CreateEnc NULL
#endif

REGISTER_CODEC_2(ZstdLib, CreateDec, CreateEnc, 0x4F71101, "ZSTD")

}}
