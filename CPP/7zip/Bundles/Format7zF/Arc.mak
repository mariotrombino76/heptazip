COMMON_OBJS = \
  $O\CRC.obj \
  $O\CrcReg.obj \
  $O\DynLimBuf.obj \
  $O\IntToString.obj \
  $O\LzFindPrepare.obj \
  $O\Md5Reg.obj \
  $O\MyMap.obj \
  $O\MyString.obj \
  $O\MyVector.obj \
  $O\MyXml.obj \
  $O\NewHandler.obj \
  $O\Sha1Reg.obj \
  $O\Sha256Reg.obj \
  $O\Sha3Reg.obj \
  $O\Sha512Reg.obj \
  $O\Sha512Prepare.obj \
  $O\StringConvert.obj \
  $O\StringToInt.obj \
  $O\UTFConvert.obj \
  $O\Wildcard.obj \
  $O\Xxh64Reg.obj \
  $O\XzCrc64Init.obj \
  $O\XzCrc64Reg.obj \

WIN_OBJS = \
  $O\FileDir.obj \
  $O\FileFind.obj \
  $O\FileIO.obj \
  $O\FileName.obj \
  $O\PropVariant.obj \
  $O\PropVariantConv.obj \
  $O\PropVariantUtils.obj \
  $O\Synchronization.obj \
  $O\System.obj \
  $O\TimeUtils.obj \

7ZIP_COMMON_OBJS = \
  $O\CreateCoder.obj \
  $O\CWrappers.obj \
  $O\InBuffer.obj \
  $O\InOutTempBuffer.obj \
  $O\FilterCoder.obj \
  $O\LimitedStreams.obj \
  $O\LockedStream.obj \
  $O\MemBlocks.obj \
  $O\MethodId.obj \
  $O\MethodProps.obj \
  $O\MtTaskPool.obj \
  $O\OffsetStream.obj \
  $O\OutBuffer.obj \
  $O\OutMemStream.obj \
  $O\ProgressMt.obj \
  $O\ProgressUtils.obj \
  $O\PropId.obj \
  $O\StreamBinder.obj \
  $O\StreamObjects.obj \
  $O\StreamUtils.obj \
  $O\UniqBlocks.obj \
  $O\VirtThread.obj \

AR_OBJS = \
  $O\ApfsHandler.obj \
  $O\ApmHandler.obj \
  $O\ArHandler.obj \
  $O\ArjHandler.obj \
  $O\Base64Handler.obj \
  $O\Bz2Handler.obj \
  $O\ComHandler.obj \
  $O\CpioHandler.obj \
  $O\CramfsHandler.obj \
  $O\DeflateProps.obj \
  $O\DmgHandler.obj \
  $O\ElfHandler.obj \
  $O\ExtHandler.obj \
  $O\FatHandler.obj \
  $O\FlvHandler.obj \
  $O\GzHandler.obj \
  $O\GptHandler.obj \
  $O\HandlerCont.obj \
  $O\HfsHandler.obj \
  $O\IhexHandler.obj \
  $O\LpHandler.obj \
  $O\LzhHandler.obj \
  $O\LzmaHandler.obj \
  $O\MachoHandler.obj \
  $O\MbrHandler.obj \
  $O\MslzHandler.obj \
  $O\MubHandler.obj \
  $O\NtfsHandler.obj \
  $O\PeHandler.obj \
  $O\PpmdHandler.obj \
  $O\QcowHandler.obj \
  $O\RpmHandler.obj \
  $O\SparseHandler.obj \
  $O\SplitHandler.obj \
  $O\SquashfsHandler.obj \
  $O\SwfHandler.obj \
  $O\UefiHandler.obj \
  $O\VdiHandler.obj \
  $O\VhdHandler.obj \
  $O\VhdxHandler.obj \
  $O\VmdkHandler.obj \
  $O\XarHandler.obj \
  $O\XzHandler.obj \
  $O\ZHandler.obj \
  $O\ZstdHandler.obj \

AR_COMMON_OBJS = \
  $O\CoderMixer2.obj \
  $O\DummyOutStream.obj \
  $O\FindSignature.obj \
  $O\InStreamWithCRC.obj \
  $O\ItemNameUtils.obj \
  $O\MultiStream.obj \
  $O\OutStreamWithCRC.obj \
  $O\OutStreamWithSha1.obj \
  $O\HandlerOut.obj \
  $O\ParseProperties.obj \

7Z_OBJS = \
  $O\7zCompressionMode.obj \
  $O\7zDecode.obj \
  $O\7zEncode.obj \
  $O\7zExtract.obj \
  $O\7zExtractMt.obj \
  $O\7zFolderInStream.obj \
  $O\7zHandler.obj \
  $O\7zHandlerOut.obj \
  $O\7zHeader.obj \
  $O\7zIn.obj \
  $O\7zOut.obj \
  $O\7zProperties.obj \
  $O\7zSpecStream.obj \
  $O\7zUpdate.obj \
  $O\7zUpdateMt.obj \
  $O\7zRegister.obj \

