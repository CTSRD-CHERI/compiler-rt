//=-- lsan_common.h -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of LeakSanitizer.
// Private LSan header.
//
//===----------------------------------------------------------------------===//

#ifndef LSAN_COMMON_H
#define LSAN_COMMON_H

#include "sanitizer_common/sanitizer_allocator.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_platform.h"
#include "sanitizer_common/sanitizer_stackdepot.h"
#include "sanitizer_common/sanitizer_stoptheworld.h"
#include "sanitizer_common/sanitizer_symbolizer.h"

// LeakSanitizer relies on some Glibc's internals (e.g. TLS machinery) on Linux.
// Also, LSan doesn't like 32 bit architectures
// because of "small" (4 bytes) pointer size that leads to high false negative
// ratio on large leaks. But we still want to have it for some 32 bit arches
// (e.g. x86), see https://github.com/google/sanitizers/issues/403.
// To enable LeakSanitizer on a new architecture, one needs to implement the
// internal_clone function as well as (probably) adjust the TLS machinery for
// the new architecture inside the sanitizer library.
// Exclude leak-detection on arm32 for Android because `__aeabi_read_tp`
// is missing. This caused a link error.
#if SANITIZER_ANDROID && (__ANDROID_API__ < 28 || defined(__arm__))
#define CAN_SANITIZE_LEAKS 0
#elif (SANITIZER_LINUX || SANITIZER_MAC) && (SANITIZER_WORDSIZE == 64) && \
    (defined(__x86_64__) || defined(__mips64) || defined(__aarch64__) ||  \
     defined(__powerpc64__) || defined(__s390x__))
#define CAN_SANITIZE_LEAKS 1
#elif defined(__i386__) && (SANITIZER_LINUX || SANITIZER_MAC)
#define CAN_SANITIZE_LEAKS 1
#elif defined(__arm__) && SANITIZER_LINUX
#define CAN_SANITIZE_LEAKS 1
#elif SANITIZER_RISCV64 && SANITIZER_LINUX
#define CAN_SANITIZE_LEAKS 1
#elif SANITIZER_NETBSD || SANITIZER_FUCHSIA
#define CAN_SANITIZE_LEAKS 1
#else
#define CAN_SANITIZE_LEAKS 0
#endif

namespace __sanitizer {
class FlagParser;
class ThreadRegistry;
class ThreadContextBase;
struct DTLS;
}

namespace __lsan {

// Chunk tags.
enum ChunkTag {
  kDirectlyLeaked = 0,  // default
  kIndirectlyLeaked = 1,
  kReachable = 2,
  kIgnored = 3
};

struct Flags {
#define LSAN_FLAG(Type, Name, DefaultValue, Description) Type Name;
#include "lsan_flags.inc"
#undef LSAN_FLAG

  void SetDefaults();
  uptr pointer_alignment() const {
    return use_unaligned ? 1 : sizeof(uptr);
  }
};

extern Flags lsan_flags;
inline Flags *flags() { return &lsan_flags; }
void RegisterLsanFlags(FlagParser *parser, Flags *f);

struct Leak {
  u32 id;
  uptr hit_count;
  uptr total_size;
  u32 stack_trace_id;
  bool is_directly_leaked;
  bool is_suppressed;
};

struct LeakedObject {
  u32 leak_id;
  uptr addr;
  uptr size;
};

// Aggregates leaks by stack trace prefix.
class LeakReport {
 public:
  LeakReport() {}
  void AddLeakedChunk(uptr chunk, u32 stack_trace_id, uptr leaked_size,
                      ChunkTag tag);
  void ReportTopLeaks(uptr max_leaks);
  void PrintSummary();
  uptr ApplySuppressions();
  uptr UnsuppressedLeakCount();
  uptr IndirectUnsuppressedLeakCount();

 private:
  void PrintReportForLeak(uptr index);
  void PrintLeakedObjectsForLeak(uptr index);

