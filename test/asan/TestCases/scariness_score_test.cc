// Test how we produce the scariness score.

// RUN: %clangxx_asan -O0 %s -o %t
// RUN: export %env_asan_opts=detect_stack_use_after_return=1:handle_abort=1:print_scariness=1
// RUN: not %run %t  1 2>&1 | FileCheck %s --check-prefix=CHECK1
// RUN: not %run %t  2 2>&1 | FileCheck %s --check-prefix=CHECK2
// RUN: not %run %t  3 2>&1 | FileCheck %s --check-prefix=CHECK3
// RUN: not %run %t  4 2>&1 | FileCheck %s --check-prefix=CHECK4
// RUN: not %run %t  5 2>&1 | FileCheck %s --check-prefix=CHECK5
// RUN: not %run %t  6 2>&1 | FileCheck %s --check-prefix=CHECK6
// RUN: not %run %t  7 2>&1 | FileCheck %s --check-prefix=CHECK7
// RUN: not %run %t  8 2>&1 | FileCheck %s --check-prefix=CHECK8
// RUN: not %run %t  9 2>&1 | FileCheck %s --check-prefix=CHECK9
// RUN: not %run %t 10 2>&1 | FileCheck %s --check-prefix=CHECK10
// RUN: not %run %t 11 2>&1 | FileCheck %s --check-prefix=CHECK11
// RUN: not %run %t 12 2>&1 | FileCheck %s --check-prefix=CHECK12
// RUN: not %run %t 13 2>&1 | FileCheck %s --check-prefix=CHECK13
// RUN: not %run %t 14 2>&1 | FileCheck %s --check-prefix=CHECK14
// RUN: not %run %t 15 2>&1 | FileCheck %s --check-prefix=CHECK15
// RUN: not %run %t 16 2>&1 | FileCheck %s --check-prefix=CHECK16
// RUN: not %run %t 17 2>&1 | FileCheck %s --check-prefix=CHECK17
// RUN: not %run %t 18 2>&1 | FileCheck %s --check-prefix=CHECK18
// RUN: not %run %t 19 2>&1 | FileCheck %s --check-prefix=CHECK19
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum ReadOrWrite { Read = 0, Write = 1 };

struct S32 {
  char x[32];
};

template<class T>
void HeapBuferOverflow(int Idx, ReadOrWrite w) {
  T *t = new T[100];
  static T sink;
  if (w)
    t[100 + Idx] = T();
  else
    sink = t[100 + Idx];
  delete [] t;
}

template<class T>
void HeapUseAfterFree(int Idx, ReadOrWrite w) {
  T *t = new T[100];
  static T sink;
  sink = t[0];
  delete [] t;
  if (w)
    t[Idx] = T();
  else
    sink = t[Idx];
}

template<class T>
void StackBufferOverflow(int Idx, ReadOrWrite w) {
  T t[100];
  static T sink;
  sink = t[Idx];
  if (w)
    t[100 + Idx] = T();
  else
    sink = t[100 + Idx];
}

template<class T>
T *LeakStack() {
  T t[100];
  static volatile T *x;
  x = &t[0];
  return (T*)x;
}

template<class T>
void StackUseAfterReturn(int Idx, ReadOrWrite w) {
  static T sink;
  T *t = LeakStack<T>();
  if (w)
    t[100 + Idx] = T();
  else
    sink = t[100 + Idx];
}

char    g1[100];
short   g2[100];
int     g4[100];
int64_t g8[100];
S32     gm[100];

void DoubleFree() {
  int *x = new int;
  static volatile int two = 2;
  for (int i = 0; i < two; i++)
    delete x;
}

void StackOverflow(int Idx) {
  int some_stack[10000];
  static volatile int *x;
  x = &some_stack[0];
  if (Idx > 0)
    StackOverflow(Idx - 1);
}

