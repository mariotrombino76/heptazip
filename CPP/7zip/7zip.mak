OBJS = \
  $O\StdAfx.obj \
  $(CURRENT_OBJS) \
  $(COMMON_OBJS) \
  $(WIN_OBJS) \
  $(WIN_CTRL_OBJS) \
  $(7ZIP_COMMON_OBJS) \
  $(UI_COMMON_OBJS) \
  $(AGENT_OBJS) \
  $(CONSOLE_OBJS) \
  $(EXPLORER_OBJS) \
  $(FM_OBJS) \
  $(GUI_OBJS) \
  $(AR_COMMON_OBJS) \
  $(AR_OBJS) \
  $(7Z_OBJS) \
  $(CAB_OBJS) \
  $(CHM_OBJS) \
  $(COM_OBJS) \
  $(ISO_OBJS) \
  $(NSIS_OBJS) \
  $(RAR_OBJS) \
  $(TAR_OBJS) \
  $(UDF_OBJS) \
  $(WIM_OBJS) \
  $(ZIP_OBJS) \
  $(COMPRESS_OBJS) \
  $(CRYPTO_OBJS) \
  $(C_OBJS) \
  $(HZ_LIB_OBJS) \
  $(ASM_OBJS) \
  $O\resource.res \

!include "../../../Build.mak"

# MAK_SINGLE_FILE = 1

!IFDEF MAK_SINGLE_FILE

!IFDEF CURRENT_OBJS
$(CURRENT_OBJS): ./$(*B).cpp
	$(COMPL)
!ENDIF


!IFDEF COMMON_OBJS
$(COMMON_OBJS): ../../../Common/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF WIN_OBJS
$(WIN_OBJS): ../../../Windows/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF WIN_CTRL_OBJS
$(WIN_CTRL_OBJS): ../../../Windows/Control/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF 7ZIP_COMMON_OBJS
$(7ZIP_COMMON_OBJS): ../../Common/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF AR_OBJS
$(AR_OBJS): ../../Archive/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF AR_COMMON_OBJS
$(AR_COMMON_OBJS): ../../Archive/Common/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF 7Z_OBJS
$(7Z_OBJS): ../../Archive/7z/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF CAB_OBJS
$(CAB_OBJS): ../../Archive/Cab/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF CHM_OBJS
$(CHM_OBJS): ../../Archive/Chm/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF COM_OBJS
$(COM_OBJS): ../../Archive/Com/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF ISO_OBJS
$(ISO_OBJS): ../../Archive/Iso/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF NSIS_OBJS
$(NSIS_OBJS): ../../Archive/Nsis/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF RAR_OBJS
$(RAR_OBJS): ../../Archive/Rar/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF TAR_OBJS
$(TAR_OBJS): ../../Archive/Tar/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF UDF_OBJS
$(UDF_OBJS): ../../Archive/Udf/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF WIM_OBJS
$(WIM_OBJS): ../../Archive/Wim/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF ZIP_OBJS
$(ZIP_OBJS): ../../Archive/Zip/$(*B).cpp
	$(COMPL) $(ZIP_FLAGS)
!ENDIF

!IFDEF COMPRESS_OBJS
$(COMPRESS_OBJS): ../../Compress/$(*B).cpp
	$(COMPL_O2)
!ENDIF

!IFDEF CRYPTO_OBJS
$(CRYPTO_OBJS): ../../Crypto/$(*B).cpp
	$(COMPL_O2)
!ENDIF

!IFDEF UI_COMMON_OBJS
$(UI_COMMON_OBJS): ../../UI/Common/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF AGENT_OBJS
$(AGENT_OBJS): ../../UI/Agent/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF CONSOLE_OBJS
$(CONSOLE_OBJS): ../../UI/Console/$(*B).cpp
	$(COMPL) $(CONSOLE_VARIANT_FLAGS)
!ENDIF

!IFDEF EXPLORER_OBJS
$(EXPLORER_OBJS): ../../UI/Explorer/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF FM_OBJS
$(FM_OBJS): ../../UI/FileManager/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF GUI_OBJS
$(GUI_OBJS): ../../UI/GUI/$(*B).cpp
	$(COMPL)
!ENDIF

!IFDEF C_OBJS
$(C_OBJS): ../../../../C/$(*B).c
	$(COMPL_O2)
!ENDIF


!ELSE

{.}.cpp{$O}.obj::
	$(COMPLB)
{../../../Common}.cpp{$O}.obj::
	$(COMPLB)
{../../../Windows}.cpp{$O}.obj::
	$(COMPLB)
{../../../Windows/Control}.cpp{$O}.obj::
	$(COMPLB)
{../../Common}.cpp{$O}.obj::
	$(COMPLB)

