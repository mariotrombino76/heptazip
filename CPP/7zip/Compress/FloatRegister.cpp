// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
// FloatRegister.cpp
// Tensor7: registers the "FLOAT" method (design 0008).
//
// Method ID 3F 70 7F 5E 72 8B 00 01: a random developer ID (prefix 3F, DOC/Methods.txt,
// "Generate 8-bytes ID") of Tensor7, method 0001. 7-Zip and its forks without this
// method report the archive as "unsupported method", like any unknown codec.

#include "StdAfx.h"

#include "../Common/RegisterCodec.h"

#include "FloatCoder.h"

namespace NCompress {
namespace NFloat {

REGISTER_CODEC_E(
  Float,
  CDecoder(),
  CEncoder(),
  0x3F707F5E728B0001,
  "FLOAT")

}}
