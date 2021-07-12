//===-- sanitizer_allocator_primary32.h -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Part of the Sanitizer Allocator.
//
//===----------------------------------------------------------------------===//
#ifndef SANITIZER_ALLOCATOR_H
#error This file must be included inside sanitizer_allocator.h
#endif

template<class SizeClassAllocator> struct SizeClassAllocator32LocalCache;

// SizeClassAllocator32 -- allocator for 32-bit address space.
// This allocator can theoretically be used on 64-bit arch, but there it is less
// efficient than SizeClassAllocator64.
//
// [kSpaceBeg, kSpaceBeg + kSpaceSize) is the range of addresses which can
// be returned by MmapOrDie().
//
// Region:
//   a result of a single call to MmapAlignedOrDieOnFatalError(kRegionSize,
//                                                             kRegionSize).
// Since the regions are aligned by kRegionSize, there are exactly
// kNumPossibleRegions possible regions in the address space and so we keep
// a ByteMap possible_regions to store the size classes of each Region.
// 0 size class means the region is not used by the allocator.
//
// One Region is used to allocate chunks of a single size class.
// A Region looks like this:
// UserChunk1 .. UserChunkN <gap> MetaChunkN .. MetaChunk1
//
// In order to avoid false sharing the objects of this class should be
// chache-line aligned.

struct SizeClassAllocator32FlagMasks {  //  Bit masks.
  enum {
    kRandomShuffleChunks = 1,
    kUseSeparateSizeClassForBatch = 2,
  };
};

template <class Params>
class SizeClassAllocator32 {
 private:
  static const u64 kTwoLevelByteMapSize1 =
      (Params::kSpaceSize >> Params::kRegionSizeLog) >> 12;
  static const u64 kMinFirstMapSizeTwoLevelByteMap = 4;

 public:
  using AddressSpaceView = typename Params::AddressSpaceView;
  static const vaddr kSpaceBeg = Params::kSpaceBeg;
  static const u64 kSpaceSize = Params::kSpaceSize;
  static const usize kMetadataSize = Params::kMetadataSize;
  typedef typename Params::SizeClassMap SizeClassMap;
  static const usize kRegionSizeLog = Params::kRegionSizeLog;
  typedef typename Params::MapUnmapCallback MapUnmapCallback;
  using ByteMap = typename conditional<
      (kTwoLevelByteMapSize1 < kMinFirstMapSizeTwoLevelByteMap),
      FlatByteMap<(Params::kSpaceSize >> Params::kRegionSizeLog),
                  AddressSpaceView>,
      TwoLevelByteMap<kTwoLevelByteMapSize1, 1 << 12, AddressSpaceView>>::type;

  COMPILER_CHECK(!SANITIZER_SIGN_EXTENDED_ADDRESSES ||
                 (kSpaceSize & (kSpaceSize - 1)) == 0);

  static const bool kRandomShuffleChunks = Params::kFlags &
      SizeClassAllocator32FlagMasks::kRandomShuffleChunks;
  static const bool kUseSeparateSizeClassForBatch = Params::kFlags &
      SizeClassAllocator32FlagMasks::kUseSeparateSizeClassForBatch;

  struct TransferBatch {
    static const usize kMaxNumCached = SizeClassMap::kMaxNumCachedHint - 2;
    void SetFromArray(void *batch[], usize count) {
      DCHECK_LE(count, kMaxNumCached);
      count_ = count;
      for (usize i = 0; i < count; i++)
        batch_[i] = batch[i];
    }
    usize Count() const { return count_; }
    void Clear() { count_ = 0; }
    void Add(void *ptr) {
      batch_[count_++] = ptr;
      DCHECK_LE(count_, kMaxNumCached);
    }
    void CopyToArray(void *to_batch[]) const {
      for (usize i = 0, n = Count(); i < n; i++)
        to_batch[i] = batch_[i];
    }

    // How much memory do we need for a batch containing n elements.
    static usize AllocationSizeRequiredForNElements(usize n) {
      return sizeof(usize) * 2 + sizeof(void *) * n;
    }
    static usize MaxCached(usize size) {
      return Min(kMaxNumCached, SizeClassMap::MaxCachedHint(size));
    }

    TransferBatch *next;

   private:
    usize count_;
    void *batch_[kMaxNumCached];
  };

  static const usize kBatchSize = sizeof(TransferBatch);
  COMPILER_CHECK((kBatchSize & (kBatchSize - 1)) == 0);
  COMPILER_CHECK(kBatchSize == SizeClassMap::kMaxNumCachedHint * sizeof(uptr));

  static usize ClassIdToSize(usize class_id) {
    return (class_id == SizeClassMap::kBatchClassID) ?
        kBatchSize : SizeClassMap::Size(class_id);
  }

  typedef SizeClassAllocator32<Params> ThisT;
  typedef SizeClassAllocator32LocalCache<ThisT> AllocatorCache;

  void Init(s32 release_to_os_interval_ms, uptr heap_start = 0) {
    CHECK(!heap_start);
    possible_regions.Init();
    internal_memset(size_class_info_array, 0, sizeof(size_class_info_array));
  }

