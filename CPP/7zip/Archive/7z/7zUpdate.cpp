// 7zUpdate.cpp

#include "StdAfx.h"

#include "../../../../C/CpuArch.h"

#include "../../../Common/MyLinux.h"
#include "../../../Common/StringToInt.h"
#include "../../../Common/Wildcard.h"

#ifndef Z7_ST
#include <atomic>
#include <deque>
#include <memory>
#include <vector>
#endif

#include "../../Common/CreateCoder.h"
#include "../../Common/LimitedStreams.h"
#include "../../Common/ProgressUtils.h"
#include "../../Common/StreamObjects.h"
#include "../../Common/StreamUtils.h"
#ifndef Z7_ST
#include "../../Common/MtTaskPool.h"
#endif

#include "../../Compress/CopyCoder.h"

#include "../Common/ItemNameUtils.h"

#include "7zDecode.h"
#include "7zEncode.h"
#include "7zFolderInStream.h"
#include "7zHandler.h"
#include "7zOut.h"
#include "7zUpdate.h"
#include "7zUpdateMt.h"
#ifndef Z7_ST
#include "7zExtractMt.h" // CMemoryBudget
#endif

namespace NArchive {
namespace N7z {

#define k_X86 k_BCJ

/* 7zip-modern: group of the files with AI model weights (safetensors, GGUF in F16/BF16/F32),
   compressed with the FLOAT method (design 0008) instead of LZMA/LZMA2. It is a marker of
   CFilterMode::Id only (not a method ID: it is never written to the archive), handled
   before MakeExeMethod(). */
static const UInt32 k_FLOAT_FilterGroup = 0x3F70F10A;

struct CFilterMode
{
  UInt32 Id;
  UInt32 Delta;  // required File Size alignment, if Id is not k_Delta.
                 // (Delta == 0) means unknown alignment
  UInt32 Offset; // for k_ARM64 / k_RISCV
  // UInt32 AlignSizeOpt; // for k_ARM64

  void ClearFilterMode()
  {
    Id = 0;
    Delta = 0;
    Offset = 0;
    // AlignSizeOpt = 0;
  }

  // it sets Delta as Align value, if Id is exe filter
  // in another cases it sets Delta = 0, that
  void SetDelta()
  {
    if (Id == k_IA64)
      Delta = 16;
    else if (Id == k_ARM64 || Id == k_ARM || Id == k_PPC || Id == k_SPARC)
      Delta = 4;
    else if (Id == k_ARMT || Id == k_RISCV)
      Delta = 2;
    else if (Id == k_BCJ || Id == k_BCJ2)
      Delta = 1; // do we need it?
    else
      Delta = 0;
  }
};


/* ---------- PE ---------- */

#define MZ_SIG 0x5A4D

#define PE_SIG 0x00004550
#define PE_OptHeader_Magic_32 0x10B
#define PE_OptHeader_Magic_64 0x20B
// #define PE_SectHeaderSize 40
// #define PE_SECT_EXECUTE 0x20000000

static int Parse_EXE(const Byte *buf, size_t size, CFilterMode *filterMode)
{
  if (size < 512 || GetUi16(buf) != MZ_SIG)
    return 0;

  const Byte *p;
  UInt32 peOffset, optHeaderSize, filterId;

  peOffset = GetUi32(buf + 0x3C);
  if (peOffset >= 0x1000 || peOffset + 512 > size || (peOffset & 7) != 0)
    return 0;
  p = buf + peOffset;
  if (GetUi32(p) != PE_SIG)
    return 0;
  p += 4;
  
  const unsigned machine = GetUi16(p);

  switch (machine)
  {
    case 0x014C:
    case 0x8664:  filterId = k_X86; break;
    case 0xAA64:  filterId = k_ARM64; break;
    
    /*
    IMAGE_FILE_MACHINE_ARM   0x01C0  // ARM LE
    IMAGE_FILE_MACHINE_THUMB 0x01C2  // ARM Thumb / Thumb-2 LE
    IMAGE_FILE_MACHINE_ARMNT 0x01C4  // ARM Thumb-2, LE
    Note: We use ARM filter for 0x01C2. (WinCE 5 - 0x01C2) files mostly contain ARM code (not Thumb/Thumb-2).
    */

    case 0x01C0:                            // WinCE old
    case 0x01C2:  filterId = k_ARM; break;  // WinCE new
    case 0x01C4:  filterId = k_ARMT; break; // WinRT

    case 0x5032:  // RISCV32
    case 0x5064:  // RISCV64
    // case 0x5128:  // RISCV128
                  filterId = k_RISCV; break;

    case 0x0200:  filterId = k_IA64; break;
    default:  return 0;
  }

  const UInt32 numSections = GetUi16(p + 2);
  optHeaderSize = GetUi16(p + 16);
  if (optHeaderSize > (1 << 10))
    return 0;

  p += 20; /* headerSize */

  switch (GetUi16(p))
  {
    case PE_OptHeader_Magic_32:
    case PE_OptHeader_Magic_64:
      break;
    default:
      return 0;
  }

    // Windows exe file sizes are not aligned for 4 KiB.
    // So we can't use (CFilterMode::Offset != 0) in solid archives.
    // So we just don't set Offset here.
#define NUM_SCAN_SECTIONS_MAX (1 << 6)
// #define EXE_SECTION_OFFSET_MAX (1 << 27)
// #define EXE_SECTION_SIZE_MIN (1 << 8)
// #define EXE_SECTION_SIZE_MAX (1 << 27)
#define PE_SectHeaderSize 40
// #define PE_SECT_EXECUTE 0x20000000

/*
  if (numSections > NUM_SCAN_SECTIONS_MAX)
    return 0;
*/

  if ((size_t)(p - buf) + optHeaderSize <= size)
  {
  p += optHeaderSize;
/*
  // UInt32 numExeSections = 0;
  // bool execute_finded = false;
  // UInt32 sect_va = 0;
  // UInt32 sect_size = 0;
  // UInt32 sect_offset = 0;
*/
  if (numSections <= NUM_SCAN_SECTIONS_MAX)
  if (machine == 0x8664)
  for (UInt32 i = 0; i < numSections
        ; i++, p += PE_SectHeaderSize)
  {
    // UInt32 characts, rawSize, offset;
    if ((UInt32)(p - buf) + PE_SectHeaderSize > size)
    {
      // return 0;
      break;
    }
    if (memcmp(p, ".a64xrm", 8) == 0)
    {
      // ARM64EC
      filterId = k_ARM64;
      break;
    }
/*
    rawSize = GetUi32(p + 16);
    offset = GetUi32(p + 20);
    characts = GetUi32(p + 36);
    if (rawSize >= EXE_SECTION_SIZE_MIN &&
        rawSize <= EXE_SECTION_SIZE_MAX &&
        offset <= EXE_SECTION_OFFSET_MAX &&
        // offset < limit &&
        offset > 0)
    {
      if ((characts & PE_SECT_EXECUTE) != 0)
      {
        // execute_finded = true;
        // sect_va = GetUi32(p + 12);
        // sect_size = rawSize;
        // sect_offset = offset;
        break;
      }
    }
*/
  }
  }

  /*
  filterMode->Offset = 0;
  if (filterId == k_ARM64)
  {
    // filterMode->AlignSizeOpt = (1 << 12);
    // const UInt32 offs = (sect_va - sect_offset) & 0xFFF;
    // if (offs != 0)
    // filterMode->Offset = offs; // change it
  }
  */
  filterMode->Id = filterId;
  return 1;
}


/*
  Filters don't improve the compression ratio for relocatable object files (".o").
  But we can get compression ratio gain, if we compress object
  files and executables in same solid block.
  So we use filters for relocatable object files (".o"):
*/
// #define Z7_7Z_CREATE_ARC_DISABLE_FILTER_FOR_OBJ

/* ---------- ELF ---------- */

#define ELF_SIG 0x464C457F

#define ELF_CLASS_32  1
#define ELF_CLASS_64  2

#define ELF_DATA_2LSB 1
#define ELF_DATA_2MSB 2

static UInt16 Get16(const Byte *p, BoolInt be) { if (be) return (UInt16)GetBe16(p); return (UInt16)GetUi16(p); }
static UInt32 Get32(const Byte *p, BoolInt be) { if (be) return GetBe32(p); return GetUi32(p); }
// static UInt64 Get64(const Byte *p, BoolInt be) { if (be) return GetBe64(p); return GetUi64(p); }

static int Parse_ELF(const Byte *buf, size_t size, CFilterMode *filterMode)
{
  BoolInt /* is32, */ be;
  UInt32 filterId;

  if (size < 512 || buf[6] != 1) /* ver */
    return 0;

  if (GetUi32(buf) != ELF_SIG)
    return 0;

  switch (buf[4])
  {
    case ELF_CLASS_32: /* is32 = True; */ break;
    case ELF_CLASS_64: /* is32 = False; */ break;
    default: return 0;
  }

  switch (buf[5])
  {
    case ELF_DATA_2LSB: be = False; break;
    case ELF_DATA_2MSB: be = True; break;
    default: return 0;
  }

#ifdef Z7_7Z_CREATE_ARC_DISABLE_FILTER_FOR_OBJ
#define ELF_ET_REL  1
  if (Get16(buf + 0x10, be) == ELF_ET_REL)
    return 0;
#endif

  switch (Get16(buf + 0x12, be))
  {
    case 3:
    case 6:
    case 62: filterId = k_X86; break;
    case 2:
    case 18:
    case 43: filterId = k_SPARC; break;
    case 20:
    case 21: if (!be) return 0; filterId = k_PPC; break;
    case 40:  if (be) return 0; filterId = k_ARM; break;
    case 183: if (be) return 0; filterId = k_ARM64; break;
    case 243: if (be) return 0; filterId = k_RISCV; break;

    /* Some IA-64 ELF executables have size that is not aligned for 16 bytes.
       So we don't use IA-64 filter for IA-64 ELF */
    // case 50: if ( be) return 0; filterId = k_IA64; break;

    default: return 0;
  }

  filterMode->Id = filterId;
  return 1;
}



/* ---------- Mach-O ---------- */

#define MACH_SIG_BE_32 0xCEFAEDFE
#define MACH_SIG_BE_64 0xCFFAEDFE
#define MACH_SIG_LE_32 0xFEEDFACE
#define MACH_SIG_LE_64 0xFEEDFACF

#define MACH_ARCH_ABI64 (1 << 24)
#define MACH_MACHINE_386 7
#define MACH_MACHINE_ARM 12
#define MACH_MACHINE_SPARC 14
#define MACH_MACHINE_PPC 18
#define MACH_MACHINE_PPC64 (MACH_ARCH_ABI64 | MACH_MACHINE_PPC)
#define MACH_MACHINE_AMD64 (MACH_ARCH_ABI64 | MACH_MACHINE_386)
#define MACH_MACHINE_ARM64 (MACH_ARCH_ABI64 | MACH_MACHINE_ARM)

static unsigned Parse_MACH(const Byte *buf, size_t size, CFilterMode *filterMode)
{
  UInt32 filterId, numCommands, commandsSize;

  if (size < 512)
    return 0;

  BoolInt /* mode64, */ be;
  switch (GetUi32(buf))
  {
    case MACH_SIG_BE_32: /* mode64 = False; */ be = True; break;
    case MACH_SIG_BE_64: /* mode64 = True;  */ be = True; break;
    case MACH_SIG_LE_32: /* mode64 = False; */ be = False; break;
    case MACH_SIG_LE_64: /* mode64 = True;  */ be = False; break;
    default: return 0;
  }

#ifdef Z7_7Z_CREATE_ARC_DISABLE_FILTER_FOR_OBJ
#define MACH_TYPE_OBJECT 1
  if (Get32(buf + 0xC, be) == MACH_TYPE_OBJECT)
      return 0;
#endif

  switch (Get32(buf + 4, be))
  {
    case MACH_MACHINE_386:
    case MACH_MACHINE_AMD64: filterId = k_X86; break;
    case MACH_MACHINE_ARM:   if ( be) return 0; filterId = k_ARM; break;
    case MACH_MACHINE_SPARC: if (!be) return 0; filterId = k_SPARC; break;
    case MACH_MACHINE_PPC:
    case MACH_MACHINE_PPC64: if (!be) return 0; filterId = k_PPC; break;
    case MACH_MACHINE_ARM64: if ( be) return 0; filterId = k_ARM64; break;
    default: return 0;
  }

  numCommands = Get32(buf + 0x10, be);
  commandsSize = Get32(buf + 0x14, be);

  if (commandsSize > (1 << 24) || numCommands > (1 << 18))
    return 0;

  filterMode->Id = filterId;
  return 1;
}


/* ---------- WAV ---------- */

#define WAV_SUBCHUNK_fmt  0x20746D66
#define WAV_SUBCHUNK_data 0x61746164

#define RIFF_SIG 0x46464952

static BoolInt Parse_WAV(const Byte *buf, size_t size, CFilterMode *filterMode)
{
  UInt32 subChunkSize, pos;
  if (size < 0x2C)
    return False;

  if (GetUi32(buf + 0) != RIFF_SIG ||
      GetUi32(buf + 8) != 0x45564157 || // WAVE
      GetUi32(buf + 0xC) != WAV_SUBCHUNK_fmt)
    return False;
  subChunkSize = GetUi32(buf + 0x10);
  /* [0x14 = format] = 1 (PCM) */
  if (subChunkSize < 0x10 || subChunkSize > 0x12 || GetUi16(buf + 0x14) != 1)
    return False;
  
  const unsigned numChannels = GetUi16(buf + 0x16);
  const unsigned bitsPerSample = GetUi16(buf + 0x22);
  if ((bitsPerSample & 0x7) != 0)
    return False;
  const UInt32 delta = (UInt32)numChannels * (bitsPerSample >> 3);
  if (delta == 0 || delta > 256)
    return False;

  pos = 0x14 + subChunkSize;

  const int kNumSubChunksTests = 10;
  // Do we need to scan more than 3 sub-chunks?
  for (int i = 0; i < kNumSubChunksTests; i++)
  {
    if (pos + 8 > size)
      return False;
    subChunkSize = GetUi32(buf + pos + 4);
    if (GetUi32(buf + pos) == WAV_SUBCHUNK_data)
    {
      filterMode->Id = k_Delta;
      filterMode->Delta = delta;
      return True;
    }
    if (subChunkSize > (1 << 16))
      return False;
    pos += subChunkSize + 8;
  }
  return False;
}


/* ---------- AI model weights (7zip-modern, design 0008) ---------- */

static size_t CountSubstr(const Byte *buf, size_t size, const char *s)
{
  const size_t len = strlen(s);
  size_t num = 0;
  for (size_t i = 0; i + len <= size; i++)
    if (buf[i] == (Byte)s[0] && memcmp(buf + i, s, len) == 0)
      num++;
  return num;
}

/* safetensors: u64 size of the JSON header, then the header ({"name":{"dtype":"BF16",
   "shape":[..],"data_offsets":[..]},...}), then the tensors. FLOAT is chosen if the
   float dtypes it codes (BF16, F16, F32) are more frequent than the others (integer,
   FP8, F64: quantized models stay with LZMA2) in the part of the header we read. */
static BoolInt Parse_Safetensors(const Byte *buf, size_t size, CFilterMode *filterMode)
{
  if (size < 16)
    return False;
  const UInt64 headerSize = GetUi64(buf);
  if (headerSize < 2 || headerSize > ((UInt64)100 << 20) || buf[8] != '{')
    return False;
  const size_t scan = (size_t)MyMin((UInt64)size, 8 + headerSize);
  const size_t numFloat =
        CountSubstr(buf, scan, "\"BF16\"")
      + CountSubstr(buf, scan, "\"F16\"")
      + CountSubstr(buf, scan, "\"F32\"");
  static const char * const kOthers[] = { "\"F64\"", "\"I8\"", "\"U8\"", "\"I16\"",
      "\"U16\"", "\"I32\"", "\"U32\"", "\"I64\"", "\"U64\"", "\"BOOL\"", "\"F8_" };
  size_t numOther = 0;
  for (unsigned i = 0; i < Z7_ARRAY_SIZE(kOthers); i++)
    numOther += CountSubstr(buf, scan, kOthers[i]);
  if (numFloat == 0 || numFloat <= numOther)
    return False;
  filterMode->Id = k_FLOAT_FilterGroup;
  return True;
}

/* GGUF (llama.cpp): "GGUF", version 2 or 3, then key-value metadata. The key
   general.file_type (u32 value type 4) gives the type of most tensors: 0 = F32,
   1 = F16, 32 = BF16 go to FLOAT; quantized types (Q4_K, Q8_0...) stay with LZMA2. If
   the key is not in the part we read, the file stays with LZMA2. */
static BoolInt Parse_Gguf(const Byte *buf, size_t size, CFilterMode *filterMode)
{
  if (size < 24 || GetUi32(buf) != 0x46554747)   // "GGUF"
    return False;
  const UInt32 version = GetUi32(buf + 4);
  if (version != 2 && version != 3)
    return False;
  static const char kKey[] = "general.file_type";
  const size_t keyLen = sizeof(kKey) - 1;
  for (size_t i = 8; i + 8 + keyLen + 8 <= size; i++)
  {
    // the key string is stored as u64 length + bytes
    if (GetUi64(buf + i) != keyLen || memcmp(buf + i + 8, kKey, keyLen) != 0)
      continue;
    const Byte *v = buf + i + 8 + keyLen;
    if (GetUi32(v) != 4)   // GGUF_TYPE_UINT32
      return False;
    const UInt32 fileType = GetUi32(v + 4);
    if (fileType != 0 && fileType != 1 && fileType != 32)
      return False;
    filterMode->Id = k_FLOAT_FilterGroup;
    return True;
  }
  return False;
}


/*
  filterMode->Delta will be set as:
    = delta value : [1, 256] : for k_Delta
    = 0 for another filters (branch filters)
*/
static BoolInt ParseFile(const Byte *buf, size_t size, CFilterMode *filterMode)
{
  filterMode->ClearFilterMode();

  if (Parse_EXE(buf, size, filterMode)) return True;
  if (Parse_ELF(buf, size, filterMode)) return True;
  if (Parse_MACH(buf, size, filterMode)) return True;
  if (Parse_Safetensors(buf, size, filterMode)) return True;
  if (Parse_Gguf(buf, size, filterMode)) return True;
  return Parse_WAV(buf, size, filterMode);
}




struct CFilterMode2: public CFilterMode
{
  bool Encrypted;
  unsigned GroupIndex;
  
