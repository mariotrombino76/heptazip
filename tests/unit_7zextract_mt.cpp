// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// unit_7zextract_mt.cpp
//
// Test unitari di CPP/7zip/Archive/7z/7zExtractMt.{h,cpp} e CPP/7zip/Common/MtTaskPool.{h,cpp}
// (docs/design/0001, passo 3). Harness minimale senza dipendenze esterne:
// ogni CHECK fallito viene stampato e il programma termina con exit code 1.

#include "../CPP/Common/Common.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <vector>

#include "../CPP/Common/MyWindows.h"
#include "../CPP/Common/MyInitGuid.h"

#include "../CPP/7zip/Archive/7z/7zExtractMt.h"
#include "../CPP/7zip/Archive/7z/7zHeader.h"
#include "../CPP/7zip/UI/Common/IFileExtractCallback.h" // definisce i GUID usati da UI/Common

using namespace NArchive::N7z;

static unsigned g_NumChecks = 0;
static unsigned g_NumFailures = 0;

#define CHECK(cond) \
  do { g_NumChecks++; if (!(cond)) { g_NumFailures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---------------------------------------------------------------------------
// Stima della memoria
// ---------------------------------------------------------------------------

static void Test_EstimateMemory()
{
  UInt64 mem;

  // LZMA: lc=3 lp=0 pb=2 -> props[0] = 3 + 9*(0 + 5*2) = 93, dizionario 16 MiB
  {
    const Byte props[5] = { 93, 0, 0, 0, 1 };
    CHECK(EstimateCoderDecodeMemory(k_LZMA, props, 5, mem));
    CHECK(mem >= ((UInt64)16 << 20));
    CHECK(mem < ((UInt64)20 << 20));
  }
  // LZMA: proprieta' troppo corte o lc/lp/pb non validi
  {
    const Byte props[5] = { 9 * 5 * 5, 0, 0, 0, 1 };
    CHECK(!EstimateCoderDecodeMemory(k_LZMA, props, 4, mem));
    CHECK(!EstimateCoderDecodeMemory(k_LZMA, props, 5, mem));
  }
  // LZMA: con lc+lp = 12 il modello di probabilita' pesa (0x300 << 12) * 2 = 6 MiB
  // in piu' rispetto a lc = lp = 0, a parita' di dizionario (4 KiB)
  {
    const Byte propsBig[5] = { 8 + 9 * 4, 0, 0x10, 0, 0 };  // lc=8 lp=4 pb=0
    const Byte propsSmall[5] = { 0, 0, 0x10, 0, 0 };        // lc=0 lp=0 pb=0
    UInt64 memSmall;
    CHECK(EstimateCoderDecodeMemory(k_LZMA, propsBig, 5, mem));
    CHECK(EstimateCoderDecodeMemory(k_LZMA, propsSmall, 5, memSmall));
    CHECK(mem - memSmall == (((UInt64)0x300 << 12) - 0x300) * 2);
  }
  // FLOAT: blocchi da 1 MiB (prop 20) -> 2 MiB di buffer; prop fuori da 16..22 non valida
  {
    Byte p = 20;
    CHECK(EstimateCoderDecodeMemory(k_FLOAT, &p, 1, mem));
    CHECK(mem >= ((UInt64)2 << 20) && mem < ((UInt64)5 << 20));
    p = 23;
    CHECK(!EstimateCoderDecodeMemory(k_FLOAT, &p, 1, mem));
    CHECK(!EstimateCoderDecodeMemory(k_FLOAT, &p, 0, mem));
  }
  // LZMA2: prop 24 -> 2 << 23 = 16 MiB; prop 40 -> 4 GiB - 1; prop 41 non valida
  {
    Byte p = 24;
    CHECK(EstimateCoderDecodeMemory(k_LZMA2, &p, 1, mem));
    CHECK(mem >= ((UInt64)16 << 20) && mem < ((UInt64)20 << 20));
    p = 40;
    CHECK(EstimateCoderDecodeMemory(k_LZMA2, &p, 1, mem));
    CHECK(mem >= (UInt64)0xFFFFFFFF);
    p = 41;
    CHECK(!EstimateCoderDecodeMemory(k_LZMA2, &p, 1, mem));
    CHECK(!EstimateCoderDecodeMemory(k_LZMA2, &p, 0, mem));
  }
  // PPMd: order 6, memoria 192 MiB
  {
    const Byte props[5] = { 6, 0, 0, 0, 12 };
    CHECK(EstimateCoderDecodeMemory(k_PPMD, props, 5, mem));
    CHECK(mem >= ((UInt64)192 << 20));
  }
  // metodi a memoria fissa e filtri
  CHECK(EstimateCoderDecodeMemory(k_Copy, NULL, 0, mem) && mem > 0);
  CHECK(EstimateCoderDecodeMemory(k_BZip2, NULL, 0, mem) && mem >= ((UInt64)8 << 20));
  CHECK(EstimateCoderDecodeMemory(k_Deflate, NULL, 0, mem));
  CHECK(EstimateCoderDecodeMemory(k_BCJ, NULL, 0, mem));
  CHECK(EstimateCoderDecodeMemory(k_BCJ2, NULL, 0, mem));
  CHECK(EstimateCoderDecodeMemory(k_ARM64, NULL, 0, mem));
  CHECK(EstimateCoderDecodeMemory(k_AES, NULL, 0, mem));
  // zstd (ID di 7-Zip-zstd): finestra massima dei livelli standard (2^27)
  {
    const Byte props[5] = { 1, 5, 3, 0, 0 };
    CHECK(EstimateCoderDecodeMemory(0x4F71101, props, 5, mem) && mem >= ((UInt64)128 << 20));
  }
  // metodi sconosciuti -> false (il folder va al percorso sequenziale)
  CHECK(!EstimateCoderDecodeMemory(0x4F71103, NULL, 0, mem));
  CHECK(!EstimateCoderDecodeMemory((UInt64)1 << 40, NULL, 0, mem));
  CHECK(mem == 0);
}

// ---------------------------------------------------------------------------
// Budget di memoria
// ---------------------------------------------------------------------------

static void Test_MemoryBudget()
{
  CMemoryBudget b;
  b.Init(1000);
  CHECK(b.GetLimit() == 1000 && b.GetUsed() == 0);
  CHECK(!b.IsLargeJob(250));
  CHECK(b.IsLargeJob(251));
  CHECK(b.CanReserve(1000));
  CHECK(!b.CanReserve(1001));
  b.Reserve(600);
  CHECK(b.CanReserve(400));
  CHECK(!b.CanReserve(401));
  b.Reserve(400);
  CHECK(!b.CanReserve(1));
  CHECK(b.CanReserve(0));
  b.Release(600);
  CHECK(b.GetUsed() == 400);
  CHECK(b.CanReserve(600));
  // nessun overflow con valori enormi
  CHECK(!b.CanReserve((UInt64)(Int64)-1));
  // Reserve() oltre il limite (la compressione riserva la dimensione reale di un
  // folder cresciuto): CanReserve() resta falso, senza underflow di (limit - used)
  b.Reserve(700);
  CHECK(b.GetUsed() == 1100);
  CHECK(!b.CanReserve(0));
  CHECK(!b.CanReserve(1));
  b.Release(700);
  CHECK(b.CanReserve(600));
  b.Init(0);
  CHECK(!b.CanReserve(1));
  CHECK(b.IsLargeJob(1));
  // cartella "grande" in valore assoluto (decompressa in linea con tutti i thread)
  CHECK(!CMemoryBudget::IsBigFolder((UInt64)64 << 20));
  CHECK(CMemoryBudget::IsBigFolder(((UInt64)64 << 20) + 1));
}

// ---------------------------------------------------------------------------
// CFolderBufOutStream
// ---------------------------------------------------------------------------

static void Test_FolderBufOutStream()
{
  Byte buf[10];
  memset(buf, 0, sizeof(buf));
  CFolderBufOutStream *spec = new CFolderBufOutStream;
  CMyComPtr<ISequentialOutStream> stream = spec;
  spec->Init(buf, sizeof(buf));

  const Byte data[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
  UInt32 processed = 99;

  CHECK(stream->Write(data, 0, &processed) == S_OK && processed == 0);
  CHECK(stream->Write(data, 4, &processed) == S_OK && processed == 4);
  CHECK(spec->GetPos() == 4);
  CHECK(stream->Write(data + 4, 6, &processed) == S_OK && processed == 6);
  CHECK(spec->GetPos() == 10 && !spec->WritingWasCut);
  CHECK(memcmp(buf, data, 10) == 0);
  // buffer pieno: stessa semantica di CFolderOutStream (nessun file richiesto rimasto)
  CHECK(stream->Write(data, 3, &processed) == k_My_HRESULT_WritingWasCut && processed == 0);
  CHECK(spec->WritingWasCut);

  // scrittura a cavallo del limite: si scrivono i byte che entrano
  spec->Init(buf, 5);
  CHECK(stream->Write(data, 8, &processed) == k_My_HRESULT_WritingWasCut && processed == 5);
  CHECK(spec->GetPos() == 5 && spec->WritingWasCut);
  CHECK(stream->Write(data, 1, NULL) == k_My_HRESULT_WritingWasCut);

  // buffer di dimensione zero (folder con solo file vuoti richiesti)
  spec->Init(NULL, 0);
  CHECK(stream->Write(data, 0, &processed) == S_OK && processed == 0);
  CHECK(stream->Write(data, 1, &processed) == k_My_HRESULT_WritingWasCut && processed == 0);
}

// ---------------------------------------------------------------------------
// CBufInStreamAt
// ---------------------------------------------------------------------------

static void Test_BufInStreamAt()
{
  const Byte data[8] = { 10, 11, 12, 13, 14, 15, 16, 17 };
  CBufInStreamAt *spec = new CBufInStreamAt;
  CMyComPtr<IInStream> stream = spec;
  const UInt64 kBase = (UInt64)5 << 32; // posizione oltre 4 GiB nell'archivio
  spec->Init(data, sizeof(data), kBase, S_OK);

  Byte buf[16];
  UInt32 processed = 99;
  UInt64 pos = 0;

  // posizione iniziale = base: lettura senza Seek
  CHECK(stream->Read(buf, 3, &processed) == S_OK && processed == 3 && buf[0] == 10 && buf[2] == 12);
  // Seek assoluto alla posizione reale nell'archivio
  CHECK(stream->Seek((Int64)kBase + 6, STREAM_SEEK_SET, &pos) == S_OK && pos == kBase + 6);
  CHECK(stream->Read(buf, 10, &processed) == S_OK && processed == 2 && buf[0] == 16 && buf[1] == 17);
  // alla fine dei dati: endRes (S_OK = fine dello stream, 0 byte)
  CHECK(stream->Read(buf, 4, &processed) == S_OK && processed == 0);
  CHECK(stream->Read(buf, 0, &processed) == S_OK && processed == 0);
  // Seek relativo
  CHECK(stream->Seek(-4, STREAM_SEEK_CUR, &pos) == S_OK && pos == kBase + 4);
  CHECK(stream->Read(buf, 1, &processed) == S_OK && processed == 1 && buf[0] == 14);
  // prima della base: errore (il decoder non legge fuori dai pack stream del folder)
  CHECK(stream->Seek((Int64)kBase - 1, STREAM_SEEK_SET, &pos) == S_OK);
  CHECK(stream->Read(buf, 1, &processed) == E_FAIL && processed == 0);
  // Seek negativo e origine non valida
  CHECK(stream->Seek(-1, STREAM_SEEK_SET, &pos) == HRESULT_WIN32_ERROR_NEGATIVE_SEEK);
  CHECK(stream->Seek(0, 7, &pos) == STG_E_INVALIDFUNCTION);

  // lettura interrotta da un errore: i byte letti, poi l'errore alla fine
  spec->Init(data, 4, kBase, E_OUTOFMEMORY);
  CHECK(stream->Read(buf, 10, &processed) == S_OK && processed == 4);
  CHECK(stream->Read(buf, 10, &processed) == E_OUTOFMEMORY && processed == 0);
}


// ---------------------------------------------------------------------------
// CBufInStreamAt con caricamento a richiesta (CBufRefill)
// ---------------------------------------------------------------------------

namespace {

/* Sorgente di prova: carica i dati a blocchi di (Step) byte fino a (Avail),
   poi riporta (FinalRes) come risultato di fine dati. */
struct CTestRefill: public CBufRefill
{
  const Byte *Data;
  size_t Avail;
  size_t Step;
  size_t Loaded;
  HRESULT FinalRes;
  unsigned NumCalls;
  size_t LastNeedEnd;

  CTestRefill(): Data(NULL), Avail(0), Step(1), Loaded(0), FinalRes(S_OK), NumCalls(0), LastNeedEnd(0) {}

  void Refill(size_t needEnd, size_t &size, HRESULT &endRes) Z7_override
  {
    NumCalls++;
    LastNeedEnd = needEnd;
    size_t target = Loaded + Step;
    if (target < needEnd)
      target = needEnd;
    if (target > Avail)
      target = Avail;
    Loaded = target;
    size = Loaded;
    endRes = (Loaded == Avail) ? FinalRes : S_OK;
  }
};

}

static void Test_BufInStreamAt_Refill()
{
  Byte data[64];
  for (unsigned i = 0; i < sizeof(data); i++)
    data[i] = (Byte)(100 + i);
  const UInt64 kBase = 1000;
  Byte buf[64];
  UInt32 processed = 0;
  UInt64 pos = 0;

  // i primi 8 byte sono gia' caricati; il resto arriva a blocchi di 16
  {
    CTestRefill refill;
    refill.Data = data;
    refill.Avail = sizeof(data);
    refill.Step = 16;
    refill.Loaded = 8;
    CBufInStreamAt *spec = new CBufInStreamAt;
    CMyComPtr<IInStream> stream = spec;
    spec->Init(data, 8, kBase, S_OK, &refill);

    // dentro i dati caricati: nessuna ricarica
    CHECK(stream->Read(buf, 5, &processed) == S_OK && processed == 5 && buf[4] == 104);
    CHECK(refill.NumCalls == 0);
    // fino alla fine dei dati caricati: ancora nessuna ricarica
    CHECK(stream->Read(buf, 3, &processed) == S_OK && processed == 3 && buf[2] == 107);
    CHECK(refill.NumCalls == 0);
    // oltre: ricarica con needEnd = posizione + richiesta (28), poi i byte richiesti
    CHECK(stream->Read(buf, 20, &processed) == S_OK && processed == 20 && buf[0] == 108 && buf[19] == 127);
    CHECK(refill.NumCalls == 1 && refill.LastNeedEnd == 28 && refill.Loaded == 28);
    // il Seek non carica nulla
    CHECK(stream->Seek((Int64)kBase + 40, STREAM_SEEK_SET, &pos) == S_OK && pos == kBase + 40);
    CHECK(refill.NumCalls == 1);
    // lettura oltre i 28 byte caricati: la ricarica porta fino a needEnd (44)
    CHECK(stream->Read(buf, 4, &processed) == S_OK && processed == 4 && buf[0] == 140);
    CHECK(refill.NumCalls == 2 && refill.LastNeedEnd == 44 && refill.Loaded == 44);
    // tutto il resto in una ricarica (needEnd oltre la fine: si ferma a 64)
    CHECK(stream->Read(buf, 64, &processed) == S_OK && processed == 20 && buf[19] == 163);
    CHECK(refill.NumCalls == 3 && refill.Loaded == 64);
    // fine dei dati: la sorgente non aggiunge nulla e viene scartata
    CHECK(stream->Read(buf, 1, &processed) == S_OK && processed == 0);
    CHECK(refill.NumCalls == 4);
    CHECK(stream->Read(buf, 1, &processed) == S_OK && processed == 0);
    CHECK(refill.NumCalls == 4);
  }

  // errore di lettura dell'archivio dopo 24 byte: i byte caricati, poi l'errore
  {
    CTestRefill refill;
    refill.Data = data;
    refill.Avail = 24;
    refill.Step = 16;
    refill.Loaded = 8;
    refill.FinalRes = E_FAIL;
    CBufInStreamAt *spec = new CBufInStreamAt;
    CMyComPtr<IInStream> stream = spec;
    spec->Init(data, 8, kBase, S_OK, &refill);
    CHECK(stream->Read(buf, 64, &processed) == S_OK && processed == 8);
    CHECK(stream->Read(buf, 64, &processed) == S_OK && processed == 16);
    CHECK(refill.NumCalls == 1);
    CHECK(stream->Read(buf, 64, &processed) == E_FAIL && processed == 0);
    CHECK(refill.NumCalls == 1); // scartata al primo errore
    CHECK(stream->Read(buf, 64, &processed) == E_FAIL && processed == 0);
  }

  // dati gia' completi con errore finale: la sorgente non viene mai usata
  {
    CTestRefill refill;
    CBufInStreamAt *spec = new CBufInStreamAt;
    CMyComPtr<IInStream> stream = spec;
    spec->Init(data, 4, kBase, E_OUTOFMEMORY, &refill);
    CHECK(stream->Read(buf, 64, &processed) == S_OK && processed == 4);
    CHECK(stream->Read(buf, 64, &processed) == E_OUTOFMEMORY && processed == 0);
    CHECK(refill.NumCalls == 0);
  }
}


// ---------------------------------------------------------------------------
// CSharedInStream / CSharedInStreamView
// ---------------------------------------------------------------------------

namespace {

/* IInStream di prova in memoria che conta Seek e Read e limita ogni Read a
   (MaxRead) byte, come uno stream reale che restituisce meno del richiesto. */
Z7_CLASS_IMP_IInStream(
  CCountingInStream
)
public:
  const Byte *Data;
  size_t Size;
  UInt64 Pos;
  UInt32 MaxRead;
  unsigned NumSeeks;
  unsigned NumReads;
  CCountingInStream(): Data(NULL), Size(0), Pos(0), MaxRead(0xFFFFFFFF), NumSeeks(0), NumReads(0) {}
};

Z7_COM7F_IMF(CCountingInStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  NumReads++;
  if (processedSize)
    *processedSize = 0;
  if (Pos >= Size)
    return S_OK;
  size_t rem = Size - (size_t)Pos;
  if (rem > size)
    rem = size;
  if (rem > MaxRead)
    rem = MaxRead;
  memcpy(data, Data + (size_t)Pos, rem);
  Pos += rem;
  if (processedSize)
    *processedSize = (UInt32)rem;
  return S_OK;
}

Z7_COM7F_IMF(CCountingInStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  NumSeeks++;
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: offset += (Int64)Pos; break;
    case STREAM_SEEK_END: offset += (Int64)Size; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (offset < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  Pos = (UInt64)offset;
  if (newPosition)
    *newPosition = Pos;
  return S_OK;
}

}

static void Test_SharedInStream()
{
  Byte data[100];
  for (unsigned i = 0; i < sizeof(data); i++)
    data[i] = (Byte)i;
  CCountingInStream *baseSpec = new CCountingInStream;
  CMyComPtr<IInStream> base = baseSpec;
  baseSpec->Data = data;
  baseSpec->Size = sizeof(data);

  CSharedInStream shared;
  shared.Init(base);

  Byte buf[100];
  UInt32 processed = 0;
  UInt64 pos = 0;

  // due viste con posizioni proprie, letture alternate: ognuna vede i propri byte
  CSharedInStreamView *aSpec = new CSharedInStreamView;
  CMyComPtr<IInStream> a = aSpec;
  aSpec->Init(&shared, 0);
  CSharedInStreamView *bSpec = new CSharedInStreamView;
  CMyComPtr<IInStream> b = bSpec;
  bSpec->Init(&shared, 50);

  CHECK(a->Read(buf, 10, &processed) == S_OK && processed == 10 && buf[0] == 0 && buf[9] == 9);
  CHECK(b->Read(buf, 10, &processed) == S_OK && processed == 10 && buf[0] == 50 && buf[9] == 59);
  CHECK(a->Read(buf, 10, &processed) == S_OK && processed == 10 && buf[0] == 10);
  CHECK(b->Read(buf, 10, &processed) == S_OK && processed == 10 && buf[0] == 60);
  // una lettura consecutiva della stessa vista non fa Seek
  const unsigned seeks = baseSpec->NumSeeks;
  CHECK(b->Read(buf, 5, &processed) == S_OK && processed == 5 && buf[0] == 70);
  CHECK(baseSpec->NumSeeks == seeks);
  CHECK(a->Read(buf, 5, &processed) == S_OK && processed == 5 && buf[0] == 20);
  CHECK(baseSpec->NumSeeks == seeks + 1);

  // Seek delle viste: relativo, assoluto, dalla fine
  CHECK(a->Seek(-5, STREAM_SEEK_CUR, &pos) == S_OK && pos == 20);
  CHECK(a->Seek(0, STREAM_SEEK_END, &pos) == S_OK && pos == 100);
  CHECK(a->Read(buf, 5, &processed) == S_OK && processed == 0);
  CHECK(a->Seek(-3, STREAM_SEEK_CUR, &pos) == S_OK && pos == 97);
  CHECK(a->Read(buf, 5, &processed) == S_OK && processed == 3 && buf[0] == 97);
  CHECK(a->Seek(-1, STREAM_SEEK_SET, &pos) == HRESULT_WIN32_ERROR_NEGATIVE_SEEK);
  CHECK(a->Seek(0, 9, &pos) == STG_E_INVALIDFUNCTION);

  // ReadFullAt: completa la lettura anche se lo stream restituisce poco per volta
  baseSpec->MaxRead = 7;
  size_t size = 30;
  CHECK(shared.ReadFullAt(40, buf, size) == S_OK && size == 30 && buf[0] == 40 && buf[29] == 69);
  size = 30;
  CHECK(shared.ReadFullAt(90, buf, size) == S_OK && size == 10 && buf[9] == 99);
  // la vista dopo un ReadFullAt riparte dalla propria posizione
  CHECK(b->Read(buf, 5, &processed) == S_OK && processed == 5 && buf[0] == 75);

  UInt64 end = 0;
  CHECK(shared.GetEnd(end) == S_OK && end == 100);
}

// ---------------------------------------------------------------------------
// CCachedPassword
// ---------------------------------------------------------------------------

#ifndef Z7_NO_CRYPTO

namespace {

Z7_CLASS_IMP_COM_1(
  CCountingPassword
  , ICryptoGetTextPassword
)
public:
  unsigned NumCalls;
  HRESULT Res;
  UString Password;
  CCountingPassword(): NumCalls(0), Res(S_OK) {}
};

Z7_COM7F_IMF(CCountingPassword::CryptoGetTextPassword(BSTR *password))
{
  NumCalls++;
  if (Res != S_OK)
    return Res;
  return StringToBstr(Password, password);
}

UString BstrToString(BSTR b)
{
  UString s;
  if (b)
    s = b;
  return s;
}

} // namespace

static void Test_CachedPassword()
{
  {
    CCountingPassword *realSpec = new CCountingPassword;
    CMyComPtr<ICryptoGetTextPassword> real = realSpec;
    realSpec->Password = L"segreto";

    CCachedPassword *cacheSpec = new CCachedPassword;
    CMyComPtr<ICryptoGetTextPassword> cache = cacheSpec;
    CHECK(!cacheSpec->WasFetched());
    BSTR b = NULL;
    CHECK(cache->CryptoGetTextPassword(&b) == E_FAIL && b == NULL); // Fetch() non chiamata

    CHECK(cacheSpec->Fetch(real) == S_OK);
    CHECK(cacheSpec->WasFetched());
    for (int i = 0; i < 3; i++)
    {
      b = NULL;
      CHECK(cache->CryptoGetTextPassword(&b) == S_OK);
      CHECK(BstrToString(b) == L"segreto");
      SysFreeString(b);
    }
    CHECK(realSpec->NumCalls == 1); // la UI e' interrogata una sola volta
  }
  {
    // errore del callback (es. l'utente annulla): propagato invariato
    CCountingPassword *realSpec = new CCountingPassword;
    CMyComPtr<ICryptoGetTextPassword> real = realSpec;
    realSpec->Res = E_ABORT;
    CCachedPassword *cacheSpec = new CCachedPassword;
    CMyComPtr<ICryptoGetTextPassword> cache = cacheSpec;
    CHECK(cacheSpec->Fetch(real) == E_ABORT);
    BSTR b = NULL;
    CHECK(cache->CryptoGetTextPassword(&b) == E_ABORT);
  }
  {
    // nessun callback: come 7zDecode.cpp, E_NOTIMPL
    CCachedPassword *cacheSpec = new CCachedPassword;
    CMyComPtr<ICryptoGetTextPassword> cache = cacheSpec;
    CHECK(cacheSpec->Fetch(NULL) == E_NOTIMPL);
    BSTR b = NULL;
    CHECK(cache->CryptoGetTextPassword(&b) == E_NOTIMPL);
  }
}

#endif

// ---------------------------------------------------------------------------
// CMtTaskPool
// ---------------------------------------------------------------------------

#ifndef Z7_ST

namespace {

class CTestTask: public CMtTask
{
public:
  unsigned Id;
  unsigned Work;
  bool Throw;
  std::atomic<unsigned> NumRuns;
  unsigned WorkerIndex;
  UInt64 Result;

  CTestTask(): Id(0), Work(0), Throw(false), NumRuns(0), WorkerIndex(0), Result(0) {}

  void Run(unsigned workerIndex) Z7_override
  {
    NumRuns++;
    WorkerIndex = workerIndex;
    UInt64 x = Id;
    for (unsigned i = 0; i < Work; i++)
      x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    Result = x;
    if (Throw)
      throw 1;
  }
};

UInt64 ExpectedResult(unsigned id, unsigned work)
{
  UInt64 x = id;
  for (unsigned i = 0; i < work; i++)
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
  return x;
}

} // namespace

static void Test_TaskPool_Basic()
{
  CMtTaskPool pool;
  CTestTask t0;
  CHECK(pool.Submit(&t0) == E_FAIL); // non avviato
  CHECK(pool.Start(0) == E_FAIL);
  CHECK(pool.Start(4) == S_OK);
  CHECK(pool.GetNumThreads() == 4);
  CHECK(pool.Start(4) == E_FAIL); // gia' avviato

  // 500 task con carico variabile; attesa in ordine di invio (come il replay)
  const unsigned kNum = 500;
  std::vector<CTestTask> tasks(kNum);
  for (unsigned i = 0; i < kNum; i++)
  {
    tasks[i].Id = i;
    tasks[i].Work = (i * 7919) % 20000;
    tasks[i].Throw = (i % 97 == 13);
    CHECK(pool.Submit(&tasks[i]) == S_OK);
  }
  for (unsigned i = 0; i < kNum; i++)
  {
    CHECK(tasks[i].Done.Lock() == 0);
    CHECK(tasks[i].NumRuns == 1);
    CHECK(tasks[i].WorkerIndex < 4);
    CHECK(tasks[i].Result == ExpectedResult(i, tasks[i].Work));
    CHECK(tasks[i].ExceptionWasCaught == tasks[i].Throw);
  }

  // riuso di un task gia' completato: Submit() resetta Done
  CHECK(pool.Submit(&tasks[0]) == S_OK);
  CHECK(tasks[0].Done.Lock() == 0);
  CHECK(tasks[0].NumRuns == 2);

  pool.Stop();
  CHECK(pool.GetNumThreads() == 0);
  CHECK(pool.Submit(&t0) == E_FAIL); // fermato
  pool.Stop(); // idempotente

  // riavvio dopo Stop()
  CHECK(pool.Start(2) == S_OK);
  CHECK(pool.Submit(&t0) == S_OK);
  CHECK(t0.Done.Lock() == 0 && t0.NumRuns == 1);
}

static void Test_TaskPool_StopWithPending()
{
  // Stop() con molti task in coda: termina, i task avviati finiscono,
  // nessun task e' eseguito due volte, nessun thread resta attivo.
  for (unsigned round = 0; round < 50; round++)
  {
    CMtTaskPool pool;
    CHECK(pool.Start(3) == S_OK);
    const unsigned kNum = 200;
    std::vector<CTestTask> tasks(kNum);
    for (unsigned i = 0; i < kNum; i++)
    {
      tasks[i].Id = i;
      tasks[i].Work = 5000;
      CHECK(pool.Submit(&tasks[i]) == S_OK);
    }
    pool.Stop();
    unsigned numRun = 0;
    for (unsigned i = 0; i < kNum; i++)
    {
      CHECK(tasks[i].NumRuns <= 1);
      numRun += tasks[i].NumRuns;
    }
    CHECK(numRun <= kNum);
  }
}

static void Test_TaskPool_Stress()
{
  // molti cicli brevi di avvio/invio/attesa/arresto (utile con TSan)
  for (unsigned round = 0; round < 200; round++)
  {
    CMtTaskPool pool;
    const unsigned numThreads = 1 + round % 8;
    CHECK(pool.Start(numThreads) == S_OK);
    CTestTask tasks[16];
    for (unsigned i = 0; i < 16; i++)
    {
      tasks[i].Id = round * 16 + i;
      tasks[i].Work = i * 13;
      CHECK(pool.Submit(&tasks[i]) == S_OK);
    }
    for (unsigned i = 0; i < 16; i++)
    {
      CHECK(tasks[i].Done.Lock() == 0);
      CHECK(tasks[i].NumRuns == 1 && tasks[i].WorkerIndex < numThreads);
    }
  }
}

#endif // Z7_ST

int main()
{
  Test_EstimateMemory();
  Test_MemoryBudget();
  Test_FolderBufOutStream();
  Test_BufInStreamAt();
  Test_BufInStreamAt_Refill();
  Test_SharedInStream();
#ifndef Z7_NO_CRYPTO
  Test_CachedPassword();
#endif
#ifndef Z7_ST
  Test_TaskPool_Basic();
  Test_TaskPool_StopWithPending();
  Test_TaskPool_Stress();
#endif
  printf("%u controlli, %u falliti\n", g_NumChecks, g_NumFailures);
  return g_NumFailures == 0 ? 0 : 1;
}
