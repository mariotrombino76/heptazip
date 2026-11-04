/* LzFindMt.c -- multithreaded Match finder for LZ algorithms
: Igor Pavlov : Public domain
Heptazip (issue #77): the bt work split over up to MT_BT_THREADS_MAX threads by hash
bucket, with identical output. See LzFindMt.h and docs/design/0015-match-finder-bt-threads.md */

#include "Precomp.h"

// #include <stdio.h>

#include "CpuArch.h"

#include "LzHash.h"
#include "LzFindMt.h"

// #define LOG_ITERS

// #define LOG_THREAD

#ifdef LOG_THREAD
#include <stdio.h>
#define PRF(x) x
#else
#define PRF(x)
#endif

#ifdef LOG_ITERS
#include <stdio.h>
extern UInt64 g_NumIters_Tree;
extern UInt64 g_NumIters_Loop;
extern UInt64 g_NumIters_Bytes;
#define LOG_ITER(x) x
#else
#define LOG_ITER(x)
#endif

#define kMtHashBlockSize ((UInt32)1 << 17)
/* Heptazip (#77): 4 slots (upstream: 2), so that the bt threads can stay one hash block
   apart from each other (see Bt_TakeNextHashBlock) without stalling the hash thread */
#define kMtHashNumBlocks (1 << 2)

#define GET_HASH_BLOCK_OFFSET(i)  (((i) & (kMtHashNumBlocks - 1)) * kMtHashBlockSize)

/* Heptazip (#77): a hash block holds the header (2 words), (num) heads (one word each)
   and (num) owner bytes after the heads: 2 + num + ceil(num / 4) <= kMtHashBlockSize.
   It must hold more positions than a bt block can (kMtBtBlockSize words, at least one
   word per position): then the LZ thread never waits for a bt thread that is a full
   hash block ahead of another, which rules out a deadlock between the hash ring, the
   bt rings and the LZ thread. */
#define kMtHashBlockNumMax 104848

#define kMtBtBlockSize ((UInt32)1 << 16)
#define kMtBtNumBlocks (1 << 4)

#define GET_BT_BLOCK_OFFSET(i)  (((i) & (kMtBtNumBlocks - 1)) * (size_t)kMtBtBlockSize)

/* Heptazip (#77): extra nodes of (son) with several bt threads. A bt thread can be
   ahead of another by less than kMtHashNumBlocks hash blocks (a slot of the hash ring
   is reused only when every bt thread released it), so the node of a position stays
   untouched for at least this many positions after it leaves the window of the thread
   that is behind. */
#define kMtSonExtra (kMtHashNumBlocks * kMtHashBlockSize)

/* the same numbers as above, for the preprocessor (no casts): 1 << 17, 1 << 16, 4 */
#if kMtHashBlockNumMax < 65536
  #error Stop_Compiling_Bad_kMtHashBlockNumMax
#endif
#if kMtHashBlockNumMax + 2 + (kMtHashBlockNumMax + 3) / 4 > 131072
  #error Stop_Compiling_Bad_kMtHashBlockNumMax_2
#endif
#if 4 * 131072 < 4 * kMtHashBlockNumMax
  #error Stop_Compiling_Bad_kMtSonExtra
#endif
#if MT_HASH_RING_MAX != 4
  #error Stop_Compiling_Bad_MT_HASH_RING_MAX
#endif

/*
  HASH functions:
  We use raw 8/16 bits from a[1] and a[2],
  xored with crc(a[0]) and crc(a[3]).
  We check a[0], a[3] only. We don't need to compare a[1] and a[2] in matches.
  our crc() function provides one-to-one correspondence for low 8-bit values:
    (crc[0...0xFF] & 0xFF) <-> [0...0xFF]
*/

#define MF(mt) ((mt)->MatchFinder)
#define MF_CRC (p->crc)

// #define MF(mt) (&(mt)->MatchFinder)
// #define MF_CRC (p->MatchFinder.crc)

#define MT_HASH2_CALC \
  h2 = (MF_CRC[cur[0]] ^ cur[1]) & (kHash2Size - 1);

#define MT_HASH3_CALC { \
  UInt32 temp = MF_CRC[cur[0]] ^ cur[1]; \
  h2 = temp & (kHash2Size - 1); \
  h3 = (temp ^ ((UInt32)cur[2] << 8)) & (kHash3Size - 1); }

/*
#define MT_HASH3_CALC__NO_2 { \
  UInt32 temp = p->crc[cur[0]] ^ cur[1]; \
  h3 = (temp ^ ((UInt32)cur[2] << 8)) & (kHash3Size - 1); }

#define MT_HASH4_CALC { \
  UInt32 temp = p->crc[cur[0]] ^ cur[1]; \
  h2 = temp & (kHash2Size - 1); \
  temp ^= ((UInt32)cur[2] << 8); \
  h3 = temp & (kHash3Size - 1); \
  h4 = (temp ^ (p->crc[cur[3]] << kLzHash_CrcShift_1)) & p->hash4Mask; }
  // (kHash4Size - 1);
*/

/* Heptazip (#77): the owner of a position is a function of its first bytes that is
   constant inside a hash bucket: the low bits of the hash value. For the hash of 2
   bytes (GetHeads2) and the "3b" hash (GetUi16(p) ^ p[2] << 16) the low bits of the
   value are the low bits of p[0]; for the crc hashes (3, 4, 4b, 5, 5b) they are the
   low bits of (crc[p[0]] ^ p[1]), because the other terms are shifted left by
   kLzHash_CrcShift_1 (and _2) >= 2 bits or are bytes above bit 8, and hashMask always
   covers the low 16 bits for these hashes. ownerMask is 0, 1 or 3. The hash thread
   uses these same expressions (OWNER_BYTE0 / OWNER_CRC in GetHeads), and the bt threads
   use MtOwner() to find their own nodes when they normalize (son). */
#define OWNER_BYTE0(p, ownerMask)       ((UInt32)(p)[0] & (ownerMask))
#define OWNER_CRC(crc, p, ownerMask)    (((crc)[(p)[0]] ^ (UInt32)(p)[1]) & (ownerMask))

static UInt32 MtOwner(const CMatchFinderMt *p, const Byte *data)
{
  if (p->ownerByte0)
    return OWNER_BYTE0(data, p->ownerMask);
  return OWNER_CRC(p->crc, data, p->ownerMask);
}


Z7_NO_INLINE
static void MtSync_Construct(CMtSync *p)
{
  p->affinityGroup = -1;
  p->affinityInGroup = 0;
  p->affinity = 0;
  p->wasCreated = False;
  p->csWasInitialized = False;
  p->csWasEntered = False;
  Z7_ATOMIC32_STORE_RELAXED(&p->stopWriting, False);
  Thread_CONSTRUCT(&p->thread)
  Event_Construct(&p->canStart);
  Event_Construct(&p->wasStopped);
  Semaphore_Construct(&p->freeSemaphore);
  Semaphore_Construct(&p->filledSemaphore);
}


// #define DEBUG_BUFFER_LOCK   // define it to debug lock state

#ifdef DEBUG_BUFFER_LOCK
#include <stdlib.h>
#define BUFFER_MUST_BE_LOCKED(p)    if (!(p)->csWasEntered) exit(1);
#define BUFFER_MUST_BE_UNLOCKED(p)  if ( (p)->csWasEntered) exit(1);
#else
#define BUFFER_MUST_BE_LOCKED(p)
#define BUFFER_MUST_BE_UNLOCKED(p)
#endif

#define LOCK_BUFFER(p) { \
    BUFFER_MUST_BE_UNLOCKED(p); \
    CriticalSection_Enter(&(p)->cs); \
    (p)->csWasEntered = True; }

#define UNLOCK_BUFFER(p) { \
    BUFFER_MUST_BE_LOCKED(p); \
    CriticalSection_Leave(&(p)->cs); \
    (p)->csWasEntered = False; }

/* Heptazip (#77): the lock of the LZ thread on the data buffer */
#define LZ_LOCK(p) { \
    CriticalSection_Enter(&(p)->lzCs); \
    (p)->lzCsWasEntered = True; }

#define LZ_UNLOCK(p) { \
    CriticalSection_Leave(&(p)->lzCs); \
    (p)->lzCsWasEntered = False; }


/* Heptazip (#77): the start of a worker thread for a new stream, as the (needStart)
   branch of MtSync_GetNextBlock() of upstream. The LZ thread starts the hash thread
   and the bt threads together (MatchFinderMt_StartThreads). */