  void Construct()
  {
    ClearFilterMode();
    Encrypted = false;
  }

  int Compare(const CFilterMode2 &m) const
  {
    if (!Encrypted)
    {
      if (m.Encrypted)
        return -1;
    }
    else if (!m.Encrypted)
      return 1;
    
    const UInt32 id1 = Id;
    const UInt32 id2 = m.Id;
    /*
    // we can change the order to place k_ARM64 files close to another exe files
    if (id1 <= k_SPARC &&
        id2 <= k_SPARC)
    {
      #define k_ARM64_FOR_SORT 0x3030901
      if (id1 == k_ARM64) id1 = k_ARM64_FOR_SORT;
      if (id2 == k_ARM64) id2 = k_ARM64_FOR_SORT;
    }
    */
    if (id1 < id2) return -1;
    if (id1 > id2) return 1;

    if (Delta < m.Delta) return -1;
    if (Delta > m.Delta) return 1;

    if (Offset < m.Offset) return -1;
    if (Offset > m.Offset) return 1;

    /* we don't go here, because GetGroup()
       and operator ==(const CFilterMode2 &m)
       add only unique CFilterMode2:: { Id, Delta, Offset, Encrypted } items.
    */
    /*
    if (GroupIndex < m.GroupIndex) return -1;
    if (GroupIndex > m.GroupIndex) return 1;
    */
    return 0;
  }
  
  bool operator ==(const CFilterMode2 &m) const
  {
    return Id == m.Id
        && Delta == m.Delta
        && Offset == m.Offset
        && Encrypted == m.Encrypted;
  }
};

static unsigned GetGroup(CRecordVector<CFilterMode2> &filters, const CFilterMode2 &m)
{
  unsigned i;
  for (i = 0; i < filters.Size(); i++)
  {
    const CFilterMode2 &m2 = filters[i];
    if (m == m2)
      return i;
    /*
    if (m.Encrypted != m2.Encrypted)
    {
      if (!m.Encrypted)
        break;
      continue;
    }
    
    if (m.Id < m2.Id)  break;
    if (m.Id != m2.Id) continue;

    if (m.Delta < m2.Delta) break;
    if (m.Delta != m2.Delta) continue;
    */
  }
  // filters.Insert(i, m);
  // return i;
  return filters.Add(m);
}

static inline bool Is86Filter(CMethodId m)
{
  return (m == k_BCJ || m == k_BCJ2);
}

static inline bool IsExeFilter(CMethodId m)
{
  switch (m)
  {
    case k_ARM64:
    case k_RISCV:
    case k_BCJ:
    case k_BCJ2:
    case k_ARM:
    case k_ARMT:
    case k_PPC:
    case k_SPARC:
    case k_IA64:
      return true;
    default: break;
  }
  return false;
}

static unsigned Get_FilterGroup_for_Folder(
    CRecordVector<CFilterMode2> &filters, const CFolderEx &f, bool extractFilter)
{
  CFilterMode2 m;
  m.Construct();
  // m.Id = 0;
  // m.Delta = 0;
  // m.Offset = 0;
  m.Encrypted = f.IsEncrypted();

  if (extractFilter)
  {
    const CCoderInfo &coder = f.Coders[f.UnpackCoder];
  
    if (coder.MethodID == k_Delta)
    {
      if (coder.Props.Size() == 1)
      {
        m.Delta = (unsigned)coder.Props[0] + 1;
        m.Id = k_Delta;
      }
    }
    else if (coder.MethodID == k_FLOAT && f.Coders.Size() == 1)
      m.Id = k_FLOAT_FilterGroup;
    else if (IsExeFilter(coder.MethodID))
    {
      m.Id = (UInt32)coder.MethodID;
      if (m.Id == k_BCJ2)
        m.Id = k_BCJ;
      m.SetDelta();
      if (m.Id == k_ARM64 ||
          m.Id == k_RISCV)
        if (coder.Props.Size() == 4)
          m.Offset = GetUi32(coder.Props);
    }
  }
  
  return GetGroup(filters, m);
}




static HRESULT WriteRange(IInStream *inStream, ISequentialOutStream *outStream,
    UInt64 position, UInt64 size, ICompressProgressInfo *progress)
{
  RINOK(InStream_SeekSet(inStream, position))
  CMyComPtr2_Create<ISequentialInStream, CLimitedSequentialInStream> streamSpec;
  streamSpec->SetStream(inStream);
  streamSpec->Init(size);
  CMyComPtr2_Create<ICompressCoder, NCompress::CCopyCoder> copyCoder;
  RINOK(copyCoder.Interface()->Code(streamSpec, outStream, NULL, NULL, progress))
  return (copyCoder->TotalSize == size ? S_OK : E_FAIL);
}

/*
unsigned CUpdateItem::GetExtensionPos() const
{
  int slashPos = Name.ReverseFind_PathSepar();
  int dotPos = Name.ReverseFind_Dot();
  if (dotPos <= slashPos)
    return Name.Len();
  return dotPos + 1;
}

UString CUpdateItem::GetExtension() const
{
  return Name.Ptr(GetExtensionPos());
}
*/

#define RINOZ(x) { const int _t_ = (x); if (_t_ != 0) return _t_; }

#define RINOZ_COMP(a, b) RINOZ(MyCompare(a, b))

/*
static int CompareBuffers(const CByteBuffer &a1, const CByteBuffer &a2)
{
  size_t c1 = a1.GetCapacity();
  size_t c2 = a2.GetCapacity();
  RINOZ_COMP(c1, c2);
  for (size_t i = 0; i < c1; i++)
    RINOZ_COMP(a1[i], a2[i]);
  return 0;
}

static int CompareCoders(const CCoderInfo &c1, const CCoderInfo &c2)
{
  RINOZ_COMP(c1.NumInStreams, c2.NumInStreams);
  RINOZ_COMP(c1.NumOutStreams, c2.NumOutStreams);
  RINOZ_COMP(c1.MethodID, c2.MethodID);
  return CompareBuffers(c1.Props, c2.Props);
}

static int CompareBonds(const CBond &b1, const CBond &b2)
{
  RINOZ_COMP(b1.InIndex, b2.InIndex);
  return MyCompare(b1.OutIndex, b2.OutIndex);
}

static int CompareFolders(const CFolder &f1, const CFolder &f2)
{
  int s1 = f1.Coders.Size();
  int s2 = f2.Coders.Size();
  RINOZ_COMP(s1, s2);
  int i;
  for (i = 0; i < s1; i++)
    RINOZ(CompareCoders(f1.Coders[i], f2.Coders[i]));
  s1 = f1.Bonds.Size();
  s2 = f2.Bonds.Size();
  RINOZ_COMP(s1, s2);
  for (i = 0; i < s1; i++)
    RINOZ(CompareBonds(f1.Bonds[i], f2.Bonds[i]));
  return 0;
}
*/

/*
static int CompareFiles(const CFileItem &f1, const CFileItem &f2)
{
  return CompareFileNames(f1.Name, f2.Name);
}
*/

struct CFolderRepack
{
  unsigned FolderIndex;
  CNum NumCopyFiles;
};

/*
static int CompareFolderRepacks(const CFolderRepack *p1, const CFolderRepack *p2, void *)
{
  int i1 = p1->FolderIndex;
  int i2 = p2->FolderIndex;
  // In that version we don't want to parse folders here, so we don't compare folders
  // probably it must be improved in future
  // const CDbEx &db = *(const CDbEx *)param;
  // RINOZ(CompareFolders(
  //     db.Folders[i1],
  //     db.Folders[i2]));

  return MyCompare(i1, i2);
  
  // RINOZ_COMP(
  //     db.NumUnpackStreamsVector[i1],
  //     db.NumUnpackStreamsVector[i2]);
  // if (db.NumUnpackStreamsVector[i1] == 0)
  //   return 0;
  // return CompareFiles(
  //     db.Files[db.FolderStartFileIndex[i1]],
  //     db.Files[db.FolderStartFileIndex[i2]]);
}
*/

/*
  we sort empty files and dirs in such order:
  - Dir.NonAnti   (name sorted)
  - File.NonAnti  (name sorted)
  - File.Anti     (name sorted)
  - Dir.Anti (reverse name sorted)
*/

static int CompareEmptyItems(const unsigned *p1, const unsigned *p2, void *param)
{
  const CObjectVector<CUpdateItem> &updateItems = *(const CObjectVector<CUpdateItem> *)param;
  const CUpdateItem &u1 = updateItems[*p1];
  const CUpdateItem &u2 = updateItems[*p2];
  // NonAnti < Anti
  if (u1.IsAnti != u2.IsAnti)
    return (u1.IsAnti ? 1 : -1);
  if (u1.IsDir != u2.IsDir)
  {
    // Dir.NonAnti < File < Dir.Anti
    if (u1.IsDir)
      return (u1.IsAnti ? 1 : -1);
    return (u2.IsAnti ? -1 : 1);
  }
  int n = CompareFileNames(u1.Name, u2.Name);
  return (u1.IsDir && u1.IsAnti) ? -n : n;
}

static const char * const g_Exts =
  " 7z xz lzma ace arc arj bz tbz bz2 tbz2 cab deb gz tgz ha lha lzh lzo lzx pak rar rpm sit zoo"
  " zip jar ear war msi"
  " 3gp avi mov mpeg mpg mpe wmv"
  " aac ape fla flac la mp3 m4a mp4 ofr ogg pac ra rm rka shn swa tta wv wma wav"
  " swf"
  " chm hxi hxs"
  " gif jpeg jpg jp2 png tiff  bmp ico psd psp"
  " awg ps eps cgm dxf svg vrml wmf emf ai md"
  " cad dwg pps key sxi"
  " max 3ds"
  " iso bin nrg mdf img pdi tar cpio xpi"
  " vfd vhd vud vmc vsv"
  " vmdk dsk nvram vmem vmsd vmsn vmss vmtm"
  " inl inc idl acf asa"
  " h hpp hxx c cpp cxx m mm go swift"
  " rc java cs rs pas bas vb cls ctl frm dlg def"
  " f77 f f90 f95"
  " asm s"
  " sql manifest dep"
  " mak clw csproj vcproj sln dsp dsw"
  " class"
  " bat cmd bash sh"
  " xml xsd xsl xslt hxk hxc htm html xhtml xht mht mhtml htw asp aspx css cgi jsp shtml"
  " awk sed hta js json php php3 php4 php5 phptml pl pm py pyo rb tcl ts vbs"
  " text txt tex ans asc srt reg ini doc docx mcw dot rtf hlp xls xlr xlt xlw ppt pdf"
  " sxc sxd sxi sxg sxw stc sti stw stm odt ott odg otg odp otp ods ots odf"
  " abw afp cwk lwp wpd wps wpt wrf wri"
  " abf afm bdf fon mgf otf pcf pfa snf ttf"
  " dbf mdb nsf ntf wdb db fdb gdb"
  " exe dll ocx vbx sfx sys tlb awx com obj lib out o so"
  " pdb pch idb ncb opt";

static unsigned GetExtIndex(const char *ext)
{
  unsigned extIndex = 1;
  const char *p = g_Exts;
  for (;;)
  {
    char c = *p++;
    if (c == 0)
      return extIndex;
    if (c == ' ')
      continue;
    unsigned pos = 0;
    for (;;)
    {
      char c2 = ext[pos++];
      if (c2 == 0 && (c == 0 || c == ' '))
        return extIndex;
      if (c != c2)
        break;
      c = *p++;
    }
    extIndex++;
    for (;;)
    {
      if (c == 0)
        return extIndex;
      if (c == ' ')
        break;
      c = *p++;
    }
  }
}

struct CRefItem
{
  const CUpdateItem *UpdateItem;
  UInt32 Index;
  unsigned ExtensionPos;
  unsigned NamePos;
  unsigned ExtensionIndex;
  
