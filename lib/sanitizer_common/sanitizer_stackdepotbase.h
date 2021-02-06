//===-- sanitizer_stackdepotbase.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implementation of a mapping from arbitrary values to unique 32-bit
// identifiers.
//===----------------------------------------------------------------------===//

#ifndef SANITIZER_STACKDEPOTBASE_H
#define SANITIZER_STACKDEPOTBASE_H

#include <stdio.h>

#include "sanitizer_atomic.h"
#include "sanitizer_internal_defs.h"
#include "sanitizer_mutex.h"
#include "sanitizer_persistent_allocator.h"

#ifdef __CHERI_PURE_CAPABILITY__
#include <cheri.h>
#endif

namespace __sanitizer {

template <usize mask>
inline usize GetLowPtrBits(uptr ptr) {
#ifdef __CHERI_PURE_CAPABILITY__
  return cheri_low_bits_get(ptr, mask);
#else
  return ptr & mask;
#endif
}
inline uptr SetLowPtrBits(uptr ptr, usize bits) { return ptr | bits; }

template <usize mask>
inline uptr ClearLowPtrBits(uptr ptr) {
#ifdef __CHERI_PURE_CAPABILITY__
  return cheri_low_bits_clear(ptr, mask);
#else
  return ptr & ~mask;
#endif
}

template <class Node, int kReservedBits, int kTabSizeLog>
class StackDepotBase {
 public:
  typedef typename Node::args_type args_type;
  typedef typename Node::handle_type handle_type;
  // Maps stack trace to an unique id.
  handle_type Put(args_type args, bool *inserted = nullptr);
  // Retrieves a stored stack trace by the id.
  args_type Get(u32 id);

  StackDepotStats *GetStats() { return &stats; }

  void LockAll();
  void UnlockAll();
  void PrintAll();
  void Free();

 private:
  static Node *find(Node *s, args_type args, u32 hash);
  static Node *lock(atomic_uintptr_t *p);
  static void unlock(atomic_uintptr_t *p, Node *s);

  Node *alloc(uptr part, uptr memsz);

  static const int kTabSize = 1 << kTabSizeLog;  // Hash table size.
  static const int kPartBits = 8;
  static const int kPartShift = sizeof(u32) * 8 - kPartBits - kReservedBits;
  static const int kPartCount =
      1 << kPartBits;  // Number of subparts in the table.
  static const int kPartSize = kTabSize / kPartCount;
  static const int kMaxId = 1 << kPartShift;

  atomic_uintptr_t tab[kTabSize];   // Hash table of Node's.
  atomic_uint32_t seq[kPartCount];  // Unique id generators.
  atomic_uintptr_t freeNodes[kPartCount];

  StackDepotStats stats;

