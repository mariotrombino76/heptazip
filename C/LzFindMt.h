/* LzFindMt.h -- multithreaded Match finder for LZ algorithms
: Igor Pavlov : Public domain */

#ifndef ZIP7_INC_LZ_FIND_MT_H
#define ZIP7_INC_LZ_FIND_MT_H

#include "LzFind.h"
#include "Threads.h"

EXTERN_C_BEGIN

typedef struct
{
  UInt32 numProcessedBlocks;
  Int32 affinityGroup;
  UInt64 affinityInGroup;
  UInt64 affinity;
  CThread thread;

  BoolInt wasCreated;
  BoolInt needStart;
  BoolInt csWasInitialized;
  BoolInt csWasEntered;

  BoolInt exit;
  /* Heptazip: (stopWriting) is written by the thread that calls the match
     finder and read by the worker thread without a lock: it's accessed only with
     Z7_ATOMIC32_LOAD_RELAXED / Z7_ATOMIC32_STORE_RELAXED. Relaxed order is enough:
     the read that must see (True) follows Semaphore_Wait(freeSemaphore), and the
     writer releases that semaphore after the store. Earlier reads can see the old
     value: then the worker fills one more block, as it always did. */
  BoolInt stopWriting;

  CAutoResetEvent canStart;
  CAutoResetEvent wasStopped;
  CSemaphore freeSemaphore;
  CSemaphore filledSemaphore;
  CCriticalSection cs;
  // UInt32 numBlocks_Sent;
} CMtSync;


struct CMatchFinderMt_;

typedef UInt32 * (*Mf_Mix_Matches)(struct CMatchFinderMt_ *p, UInt32 matchMinPos, UInt32 *distances);

/* kMtCacheLineDummy must be >= size_of_CPU_cache_line */
#define kMtCacheLineDummy 128

/* Heptazip (#77): also writes the owner (bt thread) of each position to (owners) */
typedef void (*Mf_GetHeads)(const Byte *buffer, UInt32 pos,
  UInt32 *hash, UInt32 hashMask, UInt32 *heads, UInt32 numHeads, const UInt32 *crc,
  Byte *owners, UInt32 ownerMask);

/* Heptazip (issue #77): the bt (binary tree) work of the multithreaded match finder can
   be split over MT_BT_THREADS_MAX threads. The trees of different hash buckets are
   independent: the hash thread assigns each position to a bt thread from its hash
   value (owner = hash & (numBtThreads - 1)), every bt thread walks all the positions
   of a hash block but inserts and searches only its own, and writes the matches of its
   positions, or the word kMtNotOwned for the others, to its own stream of blocks. The
   LZ thread reads the N streams in step. Same matches, same order per tree: the
   archives are identical to the ones of a single bt thread (and of the single-thread
   match finder). numBtThreads is 1, 2 or 4 (LzmaEnc: numThreads - 1, rounded down). */
#define MT_BT_THREADS_MAX 4
#define MT_HASH_RING_MAX 4   /* slots of the hash block ring (kMtHashNumBlocks in LzFindMt.c) */

typedef struct
{
  CMtSync sync;         /* the bt thread (writer of btBuf) and its block ring to the LZ thread.
                           sync.cs: held by the bt thread while it uses the data buffer */
  struct CMatchFinderMt_ *mt;
  UInt32 index;         /* 0 .. numBtThreads - 1 = owner value */
  CSemaphore prevDone;  /* released once per hash block by thread (index - 1): when it
                           finished the block, or earlier when it is past half of it or
                           its output ring is about to block it. Thread (index) starts a
                           hash block only after that, so the two threads do not write the
                           nodes of neighbouring positions (same cache line) at the same
                           time, and no thread waits for a thread that is itself blocked */
  BoolInt prevGranted;  /* this thread already released prevDone of the next thread for
                           the hash block it holds */
  UInt32 numProduced;   /* output blocks produced in this stream (the ring fill is
                           numProduced - lzNumBlocks) */
  UInt32 *btBuf;        /* kBtBufferSize words: the ring of match blocks of this thread */

  /* state of the bt thread */
  UInt32 hashBufPos;
  UInt32 hashBufPosLimit;
  UInt32 hashBufStart;       /* first head of the current hash block (owners[0]) */
  UInt32 hashNumAvail;
  UInt32 hashBlockIndex;     /* number of hash blocks taken */
  BoolInt hashBlockHeld;     /* a hash block is taken and not released yet */
  const Byte *owners;        /* owner bytes of the current hash block, one per head */
  UInt32 failure_BT;
  UInt32 pos;
  const Byte *buffer;
  UInt32 cyclicBufferPos;
  CSemaphore hashFilled;     /* released by the hash thread for each filled hash block */

  /* state of the LZ thread for this stream */
  const UInt32 *btBufPos;
  const UInt32 *btBufPosLimit;
  UInt32 lzNumBlocks;        /* blocks taken by the LZ thread */
  UInt32 failureBuf[1];

  Byte dummy[kMtCacheLineDummy];
} CMtBtThread;

