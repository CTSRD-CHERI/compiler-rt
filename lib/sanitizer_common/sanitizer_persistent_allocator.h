//===-- sanitizer_persistent_allocator.h ------------------------*- C++ -*-===//
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

#ifndef SANITIZER_PERSISTENT_ALLOCATOR_H
#define SANITIZER_PERSISTENT_ALLOCATOR_H

#include "sanitizer_internal_defs.h"
#include "sanitizer_mutex.h"
#include "sanitizer_atomic.h"
#include "sanitizer_common.h"

namespace __sanitizer {

class PersistentAllocator {
 public:
  void *alloc(usize size);

 private:
  void *tryAlloc(usize size);
  void *refillAndAlloc(usize size);
  StaticSpinMutex mtx;  // Protects alloc of new blocks for region allocator.
  atomic_uintptr_t region_pos;  // Region allocator for Node's.
  atomic_uintptr_t region_end;
};

inline void *PersistentAllocator::tryAlloc(usize size) {
  // Optimisic lock-free allocation, essentially try to bump the region ptr.
  for (;;) {
    uptr cmp = atomic_load(&region_pos, memory_order_acquire);
    uptr end = atomic_load(&region_end, memory_order_acquire);
    if (cmp == 0 || cmp + size > end) return nullptr;
    if (atomic_compare_exchange_weak(&region_pos, &cmp, cmp + size,
                                     memory_order_acquire))
      return (void *)cmp;
  }
}

inline void *PersistentAllocator::alloc(usize size) {
  // First, try to allocate optimisitically.
  void *s = tryAlloc(size);
  if (s) return s;
  return refillAndAlloc(size);
}

extern PersistentAllocator thePersistentAllocator;
inline void *PersistentAlloc(usize sz) {
  return thePersistentAllocator.alloc(sz);
}

} // namespace __sanitizer

#endif // SANITIZER_PERSISTENT_ALLOCATOR_H