{../../UI/Common}.cpp{$O}.obj::
	$(COMPLB)
{../../UI/Agent}.cpp{$O}.obj::
	$(COMPLB)
{../../UI/Console}.cpp{$O}.obj::
	$(COMPLB) $(CONSOLE_VARIANT_FLAGS)
{../../UI/Explorer}.cpp{$O}.obj::
	$(COMPLB)
{../../UI/FileManager}.cpp{$O}.obj::
	$(COMPLB)
{../../UI/GUI}.cpp{$O}.obj::
	$(COMPLB)


{../../Archive}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Common}.cpp{$O}.obj::
	$(COMPLB)

{../../Archive/7z}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Cab}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Chm}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Com}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Iso}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Nsis}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Rar}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Tar}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Udf}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Wim}.cpp{$O}.obj::
	$(COMPLB)
{../../Archive/Zip}.cpp{$O}.obj::
	$(COMPLB) $(ZIP_FLAGS)

{../../Compress}.cpp{$O}.obj::
	$(COMPLB_O2)
{../../Crypto}.cpp{$O}.obj::
	$(COMPLB_O2)
{../../../../C}.c{$O}.obj::
	$(CCOMPLB)

!ENDIF

# Heptazip: third-party libraries (third_party/, HZ_LIB_OBJS of Format7zF/Arc.mak),
# compiled without warnings: the C flags without -Wall and -WX (with them, cl stops on
# the warnings of -Wall even after -w), plus -w
!IFDEF HZ_LIB_OBJS
HZ_CFLAGS_NO_WX = $(CFLAGS_C_ALL:-WX=)
HZ_CFLAGS = $(HZ_CFLAGS_NO_WX:-Wall=) -w
HZ_ZSTD_LIB_FLAGS = -DZSTD_MULTITHREAD -DZSTD_DISABLE_ASM -DXXH_NAMESPACE=ZSTD_
{../../../../third_party/zstd/lib/common}.c{$O}.obj::
	$(CC) $(HZ_CFLAGS) $(HZ_ZSTD_LIB_FLAGS) $<
{../../../../third_party/zstd/lib/compress}.c{$O}.obj::
	$(CC) $(HZ_CFLAGS) $(HZ_ZSTD_LIB_FLAGS) $<
$O\deflate_compress.obj: ../../../../third_party/libdeflate/lib/deflate_compress.c
	$(CC) $(HZ_CFLAGS) $**
$O\libdeflate_utils.obj: ../../../../third_party/libdeflate/lib/utils.c
	$(CC) $(HZ_CFLAGS) -Fo$O\libdeflate_utils.obj $**
