// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// extract_trace.cpp
//
// Strumento di test: apre un archivio .7z con l'handler 7z (senza la UI) ed esegue
// IInArchive::Extract() con un IArchiveExtractCallback che REGISTRA ogni chiamata.
// La traccia stampata su stdout serve a verificare che due configurazioni (es.
// estrazione sequenziale vs parallela, vedi docs/design/0001-*) o due versioni del
// codice producano esattamente la stessa sequenza osservabile di callback.
//
// La traccia contiene, nell'ordine:
//   OPEN, ITEMS, SETTOTAL, GETSTREAM, PREPARE, RESULT (con dimensione e CRC32 dei
//   byte ricevuti per l'elemento), REPORT (IArchiveExtractCallbackMessage2), EXTRACT.
// NON contiene, perche' non fanno parte del contratto osservabile:
//   - la granularita' delle singole Write() (conta solo la sequenza dei byte);
//   - l'avanzamento (SetCompleted);
//   - il numero/momento delle richieste di password (riportato solo su stderr).
//
// Uso:
//   extract_trace <archivio> [-t] [-p<password>] [-i<i,j,...>] [-m<nome>=<valore>]...
//                 [--abort-at-file=N] [--fail-write-at-file=N]
//
//   -t                    modalita' test (askMode = kTest), altrimenti kExtract
//   -i                    estrae solo gli indici indicati (come la selezione della UI)
//   -m                    proprieta' dell'handler, stessa sintassi di 7zz (-mmt=1, -mmemuse=...)
//   --abort-at-file=N     la N-esima GetStream() (da 1) restituisce E_ABORT (annullamento)
//   --fail-write-at-file=N  lo stream della N-esima GetStream() fallisce alla prima
//                         Write() con E_FAIL (simula disco pieno)
//
// Exit code: 0 = traccia prodotta; 2 = argomenti non validi;
//            3 = VIOLAZIONE del contratto (callback chiamate in concorrenza).

#include "../CPP/Common/Common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "../CPP/Common/MyWindows.h"
#include "../CPP/Common/MyInitGuid.h"

#include "../CPP/Common/MyCom.h"
#include "../CPP/Common/MyString.h"
#include "../CPP/Common/StringConvert.h"
#include "../CPP/Common/StringToInt.h"

#include "../CPP/7zip/Common/FileStreams.h"
#include "../CPP/7zip/Archive/IArchive.h"
#include "../CPP/7zip/Archive/7z/7zHandler.h"
#include "../CPP/7zip/Archive/Zip/ZipHandler.h"  // Heptazip (#86): -z opens a zip
#include "../CPP/7zip/IPassword.h"
#include "../CPP/7zip/UI/Common/SetProperties.h"
// Non usato direttamente: incluso dopo MyInitGuid.h per DEFINIRE i GUID delle
// interfacce della UI referenziati da UI/Common (in 7zz li definisce Main.cpp,
// che qui non e' linkato).
#include "../CPP/7zip/UI/Common/IFileExtractCallback.h"

#include "../C/7zCrc.h"

namespace {

// ---------------------------------------------------------------------------
// Rilevazione di chiamate concorrenti.
// IArchive.h: GetStream / PrepareOperation / SetOperationResult non devono mai
// essere chiamate da thread diversi contemporaneamente. Estendiamo il controllo
// a Write() e ReportExtractResult(), che nel nostro modello sono serializzate
// con le precedenti.
// ---------------------------------------------------------------------------

std::atomic<int> g_CallDepth(0);
std::atomic<bool> g_ConcurrencyViolation(false);

class CCallGuard
{
public:
  CCallGuard()
  {
    if (g_CallDepth.fetch_add(1) != 0)
      g_ConcurrencyViolation = true;
  }
  ~CCallGuard() { g_CallDepth.fetch_sub(1); }
};

void PrintHResult(const char *tag, HRESULT hr)
{
  printf("%s hr=0x%08X\n", tag, (unsigned)hr);
}

// ---------------------------------------------------------------------------
// Stream di output: calcola dimensione e CRC32 dei byte ricevuti.
// ---------------------------------------------------------------------------

Z7_CLASS_IMP_COM_1(
  CRecordOutStream
  , ISequentialOutStream
)
public:
  UInt64 Size;
  UInt32 Crc;
  bool FailOnWrite;