  u32 next_id_ = 0;
  InternalMmapVector<Leak> leaks_;
  InternalMmapVector<LeakedObject> leaked_objects_;
};

typedef InternalMmapVector<uptr> Frontier;

// Platform-specific functions.
void InitializePlatformSpecificModules();
void ProcessGlobalRegions(Frontier *frontier);
void ProcessPlatformSpecificAllocations(Frontier *frontier);

struct RootRegion {
  uptr begin;
  uptr size;
};

// LockStuffAndStopTheWorld can start to use Scan* calls to collect into
// this Frontier vector before the StopTheWorldCallback actually runs.
// This is used when the OS has a unified callback API for suspending
// threads and enumerating roots.
struct CheckForLeaksParam {
  Frontier frontier;
  LeakReport leak_report;
  bool success = false;
};

InternalMmapVectorNoCtor<RootRegion> const *GetRootRegions();
void ScanRootRegion(Frontier *frontier, RootRegion const &region,
                    uptr region_begin, uptr region_end, bool is_readable);
void ForEachExtraStackRangeCb(uptr begin, uptr end, void* arg);
void GetAdditionalThreadContextPtrs(ThreadContextBase *tctx, void *ptrs);
// Run stoptheworld while holding any platform-specific locks, as well as the
// allocator and thread registry locks.
void LockStuffAndStopTheWorld(StopTheWorldCallback callback,
                              CheckForLeaksParam* argument);

void ScanRangeForPointers(uptr begin, uptr end,
                          Frontier *frontier,
                          const char *region_type, ChunkTag tag);
void ScanGlobalRange(uptr begin, uptr end, Frontier *frontier);

enum IgnoreObjectResult {
  kIgnoreObjectSuccess,
  kIgnoreObjectAlreadyIgnored,
  kIgnoreObjectInvalid
};

// Functions called from the parent tool.
const char *MaybeCallLsanDefaultOptions();
void InitCommonLsan();
void DoLeakCheck();
void DoRecoverableLeakCheckVoid();
void DisableCounterUnderflow();
bool DisabledInThisThread();

// Used to implement __lsan::ScopedDisabler.
void DisableInThisThread();
void EnableInThisThread();
// Can be used to ignore memory allocated by an intercepted
// function.
struct ScopedInterceptorDisabler {
  ScopedInterceptorDisabler() { DisableInThisThread(); }
  ~ScopedInterceptorDisabler() { EnableInThisThread(); }
};

// According to Itanium C++ ABI array cookie is a one word containing
// size of allocated array.
static inline bool IsItaniumABIArrayCookie(uptr chunk_beg, uptr chunk_size,
                                           uptr addr) {
  return chunk_size == sizeof(uptr) && chunk_beg + chunk_size == addr &&
         *reinterpret_cast<uptr *>(chunk_beg) == 0;
}

// According to ARM C++ ABI array cookie consists of two words:
// struct array_cookie {
//   std::size_t element_size; // element_size != 0
//   std::size_t element_count;
// };
static inline bool IsARMABIArrayCookie(uptr chunk_beg, uptr chunk_size,
                                       uptr addr) {
  return chunk_size == 2 * sizeof(uptr) && chunk_beg + chunk_size == addr &&
         *reinterpret_cast<uptr *>(chunk_beg + sizeof(uptr)) == 0;
}

// Special case for "new T[0]" where T is a type with DTOR.
// new T[0] will allocate a cookie (one or two words) for the array size (0)
// and store a pointer to the end of allocated chunk. The actual cookie layout
// varies between platforms according to their C++ ABI implementation.
inline bool IsSpecialCaseOfOperatorNew0(uptr chunk_beg, uptr chunk_size,
                                        uptr addr) {
#if defined(__arm__)
  return IsARMABIArrayCookie(chunk_beg, chunk_size, addr);
#else
  return IsItaniumABIArrayCookie(chunk_beg, chunk_size, addr);
#endif
}

// The following must be implemented in the parent tool.

void ForEachChunk(ForEachChunkCallback callback, void *arg);
// Returns the address range occupied by the global allocator object.
void GetAllocatorGlobalRange(uptr *begin, uptr *end);
// Wrappers for allocator's ForceLock()/ForceUnlock().
void LockAllocator();
void UnlockAllocator();
// Returns true if [addr, addr + sizeof(void *)) is poisoned.
bool WordIsPoisoned(uptr addr);
// Wrappers for ThreadRegistry access.
void LockThreadRegistry() NO_THREAD_SAFETY_ANALYSIS;
void UnlockThreadRegistry() NO_THREAD_SAFETY_ANALYSIS;
ThreadRegistry *GetThreadRegistryLocked();
bool GetThreadRangesLocked(tid_t os_id, uptr *stack_begin, uptr *stack_end,
                           uptr *tls_begin, uptr *tls_end, uptr *cache_begin,
                           uptr *cache_end, DTLS **dtls);
void GetAllThreadAllocatorCachesLocked(InternalMmapVector<uptr> *caches);
void ForEachExtraStackRange(tid_t os_id, RangeIteratorCallback callback,
                            void *arg);
// If called from the main thread, updates the main thread's TID in the thread
// registry. We need this to handle processes that fork() without a subsequent
// exec(), which invalidates the recorded TID. To update it, we must call
// gettid() from the main thread. Our solution is to call this function before
// leak checking and also before every call to pthread_create() (to handle cases
// where leak checking is initiated from a non-main thread).
void EnsureMainThreadIDIsCorrect();
// If p points into a chunk that has been allocated to the user, returns its
// user-visible address. Otherwise, returns 0.
uptr PointsIntoChunk(void *p);
// Returns address of user-visible chunk contained in this allocator chunk.
uptr GetUserBegin(uptr chunk);
// Helper for __lsan_ignore_object().
IgnoreObjectResult IgnoreObjectLocked(const void *p);

// Return the linker module, if valid for the platform.
LoadedModule *GetLinker();

// Return true if LSan has finished leak checking and reported leaks.
bool HasReportedLeaks();

// Run platform-specific leak handlers.
void HandleLeaks();

// Wrapper for chunk metadata operations.
class LsanMetadata {
 public:
  // Constructor accepts address of user-visible chunk.
  explicit LsanMetadata(uptr chunk);
  bool allocated() const;
  ChunkTag tag() const;
  void set_tag(ChunkTag value);
  uptr requested_size() const;
  u32 stack_trace_id() const;
 private:
  void *metadata_;
};

}  // namespace __lsan

extern "C" {
SANITIZER_INTERFACE_ATTRIBUTE SANITIZER_WEAK_ATTRIBUTE
const char *__lsan_default_options();

SANITIZER_INTERFACE_ATTRIBUTE SANITIZER_WEAK_ATTRIBUTE
int __lsan_is_turned_off();

SANITIZER_INTERFACE_ATTRIBUTE SANITIZER_WEAK_ATTRIBUTE
const char *__lsan_default_suppressions();
}  // extern "C"

#endif  // LSAN_COMMON_H
