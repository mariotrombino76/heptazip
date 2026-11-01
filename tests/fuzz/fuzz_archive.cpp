// fuzz_archive.cpp -- harness libFuzzer per i gestori dei formati di archivio (design 0007)
//
// Per ogni input: apre i dati con il gestore del formato scelto, legge le proprieta' di
// ogni elemento ed estrae tutto verso uno stream che scarta i dati (fino a 256 MiB).
// Con ASan+UBSan ogni accesso fuori dai limiti o comportamento indefinito diventa un
// crash riproducibile.
//
//   FUZZ_FORMAT=7z ./fuzz_archive -malloc_limit_mb=1024 corpus/7z
//
// FUZZ_FORMAT: nome del formato come per -t (7z, zip, rar, cab, iso, tar, xz, ...).
// Il limite di memoria dei decoder che lo chiedono (IArchiveRequestMemoryUseCallback)
// e' kMemLimit; per gli altri vale -malloc_limit_mb di libFuzzer.
//
// Senza libFuzzer (FUZZ_REPLAY definito) lo stesso codice diventa fuzz_replay, che
// riesegue gli input salvati in tests/fuzz/regress/<formato>/ (test CTest fuzz_regress):
//
//   fuzz_replay <dir_regress>

#include "../../CPP/Common/Common.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef FUZZ_REPLAY
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#endif

#include "../../CPP/Common/MyWindows.h"
#include "../../CPP/Common/MyInitGuid.h"

#include "../../CPP/Common/MyCom.h"
#include "../../CPP/Common/MyString.h"
#include "../../CPP/Common/StringConvert.h"
#include "../../CPP/Windows/PropVariant.h"

#include "../../CPP/7zip/Common/StreamObjects.h"
#include "../../CPP/7zip/Archive/IArchive.h"
#include "../../CPP/7zip/IPassword.h"
#include "../../CPP/7zip/UI/Common/LoadCodecs.h"
#include "../../CPP/7zip/UI/Common/IFileExtractCallback.h"

namespace {

const UInt64 kMemLimit = (UInt64)512 << 20;

// Byte estratti per input: oltre, la scrittura fallisce e l'estrazione si ferma. Le
// immagini disco (sparse, qcow, vhd...) possono descrivere legittimamente dischi di
// terabyte fatti di zeri in poche decine di byte: senza limite sarebbero timeout, non bug.
const UInt64 kMaxOutputPerInput = (UInt64)256 << 20;
UInt64 g_OutputTotal;

Z7_CLASS_IMP_COM_1(
  CNullOutStream
  , ISequentialOutStream
)
};

Z7_COM7F_IMF(CNullOutStream::Write(const void *, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (g_OutputTotal + size > kMaxOutputPerInput)
    return E_FAIL;
  g_OutputTotal += size;
  if (processedSize)
    *processedSize = size;
  return S_OK;
}


class COpenCallback Z7_final:
  public IArchiveOpenCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IArchiveOpenCallback, ICryptoGetTextPassword)
};

Z7_COM7F_IMF(COpenCallback::SetTotal(const UInt64 *, const UInt64 *)) { return S_OK; }
Z7_COM7F_IMF(COpenCallback::SetCompleted(const UInt64 *, const UInt64 *)) { return S_OK; }
Z7_COM7F_IMF(COpenCallback::CryptoGetTextPassword(BSTR *password))
{
  return StringToBstr(L"fuzz", password);
}


class CExtractCallback Z7_final:
  public IArchiveExtractCallback,
  public ICryptoGetTextPassword,
  public IArchiveRequestMemoryUseCallback,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_3(IArchiveExtractCallback, ICryptoGetTextPassword, IArchiveRequestMemoryUseCallback)
  Z7_IFACE_COM7_IMP(IProgress)
};

Z7_COM7F_IMF(CExtractCallback::SetTotal(UInt64)) { return S_OK; }
Z7_COM7F_IMF(CExtractCallback::SetCompleted(const UInt64 *)) { return S_OK; }

Z7_COM7F_IMF(CExtractCallback::GetStream(UInt32, ISequentialOutStream **outStream, Int32 askExtractMode))
{
  *outStream = NULL;
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract)
    return S_OK;
  CMyComPtr<ISequentialOutStream> s = new CNullOutStream;
  *outStream = s.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::PrepareOperation(Int32)) { return S_OK; }
Z7_COM7F_IMF(CExtractCallback::SetOperationResult(Int32)) { return S_OK; }

Z7_COM7F_IMF(CExtractCallback::CryptoGetTextPassword(BSTR *password))
{
  return StringToBstr(L"fuzz", password);
}