  CRecordOutStream(): Size(0), Crc(CRC_INIT_VAL), FailOnWrite(false) {}
};

Z7_COM7F_IMF(CRecordOutStream::Write(const void *data, UInt32 size, UInt32 *processedSize))
{
  CCallGuard guard;
  if (processedSize)
    *processedSize = 0;
  if (FailOnWrite && size != 0)
    return E_FAIL;
  Crc = CrcUpdate(Crc, data, size);
  Size += size;
  if (processedSize)
    *processedSize = size;
  return S_OK;
}

// ---------------------------------------------------------------------------
// Callback di apertura (serve solo per gli archivi con intestazioni cifrate).
// ---------------------------------------------------------------------------

class COpenCallback Z7_final:
  public IArchiveOpenCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IArchiveOpenCallback, ICryptoGetTextPassword)
public:
  bool PasswordIsDefined;
  UString Password;
  COpenCallback(): PasswordIsDefined(false) {}
};

Z7_COM7F_IMF(COpenCallback::SetTotal(const UInt64 *, const UInt64 *)) { return S_OK; }
Z7_COM7F_IMF(COpenCallback::SetCompleted(const UInt64 *, const UInt64 *)) { return S_OK; }

Z7_COM7F_IMF(COpenCallback::CryptoGetTextPassword(BSTR *password))
{
  if (!PasswordIsDefined)
    return E_ABORT;
  return StringToBstr(Password, password);
}

// ---------------------------------------------------------------------------
// Callback di estrazione che registra la traccia.
// ---------------------------------------------------------------------------

class CTraceExtractCallback Z7_final:
  public IArchiveExtractCallback,
  public IArchiveExtractCallbackMessage2,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_3(IArchiveExtractCallback, IArchiveExtractCallbackMessage2, ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IProgress)

  UInt32 _curIndex;
  bool _curIndexDefined;
  CRecordOutStream *_curStreamSpec;
  CMyComPtr<ISequentialOutStream> _curStream;
  unsigned _numGetStreamCalls;

public:
  bool PasswordIsDefined;
  UString Password;
  unsigned NumPasswordRequests;
  unsigned AbortAtFile;      // 0 = disabilitato
  unsigned FailWriteAtFile;  // 0 = disabilitato

  CTraceExtractCallback():
      _curIndex(0),
      _curIndexDefined(false),
      _curStreamSpec(NULL),
      _numGetStreamCalls(0),
      PasswordIsDefined(false),
      NumPasswordRequests(0),
      AbortAtFile(0),
      FailWriteAtFile(0)
      {}
};

Z7_COM7F_IMF(CTraceExtractCallback::SetTotal(UInt64 size))
{
  printf("SETTOTAL %llu\n", (unsigned long long)size);
  return S_OK;
}

Z7_COM7F_IMF(CTraceExtractCallback::SetCompleted(const UInt64 *))
{
  // l'avanzamento non fa parte della traccia (vedi intestazione del file)
  return S_OK;
}

Z7_COM7F_IMF(CTraceExtractCallback::GetStream(UInt32 index, ISequentialOutStream **outStream, Int32 askExtractMode))
{
  CCallGuard guard;
  *outStream = NULL;
  _numGetStreamCalls++;
  _curIndex = index;
  _curIndexDefined = true;
  _curStream.Release();
  _curStreamSpec = NULL;

  if (AbortAtFile != 0 && _numGetStreamCalls == AbortAtFile)
  {
    printf("GETSTREAM %u %d -> abort\n", (unsigned)index, (int)askExtractMode);
    return E_ABORT;
  }

  // Come la UI: si fornisce uno stream solo in modalita' kExtract.
  if (askExtractMode == NArchive::NExtract::NAskMode::kExtract)
  {
    _curStreamSpec = new CRecordOutStream;
    _curStream = _curStreamSpec;
    _curStreamSpec->FailOnWrite = (FailWriteAtFile != 0 && _numGetStreamCalls == FailWriteAtFile);
    *outStream = _curStream;
    _curStream->AddRef();
  }
  printf("GETSTREAM %u %d -> %s\n", (unsigned)index, (int)askExtractMode,
      _curStreamSpec ? (_curStreamSpec->FailOnWrite ? "stream(fail-write)" : "stream") : "null");
  return S_OK;
}