int main(int argc, char **argv) {
  char arr[100];
  static volatile int zero = 0;
  static volatile int *zero_ptr = 0;
  static volatile int *wild_addr = (int*)0x10000000; // System-dependent.
  if (argc != 2) return 1;
  int kind = atoi(argv[1]);
  switch (kind) {
    case 1: HeapBuferOverflow<char>(0, Read); break;
    case 2: HeapBuferOverflow<int>(0, Read); break;
    case 3: HeapBuferOverflow<short>(0, Write); break;
    case 4: HeapBuferOverflow<int64_t>(2, Write); break;
    case 5: HeapBuferOverflow<S32>(4, Write); break;
    case 6: HeapUseAfterFree<char>(0, Read); break;
    case 7: HeapUseAfterFree<int>(0, Write); break;
    case 8: HeapUseAfterFree<int64_t>(0, Read); break;
    case 9: HeapUseAfterFree<S32>(0, Write); break;
    case 10: StackBufferOverflow<char>(0, Write); break;
    case 11: StackBufferOverflow<int64_t>(0, Read); break;
    case 12: StackBufferOverflow<int>(4, Write); break;
    case 13: StackUseAfterReturn<char>(0, Read); break;
    case 14: StackUseAfterReturn<S32>(0, Write); break;
    case 15: g1[zero + 100] = 0; break;
    case 16: gm[0] = gm[zero + 100 + 1]; break;
    case 17: DoubleFree(); break;
    case 18: StackOverflow(1000000); break;
    case 19: *zero_ptr = 0; break;
    case 20: *wild_addr = 0; break;
    case 21: zero = *wild_addr; break;
    case 22: abort(); break;
    case 23: ((void (*)(void))wild_addr)(); break;
    case 24: delete (new int[10]); break;
    case 25: free((char*)malloc(100) + 10); break;
    case 26: memcpy(arr, arr+10, 20);  break;
    // CHECK1: SCARINESS: 12 (1-byte-read-heap-buffer-overflow)
    // CHECK2: SCARINESS: 17 (4-byte-read-heap-buffer-overflow)
    // CHECK3: SCARINESS: 33 (2-byte-write-heap-buffer-overflow)
    // CHECK4: SCARINESS: 52 (8-byte-write-heap-buffer-overflow-far-from-bounds)
    // CHECK5: SCARINESS: 55 (multi-byte-write-heap-buffer-overflow-far-from-bounds)
    // CHECK6: SCARINESS: 22 (1-byte-read-heap-use-after-free)
    // CHECK7: SCARINESS: 46 (4-byte-write-heap-use-after-free)
    // CHECK8: SCARINESS: 33 (8-byte-read-heap-use-after-free)
    // CHECK9: SCARINESS: 55 (multi-byte-write-heap-use-after-free)
    // CHECK10: SCARINESS: 46 (1-byte-write-stack-buffer-overflow)
    // CHECK11: SCARINESS: 38 (8-byte-read-stack-buffer-overflow)
    // CHECK12: SCARINESS: 61 (4-byte-write-stack-buffer-overflow-far-from-bounds)
    // CHECK13: SCARINESS: 32 (1-byte-read-stack-use-after-return)
    // CHECK14: SCARINESS: 65 (multi-byte-write-stack-use-after-return)
    // CHECK15: SCARINESS: 31 (1-byte-write-global-buffer-overflow)
    // CHECK16: SCARINESS: 36 (multi-byte-read-global-buffer-overflow-far-from-bounds)
    // CHECK17: SCARINESS: 42 (double-free)
    // CHECK18: SCARINESS: 15 (stack-overflow)
    // CHECK19: SCARINESS: 10 (null-deref)
    // CHECK20: SCARINESS: 30 (wild-addr-write)
    // CHECK21: SCARINESS: 20 (wild-addr-read)
    // CHECK22: SCARINESS: 10 (signal)
    // CHECK23: SCARINESS: 60 (wild-jump)
    // CHECK24: SCARINESS: 10 (alloc-dealloc-mismatch)
    // CHECK25: SCARINESS: 10 (bad-free)
    // CHECK26: SCARINESS: 10 (memcpy-param-overlap)
  }
}