static void MtSync_Start(CMtSync *p)
{
  BUFFER_MUST_BE_UNLOCKED(p)
  p->numProcessedBlocks = 1;
  p->needStart = False;
  Z7_ATOMIC32_STORE_RELAXED(&p->stopWriting, False);
  p->exit = False;
  Event_Reset(&p->wasStopped);
  Event_Set(&p->canStart);
}


/* if Writing (Processing) thread was started, we must call MtSync_StopWriting() */

Z7_NO_INLINE
static void MtSync_StopWriting(CMtSync *p)
{
  if (!Thread_WasCreated(&p->thread) || p->needStart)
    return;

    PRF(printf("\nMtSync_StopWriting %p\n", p));

  if (p->csWasEntered)
  {
    /* we don't use buffer in this thread after StopWriting().
       So we UNLOCK buffer.
       And we restore default UNLOCKED state for stopped thread */
    UNLOCK_BUFFER(p)
  }

  /* We send (p->stopWriting) message and release freeSemaphore
     to free current block.
     So the thread will see (p->stopWriting) at some
     iteration after Wait(freeSemaphore).
     The thread doesn't need to fill all avail free blocks,
     so we can get fast thread stop.
  */

  Z7_ATOMIC32_STORE_RELAXED(&p->stopWriting, True);
  Semaphore_Release1(&p->freeSemaphore); // check semaphore count !!!

    PRF(printf("\nafter Release1 %p\n", p));
  Event_Wait(&p->wasStopped);
    PRF(printf("\nafter Event_Wait %p\n", p));

  /* 21.03 : we don't restore samaphore counters here.
     We will recreate and reinit samaphores in next start */

  p->needStart = True;
}


Z7_NO_INLINE
static void MtSync_Destruct(CMtSync *p)
{
    PRF(printf("\nMtSync_Destruct %p\n", p));

  if (Thread_WasCreated(&p->thread))
  {
    /* we want thread to be in Stopped state before sending EXIT command.
       note: stop(btSync) will stop (htSync) also */
    MtSync_StopWriting(p);
    /* thread in Stopped state here : (p->needStart == true) */
    p->exit = True;
    // if (p->needStart)  // it's (true)
    Event_Set(&p->canStart);  // we send EXIT command to thread
    Thread_Wait_Close(&p->thread);  // we wait thread finishing
  }

  if (p->csWasInitialized)
  {
    CriticalSection_Delete(&p->cs);
    p->csWasInitialized = False;
  }
  p->csWasEntered = False;

  Event_Close(&p->canStart);
  Event_Close(&p->wasStopped);
  Semaphore_Close(&p->freeSemaphore);
  Semaphore_Close(&p->filledSemaphore);

  p->wasCreated = False;
}


// #define RINOK_THREAD(x) { if ((x) != 0) return SZ_ERROR_THREAD; }
// we want to get real system error codes here instead of SZ_ERROR_THREAD
#define RINOK_THREAD(x)  RINOK_WRes(x)


// call it before each new file (when new starting is required):
Z7_NO_INLINE
static SRes MtSync_Init(CMtSync *p, UInt32 numBlocks)
{
  WRes wres;
  // BUFFER_MUST_BE_UNLOCKED(p)
  if (!p->needStart || p->csWasEntered)
    return SZ_ERROR_FAIL;
  wres = Semaphore_OptCreateInit(&p->freeSemaphore, numBlocks, numBlocks);
  if (wres == 0)
    wres = Semaphore_OptCreateInit(&p->filledSemaphore, 0, numBlocks);
  return MY_SRes_HRESULT_FROM_WRes(wres);
}


static WRes MtSync_Create_WRes(CMtSync *p, THREAD_FUNC_TYPE startAddress, void *obj)
{
  WRes wres;

  if (p->wasCreated)
    return SZ_OK;

  RINOK_THREAD(CriticalSection_Init(&p->cs))
  p->csWasInitialized = True;
  p->csWasEntered = False;

  RINOK_THREAD(AutoResetEvent_CreateNotSignaled(&p->canStart))
  RINOK_THREAD(AutoResetEvent_CreateNotSignaled(&p->wasStopped))

  p->needStart = True;
  p->exit = True;  /* p->exit is unused before (canStart) Event.
     But in case of some unexpected code failure we will get fast exit from thread */

  // return ERROR_TOO_MANY_POSTS; // for debug
  // return EINVAL; // for debug

#ifdef _WIN32
  if (p->affinityGroup >= 0)
    wres = Thread_Create_With_Group(&p->thread, startAddress, obj,
        (unsigned)(UInt32)p->affinityGroup, (CAffinityMask)p->affinityInGroup);
  else
#endif
  if (p->affinity != 0)
    wres = Thread_Create_With_Affinity(&p->thread, startAddress, obj, (CAffinityMask)p->affinity);
  else
    wres = Thread_Create(&p->thread, startAddress, obj);

  RINOK_THREAD(wres)
  p->wasCreated = True;
  return SZ_OK;
}


Z7_NO_INLINE
static SRes MtSync_Create(CMtSync *p, THREAD_FUNC_TYPE startAddress, void *obj)
{
  const WRes wres = MtSync_Create_WRes(p, startAddress, obj);
  if (wres == 0)
    return 0;
  MtSync_Destruct(p);
  return MY_SRes_HRESULT_FROM_WRes(wres);
}


/* Heptazip (#77): the ring of hash blocks has (numBtThreads) consumers. The hash
   thread fills a slot, sets its reference count to (numBtThreads) and releases the
   (hashFilled) semaphore of every bt thread; a bt thread that leaves a block drops the
   count, and the one that brings it to zero frees the slot for the hash thread
   (hashSync.freeSemaphore). hashSync.cs guards the counts. */
static void HashBlock_Release(CMatchFinderMt *p, UInt32 blockIndex)
{
  UInt32 c;
  CriticalSection_Enter(&p->hashSync.cs);
  c = --p->hashRefCount[blockIndex & (kMtHashNumBlocks - 1)];
  CriticalSection_Leave(&p->hashSync.cs);
  if (c == 0)
    Semaphore_Release1(&p->hashSync.freeSemaphore);
}


#ifndef kMtMaxValForNormalize  /* Heptazip: a test build can define a small value */
#define kMtMaxValForNormalize 0xFFFFFFFF
#endif
// #define kMtMaxValForNormalize ((1 << 21)) // for debug
// #define kNormalizeAlign (1 << 7) // alignment for speculated accesses

#ifdef MY_CPU_LE_UNALIGN
  #define GetUi24hi_from32(p) ((UInt32)GetUi32(p) >> 8)
#else
  #define GetUi24hi_from32(p) ((p)[1] ^ ((UInt32)(p)[2] << 8) ^ ((UInt32)(p)[3] << 16))
#endif

#define GetHeads_DECL(name) \
    static void GetHeads ## name(const Byte *p, UInt32 pos, \
      UInt32 *hash, UInt32 hashMask, UInt32 *heads, UInt32 numHeads, const UInt32 *crc, \
      Byte *owners, UInt32 ownerMask)

/* Heptazip (#77): (ow) is the owner expression of the position at (p), evaluated
   before (p) advances */
#define GetHeads_LOOP(v, ow) \
    for (; numHeads != 0; numHeads--) { \
      const UInt32 value = (v); \
      *owners++ = (Byte)(ow); \
      p++; \
      *heads++ = pos - hash[value]; \
      hash[value] = pos++; }

#define DEF_GetHeads2(name, v, ow, action) \
    GetHeads_DECL(name) { action \
    GetHeads_LOOP(v, ow) }

#define DEF_GetHeads(name, v, ow) DEF_GetHeads2(name, v, ow, ;)

DEF_GetHeads2(2, GetUi16(p), OWNER_BYTE0(p, ownerMask), UNUSED_VAR(hashMask); UNUSED_VAR(crc); )
DEF_GetHeads(3,  (crc[p[0]] ^ GetUi16(p + 1)) & hashMask, OWNER_CRC(crc, p, ownerMask))
DEF_GetHeads2(3b, GetUi16(p) ^ ((UInt32)(p)[2] << 16), OWNER_BYTE0(p, ownerMask), UNUSED_VAR(hashMask); UNUSED_VAR(crc); )
// BT3 is not good for crc collisions for big hashMask values.