Z7_COM7F_IMF(CExtractCallback::RequestMemoryUse(UInt32 flags, UInt32, UInt32, const wchar_t *,
    UInt64 requiredSize, UInt64 *allowedSize, UInt32 *answerFlags))
{
  if (*allowedSize > kMemLimit)
    *allowedSize = kMemLimit;
  if ((flags & NRequestMemoryUseFlags::k_IsReport) == 0 && requiredSize > kMemLimit)
    *answerFlags = NRequestMemoryAnswerFlags::k_SkipArc | NRequestMemoryAnswerFlags::k_Limit_Exceeded;
  return S_OK;
}

CCodecs *g_Codecs;
int g_FormatIndex = -1;

const PROPID kProps[] = { kpidPath, kpidIsDir, kpidSize, kpidPackSize, kpidMTime, kpidAttrib,
    kpidCRC, kpidMethod, kpidEncrypted, kpidBlock, kpidSymLink, kpidHardLink, kpidComment };

}


static bool SelectFormat(const char *format)
{
  if (!g_Codecs)
  {
    g_Codecs = new CCodecs;
    if (g_Codecs->Load() != S_OK)
      return false;
  }
  g_FormatIndex = g_Codecs->FindFormatForArchiveType(GetUnicodeString(AString(format)));
  if (g_FormatIndex < 0)
  {
    fprintf(stderr, "formato sconosciuto: %s\n", format);
    return false;
  }
  return true;
}

extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
  const char *format = getenv("FUZZ_FORMAT");
  if (!format)
  {
    fprintf(stderr, "FUZZ_FORMAT non impostata (es. FUZZ_FORMAT=7z)\n");
    exit(2);
  }
  if (!SelectFormat(format))
    exit(2);
  return 0;
}


extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  g_OutputTotal = 0;
  CMyComPtr<IInArchive> archive;
  if (g_Codecs->CreateInArchive((unsigned)g_FormatIndex, archive) != S_OK || !archive)
    return 0;

  CMyComPtr2_Create<IInStream, CBufInStream> stream;
  stream->Init(data, size);

  CMyComPtr<IArchiveOpenCallback> openCallback = new COpenCallback;
  const UInt64 maxCheckStartPosition = 1 << 16;
  if (archive->Open(stream, &maxCheckStartPosition, openCallback) != S_OK)
  {
    archive->Close();
    return 0;
  }

  UInt32 numItems = 0;
  if (archive->GetNumberOfItems(&numItems) == S_OK)
  {
    const UInt32 kMaxItemsForProps = 1 << 12;
    for (UInt32 i = 0; i < numItems && i < kMaxItemsForProps; i++)
      for (unsigned k = 0; k < Z7_ARRAY_SIZE(kProps); k++)
      {
        NWindows::NCOM::CPropVariant prop;
        archive->GetProperty(i, kProps[k], &prop);
      }
    CMyComPtr<IArchiveExtractCallback> extractCallback = new CExtractCallback;
    // estrazione (non test) verso CNullOutStream: i dati passano tutti dallo stream, che
    // applica kMaxOutputPerInput (in modalita' test molti gestori non chiedono lo stream)
    archive->Extract(NULL, (UInt32)(Int32)-1, 0, extractCallback);
  }
  archive->Close();
  return 0;
}


#ifdef FUZZ_REPLAY

static std::vector<std::string> ListDir(const std::string &dir, bool wantDirs)
{
  std::vector<std::string> names;
  DIR *d = opendir(dir.c_str());
  if (!d)
    return names;
  while (const struct dirent *e = readdir(d))
  {
    if (e->d_name[0] == '.')
      continue;
    struct stat st;
    if (stat((dir + "/" + e->d_name).c_str(), &st) != 0)
      continue;
    const bool isDir = S_ISDIR(st.st_mode);
    if (isDir == wantDirs)
      names.push_back(e->d_name);
  }
  closedir(d);
  return names;
}

static bool ReadFile(const std::string &path, std::vector<uint8_t> &data)
{
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return false;
  data.clear();
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) != 0)
    data.insert(data.end(), buf, buf + n);
  fclose(f);
  return true;
}

// Riesegue <dir>/<formato>/<file>: un crash (o un errore dei sanitizer) fa fallire il test.
int main(int argc, char **argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "uso: %s <dir_regress>\n", argv[0]);
    return 2;
  }
  const std::string root = argv[1];
  unsigned numFiles = 0;
  for (const std::string &format : ListDir(root, true))
  {
    if (!SelectFormat(format.c_str()))
      return 1;
    for (const std::string &name : ListDir(root + "/" + format, false))
    {
      std::vector<uint8_t> data;
      if (!ReadFile(root + "/" + format + "/" + name, data))
        return 1;
      LLVMFuzzerTestOneInput(data.data(), data.size());
      numFiles++;
    }
  }
  printf("fuzz_replay: %u input rieseguiti\n", numFiles);
  return numFiles != 0 ? 0 : 1;
}

#endif
