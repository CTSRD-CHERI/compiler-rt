//===-- scudo_allocator.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// Scudo Hardened Allocator implementation.
/// It uses the sanitizer_common allocator as a base and aims at mitigating
/// heap corruption vulnerabilities. It provides a checksum-guarded chunk
/// header, a delayed free list, and additional sanity checks.
///
//===----------------------------------------------------------------------===//

#include "scudo_allocator.h"
#include "scudo_crc32.h"
#include "scudo_flags.h"
#include "scudo_tsd.h"
#include "scudo_utils.h"

#include "sanitizer_common/sanitizer_allocator_checks.h"
#include "sanitizer_common/sanitizer_allocator_interface.h"
#include "sanitizer_common/sanitizer_quarantine.h"

#include <errno.h>
#include <string.h>

namespace __scudo {

// Global static cookie, initialized at start-up.
static u32 Cookie;

// We default to software CRC32 if the alternatives are not supported, either
// at compilation or at runtime.
static atomic_uint8_t HashAlgorithm = { CRC32Software };

INLINE u32 computeCRC32(u32 Crc, uptr Value, uptr *Array, uptr ArraySize) {
  // If the hardware CRC32 feature is defined here, it was enabled everywhere,
  // as opposed to only for scudo_crc32.cpp. This means that other hardware
  // specific instructions were likely emitted at other places, and as a
  // result there is no reason to not use it here.
#if defined(__SSE4_2__) || defined(__ARM_FEATURE_CRC32)
  Crc = CRC32_INTRINSIC(Crc, Value);
  for (uptr i = 0; i < ArraySize; i++)
    Crc = CRC32_INTRINSIC(Crc, Array[i]);
  return Crc;
#else
  if (atomic_load_relaxed(&HashAlgorithm) == CRC32Hardware) {
    Crc = computeHardwareCRC32(Crc, Value);
    for (uptr i = 0; i < ArraySize; i++)
      Crc = computeHardwareCRC32(Crc, Array[i]);
    return Crc;
  }
  Crc = computeSoftwareCRC32(Crc, Value);
  for (uptr i = 0; i < ArraySize; i++)
    Crc = computeSoftwareCRC32(Crc, Array[i]);
  return Crc;
#endif  // defined(__SSE4_2__) || defined(__ARM_FEATURE_CRC32)
}

static ScudoBackendAllocator &getBackendAllocator();

struct ScudoChunk : UnpackedHeader {
  // We can't use the offset member of the chunk itself, as we would double
  // fetch it without any warranty that it wouldn't have been tampered. To
  // prevent this, we work with a local copy of the header.
  void *getBackendPtr(UnpackedHeader *Header) {
    return reinterpret_cast<void *>(
        reinterpret_cast<uptr>(this) - (Header->Offset << MinAlignmentLog));
  }

  // Returns the usable size for a chunk, meaning the amount of bytes from the
  // beginning of the user data to the end of the backend allocated chunk.
  uptr getUsableSize(UnpackedHeader *Header) {
    const uptr Size =
        getBackendAllocator().getActuallyAllocatedSize(getBackendPtr(Header),
                                                       Header->ClassId);
    if (Size == 0)
      return 0;
    return Size - AlignedChunkHeaderSize - (Header->Offset << MinAlignmentLog);
  }

  // Compute the checksum of the Chunk pointer and its ChunkHeader.
  u16 computeChecksum(UnpackedHeader *Header) const {
    UnpackedHeader ZeroChecksumHeader = *Header;
    ZeroChecksumHeader.Checksum = 0;
    uptr HeaderHolder[sizeof(UnpackedHeader) / sizeof(uptr)];
    memcpy(&HeaderHolder, &ZeroChecksumHeader, sizeof(HeaderHolder));
    u32 Crc = computeCRC32(Cookie, reinterpret_cast<uptr>(this), HeaderHolder,
                           ARRAY_SIZE(HeaderHolder));
    return static_cast<u16>(Crc);
  }

