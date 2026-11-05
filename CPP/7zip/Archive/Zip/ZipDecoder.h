// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// ZipDecoder.h
// Heptazip (issue #86): the decoder of one zip item (CZipDecoder) and its helpers, moved
// here from ZipHandler.cpp (the code stays there) so that the parallel extraction
// (ZipExtractMt.cpp) can own one decoder per worker thread.

#ifndef ZIP7_INC_ZIP_DECODER_H
#define ZIP7_INC_ZIP_DECODER_H

#include "../../../Common/MyCom.h"

#include "../../ICoder.h"
#include "../../IPassword.h"

#include "../../Common/CreateCoder.h"
#include "../../Common/FilterCoder.h"

#include "../../Compress/LzmaDecoder.h"

#include "../../Crypto/WzAes.h"
#include "../../Crypto/ZipCrypto.h"
#include "../../Crypto/ZipStrong.h"

#include "../IArchive.h"

#include "ZipIn.h"

namespace NArchive {
namespace NZip {

/* LZMA in zip: 4 bytes of version and props size before the LZMA props */
Z7_CLASS_IMP_NOQIB_3(
  CLzmaDecoder
  , ICompressCoder
  , ICompressSetFinishMode
  , ICompressGetInStreamProcessedSize
)
public:
  CMyComPtr2_Create<ICompressCoder, NCompress::NLzma::CDecoder> Decoder;
};

struct CMethodItem
{
  unsigned ZipMethod;
  CMyComPtr<ICompressCoder> Coder;
};

class CZipDecoder
{
  CMyComPtr2<ICompressFilter, NCrypto::NZip::CDecoder> _zipCryptoDecoder;
  CMyComPtr2<ICompressFilter, NCrypto::NZipStrong::CDecoder> _pkAesDecoder;
  CMyComPtr2<ICompressFilter, NCrypto::NWzAes::CDecoder> _wzAesDecoder;

  CMyComPtr2<ISequentialInStream, CFilterCoder> filterStream;
  CMyComPtr<ICryptoGetTextPassword> getTextPassword;
  CObjectVector<CMethodItem> methodItems;

  CLzmaDecoder *lzmaDecoderSpec;
public:
  CZipDecoder():
      lzmaDecoderSpec(NULL)
    {}

  /* Heptazip (issue #86): the password provider of a worker thread of the parallel
     extraction (ZipExtractMt.cpp), set before Decode(): then (extractCallback) is not
     asked for the password and can be NULL. */
  void SetPasswordProvider(ICryptoGetTextPassword *provider) { getTextPassword = provider; }

  /* (packStreamIn): Heptazip (#86): the packed data of the item already read into memory
     (an IInStream over the buffer, at position 0); NULL: read it from (archive), as upstream. */
  HRESULT Decode(
    DECL_EXTERNAL_CODECS_LOC_VARS
    CInArchive &archive, const CItemEx &item,
    ISequentialOutStream *realOutStream,
    IArchiveExtractCallback *extractCallback,
    ICompressProgressInfo *compressProgress,
    #ifndef Z7_ST
    UInt32 numThreads, UInt64 memUsage,
    #endif
    Int32 &res,
    ISequentialInStream *packStreamIn = NULL);
};

}}

#endif