/*
GetHeads_DECL(3b)
{
  UNUSED_VAR(hashMask);
  UNUSED_VAR(crc);
  {
  const Byte *pLim = p + numHeads;
  if (numHeads == 0)
    return;
  pLim--;
  while (p < pLim)
  {
    UInt32 v1 = GetUi32(p);
    UInt32 v0 = v1 & 0xFFFFFF;
    UInt32 h0, h1;
    p += 2;
    v1 >>= 8;
    h0 = hash[v0]; hash[v0] = pos; heads[0] = pos - h0; pos++;
    h1 = hash[v1]; hash[v1] = pos; heads[1] = pos - h1; pos++;
    heads += 2;
  }
  if (p == pLim)
  {
    UInt32 v0 = GetUi16(p) ^ ((UInt32)(p)[2] << 16);
    *heads = pos - hash[v0];
    hash[v0] = pos;
  }
  }
}
*/

/*
GetHeads_DECL(4)
{
  unsigned sh = 0;
  UNUSED_VAR(crc)
  while ((hashMask & 0x80000000) == 0)
  {
    hashMask <<= 1;
    sh++;
  }
  GetHeads_LOOP((GetUi32(p) * 0xa54a1) >> sh)
}
#define GetHeads4b GetHeads4
*/

#define USE_GetHeads_LOCAL_CRC

#ifdef USE_GetHeads_LOCAL_CRC

GetHeads_DECL(4)
{
  UInt32 crc0[256];
  UInt32 crc1[256];
  {
    unsigned i;
    for (i = 0; i < 256; i++)
    {
      UInt32 v = crc[i];
      crc0[i] = v & hashMask;
      crc1[i] = (v << kLzHash_CrcShift_1) & hashMask;
      // crc1[i] = rotlFixed(v, 8) & hashMask;
    }
  }
  GetHeads_LOOP(crc0[p[0]] ^ crc1[p[3]] ^ (UInt32)GetUi16(p+1), OWNER_CRC(crc, p, ownerMask))
}

GetHeads_DECL(4b)
{
  UInt32 crc0[256];
  {
    unsigned i;
    for (i = 0; i < 256; i++)
      crc0[i] = crc[i] & hashMask;
  }
  GetHeads_LOOP(crc0[p[0]] ^ GetUi24hi_from32(p), OWNER_CRC(crc, p, ownerMask))
}

GetHeads_DECL(5)
{
  UInt32 crc0[256];
  UInt32 crc1[256];
  UInt32 crc2[256];
  {
    unsigned i;
    for (i = 0; i < 256; i++)
    {
      UInt32 v = crc[i];
      crc0[i] = v & hashMask;
      crc1[i] = (v << kLzHash_CrcShift_1) & hashMask;
      crc2[i] = (v << kLzHash_CrcShift_2) & hashMask;
    }
  }
  GetHeads_LOOP(crc0[p[0]] ^ crc1[p[3]] ^ crc2[p[4]] ^ (UInt32)GetUi16(p+1), OWNER_CRC(crc, p, ownerMask))
}

GetHeads_DECL(5b)
{
  UInt32 crc0[256];
  UInt32 crc1[256];
  {
    unsigned i;
    for (i = 0; i < 256; i++)
    {
      UInt32 v = crc[i];
      crc0[i] = v & hashMask;
      crc1[i] = (v << kLzHash_CrcShift_1) & hashMask;
    }
  }
  GetHeads_LOOP(crc0[p[0]] ^ crc1[p[4]] ^ GetUi24hi_from32(p), OWNER_CRC(crc, p, ownerMask))
}

#else

DEF_GetHeads(4,  (crc[p[0]] ^ (crc[p[3]] << kLzHash_CrcShift_1) ^ (UInt32)GetUi16(p+1)) & hashMask, OWNER_CRC(crc, p, ownerMask))
DEF_GetHeads(4b, (crc[p[0]] ^ GetUi24hi_from32(p)) & hashMask, OWNER_CRC(crc, p, ownerMask))
DEF_GetHeads(5,  (crc[p[0]] ^ (crc[p[3]] << kLzHash_CrcShift_1) ^ (crc[p[4]] << kLzHash_CrcShift_2) ^ (UInt32)GetUi16(p + 1)) & hashMask, OWNER_CRC(crc, p, ownerMask))
DEF_GetHeads(5b, (crc[p[0]] ^ (crc[p[4]] << kLzHash_CrcShift_1) ^ GetUi24hi_from32(p)) & hashMask, OWNER_CRC(crc, p, ownerMask))

#endif


static void HashThreadFunc(CMatchFinderMt *mt)
{
  CMtSync *p = &mt->hashSync;
    PRF(printf("\nHashThreadFunc\n"));

  for (;;)
  {
    UInt32 blockIndex = 0;
      PRF(printf("\nHashThreadFunc : Event_Wait(&p->canStart)\n"));
    Event_Wait(&p->canStart);
      PRF(printf("\nHashThreadFunc : Event_Wait(&p->canStart) : after \n"));
    if (p->exit)
    {
      PRF(printf("\nHashThreadFunc : exit \n"));
      return;
    }

    MatchFinder_Init_HighHash(MF(mt));

    for (;;)
    {
      PRF(printf("Hash thread block = %d pos = %d\n", (unsigned)blockIndex, mt->MatchFinder->pos));

      {
        CMatchFinder *mf = MF(mt);
        if (MatchFinder_NeedMove(mf))
        {
          /* Heptazip (#77): the move excludes the LZ thread and every bt thread */
          unsigned k;
          CriticalSection_Enter(&mt->lzCs);
          for (k = 0; k < mt->numBtThreads; k++)
            CriticalSection_Enter(&mt->bt[k].sync.cs);
          {
            const Byte *beforePtr = Inline_MatchFinder_GetPointerToCurrentPos(mf);
            ptrdiff_t offset;
            MatchFinder_MoveBlock(mf);
            offset = beforePtr - Inline_MatchFinder_GetPointerToCurrentPos(mf);
            mt->pointerToCurPos -= offset;
            for (k = 0; k < mt->numBtThreads; k++)
              mt->bt[k].buffer -= offset;
          }
          for (k = mt->numBtThreads; k != 0; k--)
            CriticalSection_Leave(&mt->bt[k - 1].sync.cs);
          CriticalSection_Leave(&mt->lzCs);
          continue;
        }

        Semaphore_Wait(&p->freeSemaphore);

        if (p->exit) // exit is unexpected here. But we check it here for some failure case
          return;

        // for faster stop : we check (p->stopWriting) after Wait(freeSemaphore)
        if (Z7_ATOMIC32_LOAD_RELAXED(&p->stopWriting))
          break;

        MatchFinder_ReadIfRequired(mf);
        {
          UInt32 *heads = mt->hashBuf + GET_HASH_BLOCK_OFFSET(blockIndex);
          UInt32 num = Inline_MatchFinder_GetNumAvailableBytes(mf);
          heads[0] = 2;
          heads[1] = num;

          /* heads[1] contains the number of avail bytes:
             if (avail < mf->numHashBytes) :
             {
               it means that stream was finished
               HASH_THREAD and BT_TREAD must move position for heads[1] (avail) bytes.
               HASH_THREAD doesn't stop,
               HASH_THREAD fills only the header (2 numbers) for all next blocks:
               {2, NumHashBytes - 1}, {2,0}, {2,0}, ... , {2,0}
             }
             else
             {
               HASH_THREAD and BT_TREAD must move position for (heads[0] - 2) bytes;
             }
          */

          if (num >= mf->numHashBytes)
          {
            num = num - mf->numHashBytes + 1;
            if (num > kMtHashBlockNumMax)
              num = kMtHashBlockNumMax;

            if (mf->pos > (UInt32)kMtMaxValForNormalize - num)
            {
              const UInt32 subValue = (mf->pos - mf->historySize - 1); // & ~(UInt32)(kNormalizeAlign - 1);
              MatchFinder_REDUCE_OFFSETS(mf, subValue)
              MatchFinder_Normalize3(subValue, mf->hash + mf->fixedHashSize, (size_t)mf->hashMask + 1);
            }

            heads[0] = 2 + num;
            /* Heptazip (#77): the owner bytes follow the heads */
            mt->GetHeadsFunc(mf->buffer, mf->pos, mf->hash + mf->fixedHashSize, mf->hashMask, heads + 2, num, mf->crc,
                (Byte *)(heads + 2 + num), mt->ownerMask);
          }

          mf->pos += num;  // wrap over zero is allowed at the end of stream
          mf->buffer += num;
        }
      }

      /* Heptazip (#77): the block is published to every bt thread */
      {
        unsigned k;
        CriticalSection_Enter(&p->cs);
        mt->hashRefCount[blockIndex & (kMtHashNumBlocks - 1)] = mt->numBtThreads;
        CriticalSection_Leave(&p->cs);
        blockIndex++;
        for (k = 0; k < mt->numBtThreads; k++)
          Semaphore_Release1(&mt->bt[k].hashFilled);
      }
    } // for() processing end

    // p->numBlocks_Sent = blockIndex;
    Event_Set(&p->wasStopped);
  } // for() thread end
}