  // Checks the validity of a chunk by verifying its checksum. It doesn't
  // incur termination in the event of an invalid chunk.
  bool isValid() {
    UnpackedHeader NewUnpackedHeader;
    const AtomicPackedHeader *AtomicHeader =
        reinterpret_cast<const AtomicPackedHeader *>(this);
    PackedHeader NewPackedHeader = atomic_load_relaxed(AtomicHeader);
    NewUnpackedHeader = bit_cast<UnpackedHeader>(NewPackedHeader);
    return (NewUnpackedHeader.Checksum == computeChecksum(&NewUnpackedHeader));
  }

  // Nulls out a chunk header. When returning the chunk to the backend, there
  // is no need to store a valid ChunkAvailable header, as this would be
  // computationally expensive. Zeroing out serves the same purpose by making
  // the header invalid. In the extremely rare event where 0 would be a valid
  // checksum for the chunk, the state of the chunk is ChunkAvailable anyway.
  COMPILER_CHECK(ChunkAvailable == 0);
  void eraseHeader() {
    PackedHeader NullPackedHeader = 0;
    AtomicPackedHeader *AtomicHeader =
        reinterpret_cast<AtomicPackedHeader *>(this);
    atomic_store_relaxed(AtomicHeader, NullPackedHeader);
  }

  // Loads and unpacks the header, verifying the checksum in the process.
  void loadHeader(UnpackedHeader *NewUnpackedHeader) const {
    const AtomicPackedHeader *AtomicHeader =
        reinterpret_cast<const AtomicPackedHeader *>(this);
    PackedHeader NewPackedHeader = atomic_load_relaxed(AtomicHeader);
    *NewUnpackedHeader = bit_cast<UnpackedHeader>(NewPackedHeader);
    if (UNLIKELY(NewUnpackedHeader->Checksum !=
        computeChecksum(NewUnpackedHeader))) {
      dieWithMessage("ERROR: corrupted chunk header at address %p\n", this);
    }
  }

  // Packs and stores the header, computing the checksum in the process.
  void storeHeader(UnpackedHeader *NewUnpackedHeader) {
    NewUnpackedHeader->Checksum = computeChecksum(NewUnpackedHeader);
    PackedHeader NewPackedHeader = bit_cast<PackedHeader>(*NewUnpackedHeader);
    AtomicPackedHeader *AtomicHeader =
        reinterpret_cast<AtomicPackedHeader *>(this);
    atomic_store_relaxed(AtomicHeader, NewPackedHeader);
  }

  // Packs and stores the header, computing the checksum in the process. We
  // compare the current header with the expected provided one to ensure that
  // we are not being raced by a corruption occurring in another thread.
  void compareExchangeHeader(UnpackedHeader *NewUnpackedHeader,
                             UnpackedHeader *OldUnpackedHeader) {
    NewUnpackedHeader->Checksum = computeChecksum(NewUnpackedHeader);
    PackedHeader NewPackedHeader = bit_cast<PackedHeader>(*NewUnpackedHeader);
    PackedHeader OldPackedHeader = bit_cast<PackedHeader>(*OldUnpackedHeader);
    AtomicPackedHeader *AtomicHeader =
        reinterpret_cast<AtomicPackedHeader *>(this);
    if (UNLIKELY(!atomic_compare_exchange_strong(AtomicHeader,
                                                 &OldPackedHeader,
                                                 NewPackedHeader,
                                                 memory_order_relaxed))) {
      dieWithMessage("ERROR: race on chunk header at address %p\n", this);
    }
  }
};

INLINE ScudoChunk *getScudoChunk(uptr UserBeg) {
  return reinterpret_cast<ScudoChunk *>(UserBeg - AlignedChunkHeaderSize);
}

struct QuarantineCallback {
  explicit QuarantineCallback(AllocatorCache *Cache)
    : Cache_(Cache) {}