$O\br_common_constants.obj: ../../../../third_party/brotli/c/common/constants.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_common_constants.obj $**
$O\br_common_context.obj: ../../../../third_party/brotli/c/common/context.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_common_context.obj $**
$O\br_common_dictionary.obj: ../../../../third_party/brotli/c/common/dictionary.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_common_dictionary.obj $**
$O\br_common_platform.obj: ../../../../third_party/brotli/c/common/platform.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_common_platform.obj $**
$O\br_common_shared_dictionary.obj: ../../../../third_party/brotli/c/common/shared_dictionary.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_common_shared_dictionary.obj $**
$O\br_common_transform.obj: ../../../../third_party/brotli/c/common/transform.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_common_transform.obj $**
$O\br_dec_bit_reader.obj: ../../../../third_party/brotli/c/dec/bit_reader.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_dec_bit_reader.obj $**
$O\br_dec_decode.obj: ../../../../third_party/brotli/c/dec/decode.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_dec_decode.obj $**
$O\br_dec_huffman.obj: ../../../../third_party/brotli/c/dec/huffman.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_dec_huffman.obj $**
$O\br_dec_prefix.obj: ../../../../third_party/brotli/c/dec/prefix.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_dec_prefix.obj $**
$O\br_dec_state.obj: ../../../../third_party/brotli/c/dec/state.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_dec_state.obj $**
$O\br_dec_static_init.obj: ../../../../third_party/brotli/c/dec/static_init.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_dec_static_init.obj $**
$O\br_enc_backward_references.obj: ../../../../third_party/brotli/c/enc/backward_references.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_backward_references.obj $**
$O\br_enc_backward_references_hq.obj: ../../../../third_party/brotli/c/enc/backward_references_hq.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_backward_references_hq.obj $**
$O\br_enc_bit_cost.obj: ../../../../third_party/brotli/c/enc/bit_cost.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_bit_cost.obj $**
$O\br_enc_block_splitter.obj: ../../../../third_party/brotli/c/enc/block_splitter.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_block_splitter.obj $**
$O\br_enc_brotli_bit_stream.obj: ../../../../third_party/brotli/c/enc/brotli_bit_stream.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_brotli_bit_stream.obj $**
$O\br_enc_cluster.obj: ../../../../third_party/brotli/c/enc/cluster.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_cluster.obj $**
$O\br_enc_command.obj: ../../../../third_party/brotli/c/enc/command.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_command.obj $**
$O\br_enc_compound_dictionary.obj: ../../../../third_party/brotli/c/enc/compound_dictionary.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_compound_dictionary.obj $**
$O\br_enc_compress_fragment.obj: ../../../../third_party/brotli/c/enc/compress_fragment.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_compress_fragment.obj $**
$O\br_enc_compress_fragment_two_pass.obj: ../../../../third_party/brotli/c/enc/compress_fragment_two_pass.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_compress_fragment_two_pass.obj $**
$O\br_enc_dictionary_hash.obj: ../../../../third_party/brotli/c/enc/dictionary_hash.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_dictionary_hash.obj $**
$O\br_enc_encode.obj: ../../../../third_party/brotli/c/enc/encode.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_encode.obj $**
$O\br_enc_encoder_dict.obj: ../../../../third_party/brotli/c/enc/encoder_dict.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_encoder_dict.obj $**
$O\br_enc_entropy_encode.obj: ../../../../third_party/brotli/c/enc/entropy_encode.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_entropy_encode.obj $**
$O\br_enc_fast_log.obj: ../../../../third_party/brotli/c/enc/fast_log.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_fast_log.obj $**
$O\br_enc_histogram.obj: ../../../../third_party/brotli/c/enc/histogram.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_histogram.obj $**
$O\br_enc_literal_cost.obj: ../../../../third_party/brotli/c/enc/literal_cost.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_literal_cost.obj $**
$O\br_enc_memory.obj: ../../../../third_party/brotli/c/enc/memory.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_memory.obj $**
$O\br_enc_metablock.obj: ../../../../third_party/brotli/c/enc/metablock.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_metablock.obj $**
$O\br_enc_static_dict.obj: ../../../../third_party/brotli/c/enc/static_dict.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_static_dict.obj $**
$O\br_enc_static_dict_lut.obj: ../../../../third_party/brotli/c/enc/static_dict_lut.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_static_dict_lut.obj $**
$O\br_enc_static_init.obj: ../../../../third_party/brotli/c/enc/static_init.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_static_init.obj $**
$O\br_enc_utf8_util.obj: ../../../../third_party/brotli/c/enc/utf8_util.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/brotli/c/include -Fo$O\br_enc_utf8_util.obj $**
$O\BrotliCoder.obj: ../../Compress/BrotliCoder.cpp
	$(COMPL_O2) -I../../../../third_party/brotli/c/include
# libbsc (method BWT, design 0016): C++ and C sources, no OpenMP
HZ_BSC_DIR = ../../../../third_party/libbsc/libbsc
$O\bsc_adler32.obj: $(HZ_BSC_DIR)/adler32/adler32.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_adler32.obj $**
$O\bsc_bwt.obj: $(HZ_BSC_DIR)/bwt/bwt.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_bwt.obj $**
$O\bsc_libsais.obj: $(HZ_BSC_DIR)/bwt/libsais/libsais.c
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_libsais.obj $**
$O\bsc_coder.obj: $(HZ_BSC_DIR)/coder/coder.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_coder.obj $**
$O\bsc_qlfc.obj: $(HZ_BSC_DIR)/coder/qlfc/qlfc.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_qlfc.obj $**
$O\bsc_qlfc_model.obj: $(HZ_BSC_DIR)/coder/qlfc/qlfc_model.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_qlfc_model.obj $**
$O\bsc_detectors.obj: $(HZ_BSC_DIR)/filters/detectors.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_detectors.obj $**
$O\bsc_preprocessing.obj: $(HZ_BSC_DIR)/filters/preprocessing.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_preprocessing.obj $**
$O\bsc_libbsc.obj: $(HZ_BSC_DIR)/libbsc/libbsc.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_libbsc.obj $**
$O\bsc_lzp.obj: $(HZ_BSC_DIR)/lzp/lzp.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_lzp.obj $**
$O\bsc_platform.obj: $(HZ_BSC_DIR)/platform/platform.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_platform.obj $**
$O\bsc_st.obj: $(HZ_BSC_DIR)/st/st.cpp
	$(CC) $(HZ_CFLAGS) -I../../../../third_party/libbsc -DLIBBSC_NO_UNALIGNED_ACCESS -Fo$O\bsc_st.obj $**
$O\BscCoder.obj: ../../Compress/BscCoder.cpp
	$(COMPL_O2) -I../../../../third_party/libbsc
!ENDIF

!include "Asm.mak"