  void Construct(UInt32 index, const CUpdateItem &ui, bool sortByType)
  {
    UpdateItem = &ui;
    Index = index;
    ExtensionPos = 0;
    NamePos = 0;
    ExtensionIndex = 0;

    if (sortByType)
    {
      const int slashPos = ui.Name.ReverseFind_PathSepar();
      NamePos = (unsigned)(slashPos + 1);
      const int dotPos = ui.Name.ReverseFind_Dot();
      if (dotPos <= slashPos)
        ExtensionPos = ui.Name.Len();
      else
      {
        ExtensionPos = (unsigned)(dotPos + 1);
        if (ExtensionPos != ui.Name.Len())
        {
          AString s;
          for (unsigned pos = ExtensionPos;; pos++)
          {
            const wchar_t c = ui.Name[pos];
            if (c >= 0x80)
              break;
            if (c == 0)
            {
              ExtensionIndex = GetExtIndex(s);
              break;
            }
            s.Add_Char((char)MyCharLower_Ascii((char)c));
          }
        }
      }
    }
  }
};

struct CSortParam
{
  // const CObjectVector<CTreeFolder> *TreeFolders;
  bool SortByType;
};

/*
  we sort files in such order:
  - Dir.NonAnti   (name sorted)
  - alt streams
  - Dirs
  - Dir.Anti (reverse name sorted)
*/


static int CompareUpdateItems(const CRefItem *p1, const CRefItem *p2, void *param)
{
  const CRefItem &a1 = *p1;
  const CRefItem &a2 = *p2;
  const CUpdateItem &u1 = *a1.UpdateItem;
  const CUpdateItem &u2 = *a2.UpdateItem;

  /*
  if (u1.IsAltStream != u2.IsAltStream)
    return u1.IsAltStream ? 1 : -1;
  */
  
  // Actually there are no dirs that time. They were stored in other steps
  // So that code is unused?
  if (u1.IsDir != u2.IsDir)
    return u1.IsDir ? 1 : -1;
  if (u1.IsDir)
  {
    if (u1.IsAnti != u2.IsAnti)
      return (u1.IsAnti ? 1 : -1);
    int n = CompareFileNames(u1.Name, u2.Name);
    return -n;
  }
  
  // bool sortByType = *(bool *)param;
  const CSortParam *sortParam = (const CSortParam *)param;
  const bool sortByType = sortParam->SortByType;
  if (sortByType)
  {
    RINOZ_COMP(a1.ExtensionIndex, a2.ExtensionIndex)
    RINOZ(CompareFileNames(u1.Name.Ptr(a1.ExtensionPos), u2.Name.Ptr(a2.ExtensionPos)))
    RINOZ(CompareFileNames(u1.Name.Ptr(a1.NamePos), u2.Name.Ptr(a2.NamePos)))
    if (!u1.MTimeDefined && u2.MTimeDefined) return 1;
    if (u1.MTimeDefined && !u2.MTimeDefined) return -1;
    if (u1.MTimeDefined && u2.MTimeDefined) RINOZ_COMP(u1.MTime, u2.MTime)
    RINOZ_COMP(u1.Size, u2.Size)
  }
  /*
  int par1 = a1.UpdateItem->ParentFolderIndex;
  int par2 = a2.UpdateItem->ParentFolderIndex;
  const CTreeFolder &tf1 = (*sortParam->TreeFolders)[par1];
  const CTreeFolder &tf2 = (*sortParam->TreeFolders)[par2];

  int b1 = tf1.SortIndex, e1 = tf1.SortIndexEnd;
  int b2 = tf2.SortIndex, e2 = tf2.SortIndexEnd;
  if (b1 < b2)
  {
    if (e1 <= b2)
      return -1;
    // p2 in p1
    int par = par2;
    for (;;)
    {
      const CTreeFolder &tf = (*sortParam->TreeFolders)[par];
      par = tf.Parent;
      if (par == par1)
      {
        RINOZ(CompareFileNames(u1.Name, tf.Name));
        break;
      }
    }
  }
  else if (b2 < b1)
  {
    if (e2 <= b1)
      return 1;
    // p1 in p2
    int par = par1;
    for (;;)
    {
      const CTreeFolder &tf = (*sortParam->TreeFolders)[par];
      par = tf.Parent;
      if (par == par2)
      {
        RINOZ(CompareFileNames(tf.Name, u2.Name));
        break;
      }
    }
  }
  */
  // RINOZ_COMP(a1.UpdateItem->ParentSortIndex, a2.UpdateItem->ParentSortIndex);
  RINOK(CompareFileNames(u1.Name, u2.Name))
  RINOZ_COMP(a1.UpdateItem->IndexInClient, a2.UpdateItem->IndexInClient)
  RINOZ_COMP(a1.UpdateItem->IndexInArchive, a2.UpdateItem->IndexInArchive)
  return 0;
}

struct CSolidGroup
{
  CRecordVector<UInt32> Indices;

  CRecordVector<CFolderRepack> folderRefs;
};

static const char * const g_Exe_Exts[] =
{
    "dll"
  , "exe"
  , "ocx"
  , "sfx"
  , "sys"
};

static const char * const g_ExeUnix_Exts[] =
{
    "so"
  , "dylib"
};

static bool IsExt_Exe(const wchar_t *ext)
{
  for (unsigned i = 0; i < Z7_ARRAY_SIZE(g_Exe_Exts); i++)
    if (StringsAreEqualNoCase_Ascii(ext, g_Exe_Exts[i]))
      return true;
  return false;
}

/*
static bool IsExt_ExeUnix(const wchar_t *ext)
{
  for (unsigned i = 0; i < Z7_ARRAY_SIZE(g_ExeUnix_Exts); i++)
    if (StringsAreEqualNoCase_Ascii(ext, g_ExeUnix_Exts[i]))
      return true;
  return false;
}
*/

// we try to find "so" extension in such name: libstdc++.so.6.0.29
static bool IsExt_ExeUnix_NumericAllowed(const UString &path)
{
  unsigned pos = path.Len();
  unsigned dotPos = pos;
  for (;;)
  {
    if (pos == 0)
      return false;
    const wchar_t c = path[--pos];
    if (IS_PATH_SEPAR(c))
      return false;
    if (c == '.')
    {
      const unsigned num = (dotPos - pos) - 1;
      if (num < 1)
        return false;
      const wchar_t *cur = path.Ptr(pos + 1);
      for (unsigned i = 0; i < Z7_ARRAY_SIZE(g_ExeUnix_Exts); i++)
      {
        const char *ext = g_ExeUnix_Exts[i];
        if (num == MyStringLen(ext))
          if (IsString1PrefixedByString2_NoCase_Ascii(cur, ext))
            return true;
      }
      const wchar_t *end;
      ConvertStringToUInt32(cur, &end);
      if ((size_t)(end - cur) != num)
        return false;
      dotPos = pos;
    }
  }
}


struct CAnalysis
{
  CMyComPtr<IArchiveUpdateCallbackFile> Callback;
  CByteBuffer Buffer;

  bool ParseWav;
  bool ParseModel;   // 7zip-modern: .safetensors, .gguf (FLOAT method, design 0008)
  bool ParseExe;
  bool ParseExeUnix;
  bool ParseNoExt;
  bool ParseAll;

  /*
  bool Need_ATime;
  bool ATime_Defined;
  FILETIME ATime;
  */

  CAnalysis():
      ParseWav(false),
      ParseModel(false),
      ParseExe(false),
      ParseExeUnix(false),
      ParseNoExt(false),
      ParseAll(false)
      /*
      , Need_ATime(false)
      , ATime_Defined(false)
      */
  {}

  HRESULT GetFilterGroup(UInt32 index, const CUpdateItem &ui, CFilterMode &filterMode);
};

static const size_t kAnalysisBufSize = 1 << 14;

HRESULT CAnalysis::GetFilterGroup(UInt32 index, const CUpdateItem &ui, CFilterMode &filterMode)
{
  filterMode.Id = 0;
  filterMode.Delta = 0;
  filterMode.Offset = 0;

  CFilterMode filterModeTemp = filterMode;

  const int slashPos = ui.Name.ReverseFind_PathSepar();
  const int dotPos = ui.Name.ReverseFind_Dot();

  // if (dotPos > slashPos)
  {
    bool needReadFile = ParseAll;
    /* if (Callback) is not supported by client,
       we still try to use file name extension to detect executable file */
    bool probablyIsSameIsa = false;

    if (!needReadFile || !Callback)
    {
      const wchar_t *ext = NULL;
      if (dotPos > slashPos)
        ext = ui.Name.Ptr((unsigned)(dotPos + 1));
      // 7-zip stores posix attributes in high 16 bits and sets (0x8000) flag
      if (ui.Attrib & 0x8000)
      {
        const unsigned st_mode = ui.Attrib >> 16;
        /* note: executable ".so" can be without execute permission,
           and symbolic link to such ".so" file is possible */
        // st_mode = 00111; // for debug
        /* in Linux we expect such permissions:
             0755 : for most executables
             0644 : for some ".so" files
             0777 : in WSL for all files.
                    We can try to exclude some such 0777 cases from analysis,
                    if there is non-executable extension.
        */

        if ((st_mode & (
              MY_LIN_S_IXUSR |
              MY_LIN_S_IXGRP |
              MY_LIN_S_IXOTH)) != 0
            && MY_LIN_S_ISREG(st_mode)
            && (ui.Size >= (1u << 11)))
        {
          #ifndef _WIN32
          probablyIsSameIsa = true;
          #endif
          needReadFile = true;
        }
      }

      if (!needReadFile)
      {
        if (!ext)
          needReadFile = ParseNoExt;
        else
        {
          bool isUnixExt = false;
          if (ParseExeUnix)
            isUnixExt = IsExt_ExeUnix_NumericAllowed(ui.Name);
          if (isUnixExt)
          {
            needReadFile = true;
            #ifndef _WIN32
              probablyIsSameIsa = true;
            #endif
          }
          else if (IsExt_Exe(ext))
          {
            needReadFile = ParseExe;
            #ifdef _WIN32
              probablyIsSameIsa = true;
            #endif
          }
          else if (StringsAreEqualNoCase_Ascii(ext, "wav"))
          {
            if (!needReadFile)
              needReadFile = ParseWav;
          }
          else if (StringsAreEqualNoCase_Ascii(ext, "safetensors")
              || StringsAreEqualNoCase_Ascii(ext, "gguf"))
          {
            if (!needReadFile)
              needReadFile = ParseModel;
          }
        }
      }
    }

    if (needReadFile)
    {
      BoolInt parseRes = false;
      if (Callback)
      {
        if (Buffer.Size() != kAnalysisBufSize)
          Buffer.Alloc(kAnalysisBufSize);
        CMyComPtr<ISequentialInStream> stream;
        HRESULT result = Callback->GetStream2(index, &stream, NUpdateNotifyOp::kAnalyze);
        if (result == S_OK && stream)
        {
          /*
          if (Need_ATime)
          {
            // access time could be changed in analysis pass
            CMyComPtr<IStreamGetProps> getProps;
            stream.QueryInterface(IID_IStreamGetProps, (void **)&getProps);
            if (getProps)
              if (getProps->GetProps(NULL, NULL, &ATime, NULL, NULL) == S_OK)
                ATime_Defined = true;
          }
          */
          size_t size = kAnalysisBufSize;
          result = ReadStream(stream, Buffer, &size);
          stream.Release();
          // RINOK(Callback->SetOperationResult2(index, NUpdate::NOperationResult::kOK));
          if (result == S_OK)
          {
            parseRes = ParseFile(Buffer, size, &filterModeTemp);
          }
        }
      } // Callback
      else if (probablyIsSameIsa)
      {
        #ifdef MY_CPU_X86_OR_AMD64
          filterModeTemp.Id = k_X86;
        #endif
        #ifdef MY_CPU_ARM64
          filterModeTemp.Id = k_ARM64;
        #endif
        #ifdef MY_CPU_RISCV
          filterModeTemp.Id = k_RISCV;
        #endif
        #ifdef MY_CPU_SPARC
          filterModeTemp.Id = k_SPARC;
        #endif
        parseRes = true;
      }

      if (parseRes
          && filterModeTemp.Id != k_Delta
          && filterModeTemp.Delta == 0)
      {
        /* ParseFile() sets (filterModeTemp.Delta == 0) for all
           methods except of k_Delta. */
        // it's not k_Delta
        // So we call SetDelta() to set Delta
        filterModeTemp.SetDelta();
        if (filterModeTemp.Delta > 1)
        {
          /* If file Size is not aligned, then branch filter
             will not work for next file in solid block.
             Maybe we should allow filter for non-aligned-size file in non-solid archives ?
          */
          if (ui.Size % filterModeTemp.Delta != 0)
            parseRes = false;
          // windows exe files are not aligned for 4 KiB.
          /*
          else if (filterModeTemp.Id == k_ARM64 && filterModeTemp.Offset != 0)
          {
            if (ui.Size % (1 << 12) != 0)
            {
              // If Size is not aligned for 4 KiB, then Offset will not work for next file in solid block.
              // so we place such file in group with (Offset==0).
              filterModeTemp.Offset = 0;
            }
          }
          */
        }
      }
      if (!parseRes)
        filterModeTemp.ClearFilterMode();
    }
  }
  
  filterMode = filterModeTemp;
  return S_OK;
}

static inline void GetMethodFull(UInt64 methodID, UInt32 numStreams, CMethodFull &m)
{
  m.Id = methodID;
  m.NumStreams = numStreams;
}


// we add bond for mode.Methods[0] that is filter
static HRESULT AddBondForFilter(CCompressionMethodMode &mode)
{
  for (unsigned c = 1; c < mode.Methods.Size(); c++)
  {
    if (!mode.IsThereBond_to_Coder(c))
    {
      CBond2 bond;
      bond.OutCoder = 0;
      bond.OutStream = 0;
      bond.InCoder = c;
      mode.Bonds.Add(bond);
      return S_OK;
    }
  }
  return E_INVALIDARG;
}

/*
static HRESULT AddBondForFilter_if_ThereAreBonds(CCompressionMethodMode &mode)
{
  if (!mode.Bonds.IsEmpty())
    return AddBondForFilter(mode);
  return S_OK;
}
*/

static HRESULT AddBcj2Methods(CCompressionMethodMode &mode)
{
  // mode.Methods[0] must be k_BCJ2 method !
  // mode.Methods[1] : we expect that there is at least one method after BCJ2

  CMethodFull m;
  GetMethodFull(k_LZMA, 1, m);
  
  m.AddProp32(NCoderPropID::kDictionarySize, 1 << 20);
  m.AddProp32(NCoderPropID::kNumFastBytes, 128);
  m.AddProp32(NCoderPropID::kNumThreads, 1);
  m.AddProp32(NCoderPropID::kLitPosBits, 2);
  m.AddProp32(NCoderPropID::kLitContextBits, 0);
  // m.AddProp_Ascii(NCoderPropID::kMatchFinder, "BT2");

  const unsigned methodIndex = mode.Methods.Size();

  if (mode.Bonds.IsEmpty())
  {
    for (unsigned i = 1; i + 1 < mode.Methods.Size(); i++)
    {
      CBond2 bond;
      bond.OutCoder = i;
      bond.OutStream = 0;
      bond.InCoder = i + 1;
      mode.Bonds.Add(bond);
    }
  }

  mode.Methods.Add(m);
  mode.Methods.Add(m);
  
  RINOK(AddBondForFilter(mode))
  CBond2 bond;
  bond.OutCoder = 0; // index of BCJ2 coder
  bond.InCoder = methodIndex;      bond.OutStream = 1;  mode.Bonds.Add(bond);
  bond.InCoder = methodIndex + 1;  bond.OutStream = 2;  mode.Bonds.Add(bond);
  return S_OK;
}


static HRESULT MakeExeMethod(CCompressionMethodMode &mode,
    const CFilterMode &filterMode,
    const bool bcj2_IsAllowed,
    const CUIntVector &disabledFilterIDs)
{
  if (mode.Filter_was_Inserted)
  {
    // filter was inserted, but bond for that filter was not added still.
    const CMethodFull &m = mode.Methods[0];
    if (m.Id == k_BCJ2)
      return AddBcj2Methods(mode);
    if (!m.IsSimpleCoder())
      return E_NOTIMPL;
    if (mode.Bonds.IsEmpty())
      return S_OK;
    return AddBondForFilter(mode);
  }

  if (filterMode.Id == 0)
    return S_OK;

  unsigned nextCoder;

  const bool useBcj2 = bcj2_IsAllowed
      && Is86Filter(filterMode.Id)
      && disabledFilterIDs.FindInSorted(k_BCJ2) < 0;

  if (!useBcj2 && disabledFilterIDs.FindInSorted(filterMode.Id) >= 0)
  {
    // required filter is disabled,
    // but we still can use information about data alignment.
#if 0 // 1 for debug
    // we can return here, if we want default lzma properties
    return S_OK;
#else
    // we will try to change lzma/lzma2 properties
    nextCoder = 0;
    if (!mode.Bonds.IsEmpty())
      for (unsigned c = 0;; c++)
      {
        if (c == mode.Methods.Size())
          return S_OK;
        if (!mode.IsThereBond_to_Coder(c))
        {
          nextCoder = c;
          break;
        }
      }
#endif
  }
  else
  {
    // we insert new filter method:
    CMethodFull &m = mode.Methods.InsertNew(0); // 0 == index of new inserted item
    {
      // we move all coder indexes in bonds up for 1 position:
      FOR_VECTOR (k, mode.Bonds)
      {
        CBond2 &bond = mode.Bonds[k];
        bond.InCoder++;
        bond.OutCoder++;
      }
    }
    if (useBcj2)
    {
      GetMethodFull(k_BCJ2, 4, m);
      return AddBcj2Methods(mode);
    }
    
    GetMethodFull(filterMode.Id, 1, m);
    
    if (filterMode.Id == k_Delta)
      m.AddProp32(NCoderPropID::kDefaultProp, filterMode.Delta);
    else if (filterMode.Id == k_ARM64
          || filterMode.Id == k_RISCV)
    {
      // if (filterMode.Offset != 0)
      m.AddProp32(
        NCoderPropID::kDefaultProp,
        // NCoderPropID::kBranchOffset,
        filterMode.Offset);
    }
    
    nextCoder = 1;
    if (!mode.Bonds.IsEmpty())
    {
      RINOK(AddBondForFilter(mode))
      nextCoder = mode.Bonds.Back().InCoder;
    }
  }

  if (nextCoder >= mode.Methods.Size())
  {
    // we don't expect that case, if there was non-filter method.
    // but we return S_OK to support filter-only case.
    return S_OK;
  }

  int alignBits = -1;
  {
    const UInt32 delta = filterMode.Delta;
    if (delta == 0 || delta > 16)
    {
      // if (delta == 0) alignBits = GetAlignForFilterMethod(filterMode.Id);
    }
    else if ((delta & ((1 << 4) - 1)) == 0) alignBits = 4;
    else if ((delta & ((1 << 3) - 1)) == 0) alignBits = 3;
    else if ((delta & ((1 << 2) - 1)) == 0) alignBits = 2;
    else if ((delta & ((1 << 1) - 1)) == 0) alignBits = 1;
    // else alignBits = 0;
    /* alignBits=0 is default mode for lzma/lzma2.
    So we don't set alignBits=0 here. */
  }
  if (alignBits <= 0)
    return S_OK;
  // (alignBits > 0)
  CMethodFull &nextMethod = mode.Methods[nextCoder];
  if (nextMethod.Id == k_LZMA || nextMethod.Id == k_LZMA2)
  if (!nextMethod.Are_Lzma_Model_Props_Defined())
  {
    if (alignBits > 2 || filterMode.Id == k_Delta)
      nextMethod.AddProp32(NCoderPropID::kPosStateBits, (unsigned)alignBits);
    const unsigned lc = (alignBits < 3) ? (unsigned)(3 - alignBits) : 0u;
    nextMethod.AddProp32(NCoderPropID::kLitContextBits, lc);
    nextMethod.AddProp32(NCoderPropID::kLitPosBits, (unsigned)alignBits);
  }
  return S_OK;
}


/* 7zip-modern: the group of AI model weights uses FLOAT (design 0008) instead of the main
   method, if that is LZMA or LZMA2 (the default) alone. Other choices of the user (zstd,
   PPMd, copy with -mx0, chains of methods) are kept; -mf=off disables the analysis. */
static bool UseFloatMethod(CCompressionMethodMode &mode)
{
  if (mode.Methods.Size() != 1 || !mode.Bonds.IsEmpty() || mode.Filter_was_Inserted)
    return false;
  const CMethodFull &old = mode.Methods[0];
  if (old.Id != k_LZMA && old.Id != k_LZMA2)
    return false;
  CMethodFull m;
  m.Id = k_FLOAT;
  m.NumStreams = 1;
  m.AddProp_NumThreads(mode.NumThreads);
  mode.Methods[0] = m;
  return true;
}


static void UpdateItem_To_FileItem2(const CUpdateItem &ui, CFileItem2 &file2)
{
  file2.Attrib = ui.Attrib;  file2.AttribDefined = ui.AttribDefined;
  file2.CTime = ui.CTime;  file2.CTimeDefined = ui.CTimeDefined;
  file2.ATime = ui.ATime;  file2.ATimeDefined = ui.ATimeDefined;
  file2.MTime = ui.MTime;  file2.MTimeDefined = ui.MTimeDefined;
  file2.IsAnti = ui.IsAnti;
  // file2.IsAux = false;
  file2.StartPosDefined = false;
  // file2.StartPos = 0;
}


static void UpdateItem_To_FileItem(const CUpdateItem &ui,
    CFileItem &file, CFileItem2 &file2)
{
  UpdateItem_To_FileItem2(ui, file2);

  file.Size = ui.Size;
  file.IsDir = ui.IsDir;
  file.HasStream = ui.HasStream();
  // file.IsAltStream = ui.IsAltStream;
}



Z7_CLASS_IMP_COM_2(
  CRepackInStreamWithSizes
  , ISequentialInStream
  , ICompressGetSubStreamSize
)
  CMyComPtr<ISequentialInStream> _stream;
  UInt64 _size;
  const CBoolVector *_extractStatuses;
  UInt32 _startIndex;
public:
  const CDbEx *_db;