  // Chunk recycling function, returns a quarantined chunk to the backend,
  // first making sure it hasn't been tampered with.
  void Recycle(ScudoChunk *Chunk) {
    UnpackedHeader Header;
    Chunk->loadHeader(&Header);
    if (UNLIKELY(Header.State != ChunkQuarantine)) {
      dieWithMessage("ERROR: invalid chunk state when recycling address %p\n",
                     Chunk);
    }
    Chunk->eraseHeader();
    void *Ptr = Chunk->getBackendPtr(&Header);
    if (Header.ClassId)
      getBackendAllocator().deallocatePrimary(Cache_, Ptr, Header.ClassId);
    else
      getBackendAllocator().deallocateSecondary(Ptr);
  }

  // Internal quarantine allocation and deallocation functions. We first check
  // that the batches are indeed serviced by the Primary.
  // TODO(kostyak): figure out the best way to protect the batches.
  void *Allocate(uptr Size) {
    return getBackendAllocator().allocatePrimary(Cache_, BatchClassId);
  }

  void Deallocate(void *Ptr) {
    getBackendAllocator().deallocatePrimary(Cache_, Ptr, BatchClassId);
  }

  AllocatorCache *Cache_;
  COMPILER_CHECK(sizeof(QuarantineBatch) < SizeClassMap::kMaxSize);
  const uptr BatchClassId = SizeClassMap::ClassID(sizeof(QuarantineBatch));
};

typedef Quarantine<QuarantineCallback, ScudoChunk> ScudoQuarantine;
typedef ScudoQuarantine::Cache ScudoQuarantineCache;
COMPILER_CHECK(sizeof(ScudoQuarantineCache) <=
               sizeof(ScudoTSD::QuarantineCachePlaceHolder));

ScudoQuarantineCache *getQuarantineCache(ScudoTSD *TSD) {
  return reinterpret_cast<ScudoQuarantineCache *>(
      TSD->QuarantineCachePlaceHolder);
}

struct ScudoAllocator {
  static const uptr MaxAllowedMallocSize =
      FIRST_32_SECOND_64(2UL << 30, 1ULL << 40);

  typedef ReturnNullOrDieOnFailure FailureHandler;

  ScudoBackendAllocator BackendAllocator;
  ScudoQuarantine AllocatorQuarantine;

  u32 QuarantineChunksUpToSize;

  bool DeallocationTypeMismatch;
  bool ZeroContents;
  bool DeleteSizeMismatch;

  bool CheckRssLimit;
  uptr HardRssLimitMb;
  uptr SoftRssLimitMb;
  atomic_uint8_t RssLimitExceeded;
  atomic_uint64_t RssLastCheckedAtNS;

  explicit ScudoAllocator(LinkerInitialized)
    : AllocatorQuarantine(LINKER_INITIALIZED) {}

  void performSanityChecks() {
    // Verify that the header offset field can hold the maximum offset. In the
    // case of the Secondary allocator, it takes care of alignment and the
    // offset will always be 0. In the case of the Primary, the worst case
    // scenario happens in the last size class, when the backend allocation
    // would already be aligned on the requested alignment, which would happen
    // to be the maximum alignment that would fit in that size class. As a
    // result, the maximum offset will be at most the maximum alignment for the
    // last size class minus the header size, in multiples of MinAlignment.
    UnpackedHeader Header = {};
    const uptr MaxPrimaryAlignment =
        1 << MostSignificantSetBitIndex(SizeClassMap::kMaxSize - MinAlignment);
    const uptr MaxOffset =
        (MaxPrimaryAlignment - AlignedChunkHeaderSize) >> MinAlignmentLog;
    Header.Offset = MaxOffset;
    if (Header.Offset != MaxOffset) {
      dieWithMessage("ERROR: the maximum possible offset doesn't fit in the "
                     "header\n");
    }
    // Verify that we can fit the maximum size or amount of unused bytes in the
    // header. Given that the Secondary fits the allocation to a page, the worst
    // case scenario happens in the Primary. It will depend on the second to
    // last and last class sizes, as well as the dynamic base for the Primary.
    // The following is an over-approximation that works for our needs.
    const uptr MaxSizeOrUnusedBytes = SizeClassMap::kMaxSize - 1;
    Header.SizeOrUnusedBytes = MaxSizeOrUnusedBytes;
    if (Header.SizeOrUnusedBytes != MaxSizeOrUnusedBytes) {
      dieWithMessage("ERROR: the maximum possible unused bytes doesn't fit in "
                     "the header\n");
    }

    const uptr LargestClassId = SizeClassMap::kLargestClassID;
    Header.ClassId = LargestClassId;
    if (Header.ClassId != LargestClassId) {
      dieWithMessage("ERROR: the largest class ID doesn't fit in the header\n");
    }
  }