/* we use one variable instead of two (cyclicBufferPos == pos) before CyclicBuf wrap.
   here we define fixed offset of (p->pos) from (p->cyclicBufferPos) */
#define CYC_TO_POS_OFFSET 0
// #define CYC_TO_POS_OFFSET 1 // for debug

#define MFMT_GM_INLINE

#ifdef MFMT_GM_INLINE

/*
  we use size_t for (pos) instead of UInt32
  to eliminate "movsx" BUG in old MSVC x64 compiler.
*/


UInt32 * Z7_FASTCALL GetMatchesSpecN_2(const Byte *lenLimit, size_t pos, const Byte *cur, CLzRef *son,
    UInt32 _cutValue, UInt32 *d, size_t _maxLen, const UInt32 *hash, const UInt32 *limit, const UInt32 *size,
    size_t _cyclicBufferPos, UInt32 _cyclicBufferSize, UInt32 sonSize,
    const Byte *owners, UInt32 ownerSel,
    UInt32 *posRes);

#endif


/* Heptazip (#77): lets the next bt thread start the hash block this thread holds
   (once per block) */
static void Bt_GrantNext(CMatchFinderMt *p, CMtBtThread *bt)
{
  if (!bt->prevGranted && bt->index + 1 < p->numBtThreads)
  {
    bt->prevGranted = True;
    Semaphore_Release1(&p->bt[bt->index + 1].prevDone);
  }
}

/* called between two output blocks: grant the current hash block to the next thread
   when we are past half of it, or when the output ring is about to block us (the LZ
   thread consumes the streams in step: it may be waiting for the next thread) */
static void Bt_GrantNextIfNeeded(CMatchFinderMt *p, CMtBtThread *bt)
{
  if (bt->prevGranted || !bt->hashBlockHeld)
    return;
  if (bt->hashBufPos - bt->hashBufStart >= kMtHashBlockNumMax / 2
      || bt->numProduced - (UInt32)Z7_ATOMIC32_LOAD_RELAXED(&bt->lzNumBlocks) >= kMtBtNumBlocks - 1)
    Bt_GrantNext(p, bt);
}

/* Heptazip (#77): the bt thread takes the next hash block. As MtSync_GetNextBlock()
   of upstream for the hash ring: the buffer lock of the thread is released while it
   waits, so the hash thread can move the buffer. Called with the lock held. */
static void Bt_TakeNextHashBlock(CMatchFinderMt *p, CMtBtThread *bt)
{
  UNLOCK_BUFFER(&bt->sync)
  if (bt->hashBlockHeld)
  {
    bt->hashBlockHeld = False;
    HashBlock_Release(p, bt->hashBlockIndex - 1);
    /* the next thread may start the block we finished */
    Bt_GrantNext(p, bt);
    bt->prevGranted = False;
  }
  /* Heptazip (#77): behind the previous thread. The nodes of 8 neighbouring positions
     share a cache line and belong to different threads: two threads inserting the same
     positions at the same time would pass every line back and forth (measured: +45%
     CPU with 2 threads, +85% with 4, for a small gain). Half a block (52K positions,
     420 KiB of nodes) later the line moves once. The previous thread grants the block
     when it finished it, when it is past half of it, or before it blocks on its output
     ring (Bt_GrantNext): so this wait never depends on a thread that is blocked. */
  if (bt->index != 0)
    Semaphore_Wait(&bt->prevDone);
  Semaphore_Wait(&bt->hashFilled);
  LOCK_BUFFER(&bt->sync)
  bt->hashBlockHeld = True;
  {
    const UInt32 bi = bt->hashBlockIndex++;
    const UInt32 k = GET_HASH_BLOCK_OFFSET(bi);
    const UInt32 *h = p->hashBuf + k;
    bt->hashNumAvail = h[1];
    bt->hashBufPosLimit = k + h[0];
    bt->hashBufPos = k + 2;
    bt->hashBufStart = k + 2;
    bt->owners = (const Byte *)(h + h[0]);
  }
}


/* Heptazip (#77): with several bt threads each thread normalizes only the nodes of
   its own positions inside its window, (pos - cyclicBufferSize, pos): the other
   threads can be at a different position (less than kMtSonExtra away) and still read
   their own nodes with their own base. The node of position (pos - i) is node
   ((cyclicBufferPos - i) mod sonSize) and its owner comes from its data, (buffer - i),
   still in the input buffer (keepSizeBefore covers the window + the lead of the hash
   thread). The nodes of older positions are not touched: this thread never reads
   them (the window check cuts them) and overwrites them when it inserts a new
   position; and a thread ahead of us may already have put a position of its own in
   such a node, which only it may touch. With one bt thread all nodes are ours. */
static void Bt_Normalize(CMatchFinderMt *p, CMtBtThread *bt, UInt32 subValue)
{
  CLzRef *son = p->son;
  if (p->numBtThreads == 1)
  {
    MatchFinder_Normalize3(subValue, son, (size_t)p->sonSize * 2);
    return;
  }
  {
    const UInt32 sonSize = p->sonSize;
    const UInt32 cbs = p->cyclicBufferSize;
    const Byte *data = bt->buffer;
    UInt32 slot = bt->cyclicBufferPos;
    UInt32 i;
    for (i = 1; i < cbs && i < bt->pos; i++)
    {
      data--;
      slot = (slot == 0 ? sonSize : slot) - 1;
      if (MtOwner(p, data) == bt->index)
      {
        CLzRef *node = son + ((size_t)slot << 1);
        UInt32 v0 = node[0], v1 = node[1];
        node[0] = (v0 < subValue ? 0 : v0 - subValue);
        node[1] = (v1 < subValue ? 0 : v1 - subValue);
      }
    }
  }
}