  friend class StackDepotReverseMap;
};

template <class Node, int kReservedBits, int kTabSizeLog>
Node *StackDepotBase<Node, kReservedBits, kTabSizeLog>::find(Node *s,
                                                             args_type args,
                                                             u32 hash) {
  // Searches linked list s for the stack, returns its id.
  for (; s; s = s->link) {
    if (s->eq(hash, args)) {
      return s;
    }
  }
  return nullptr;
}

template <class Node, int kReservedBits, int kTabSizeLog>
Node *StackDepotBase<Node, kReservedBits, kTabSizeLog>::lock(
    atomic_uintptr_t *p) {
  // Uses the pointer lsb as mutex.
  for (int i = 0;; i++) {
    uptr cmp = atomic_load(p, memory_order_relaxed);
    if (GetLowPtrBits<1>(cmp) == 0 &&
        atomic_compare_exchange_weak(p, &cmp, cmp | 1, memory_order_acquire))
      return (Node *)cmp;
    if (i < 10)
      proc_yield(10);
    else
      internal_sched_yield();
  }
}

template <class Node, int kReservedBits, int kTabSizeLog>
void StackDepotBase<Node, kReservedBits, kTabSizeLog>::unlock(
    atomic_uintptr_t *p, Node *s) {
  DCHECK_EQ(GetLowPtrBits<1>((uptr)s), 0);
  atomic_store(p, (uptr)s, memory_order_release);
}

template <class Node, int kReservedBits, int kTabSizeLog>
void StackDepotBase<Node, kReservedBits, kTabSizeLog>::Free() {
  LockAll();
  for (int i = 0; i < kPartCount; ++i) {
    lock(&freeNodes[i]);
  }

  for (int i = 0; i < kTabSize; ++i) {
    atomic_uintptr_t *p_tab = &tab[i];
    Node *s = (Node *)(atomic_load(p_tab, memory_order_relaxed) & ~1UL);
    while (s) {
      uptr part = s->id >> kPartShift;
      atomic_uintptr_t *p_free_nodes = &freeNodes[part];
      Node *free_nodes_head =
          (Node *)(atomic_load(p_free_nodes, memory_order_relaxed) & ~1UL);
      Node *next = s->link;
      s->link = free_nodes_head;
      atomic_store(p_free_nodes, (uptr)s, memory_order_release);
      s = next;
    }
    atomic_store(p_tab, (uptr)nullptr, memory_order_release);
  }

  stats.n_uniq_ids = 0;

  for (int i = 0; i < kPartCount; ++i)
    (void)atomic_exchange(&seq[i], 0, memory_order_relaxed);

  for (int i = kPartCount - 1; i >= 0; --i) {
    atomic_uintptr_t *p = &freeNodes[i];
    uptr s = atomic_load(p, memory_order_relaxed);
    unlock(p, (Node *)(s & ~1UL));
  }
  UnlockAll();
}

template <class Node, int kReservedBits, int kTabSizeLog>
Node *StackDepotBase<Node, kReservedBits, kTabSizeLog>::alloc(uptr part,
                                                              uptr memsz) {
  atomic_uintptr_t *p = &freeNodes[part];
  Node *head = lock(p);
  if (head) {
    unlock(p, head->link);
    return head;
  }
  unlock(p, head);
  Node *s = (Node *)PersistentAlloc(memsz);
  stats.allocated += memsz;
  return s;
}

template <class Node, int kReservedBits, int kTabSizeLog>
typename StackDepotBase<Node, kReservedBits, kTabSizeLog>::handle_type
StackDepotBase<Node, kReservedBits, kTabSizeLog>::Put(args_type args,
                                                      bool *inserted) {
  if (inserted) *inserted = false;
  if (!Node::is_valid(args)) return handle_type();
  uptr h = Node::hash(args);
  atomic_uintptr_t *p = &tab[h % kTabSize];
  uptr v = atomic_load(p, memory_order_consume);
  Node *s = (Node *)ClearLowPtrBits<1>(v);
  // First, try to find the existing stack.
  Node *node = find(s, args, h);
  if (node) return node->get_handle();
  // If failed, lock, retry and insert new.
  Node *s2 = lock(p);
  if (s2 != s) {
    node = find(s2, args, h);
    if (node) {
      unlock(p, s2);
      return node->get_handle();
    }
  }
  usize part = (h % kTabSize) / kPartSize;
  u32 id = atomic_fetch_add(&seq[part], 1, memory_order_relaxed) + 1;
  stats.n_uniq_ids++;
  CHECK_LT(id, kMaxId);
  id |= part << kPartShift;
  CHECK_NE(id, 0);
  CHECK_EQ(id & (((u32)-1) >> kReservedBits), id);
  usize memsz = Node::storage_size(args);
  s = alloc(part, memsz);
  s->id = id;
  s->store(args, h);
  s->link = s2;
  unlock(p, s);
  if (inserted) *inserted = true;
  return s->get_handle();
}

template <class Node, int kReservedBits, int kTabSizeLog>
typename StackDepotBase<Node, kReservedBits, kTabSizeLog>::args_type
StackDepotBase<Node, kReservedBits, kTabSizeLog>::Get(u32 id) {
  if (id == 0) {
    return args_type();
  }
  CHECK_EQ(id & (((u32)-1) >> kReservedBits), id);
  // High kPartBits contain part id, so we need to scan at most kPartSize lists.
  uptr part = id >> kPartShift;
  for (int i = 0; i != kPartSize; i++) {
    uptr idx = part * kPartSize + i;
    CHECK_LT(idx, kTabSize);
    atomic_uintptr_t *p = &tab[idx];
    uptr v = atomic_load(p, memory_order_consume);
    Node *s = (Node *)ClearLowPtrBits<1>(v);
    for (; s; s = s->link) {
      if (s->id == id) {
        return s->load();
      }
    }
  }
  return args_type();
}

template <class Node, int kReservedBits, int kTabSizeLog>
void StackDepotBase<Node, kReservedBits, kTabSizeLog>::LockAll() {
  for (int i = 0; i < kTabSize; ++i) {
    lock(&tab[i]);
  }
}

template <class Node, int kReservedBits, int kTabSizeLog>
void StackDepotBase<Node, kReservedBits, kTabSizeLog>::UnlockAll() {
  for (int i = kTabSize - 1; i >= 0; --i) {
    atomic_uintptr_t *p = &tab[i];
    uptr s = atomic_load(p, memory_order_relaxed);
    unlock(p, (Node *)ClearLowPtrBits<1>(s));
  }
}

template <class Node, int kReservedBits, int kTabSizeLog>
void StackDepotBase<Node, kReservedBits, kTabSizeLog>::PrintAll() {
  for (int i = 0; i < kTabSize; ++i) {
    atomic_uintptr_t *p = &tab[i];
    lock(p);
    uptr v = atomic_load(p, memory_order_relaxed);
    Node *s = (Node *)(v & ~1UL);
    for (; s; s = s->link) {
      Printf("Stack for id %u:\n", s->id);
      s->load().Print();
    }
    unlock(p, s);
  }
}

} // namespace __sanitizer

#endif // SANITIZER_STACKDEPOTBASE_H