  void init() {
    SanitizerToolName = "Scudo";
    initFlags();

    performSanityChecks();

    // Check if hardware CRC32 is supported in the binary and by the platform,
    // if so, opt for the CRC32 hardware version of the checksum.
    if (&computeHardwareCRC32 && hasHardwareCRC32())
      atomic_store_relaxed(&HashAlgorithm, CRC32Hardware);

    SetAllocatorMayReturnNull(common_flags()->allocator_may_return_null);
    BackendAllocator.init(common_flags()->allocator_release_to_os_interval_ms);
    HardRssLimitMb = common_flags()->hard_rss_limit_mb;
    SoftRssLimitMb = common_flags()->soft_rss_limit_mb;
    AllocatorQuarantine.Init(
        static_cast<uptr>(getFlags()->QuarantineSizeKb) << 10,
        static_cast<uptr>(getFlags()->ThreadLocalQuarantineSizeKb) << 10);
    QuarantineChunksUpToSize = getFlags()->QuarantineChunksUpToSize;
    DeallocationTypeMismatch = getFlags()->DeallocationTypeMismatch;
    DeleteSizeMismatch = getFlags()->DeleteSizeMismatch;
    ZeroContents = getFlags()->ZeroContents;

    if (UNLIKELY(!GetRandom(reinterpret_cast<void *>(&Cookie), sizeof(Cookie),
                            /*blocking=*/false))) {
      Cookie = static_cast<u32>((NanoTime() >> 12) ^
                                (reinterpret_cast<uptr>(this) >> 4));
    }

    CheckRssLimit = HardRssLimitMb || SoftRssLimitMb;
    if (CheckRssLimit)
      atomic_store_relaxed(&RssLastCheckedAtNS, MonotonicNanoTime());
  }

  // Helper function that checks for a valid Scudo chunk. nullptr isn't.
  bool isValidPointer(const void *UserPtr) {
    initThreadMaybe();
    if (UNLIKELY(!UserPtr))
      return false;
    uptr UserBeg = reinterpret_cast<uptr>(UserPtr);
    if (!IsAligned(UserBeg, MinAlignment))
      return false;
    return getScudoChunk(UserBeg)->isValid();
  }

  // Opportunistic RSS limit check. This will update the RSS limit status, if
  // it can, every 100ms, otherwise it will just return the current one.
  bool isRssLimitExceeded() {
    u64 LastCheck = atomic_load_relaxed(&RssLastCheckedAtNS);
    const u64 CurrentCheck = MonotonicNanoTime();
    if (LIKELY(CurrentCheck < LastCheck + (100ULL * 1000000ULL)))
      return atomic_load_relaxed(&RssLimitExceeded);
    if (!atomic_compare_exchange_weak(&RssLastCheckedAtNS, &LastCheck,
                                      CurrentCheck, memory_order_relaxed))
      return atomic_load_relaxed(&RssLimitExceeded);
    // TODO(kostyak): We currently use sanitizer_common's GetRSS which reads the
    //                RSS from /proc/self/statm by default. We might want to
    //                call getrusage directly, even if it's less accurate.
    const uptr CurrentRssMb = GetRSS() >> 20;
    if (HardRssLimitMb && HardRssLimitMb < CurrentRssMb) {
      Report("%s: hard RSS limit exhausted (%zdMb vs %zdMb)\n",
             SanitizerToolName, HardRssLimitMb, CurrentRssMb);
      DumpProcessMap();
      Die();
    }
    if (SoftRssLimitMb) {
      if (atomic_load_relaxed(&RssLimitExceeded)) {
        if (CurrentRssMb <= SoftRssLimitMb)
          atomic_store_relaxed(&RssLimitExceeded, false);
      } else {
        if (CurrentRssMb > SoftRssLimitMb) {
          atomic_store_relaxed(&RssLimitExceeded, true);
          Report("%s: soft RSS limit exhausted (%zdMb vs %zdMb)\n",
                 SanitizerToolName, SoftRssLimitMb, CurrentRssMb);
        }
      }
    }
    return atomic_load_relaxed(&RssLimitExceeded);
  }