  void Init(ISequentialInStream *stream, UInt32 startIndex, const CBoolVector *extractStatuses)
  {
    _startIndex = startIndex;
    _extractStatuses = extractStatuses;
    _size = 0;
    _stream = stream;
  }
  UInt64 GetSize() const { return _size; }
};

Z7_COM7F_IMF(CRepackInStreamWithSizes::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  UInt32 realProcessedSize;
  const HRESULT result = _stream->Read(data, size, &realProcessedSize);
  _size += realProcessedSize;
  if (processedSize)
    *processedSize = realProcessedSize;
  return result;
}

Z7_COM7F_IMF(CRepackInStreamWithSizes::GetSubStreamSize(UInt64 subStream, UInt64 *value))
{
  *value = 0;
  if (subStream >= _extractStatuses->Size())
    return S_FALSE; // E_FAIL;
  const unsigned index = (unsigned)subStream;
  if ((*_extractStatuses)[index])
  {
    const CFileItem &fi = _db->Files[_startIndex + index];
    if (fi.HasStream)
      *value = fi.Size;
  }
  return S_OK;
}


class CRepackStreamBase
{
protected:
  bool _needWrite;
  bool _fileIsOpen;
  bool _calcCrc;
  UInt32 _crc;
  UInt64 _rem;

  const CBoolVector *_extractStatuses;
  UInt32 _startIndex;
  unsigned _currentIndex;

  HRESULT OpenFile();
  HRESULT CloseFile();
  HRESULT ProcessEmptyFiles();

public:
  const CDbEx *_db;
  CMyComPtr<IArchiveUpdateCallbackFile> _opCallback;
  CMyComPtr<IArchiveExtractCallbackMessage2> _extractCallback;

  HRESULT Init(UInt32 startIndex, const CBoolVector *extractStatuses);
  HRESULT CheckFinishedState() const { return (_currentIndex == _extractStatuses->Size()) ? S_OK: E_FAIL; }
};

HRESULT CRepackStreamBase::Init(UInt32 startIndex, const CBoolVector *extractStatuses)
{
  _startIndex = startIndex;
  _extractStatuses = extractStatuses;

  _currentIndex = 0;
  _fileIsOpen = false;
  
  return ProcessEmptyFiles();
}

HRESULT CRepackStreamBase::OpenFile()
{
  UInt32 arcIndex = _startIndex + _currentIndex;
  const CFileItem &fi = _db->Files[arcIndex];
  
  _needWrite = (*_extractStatuses)[_currentIndex];
  if (_opCallback)
  {
    RINOK(_opCallback->ReportOperation(
        NEventIndexType::kInArcIndex, arcIndex,
        _needWrite ?
            NUpdateNotifyOp::kRepack :
            NUpdateNotifyOp::kSkip))
  }

  _crc = CRC_INIT_VAL;
  _calcCrc = (fi.CrcDefined && !fi.IsDir);

  _fileIsOpen = true;
  _rem = fi.Size;
  return S_OK;
}

const HRESULT k_My_HRESULT_CRC_ERROR = 0x20000002;

HRESULT CRepackStreamBase::CloseFile()
{
  UInt32 arcIndex = _startIndex + _currentIndex;
  const CFileItem &fi = _db->Files[arcIndex];
  _fileIsOpen = false;
  _currentIndex++;
  if (!_calcCrc || fi.Crc == CRC_GET_DIGEST(_crc))
    return S_OK;

  if (_extractCallback)
  {
    RINOK(_extractCallback->ReportExtractResult(
        NEventIndexType::kInArcIndex, arcIndex,
        NExtract::NOperationResult::kCRCError))
  }
  // return S_FALSE;
  return k_My_HRESULT_CRC_ERROR;
}

HRESULT CRepackStreamBase::ProcessEmptyFiles()
{
  while (_currentIndex < _extractStatuses->Size() && _db->Files[_startIndex + _currentIndex].Size == 0)
  {
    RINOK(OpenFile())
    RINOK(CloseFile())
  }
  return S_OK;
}
  


#ifndef Z7_ST

class CFolderOutStream2 Z7_final:
  public CRepackStreamBase,
  public ISequentialOutStream,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_0
  Z7_IFACE_COM7_IMP(ISequentialOutStream)
public:
  CMyComPtr<ISequentialOutStream> _stream;
};

Z7_COM7F_IMF(CFolderOutStream2::Write(const void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  
  while (size != 0)
  {
    if (_fileIsOpen)
    {
      UInt32 cur = (size < _rem ? size : (UInt32)_rem);
      HRESULT result = S_OK;
      if (_needWrite)
        result = _stream->Write(data, cur, &cur);
      if (_calcCrc)
        _crc = CrcUpdate(_crc, data, cur);
      if (processedSize)
        *processedSize += cur;
      data = (const Byte *)data + cur;
      size -= cur;
      _rem -= cur;
      if (_rem == 0)
      {
        RINOK(CloseFile())
        RINOK(ProcessEmptyFiles())
      }
      RINOK(result)
      if (cur == 0)
        break;
      continue;
    }

    RINOK(ProcessEmptyFiles())
    if (_currentIndex == _extractStatuses->Size())
    {
      // we don't support write cut here
      return E_FAIL;
    }
    RINOK(OpenFile())
  }

  return S_OK;
}

#endif



static const UInt32 kTempBufSize = 1 << 16;

class CFolderInStream2 Z7_final:
  public CRepackStreamBase,
  public ISequentialInStream,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_0
  Z7_IFACE_COM7_IMP(ISequentialInStream)

  Byte *_buf;
public:
  CMyComPtr<ISequentialInStream> _inStream;
  HRESULT Result;

  CFolderInStream2():
      Result(S_OK)
  {
    _buf = new Byte[kTempBufSize];
  }

  ~CFolderInStream2()
  {
    delete []_buf;
  }

  void Init() { Result = S_OK; }
};

Z7_COM7F_IMF(CFolderInStream2::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  
  while (size != 0)
  {
    if (_fileIsOpen)
    {
      UInt32 cur = (size < _rem ? size : (UInt32)_rem);
      
      void *buf;
      if (_needWrite)
        buf = data;
      else
      {
        buf = _buf;
        if (cur > kTempBufSize)
          cur = kTempBufSize;
      }

      const HRESULT result = _inStream->Read(buf, cur, &cur);
      _crc = CrcUpdate(_crc, buf, cur);
      _rem -= cur;

      if (_needWrite)
      {
        data = (Byte *)data + cur;
        size -= cur;
        if (processedSize)
          *processedSize += cur;
      }

      if (result != S_OK)
        Result = result;

      if (_rem == 0)
      {
        RINOK(CloseFile())
        RINOK(ProcessEmptyFiles())
      }

      RINOK(result)
      
      if (cur == 0)
        return E_FAIL;

      continue;
    }

    RINOK(ProcessEmptyFiles())
    if (_currentIndex == _extractStatuses->Size())
    {
      return S_OK;
    }
    RINOK(OpenFile())
  }
  
  return S_OK;
}


class CThreadDecoder Z7_final
  #ifndef Z7_ST
    : public CVirtThread
  #endif
{
public:
  CDecoder Decoder;

  CThreadDecoder(bool multiThreadMixer):
      Decoder(multiThreadMixer)
  {
    #ifndef Z7_ST
    if (multiThreadMixer)
    {
      MtMode = false;
      NumThreads = 1;
      FosSpec = new CFolderOutStream2;
      Fos = FosSpec;
      Result = E_FAIL;
    }
    #endif
    // UnpackSize = 0;
    // send_UnpackSize = false;
  }

  #ifndef Z7_ST
  
  bool dataAfterEnd_Error;
  HRESULT Result;
  CMyComPtr<IInStream> InStream;

  CFolderOutStream2 *FosSpec;
  CMyComPtr<ISequentialOutStream> Fos;

  UInt64 StartPos;
  const CFolders *Folders;
  unsigned FolderIndex;

  // bool send_UnpackSize;
  // UInt64 UnpackSize;
  
  #ifndef Z7_NO_CRYPTO
  CMyComPtr<ICryptoGetTextPassword> getTextPassword;
  #endif

  DECL_EXTERNAL_CODECS_LOC_VARS_DECL

  #ifndef Z7_ST
  bool MtMode;
  UInt32 NumThreads;
  #endif

  
  ~CThreadDecoder() Z7_DESTRUCTOR_override
  {
    /* WaitThreadFinish() will be called in ~CVirtThread().
       But we need WaitThreadFinish() call before
       destructors of this class members.
    */
    CVirtThread::WaitThreadFinish();
  }
private:
  virtual void Execute() Z7_override;

  #endif
};

#ifndef Z7_ST

void CThreadDecoder::Execute()
{
  try
  {
    #ifndef Z7_NO_CRYPTO
      bool isEncrypted = false;
      bool passwordIsDefined = false;
      UString password;
    #endif
 
    dataAfterEnd_Error = false;
      
    Result = Decoder.Decode(
      EXTERNAL_CODECS_LOC_VARS
      InStream,
      StartPos,
      *Folders, FolderIndex,
      
      // send_UnpackSize ? &UnpackSize : NULL,
      NULL, // unpackSize : FULL unpack
      
      Fos,
      NULL, // compressProgress

      NULL  // *inStreamMainRes
      , dataAfterEnd_Error

      Z7_7Z_DECODER_CRYPRO_VARS
      #ifndef Z7_ST
        , MtMode, NumThreads,
        0 // MemUsage
      #endif

      );
  }
  catch(...)
  {
    Result = E_FAIL;
  }
  
  /*
  if (Result == S_OK)
    Result = FosSpec->CheckFinishedState();
  */
  FosSpec->_stream.Release();
}

#endif

#ifndef Z7_NO_CRYPTO

Z7_CLASS_IMP_NOQIB_1(
  CCryptoGetTextPassword
  , ICryptoGetTextPassword
)
public:
  UString Password;
};

Z7_COM7F_IMF(CCryptoGetTextPassword::CryptoGetTextPassword(BSTR *password))
{
  return StringToBstr(Password, password);
}

#endif