static void BtGetMatches(CMatchFinderMt *p, CMtBtThread *bt, UInt32 *d)
{
  UInt32 numProcessed = 0;
  UInt32 curPos = 2;

  /* GetMatchesSpec() functions don't create (len = 1)
     in [len, dist] match pairs, if (p->numHashBytes >= 2)
     Also we suppose here that (matchMaxLen >= 2).
     So the following code for (reserve) is not required
     UInt32 reserve = (p->matchMaxLen * 2);
     const UInt32 kNumHashBytes_Max = 5; // BT_HASH_BYTES_MAX
     if (reserve < kNumHashBytes_Max - 1)
        reserve = kNumHashBytes_Max - 1;
     const UInt32 limit = kMtBtBlockSize - (reserve);
  */

  const UInt32 limit = kMtBtBlockSize - (p->matchMaxLen * 2);

  d[1] = bt->hashNumAvail;

  if (bt->failure_BT)
  {
    // printf("\n == 1 BtGetMatches() p->failure_BT\n");
    d[0] = 0;
    // d[1] = 0;
    return;
  }

  while (curPos < limit)
  {
    if (bt->hashBufPos == bt->hashBufPosLimit)
    {
      UInt32 avail;
      Bt_TakeNextHashBlock(p, bt);
      avail = bt->hashNumAvail;

      {
        /* we must prevent UInt32 overflow for avail total value,
           if avail was increased with new hash block */
        UInt32 availSum = numProcessed + avail;
        if (availSum < numProcessed)
          availSum = (UInt32)(Int32)-1;
        d[1] = availSum;
      }

      if (avail >= p->numHashBytes)
        continue;

      // if (p->hashBufPos != p->hashBufPosLimit) exit(1);

      /* (avail < p->numHashBytes)
         It means that stream was finished.
         And (avail) - is a number of remaining bytes,
         we fill (d) for (avail) bytes for LZ_THREAD (receiver).
         but we don't update (p->pos) and (p->cyclicBufferPos) here in BT_THREAD */

      /* here we suppose that we have space enough:
         (kMtBtBlockSize - curPos >= p->hashNumAvail) */
      bt->hashNumAvail = 0;
      d[0] = curPos + avail;
      d += curPos;
      for (; avail != 0; avail--)
        *d++ = 0;
      return;
    }
    {
      UInt32 size = bt->hashBufPosLimit - bt->hashBufPos;
      UInt32 pos = bt->pos;
      UInt32 cyclicBufferPos = bt->cyclicBufferPos;
      UInt32 lenLimit = p->matchMaxLen;
      if (lenLimit >= bt->hashNumAvail)
        lenLimit = bt->hashNumAvail;
      {
        UInt32 size2 = bt->hashNumAvail - lenLimit + 1;
        if (size2 < size)
          size = size2;
        size2 = p->sonSize - cyclicBufferPos;
        if (size2 < size)
          size = size2;
      }

      if (pos > (UInt32)kMtMaxValForNormalize - size)
      {
        const UInt32 subValue = (pos - p->cyclicBufferSize); // & ~(UInt32)(kNormalizeAlign - 1);
        Bt_Normalize(p, bt, subValue);
        pos -= subValue;
        bt->pos = pos;
      }

      #ifndef MFMT_GM_INLINE
      #error Stop_Compiling_MFMT_GM_INLINE_is_required_for_several_bt_threads
      #else
      {
        UInt32 posRes = pos;
        const UInt32 *d_end;
        {
          d_end = GetMatchesSpecN_2(
              bt->buffer + lenLimit - 1,
              pos, bt->buffer, p->son, p->cutValue, d + curPos,
              p->numHashBytes - 1, p->hashBuf + bt->hashBufPos,
              d + limit, p->hashBuf + bt->hashBufPos + size,
              cyclicBufferPos, p->cyclicBufferSize, p->sonSize,
              p->numBtThreads == 1 ? NULL : bt->owners + (bt->hashBufPos - bt->hashBufStart),
              bt->index,
              &posRes);
        }
        {
          if (!d_end)
          {
            // printf("\n == 2 BtGetMatches() p->failure_BT\n");
            // internal data failure
            bt->failure_BT = True;
            d[0] = 0;
            // d[1] = 0;
            return;
          }
        }
        curPos = (UInt32)(d_end - d);
        {
          const UInt32 processed = posRes - pos;
          pos = posRes;
          bt->hashBufPos += processed;
          cyclicBufferPos += processed;
          bt->buffer += processed;
        }
      }
      #endif

      {
        const UInt32 processed = pos - bt->pos;
        numProcessed += processed;
        bt->hashNumAvail -= processed;
        bt->pos = pos;
      }
      if (cyclicBufferPos == p->sonSize)
        cyclicBufferPos = 0;
      bt->cyclicBufferPos = cyclicBufferPos;
    }
  }

  d[0] = curPos;
}


static void BtFillBlock(CMatchFinderMt *p, CMtBtThread *bt, UInt32 globalBlockIndex)
{
  BUFFER_MUST_BE_UNLOCKED(&bt->sync)
  LOCK_BUFFER(&bt->sync)
  BtGetMatches(p, bt, bt->btBuf + GET_BT_BLOCK_OFFSET(globalBlockIndex));
  UNLOCK_BUFFER(&bt->sync)
}


Z7_NO_INLINE
static void BtThreadFunc(CMtBtThread *bt)
{
  CMatchFinderMt *mt = bt->mt;
  CMtSync *p = &bt->sync;
  for (;;)
  {
    UInt32 blockIndex = 0;
    Event_Wait(&p->canStart);

    for (;;)
    {
        PRF(printf("  BT thread %u block = %d  pos = %d\n", (unsigned)bt->index, (unsigned)blockIndex, bt->pos));
      /* (p->exit == true) is possible after (p->canStart) at first loop iteration
         and is unexpected after more Wait(freeSemaphore) iterations */
      if (p->exit)
        return;

      Bt_GrantNextIfNeeded(mt, bt);  // Heptazip (#77): before a possible wait
      Semaphore_Wait(&p->freeSemaphore);

      // for faster stop : we check (p->stopWriting) after Wait(freeSemaphore)
      if (Z7_ATOMIC32_LOAD_RELAXED(&p->stopWriting))
        break;

      BtFillBlock(mt, bt, blockIndex++);
      bt->numProduced = blockIndex;

      Semaphore_Release1(&p->filledSemaphore);
    }

    /* Heptazip (#77): the hash block we hold goes back to the ring, so the hash
       thread (stopped by the LZ thread after all bt threads) and the other bt threads
       are never blocked by us. The hash thread is stopped in MatchFinderMt_ReleaseStream(). */
    if (bt->hashBlockHeld)
    {
      bt->hashBlockHeld = False;
      HashBlock_Release(mt, bt->hashBlockIndex - 1);
    }
    /* the next thread must not wait for us any more: it can still need up to 2 hash
       blocks to finish its current output block before it sees the stop */
    if (bt->index + 1 < mt->numBtThreads)
      Semaphore_ReleaseN(&mt->bt[bt->index + 1].prevDone, kMtHashNumBlocks);

    // p->numBlocks_Sent = blockIndex;
    Event_Set(&p->wasStopped);
  }
}


void MatchFinderMt_Construct(CMatchFinderMt *p)
{
  unsigned k;
  p->hashBuf = NULL;
  p->numBtThreads = 1;
  p->numBtThreads_Allocated = 0;
  p->ownerMask = 0;
  p->ownerByte0 = False;
  p->lzCsWasInitialized = False;
  p->lzCsWasEntered = False;
  p->needStart = True;
  MtSync_Construct(&p->hashSync);
  for (k = 0; k < MT_BT_THREADS_MAX; k++)
  {
    CMtBtThread *bt = &p->bt[k];
    MtSync_Construct(&bt->sync);
    Semaphore_Construct(&bt->hashFilled);
    Semaphore_Construct(&bt->prevDone);
    bt->mt = p;
    bt->index = k;
    bt->btBuf = NULL;
    bt->hashBlockHeld = False;
  }
}

static void MatchFinderMt_FreeMem(CMatchFinderMt *p, ISzAllocPtr alloc)
{
  unsigned k;
  ISzAlloc_Free(alloc, p->hashBuf);
  p->hashBuf = NULL;
  for (k = 0; k < MT_BT_THREADS_MAX; k++)
    p->bt[k].btBuf = NULL;
  p->numBtThreads_Allocated = 0;
}

void MatchFinderMt_Destruct(CMatchFinderMt *p, ISzAllocPtr alloc)
{
  unsigned k;
  /*
     HASH_THREAD can use CriticalSection(s) lzCs, hashSync.cs and bt[].sync.cs.
     So we must be sure that HASH_THREAD will not use CriticalSection(s)
     after deleting CriticalSection here.

     we call ReleaseStream(p)
       that calls StopWriting(bt[k].sync) for every bt thread
         and then StopWriting(hashSync), if it's required to stop HASH_THREAD.
     after StopWriting() it's safe to destruct MtSync(s) in any order */

  MatchFinderMt_ReleaseStream(p);

  for (k = 0; k < MT_BT_THREADS_MAX; k++)
  {
    MtSync_Destruct(&p->bt[k].sync);
    Semaphore_Close(&p->bt[k].hashFilled);
    Semaphore_Close(&p->bt[k].prevDone);
  }
  MtSync_Destruct(&p->hashSync);

  if (p->lzCsWasInitialized)
  {
    CriticalSection_Delete(&p->lzCs);
    p->lzCsWasInitialized = False;
  }
  p->lzCsWasEntered = False;

  LOG_ITER(
  printf("\nTree %9d * %7d iter = %9d = sum  :  bytes = %9d\n",
      (UInt32)(g_NumIters_Tree / 1000),
      (UInt32)(((UInt64)g_NumIters_Loop * 1000) / (g_NumIters_Tree + 1)),
      (UInt32)(g_NumIters_Loop / 1000),
      (UInt32)(g_NumIters_Bytes / 1000)
      ));

  MatchFinderMt_FreeMem(p, alloc);
}


#define kHashBufferSize (kMtHashBlockSize * kMtHashNumBlocks)
#define kBtBufferSize (kMtBtBlockSize * kMtBtNumBlocks)


static THREAD_FUNC_DECL HashThreadFunc2(void *p) { HashThreadFunc((CMatchFinderMt *)p);  return 0; }
static THREAD_FUNC_DECL BtThreadFunc2(void *p)
{
  Byte allocaDummy[0x180];
  unsigned i = 0;
  for (i = 0; i < 16; i++)
    allocaDummy[i] = (Byte)0;
  if (allocaDummy[0] == 0)
    BtThreadFunc((CMtBtThread *)p);
  return 0;
}