CAB_OBJS = \
  $O\CabBlockInStream.obj \
  $O\CabHandler.obj \
  $O\CabHeader.obj \
  $O\CabIn.obj \
  $O\CabRegister.obj \

CHM_OBJS = \
  $O\ChmHandler.obj \
  $O\ChmIn.obj \

ISO_OBJS = \
  $O\IsoHandler.obj \
  $O\IsoHandlerOut.obj \
  $O\IsoHeader.obj \
  $O\IsoIn.obj \
  $O\IsoOut.obj \
  $O\IsoRegister.obj \

NSIS_OBJS = \
  $O\NsisDecode.obj \
  $O\NsisHandler.obj \
  $O\NsisIn.obj \
  $O\NsisRegister.obj \

RAR_OBJS = \
  $O\RarHandler.obj \
  $O\Rar5Handler.obj \

TAR_OBJS = \
  $O\TarHandler.obj \
  $O\TarHandlerOut.obj \
  $O\TarHeader.obj \
  $O\TarIn.obj \
  $O\TarOut.obj \
  $O\TarUpdate.obj \
  $O\TarRegister.obj \

UDF_OBJS = \
  $O\UdfHandler.obj \
  $O\UdfIn.obj \

WIM_OBJS = \
  $O\WimHandler.obj \
  $O\WimHandlerOut.obj \
  $O\WimIn.obj \
  $O\WimRegister.obj \

ZIP_OBJS = \
  $O\ZipAddCommon.obj \
  $O\ZipHandler.obj \
  $O\ZipExtractMt.obj \
  $O\ZipHandlerOut.obj \
  $O\ZipIn.obj \
  $O\ZipItem.obj \
  $O\ZipOut.obj \
  $O\ZipUpdate.obj \
  $O\ZipRegister.obj \

COMPRESS_OBJS = \
  $O\Bcj2Coder.obj \
  $O\Bcj2Register.obj \
  $O\BcjCoder.obj \
  $O\BcjRegister.obj \
  $O\BitlDecoder.obj \
  $O\BranchMisc.obj \
  $O\BranchRegister.obj \
  $O\ByteSwap.obj \
  $O\BZip2Crc.obj \
  $O\BZip2Decoder.obj \
  $O\BZip2Encoder.obj \
  $O\BZip2Register.obj \
  $O\CopyCoder.obj \
  $O\CopyRegister.obj \
  $O\Deflate64Register.obj \
  $O\DeflateDecoder.obj \
  $O\DeflateEncoder.obj \
  $O\DeflateRegister.obj \
  $O\DeltaFilter.obj \
  $O\ImplodeDecoder.obj \
  $O\LzfseDecoder.obj \
  $O\LzhDecoder.obj \
  $O\Lzma2Decoder.obj \
  $O\Lzma2Encoder.obj \
  $O\Lzma2Register.obj \
  $O\LzmaDecoder.obj \
  $O\LzmaEncoder.obj \
  $O\LzmaRegister.obj \
  $O\LzmsDecoder.obj \
  $O\LzOutWindow.obj \
  $O\LzxDecoder.obj \
  $O\PpmdDecoder.obj \
  $O\PpmdEncoder.obj \
  $O\PpmdRegister.obj \
  $O\PpmdZip.obj \
  $O\QuantumDecoder.obj \
  $O\Rar1Decoder.obj \
  $O\Rar2Decoder.obj \
  $O\Rar3Decoder.obj \
  $O\Rar3Vm.obj \
  $O\Rar5Decoder.obj \
  $O\RarCodecsRegister.obj \
  $O\ShrinkDecoder.obj \
  $O\XpressDecoder.obj \
  $O\XzDecoder.obj \
  $O\XzEncoder.obj \
  $O\ZlibDecoder.obj \
  $O\ZlibEncoder.obj \
  $O\ZDecoder.obj \
  $O\ZstdDecoder.obj \
  $O\ZstdCodecRegister.obj \
  $O\ZstdLibEncoder.obj \
  $O\FloatCoder.obj \
  $O\FloatRegister.obj \
  $O\BrotliCoder.obj \
  $O\BrotliRegister.obj \
  $O\BscCoder.obj \
  $O\BscRegister.obj \

CRYPTO_OBJS = \
  $O\7zAes.obj \
  $O\7zAesRegister.obj \
  $O\HmacSha1.obj \
  $O\HmacSha256.obj \
  $O\MyAes.obj \
  $O\MyAesReg.obj \
  $O\Pbkdf2HmacSha1.obj \
  $O\RandGen.obj \
  $O\Rar20Crypto.obj \
  $O\Rar5Aes.obj \
  $O\RarAes.obj \
  $O\WzAes.obj \
  $O\ZipCrypto.obj \
  $O\ZipStrong.obj \

