// unit_7zupdate_mt.cpp
//
// Test unitari di CPP/7zip/Archive/7z/7zUpdateMt.{h,cpp} (docs/design/0003):
// stima di thread e memoria degli encoder per un folder, e CFolderReplayInStream
// (stessa granularita' delle letture di CFolderInStream).
// Harness minimale: ogni CHECK fallito viene stampato, exit code 1.

#include "../CPP/Common/Common.h"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "../CPP/Common/MyWindows.h"
#include "../CPP/Common/MyInitGuid.h"

#include "../CPP/7zip/Archive/7z/7zHeader.h"
#include "../CPP/7zip/Archive/7z/7zUpdateMt.h"
#include "../CPP/7zip/Common/StreamObjects.h"
#include "../CPP/7zip/UI/Common/IFileExtractCallback.h" // definisce i GUID usati da UI/Common

using namespace NArchive::N7z;

static unsigned g_NumChecks = 0;
static unsigned g_NumFailures = 0;

#define CHECK(cond) \
  do { g_NumChecks++; if (!(cond)) { g_NumFailures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const UInt64 kMiB = (UInt64)1 << 20;

// ---------------------------------------------------------------------------
// Metodi come li prepara CHandler::SetMainMethod(): livello e thread nelle proprieta'
// ---------------------------------------------------------------------------

static CMethodFull &AddMethod(CCompressionMethodMode &mode, CMethodId id, UInt32 level,
    UInt32 numThreads, UInt32 numStreams = 1)
{
  CMethodFull &m = mode.Methods.AddNew();
  m.Id = id;
  m.NumStreams = numStreams;
  m.AddProp_Level(level);
  m.AddProp_NumThreads(numThreads);
  m.NumThreads = numThreads;
  return m;
}

static bool Cost(const CCompressionMethodMode &mode, UInt64 reduceSize, UInt64 folderSize,
    CFolderEncodeCost &c)
{
  return EstimateFolderEncodeCost(mode, reduceSize, folderSize, c);
}

static void Test_Estimate_Lzma2()
{
  CFolderEncodeCost c;
  {
    // -mx5, 20 thread, operazione piccola (reduce = 1 MiB): dizionario ridotto a 1 MiB,
    // blocco da 4 MiB, un solo blocco -> nessun MtCoder: 2 thread (match finder bt4 MT)
    CCompressionMethodMode mode;
    AddMethod(mode, k_LZMA2, 5, 20);
    CHECK(Cost(mode, kMiB, kMiB, c));
    CHECK(c.NumThreads == 2);
    CHECK(c.EncoderMemory < 32 * kMiB);
    // operazione grande (reduce = 1 GiB): dizionario 16 MiB, blocchi da 64 MiB,
    // 10 thread di blocco -> MtCoder; un folder da 1 MiB usa 1 blocco: 2 thread
    CFolderEncodeCost big;
    CHECK(Cost(mode, (UInt64)1 << 30, kMiB, big));
    CHECK(big.NumThreads == 2);
    CHECK(big.EncoderMemory > c.EncoderMemory);
    // con i buffer di MtCoder: almeno blocco di input + chunk di output (2 x 64 MiB)
    CHECK(big.EncoderMemory >= 128 * kMiB);
  }
  {
    // blocchi espliciti da 1 MiB (c=1m): un folder da 4 MiB usa 4 blocchi -> 8 thread
    CCompressionMethodMode mode;
    CMethodFull &m = AddMethod(mode, k_LZMA2, 5, 20);
    m.AddProp32(NCoderPropID::kBlockSize, (UInt32)kMiB);
    CHECK(Cost(mode, (UInt64)1 << 30, 4 * kMiB, c));
    CHECK(c.NumThreads == 8);
    // ...ma non piu' dei thread di blocco disponibili (4 thread totali / 2 = 2 blocchi)
    CCompressionMethodMode mode4;
    CMethodFull &m4 = AddMethod(mode4, k_LZMA2, 5, 4);
    m4.AddProp32(NCoderPropID::kBlockSize, (UInt32)kMiB);
    CHECK(Cost(mode4, (UInt64)1 << 30, 16 * kMiB, c));
    CHECK(c.NumThreads == 4);
  }
  {
    // 1 thread: un solo stream, 1 thread
    CCompressionMethodMode mode;
    AddMethod(mode, k_LZMA2, 5, 1);
    CHECK(Cost(mode, (UInt64)1 << 30, kMiB, c));
    CHECK(c.NumThreads == 1);
    // -mx1 (hc4, nessun match finder MT): 1 thread anche con 20
    CCompressionMethodMode fast;
    AddMethod(fast, k_LZMA2, 1, 20);
    CHECK(Cost(fast, kMiB, kMiB, c));
    CHECK(c.NumThreads == 1);
  }
}

static void Test_Estimate_Others()
{
  CFolderEncodeCost c;
  {
    // LZMA: 2 thread con bt4 (livello 5), 1 con hc4
    CCompressionMethodMode mode;
    AddMethod(mode, k_LZMA, 5, 8);
    CHECK(Cost(mode, kMiB, kMiB, c));
    CHECK(c.NumThreads == 2);
    CCompressionMethodMode hc;
    CMethodFull &m = AddMethod(hc, k_LZMA, 5, 8);
    m.AddProp_Ascii(NCoderPropID::kMatchFinder, "hc4");
    CHECK(Cost(hc, kMiB, kMiB, c));
    CHECK(c.NumThreads == 1);
  }
  {
    // PPMd: modello ridotto alla dimensione dei dati (CEncProps::Normalize)
    CCompressionMethodMode mode;
    AddMethod(mode, k_PPMD, 5, 8);   // livello 5: 16 MiB
    CFolderEncodeCost full;
    CHECK(Cost(mode, (UInt64)1 << 30, kMiB, full));
    CHECK(full.NumThreads == 1);
    CHECK(full.EncoderMemory >= 16 * kMiB);
    CHECK(Cost(mode, 100000, 100000, c));
    CHECK(c.EncoderMemory < full.EncoderMemory);
  }
  {
    // BZip2: un thread per blocco da 900 KB, non piu' di quelli disponibili
    CCompressionMethodMode mode;
    AddMethod(mode, k_BZip2, 5, 8);
    CHECK(Cost(mode, (UInt64)1 << 30, 2 * kMiB, c));
    CHECK(c.NumThreads == 3);
    CHECK(Cost(mode, (UInt64)1 << 30, 64 * kMiB, c));
    CHECK(c.NumThreads == 8);
  }
  {
    CCompressionMethodMode mode;
    AddMethod(mode, k_Deflate, 5, 8);
    CHECK(Cost(mode, kMiB, kMiB, c));
    CHECK(c.NumThreads == 1);
  }
 #ifdef Z7_ZSTD_LIB_ENCODER
  {
    // zstd -mx3: finestra 2 MiB, job ZSTDMT da 8 MiB
    CCompressionMethodMode mode;
    AddMethod(mode, k_ZSTD_7zZstd, 3, 8);
    CHECK(Cost(mode, (UInt64)1 << 30, kMiB, c));
    CHECK(c.NumThreads == 1);
    CHECK(Cost(mode, (UInt64)1 << 30, 20 * kMiB, c));
    CHECK(c.NumThreads == 3);
    CCompressionMethodMode st;
    AddMethod(st, k_ZSTD_7zZstd, 3, 1);
    CHECK(Cost(st, (UInt64)1 << 30, 20 * kMiB, c));
    CHECK(c.NumThreads == 1);
  }
 #endif
}

static void Test_Estimate_Chains()
{
  CFolderEncodeCost c, alone;
  {
    // filtro + metodo: thread del metodo, memoria sommata; AES aggiunge memoria
    CCompressionMethodMode mode;
    AddMethod(mode, k_Delta, 5, 8);
    AddMethod(mode, k_LZMA2, 5, 8);
    CCompressionMethodMode only;
    AddMethod(only, k_LZMA2, 5, 8);
    CHECK(Cost(mode, kMiB, kMiB, c));
    CHECK(Cost(only, kMiB, kMiB, alone));
    CHECK(c.NumThreads == alone.NumThreads);
    CHECK(c.EncoderMemory > alone.EncoderMemory);
    only.PasswordIsDefined = true;
    CFolderEncodeCost aes;
    CHECK(Cost(only, kMiB, kMiB, aes));
    CHECK(aes.EncoderMemory > alone.EncoderMemory);
  }
  {
    // non supportati: Copy, BCJ2 (4 stream), solo filtri, metodo sconosciuto, vuoto
    CCompressionMethodMode copy;
    AddMethod(copy, k_Copy, 0, 8);
    CHECK(!Cost(copy, kMiB, kMiB, c));
    CCompressionMethodMode bcj2;
    AddMethod(bcj2, k_BCJ2, 5, 8, 4);
    AddMethod(bcj2, k_LZMA2, 5, 8);
    CHECK(!Cost(bcj2, kMiB, kMiB, c));
    CCompressionMethodMode filter;
    AddMethod(filter, k_BCJ, 5, 8);
    CHECK(!Cost(filter, kMiB, kMiB, c));
    CCompressionMethodMode unknown;
    AddMethod(unknown, 0x7777777, 5, 8);
    CHECK(!Cost(unknown, kMiB, kMiB, c));
    CCompressionMethodMode empty;
    CHECK(!Cost(empty, kMiB, kMiB, c));
  }
}

// ---------------------------------------------------------------------------
// CFolderReplayInStream
// ---------------------------------------------------------------------------

/* Sequenza delle dimensioni restituite da Read(request) fino alla fine. */
static std::vector<UInt32> ReadAll(ISequentialInStream *s, UInt32 request, std::vector<Byte> &out)
{
  std::vector<UInt32> sizes;
  std::vector<Byte> buf(request);
  for (unsigned guard = 0; guard < 10000; guard++)
  {
    UInt32 processed = 12345;
    if (s->Read(buf.data(), request, &processed) != S_OK)
    {
      sizes.push_back(0xFFFFFFFF);
      break;
    }
    if (processed == 0)
      break;
    sizes.push_back(processed);
    out.insert(out.end(), buf.begin(), buf.begin() + processed);
  }
  return sizes;
}

static void Test_Replay()
{
  const UInt32 kMax = CFolderReplayInStream::kMaxReadSize;
  // file: 3, 0 (vuoto o non aperto), 5, kMax*2+7, 0, 1 byte
  const UInt64 fileSizes[] = { 3, 0, 5, (UInt64)kMax * 2 + 7, 0, 1 };
  const unsigned numFiles = 6;
  size_t total = 0;
  for (unsigned i = 0; i < numFiles; i++)
    total += (size_t)fileSizes[i];
  std::vector<Byte> data(total);
  for (size_t i = 0; i < total; i++)
    data[i] = (Byte)(i * 7 + 1);

  {
    // richieste grandi: mai oltre la fine di un file, mai piu' di kMaxReadSize
    CMyComPtr2_Create<ISequentialInStream, CFolderReplayInStream> s;
    s->Init(data.data(), data.size(), fileSizes, numFiles, NULL);
    std::vector<Byte> out;
    const std::vector<UInt32> sizes = ReadAll(s, kMax * 4, out);
    const std::vector<UInt32> expected = { 3, 5, kMax, kMax, 7, 1 };
    CHECK(sizes == expected);
    CHECK(out == data);
  }
  {
    // richieste piccole: dimensione richiesta, troncata alla fine del file
    CMyComPtr2_Create<ISequentialInStream, CFolderReplayInStream> s;
    const UInt64 small[] = { 3, 5, 4 };
    s->Init(data.data(), 12, small, 3, NULL);
    std::vector<Byte> out;
    const std::vector<UInt32> sizes = ReadAll(s, 2, out);
    const std::vector<UInt32> expected = { 2, 1, 2, 2, 1, 2, 2 };
    CHECK(sizes == expected);
    CHECK(out.size() == 12 && memcmp(out.data(), data.data(), 12) == 0);
  }
  {
    // dati oltre i file completi (file in lettura) e poi lo stream (rest)
    const UInt64 done[] = { 3, 5 };
    CMyComPtr2_Create<ISequentialInStream, CBufInStream> rest;
    const Byte tail[] = { 0xAA, 0xBB, 0xCC };
    rest->Init(tail, sizeof(tail));
    CMyComPtr2_Create<ISequentialInStream, CFolderReplayInStream> s;
    s->Init(data.data(), 8 + 4, done, 2, rest);
    std::vector<Byte> out;
    const std::vector<UInt32> sizes = ReadAll(s, 100, out);
    const std::vector<UInt32> expected = { 3, 5, 4, 3 };
    CHECK(sizes == expected);
    CHECK(out.size() == 15 && out[12] == 0xAA && out[14] == 0xCC);
  }
  {
    // dimensioni dei file maggiori dei dati: errore, non dati inventati
    const UInt64 tooBig[] = { 3, 100 };
    CMyComPtr2_Create<ISequentialInStream, CFolderReplayInStream> s;
    s->Init(data.data(), 10, tooBig, 2, NULL);
    std::vector<Byte> out;
    const std::vector<UInt32> sizes = ReadAll(s, 100, out);
    CHECK(!sizes.empty() && sizes.back() == 0xFFFFFFFF);
  }
  {
    // nessun file, nessun dato
    CMyComPtr2_Create<ISequentialInStream, CFolderReplayInStream> s;
    s->Init(NULL, 0, NULL, 0, NULL);
    std::vector<Byte> out;
    CHECK(ReadAll(s, 100, out).empty());
  }
}

int main()
{
  Test_Estimate_Lzma2();
  Test_Estimate_Others();
  Test_Estimate_Chains();
  Test_Replay();
  printf("%u controlli, %u falliti\n", g_NumChecks, g_NumFailures);
  return g_NumFailures == 0 ? 0 : 1;
}