static void GetFile(const CDatabase &inDb, unsigned index, CFileItem &file, CFileItem2 &file2)
{
  file = inDb.Files[index];
  file2.CTimeDefined = inDb.CTime.GetItem(index, file2.CTime);
  file2.ATimeDefined = inDb.ATime.GetItem(index, file2.ATime);
  file2.MTimeDefined = inDb.MTime.GetItem(index, file2.MTime);
  file2.StartPosDefined = inDb.StartPos.GetItem(index, file2.StartPos);
  file2.AttribDefined = inDb.Attrib.GetItem(index, file2.Attrib);
  file2.IsAnti = inDb.IsItemAnti(index);
  // file2.IsAux = inDb.IsItemAux(index);
}

/* 7zip-modern: the part of the compression of a new folder that follows
   CEncoder::Encode1() / Encode_Post(): it adds to (newDatabase) the files of the
   folder, with the sizes, CRCs, times and attributes collected by (inStream),
   and the number of unpack streams of the folder; it updates the progress
   counters. The folder itself (Folders, PackSizes from startPackIndex,
   CoderUnpackSizes) must already be in (newDatabase).
   Common to the sequential loop in Update() and to the parallel compression
   of folders (design 0003): both produce the same database. */
static HRESULT AddNewFolderFiles(
    const CDbEx *db,
    const CObjectVector<CUpdateItem> &updateItems,
    const UInt32 *indices, unsigned numSubFiles,
    const CFolderInStream &inStream,
    UInt64 curFolderUnpackSize,
    unsigned startPackIndex,
    CArchiveDatabaseOut &newDatabase,
    CLocalProgress *lps,
    IArchiveUpdateCallback *updateCallback,
    UInt64 &complexity)
{
  UInt64 packSize = 0;
  for (; startPackIndex < newDatabase.PackSizes.Size(); startPackIndex++)
    packSize += newDatabase.PackSizes[startPackIndex];
  lps->OutSize += packSize;

  CNum numUnpackStreams = 0;
  UInt64 skippedSize = 0;
  UInt64 procSize = 0;

  for (unsigned subIndex = 0; subIndex < numSubFiles; subIndex++)
  {
    const CUpdateItem &ui = updateItems[indices[subIndex]];
    CFileItem file;
    CFileItem2 file2;
    UString name;
    if (ui.NewProps)
    {
      UpdateItem_To_FileItem(ui, file, file2);
      name = ui.Name;
    }
    else
    {
      GetFile(*db, (unsigned)ui.IndexInArchive, file, file2);
      db->GetPath((unsigned)ui.IndexInArchive, name);
    }
    if (file2.IsAnti || file.IsDir)
      return E_FAIL;

    if (!inStream.Processed[subIndex])
    {
      // the file was not opened (locked or deleted): we don't add it
      skippedSize += ui.Size;
      continue;
    }

    file.Crc = inStream.CRCs[subIndex];
    file.Size = inStream.Sizes[subIndex];
    procSize += file.Size;
    if (file.Size != 0)
    {
      file.CrcDefined = true;
      file.HasStream = true;
      numUnpackStreams++;
    }
    else
    {
      file.CrcDefined = false;
      file.HasStream = false;
    }

    if (inStream.TimesDefined[subIndex])
    {
      if (inStream.Need_CTime)
        { file2.CTimeDefined = true;  file2.CTime = inStream.CTimes[subIndex]; }
      if (inStream.Need_ATime)
        { file2.ATimeDefined = true;  file2.ATime = inStream.ATimes[subIndex]; }
      if (inStream.Need_MTime)
        { file2.MTimeDefined = true;  file2.MTime = inStream.MTimes[subIndex]; }
      if (inStream.Need_Attrib)
      {
        file2.AttribDefined = true;
        file2.Attrib = inStream.Attribs[subIndex];
      }
    }

    newDatabase.AddFile(file, file2, name);
  }

  // it's optional check to ensure that sizes are correct
  if (procSize != curFolderUnpackSize)
    return E_FAIL;

  lps->InSize += procSize;

  // numUnpackStreams = 0 is very bad case for locked files
  // v3.13 doesn't understand it.
  newDatabase.NumUnpackStreamsVector.Add(numUnpackStreams);

  if (skippedSize != 0 && complexity >= skippedSize)
  {
    complexity -= skippedSize;
    RINOK(updateCallback->SetTotal(complexity))
  }
  return S_OK;
}


/* 7zip-modern: what the compression of a new folder needs, besides the encoder
   and the files. Common to EncodeNewFolderInline() and CParallelFolderEncoder. */
struct CNewFolderContext
{
  DECL_EXTERNAL_CODECS_LOC_VARS_DECL
  const CDbEx *Db;
  const CObjectVector<CUpdateItem> *UpdateItems;
  IArchiveUpdateCallback *UpdateCallback;
  const CUpdateOptions *Options;
  const UInt64 *InSizeForReduce;
  CArchiveDatabaseOut *NewDatabase;
  ISequentialOutStream *ArcStream;
  CLocalProgress *Lps;
  ICompressProgressInfo *LpsProgress; // (Lps) as interface for the coders
  UInt64 *Complexity;
};

static void SetFolderInStreamOptions(CFolderInStream &s, const CUpdateOptions &options)
{
  s.Need_CTime = options.Need_CTime;
  s.Need_ATime = options.Need_ATime;
  s.Need_MTime = options.Need_MTime;
  s.Need_Attrib = options.Need_Attrib;
}


/* 7zip-modern: compresses a new folder in the calling thread and adds it to
   the database: the body of the "Compress files to new solid blocks" loop of
   upstream. (inStream) provides the data of the files: (folderInStream) itself,
   or a stream that ends with it (CFolderReplayInStream after a read ahead). */
static HRESULT EncodeNewFolderInline(const CNewFolderContext &ctx, CEncoder &encoder,
    ISequentialInStream *inStream, const CFolderInStream &folderInStream,
    const UInt32 *indices, unsigned numSubFiles, UInt64 expectedDataSize)
{
 #ifdef Z7_EXTERNAL_CODECS
  const CExternalCodecs *_externalCodecs = ctx._externalCodecs;
 #endif
  CArchiveDatabaseOut &newDatabase = *ctx.NewDatabase;
  const unsigned startPackIndex = newDatabase.PackSizes.Size();

  RINOK(encoder.Encode1(
      EXTERNAL_CODECS_LOC_VARS
      inStream,
      ctx.InSizeForReduce,
      expectedDataSize, // expected size
      newDatabase.Folders.AddNew(),
      ctx.ArcStream, newDatabase.PackSizes, ctx.LpsProgress))

  if (!folderInStream.WasFinished())
    return E_FAIL;

  const UInt64 curFolderUnpackSize = folderInStream.Get_TotalSize_for_Coder();
  encoder.Encode_Post(curFolderUnpackSize, newDatabase.CoderUnpackSizes);

  return AddNewFolderFiles(ctx.Db, *ctx.UpdateItems, indices, numSubFiles,
      folderInStream, curFolderUnpackSize, startPackIndex,
      newDatabase, ctx.Lps, ctx.UpdateCallback, *ctx.Complexity);
}


#ifndef Z7_ST

// ---------------------------------------------------------------------------
// 7zip-modern: parallel compression of new folders (docs/design/0003)
//
// The main thread reads the files of a folder into memory (CFolderInStream,
// so the callbacks are called in the same order as in the sequential loop),
// a worker thread compresses the folder from memory into memory with its own
// CEncoder (same options: same output), and the main thread writes the
// folders to the archive and to the database in the original order.
// ---------------------------------------------------------------------------

class CParallelFolderEncoder;

/* Progress of the coders of one folder. Written by the worker thread, read by
   the main thread, that reports it: the callbacks of the user interface are
   called only by the main thread. */
Z7_CLASS_IMP_COM_1(
  CFolderEncodeProgress
  , ICompressProgressInfo
)
  UInt64 _wakeInSize; // used only by the worker thread
public:
  std::atomic<UInt64> InSize;
  std::atomic<UInt64> OutSize;
  CParallelFolderEncoder *Owner;
  CFolderEncodeProgress(): _wakeInSize(0), InSize(0), OutSize(0), Owner(NULL) {}
};


class CFolderEncodeTask: public CMtTask
{
  Z7_CLASS_NO_COPY(CFolderEncodeTask)
  HRESULT Encode();
public:
  CParallelFolderEncoder *Owner;
  // indexes of the files in (updateItems): a copy, because the folder can be
  // written after the end of the loop of its group in Update()
  std::vector<UInt32> IndexStorage;
  const UInt32 *Indices;
  unsigned NumSubFiles;
  UInt64 ExpectedSize;

  UInt32 NumThreads;     // in the thread budget of the owner while the task runs
  UInt64 EncoderMemory;  // estimate for the encoder (kept by the encoder slot)
  unsigned Slot;         // encoder slot of the owner
  // in the memory budget of the owner:
  UInt64 InMemory;       // released when the task finished
  UInt64 OutMemory;      // released when the folder was written

  // read ahead by the main thread
  CMyComPtr2_Create<ISequentialInStream, CFolderInStream> InStream; // sizes, CRCs, times
  CByteDynBuffer InData;
  size_t InDataSize;

  // results of the worker thread
  CMyComPtr2_Create<ISequentialOutStream, CDynBufSeqOutStream> OutData;
  CMyComPtr2_Create<ICompressProgressInfo, CFolderEncodeProgress> Progress;
  CFolder Folder;
  CRecordVector<UInt64> PackSizes;
  CRecordVector<UInt64> CoderUnpackSizes;
  HRESULT Result;
  std::atomic<bool> Finished; // set by Run() after the last access to the input

  bool Accounted; // main thread: threads and input memory were released

  CFolderEncodeTask():
      Owner(NULL), Indices(NULL), NumSubFiles(0), ExpectedSize(0),
      NumThreads(1), EncoderMemory(0), Slot(0), InMemory(0), OutMemory(0),
      InDataSize(0), Result(S_OK), Finished(false), Accounted(false)
      {}

  void Run(unsigned workerIndex) Z7_override;
};


class CParallelFolderEncoder
{
  Z7_CLASS_NO_COPY(CParallelFolderEncoder)

  const CNewFolderContext &_ctx;
  /* methods of the groups of filters, in the order of Update(): the folders of
     several groups can be compressed at the same time (a copy of each method,
     because the encoders of a group can work after the end of its loop). */
  std::vector<std::unique_ptr<CCompressionMethodMode> > _methods;
  unsigned _curGroup;   // group of the folders that are added now
  UInt32 _numThreads;
  UInt64 _memLimit;
  UInt32 _threadsInUse;
  CMemoryBudget _budget;

  /* Encoders, reused for the next folders as in the sequential loop. An encoder
     keeps its memory (match finder, buffers) after a folder: that memory stays
     reserved in (_budget) for the slot, as the largest estimate of the folders
     the slot compressed, until the slot is released. So the reserved memory
     bounds the real one, also with idle encoders. */
  struct CEncoderSlot
  {
    std::unique_ptr<CEncoder> Encoder; // for the method of (Group); NULL: not created
    unsigned Group;
    UInt64 Memory;
    bool Busy;
    CEncoderSlot(): Group(0), Memory(0), Busy(false) {}
  };
  std::vector<CEncoderSlot> _slots;

  // submitted folders, in the order of the archive
  std::deque<std::unique_ptr<CFolderEncodeTask> > _window;
  CMtTaskPool _pool;

  int FindFreeSlot(UInt64 encoderMemory) const;
  bool ReleaseIdleSlots(unsigned except);
  void ReleaseSlot(CEncoderSlot &slot);
  HRESULT ReadAhead(CFolderEncodeTask &task, bool &stoppedAtLimit);
  HRESULT Commit(CFolderEncodeTask &task);
  HRESULT ReportProgress();
  HRESULT Poll(bool &progress);
  HRESULT WaitAndCommit();

public:
  NWindows::NSynchronization::CAutoResetEvent Wake; // a task finished or made progress
  std::atomic<bool> StopRequested;

  CParallelFolderEncoder(const CNewFolderContext &ctx, UInt32 numThreads, UInt64 memLimit):
      _ctx(ctx), _curGroup(0), _numThreads(numThreads), _memLimit(memLimit), _threadsInUse(0),
      StopRequested(false)
      {}
  ~CParallelFolderEncoder()
  {
    // the running tasks stop at the next progress call; they use (Wake)
    StopRequested = true;
    _pool.Stop();
  }

  const CNewFolderContext &Ctx() const { return _ctx; }
  CEncoder &GetEncoder(unsigned slot) { return *_slots[slot].Encoder; }

  HRESULT Start();

  /* The next folders are of a new group of filters, with (method). The folders
     of the previous groups that are queued continue. */
  void BeginGroup(const CCompressionMethodMode &method)
  {
    _methods.push_back(std::make_unique<CCompressionMethodMode>(method));
    _curGroup = (unsigned)_methods.size() - 1;
  }

  /* (done == true): the folder is queued (or was compressed) by this object.
     (done == false): the caller must compress it inline; all the queued folders
     were written before.
     (isLastFolder): the last new folder of the operation: if no other folder is
     being compressed, it's compressed inline (nothing to overlap with: it keeps
     the streaming of the sequential loop, e.g. for an archive with one solid
     block). (indices) is copied: the caller can free it after the call. */
  HRESULT AddFolder(const UInt32 *indices, unsigned numSubFiles, UInt64 expectedSize,
      bool isLastFolder, CEncoder &inlineEncoder, bool &done);

  /* Waits for the queued folders and writes them. */
  HRESULT Flush();
};


Z7_COM7F_IMF(CFolderEncodeProgress::SetRatioInfo(const UInt64 *inSize, const UInt64 *outSize))
{
  if (Owner->StopRequested.load(std::memory_order_relaxed))
    return E_ABORT;
  if (outSize)
    OutSize.store(*outSize, std::memory_order_relaxed);
  if (inSize)
  {
    InSize.store(*inSize, std::memory_order_relaxed);
    // wake the main thread up to report the progress, at most once per MiB
    if (*inSize - _wakeInSize >= ((UInt32)1 << 20))
    {
      _wakeInSize = *inSize;
      Owner->Wake.Set();
    }
  }
  return S_OK;
}


HRESULT CFolderEncodeTask::Encode()
{
  const CNewFolderContext &ctx = Owner->Ctx();
 #ifdef Z7_EXTERNAL_CODECS
  const CExternalCodecs *_externalCodecs = ctx._externalCodecs;
 #endif
  const CFolderInStream &folderInStream = *InStream.ClsPtr();

  CMyComPtr2_Create<ISequentialInStream, CFolderReplayInStream> replay;
  replay->Init(InData, InDataSize,
      folderInStream.Sizes.ConstData(), folderInStream.Sizes.Size(), NULL);

  OutData->Init();
  if (!OutData->GetBufPtrForWriting((size_t)OutMemory))
    return E_OUTOFMEMORY;
  Progress->Owner = Owner;

  CEncoder &encoder = Owner->GetEncoder(Slot);
  RINOK(encoder.Encode1(
      EXTERNAL_CODECS_LOC_VARS
      replay,
      ctx.InSizeForReduce,
      ExpectedSize,
      Folder,
      OutData, PackSizes, Progress))

  encoder.Encode_Post(folderInStream.Get_TotalSize_for_Coder(), CoderUnpackSizes);
  return S_OK;
}


void CFolderEncodeTask::Run(unsigned /* workerIndex */)
{
  try
  {
    Result = Encode();
  }
  catch (const CNewException &)
  {
    Result = E_OUTOFMEMORY;
  }
  catch (...)
  {
    Result = E_FAIL;
  }
  // the main thread can free the input and reuse the threads after this point
  Finished.store(true, std::memory_order_release);
  Owner->Wake.Set();
}