SRes MatchFinderMt_Create(CMatchFinderMt *p, UInt32 historySize, UInt32 keepAddBufferBefore,
    UInt32 matchMaxLen, UInt32 keepAddBufferAfter, ISzAllocPtr alloc)
{
  CMatchFinder *mf = MF(p);
  unsigned k;
  UInt32 numBt = p->numBtThreads;
  /* Heptazip (#77): 1, 2 or 4 bt threads (the owner is a mask of the hash value) */
  numBt = (numBt >= 4) ? 4 : (numBt >= 2 ? 2 : 1);
  p->numBtThreads = numBt;
  p->ownerMask = numBt - 1;

  p->historySize = historySize;
  if (kMtBtBlockSize <= matchMaxLen * 4)
    return SZ_ERROR_PARAM;
  if (!p->hashBuf || p->numBtThreads_Allocated < numBt)
  {
    MatchFinderMt_FreeMem(p, alloc);
    p->hashBuf = (UInt32 *)ISzAlloc_Alloc(alloc, ((size_t)kHashBufferSize + (size_t)kBtBufferSize * numBt) * sizeof(UInt32));
    if (!p->hashBuf)
      return SZ_ERROR_MEM;
    for (k = 0; k < numBt; k++)
      p->bt[k].btBuf = p->hashBuf + kHashBufferSize + (size_t)kBtBufferSize * k;
    p->numBtThreads_Allocated = numBt;
  }
  keepAddBufferBefore += (kHashBufferSize + kBtBufferSize);
  keepAddBufferAfter += kMtHashBlockSize;
  /* Heptazip (#77): the nodes of (son) outlive the window by kMtSonExtra positions
     with several bt threads. Bt_Normalize() reads the data of the nodes in the window of
     the thread, which is up to kMtHashNumBlocks hash blocks behind the hash thread: the
     input buffer keeps that much more than the window (keepAddBufferBefore includes
     kHashBufferSize + kBtBufferSize, added above). */
  mf->sonExtra = (numBt > 1) ? kMtSonExtra : 0;
  if (numBt > 1 && keepAddBufferBefore < kMtHashNumBlocks * kMtHashBlockSize)
    return SZ_ERROR_PARAM;
  if (!MatchFinder_Create(mf, historySize, keepAddBufferBefore, matchMaxLen, keepAddBufferAfter, alloc))
    return SZ_ERROR_MEM;

  if (!p->lzCsWasInitialized)
  {
    RINOK_THREAD(CriticalSection_Init(&p->lzCs))
    p->lzCsWasInitialized = True;
  }
  RINOK(MtSync_Create(&p->hashSync, HashThreadFunc2, p))
  for (k = 0; k < numBt; k++)
  {
    RINOK(MtSync_Create(&p->bt[k].sync, BtThreadFunc2, &p->bt[k]))
  }
  return SZ_OK;
}


SRes MatchFinderMt_InitMt(CMatchFinderMt *p)
{
  unsigned k;
  RINOK(MtSync_Init(&p->hashSync, kMtHashNumBlocks))
  for (k = 0; k < kMtHashNumBlocks; k++)
    p->hashRefCount[k] = 0;
  for (k = 0; k < p->numBtThreads; k++)
  {
    CMtBtThread *bt = &p->bt[k];
    RINOK(MtSync_Init(&bt->sync, kMtBtNumBlocks))
    {
      WRes wres = Semaphore_OptCreateInit(&bt->hashFilled, 0, kMtHashNumBlocks);
      if (wres == 0)  /* the stop path releases kMtHashNumBlocks more */
        wres = Semaphore_OptCreateInit(&bt->prevDone, 0, 2 * kMtHashNumBlocks + 1);
      if (wres != 0)
        return MY_SRes_HRESULT_FROM_WRes(wres);
    }
  }
  return SZ_OK;
}


static void MatchFinderMt_Init(void *_p)
{
  CMatchFinderMt *p = (CMatchFinderMt *)_p;
  CMatchFinder *mf = MF(p);
  unsigned k;

  /* Init without data reading. We don't want to read data in this thread */
  MatchFinder_Init_4(mf);

  MatchFinder_Init_LowHash(mf);

  p->pointerToCurPos = Inline_MatchFinder_GetPointerToCurrentPos(mf);
  p->btNumAvailBytes = 0;
  p->failure_LZ_BT = False;
  // p->failure_LZ_LZ = False;

  p->lzPos =
      1; // optimal smallest value
      // 0; // for debug: ignores match to start
      // kNormalizeAlign; // for debug

  p->hash = mf->hash;
  p->fixedHashSize = mf->fixedHashSize;
  // p->hash4Mask = mf->hash4Mask;
  p->crc = mf->crc;
  // memcpy(p->crc, mf->crc, sizeof(mf->crc));

  p->son = mf->son;
  p->matchMaxLen = mf->matchMaxLen;
  p->numHashBytes = mf->numHashBytes;
  p->cyclicBufferSize = mf->cyclicBufferSize;
  p->sonSize = mf->cyclicBufferSize + mf->sonExtra;
  p->cutValue = mf->cutValue;

  /* (mf->pos) and (mf->streamPos) were already initialized to 1 in MatchFinder_Init_4() */
  // mf->streamPos = mf->pos = 1; // optimal smallest value
      // 0; // for debug: ignores match to start
      // kNormalizeAlign; // for debug

  for (k = 0; k < p->numBtThreads; k++)
  {
    CMtBtThread *bt = &p->bt[k];
    bt->btBufPos =
    bt->btBufPosLimit = NULL;
    bt->lzNumBlocks = 0;
    bt->hashBufPos =
    bt->hashBufPosLimit = 0;
    bt->hashBufStart = 0;
    bt->hashNumAvail = 0; // 21.03
    bt->hashBlockIndex = 0;
    bt->hashBlockHeld = False;
    bt->prevGranted = False;
    bt->numProduced = 0;
    bt->owners = NULL;
    bt->failure_BT = False;
    /* we must init (bt->pos = mf->pos) for BT, because
       BT code needs (bt->pos == delta_value_for_empty_hash_record == mf->pos) */
    bt->pos = mf->pos; // do not change it
    bt->cyclicBufferPos = (bt->pos - CYC_TO_POS_OFFSET);
    bt->buffer = mf->buffer;
  }
  // p->son[0] = p->son[1] = 0; // unused: to init skipped record for speculated accesses.
}


/* Heptazip (#77): the LZ thread stops every bt thread first (all of them get the
   stop message before we wait for any: a bt thread blocked on the hash ring is freed
   when the others leave), then the hash thread. */
void MatchFinderMt_ReleaseStream(CMatchFinderMt *p)
{
  unsigned k;
  // Sleep(1); // for debug
  if (p->needStart)
    return;
  if (p->lzCsWasEntered)
    LZ_UNLOCK(p)
  for (k = 0; k < p->numBtThreads; k++)
  {
    CMtSync *s = &p->bt[k].sync;
    if (Thread_WasCreated(&s->thread) && !s->needStart)
    {
      Z7_ATOMIC32_STORE_RELAXED(&s->stopWriting, True);
      Semaphore_Release1(&s->freeSemaphore);
    }
  }
  for (k = 0; k < p->numBtThreads; k++)
  {
    CMtSync *s = &p->bt[k].sync;
    if (Thread_WasCreated(&s->thread) && !s->needStart)
    {
      Event_Wait(&s->wasStopped);
      s->needStart = True;
    }
  }
  MtSync_StopWriting(&p->hashSync);
  p->needStart = True;
  // Sleep(200); // for debug
  /* p->MatchFinder->ReleaseStream(); */
}


/* Heptazip (#77): the first block request of a stream starts all the threads */
static void MatchFinderMt_StartThreads(CMatchFinderMt *p)
{
  unsigned k;
  MtSync_Start(&p->hashSync);
  for (k = 0; k < p->numBtThreads; k++)
    MtSync_Start(&p->bt[k].sync);
  p->needStart = False;
}


/* Heptazip (#77): the LZ thread takes the next block of stream (k). The LZ lock is
   released while it waits (the hash thread can move the buffer), as in
   MtSync_GetNextBlock() of upstream; the LZ thread holds no other lock meanwhile. */