Z7_COM7F_IMF(CTraceExtractCallback::PrepareOperation(Int32 askExtractMode))
{
  CCallGuard guard;
  printf("PREPARE %d\n", (int)askExtractMode);
  return S_OK;
}

Z7_COM7F_IMF(CTraceExtractCallback::SetOperationResult(Int32 opRes))
{
  CCallGuard guard;
  if (_curStreamSpec)
    printf("RESULT %u %d size=%llu crc=%08X\n",
        (unsigned)_curIndex, (int)opRes,
        (unsigned long long)_curStreamSpec->Size,
        (unsigned)CRC_GET_DIGEST(_curStreamSpec->Crc));
  else if (_curIndexDefined)
    printf("RESULT %u %d\n", (unsigned)_curIndex, (int)opRes);
  else
    printf("RESULT ? %d\n", (int)opRes);
  _curStream.Release();
  _curStreamSpec = NULL;
  return S_OK;
}

Z7_COM7F_IMF(CTraceExtractCallback::ReportExtractResult(UInt32 indexType, UInt32 index, Int32 opRes))
{
  CCallGuard guard;
  printf("REPORT %u %u %d\n", (unsigned)indexType, (unsigned)index, (int)opRes);
  return S_OK;
}

Z7_COM7F_IMF(CTraceExtractCallback::CryptoGetTextPassword(BSTR *password))
{
  // NON fa parte della traccia: la modalita' parallela puo' chiedere la password
  // in un momento diverso (docs/design/0001, par. 5.8).
  NumPasswordRequests++;
  if (!PasswordIsDefined)
    return E_ABORT;
  return StringToBstr(Password, password);
}

// ---------------------------------------------------------------------------
// Parsing degli argomenti
// ---------------------------------------------------------------------------

struct COptions
{
  const char *ArcPath;
  bool Zip;          // -z: the zip handler instead of the 7z one (Heptazip, #86)
  bool TestMode;
  bool PasswordIsDefined;
  UString Password;
  bool IndicesDefined;
  std::vector<UInt32> Indices;
  CObjectVector<CProperty> Props;
  unsigned AbortAtFile;
  unsigned FailWriteAtFile;

  COptions():
      ArcPath(NULL),
      Zip(false),
      TestMode(false),
      PasswordIsDefined(false),
      IndicesDefined(false),
      AbortAtFile(0),
      FailWriteAtFile(0)
      {}
};

bool ParseUInt(const char *s, unsigned &res)
{
  const char *end;
  const UInt32 v = ConvertStringToUInt32(s, &end);
  if (end == s || *end != 0)
    return false;
  res = v;
  return true;
}

bool ParseIndices(const char *s, std::vector<UInt32> &indices)
{
  for (;;)
  {
    const char *end;
    const UInt32 v = ConvertStringToUInt32(s, &end);
    if (end == s)
      return false;
    indices.push_back(v);
    if (*end == 0)
      break;
    if (*end != ',')
      return false;
    s = end + 1;
  }
  // IInArchive::Extract() richiede indici crescenti e senza duplicati
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  return true;
}

bool ParseArgs(int argc, char **argv, COptions &o)
{
  for (int i = 1; i < argc; i++)
  {
    const char *a = argv[i];
    if (strcmp(a, "-t") == 0)
      o.TestMode = true;
    else if (strcmp(a, "-z") == 0)
      o.Zip = true;
    else if (strncmp(a, "-p", 2) == 0)
    {
      o.PasswordIsDefined = true;
      o.Password = GetUnicodeString(AString(a + 2));
    }
    else if (strncmp(a, "-i", 2) == 0)
    {
      o.IndicesDefined = true;
      if (!ParseIndices(a + 2, o.Indices))
        return false;
    }
    else if (strncmp(a, "-m", 2) == 0)
    {
      const char *eq = strchr(a + 2, '=');
      CProperty prop;
      if (eq)
      {
        AString name;
        name.SetFrom(a + 2, (unsigned)(eq - (a + 2)));
        prop.Name = GetUnicodeString(name);
        prop.Value = GetUnicodeString(AString(eq + 1));
      }
      else
        prop.Name = GetUnicodeString(AString(a + 2));
      if (prop.Name.IsEmpty())
        return false;
      o.Props.Add(prop);
    }
    else if (strncmp(a, "--abort-at-file=", 16) == 0)
    {
      if (!ParseUInt(a + 16, o.AbortAtFile))
        return false;
    }
    else if (strncmp(a, "--fail-write-at-file=", 21) == 0)
    {
      if (!ParseUInt(a + 21, o.FailWriteAtFile))
        return false;
    }
    else if (a[0] == '-' || o.ArcPath)
      return false;
    else
      o.ArcPath = a;
  }
  return o.ArcPath != NULL;
}