typedef struct CMatchFinderMt_
{
  /* LZ */
  const Byte *pointerToCurPos;
  UInt32 lzPos;
  UInt32 btNumAvailBytes;
  UInt32 numBtThreads;       /* 1, 2 or 4: set before MatchFinderMt_Create() */

  UInt32 *hash;
  UInt32 fixedHashSize;
  // UInt32 hash4Mask;
  UInt32 historySize;
  const UInt32 *crc;

  Mf_Mix_Matches MixMatchesFunc;
  UInt32 failure_LZ_BT; // failure in BT transfered to LZ
  // UInt32 failure_LZ_LZ; // failure in LZ tables
  // UInt32 crc[256];

  /* LZ + BT + Hash: the LZ thread holds (lzCs) while it uses the data buffer; the
     hash thread takes (lzCs) and every bt[].sync.cs to move the buffer */
  CCriticalSection lzCs;
  BoolInt lzCsWasInitialized;
  BoolInt lzCsWasEntered;
  BoolInt needStart;         /* no thread was started for the current stream */
  Byte btDummy[kMtCacheLineDummy];

  /* BT (shared, read-only during the stream) */
  UInt32 *hashBuf;
  CLzRef *son;
  UInt32 matchMaxLen;
  UInt32 numHashBytes;
  UInt32 cyclicBufferSize; /* it must be = (historySize + 1) */
  UInt32 sonSize;          /* cyclicBufferSize + sonExtra: nodes of (son) */
  UInt32 cutValue;
  UInt32 numBtThreads_Allocated;  /* btBuf and threads created for this many */

  CMtBtThread bt[MT_BT_THREADS_MAX];

  /* BT + Hash: the hash block ring; hashSync.cs guards hashRefCount */
  CMtSync hashSync;
  UInt32 hashRefCount[MT_HASH_RING_MAX];  /* bt threads that still hold the block in the slot */
  /* Byte hashDummy[kMtCacheLineDummy]; */

  /* Hash */
  Mf_GetHeads GetHeadsFunc;
  UInt32 ownerMask;        /* numBtThreads - 1 */
  BoolInt ownerByte0;      /* owner from p[0] (hash of 2 bytes, "3b") instead of crc[p[0]] ^ p[1] */
  CMatchFinder *MatchFinder;
  // CMatchFinder MatchFinder;
} CMatchFinderMt;

// only for Mt part
void MatchFinderMt_Construct(CMatchFinderMt *p);
void MatchFinderMt_Destruct(CMatchFinderMt *p, ISzAllocPtr alloc);

SRes MatchFinderMt_Create(CMatchFinderMt *p, UInt32 historySize, UInt32 keepAddBufferBefore,
    UInt32 matchMaxLen, UInt32 keepAddBufferAfter, ISzAllocPtr alloc);
void MatchFinderMt_CreateVTable(CMatchFinderMt *p, IMatchFinder2 *vTable);

/* call MatchFinderMt_InitMt() before IMatchFinder::Init() */
SRes MatchFinderMt_InitMt(CMatchFinderMt *p);
void MatchFinderMt_ReleaseStream(CMatchFinderMt *p);

EXTERN_C_END

#endif