  s32 ReleaseToOSIntervalMs() const {
    return kReleaseToOSIntervalNever;
  }

  void SetReleaseToOSIntervalMs(s32 release_to_os_interval_ms) {
    // This is empty here. Currently only implemented in 64-bit allocator.
  }

  void ForceReleaseToOS() {
    // Currently implemented in 64-bit allocator only.
  }

  void *MapWithCallback(usize size) {
    void *res = MmapOrDie(size, PrimaryAllocatorName);
    MapUnmapCallback().OnMap((uptr)res, size);
    return res;
  }

  void UnmapWithCallback(uptr beg, usize size) {
    MapUnmapCallback().OnUnmap(beg, size);
    UnmapOrDie(reinterpret_cast<void *>(beg), size);
  }

  static bool CanAllocate(usize size, usize alignment) {
    return size <= SizeClassMap::kMaxSize &&
      alignment <= SizeClassMap::kMaxSize;
  }

  void *GetMetaData(const void *p) {
    CHECK(kMetadataSize);
    CHECK(PointerIsMine(p));
    uptr mem = reinterpret_cast<uptr>(p);
    uptr beg = ComputeRegionBeg(mem);
    usize size = ClassIdToSize(GetSizeClass(p));
    u32 offset = mem - beg;
    usize n = offset / (u32)size;  // 32-bit division
    uptr meta = (beg + kRegionSize) - (n + 1) * kMetadataSize;
    return reinterpret_cast<void*>(meta);
  }

  NOINLINE TransferBatch *AllocateBatch(AllocatorStats *stat, AllocatorCache *c,
                                        usize class_id) {
    DCHECK_LT(class_id, kNumClasses);
    SizeClassInfo *sci = GetSizeClassInfo(class_id);
    SpinMutexLock l(&sci->mutex);
    if (sci->free_list.empty()) {
      if (UNLIKELY(!PopulateFreeList(stat, c, sci, class_id)))
        return nullptr;
      DCHECK(!sci->free_list.empty());
    }
    TransferBatch *b = sci->free_list.front();
    sci->free_list.pop_front();
    return b;
  }

  NOINLINE void DeallocateBatch(AllocatorStats *stat, usize class_id,
                                TransferBatch *b) {
    DCHECK_LT(class_id, kNumClasses);
    CHECK_GT(b->Count(), 0);
    SizeClassInfo *sci = GetSizeClassInfo(class_id);
    SpinMutexLock l(&sci->mutex);
    sci->free_list.push_front(b);
  }

  bool PointerIsMine(const void *p) {
    vaddr mem = reinterpret_cast<vaddr>(p);
    if (SANITIZER_SIGN_EXTENDED_ADDRESSES)
      mem &= (kSpaceSize - 1);
    if (mem < kSpaceBeg || mem >= kSpaceBeg + kSpaceSize)
      return false;
    return GetSizeClass(p) != 0;
  }

  usize GetSizeClass(const void *p) {
    return possible_regions[ComputeRegionId(reinterpret_cast<uptr>(p))];
  }

  void *GetBlockBegin(const void *p) {
    CHECK(PointerIsMine(p));
    uptr mem = reinterpret_cast<uptr>(p);
    uptr beg = ComputeRegionBeg(mem);
    usize size = ClassIdToSize(GetSizeClass(p));
    u32 offset = mem - beg;
    u32 n = offset / (u32)size;  // 32-bit division
    uptr res = beg + (n * (u32)size);
    return reinterpret_cast<void*>(res);
  }

  usize GetActuallyAllocatedSize(void *p) {
    CHECK(PointerIsMine(p));
    return ClassIdToSize(GetSizeClass(p));
  }

  static usize ClassID(usize size) { return SizeClassMap::ClassID(size); }

  usize TotalMemoryUsed() {
    // No need to lock here.
    usize res = 0;
    for (usize i = 0; i < kNumPossibleRegions; i++)
      if (possible_regions[i])
        res += kRegionSize;
    return res;
  }

  void TestOnlyUnmap() {
    for (usize i = 0; i < kNumPossibleRegions; i++)
      if (possible_regions[i])
        UnmapWithCallback((i * kRegionSize), kRegionSize);
  }

  // ForceLock() and ForceUnlock() are needed to implement Darwin malloc zone
  // introspection API.
  void ForceLock() NO_THREAD_SAFETY_ANALYSIS {
    for (usize i = 0; i < kNumClasses; i++) {
      GetSizeClassInfo(i)->mutex.Lock();
    }
  }

  void ForceUnlock() NO_THREAD_SAFETY_ANALYSIS {
    for (int i = kNumClasses - 1; i >= 0; i--) {
      GetSizeClassInfo(i)->mutex.Unlock();
    }
  }