HRESULT CParallelFolderEncoder::Start()
{
  if (_numThreads < 2)
    return E_FAIL;
  {
    const WRes wres = Wake.CreateIfNotCreated_Reset();
    if (wres != 0)
      return HRESULT_FROM_WIN32(wres);
  }
  _budget.Init(_memLimit);
  // no more running tasks than threads: each task has a weight >= 1;
  // the encoders are created for the group of their first folder
  _slots.resize(_numThreads);
  return _pool.Start(_numThreads);
}


/* A free slot for a task of the current group with (encoderMemory). Among the
   slots of the current group: the smallest one with enough reserved memory, else
   the largest one (the least memory to add). Else a slot of another group (its
   encoder will be replaced): the one with the least memory. -1: none. */
int CParallelFolderEncoder::FindFreeSlot(UInt64 encoderMemory) const
{
  int fit = -1;
  int largest = -1;
  int other = -1;
  for (unsigned i = 0; i < _slots.size(); i++)
  {
    const CEncoderSlot &slot = _slots[i];
    if (slot.Busy)
      continue;
    if (!slot.Encoder || slot.Group != _curGroup)
    {
      if (other < 0 || slot.Memory < _slots[(unsigned)other].Memory)
        other = (int)i;
      continue;
    }
    if (slot.Memory >= encoderMemory)
    {
      if (fit < 0 || slot.Memory < _slots[(unsigned)fit].Memory)
        fit = (int)i;
    }
    else if (largest < 0 || slot.Memory > _slots[(unsigned)largest].Memory)
      largest = (int)i;
  }
  return fit >= 0 ? fit : largest >= 0 ? largest : other;
}


/* Destroys the encoder of an idle slot: its memory is freed. */
void CParallelFolderEncoder::ReleaseSlot(CEncoderSlot &slot)
{
  slot.Encoder.reset();
  _budget.Release(slot.Memory);
  slot.Memory = 0;
}


/* Frees the memory of the idle encoders (but (except)): the slot creates a new
   CEncoder for its next folder. Returns false if nothing was freed. */
bool CParallelFolderEncoder::ReleaseIdleSlots(unsigned except)
{
  bool released = false;
  for (unsigned i = 0; i < _slots.size(); i++)
  {
    CEncoderSlot &slot = _slots[i];
    if (slot.Busy || i == except || slot.Memory == 0)
      continue;
    ReleaseSlot(slot);
    released = true;
  }
  return released;
}


HRESULT CParallelFolderEncoder::ReadAhead(CFolderEncodeTask &task, bool &stoppedAtLimit)
{
  stoppedAtLimit = false;
  CFolderInStream &folderInStream = *task.InStream.ClsPtr();
  SetFolderInStreamOptions(folderInStream, *_ctx.Options);
  folderInStream.Init(_ctx.UpdateCallback, task.Indices, task.NumSubFiles);

  const UInt32 kChunk = (UInt32)1 << 20;
  if (!task.InData.EnsureCapacity((size_t)task.ExpectedSize + kChunk))
    return E_OUTOFMEMORY;
  size_t size = 0;
  for (;;)
  {
    if (!task.InData.EnsureCapacity(size + kChunk))
      return E_OUTOFMEMORY;
    UInt32 processed = 0;
    RINOK(task.InStream.Interface()->Read((Byte *)task.InData + size, kChunk, &processed))
    if (processed == 0)
      break;
    size += processed;
    if (size >= kMaxParallelEncodeReadSize)
    {
      stoppedAtLimit = true;
      break;
    }
  }
  task.InDataSize = size;
  if (!stoppedAtLimit && !folderInStream.WasFinished())
    return E_FAIL;
  return S_OK;
}


static void CopyFolder(const CFolder &src, CFolder &dest)
{
  unsigned i;
  dest.Coders.SetSize(src.Coders.Size());
  for (i = 0; i < src.Coders.Size(); i++)
    dest.Coders[i] = src.Coders[i];
  dest.Bonds.SetSize(src.Bonds.Size());
  for (i = 0; i < src.Bonds.Size(); i++)
    dest.Bonds[i] = src.Bonds[i];
  dest.PackStreams.SetSize(src.PackStreams.Size());
  for (i = 0; i < src.PackStreams.Size(); i++)
    dest.PackStreams[i] = src.PackStreams[i];
}


HRESULT CParallelFolderEncoder::Commit(CFolderEncodeTask &task)
{
  // the worker thread doesn't use the task after (Done)
  RINOK_WRes(task.Done.Lock())
  if (task.ExceptionWasCaught)
    return E_FAIL;
  RINOK(task.Result)

  RINOK(WriteStream(_ctx.ArcStream, task.OutData->GetBuffer(), task.OutData->GetSize()))

  // the same entries as EncodeNewFolderInline()
  CArchiveDatabaseOut &newDatabase = *_ctx.NewDatabase;
  CopyFolder(task.Folder, newDatabase.Folders.AddNew());
  const unsigned startPackIndex = newDatabase.PackSizes.Size();
  FOR_VECTOR (i, task.PackSizes)
    newDatabase.PackSizes.Add(task.PackSizes[i]);
  FOR_VECTOR (k, task.CoderUnpackSizes)
    newDatabase.CoderUnpackSizes.Add(task.CoderUnpackSizes[k]);

  const CFolderInStream &folderInStream = *task.InStream.ClsPtr();
  RINOK(AddNewFolderFiles(_ctx.Db, *_ctx.UpdateItems, task.Indices, task.NumSubFiles,
      folderInStream, folderInStream.Get_TotalSize_for_Coder(), startPackIndex,
      newDatabase, _ctx.Lps, _ctx.UpdateCallback, *_ctx.Complexity))

  _budget.Release(task.OutMemory);
  task.OutMemory = 0;
  return S_OK;
}


HRESULT CParallelFolderEncoder::ReportProgress()
{
  UInt64 inSize = 0;
  UInt64 outSize = 0;
  for (const auto &task : _window)
  {
    inSize += task->Progress->InSize.load(std::memory_order_relaxed);
    outSize += task->Progress->OutSize.load(std::memory_order_relaxed);
  }
  return _ctx.LpsProgress->SetRatioInfo(&inSize, &outSize);
}


HRESULT CParallelFolderEncoder::Poll(bool &progress)
{
  progress = false;
  for (const auto &task : _window)
  {
    if (task->Accounted || !task->Finished.load(std::memory_order_acquire))
      continue;
    task->Accounted = true;
    _threadsInUse -= task->NumThreads;
    _slots[task->Slot].Busy = false;
    _budget.Release(task->InMemory);
    task->InMemory = 0;
    task->InData.Free();
    progress = true;
  }
  while (!_window.empty() && _window.front()->Accounted)
  {
    RINOK(Commit(*_window.front()))
    _window.pop_front();
    progress = true;
  }
  return S_OK;
}


HRESULT CParallelFolderEncoder::WaitAndCommit()
{
  for (;;)
  {
    bool progress;
    RINOK(Poll(progress))
    if (progress || _window.empty())
      return S_OK;
    RINOK(ReportProgress())
    RINOK_WRes(Wake.Lock())
  }
}


HRESULT CParallelFolderEncoder::Flush()
{
  while (!_window.empty())
  {
    RINOK(WaitAndCommit())
  }
  return S_OK;
}


HRESULT CParallelFolderEncoder::AddFolder(const UInt32 *indices, unsigned numSubFiles,
    UInt64 expectedSize, bool isLastFolder, CEncoder &inlineEncoder, bool &done)
{
  done = false;

  if (isLastFolder)
  {
    bool progress;
    RINOK(Poll(progress))
    if (_window.empty())
      return S_OK;
  }

  CFolderEncodeCost cost;
  if (expectedSize > kMaxParallelEncodeFolderSize
      || !EstimateFolderEncodeCost(*_methods[_curGroup], *_ctx.InSizeForReduce, expectedSize, cost)
      // the encoder alone uses all the threads (e.g. BZip2 blocks): nothing to gain
      || cost.NumThreads >= _numThreads)
    return Flush();
  const UInt64 outMemory = EstimateFolderEncodeOutSize(expectedSize);
  if (_budget.IsLargeJob(expectedSize + outMemory + cost.EncoderMemory))
    return Flush();

  std::unique_ptr<CFolderEncodeTask> task = std::make_unique<CFolderEncodeTask>();
  task->Owner = this;
  task->IndexStorage.assign(indices, indices + numSubFiles);
  task->Indices = task->IndexStorage.data();
  task->NumSubFiles = numSubFiles;
  task->ExpectedSize = expectedSize;
  task->NumThreads = cost.NumThreads;
  task->EncoderMemory = cost.EncoderMemory;
  task->InMemory = expectedSize;
  task->OutMemory = outMemory;

  // buffers of this folder; then the read ahead, while the workers compress
  {
    const UInt64 total = task->InMemory + task->OutMemory;
    while (!_window.empty() && !_budget.CanReserve(total))
    {
      RINOK(WaitAndCommit())
    }
    _budget.Reserve(total);
  }
  bool stoppedAtLimit;
  {
    const HRESULT res = ReadAhead(*task, stoppedAtLimit);
    if (res != S_OK)
    {
      _budget.Release(task->InMemory + task->OutMemory);
      return res;
    }
  }

  if (stoppedAtLimit)
  {
    // files grew after the planning: the rest of the folder is compressed inline
    _budget.Release(task->InMemory + task->OutMemory);
    RINOK(Flush())
    const CFolderInStream &folderInStream = *task->InStream.ClsPtr();
    CMyComPtr2_Create<ISequentialInStream, CFolderReplayInStream> replay;
    replay->Init(task->InData, task->InDataSize,
        folderInStream.Sizes.ConstData(), folderInStream.Sizes.Size(), task->InStream);
    RINOK(EncodeNewFolderInline(_ctx, inlineEncoder, replay, folderInStream,
        indices, numSubFiles, expectedSize))
    done = true;
    return S_OK;
  }

  if (task->InDataSize > task->InMemory)
  {
    // files grew a little: we reserve the real size
    const UInt64 extra = task->InDataSize - task->InMemory;
    while (!_window.empty() && !_budget.CanReserve(extra))
    {
      RINOK(WaitAndCommit())
    }
    _budget.Reserve(extra);
    task->InMemory = task->InDataSize;
  }

  // threads, and an encoder slot with memory for the encoder
  for (;;)
  {
    if (_threadsInUse == 0 || _threadsInUse + task->NumThreads <= _numThreads)
    {
      const int slotIndex = FindFreeSlot(task->EncoderMemory);
      if (slotIndex >= 0)
      {
        CEncoderSlot &slot = _slots[(unsigned)slotIndex];
        if (slot.Encoder && slot.Group != _curGroup)
          ReleaseSlot(slot);  // the encoder of another group
        const UInt64 extra = slot.Memory >= task->EncoderMemory ? 0 :
            task->EncoderMemory - slot.Memory;
        // memory kept by idle encoders is freed before waiting for running tasks
        if (extra != 0 && !_budget.CanReserve(extra) && ReleaseIdleSlots((unsigned)slotIndex))
          continue;
        // (_window.empty()): this folder alone; it's not a large job (checked above)
        if (extra == 0 || _budget.CanReserve(extra) || _window.empty())
        {
          if (!slot.Encoder)
          {
            slot.Encoder = std::make_unique<CEncoder>(*_methods[_curGroup]);
            slot.Group = _curGroup;
          }
          _budget.Reserve(extra);
          slot.Memory += extra;
          slot.Busy = true;
          task->Slot = (unsigned)slotIndex;
          break;
        }
      }
    }
    RINOK(WaitAndCommit())
  }

  {
    const HRESULT res = _pool.Submit(task.get());
    if (res != S_OK)
    {
      _slots[task->Slot].Busy = false;
      _budget.Release(task->InMemory + task->OutMemory);
      return res;
    }
  }
  _threadsInUse += task->NumThreads;
  _window.push_back(std::move(task));
  done = true;

  bool progress;
  return Poll(progress);
}

#endif // Z7_ST