  // Allocates a chunk.
  void *allocate(uptr Size, uptr Alignment, AllocType Type,
                 bool ForceZeroContents = false) {
    initThreadMaybe();
    if (UNLIKELY(Alignment > MaxAlignment))
      return FailureHandler::OnBadRequest();
    if (UNLIKELY(Alignment < MinAlignment))
      Alignment = MinAlignment;
    if (UNLIKELY(Size >= MaxAllowedMallocSize))
      return FailureHandler::OnBadRequest();
    if (UNLIKELY(Size == 0))
      Size = 1;

    uptr NeededSize = RoundUpTo(Size, MinAlignment) + AlignedChunkHeaderSize;
    uptr AlignedSize = (Alignment > MinAlignment) ?
        NeededSize + (Alignment - AlignedChunkHeaderSize) : NeededSize;
    if (UNLIKELY(AlignedSize >= MaxAllowedMallocSize))
      return FailureHandler::OnBadRequest();

    if (CheckRssLimit && UNLIKELY(isRssLimitExceeded()))
      return FailureHandler::OnOOM();

    // Primary and Secondary backed allocations have a different treatment. We
    // deal with alignment requirements of Primary serviced allocations here,
    // but the Secondary will take care of its own alignment needs.
    void *Ptr;
    u8 ClassId;
    uptr AllocSize;
    if (PrimaryAllocator::CanAllocate(AlignedSize, MinAlignment)) {
      AllocSize = AlignedSize;
      ClassId = SizeClassMap::ClassID(AllocSize);
      ScudoTSD *TSD = getTSDAndLock();
      Ptr = BackendAllocator.allocatePrimary(&TSD->Cache, ClassId);
      TSD->unlock();
    } else {
      AllocSize = NeededSize;
      ClassId = 0;
      Ptr = BackendAllocator.allocateSecondary(AllocSize, Alignment);
    }
    if (UNLIKELY(!Ptr))
      return FailureHandler::OnOOM();

    // If requested, we will zero out the entire contents of the returned chunk.
    if ((ForceZeroContents || ZeroContents) && ClassId)
      memset(Ptr, 0, BackendAllocator.getActuallyAllocatedSize(Ptr, ClassId));

    UnpackedHeader Header = {};
    uptr BackendPtr = reinterpret_cast<uptr>(Ptr);
    uptr UserBeg = BackendPtr + AlignedChunkHeaderSize;
    if (UNLIKELY(!IsAligned(UserBeg, Alignment))) {
      // Since the Secondary takes care of alignment, a non-aligned pointer
      // means it is from the Primary. It is also the only case where the offset
      // field of the header would be non-zero.
      CHECK(ClassId);
      UserBeg = RoundUpTo(UserBeg, Alignment);
      uptr Offset = UserBeg - AlignedChunkHeaderSize - BackendPtr;
      Header.Offset = Offset >> MinAlignmentLog;
    }
    CHECK_LE(UserBeg + Size, BackendPtr + AllocSize);
    Header.State = ChunkAllocated;
    Header.AllocType = Type;
    if (ClassId) {
      Header.ClassId = ClassId;
      Header.SizeOrUnusedBytes = Size;
    } else {
      // The secondary fits the allocations to a page, so the amount of unused
      // bytes is the difference between the end of the user allocation and the
      // next page boundary.
      const uptr PageSize = GetPageSizeCached();
      const uptr TrailingBytes = (UserBeg + Size) & (PageSize - 1);
      if (TrailingBytes)
        Header.SizeOrUnusedBytes = PageSize - TrailingBytes;
    }
    getScudoChunk(UserBeg)->storeHeader(&Header);
    void *UserPtr = reinterpret_cast<void *>(UserBeg);
    // if (&__sanitizer_malloc_hook) __sanitizer_malloc_hook(UserPtr, Size);
    return UserPtr;
  }