Z7_NO_INLINE
static UInt32 Lz_TakeNextBlock(CMatchFinderMt *p, CMtBtThread *bt)
{
  if (p->needStart)
    MatchFinderMt_StartThreads(p);
  else
  {
    if (p->lzCsWasEntered)
      LZ_UNLOCK(p)
    if (bt->lzNumBlocks != 0)
      Semaphore_Release1(&bt->sync.freeSemaphore);
  }
  Semaphore_Wait(&bt->sync.filledSemaphore);
  LZ_LOCK(p)
  {
    const UInt32 bi = bt->lzNumBlocks;
    Z7_ATOMIC32_STORE_RELAXED(&bt->lzNumBlocks, bi + 1);  // read by the bt thread (Bt_GrantNextIfNeeded)
    return bi;
  }
}


static UInt32 MatchFinderMt_GetNextBlock_Bt(CMatchFinderMt *p)
{
  unsigned k;
  for (k = 0; k < p->numBtThreads; k++)
  {
    CMtBtThread *bt = &p->bt[k];
    if (bt->btBufPos != bt->btBufPosLimit)
      continue;
    if (p->failure_LZ_BT)
    {
      bt->failureBuf[0] = 0;
      bt->btBufPos = bt->failureBuf;
      bt->btBufPosLimit = bt->failureBuf + 1;
      continue;
    }
    {
      const UInt32 bi = Lz_TakeNextBlock(p, bt);
      const UInt32 *b = bt->btBuf + GET_BT_BLOCK_OFFSET(bi);
      const UInt32 numItems = b[0];
      bt->btBufPosLimit = b + numItems;
      p->btNumAvailBytes = b[1];
      bt->btBufPos = b + 2;
      if (numItems < 2 || numItems > kMtBtBlockSize)
      {
        bt->failureBuf[0] = 0;
        bt->btBufPos = bt->failureBuf;
        bt->btBufPosLimit = bt->failureBuf + 1;
        p->failure_LZ_BT = True;
        // p->btNumAvailBytes = 0;
        /* we don't want to decrease AvailBytes, that was load before.
            that can be unxepected for the code that have loaded anopther value before */
      }
    }
  }

  if (!p->failure_LZ_BT
      && p->lzPos >= (UInt32)kMtMaxValForNormalize - (UInt32)kMtBtBlockSize)
  {
    /* we don't check (lzPos) over exact avail bytes in (btBuf).
       (fixedHashSize) is small, so normalization is fast */
    const UInt32 subValue = (p->lzPos - p->historySize - 1); // & ~(UInt32)(kNormalizeAlign - 1);
    p->lzPos -= subValue;
    MatchFinder_Normalize3(subValue, p->hash, p->fixedHashSize);
  }
  return p->btNumAvailBytes;
}


static const Byte * MatchFinderMt_GetPointerToCurrentPos(void *_p)
{
  CMatchFinderMt *p = (CMatchFinderMt *)_p;
  return p->pointerToCurPos;
}


/* Heptazip (#77): true when some stream needs its next block */
static BoolInt MatchFinderMt_NeedBlock(const CMatchFinderMt *p)
{
  unsigned k;
  if (p->numBtThreads == 1)
    return p->bt[0].btBufPos == p->bt[0].btBufPosLimit;
  for (k = 0; k < p->numBtThreads; k++)
    if (p->bt[k].btBufPos == p->bt[k].btBufPosLimit)
      return True;
  return False;
}

#define GET_NEXT_BLOCK_IF_REQUIRED if (MatchFinderMt_NeedBlock(p)) MatchFinderMt_GetNextBlock_Bt(p);


static UInt32 MatchFinderMt_GetNumAvailableBytes(void *_p)
{
  CMatchFinderMt *p = (CMatchFinderMt *)_p;
  if (!MatchFinderMt_NeedBlock(p))
    return p->btNumAvailBytes;
  return MatchFinderMt_GetNextBlock_Bt(p);
}


/* Heptazip (#77): the entry of the current position: the stream of the owner has
   the number of pair words followed by the pairs, every other stream has the word
   kMtNotOwned (or 0 for every stream after the end of the data). Returns the stream
   pointer at the count and advances the other streams by one word. */
static const UInt32 * Lz_TakeEntry(CMatchFinderMt *p)
{
  const UInt32 n = p->numBtThreads;
  unsigned k;
  const UInt32 *owner = NULL;
  if (n == 1)
    return p->bt[0].btBufPos;  /* one stream: as upstream */
  for (k = 0; k < n; k++)
  {
    CMtBtThread *bt = &p->bt[k];
    const UInt32 *b = bt->btBufPos;
    if (!owner && *b != kMtNotOwned)
    {
      owner = b;
      continue;
    }
    bt->btBufPos = b + 1;
  }
  if (!owner)
  {
    /* every stream says "not owned": internal data failure */
    p->failure_LZ_BT = True;
    p->bt[0].failureBuf[0] = 0;
    p->bt[0].btBufPos = p->bt[0].failureBuf;
    p->bt[0].btBufPosLimit = p->bt[0].failureBuf + 1;
    owner = p->bt[0].failureBuf;
  }
  return owner;
}

/* the owner stream pointer (b) is at the count: advance it past the entry */
#define LZ_ADVANCE_OWNER(p, b, len) { \
    if ((p)->numBtThreads == 1) (p)->bt[0].btBufPos = (b) + 1 + (len); \
    else { unsigned k_; \
    for (k_ = 0; k_ < (p)->numBtThreads; k_++) \
      if ((p)->bt[k_].btBufPos == (b)) { (p)->bt[k_].btBufPos = (b) + 1 + (len); break; } } }


// #define CHECK_FAILURE_LZ(_match_, _pos_) if (_match_ >= _pos_) { p->failure_LZ_LZ = True;  return d; }
#define CHECK_FAILURE_LZ(_match_, _pos_)

static UInt32 * MixMatches2(CMatchFinderMt *p, UInt32 matchMinPos, UInt32 *d)
{
  UInt32 h2, c2;
  UInt32 *hash = p->hash;
  const Byte *cur = p->pointerToCurPos;
  const UInt32 m = p->lzPos;
  MT_HASH2_CALC

  c2 = hash[h2];
  hash[h2] = m;

  if (c2 >= matchMinPos)
  {
    CHECK_FAILURE_LZ(c2, m)
    if (cur[(ptrdiff_t)c2 - (ptrdiff_t)m] == cur[0])
    {
      *d++ = 2;
      *d++ = m - c2 - 1;
    }
  }

  return d;
}

static UInt32 * MixMatches3(CMatchFinderMt *p, UInt32 matchMinPos, UInt32 *d)
{
  UInt32 h2, h3, c2, c3;
  UInt32 *hash = p->hash;
  const Byte *cur = p->pointerToCurPos;
  const UInt32 m = p->lzPos;
  MT_HASH3_CALC

  c2 = hash[h2];
  c3 = (hash + kFix3HashSize)[h3];

  hash[h2] = m;
  (hash + kFix3HashSize)[h3] = m;

  if (c2 >= matchMinPos)
  {
    CHECK_FAILURE_LZ(c2, m)
    if (cur[(ptrdiff_t)c2 - (ptrdiff_t)m] == cur[0])
    {
      d[1] = m - c2 - 1;
      if (cur[(ptrdiff_t)c2 - (ptrdiff_t)m + 2] == cur[2])
      {
        d[0] = 3;
        return d + 2;
      }
      d[0] = 2;
      d += 2;
    }
  }

  if (c3 >= matchMinPos)
  {
    CHECK_FAILURE_LZ(c3, m)
    if (cur[(ptrdiff_t)c3 - (ptrdiff_t)m] == cur[0])
    {
      *d++ = 3;
      *d++ = m - c3 - 1;
    }
  }

  return d;
}


#define INCREASE_LZ_POS p->lzPos++; p->pointerToCurPos++;

/*
static
UInt32* MatchFinderMt_GetMatches_Bt4(CMatchFinderMt *p, UInt32 *d)
{
  // Heptazip: the inline version of GetMatches() for bt4 of upstream was not used
  // (vTable->GetMatches = MatchFinderMt_GetMatches); removed with the streams split (#77).
}
*/