HRESULT Update(
    DECL_EXTERNAL_CODECS_LOC_VARS
    IInStream *inStream,
    const CDbEx *db,
    CObjectVector<CUpdateItem> &updateItems,
    // const CObjectVector<CTreeFolder> &treeFolders,
    // const CUniqBlocks &secureBlocks,
    ISequentialOutStream *seqOutStream,
    IArchiveUpdateCallback *updateCallback,
    const CUpdateOptions &options)
{
  UInt64 numSolidFiles = options.NumSolidFiles;
  if (numSolidFiles == 0)
    numSolidFiles = 1;

  Z7_DECL_CMyComPtr_QI_FROM(
      IArchiveUpdateCallbackFile,
      opCallback, updateCallback)

  Z7_DECL_CMyComPtr_QI_FROM(
      IArchiveExtractCallbackMessage2,
      extractCallback, updateCallback)

  /*
  Z7_DECL_CMyComPtr_QI_FROM(
      IArchiveUpdateCallbackArcProp,
      reportArcProp, updateCallback)
  */

  // size_t totalSecureDataSize = (size_t)secureBlocks.GetTotalSizeInBytes();

  CMyComPtr<IStreamSetRestriction> v_StreamSetRestriction;
  {
    Z7_DECL_CMyComPtr_QI_FROM(
        IOutStream,
        outStream, seqOutStream)
    if (!outStream)
      return E_NOTIMPL;
    const UInt64 sfxBlockSize = (db && !options.RemoveSfxBlock) ?
        db->ArcInfo.StartPosition: 0;
    seqOutStream->QueryInterface(IID_IStreamSetRestriction, (void **)&v_StreamSetRestriction);
    if (v_StreamSetRestriction)
    {
      UInt64 offset = 0;
      RINOK(outStream->Seek(0, STREAM_SEEK_CUR, &offset))
      RINOK(v_StreamSetRestriction->SetRestriction(
          outStream ? offset + sfxBlockSize : 0,
          outStream ? offset + sfxBlockSize + k_StartHeadersRewriteSize : 0))
    }
    outStream.Release();
    if (sfxBlockSize != 0)
    {
      RINOK(WriteRange(inStream, seqOutStream, 0, sfxBlockSize, NULL))
    }
  }

  CIntArr fileIndexToUpdateIndexMap;
  UInt64 complexity = 0;
  bool isThere_UnknownSize = false;
  UInt64 inSizeForReduce2 = 0;

 #ifndef Z7_NO_CRYPTO
  bool needEncryptedRepack = false;
 #endif

  CRecordVector<CFilterMode2> filters;
  CObjectVector<CSolidGroup> groups;
  
  #ifndef Z7_ST
  bool thereAreRepacks = false;
  #endif

  bool useFilters = options.UseFilters;
  if (useFilters)
  {
    const CCompressionMethodMode &method = *options.Method;

    FOR_VECTOR (i, method.Methods)
    {
      /* IsFilterMethod() knows only built-in codecs
         FIXME: we should check IsFilter status for external filters too */
      if (IsFilterMethod(method.Methods[i].Id))
      {
        useFilters = false;
        break;
      }
    }
  }
  
  if (db)
  {
    fileIndexToUpdateIndexMap.Alloc(db->Files.Size());
    unsigned i;
    
    for (i = 0; i < db->Files.Size(); i++)
      fileIndexToUpdateIndexMap[i] = -1;

    for (i = 0; i < updateItems.Size(); i++)
    {
      int index = updateItems[i].IndexInArchive;
      if (index != -1)
        fileIndexToUpdateIndexMap[(unsigned)index] = (int)i;
    }

    for (i = 0; i < db->NumFolders; i++)
    {
      CNum indexInFolder = 0;
      CNum numCopyItems = 0;
      const CNum numUnpackStreams = db->NumUnpackStreamsVector[i];
      UInt64 repackSize = 0;
      
      for (CNum fi = db->FolderStartFileIndex[i]; indexInFolder < numUnpackStreams; fi++)
      {
        if (fi >= db->Files.Size())
          return E_FAIL;

        const CFileItem &file = db->Files[fi];
        if (file.HasStream)
        {
          indexInFolder++;
          const int updateIndex = fileIndexToUpdateIndexMap[fi];
          if (updateIndex >= 0 && !updateItems[(unsigned)updateIndex].NewData)
          {
            numCopyItems++;
            repackSize += file.Size;
          }
        }
      }

      if (numCopyItems == 0)
        continue;

      CFolderRepack rep;
      rep.FolderIndex = i;
      rep.NumCopyFiles = numCopyItems;
      CFolderEx f;
      db->ParseFolderEx(i, f);

     #ifndef Z7_NO_CRYPTO
      const bool isEncrypted = f.IsEncrypted();
     #endif
      const bool needCopy = (numCopyItems == numUnpackStreams);
      const bool extractFilter = (useFilters || needCopy);

      const unsigned groupIndex = Get_FilterGroup_for_Folder(filters, f, extractFilter);
      
      while (groupIndex >= groups.Size())
        groups.AddNew();

      groups[groupIndex].folderRefs.Add(rep);
      
      if (needCopy)
        complexity += db->GetFolderFullPackSize(i);
      else
      {
        #ifndef Z7_ST
        thereAreRepacks = true;
        #endif
        complexity += repackSize;
        if (inSizeForReduce2 < repackSize)
          inSizeForReduce2 = repackSize;
       #ifndef Z7_NO_CRYPTO
        if (isEncrypted)
          needEncryptedRepack = true;
       #endif
      }
    }
  }

  UInt64 inSizeForReduce = 0;
  {
    const bool isSolid = (numSolidFiles > 1 && options.NumSolidBytes != 0);
    FOR_VECTOR (i, updateItems)
    {
      const CUpdateItem &ui = updateItems[i];
      if (ui.NewData)
      {
        if (ui.Size == (UInt64)(Int64)-1)
          isThere_UnknownSize = true;
        else
        {
          complexity += ui.Size;
          if (isSolid)
            inSizeForReduce += ui.Size;
          else if (inSizeForReduce < ui.Size)
            inSizeForReduce = ui.Size;
        }
      }
    }
  }

  if (isThere_UnknownSize)
    inSizeForReduce = (UInt64)(Int64)-1;
  else
    RINOK(updateCallback->SetTotal(complexity))

  if (inSizeForReduce < inSizeForReduce2)
      inSizeForReduce = inSizeForReduce2;


  CMyComPtr2_Create<ICompressProgressInfo, CLocalProgress> lps;
  lps->Init(updateCallback, true);

  #ifndef Z7_ST
  
  CStreamBinder sb;
  /*
  if (options.MultiThreadMixer)
  {
    RINOK(sb.CreateEvents());
  }
  */
  
  #endif

  CThreadDecoder threadDecoder(options.MultiThreadMixer);
  
  #ifndef Z7_ST
  if (options.MultiThreadMixer && thereAreRepacks)
  {
    #ifdef Z7_EXTERNAL_CODECS
    threadDecoder._externalCodecs = _externalCodecs;
    #endif
    const WRes wres = threadDecoder.Create();
    if (wres != 0)
      return HRESULT_FROM_WIN32(wres);
  }
  #endif

  {
    CAnalysis analysis;
    // analysis.Need_ATime = options.Need_ATime;
    int analysisLevel = options.AnalysisLevel;
    // (analysisLevel < 0) means default level (5)
    if (analysisLevel < 0)
      analysisLevel = 5;
    if (analysisLevel != 0)
    {
      analysis.Callback = opCallback;
      analysis.ParseWav = true;
      analysis.ParseModel = true;
      if (analysisLevel >= 5)
      {
        analysis.ParseExe = true;
        analysis.ParseExeUnix = true;
        // analysis.ParseNoExt = true;
        if (analysisLevel >= 7)
        {
          analysis.ParseNoExt = true;
          if (analysisLevel >= 9)
            analysis.ParseAll = true;
        }
      }
    }

    // ---------- Split files to groups ----------

    const CCompressionMethodMode &method = *options.Method;
    
    FOR_VECTOR (i, updateItems)
    {
      const CUpdateItem &ui = updateItems[i];
      if (!ui.NewData || !ui.HasStream())
        continue;

      CFilterMode2 fm;
      fm.Construct();
      if (useFilters)
      {
        // analysis.ATime_Defined = false;
        RINOK(analysis.GetFilterGroup(i, ui, fm))
        /*
        if (analysis.ATime_Defined)
        {
          ui.ATime = FILETIME_To_UInt64(analysis.ATime);
          ui.ATime_WasReadByAnalysis = true;
        }
        */
      }
      fm.Encrypted = method.PasswordIsDefined;

      const unsigned groupIndex = GetGroup(filters, fm);
      while (groupIndex >= groups.Size())
        groups.AddNew();
      groups[groupIndex].Indices.Add(i);
    }
  }


  #ifndef Z7_NO_CRYPTO

  CCryptoGetTextPassword *getPasswordSpec = NULL;
  CMyComPtr<ICryptoGetTextPassword> getTextPassword;
  if (needEncryptedRepack)
  {
    getPasswordSpec = new CCryptoGetTextPassword;
    getTextPassword = getPasswordSpec;
    
    #ifndef Z7_ST
    threadDecoder.getTextPassword = getPasswordSpec;
    #endif

    if (options.Method->PasswordIsDefined)
      getPasswordSpec->Password = options.Method->Password;
    else
    {
      Z7_DECL_CMyComPtr_QI_FROM(
          ICryptoGetTextPassword,
          getDecoderPassword, updateCallback)
      if (!getDecoderPassword)
        return E_NOTIMPL;
      CMyComBSTR password;
      RINOK(getDecoderPassword->CryptoGetTextPassword(&password))
      if (password)
        getPasswordSpec->Password = password;
    }
  }

  #endif

  // ---------- Compress ----------

  COutArchive archive;
  CArchiveDatabaseOut newDatabase;

  RINOK(archive.Create_and_WriteStartPrefix(seqOutStream))

  /*
  CIntVector treeFolderToArcIndex;
  treeFolderToArcIndex.Reserve(treeFolders.Size());
  for (i = 0; i < treeFolders.Size(); i++)
    treeFolderToArcIndex.Add(-1);
  // ---------- Write Tree (only AUX dirs) ----------
  for (i = 1; i < treeFolders.Size(); i++)
  {
    const CTreeFolder &treeFolder = treeFolders[i];
    CFileItem file;
    CFileItem2 file2;
    file2.Init();
    int secureID = 0;
    if (treeFolder.UpdateItemIndex < 0)
    {
      // we can store virtual dir item wuthout attrib, but we want all items have attrib.
      file.SetAttrib(FILE_ATTRIBUTE_DIRECTORY);
      file2.IsAux = true;
    }
    else
    {
      const CUpdateItem &ui = updateItems[treeFolder.UpdateItemIndex];
      // if item is not dir, then it's parent for alt streams.
      // we will write such items later
      if (!ui.IsDir)
        continue;
      secureID = ui.SecureIndex;
      if (ui.NewProps)
        UpdateItem_To_FileItem(ui, file, file2);
      else
        GetFile(*db, ui.IndexInArchive, file, file2);
    }
    file.Size = 0;
    file.HasStream = false;
    file.IsDir = true;
    file.Parent = treeFolder.Parent;
    
    treeFolderToArcIndex[i] = newDatabase.Files.Size();
    newDatabase.AddFile(file, file2, treeFolder.Name);
    
    if (totalSecureDataSize != 0)
      newDatabase.SecureIDs.Add(secureID);
  }
  */

  {
    /* ---------- Write non-AUX dirs and Empty files ---------- */
    CUIntVector emptyRefs;
    
    unsigned i;

    for (i = 0; i < updateItems.Size(); i++)
    {
      const CUpdateItem &ui = updateItems[i];
      if (ui.NewData)
      {
        if (ui.HasStream())
          continue;
      }
      else if (ui.IndexInArchive != -1 && db->Files[(unsigned)ui.IndexInArchive].HasStream)
        continue;
      /*
      if (ui.TreeFolderIndex >= 0)
        continue;
      */
      emptyRefs.Add(i);
    }
    
    emptyRefs.Sort(CompareEmptyItems, (void *)&updateItems);
    
    for (i = 0; i < emptyRefs.Size(); i++)
    {
      const CUpdateItem &ui = updateItems[emptyRefs[i]];
      CFileItem file;
      CFileItem2 file2;
      UString name;
      if (ui.NewProps)
      {
        UpdateItem_To_FileItem(ui, file, file2);
        file.CrcDefined = false;
        name = ui.Name;
      }
      else
      {
        GetFile(*db, (unsigned)ui.IndexInArchive, file, file2);
        db->GetPath((unsigned)ui.IndexInArchive, name);
      }
      
      /*
      if (totalSecureDataSize != 0)
        newDatabase.SecureIDs.Add(ui.SecureIndex);
      file.Parent = ui.ParentFolderIndex;
      */
      newDatabase.AddFile(file, file2, name);
    }
  }

  lps->ProgressOffset = 0;

  {
    // ---------- Sort Filters ----------
    FOR_VECTOR (i, filters)
    {
      filters[i].GroupIndex = i;
    }
    filters.Sort2();
  }

  CNewFolderContext newFolderCtx;
 #ifdef Z7_EXTERNAL_CODECS
  newFolderCtx._externalCodecs = _externalCodecs;
 #endif
  newFolderCtx.Db = db;
  newFolderCtx.UpdateItems = &updateItems;
  newFolderCtx.UpdateCallback = updateCallback;
  newFolderCtx.Options = &options;
  newFolderCtx.InSizeForReduce = &inSizeForReduce;
  newFolderCtx.NewDatabase = &newDatabase;
  newFolderCtx.ArcStream = archive.SeqStream;
  newFolderCtx.Lps = lps.ClsPtr();
  newFolderCtx.LpsProgress = lps;
  newFolderCtx.Complexity = &complexity;

  /* 7zip-modern: parallel compression of the new folders (design 0003). One
     object for all the groups: the folders of a group are compressed while the
     last folders of the previous group are still being compressed. */
 #ifndef Z7_ST
  std::unique_ptr<CParallelFolderEncoder> parallelEncoder;
  int lastGroupWithNewFiles = -1;
  {
    unsigned numNewFiles = 0;
    FOR_VECTOR (g, filters)
    {
      const unsigned n = groups[filters[g].GroupIndex].Indices.Size();
      numNewFiles += n;
      if (n != 0)
        lastGroupWithNewFiles = (int)g;
    }
    if (options.ParallelFolderEncoding && options.Method->NumThreads > 1 && numNewFiles > 1)
    {
      parallelEncoder = std::make_unique<CParallelFolderEncoder>(newFolderCtx,
          options.Method->NumThreads, options.Method->MemoryUsageLimit);
      if (parallelEncoder->Start() != S_OK)
        parallelEncoder.reset(); // threads can't be created: sequential compression
    }
  }
 #endif

  for (unsigned groupIndex = 0; groupIndex < filters.Size(); groupIndex++)
  {
    const CFilterMode2 &filterMode = filters[groupIndex];

    CCompressionMethodMode method = *options.Method;
    // not replaced (other method chosen by the user): the group is like a group without filter
    if (filterMode.Id != k_FLOAT_FilterGroup || !UseFloatMethod(method))
    {
      CFilterMode noFloat = filterMode;
      if (noFloat.Id == k_FLOAT_FilterGroup)
        noFloat.ClearFilterMode();
      const HRESULT res = MakeExeMethod(method, noFloat,
        // bcj2_IsAllowed:
        #ifdef Z7_ST
          false
        #else
          options.MaxFilter && options.MultiThreadMixer
        #endif
        , options.DisabledFilterIDs);

      RINOK(res)
    }

    if (filterMode.Encrypted)
    {
      if (!method.PasswordIsDefined)
      {
        #ifndef Z7_NO_CRYPTO
        if (getPasswordSpec)
          method.Password = getPasswordSpec->Password;
        #endif
        method.PasswordIsDefined = true;
      }
    }
    else
    {
      method.PasswordIsDefined = false;
      method.Password.Empty();
    }

    CEncoder encoder(method);

    // ---------- Repack and copy old solid blocks ----------

    const CSolidGroup &group = groups[filterMode.GroupIndex];

   #ifndef Z7_ST
    if (parallelEncoder)
    {
      // the old folders are written directly to the archive: after the queued ones
      if (!group.folderRefs.IsEmpty())
      {
        RINOK(parallelEncoder->Flush())
      }
      parallelEncoder->BeginGroup(method);
    }
   #endif
    
    FOR_VECTOR (folderRefIndex, group.folderRefs)
    {
      const CFolderRepack &rep = group.folderRefs[folderRefIndex];

      const unsigned folderIndex = rep.FolderIndex;
      
      const CNum numUnpackStreams = db->NumUnpackStreamsVector[folderIndex];

      if (rep.NumCopyFiles == numUnpackStreams)
      {
        if (opCallback)
        {
          RINOK(opCallback->ReportOperation(
              NEventIndexType::kBlockIndex, (UInt32)folderIndex,
              NUpdateNotifyOp::kReplicate))

          // ---------- Copy old solid block ----------
          {
            CNum indexInFolder = 0;
            for (CNum fi = db->FolderStartFileIndex[folderIndex]; indexInFolder < numUnpackStreams; fi++)
            {
              if (db->Files[fi].HasStream)
              {
                indexInFolder++;
                RINOK(opCallback->ReportOperation(
                    NEventIndexType::kInArcIndex, (UInt32)fi,
                    NUpdateNotifyOp::kReplicate))
              }
            }
          }
        }

        const UInt64 packSize = db->GetFolderFullPackSize(folderIndex);
        RINOK(WriteRange(inStream, archive.SeqStream,
            db->GetFolderStreamPos(folderIndex, 0), packSize, lps))
        lps->ProgressOffset += packSize;

        const unsigned folderIndex_New = newDatabase.Folders.Size();
        CFolder &folder = newDatabase.Folders.AddNew();
        // v23.01: we copy FolderCrc, if FolderCrc was used
        if (db->FolderCRCs.ValidAndDefined(folderIndex))
          newDatabase.FolderUnpackCRCs.SetItem(folderIndex_New,
              true, db->FolderCRCs.Vals[folderIndex]);

        db->ParseFolderInfo(folderIndex, folder);
        const CNum startIndex = db->FoStartPackStreamIndex[folderIndex];
        FOR_VECTOR (j, folder.PackStreams)
        {
          newDatabase.PackSizes.Add(db->GetStreamPackSize(startIndex + j));
          // newDatabase.PackCRCsDefined.Add(db.PackCRCsDefined[startIndex + j]);
          // newDatabase.PackCRCs.Add(db.PackCRCs[startIndex + j]);
        }

        size_t indexStart = db->FoToCoderUnpackSizes[folderIndex];
        const size_t indexEnd = db->FoToCoderUnpackSizes[folderIndex + 1];
        for (; indexStart < indexEnd; indexStart++)
          newDatabase.CoderUnpackSizes.Add(db->CoderUnpackSizes.ConstData()[indexStart]);
      }
      else
      {
        // ---------- Repack old solid block ----------

        CBoolVector extractStatuses;
        
        CNum indexInFolder = 0;

        if (opCallback)
        {
          RINOK(opCallback->ReportOperation(
              NEventIndexType::kBlockIndex, (UInt32)folderIndex,
              NUpdateNotifyOp::kRepack))
        }

        /* We could reduce data size of decoded folder, if we don't need to repack
           last files in folder. But the gain in speed is small in most cases.
           So we unpack full folder. */
           
        UInt64 sizeToEncode = 0;
  
        /*
        UInt64 importantUnpackSize = 0;
        unsigned numImportantFiles = 0;
        UInt64 decodeSize = 0;
        */

        for (CNum fi = db->FolderStartFileIndex[folderIndex]; indexInFolder < numUnpackStreams; fi++)
        {
          bool needExtract = false;
          const CFileItem &file = db->Files[fi];
  
          if (file.HasStream)
          {
            indexInFolder++;
            const int updateIndex = fileIndexToUpdateIndexMap[fi];
            if (updateIndex >= 0 && !updateItems[(unsigned)updateIndex].NewData)
              needExtract = true;
            // decodeSize += file.Size;
          }
          
          extractStatuses.Add(needExtract);
          if (needExtract)
          {
            sizeToEncode += file.Size;
            /*
            numImportantFiles = extractStatuses.Size();
            importantUnpackSize = decodeSize;
            */
          }
        }

        // extractStatuses.DeleteFrom(numImportantFiles);

        unsigned startPackIndex = newDatabase.PackSizes.Size();
        UInt64 curUnpackSize;
        {
          CMyComPtr<ISequentialInStream> sbInStream;
          CRepackStreamBase *repackBase;
          CFolderInStream2 *FosSpec2 = NULL;

          CRepackInStreamWithSizes *inStreamSizeCountSpec = new CRepackInStreamWithSizes;
          CMyComPtr<ISequentialInStream> inStreamSizeCount = inStreamSizeCountSpec;
          {
            #ifndef Z7_ST
            if (options.MultiThreadMixer)
            {
              repackBase = threadDecoder.FosSpec;
              CMyComPtr<ISequentialOutStream> sbOutStream;
              sb.CreateStreams2(sbInStream, sbOutStream);
              RINOK(sb.Create_ReInit())
              
              threadDecoder.FosSpec->_stream = sbOutStream;
              
              threadDecoder.InStream = inStream;
              threadDecoder.StartPos = db->ArcInfo.DataStartPosition; // db->GetFolderStreamPos(folderIndex, 0);
              threadDecoder.Folders = (const CFolders *)db;
              threadDecoder.FolderIndex = folderIndex;
             
              // threadDecoder.UnpackSize = importantUnpackSize;
              // threadDecoder.send_UnpackSize = true;
            }
            else
            #endif
            {
              FosSpec2 = new CFolderInStream2;
              FosSpec2->Init();
              sbInStream = FosSpec2;
              repackBase = FosSpec2;

              #ifndef Z7_NO_CRYPTO
              bool isEncrypted = false;
              bool passwordIsDefined = false;
              UString password;
              #endif
              
              CMyComPtr<ISequentialInStream> decodedStream;
              bool dataAfterEnd_Error = false;

              const HRESULT res = threadDecoder.Decoder.Decode(
                  EXTERNAL_CODECS_LOC_VARS
                  inStream,
                  db->ArcInfo.DataStartPosition, // db->GetFolderStreamPos(folderIndex, 0);,
                  *db, folderIndex,
                  // &importantUnpackSize, // *unpackSize
                  NULL, // *unpackSize : FULL unpack
                
                  NULL, // *outStream
                  NULL, // *compressProgress

                  &decodedStream
                  , dataAfterEnd_Error
                
                  Z7_7Z_DECODER_CRYPRO_VARS
                  #ifndef Z7_ST
                    , false // mtMode
                    , 1 // numThreads
                    , 0 // memUsage
                  #endif
                );
          
              RINOK(res)
              if (!decodedStream)
                return E_FAIL;

              FosSpec2->_inStream = decodedStream;
            }

            repackBase->_db = db;
            repackBase->_opCallback = opCallback;
            repackBase->_extractCallback = extractCallback;

            UInt32 startIndex = db->FolderStartFileIndex[folderIndex];
            RINOK(repackBase->Init(startIndex, &extractStatuses))

            inStreamSizeCountSpec->_db = db;
            inStreamSizeCountSpec->Init(sbInStream, startIndex, &extractStatuses);

            #ifndef Z7_ST
            if (options.MultiThreadMixer)
            {
              WRes wres = threadDecoder.Start();
              if (wres != 0)
                return HRESULT_FROM_WIN32(wres);
            }
            #endif
          }

          // curUnpackSize = sizeToEncode;

          HRESULT encodeRes = encoder.Encode1(
              EXTERNAL_CODECS_LOC_VARS
              inStreamSizeCount,
              // NULL,
              &inSizeForReduce,
              sizeToEncode, // expectedDataSize
              newDatabase.Folders.AddNew(),
              // newDatabase.CoderUnpackSizes, curUnpackSize,
              archive.SeqStream, newDatabase.PackSizes, lps);

          if (encodeRes == k_My_HRESULT_CRC_ERROR)
            return E_FAIL;

          curUnpackSize = inStreamSizeCountSpec->GetSize();

          if (encodeRes == S_OK)
          {
            encoder.Encode_Post(curUnpackSize, newDatabase.CoderUnpackSizes);
          }

          #ifndef Z7_ST
          if (options.MultiThreadMixer)
          {
            // 16.00: hang was fixed : for case if decoding was not finished.
            // We close CBinderInStream and it calls CStreamBinder::CloseRead()
            inStreamSizeCount.Release();
            sbInStream.Release();
            
            {
              const WRes wres = threadDecoder.WaitExecuteFinish();
              if (wres != 0)
                return HRESULT_FROM_WIN32(wres);
            }
            
            const HRESULT decodeRes = threadDecoder.Result;
            // if (res == k_My_HRESULT_CRC_ERROR)
            if (decodeRes == S_FALSE || threadDecoder.dataAfterEnd_Error)
            {
              if (extractCallback)
              {
                RINOK(extractCallback->ReportExtractResult(
                    NEventIndexType::kInArcIndex, db->FolderStartFileIndex[folderIndex],
                    // NEventIndexType::kBlockIndex, (UInt32)folderIndex,
                    (decodeRes != S_OK ?
                      NExtract::NOperationResult::kDataError :
                      NExtract::NOperationResult::kDataAfterEnd)))
              }
              if (decodeRes != S_OK)
                return E_FAIL;
            }
            RINOK(decodeRes)
            if (encodeRes == S_OK)
              if (sb.ProcessedSize != sizeToEncode)
                encodeRes = E_FAIL;
          }
          else
          #endif
          {
            if (FosSpec2->Result == S_FALSE)
            {
              if (extractCallback)
              {
                RINOK(extractCallback->ReportExtractResult(
                    NEventIndexType::kBlockIndex, (UInt32)folderIndex,
                    NExtract::NOperationResult::kDataError))
              }
              return E_FAIL;
            }
            RINOK(FosSpec2->Result)
          }

          RINOK(encodeRes)
          RINOK(repackBase->CheckFinishedState())

          if (curUnpackSize != sizeToEncode)
            return E_FAIL;
        }

        for (; startPackIndex < newDatabase.PackSizes.Size(); startPackIndex++)
          lps->OutSize += newDatabase.PackSizes[startPackIndex];
        lps->InSize += curUnpackSize;
      }
      
      newDatabase.NumUnpackStreamsVector.Add(rep.NumCopyFiles);
      
      CNum indexInFolder = 0;
      for (CNum fi = db->FolderStartFileIndex[folderIndex]; indexInFolder < numUnpackStreams; fi++)
      {
        if (db->Files[fi].HasStream)
        {
          indexInFolder++;
          const int updateIndex = fileIndexToUpdateIndexMap[fi];
          if (updateIndex >= 0)
          {
            const CUpdateItem &ui = updateItems[(unsigned)updateIndex];
            if (ui.NewData)
              continue;

            UString name;
            CFileItem file;
            CFileItem2 file2;
            GetFile(*db, fi, file, file2);

            if (ui.NewProps)
            {
              UpdateItem_To_FileItem2(ui, file2);
              file.IsDir = ui.IsDir;
              name = ui.Name;
            }
            else
              db->GetPath(fi, name);

            /*
            file.Parent = ui.ParentFolderIndex;
            if (ui.TreeFolderIndex >= 0)
              treeFolderToArcIndex[ui.TreeFolderIndex] = newDatabase.Files.Size();
            if (totalSecureDataSize != 0)
              newDatabase.SecureIDs.Add(ui.SecureIndex);
            */
            newDatabase.AddFile(file, file2, name);
          }
        }
      }
    }


    // ---------- Compress files to new solid blocks ----------

    const unsigned numFiles = group.Indices.Size();
    if (numFiles == 0)
      continue;
    CRecordVector<CRefItem> refItems;
    refItems.ClearAndSetSize(numFiles);
    // bool sortByType = (options.UseTypeSorting && isSoid); // numSolidFiles > 1
    const bool sortByType = options.UseTypeSorting;
    
    unsigned i;

    for (i = 0; i < numFiles; i++)
      refItems[i].Construct(group.Indices[i], updateItems[group.Indices[i]], sortByType);

    CSortParam sortParam;
    // sortParam.TreeFolders = &treeFolders;
    sortParam.SortByType = sortByType;
    refItems.Sort(CompareUpdateItems, (void *)&sortParam);
    
    CObjArray<UInt32> indices(numFiles);

    for (i = 0; i < numFiles; i++)
    {
      const UInt32 index = refItems[i].Index;
      indices[i] = index;
      /*
      const CUpdateItem &ui = updateItems[index];
      CFileItem file;
      if (ui.NewProps)
        UpdateItem_To_FileItem(ui, file);
      else
        file = db.Files[ui.IndexInArchive];
      if (file.IsAnti || file.IsDir)
        return E_FAIL;
      newDatabase.Files.Add(file);
      */
    }
    
    for (i = 0; i < numFiles;)
    {
      UInt64 totalSize = 0;
      unsigned numSubFiles;
      
      const wchar_t *prevExtension = NULL;
      
      for (numSubFiles = 0; i + numSubFiles < numFiles && numSubFiles < numSolidFiles; numSubFiles++)
      {
        const CUpdateItem &ui = updateItems[indices[i + numSubFiles]];
        totalSize += ui.Size;
        if (totalSize > options.NumSolidBytes)
          break;
        if (options.SolidExtension)
        {
          const int slashPos = ui.Name.ReverseFind_PathSepar();
          const int dotPos = ui.Name.ReverseFind_Dot();
          const wchar_t *ext = ui.Name.Ptr(dotPos <= slashPos ? ui.Name.Len() : (unsigned)(dotPos + 1));
          if (numSubFiles == 0)
            prevExtension = ext;
          else if (!StringsAreEqualNoCase(ext, prevExtension))
            break;
        }
      }

      if (numSubFiles < 1)
        numSubFiles = 1;

      RINOK(lps->SetCur())

      /*
      const unsigned folderIndex = newDatabase.NumUnpackStreamsVector.Size();

      if (opCallback)
      {
        RINOK(opCallback->ReportOperation(
            NEventIndexType::kBlockIndex, (UInt32)folderIndex,
            NUpdateNotifyOp::kAdd));
      }
      */


     #ifndef Z7_ST
      if (parallelEncoder)
      {
        bool done;
        RINOK(parallelEncoder->AddFolder(&indices[i], numSubFiles, totalSize,
            i + numSubFiles == numFiles && (int)groupIndex == lastGroupWithNewFiles,
            encoder, done))
        if (done)
        {
          i += numSubFiles;
          continue;
        }
        // not a folder for the parallel path: the previous folders were written
      }
     #endif

      {
        CMyComPtr2_Create<ISequentialInStream, CFolderInStream> inStreamSpec; // solidInStream;
        SetFolderInStreamOptions(*inStreamSpec.ClsPtr(), options);
        inStreamSpec->Init(updateCallback, &indices[i], numSubFiles);
        RINOK(EncodeNewFolderInline(newFolderCtx, encoder,
            inStreamSpec, *inStreamSpec.ClsPtr(),
            &indices[i], numSubFiles, totalSize))
      }
      i += numSubFiles;

      /*
      if (reportArcProp)
      {
        PROPVARIANT prop;
        prop.vt = VT_EMPTY;
        prop.wReserved1 = 0;
        {
          NWindows::NCOM::PropVarEm_Set_UInt32(&prop, numProcessedFiles);
          RINOK(reportArcProp->ReportProp(
              NEventIndexType::kBlockIndex, (UInt32)folderIndex, kpidNumSubFiles, &prop));
        }
        {
          NWindows::NCOM::PropVarEm_Set_UInt64(&prop, curFolderUnpackSize);
          RINOK(reportArcProp->ReportProp(
              NEventIndexType::kBlockIndex, (UInt32)folderIndex, kpidSize, &prop));
        }
        {
          NWindows::NCOM::PropVarEm_Set_UInt64(&prop, packSize);
          RINOK(reportArcProp->ReportProp(
              NEventIndexType::kBlockIndex, (UInt32)folderIndex, kpidPackSize, &prop));
        }
        {
          NWindows::NCOM::PropVarEm_Set_UInt32(&prop, numStreams);
          RINOK(reportArcProp->ReportProp(
              NEventIndexType::kBlockIndex, (UInt32)folderIndex, kpidNumStreams, &prop));
        }
        RINOK(reportArcProp->ReportFinished(NEventIndexType::kBlockIndex, (UInt32)folderIndex, NUpdate::NOperationResult::kOK));
      }
      */
      /*
      if (opCallback)
      {
        RINOK(opCallback->ReportOperation(
            NEventIndexType::kBlockIndex, (UInt32)folderIndex,
            NUpdateNotifyOp::kOpFinished));
      }
      */
    }
  }

 #ifndef Z7_ST
  if (parallelEncoder)
  {
    RINOK(parallelEncoder->Flush())
    parallelEncoder.reset();
  }
 #endif

  RINOK(lps->SetCur())

  /*
  fileIndexToUpdateIndexMap.ClearAndFree();
  groups.ClearAndFree();
  */

  /*
  for (i = 0; i < newDatabase.Files.Size(); i++)
  {
    CFileItem &file = newDatabase.Files[i];
    file.Parent = treeFolderToArcIndex[file.Parent];
  }

  if (totalSecureDataSize != 0)
  {
    newDatabase.SecureBuf.SetCapacity(totalSecureDataSize);
    size_t pos = 0;
    newDatabase.SecureSizes.Reserve(secureBlocks.Sorted.Size());
    for (i = 0; i < secureBlocks.Sorted.Size(); i++)
    {
      const CByteBuffer &buf = secureBlocks.Bufs[secureBlocks.Sorted[i]];
      size_t size = buf.GetCapacity();
      if (size != 0)
        memcpy(newDatabase.SecureBuf + pos, buf, size);
      newDatabase.SecureSizes.Add((UInt32)size);
      pos += size;
    }
  }
  */

  {
    const unsigned numFolders = newDatabase.Folders.Size();
    if (newDatabase.NumUnpackStreamsVector.Size() != numFolders
        || newDatabase.FolderUnpackCRCs.Defs.Size() > numFolders)
      return E_FAIL;
    newDatabase.FolderUnpackCRCs.if_NonEmpty_FillResidue_with_false(numFolders);
  }
  
  updateItems.ClearAndFree();
  newDatabase.ReserveDown();

  if (opCallback)
    RINOK(opCallback->ReportOperation(NEventIndexType::kNoIndex, (UInt32)(Int32)-1, NUpdateNotifyOp::kHeader))

  RINOK(archive.WriteDatabase(EXTERNAL_CODECS_LOC_VARS
      newDatabase, options.HeaderMethod, options.HeaderOptions))

  if (v_StreamSetRestriction)
    RINOK(v_StreamSetRestriction->SetRestriction(0, 0))

  return S_OK;
}

}}