  // Place a chunk in the quarantine or directly deallocate it in the event of
  // a zero-sized quarantine, or if the size of the chunk is greater than the
  // quarantine chunk size threshold.
  void quarantineOrDeallocateChunk(ScudoChunk *Chunk, UnpackedHeader *Header,
                                   uptr Size) {
    const bool BypassQuarantine = (AllocatorQuarantine.GetCacheSize() == 0) ||
        (Size > QuarantineChunksUpToSize);
    if (BypassQuarantine) {
      Chunk->eraseHeader();
      void *Ptr = Chunk->getBackendPtr(Header);
      if (Header->ClassId) {
        ScudoTSD *TSD = getTSDAndLock();
        getBackendAllocator().deallocatePrimary(&TSD->Cache, Ptr,
                                                Header->ClassId);
        TSD->unlock();
      } else {
        getBackendAllocator().deallocateSecondary(Ptr);
      }
    } else {
      // If a small memory amount was allocated with a larger alignment, we want
      // to take that into account. Otherwise the Quarantine would be filled
      // with tiny chunks, taking a lot of VA memory. This is an approximation
      // of the usable size, that allows us to not call
      // GetActuallyAllocatedSize.
      uptr EstimatedSize = Size + (Header->Offset << MinAlignmentLog);
      UnpackedHeader NewHeader = *Header;
      NewHeader.State = ChunkQuarantine;
      Chunk->compareExchangeHeader(&NewHeader, Header);
      ScudoTSD *TSD = getTSDAndLock();
      AllocatorQuarantine.Put(getQuarantineCache(TSD),
                              QuarantineCallback(&TSD->Cache),
                              Chunk, EstimatedSize);
      TSD->unlock();
    }
  }

  // Deallocates a Chunk, which means adding it to the delayed free list (or
  // Quarantine).
  void deallocate(void *UserPtr, uptr DeleteSize, AllocType Type) {
    // For a deallocation, we only ensure minimal initialization, meaning thread
    // local data will be left uninitialized for now (when using ELF TLS). The
    // fallback cache will be used instead. This is a workaround for a situation
    // where the only heap operation performed in a thread would be a free past
    // the TLS destructors, ending up in initialized thread specific data never
    // being destroyed properly. Any other heap operation will do a full init.
    initThreadMaybe(/*MinimalInit=*/true);
    // if (&__sanitizer_free_hook) __sanitizer_free_hook(UserPtr);
    if (UNLIKELY(!UserPtr))
      return;
    uptr UserBeg = reinterpret_cast<uptr>(UserPtr);
    if (UNLIKELY(!IsAligned(UserBeg, MinAlignment))) {
      dieWithMessage("ERROR: attempted to deallocate a chunk not properly "
                     "aligned at address %p\n", UserPtr);
    }
    ScudoChunk *Chunk = getScudoChunk(UserBeg);
    UnpackedHeader Header;
    Chunk->loadHeader(&Header);
    if (UNLIKELY(Header.State != ChunkAllocated)) {
      dieWithMessage("ERROR: invalid chunk state when deallocating address "
                     "%p\n", UserPtr);
    }
    if (DeallocationTypeMismatch) {
      // The deallocation type has to match the allocation one.
      if (Header.AllocType != Type) {
        // With the exception of memalign'd Chunks, that can be still be free'd.
        if (Header.AllocType != FromMemalign || Type != FromMalloc) {
          dieWithMessage("ERROR: allocation type mismatch when deallocating "
                         "address %p\n", UserPtr);
        }
      }
    }
    uptr Size = Header.ClassId ? Header.SizeOrUnusedBytes :
        Chunk->getUsableSize(&Header) - Header.SizeOrUnusedBytes;
    if (DeleteSizeMismatch) {
      if (DeleteSize && DeleteSize != Size) {
        dieWithMessage("ERROR: invalid sized delete on chunk at address %p\n",
                       UserPtr);
      }
    }
    quarantineOrDeallocateChunk(Chunk, &Header, Size);
  }