static UInt32 * MixMatches4(CMatchFinderMt *p, UInt32 matchMinPos, UInt32 *d)
{
  UInt32 h2, h3, /* h4, */ c2, c3 /* , c4 */;
  UInt32 *hash = p->hash;
  const Byte *cur = p->pointerToCurPos;
  const UInt32 m = p->lzPos;
  MT_HASH3_CALC
  // MT_HASH4_CALC
  c2 = hash[h2];
  c3 = (hash + kFix3HashSize)[h3];
  // c4 = (hash + kFix4HashSize)[h4];

  hash[h2] = m;
  (hash + kFix3HashSize)[h3] = m;
  // (hash + kFix4HashSize)[h4] = m;

  #define _USE_H2

  #ifdef _USE_H2
  if (c2 >= matchMinPos && cur[(ptrdiff_t)c2 - (ptrdiff_t)m] == cur[0])
  {
    d[1] = m - c2 - 1;
    if (cur[(ptrdiff_t)c2 - (ptrdiff_t)m + 2] == cur[2])
    {
      // d[0] = (cur[(ptrdiff_t)c2 - (ptrdiff_t)m + 3] == cur[3]) ? 4 : 3;
      // return d + 2;

      if (cur[(ptrdiff_t)c2 - (ptrdiff_t)m + 3] == cur[3])
      {
        d[0] = 4;
        return d + 2;
      }
      d[0] = 3;
      d += 2;

      #ifdef _USE_H4
      if (c4 >= matchMinPos)
        if (
          cur[(ptrdiff_t)c4 - (ptrdiff_t)m]     == cur[0] &&
          cur[(ptrdiff_t)c4 - (ptrdiff_t)m + 3] == cur[3]
          )
      {
        *d++ = 4;
        *d++ = m - c4 - 1;
      }
      #endif
      return d;
    }
    d[0] = 2;
    d += 2;
  }
  #endif

  if (c3 >= matchMinPos && cur[(ptrdiff_t)c3 - (ptrdiff_t)m] == cur[0])
  {
    d[1] = m - c3 - 1;
    if (cur[(ptrdiff_t)c3 - (ptrdiff_t)m + 3] == cur[3])
    {
      d[0] = 4;
      return d + 2;
    }
    d[0] = 3;
    d += 2;
  }

  #ifdef _USE_H4
  if (c4 >= matchMinPos)
    if (
      cur[(ptrdiff_t)c4 - (ptrdiff_t)m]     == cur[0] &&
      cur[(ptrdiff_t)c4 - (ptrdiff_t)m + 3] == cur[3]
      )
    {
      *d++ = 4;
      *d++ = m - c4 - 1;
    }
  #endif

  return d;
}


static UInt32 * MatchFinderMt2_GetMatches(void *_p, UInt32 *d)
{
  CMatchFinderMt *p = (CMatchFinderMt *)_p;
  const UInt32 *bt = Lz_TakeEntry(p);
  const UInt32 len = *bt;
  const UInt32 *btLim = bt + 1 + len;
  LZ_ADVANCE_OWNER(p, bt, len)
  bt++;
  p->btNumAvailBytes--;
  INCREASE_LZ_POS
  {
    while (bt != btLim)
    {
      const UInt32 v0 = bt[0];
      const UInt32 v1 = bt[1];
      bt += 2;
      d[0] = v0;
      d[1] = v1;
      d += 2;
    }
  }
  return d;
}



static UInt32 * MatchFinderMt_GetMatches(void *_p, UInt32 *d)
{
  CMatchFinderMt *p = (CMatchFinderMt *)_p;
  const UInt32 *bt = Lz_TakeEntry(p);
  UInt32 len = *bt;
  const UInt32 avail = p->btNumAvailBytes - 1;
  p->btNumAvailBytes = avail;
  LZ_ADVANCE_OWNER(p, bt, len)
  bt++;
  if (len == 0)
  {
    #define BT_HASH_BYTES_MAX 5
    if (avail >= (BT_HASH_BYTES_MAX - 1) - 1)
    {
      UInt32 m = p->lzPos;
      if (m > p->historySize)
        m -= p->historySize;
      else
        m = 1;
      d = p->MixMatchesFunc(p, m, d);
    }
  }
  else
  {
    /*
      first match pair from BinTree: (match_len, match_dist),
      (match_len >= numHashBytes).
      MixMatchesFunc() inserts only hash matches that are nearer than (match_dist)
    */
    d = p->MixMatchesFunc(p, p->lzPos - bt[1], d);
    // if (d) // check for failure
    do
    {
      const UInt32 v0 = bt[0];
      const UInt32 v1 = bt[1];
      bt += 2;
      d[0] = v0;
      d[1] = v1;
      d += 2;
    }
    while (len -= 2);
  }
  INCREASE_LZ_POS
  return d;
}


/* Heptazip (#77): skip the entry of the current position in every stream */
static void Lz_SkipEntry(CMatchFinderMt *p)
{
  const UInt32 *bt = Lz_TakeEntry(p);
  LZ_ADVANCE_OWNER(p, bt, *bt)
}

#define SKIP_HEADER2_MT  do { GET_NEXT_BLOCK_IF_REQUIRED
#define SKIP_HEADER_MT(n) SKIP_HEADER2_MT if (p->btNumAvailBytes-- >= (n)) { const Byte *cur = p->pointerToCurPos; UInt32 *hash = p->hash;
#define SKIP_FOOTER_MT } INCREASE_LZ_POS Lz_SkipEntry(p); } while (--num != 0);

static void MatchFinderMt0_Skip(void *_p, UInt32 num)
{
  CMatchFinderMt *p = (CMatchFinderMt *)_p;
  SKIP_HEADER2_MT { p->btNumAvailBytes--;
  SKIP_FOOTER_MT
}

static void MatchFinderMt2_Skip(void *_p, UInt32 num)
{
  CMatchFinderMt *p = (CMatchFinderMt *)_p;
  SKIP_HEADER_MT(2)
      UInt32 h2;
      MT_HASH2_CALC
      hash[h2] = p->lzPos;
  SKIP_FOOTER_MT
}

static void MatchFinderMt3_Skip(void *_p, UInt32 num)
{
  CMatchFinderMt *p = (CMatchFinderMt *)_p;
  SKIP_HEADER_MT(3)
      UInt32 h2, h3;
      MT_HASH3_CALC
      (hash + kFix3HashSize)[h3] =
      hash[                h2] =
        p->lzPos;
  SKIP_FOOTER_MT
}

/*
// MatchFinderMt4_Skip() is similar to MatchFinderMt3_Skip().
// The difference is that MatchFinderMt3_Skip() updates hash for last 3 bytes of stream.

static void MatchFinderMt4_Skip(CMatchFinderMt *p, UInt32 num)
{
  SKIP_HEADER_MT(4)
      UInt32 h2, h3; // h4
      MT_HASH3_CALC
      // MT_HASH4_CALC
      // (hash + kFix4HashSize)[h4] =
      (hash + kFix3HashSize)[h3] =
      hash[                h2] =
        p->lzPos;
  SKIP_FOOTER_MT
}
*/

void MatchFinderMt_CreateVTable(CMatchFinderMt *p, IMatchFinder2 *vTable)
{
  vTable->Init = MatchFinderMt_Init;
  vTable->GetNumAvailableBytes = MatchFinderMt_GetNumAvailableBytes;
  vTable->GetPointerToCurrentPos = MatchFinderMt_GetPointerToCurrentPos;
  vTable->GetMatches = MatchFinderMt_GetMatches;
  /* Heptazip (#77): the owner of a position depends on the hash family, see MtOwner() */
  p->ownerByte0 = False;

  switch (MF(p)->numHashBytes)
  {
    case 2:
      p->GetHeadsFunc = GetHeads2;
      p->MixMatchesFunc = NULL;
      vTable->Skip = MatchFinderMt0_Skip;
      vTable->GetMatches = MatchFinderMt2_GetMatches;
      p->ownerByte0 = True;
      break;
    case 3:
      p->GetHeadsFunc = MF(p)->bigHash ? GetHeads3b : GetHeads3;
      p->MixMatchesFunc = MixMatches2;
      vTable->Skip = MatchFinderMt2_Skip;
      p->ownerByte0 = MF(p)->bigHash;
      break;
    case 4:
      p->GetHeadsFunc = MF(p)->bigHash ? GetHeads4b : GetHeads4;

      // it's fast inline version of GetMatches()
      // vTable->GetMatches = MatchFinderMt_GetMatches_Bt4;

      p->MixMatchesFunc = MixMatches3;
      vTable->Skip = MatchFinderMt3_Skip;
      break;
    default:
      p->GetHeadsFunc = MF(p)->bigHash ? GetHeads5b : GetHeads5;
      p->MixMatchesFunc = MixMatches4;
      vTable->Skip =
          MatchFinderMt3_Skip;
          // MatchFinderMt4_Skip;
      break;
  }
}

#undef RINOK_THREAD
#undef PRF
#undef MF
#undef GetUi24hi_from32
#undef LOCK_BUFFER
#undef UNLOCK_BUFFER
#undef LZ_LOCK
#undef LZ_UNLOCK
