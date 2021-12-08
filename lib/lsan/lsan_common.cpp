//=-- lsan_common.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of LeakSanitizer.
// Implementation of common leak checking functionality.
//
//===----------------------------------------------------------------------===//

#include "lsan_common.h"

#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_flag_parser.h"
#include "sanitizer_common/sanitizer_flags.h"
#include "sanitizer_common/sanitizer_placement_new.h"
#include "sanitizer_common/sanitizer_procmaps.h"
#include "sanitizer_common/sanitizer_report_decorator.h"
#include "sanitizer_common/sanitizer_stackdepot.h"
#include "sanitizer_common/sanitizer_stacktrace.h"
#include "sanitizer_common/sanitizer_suppressions.h"
#include "sanitizer_common/sanitizer_thread_registry.h"
#include "sanitizer_common/sanitizer_tls_get_addr.h"

#if CAN_SANITIZE_LEAKS
namespace __lsan {

// This mutex is used to prevent races between DoLeakCheck and IgnoreObject, and
// also to protect the global list of root regions.
Mutex global_mutex;

Flags lsan_flags;

void DisableCounterUnderflow() {
  if (common_flags()->detect_leaks) {
    Report("Unmatched call to __lsan_enable().\n");
    Die();
  }
}

void Flags::SetDefaults() {
#  define LSAN_FLAG(Type, Name, DefaultValue, Description) Name = DefaultValue;
#  include "lsan_flags.inc"
#  undef LSAN_FLAG
}

void RegisterLsanFlags(FlagParser *parser, Flags *f) {
#  define LSAN_FLAG(Type, Name, DefaultValue, Description) \
    RegisterFlag(parser, #Name, Description, &f->Name);
#  include "lsan_flags.inc"
#  undef LSAN_FLAG
}

#  define LOG_POINTERS(...)      \
    do {                         \
      if (flags()->log_pointers) \
        Report(__VA_ARGS__);     \
    } while (0)

#  define LOG_THREADS(...)      \
    do {                        \
      if (flags()->log_threads) \
        Report(__VA_ARGS__);    \
    } while (0)

class LeakSuppressionContext {
  bool parsed = false;
  SuppressionContext context;
  bool suppressed_stacks_sorted = true;
  InternalMmapVector<u32> suppressed_stacks;

  Suppression *GetSuppressionForAddr(uptr addr);
  void LazyInit();
  bool SuppressByRule(const StackTrace &stack, uptr hit_count, uptr total_size);

 public:
  LeakSuppressionContext(const char *supprression_types[],
                         int suppression_types_num)
      : context(supprression_types, suppression_types_num) {}

  bool Suppress(u32 stack_trace_id, uptr hit_count, uptr total_size);

  const InternalMmapVector<u32> &GetSortedSuppressedStacks() {
    if (!suppressed_stacks_sorted) {
      suppressed_stacks_sorted = true;
      SortAndDedup(suppressed_stacks);
    }
    return suppressed_stacks;
  }
  void PrintMatchedSuppressions();
};

ALIGNED(64) static char suppression_placeholder[sizeof(LeakSuppressionContext)];
static LeakSuppressionContext *suppression_ctx = nullptr;
static const char kSuppressionLeak[] = "leak";
static const char *kSuppressionTypes[] = {kSuppressionLeak};
static const char kStdSuppressions[] =
#  if SANITIZER_SUPPRESS_LEAK_ON_PTHREAD_EXIT
    // For more details refer to the SANITIZER_SUPPRESS_LEAK_ON_PTHREAD_EXIT
    // definition.
    "leak:*pthread_exit*\n"
#  endif  // SANITIZER_SUPPRESS_LEAK_ON_PTHREAD_EXIT
#  if SANITIZER_MAC
    // For Darwin and os_log/os_trace: https://reviews.llvm.org/D35173
    "leak:*_os_trace*\n"
#  endif
    // TLS leak in some glibc versions, described in
    // https://sourceware.org/bugzilla/show_bug.cgi?id=12650.
    "leak:*tls_get_addr*\n";

void InitializeSuppressions() {
  CHECK_EQ(nullptr, suppression_ctx);
  suppression_ctx = new (suppression_placeholder)
      LeakSuppressionContext(kSuppressionTypes, ARRAY_SIZE(kSuppressionTypes));
}

void LeakSuppressionContext::LazyInit() {
  if (!parsed) {
    parsed = true;
    context.ParseFromFile(flags()->suppressions);
    if (&__lsan_default_suppressions)
      context.Parse(__lsan_default_suppressions());
    context.Parse(kStdSuppressions);
  }
}

Suppression *LeakSuppressionContext::GetSuppressionForAddr(uptr addr) {
  Suppression *s = nullptr;

  // Suppress by module name.
  if (const char *module_name =
          Symbolizer::GetOrInit()->GetModuleNameForPc(addr))
    if (context.Match(module_name, kSuppressionLeak, &s))
      return s;

  // Suppress by file or function name.
  SymbolizedStack *frames = Symbolizer::GetOrInit()->SymbolizePC(addr);
  for (SymbolizedStack *cur = frames; cur; cur = cur->next) {
    if (context.Match(cur->info.function, kSuppressionLeak, &s) ||
        context.Match(cur->info.file, kSuppressionLeak, &s)) {
      break;
    }
  }
  frames->ClearAll();
  return s;
}

bool LeakSuppressionContext::SuppressByRule(const StackTrace &stack,
                                            uptr hit_count, uptr total_size) {
  for (uptr i = 0; i < stack.size; i++) {
    Suppression *s = GetSuppressionForAddr(
        StackTrace::GetPreviousInstructionPc(stack.trace[i]));
    if (s) {
      s->weight += total_size;
      atomic_fetch_add(&s->hit_count, hit_count, memory_order_relaxed);
      return true;
    }
  }
  return false;
}

bool LeakSuppressionContext::Suppress(u32 stack_trace_id, uptr hit_count,
                                      uptr total_size) {
  LazyInit();
  StackTrace stack = StackDepotGet(stack_trace_id);
  if (!SuppressByRule(stack, hit_count, total_size))
    return false;
  suppressed_stacks_sorted = false;
  suppressed_stacks.push_back(stack_trace_id);
  return true;
}

static LeakSuppressionContext *GetSuppressionContext() {
  CHECK(suppression_ctx);
  return suppression_ctx;
}

static InternalMmapVectorNoCtor<RootRegion> root_regions;

InternalMmapVectorNoCtor<RootRegion> const *GetRootRegions() {
  return &root_regions;
}

void InitCommonLsan() {
  if (common_flags()->detect_leaks) {
    // Initialization which can fail or print warnings should only be done if
    // LSan is actually enabled.
    InitializeSuppressions();
    InitializePlatformSpecificModules();
  }
}

class Decorator : public __sanitizer::SanitizerCommonDecorator {
 public:
  Decorator() : SanitizerCommonDecorator() {}
  const char *Error() { return Red(); }
  const char *Leak() { return Blue(); }
};

static inline bool CanBeAHeapPointer(uptr p) {
  // Since our heap is located in mmap-ed memory, we can assume a sensible lower
  // bound on heap addresses.
  const uptr kMinAddress = 4 * 4096;
  if (p < kMinAddress)
    return false;
#  if defined(__x86_64__)
  // Accept only canonical form user-space addresses.
  return ((p >> 47) == 0);
#  elif defined(__mips64)
  return ((p >> 40) == 0);
#  elif defined(__aarch64__)
  unsigned runtimeVMA = (MostSignificantSetBitIndex(GET_CURRENT_FRAME()) + 1);
  return ((p >> runtimeVMA) == 0);
#  else
  return true;
#  endif
}

// Scans the memory range, looking for byte patterns that point into allocator
// chunks. Marks those chunks with |tag| and adds them to |frontier|.
// There are two usage modes for this function: finding reachable chunks
// (|tag| = kReachable) and finding indirectly leaked chunks
// (|tag| = kIndirectlyLeaked). In the second case, there's no flood fill,
// so |frontier| = 0.
void ScanRangeForPointers(uptr begin, uptr end, Frontier *frontier,
                          const char *region_type, ChunkTag tag) {
  CHECK(tag == kReachable || tag == kIndirectlyLeaked);
  const uptr alignment = flags()->pointer_alignment();
  LOG_POINTERS("Scanning %s range %p-%p.\n", region_type, (void *)begin,
               (void *)end);
  uptr pp = begin;
  if (pp % alignment)
    pp = pp + alignment - pp % alignment;
  for (; pp + sizeof(void *) <= end; pp += alignment) {
    void *p = *reinterpret_cast<void **>(pp);
    if (!CanBeAHeapPointer(reinterpret_cast<uptr>(p)))
      continue;
    uptr chunk = PointsIntoChunk(p);
    if (!chunk)
      continue;
    // Pointers to self don't count. This matters when tag == kIndirectlyLeaked.
    if (chunk == begin)
      continue;
    LsanMetadata m(chunk);
    if (m.tag() == kReachable || m.tag() == kIgnored)
      continue;

    // Do this check relatively late so we can log only the interesting cases.
    if (!flags()->use_poisoned && WordIsPoisoned(pp)) {
      LOG_POINTERS(
          "%p is poisoned: ignoring %p pointing into chunk %p-%p of size "
          "%zu.\n",
          (void *)pp, p, (void *)chunk, (void *)(chunk + m.requested_size()),
          m.requested_size());
      continue;
    }

    m.set_tag(tag);
    LOG_POINTERS("%p: found %p pointing into chunk %p-%p of size %zu.\n",
                 (void *)pp, p, (void *)chunk,
                 (void *)(chunk + m.requested_size()), m.requested_size());
    if (frontier)
      frontier->push_back(chunk);
  }
}

// Scans a global range for pointers
void ScanGlobalRange(uptr begin, uptr end, Frontier *frontier) {
  uptr allocator_begin = 0, allocator_end = 0;
  GetAllocatorGlobalRange(&allocator_begin, &allocator_end);
  if (begin <= allocator_begin && allocator_begin < end) {
    CHECK_LE(allocator_begin, allocator_end);
    CHECK_LE(allocator_end, end);
    if (begin < allocator_begin)
      ScanRangeForPointers(begin, allocator_begin, frontier, "GLOBAL",
                           kReachable);
    if (allocator_end < end)
      ScanRangeForPointers(allocator_end, end, frontier, "GLOBAL", kReachable);
  } else {
    ScanRangeForPointers(begin, end, frontier, "GLOBAL", kReachable);
  }
}

void ForEachExtraStackRangeCb(uptr begin, uptr end, void *arg) {
  Frontier *frontier = reinterpret_cast<Frontier *>(arg);
  ScanRangeForPointers(begin, end, frontier, "FAKE STACK", kReachable);
}

#  if SANITIZER_FUCHSIA

// Fuchsia handles all threads together with its own callback.
static void ProcessThreads(SuspendedThreadsList const &, Frontier *) {}

#  else

#    if SANITIZER_ANDROID
// FIXME: Move this out into *libcdep.cpp
extern "C" SANITIZER_WEAK_ATTRIBUTE void __libc_iterate_dynamic_tls(
    pid_t, void (*cb)(void *, void *, uptr, void *), void *);
#    endif

static void ProcessThreadRegistry(Frontier *frontier) {
  InternalMmapVector<uptr> ptrs;
  GetThreadRegistryLocked()->RunCallbackForEachThreadLocked(
      GetAdditionalThreadContextPtrs, &ptrs);

  for (uptr i = 0; i < ptrs.size(); ++i) {
    void *ptr = reinterpret_cast<void *>(ptrs[i]);
    uptr chunk = PointsIntoChunk(ptr);
    if (!chunk)
      continue;
    LsanMetadata m(chunk);
    if (!m.allocated())
      continue;

    // Mark as reachable and add to frontier.
    LOG_POINTERS("Treating pointer %p from ThreadContext as reachable\n", ptr);
    m.set_tag(kReachable);
    frontier->push_back(chunk);
  }
}

// Scans thread data (stacks and TLS) for heap pointers.
static void ProcessThreads(SuspendedThreadsList const &suspended_threads,
                           Frontier *frontier) {
  InternalMmapVector<uptr> registers;
  for (uptr i = 0; i < suspended_threads.ThreadCount(); i++) {
    tid_t os_id = static_cast<tid_t>(suspended_threads.GetThreadID(i));
    LOG_THREADS("Processing thread %llu.\n", os_id);
    uptr stack_begin, stack_end, tls_begin, tls_end, cache_begin, cache_end;
    DTLS *dtls;
    bool thread_found =
        GetThreadRangesLocked(os_id, &stack_begin, &stack_end, &tls_begin,
                              &tls_end, &cache_begin, &cache_end, &dtls);
    if (!thread_found) {
      // If a thread can't be found in the thread registry, it's probably in the
      // process of destruction. Log this event and move on.
      LOG_THREADS("Thread %llu not found in registry.\n", os_id);
      continue;
    }
    uptr sp;
    PtraceRegistersStatus have_registers =
        suspended_threads.GetRegistersAndSP(i, &registers, &sp);
    if (have_registers != REGISTERS_AVAILABLE) {
      Report("Unable to get registers from thread %llu.\n", os_id);
      // If unable to get SP, consider the entire stack to be reachable unless
      // GetRegistersAndSP failed with ESRCH.
      if (have_registers == REGISTERS_UNAVAILABLE_FATAL)
        continue;
      sp = stack_begin;
    }

    if (flags()->use_registers && have_registers) {
      uptr registers_begin = reinterpret_cast<uptr>(registers.data());
      uptr registers_end =
          reinterpret_cast<uptr>(registers.data() + registers.size());
      ScanRangeForPointers(registers_begin, registers_end, frontier,
                           "REGISTERS", kReachable);
    }

    if (flags()->use_stacks) {
      LOG_THREADS("Stack at %p-%p (SP = %p).\n", (void *)stack_begin,
                  (void *)stack_end, (void *)sp);
      if (sp < stack_begin || sp >= stack_end) {
        // SP is outside the recorded stack range (e.g. the thread is running a
        // signal handler on alternate stack, or swapcontext was used).
        // Again, consider the entire stack range to be reachable.
        LOG_THREADS("WARNING: stack pointer not in stack range.\n");
        uptr page_size = GetPageSizeCached();
        int skipped = 0;
        while (stack_begin < stack_end &&
               !IsAccessibleMemoryRange(stack_begin, 1)) {
          skipped++;
          stack_begin += page_size;
        }
        LOG_THREADS("Skipped %d guard page(s) to obtain stack %p-%p.\n",
                    skipped, (void *)stack_begin, (void *)stack_end);
      } else {
        // Shrink the stack range to ignore out-of-scope values.
        stack_begin = sp;
      }
      ScanRangeForPointers(stack_begin, stack_end, frontier, "STACK",
                           kReachable);
      ForEachExtraStackRange(os_id, ForEachExtraStackRangeCb, frontier);
    }

    if (flags()->use_tls) {
      if (tls_begin) {
        LOG_THREADS("TLS at %p-%p.\n", (void *)tls_begin, (void *)tls_end);
        // If the tls and cache ranges don't overlap, scan full tls range,
        // otherwise, only scan the non-overlapping portions
        if (cache_begin == cache_end || tls_end < cache_begin ||
            tls_begin > cache_end) {
          ScanRangeForPointers(tls_begin, tls_end, frontier, "TLS", kReachable);
        } else {
          if (tls_begin < cache_begin)
            ScanRangeForPointers(tls_begin, cache_begin, frontier, "TLS",
                                 kReachable);
          if (tls_end > cache_end)
            ScanRangeForPointers(cache_end, tls_end, frontier, "TLS",
                                 kReachable);
        }
      }
#    if SANITIZER_ANDROID
      auto *cb = +[](void *dtls_begin, void *dtls_end, uptr /*dso_idd*/,
                     void *arg) -> void {
        ScanRangeForPointers(reinterpret_cast<uptr>(dtls_begin),
                             reinterpret_cast<uptr>(dtls_end),
                             reinterpret_cast<Frontier *>(arg), "DTLS",
                             kReachable);
      };

      // FIXME: There might be a race-condition here (and in Bionic) if the
      // thread is suspended in the middle of updating its DTLS. IOWs, we
      // could scan already freed memory. (probably fine for now)
      __libc_iterate_dynamic_tls(os_id, cb, frontier);
#    else
      if (dtls && !DTLSInDestruction(dtls)) {
        ForEachDVT(dtls, [&](const DTLS::DTV &dtv, int id) {
          uptr dtls_beg = dtv.beg;
          uptr dtls_end = dtls_beg + dtv.size;
          if (dtls_beg < dtls_end) {
            LOG_THREADS("DTLS %d at %p-%p.\n", id, (void *)dtls_beg,
                        (void *)dtls_end);
            ScanRangeForPointers(dtls_beg, dtls_end, frontier, "DTLS",
                                 kReachable);
          }
        });
      } else {
        // We are handling a thread with DTLS under destruction. Log about
        // this and continue.
        LOG_THREADS("Thread %llu has DTLS under destruction.\n", os_id);
      }
#    endif
    }
  }

  // Add pointers reachable from ThreadContexts
  ProcessThreadRegistry(frontier);
}

#  endif  // SANITIZER_FUCHSIA

void ScanRootRegion(Frontier *frontier, const RootRegion &root_region,
                    uptr region_begin, uptr region_end, bool is_readable) {
  uptr intersection_begin = Max(root_region.begin, region_begin);
  uptr intersection_end = Min(region_end, root_region.begin + root_region.size);
  if (intersection_begin >= intersection_end)
    return;
  LOG_POINTERS("Root region %p-%p intersects with mapped region %p-%p (%s)\n",
               (void *)root_region.begin,
               (void *)(root_region.begin + root_region.size),
               (void *)region_begin, (void *)region_end,
               is_readable ? "readable" : "unreadable");
  if (is_readable)
    ScanRangeForPointers(intersection_begin, intersection_end, frontier, "ROOT",
                         kReachable);
}

static void ProcessRootRegion(Frontier *frontier,
                              const RootRegion &root_region) {
  MemoryMappingLayout proc_maps(/*cache_enabled*/ true);
  MemoryMappedSegment segment;
  while (proc_maps.Next(&segment)) {
    ScanRootRegion(frontier, root_region, segment.start, segment.end,
                   segment.IsReadable());
  }
}

// Scans root regions for heap pointers.
static void ProcessRootRegions(Frontier *frontier) {
  if (!flags()->use_root_regions)
    return;
  for (uptr i = 0; i < root_regions.size(); i++)
    ProcessRootRegion(frontier, root_regions[i]);
}

static void FloodFillTag(Frontier *frontier, ChunkTag tag) {
  while (frontier->size()) {
    uptr next_chunk = frontier->back();
    frontier->pop_back();
    LsanMetadata m(next_chunk);
    ScanRangeForPointers(next_chunk, next_chunk + m.requested_size(), frontier,
                         "HEAP", tag);
  }
}

// ForEachChunk callback. If the chunk is marked as leaked, marks all chunks
// which are reachable from it as indirectly leaked.
static void MarkIndirectlyLeakedCb(uptr chunk, void *arg) {
  chunk = GetUserBegin(chunk);
  LsanMetadata m(chunk);
  if (m.allocated() && m.tag() != kReachable) {
    ScanRangeForPointers(chunk, chunk + m.requested_size(),
                         /* frontier */ nullptr, "HEAP", kIndirectlyLeaked);
  }
}

static void IgnoredSuppressedCb(uptr chunk, void *arg) {
  CHECK(arg);
  chunk = GetUserBegin(chunk);
  LsanMetadata m(chunk);
  if (!m.allocated() || m.tag() == kIgnored)
    return;

  const InternalMmapVector<u32> &suppressed =
      *static_cast<const InternalMmapVector<u32> *>(arg);
  uptr idx = InternalLowerBound(suppressed, m.stack_trace_id());
  if (idx >= suppressed.size() || m.stack_trace_id() != suppressed[idx])
    return;

  LOG_POINTERS("Suppressed: chunk %p-%p of size %zu.\n", (void *)chunk,
               (void *)(chunk + m.requested_size()), m.requested_size());
  m.set_tag(kIgnored);
}

// ForEachChunk callback. If chunk is marked as ignored, adds its address to
// frontier.
static void CollectIgnoredCb(uptr chunk, void *arg) {
  CHECK(arg);
  chunk = GetUserBegin(chunk);
  LsanMetadata m(chunk);
  if (m.allocated() && m.tag() == kIgnored) {
    LOG_POINTERS("Ignored: chunk %p-%p of size %zu.\n", (void *)chunk,
                 (void *)(chunk + m.requested_size()), m.requested_size());
    reinterpret_cast<Frontier *>(arg)->push_back(chunk);
  }
}

static uptr GetCallerPC(const StackTrace &stack) {
  // The top frame is our malloc/calloc/etc. The next frame is the caller.
  if (stack.size >= 2)
    return stack.trace[1];
  return 0;
}

struct InvalidPCParam {
  Frontier *frontier;
  bool skip_linker_allocations;
};

// ForEachChunk callback. If the caller pc is invalid or is within the linker,
// mark as reachable. Called by ProcessPlatformSpecificAllocations.
static void MarkInvalidPCCb(uptr chunk, void *arg) {
  CHECK(arg);
  InvalidPCParam *param = reinterpret_cast<InvalidPCParam *>(arg);
  chunk = GetUserBegin(chunk);
  LsanMetadata m(chunk);
  if (m.allocated() && m.tag() != kReachable && m.tag() != kIgnored) {
    u32 stack_id = m.stack_trace_id();
    uptr caller_pc = 0;
    if (stack_id > 0)
      caller_pc = GetCallerPC(StackDepotGet(stack_id));
    // If caller_pc is unknown, this chunk may be allocated in a coroutine. Mark
    // it as reachable, as we can't properly report its allocation stack anyway.
    if (caller_pc == 0 || (param->skip_linker_allocations &&
                           GetLinker()->containsAddress(caller_pc))) {
      m.set_tag(kIgnored);
      param->frontier->push_back(chunk);
    }
  }
}

// On Linux, treats all chunks allocated from ld-linux.so as reachable, which
// covers dynamically allocated TLS blocks, internal dynamic loader's loaded
// modules accounting etc.
// Dynamic TLS blocks contain the TLS variables of dynamically loaded modules.
// They are allocated with a __libc_memalign() call in allocate_and_init()
// (elf/dl-tls.c). Glibc won't tell us the address ranges occupied by those
// blocks, but we can make sure they come from our own allocator by intercepting
// __libc_memalign(). On top of that, there is no easy way to reach them. Their
// addresses are stored in a dynamically allocated array (the DTV) which is
// referenced from the static TLS. Unfortunately, we can't just rely on the DTV
// being reachable from the static TLS, and the dynamic TLS being reachable from
// the DTV. This is because the initial DTV is allocated before our interception
// mechanism kicks in, and thus we don't recognize it as allocated memory. We
// can't special-case it either, since we don't know its size.
// Our solution is to include in the root set all allocations made from
// ld-linux.so (which is where allocate_and_init() is implemented). This is
// guaranteed to include all dynamic TLS blocks (and possibly other allocations
// which we don't care about).
// On all other platforms, this simply checks to ensure that the caller pc is
// valid before reporting chunks as leaked.
static void ProcessPC(Frontier *frontier) {
  InvalidPCParam arg;
  arg.frontier = frontier;
  arg.skip_linker_allocations =
      flags()->use_tls && flags()->use_ld_allocations && GetLinker() != nullptr;
  ForEachChunk(MarkInvalidPCCb, &arg);
}

// Sets the appropriate tag on each chunk.
static void ClassifyAllChunks(SuspendedThreadsList const &suspended_threads,
                              Frontier *frontier) {
  const InternalMmapVector<u32> &suppressed_stacks =
      GetSuppressionContext()->GetSortedSuppressedStacks();
  if (!suppressed_stacks.empty()) {
    ForEachChunk(IgnoredSuppressedCb,
                 const_cast<InternalMmapVector<u32> *>(&suppressed_stacks));
  }
  ForEachChunk(CollectIgnoredCb, frontier);
  ProcessGlobalRegions(frontier);
  ProcessThreads(suspended_threads, frontier);
  ProcessRootRegions(frontier);
  FloodFillTag(frontier, kReachable);

  CHECK_EQ(0, frontier->size());
  ProcessPC(frontier);

  // The check here is relatively expensive, so we do this in a separate flood
  // fill. That way we can skip the check for chunks that are reachable
  // otherwise.
  LOG_POINTERS("Processing platform-specific allocations.\n");
  ProcessPlatformSpecificAllocations(frontier);
  FloodFillTag(frontier, kReachable);

  // Iterate over leaked chunks and mark those that are reachable from other
  // leaked chunks.
  LOG_POINTERS("Scanning leaked chunks.\n");
  ForEachChunk(MarkIndirectlyLeakedCb, nullptr);
}

// ForEachChunk callback. Resets the tags to pre-leak-check state.
static void ResetTagsCb(uptr chunk, void *arg) {
  (void)arg;
  chunk = GetUserBegin(chunk);
  LsanMetadata m(chunk);
  if (m.allocated() && m.tag() != kIgnored)
    m.set_tag(kDirectlyLeaked);
}

// ForEachChunk callback. Aggregates information about unreachable chunks into
// a LeakReport.
static void CollectLeaksCb(uptr chunk, void *arg) {
  CHECK(arg);
  LeakedChunks *leaks = reinterpret_cast<LeakedChunks *>(arg);
  chunk = GetUserBegin(chunk);
  LsanMetadata m(chunk);
  if (!m.allocated())
    return;
  if (m.tag() == kDirectlyLeaked || m.tag() == kIndirectlyLeaked)
    leaks->push_back({chunk, m.stack_trace_id(), m.requested_size(), m.tag()});
}

void LeakSuppressionContext::PrintMatchedSuppressions() {
  InternalMmapVector<Suppression *> matched;
  context.GetMatched(&matched);
  if (!matched.size())
    return;
  const char *line = "-----------------------------------------------------";
  Printf("%s\n", line);
  Printf("Suppressions used:\n");
  Printf("  count      bytes template\n");
  for (uptr i = 0; i < matched.size(); i++) {
    Printf("%7zu %10zu %s\n",
           static_cast<uptr>(atomic_load_relaxed(&matched[i]->hit_count)),
           matched[i]->weight, matched[i]->templ);
  }
  Printf("%s\n\n", line);
}

static void ReportIfNotSuspended(ThreadContextBase *tctx, void *arg) {
  const InternalMmapVector<tid_t> &suspended_threads =
      *(const InternalMmapVector<tid_t> *)arg;
  if (tctx->status == ThreadStatusRunning) {
    uptr i = InternalLowerBound(suspended_threads, tctx->os_id);
    if (i >= suspended_threads.size() || suspended_threads[i] != tctx->os_id)
      Report(
          "Running thread %llu was not suspended. False leaks are possible.\n",
          tctx->os_id);
  }
}

#  if SANITIZER_FUCHSIA

// Fuchsia provides a libc interface that guarantees all threads are
// covered, and SuspendedThreadList is never really used.
static void ReportUnsuspendedThreads(const SuspendedThreadsList &) {}

#  else  // !SANITIZER_FUCHSIA

static void ReportUnsuspendedThreads(
    const SuspendedThreadsList &suspended_threads) {
  InternalMmapVector<tid_t> threads(suspended_threads.ThreadCount());
  for (uptr i = 0; i < suspended_threads.ThreadCount(); ++i)
    threads[i] = suspended_threads.GetThreadID(i);

  Sort(threads.data(), threads.size());

  GetThreadRegistryLocked()->RunCallbackForEachThreadLocked(
      &ReportIfNotSuspended, &threads);
}

#  endif  // !SANITIZER_FUCHSIA

static void CheckForLeaksCallback(const SuspendedThreadsList &suspended_threads,
                                  void *arg) {
  CheckForLeaksParam *param = reinterpret_cast<CheckForLeaksParam *>(arg);
  CHECK(param);
  CHECK(!param->success);
  ReportUnsuspendedThreads(suspended_threads);
  ClassifyAllChunks(suspended_threads, &param->frontier);
  ForEachChunk(CollectLeaksCb, &param->leaks);
  // Clean up for subsequent leak checks. This assumes we did not overwrite any
  // kIgnored tags.
  ForEachChunk(ResetTagsCb, nullptr);
  param->success = true;
}

static bool PrintResults(LeakReport &report) {
  uptr unsuppressed_count = report.UnsuppressedLeakCount();
  if (unsuppressed_count) {
    Decorator d;
    Printf(
        "\n"
        "================================================================="
        "\n");
    Printf("%s", d.Error());
    Report("ERROR: LeakSanitizer: detected memory leaks\n");
    Printf("%s", d.Default());
    report.ReportTopLeaks(flags()->max_leaks);
  }
  if (common_flags()->print_suppressions)
    GetSuppressionContext()->PrintMatchedSuppressions();
  if (unsuppressed_count > 0) {
    report.PrintSummary();
    return true;
  }
  return false;
}

static bool CheckForLeaks() {
  if (&__lsan_is_turned_off && __lsan_is_turned_off())
    return false;
  // Inside LockStuffAndStopTheWorld we can't run symbolizer, so we can't match
  // suppressions. However if a stack id was previously suppressed, it should be
  // suppressed in future checks as well.
  for (int i = 0;; ++i) {
    EnsureMainThreadIDIsCorrect();
    CheckForLeaksParam param;
    LockStuffAndStopTheWorld(CheckForLeaksCallback, &param);
    if (!param.success) {
      Report("LeakSanitizer has encountered a fatal error.\n");
      Report(
          "HINT: For debugging, try setting environment variable "
          "LSAN_OPTIONS=verbosity=1:log_threads=1\n");
      Report(
          "HINT: LeakSanitizer does not work under ptrace (strace, gdb, "
          "etc)\n");
      Die();
    }
    LeakReport leak_report;
    leak_report.AddLeakedChunks(param.leaks);

    // No new suppressions stacks, so rerun will not help and we can report.
    if (!leak_report.ApplySuppressions())
      return PrintResults(leak_report);

    // No indirect leaks to report, so we are done here.
    if (!leak_report.IndirectUnsuppressedLeakCount())
      return PrintResults(leak_report);

    if (i >= 8) {
      Report("WARNING: LeakSanitizer gave up on indirect leaks suppression.\n");
      return PrintResults(leak_report);
    }

    // We found a new previously unseen suppressed call stack. Rerun to make
    // sure it does not hold indirect leaks.
    VReport(1, "Rerun with %zu suppressed stacks.",
            GetSuppressionContext()->GetSortedSuppressedStacks().size());
  }
}

static bool has_reported_leaks = false;
bool HasReportedLeaks() { return has_reported_leaks; }

void DoLeakCheck() {
  Lock l(&global_mutex);
  static bool already_done;
  if (already_done)
    return;
  already_done = true;
  has_reported_leaks = CheckForLeaks();
  if (has_reported_leaks)
    HandleLeaks();
}

static int DoRecoverableLeakCheck() {
  Lock l(&global_mutex);
  bool have_leaks = CheckForLeaks();
  return have_leaks ? 1 : 0;
}

void DoRecoverableLeakCheckVoid() { DoRecoverableLeakCheck(); }

///// LeakReport implementation. /////

// A hard limit on the number of distinct leaks, to avoid quadratic complexity
// in LeakReport::AddLeakedChunk(). We don't expect to ever see this many leaks
// in real-world applications.
// FIXME: Get rid of this limit by moving logic into DedupLeaks.
const uptr kMaxLeaksConsidered = 5000;

void LeakReport::AddLeakedChunks(const LeakedChunks &chunks) {
  for (const LeakedChunk &leak : chunks) {
    uptr chunk = leak.chunk;
    u32 stack_trace_id = leak.stack_trace_id;
    uptr leaked_size = leak.leaked_size;
    ChunkTag tag = leak.tag;
    CHECK(tag == kDirectlyLeaked || tag == kIndirectlyLeaked);

    if (u32 resolution = flags()->resolution) {
      StackTrace stack = StackDepotGet(stack_trace_id);
      stack.size = Min(stack.size, resolution);
      stack_trace_id = StackDepotPut(stack);
    }

    bool is_directly_leaked = (tag == kDirectlyLeaked);
    uptr i;
    for (i = 0; i < leaks_.size(); i++) {
      if (leaks_[i].stack_trace_id == stack_trace_id &&
          leaks_[i].is_directly_leaked == is_directly_leaked) {
        leaks_[i].hit_count++;
        leaks_[i].total_size += leaked_size;
        break;
      }
    }
    if (i == leaks_.size()) {
      if (leaks_.size() == kMaxLeaksConsidered)
        return;
      Leak leak = {next_id_++,         /* hit_count */ 1,
                   leaked_size,        stack_trace_id,
                   is_directly_leaked, /* is_suppressed */ false};
      leaks_.push_back(leak);
    }
    if (flags()->report_objects) {
      LeakedObject obj = {leaks_[i].id, chunk, leaked_size};
      leaked_objects_.push_back(obj);
    }
  }
}

static bool LeakComparator(const Leak &leak1, const Leak &leak2) {
  if (leak1.is_directly_leaked == leak2.is_directly_leaked)
    return leak1.total_size > leak2.total_size;
  else
    return leak1.is_directly_leaked;
}

void LeakReport::ReportTopLeaks(uptr num_leaks_to_report) {
  CHECK(leaks_.size() <= kMaxLeaksConsidered);
  Printf("\n");
  if (leaks_.size() == kMaxLeaksConsidered)
    Printf(
        "Too many leaks! Only the first %zu leaks encountered will be "
        "reported.\n",
        kMaxLeaksConsidered);

  uptr unsuppressed_count = UnsuppressedLeakCount();
  if (num_leaks_to_report > 0 && num_leaks_to_report < unsuppressed_count)
    Printf("The %zu top leak(s):\n", num_leaks_to_report);
  Sort(leaks_.data(), leaks_.size(), &LeakComparator);
  uptr leaks_reported = 0;
  for (uptr i = 0; i < leaks_.size(); i++) {
    if (leaks_[i].is_suppressed)
      continue;
    PrintReportForLeak(i);
    leaks_reported++;
    if (leaks_reported == num_leaks_to_report)
      break;
  }
  if (leaks_reported < unsuppressed_count) {
    uptr remaining = unsuppressed_count - leaks_reported;
    Printf("Omitting %zu more leak(s).\n", remaining);
  }
}

void LeakReport::PrintReportForLeak(uptr index) {
  Decorator d;
  Printf("%s", d.Leak());
  Printf("%s leak of %zu byte(s) in %zu object(s) allocated from:\n",
         leaks_[index].is_directly_leaked ? "Direct" : "Indirect",
         leaks_[index].total_size, leaks_[index].hit_count);
  Printf("%s", d.Default());

  CHECK(leaks_[index].stack_trace_id);
  StackDepotGet(leaks_[index].stack_trace_id).Print();

  if (flags()->report_objects) {
    Printf("Objects leaked above:\n");
    PrintLeakedObjectsForLeak(index);
    Printf("\n");
  }
}

void LeakReport::PrintLeakedObjectsForLeak(uptr index) {
  u32 leak_id = leaks_[index].id;
  for (uptr j = 0; j < leaked_objects_.size(); j++) {
    if (leaked_objects_[j].leak_id == leak_id)
      Printf("%p (%zu bytes)\n", (void *)leaked_objects_[j].addr,
             leaked_objects_[j].size);
  }
}

void LeakReport::PrintSummary() {
  CHECK(leaks_.size() <= kMaxLeaksConsidered);
  uptr bytes = 0, allocations = 0;
  for (uptr i = 0; i < leaks_.size(); i++) {
    if (leaks_[i].is_suppressed)
      continue;
    bytes += leaks_[i].total_size;
    allocations += leaks_[i].hit_count;
  }
  InternalScopedString summary;
  summary.append("%zu byte(s) leaked in %zu allocation(s).", bytes,
                 allocations);
  ReportErrorSummary(summary.data());
}

uptr LeakReport::ApplySuppressions() {
  LeakSuppressionContext *suppressions = GetSuppressionContext();
  uptr new_suppressions = false;
  for (uptr i = 0; i < leaks_.size(); i++) {
    if (suppressions->Suppress(leaks_[i].stack_trace_id, leaks_[i].hit_count,
                               leaks_[i].total_size)) {
      leaks_[i].is_suppressed = true;
      ++new_suppressions;
    }
  }
  return new_suppressions;
}

uptr LeakReport::UnsuppressedLeakCount() {
  uptr result = 0;
  for (uptr i = 0; i < leaks_.size(); i++)
    if (!leaks_[i].is_suppressed)
      result++;
  return result;
}

uptr LeakReport::IndirectUnsuppressedLeakCount() {
  uptr result = 0;
  for (uptr i = 0; i < leaks_.size(); i++)
    if (!leaks_[i].is_suppressed && !leaks_[i].is_directly_leaked)
      result++;
  return result;
}

}  // namespace __lsan
#else   // CAN_SANITIZE_LEAKS
namespace __lsan {
void InitCommonLsan() {}
void DoLeakCheck() {}
void DoRecoverableLeakCheckVoid() {}
void DisableInThisThread() {}
void EnableInThisThread() {}
}  // namespace __lsan
#endif  // CAN_SANITIZE_LEAKS

using namespace __lsan;

extern "C" {
SANITIZER_INTERFACE_ATTRIBUTE
void __lsan_ignore_object(const void *p) {
#if CAN_SANITIZE_LEAKS
  if (!common_flags()->detect_leaks)
    return;
  // Cannot use PointsIntoChunk or LsanMetadata here, since the allocator is not
  // locked.
  Lock l(&global_mutex);
  IgnoreObjectResult res = IgnoreObjectLocked(p);
  if (res == kIgnoreObjectInvalid)
    VReport(1, "__lsan_ignore_object(): no heap object found at %p", p);
  if (res == kIgnoreObjectAlreadyIgnored)
    VReport(1,
            "__lsan_ignore_object(): "
            "heap object at %p is already being ignored\n",
            p);
  if (res == kIgnoreObjectSuccess)
    VReport(1, "__lsan_ignore_object(): ignoring heap object at %p\n", p);
#endif  // CAN_SANITIZE_LEAKS
}

SANITIZER_INTERFACE_ATTRIBUTE
void __lsan_register_root_region(const void *begin, uptr size) {
#if CAN_SANITIZE_LEAKS
  Lock l(&global_mutex);
  RootRegion region = {reinterpret_cast<uptr>(begin), size};
  root_regions.push_back(region);
  VReport(1, "Registered root region at %p of size %zu\n", begin, size);
#endif  // CAN_SANITIZE_LEAKS
}

SANITIZER_INTERFACE_ATTRIBUTE
void __lsan_unregister_root_region(const void *begin, uptr size) {
#if CAN_SANITIZE_LEAKS
  Lock l(&global_mutex);
  bool removed = false;
  for (uptr i = 0; i < root_regions.size(); i++) {
    RootRegion region = root_regions[i];
    if (region.begin == reinterpret_cast<uptr>(begin) && region.size == size) {
      removed = true;
      uptr last_index = root_regions.size() - 1;
      root_regions[i] = root_regions[last_index];
      root_regions.pop_back();
      VReport(1, "Unregistered root region at %p of size %zu\n", begin, size);
      break;
    }
  }
  if (!removed) {
    Report(
        "__lsan_unregister_root_region(): region at %p of size %zu has not "
        "been registered.\n",
        begin, size);
    Die();
  }
#endif  // CAN_SANITIZE_LEAKS
}

SANITIZER_INTERFACE_ATTRIBUTE
void __lsan_disable() {
#if CAN_SANITIZE_LEAKS
  __lsan::DisableInThisThread();
#endif
}

SANITIZER_INTERFACE_ATTRIBUTE
void __lsan_enable() {
#if CAN_SANITIZE_LEAKS
  __lsan::EnableInThisThread();
#endif
}

SANITIZER_INTERFACE_ATTRIBUTE
void __lsan_do_leak_check() {
#if CAN_SANITIZE_LEAKS
  if (common_flags()->detect_leaks)
    __lsan::DoLeakCheck();
#endif  // CAN_SANITIZE_LEAKS
}

SANITIZER_INTERFACE_ATTRIBUTE
int __lsan_do_recoverable_leak_check() {
#if CAN_SANITIZE_LEAKS
  if (common_flags()->detect_leaks)
    return __lsan::DoRecoverableLeakCheck();
#endif  // CAN_SANITIZE_LEAKS
  return 0;
}

SANITIZER_INTERFACE_WEAK_DEF(const char *, __lsan_default_options, void) {
  return "";
}

#if !SANITIZER_SUPPORTS_WEAK_HOOKS
SANITIZER_INTERFACE_ATTRIBUTE SANITIZER_WEAK_ATTRIBUTE int
__lsan_is_turned_off() {
  return 0;
}

SANITIZER_INTERFACE_ATTRIBUTE SANITIZER_WEAK_ATTRIBUTE const char *
__lsan_default_suppressions() {
  return "";
}
#endif
}  // extern "C"