  // Reallocates a chunk. We can save on a new allocation if the new requested
  // size still fits in the chunk.
  void *reallocate(void *OldPtr, uptr NewSize) {
    initThreadMaybe();
    uptr UserBeg = reinterpret_cast<uptr>(OldPtr);
    if (UNLIKELY(!IsAligned(UserBeg, MinAlignment))) {
      dieWithMessage("ERROR: attempted to reallocate a chunk not properly "
                     "aligned at address %p\n", OldPtr);
    }
    ScudoChunk *Chunk = getScudoChunk(UserBeg);
    UnpackedHeader OldHeader;
    Chunk->loadHeader(&OldHeader);
    if (UNLIKELY(OldHeader.State != ChunkAllocated)) {
      dieWithMessage("ERROR: invalid chunk state when reallocating address "
                     "%p\n", OldPtr);
    }
    if (DeallocationTypeMismatch) {
      if (UNLIKELY(OldHeader.AllocType != FromMalloc)) {
        dieWithMessage("ERROR: allocation type mismatch when reallocating "
                       "address %p\n", OldPtr);
      }
    }
    uptr UsableSize = Chunk->getUsableSize(&OldHeader);
    // The new size still fits in the current chunk, and the size difference
    // is reasonable.
    if (NewSize <= UsableSize &&
        (UsableSize - NewSize) < (SizeClassMap::kMaxSize / 2)) {
      UnpackedHeader NewHeader = OldHeader;
      NewHeader.SizeOrUnusedBytes =
          OldHeader.ClassId ? NewSize : UsableSize - NewSize;
      Chunk->compareExchangeHeader(&NewHeader, &OldHeader);
      return OldPtr;
    }
    // Otherwise, we have to allocate a new chunk and copy the contents of the
    // old one.
    void *NewPtr = allocate(NewSize, MinAlignment, FromMalloc);
    if (NewPtr) {
      uptr OldSize = OldHeader.ClassId ? OldHeader.SizeOrUnusedBytes :
          UsableSize - OldHeader.SizeOrUnusedBytes;
      memcpy(NewPtr, OldPtr, Min(NewSize, UsableSize));
      quarantineOrDeallocateChunk(Chunk, &OldHeader, OldSize);
    }
    return NewPtr;
  }

  // Helper function that returns the actual usable size of a chunk.
  uptr getUsableSize(const void *Ptr) {
    initThreadMaybe();
    if (UNLIKELY(!Ptr))
      return 0;
    uptr UserBeg = reinterpret_cast<uptr>(Ptr);
    ScudoChunk *Chunk = getScudoChunk(UserBeg);
    UnpackedHeader Header;
    Chunk->loadHeader(&Header);
    // Getting the usable size of a chunk only makes sense if it's allocated.
    if (UNLIKELY(Header.State != ChunkAllocated)) {
      dieWithMessage("ERROR: invalid chunk state when sizing address %p\n",
                     Ptr);
    }
    return Chunk->getUsableSize(&Header);
  }

  void *calloc(uptr NMemB, uptr Size) {
    initThreadMaybe();
    if (UNLIKELY(CheckForCallocOverflow(NMemB, Size)))
      return FailureHandler::OnBadRequest();
    return allocate(NMemB * Size, MinAlignment, FromMalloc, true);
  }

  void commitBack(ScudoTSD *TSD) {
    AllocatorQuarantine.Drain(getQuarantineCache(TSD),
                              QuarantineCallback(&TSD->Cache));
    BackendAllocator.destroyCache(&TSD->Cache);
  }

  uptr getStats(AllocatorStat StatType) {
    initThreadMaybe();
    uptr stats[AllocatorStatCount];
    BackendAllocator.getStats(stats);
    return stats[StatType];
  }