C_OBJS = \
  $O\7zBuf2.obj \
  $O\7zStream.obj \
  $O\Alloc.obj \
  $O\Bcj2.obj \
  $O\Bcj2Enc.obj \
  $O\Blake2s.obj \
  $O\Bra.obj \
  $O\Bra86.obj \
  $O\BraIA64.obj \
  $O\BwtSort.obj \
  $O\CpuArch.obj \
  $O\Delta.obj \
  $O\HuffEnc.obj \
  $O\LzFind.obj \
  $O\LzFindMt.obj \
  $O\Lzma2Dec.obj \
  $O\Lzma2DecMt.obj \
  $O\Lzma2Enc.obj \
  $O\LzmaDec.obj \
  $O\LzmaEnc.obj \
  $O\Md5.obj \
  $O\MtCoder.obj \
  $O\MtDec.obj \
  $O\Ppmd7.obj \
  $O\Ppmd7Dec.obj \
  $O\Ppmd7aDec.obj \
  $O\Ppmd7Enc.obj \
  $O\Ppmd8.obj \
  $O\Ppmd8Dec.obj \
  $O\Ppmd8Enc.obj \
  $O\Sha3.obj \
  $O\Sha512.obj \
  $O\Sha512Opt.obj \
  $O\SwapBytes.obj \
  $O\Threads.obj \
  $O\Xxh64.obj \
  $O\Xz.obj \
  $O\XzDec.obj \
  $O\XzEnc.obj \
  $O\XzIn.obj \
  $O\ZstdDec.obj \
  $O\FloatPlane.obj \
  $O\FloatPlaneLz.obj \
  $O\FloatQuant.obj \

# Heptazip: third-party libraries (third_party/): the zstd encoder (method zstd in .7z,
# FLOAT) and the libdeflate compressor (Deflate, design 0012). The rules are in 7zip.mak.
CFLAGS = $(CFLAGS) -DZ7_ZSTD_LIB_ENCODER -DZ7_DEFLATE_LIBDEFLATE

HZ_LIB_OBJS = \
  $O\debug.obj \
  $O\entropy_common.obj \
  $O\error_private.obj \
  $O\fse_decompress.obj \
  $O\pool.obj \
  $O\threading.obj \
  $O\xxhash.obj \
  $O\zstd_common.obj \
  $O\fse_compress.obj \
  $O\hist.obj \
  $O\huf_compress.obj \
  $O\zstd_compress.obj \
  $O\zstd_compress_literals.obj \
  $O\zstd_compress_sequences.obj \
  $O\zstd_compress_superblock.obj \
  $O\zstd_double_fast.obj \
  $O\zstd_fast.obj \
  $O\zstd_lazy.obj \
  $O\zstd_ldm.obj \
  $O\zstd_opt.obj \
  $O\zstd_preSplit.obj \
  $O\zstdmt_compress.obj \
  $O\deflate_compress.obj \
  $O\libdeflate_utils.obj \
  $O\bsc_adler32.obj \
  $O\bsc_bwt.obj \
  $O\bsc_libsais.obj \
  $O\bsc_coder.obj \
  $O\bsc_qlfc.obj \
  $O\bsc_qlfc_model.obj \
  $O\bsc_detectors.obj \
  $O\bsc_preprocessing.obj \
  $O\bsc_libbsc.obj \
  $O\bsc_lzp.obj \
  $O\bsc_platform.obj \
  $O\bsc_st.obj \
  $O\br_common_constants.obj \
  $O\br_common_context.obj \
  $O\br_common_dictionary.obj \
  $O\br_common_platform.obj \
  $O\br_common_shared_dictionary.obj \
  $O\br_common_transform.obj \
  $O\br_dec_bit_reader.obj \
  $O\br_dec_decode.obj \
  $O\br_dec_huffman.obj \
  $O\br_dec_prefix.obj \
  $O\br_dec_state.obj \
  $O\br_dec_static_init.obj \
  $O\br_enc_backward_references.obj \
  $O\br_enc_backward_references_hq.obj \
  $O\br_enc_bit_cost.obj \
  $O\br_enc_block_splitter.obj \
  $O\br_enc_brotli_bit_stream.obj \
  $O\br_enc_cluster.obj \
  $O\br_enc_command.obj \
  $O\br_enc_compound_dictionary.obj \
  $O\br_enc_compress_fragment.obj \
  $O\br_enc_compress_fragment_two_pass.obj \
  $O\br_enc_dictionary_hash.obj \
  $O\br_enc_encode.obj \
  $O\br_enc_encoder_dict.obj \
  $O\br_enc_entropy_encode.obj \
  $O\br_enc_fast_log.obj \
  $O\br_enc_histogram.obj \
  $O\br_enc_literal_cost.obj \
  $O\br_enc_memory.obj \
  $O\br_enc_metablock.obj \
  $O\br_enc_static_dict.obj \
  $O\br_enc_static_dict_lut.obj \
  $O\br_enc_static_init.obj \
  $O\br_enc_utf8_util.obj \

!include "../../Aes.mak"
!include "../../Crc.mak"
!include "../../Crc64.mak"
!include "../../LzFindOpt.mak"
!include "../../LzmaDec.mak"
!include "../../Sha1.mak"
!include "../../Sha256.mak"
!include "../../Sort.mak"
