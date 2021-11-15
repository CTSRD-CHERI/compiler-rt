//===-- sanitizer_stack_store.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A fast memory allocator that does not support free() nor realloc().
// All allocations are forever.
//===----------------------------------------------------------------------===//

#ifndef SANITIZER_STACK_STORE_H
#define SANITIZER_STACK_STORE_H

#include "sanitizer_atomic.h"
#include "sanitizer_common.h"
#include "sanitizer_internal_defs.h"
#include "sanitizer_mutex.h"

namespace __sanitizer {

class StackStore {
 public:
  uptr *alloc(usize count = 1);
  usize allocated() const { return atomic_load_relaxed(&mapped_size); }

  void TestOnlyUnmap();

 private:
  uptr *tryAlloc(usize count);
  uptr *refillAndAlloc(usize count);
  mutable StaticSpinMutex mtx;  // Protects alloc of new blocks.
  atomic_uintptr_t region_pos;  // Region allocator for Node's.
  atomic_uintptr_t region_end;
  atomic_size_t mapped_size;

  struct BlockInfo {
    const BlockInfo *next;
    uptr ptr;
    usize size;
  };
  const BlockInfo *curr;
};

inline uptr *StackStore::tryAlloc(usize count) {
  // Optimisic lock-free allocation, essentially try to bump the region ptr.
  for (;;) {
    uptr cmp = atomic_load(&region_pos, memory_order_acquire);
    uptr end = atomic_load(&region_end, memory_order_acquire);
    uptr size = count * sizeof(uptr);
    if (cmp == 0 || cmp + size > end)
      return nullptr;
    if (atomic_compare_exchange_weak(&region_pos, &cmp, cmp + size,
                                     memory_order_acquire))
      return reinterpret_cast<uptr *>(cmp);
  }
}

inline uptr *StackStore::alloc(usize count) {
  // First, try to allocate optimisitically.
  uptr *s = tryAlloc(count);
  if (LIKELY(s))
    return s;
  return refillAndAlloc(count);
}

inline uptr *StackStore::refillAndAlloc(uptr count) {
  // If failed, lock, retry and alloc new superblock.
  SpinMutexLock l(&mtx);
  for (;;) {
    uptr *s = tryAlloc(count);
    if (s)
      return s;
    atomic_store(&region_pos, 0, memory_order_relaxed);
    uptr size = count * sizeof(uptr) + sizeof(BlockInfo);
    uptr allocsz = RoundUpTo(Max<uptr>(size, 64u * 1024u), GetPageSizeCached());
    uptr mem = (uptr)MmapOrDie(allocsz, "stack depot");
    BlockInfo *new_block = (BlockInfo *)(mem + allocsz) - 1;
    new_block->next = curr;
    new_block->ptr = mem;
    new_block->size = allocsz;
    curr = new_block;

    atomic_fetch_add(&mapped_size, allocsz, memory_order_relaxed);

    allocsz -= sizeof(BlockInfo);
    atomic_store(&region_end, mem + allocsz, memory_order_release);
    atomic_store(&region_pos, mem, memory_order_release);
  }
}

inline void StackStore::TestOnlyUnmap() {
  while (curr) {
    uptr mem = curr->ptr;
    uptr allocsz = curr->size;
    curr = curr->next;
    UnmapOrDie((void *)mem, allocsz);
  }
  internal_memset(this, 0, sizeof(*this));
}

}  // namespace __sanitizer

#endif  // SANITIZER_STACK_STORE_H