  // Iterate over all existing chunks.
  // The allocator must be locked when calling this function.
  void ForEachChunk(ForEachChunkCallback callback, void *arg) {
    for (vaddr region = 0; region < kNumPossibleRegions; region++)
      if (possible_regions[region]) {
        usize chunk_size = ClassIdToSize(possible_regions[region]);
        usize max_chunks_in_region = kRegionSize / (chunk_size + kMetadataSize);
        vaddr region_beg = region * kRegionSize;
        for (vaddr chunk = region_beg;
             chunk < region_beg + max_chunks_in_region * chunk_size;
             chunk += chunk_size) {
          // Too slow: CHECK_EQ((void *)chunk, GetBlockBegin((void *)chunk));
          callback(chunk, arg);
        }
      }
  }

  void PrintStats() {}

  static usize AdditionalSize() { return 0; }

  typedef SizeClassMap SizeClassMapT;
  static const usize kNumClasses = SizeClassMap::kNumClasses;

 private:
  static const usize kRegionSize = 1 << kRegionSizeLog;
  static const usize kNumPossibleRegions = kSpaceSize / kRegionSize;

  struct ALIGNED(SANITIZER_CACHE_LINE_SIZE) SizeClassInfo {
    StaticSpinMutex mutex;
    IntrusiveList<TransferBatch> free_list;
    u32 rand_state;
  };
  COMPILER_CHECK(sizeof(SizeClassInfo) % kCacheLineSize == 0);

  uptr ComputeRegionId(vaddr mem) const {
    if (SANITIZER_SIGN_EXTENDED_ADDRESSES)
      mem &= (kSpaceSize - 1);
    const usize res = (usize)mem >> kRegionSizeLog;
    CHECK_LT(res, kNumPossibleRegions);
    return res;
  }

  uptr ComputeRegionBeg(uptr mem) {
    return RoundDownTo(mem, kRegionSize);
  }

  uptr AllocateRegion(AllocatorStats *stat, usize class_id) {
    DCHECK_LT(class_id, kNumClasses);
    const uptr res = reinterpret_cast<uptr>(MmapAlignedOrDieOnFatalError(
        kRegionSize, kRegionSize, PrimaryAllocatorName));
    if (UNLIKELY(!res))
      return 0;
    MapUnmapCallback().OnMap(res, kRegionSize);
    stat->Add(AllocatorStatMapped, kRegionSize);
    CHECK(IsAligned(res, kRegionSize));
    possible_regions.set(ComputeRegionId(res), static_cast<u8>(class_id));
    return res;
  }

  SizeClassInfo *GetSizeClassInfo(usize class_id) {
    DCHECK_LT(class_id, kNumClasses);
    return &size_class_info_array[class_id];
  }

  bool PopulateBatches(AllocatorCache *c, SizeClassInfo *sci, usize class_id,
                       TransferBatch **current_batch, usize max_count,
                       uptr *pointers_array, usize count) {
    // If using a separate class for batches, we do not need to shuffle it.
    if (kRandomShuffleChunks && (!kUseSeparateSizeClassForBatch ||
        class_id != SizeClassMap::kBatchClassID))
      RandomShuffle(pointers_array, count, &sci->rand_state);
    TransferBatch *b = *current_batch;
    for (usize i = 0; i < count; i++) {
      if (!b) {
        b = c->CreateBatch(class_id, this, (TransferBatch*)pointers_array[i]);
        if (UNLIKELY(!b))
          return false;
        b->Clear();
      }
      b->Add((void*)pointers_array[i]);
      if (b->Count() == max_count) {
        sci->free_list.push_back(b);
        b = nullptr;
      }
    }
    *current_batch = b;
    return true;
  }

  bool PopulateFreeList(AllocatorStats *stat, AllocatorCache *c,
                        SizeClassInfo *sci, usize class_id) {
    const uptr region = AllocateRegion(stat, class_id);
    if (UNLIKELY(!region))
      return false;
    if (kRandomShuffleChunks)
      if (UNLIKELY(sci->rand_state == 0))
        // The random state is initialized from ASLR (PIE) and time.
        sci->rand_state = reinterpret_cast<vaddr>(sci) ^ NanoTime();
    const usize size = ClassIdToSize(class_id);
    const usize n_chunks = kRegionSize / (size + kMetadataSize);
    const usize max_count = TransferBatch::MaxCached(size);
    DCHECK_GT(max_count, 0);
    TransferBatch *b = nullptr;
    constexpr usize kShuffleArraySize = 48;
    uptr shuffle_array[kShuffleArraySize];
    usize count = 0;
    for (uptr i = region; i < region + n_chunks * size; i += size) {
      shuffle_array[count++] = i;
      if (count == kShuffleArraySize) {
        if (UNLIKELY(!PopulateBatches(c, sci, class_id, &b, max_count,
                                      shuffle_array, count)))
          return false;
        count = 0;
      }
    }
    if (count) {
      if (UNLIKELY(!PopulateBatches(c, sci, class_id, &b, max_count,
                                    shuffle_array, count)))
        return false;
    }
    if (b) {
      CHECK_GT(b->Count(), 0);
      sci->free_list.push_back(b);
    }
    return true;
  }

  ByteMap possible_regions;
  SizeClassInfo size_class_info_array[kNumClasses];
};