  void *handleBadRequest() {
    initThreadMaybe();
    return FailureHandler::OnBadRequest();
  }
};

static ScudoAllocator Instance(LINKER_INITIALIZED);

static ScudoBackendAllocator &getBackendAllocator() {
  return Instance.BackendAllocator;
}

void initScudo() {
  Instance.init();
}

void ScudoTSD::init(bool Shared) {
  UnlockRequired = Shared;
  getBackendAllocator().initCache(&Cache);
  memset(QuarantineCachePlaceHolder, 0, sizeof(QuarantineCachePlaceHolder));
}

void ScudoTSD::commitBack() {
  Instance.commitBack(this);
}

void *scudoMalloc(uptr Size, AllocType Type) {
  return SetErrnoOnNull(Instance.allocate(Size, MinAlignment, Type));
}

void scudoFree(void *Ptr, AllocType Type) {
  Instance.deallocate(Ptr, 0, Type);
}

void scudoSizedFree(void *Ptr, uptr Size, AllocType Type) {
  Instance.deallocate(Ptr, Size, Type);
}

void *scudoRealloc(void *Ptr, uptr Size) {
  if (!Ptr)
    return SetErrnoOnNull(Instance.allocate(Size, MinAlignment, FromMalloc));
  if (Size == 0) {
    Instance.deallocate(Ptr, 0, FromMalloc);
    return nullptr;
  }
  return SetErrnoOnNull(Instance.reallocate(Ptr, Size));
}

void *scudoCalloc(uptr NMemB, uptr Size) {
  return SetErrnoOnNull(Instance.calloc(NMemB, Size));
}

void *scudoValloc(uptr Size) {
  return SetErrnoOnNull(
      Instance.allocate(Size, GetPageSizeCached(), FromMemalign));
}

void *scudoPvalloc(uptr Size) {
  uptr PageSize = GetPageSizeCached();
  if (UNLIKELY(CheckForPvallocOverflow(Size, PageSize))) {
    errno = ENOMEM;
    return Instance.handleBadRequest();
  }
  // pvalloc(0) should allocate one page.
  Size = Size ? RoundUpTo(Size, PageSize) : PageSize;
  return SetErrnoOnNull(Instance.allocate(Size, PageSize, FromMemalign));
}

void *scudoMemalign(uptr Alignment, uptr Size) {
  if (UNLIKELY(!IsPowerOfTwo(Alignment))) {
    errno = EINVAL;
    return Instance.handleBadRequest();
  }
  return SetErrnoOnNull(Instance.allocate(Size, Alignment, FromMemalign));
}

int scudoPosixMemalign(void **MemPtr, uptr Alignment, uptr Size) {
  if (UNLIKELY(!CheckPosixMemalignAlignment(Alignment))) {
    Instance.handleBadRequest();
    return EINVAL;
  }
  void *Ptr = Instance.allocate(Size, Alignment, FromMemalign);
  if (UNLIKELY(!Ptr))
    return ENOMEM;
  *MemPtr = Ptr;
  return 0;
}

void *scudoAlignedAlloc(uptr Alignment, uptr Size) {
  if (UNLIKELY(!CheckAlignedAllocAlignmentAndSize(Alignment, Size))) {
    errno = EINVAL;
    return Instance.handleBadRequest();
  }
  return SetErrnoOnNull(Instance.allocate(Size, Alignment, FromMalloc));
}

uptr scudoMallocUsableSize(void *Ptr) {
  return Instance.getUsableSize(Ptr);
}

}  // namespace __scudo

using namespace __scudo;

// MallocExtension helper functions

uptr __sanitizer_get_current_allocated_bytes() {
  return Instance.getStats(AllocatorStatAllocated);
}

uptr __sanitizer_get_heap_size() {
  return Instance.getStats(AllocatorStatMapped);
}

uptr __sanitizer_get_free_bytes() {
  return 1;
}

uptr __sanitizer_get_unmapped_bytes() {
  return 1;
}

uptr __sanitizer_get_estimated_allocated_size(uptr size) {
  return size;
}

int __sanitizer_get_ownership(const void *Ptr) {
  return Instance.isValidPointer(Ptr);
}

uptr __sanitizer_get_allocated_size(const void *Ptr) {
  return Instance.getUsableSize(Ptr);
}
