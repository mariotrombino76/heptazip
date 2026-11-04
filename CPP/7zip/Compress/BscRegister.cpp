// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// BscRegister.cpp
// Heptazip: registers the "BWT" method (libbsc) for the 7z format (design 0016).
// Method ID 0x3F707F5E728B0002 (Heptazip only, after FLOAT), name "BWT".

#include "StdAfx.h"

#include "../Common/RegisterCodec.h"

#include "BscCoder.h"

namespace NCompress {
namespace NBwt {

REGISTER_CODEC_CREATE(CreateDec, CDecoder)

#ifndef Z7_EXTRACT_ONLY
REGISTER_CODEC_CREATE(CreateEnc, CEncoder)
#else
#define CreateEnc NULL
#endif

REGISTER_CODEC_2(Bwt, CreateDec, CreateEnc, 0x3F707F5E728B0002, "BWT")

}}