int Run(const COptions &o)
{
  CInFileStream *inStreamSpec = new CInFileStream;
  CMyComPtr<IInStream> inStream = inStreamSpec;
  if (!inStreamSpec->Open(o.ArcPath))
  {
    fprintf(stderr, "impossibile aprire: %s\n", o.ArcPath);
    return 2;
  }

  CMyComPtr<IInArchive> archive;
  if (o.Zip)
    archive = new NArchive::NZip::CHandler;   // Heptazip (#86)
  else
    archive = new NArchive::N7z::CHandler;

  COpenCallback *openCallbackSpec = new COpenCallback;
  CMyComPtr<IArchiveOpenCallback> openCallback = openCallbackSpec;
  openCallbackSpec->PasswordIsDefined = o.PasswordIsDefined;
  openCallbackSpec->Password = o.Password;

  const UInt64 kMaxCheckStartPosition = (UInt64)1 << 23;
  const HRESULT openRes = archive->Open(inStream, &kMaxCheckStartPosition, openCallback);
  PrintHResult("OPEN", openRes);
  if (openRes != S_OK)
    return 0;

  UInt32 numItems = 0;
  archive->GetNumberOfItems(&numItems);
  printf("ITEMS %u\n", (unsigned)numItems);

  // Stesse regole di 7zz per -m<nome>=<valore> (UI/Common/SetProperties.cpp)
  if (!o.Props.IsEmpty())
  {
    const HRESULT res = SetProperties(archive, o.Props);
    PrintHResult("SETPROPS", res);
    if (res != S_OK)
      return 0;
  }

  CTraceExtractCallback *callbackSpec = new CTraceExtractCallback;
  CMyComPtr<IArchiveExtractCallback> callback = callbackSpec;
  callbackSpec->PasswordIsDefined = o.PasswordIsDefined;
  callbackSpec->Password = o.Password;
  callbackSpec->AbortAtFile = o.AbortAtFile;
  callbackSpec->FailWriteAtFile = o.FailWriteAtFile;

  const UInt32 *indices = NULL;
  UInt32 num = (UInt32)(Int32)-1;
  if (o.IndicesDefined)
  {
    for (size_t k = 0; k < o.Indices.size(); k++)
      if (o.Indices[k] >= numItems)
      {
        fprintf(stderr, "indice fuori intervallo: %u\n", (unsigned)o.Indices[k]);
        return 2;
      }
    indices = o.Indices.empty() ? NULL : &o.Indices[0];
    num = (UInt32)o.Indices.size();
  }

  const HRESULT extractRes = archive->Extract(indices, num, o.TestMode ? 1 : 0, callback);
  PrintHResult("EXTRACT", extractRes);
  archive->Close();

  fprintf(stderr, "richieste di password: %u\n", callbackSpec->NumPasswordRequests);
  if (g_ConcurrencyViolation)
  {
    printf("VIOLATION callback chiamate in concorrenza\n");
    return 3;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv)
{
  COptions options;
  if (!ParseArgs(argc, argv, options))
  {
    fprintf(stderr,
        "uso: extract_trace <archivio> [-z] [-t] [-p<password>] [-i<i,j,...>] [-m<nome>=<valore>]...\n"
        "  -z  archivio zip (handler zip) invece di .7z\n"
        "                   [--abort-at-file=N] [--fail-write-at-file=N]\n");
    return 2;
  }
  int res;
  try
  {
    res = Run(options);
  }
  catch(...)
  {
    printf("EXCEPTION\n");
    res = 0;
  }
  fflush(stdout);
  return res;
}
