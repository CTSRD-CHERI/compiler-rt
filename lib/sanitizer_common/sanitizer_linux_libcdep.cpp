//===-- sanitizer_linux_libcdep.cpp ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is shared between AddressSanitizer and ThreadSanitizer
// run-time libraries and implements linux-specific functions from
// sanitizer_libc.h.
//===----------------------------------------------------------------------===//

#include "sanitizer_platform.h"

#if SANITIZER_FREEBSD || SANITIZER_LINUX || SANITIZER_NETBSD || \
    SANITIZER_SOLARIS

#include "sanitizer_allocator_internal.h"
#include "sanitizer_atomic.h"
#include "sanitizer_common.h"
#include "sanitizer_file.h"
#include "sanitizer_flags.h"
#include "sanitizer_freebsd.h"
#include "sanitizer_getauxval.h"
#include "sanitizer_glibc_version.h"
#include "sanitizer_linux.h"
#include "sanitizer_placement_new.h"
#include "sanitizer_procmaps.h"

#if SANITIZER_NETBSD
#define _RTLD_SOURCE  // for __lwp_gettcb_fast() / __lwp_getprivate_fast()
#endif

#include <dlfcn.h>  // for dlsym()
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <syslog.h>

#if !defined(ElfW)
#define ElfW(type) Elf_##type
#endif

#if SANITIZER_FREEBSD
#include <pthread_np.h>
#include <osreldate.h>
#include <sys/sysctl.h>
#define pthread_getattr_np pthread_attr_get_np
#endif

#if SANITIZER_NETBSD
#include <sys/sysctl.h>
#include <sys/tls.h>
#include <lwp.h>
#endif

#if SANITIZER_SOLARIS
#include <stdlib.h>
#include <thread.h>
#endif

#if SANITIZER_ANDROID
#include <android/api-level.h>
#if !defined(CPU_COUNT) && !defined(__aarch64__)
#include <dirent.h>
#include <fcntl.h>
struct __sanitizer::linux_dirent {
  long           d_ino;
  off_t          d_off;
  unsigned short d_reclen;
  char           d_name[];
};
#endif
#endif

#if !SANITIZER_ANDROID
#include <elf.h>
#include <unistd.h>
#endif

namespace __sanitizer {

SANITIZER_WEAK_ATTRIBUTE int
real_sigaction(int signum, const void *act, void *oldact);

int internal_sigaction(int signum, const void *act, void *oldact) {
#if !SANITIZER_GO
  if (&real_sigaction)
    return real_sigaction(signum, act, oldact);
#endif
  return sigaction(signum, (const struct sigaction *)act,
                   (struct sigaction *)oldact);
}

void GetThreadStackTopAndBottom(bool at_initialization, uptr *stack_top,
                                uptr *stack_bottom) {
  CHECK(stack_top);
  CHECK(stack_bottom);
  if (at_initialization) {
    // This is the main thread. Libpthread may not be initialized yet.
    struct rlimit rl;
    CHECK_EQ(getrlimit(RLIMIT_STACK, &rl), 0);

    // Find the mapping that contains a stack variable.
    MemoryMappingLayout proc_maps(/*cache_enabled*/true);
    if (proc_maps.Error()) {
      *stack_top = *stack_bottom = 0;
      return;
    }
    MemoryMappedSegment segment;
    uptr prev_end = 0;
    while (proc_maps.Next(&segment)) {
      if ((uptr)&rl < segment.end) break;
      prev_end = segment.end;
    }
    CHECK((uptr)&rl >= segment.start && (uptr)&rl < segment.end);

    // Get stacksize from rlimit, but clip it so that it does not overlap
    // with other mappings.
    usize stacksize = rl.rlim_cur;
    if (stacksize > (char *)segment.end - (char *)prev_end)
      stacksize = (char *)segment.end - (char *)prev_end;
    // When running with unlimited stack size, we still want to set some limit.
    // The unlimited stack size is caused by 'ulimit -s unlimited'.
    // Also, for some reason, GNU make spawns subprocesses with unlimited stack.
    if (stacksize > kMaxThreadStackSize)
      stacksize = kMaxThreadStackSize;
    *stack_top = segment.end;
    *stack_bottom = segment.end - stacksize;
    return;
  }
  usize stacksize = 0;
  void *stackaddr = nullptr;
#if SANITIZER_SOLARIS
  stack_t ss;
  CHECK_EQ(thr_stksegment(&ss), 0);
  stacksize = ss.ss_size;
  stackaddr = (char *)ss.ss_sp - stacksize;
#else  // !SANITIZER_SOLARIS
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  CHECK_EQ(pthread_getattr_np(pthread_self(), &attr), 0);
  my_pthread_attr_getstack(&attr, &stackaddr, &stacksize);
  pthread_attr_destroy(&attr);
#endif  // SANITIZER_SOLARIS

  *stack_top = (uptr)stackaddr + stacksize;
  *stack_bottom = (uptr)stackaddr;
}

#if !SANITIZER_GO
bool SetEnv(const char *name, const char *value) {
  void *f = dlsym(RTLD_NEXT, "setenv");
  if (!f)
    return false;
  typedef int(*setenv_ft)(const char *name, const char *value, int overwrite);
  setenv_ft setenv_f;
  CHECK_EQ(sizeof(setenv_f), sizeof(f));
  internal_memcpy(&setenv_f, &f, sizeof(f));
  return setenv_f(name, value, 1) == 0;
}
#endif

__attribute__((unused)) static bool GetLibcVersion(int *major, int *minor,
                                                   int *patch) {
#ifdef _CS_GNU_LIBC_VERSION
  char buf[64];
  usize len = confstr(_CS_GNU_LIBC_VERSION, buf, sizeof(buf));
  if (len >= sizeof(buf))
    return false;
  buf[len] = 0;
  static const char kGLibC[] = "glibc ";
  if (internal_strncmp(buf, kGLibC, sizeof(kGLibC) - 1) != 0)
    return false;
  const char *p = buf + sizeof(kGLibC) - 1;
  *major = internal_simple_strtoll(p, &p, 10);
  *minor = (*p == '.') ? internal_simple_strtoll(p + 1, &p, 10) : 0;
  *patch = (*p == '.') ? internal_simple_strtoll(p + 1, &p, 10) : 0;
  return true;
#else
  return false;
#endif
}

#if SANITIZER_GLIBC && !SANITIZER_GO
static usize g_tls_size;

#ifdef __i386__
#define CHECK_GET_TLS_STATIC_INFO_VERSION (!__GLIBC_PREREQ(2, 27))
#else
#define CHECK_GET_TLS_STATIC_INFO_VERSION 0
#endif

#if CHECK_GET_TLS_STATIC_INFO_VERSION
#define DL_INTERNAL_FUNCTION __attribute__((regparm(3), stdcall))
#else
#define DL_INTERNAL_FUNCTION
#endif

namespace {
struct GetTlsStaticInfoCall {
  typedef void (*get_tls_func)(size_t*, size_t*);
};
struct GetTlsStaticInfoRegparmCall {
  typedef void (*get_tls_func)(size_t*, size_t*) DL_INTERNAL_FUNCTION;
};

template <typename T>
void CallGetTls(void* ptr, size_t* size, size_t* align) {
  typename T::get_tls_func get_tls;
  CHECK_EQ(sizeof(get_tls), sizeof(ptr));
  internal_memcpy(&get_tls, &ptr, sizeof(ptr));
  CHECK_NE(get_tls, 0);
  get_tls(size, align);
}

bool CmpLibcVersion(int major, int minor, int patch) {
  int ma;
  int mi;
  int pa;
  if (!GetLibcVersion(&ma, &mi, &pa))
    return false;
  if (ma > major)
    return true;
  if (ma < major)
    return false;
  if (mi > minor)
    return true;
  if (mi < minor)
    return false;
  return pa >= patch;
}

}  // namespace

void InitTlsSize() {
  // all current supported platforms have 16 bytes stack alignment
  const size_t kStackAlign = 16;
  void *get_tls_static_info_ptr = dlsym(RTLD_NEXT, "_dl_get_tls_static_info");
  size_t tls_size = 0;
  size_t tls_align = 0;
  // On i?86, _dl_get_tls_static_info used to be internal_function, i.e.
  // __attribute__((regparm(3), stdcall)) before glibc 2.27 and is normal
  // function in 2.27 and later.
  if (CHECK_GET_TLS_STATIC_INFO_VERSION && !CmpLibcVersion(2, 27, 0))
    CallGetTls<GetTlsStaticInfoRegparmCall>(get_tls_static_info_ptr,
                                            &tls_size, &tls_align);
  else
    CallGetTls<GetTlsStaticInfoCall>(get_tls_static_info_ptr,
                                     &tls_size, &tls_align);
  if (tls_align < kStackAlign)
    tls_align = kStackAlign;
  g_tls_size = RoundUpTo(tls_size, tls_align);
}
#else
void InitTlsSize() { }
#endif  // SANITIZER_GLIBC && !SANITIZER_GO

#if (defined(__x86_64__) || defined(__i386__) || defined(__mips__) ||       \
     defined(__aarch64__) || defined(__powerpc64__) || defined(__s390__) || \
     defined(__arm__) || SANITIZER_RISCV64) &&                              \
    SANITIZER_LINUX && !SANITIZER_ANDROID
// sizeof(struct pthread) from glibc.
static atomic_size_t thread_descriptor_size;

usize ThreadDescriptorSize() {
  usize val = atomic_load_relaxed(&thread_descriptor_size);
  if (val)
    return val;
#if defined(__x86_64__) || defined(__i386__) || defined(__arm__)
  int major;
  int minor;
  int patch;
  if (GetLibcVersion(&major, &minor, &patch) && major == 2) {
    /* sizeof(struct pthread) values from various glibc versions.  */
    if (SANITIZER_X32)
      val = 1728; // Assume only one particular version for x32.
    // For ARM sizeof(struct pthread) changed in Glibc 2.23.
    else if (SANITIZER_ARM)
      val = minor <= 22 ? 1120 : 1216;
    else if (minor <= 3)
      val = FIRST_32_SECOND_64(1104, 1696);
    else if (minor == 4)
      val = FIRST_32_SECOND_64(1120, 1728);
    else if (minor == 5)
      val = FIRST_32_SECOND_64(1136, 1728);
    else if (minor <= 9)
      val = FIRST_32_SECOND_64(1136, 1712);
    else if (minor == 10)
      val = FIRST_32_SECOND_64(1168, 1776);
    else if (minor == 11 || (minor == 12 && patch == 1))
      val = FIRST_32_SECOND_64(1168, 2288);
    else if (minor <= 14)
      val = FIRST_32_SECOND_64(1168, 2304);
    else if (minor < 32)  // Unknown version
      val = FIRST_32_SECOND_64(1216, 2304);
    else  // minor == 32
      val = FIRST_32_SECOND_64(1344, 2496);
  }
#elif defined(__mips__)
  // TODO(sagarthakur): add more values as per different glibc versions.
  val = FIRST_32_SECOND_64(1152, 1776);
#elif SANITIZER_RISCV64
  int major;
  int minor;
  int patch;
  if (GetLibcVersion(&major, &minor, &patch) && major == 2) {
    // TODO: consider adding an optional runtime check for an unknown (untested)
    // glibc version
    if (minor <= 28)  // WARNING: the highest tested version is 2.29
      val = 1772;     // no guarantees for this one
    else if (minor <= 31)
      val = 1772;  // tested against glibc 2.29, 2.31
    else
      val = 1936;  // tested against glibc 2.32
  }

#elif defined(__aarch64__)
  // The sizeof (struct pthread) is the same from GLIBC 2.17 to 2.22.
  val = 1776;
#elif defined(__powerpc64__)
  val = 1776; // from glibc.ppc64le 2.20-8.fc21
#elif defined(__s390__)
  val = FIRST_32_SECOND_64(1152, 1776); // valid for glibc 2.22
#endif
  if (val)
    atomic_store_relaxed(&thread_descriptor_size, val);
  return val;
}

// The offset at which pointer to self is located in the thread descriptor.
const usize kThreadSelfOffset = FIRST_32_SECOND_64(8, 16);

usize ThreadSelfOffset() { return kThreadSelfOffset; }

#if defined(__mips__) || defined(__powerpc64__) || SANITIZER_RISCV64
// TlsPreTcbSize includes size of struct pthread_descr and size of tcb
// head structure. It lies before the static tls blocks.
static usize TlsPreTcbSize() {
#if defined(__mips__)
  const usize kTcbHead = 16; // sizeof (tcbhead_t)
#elif defined(__powerpc64__)
  const usize kTcbHead = 88; // sizeof (tcbhead_t)
#elif SANITIZER_RISCV64
  const usize kTcbHead = 16;  // sizeof (tcbhead_t)
#endif
  const usize kTlsAlign = 16;
  const usize kTlsPreTcbSize =
      RoundUpTo(ThreadDescriptorSize() + kTcbHead, kTlsAlign);
  return kTlsPreTcbSize;
}
#endif

uptr ThreadSelf() {
  uptr descr_addr;
#if defined(__i386__)
  asm("mov %%gs:%c1,%0" : "=r"(descr_addr) : "i"(kThreadSelfOffset));
#elif defined(__x86_64__)
  asm("mov %%fs:%c1,%0" : "=r"(descr_addr) : "i"(kThreadSelfOffset));
#elif defined(__mips__)
  // MIPS uses TLS variant I. The thread pointer (in hardware register $29)
  // points to the end of the TCB + 0x7000. The pthread_descr structure is
  // immediately in front of the TCB. TlsPreTcbSize() includes the size of the
  // TCB and the size of pthread_descr.
  const uptr kTlsTcbOffset = 0x7000;
  uptr thread_pointer;
  asm volatile(".set push;\
                .set mips64r2;\
                rdhwr %0,$29;\
                .set pop" : "=r" (thread_pointer));
  descr_addr = thread_pointer - kTlsTcbOffset - TlsPreTcbSize();
#elif defined(__aarch64__) || defined(__arm__)
  descr_addr = reinterpret_cast<uptr>(__builtin_thread_pointer()) -
                                      ThreadDescriptorSize();
#elif SANITIZER_RISCV64
  // https://github.com/riscv/riscv-elf-psabi-doc/issues/53
  uptr thread_pointer = reinterpret_cast<uptr>(__builtin_thread_pointer());
  descr_addr = thread_pointer - TlsPreTcbSize();
#elif defined(__s390__)
  descr_addr = reinterpret_cast<uptr>(__builtin_thread_pointer());
#elif defined(__powerpc64__)
  // PPC64LE uses TLS variant I. The thread pointer (in GPR 13)
  // points to the end of the TCB + 0x7000. The pthread_descr structure is
  // immediately in front of the TCB. TlsPreTcbSize() includes the size of the
  // TCB and the size of pthread_descr.
  const uptr kTlsTcbOffset = 0x7000;
  uptr thread_pointer;
  asm("addi %0,13,%1" : "=r"(thread_pointer) : "I"(-kTlsTcbOffset));
  descr_addr = thread_pointer - TlsPreTcbSize();
#else
#error "unsupported CPU arch"
#endif
  return descr_addr;
}
#endif  // (x86_64 || i386 || MIPS) && SANITIZER_LINUX

#if SANITIZER_FREEBSD
static void **ThreadSelfSegbase() {
  void **segbase = 0;
#if defined(__i386__)
  // sysarch(I386_GET_GSBASE, segbase);
  __asm __volatile("mov %%gs:0, %0" : "=r" (segbase));
#elif defined(__x86_64__)
  // sysarch(AMD64_GET_FSBASE, segbase);
  __asm __volatile("movq %%fs:0, %0" : "=r"(segbase));
#elif defined(__mips64__)
  // MIPS uses TLS variant I. The thread pointer (in hardware register $29)
  // points to the end of the TCB + 0x7000.  The TCB contains two pointers,
  // one to the dtv and the second to the struct pthread.
  const uptr kTlsTcbOffset = 0x7000;
  const uptr kTlsTcbSize = sizeof(void *) * 2;
  uptr thread_pointer;
#ifdef __CHERI_PURE_CAPABILITY__
  asm volatile("creadhwr\t%0, $chwr_userlocal" : "=C"(thread_pointer));
#else
  asm volatile(
      ".set push;\
                .set mips64r2;\
                rdhwr %0,$29;\
                .set pop"
      : "=r"(thread_pointer));
#endif
  segbase = (void **)((char *)thread_pointer - kTlsTcbOffset - kTlsTcbSize);
#else
#error "unsupported CPU arch"
#endif
  return segbase;
}

uptr ThreadSelf() {
#if defined(__mips__)
  return (uptr)ThreadSelfSegbase()[1];
#else
  return (uptr)ThreadSelfSegbase()[2];
#endif
}
#endif  // SANITIZER_FREEBSD

#if SANITIZER_NETBSD
static struct tls_tcb * ThreadSelfTlsTcb() {
  struct tls_tcb *tcb = nullptr;
#ifdef __HAVE___LWP_GETTCB_FAST
  tcb = (struct tls_tcb *)__lwp_gettcb_fast();
#elif defined(__HAVE___LWP_GETPRIVATE_FAST)
  tcb = (struct tls_tcb *)__lwp_getprivate_fast();
#endif
  return tcb;
}

uptr ThreadSelf() { return (uptr)ThreadSelfTlsTcb()->tcb_pthread; }
#endif  // SANITIZER_NETBSD

#if SANITIZER_NETBSD || (SANITIZER_FREEBSD && defined(__mips__))
int GetSizeFromHdr(struct dl_phdr_info *info, size_t size, void *data) {
  const Elf_Phdr *hdr = info->dlpi_phdr;
  const Elf_Phdr *last_hdr = hdr + info->dlpi_phnum;

  for (; hdr != last_hdr; ++hdr) {
    if (hdr->p_type == PT_TLS && info->dlpi_tls_modid == 1) {
      *(uptr*)data = hdr->p_memsz;
      break;
    }
  }
  return 0;
}
#endif  // SANITIZER_NETBSD || (SANITIZER_FREEBSD && defined(__mips__))

#if SANITIZER_ANDROID
// Bionic provides this API since S.
extern "C" SANITIZER_WEAK_ATTRIBUTE void __libc_get_static_tls_bounds(void **,
                                                                      void **);
#endif

#if !SANITIZER_GO
static void GetTls(uptr *addr, usize *size) {
#if SANITIZER_ANDROID
  if (&__libc_get_static_tls_bounds) {
    void *start_addr;
    void *end_addr;
    __libc_get_static_tls_bounds(&start_addr, &end_addr);
    *addr = reinterpret_cast<uptr>(start_addr);
    *size = static_cast<char *>(end_addr) - static_cast<char *>(start_addr);
  } else {
    *addr = 0;
    *size = 0;
  }
#elif SANITIZER_LINUX
#if defined(__x86_64__) || defined(__i386__) || defined(__s390__)
  *addr = ThreadSelf();
  *size = GetTlsSize();
  *addr -= *size;
  *addr += ThreadDescriptorSize();
#elif defined(__mips__) || defined(__aarch64__) || defined(__powerpc64__) || \
    defined(__arm__) || SANITIZER_RISCV64
  *addr = ThreadSelf();
  *size = GetTlsSize();
#else
  *addr = 0;
  *size = 0;
#endif
#elif SANITIZER_FREEBSD
  void** segbase = ThreadSelfSegbase();
  *addr = 0;
  *size = 0;
  if (segbase != 0) {
#if defined(__mips__)
    // Variant I
    //
    // dtv = segbase[0];
    // dtv[2] = base of TLS block of the main program
    void **dtv = (void **)segbase[0];
    if ((uptr)dtv[1] >= 2) {
      // Find size (p_memsz) of TLS block of the main program.
      dl_iterate_phdr(GetSizeFromHdr, size);

      if (*size != 0)
        *addr = (uptr)dtv[2];
    }
#else
    // Variant II
    //
    // tcbalign = 16
    // tls_size = round(tls_static_space, tcbalign);
    // dtv = segbase[1];
    // dtv[2] = segbase - tls_static_space;
    void **dtv = (void **)segbase[1];
    *addr = (uptr)dtv[2];
    *size = (*addr == 0) ? 0 : ((uptr)segbase[0] - (uptr)dtv[2]);
#endif
  }
#elif SANITIZER_NETBSD
  struct tls_tcb * const tcb = ThreadSelfTlsTcb();
  *addr = 0;
  *size = 0;
  if (tcb != 0) {
    // Find size (p_memsz) of dlpi_tls_modid 1 (TLS block of the main program).
    // ld.elf_so hardcodes the index 1.
    dl_iterate_phdr(GetSizeFromHdr, size);

    if (*size != 0) {
      // The block has been found and tcb_dtv[1] contains the base address
      *addr = (uptr)tcb->tcb_dtv[1];
    }
  }
#elif SANITIZER_SOLARIS
  // FIXME
  *addr = 0;
  *size = 0;
#else
#error "Unknown OS"
#endif
}
#endif

#if !SANITIZER_GO
usize GetTlsSize() {
#if SANITIZER_FREEBSD || SANITIZER_ANDROID || SANITIZER_NETBSD || \
    SANITIZER_SOLARIS
  uptr addr;
  usize size;
  GetTls(&addr, &size);
  return size;
#elif SANITIZER_GLIBC
#if defined(__mips__) || defined(__powerpc64__) || SANITIZER_RISCV64
  return RoundUpTo(g_tls_size + TlsPreTcbSize(), 16);
#else
  return g_tls_size;
#endif
#else
  return 0;
#endif
}
#endif

void GetThreadStackAndTls(bool main, uptr *stk_addr, usize *stk_size,
                          uptr *tls_addr, usize *tls_size) {
#if SANITIZER_GO
  // Stub implementation for Go.
  *stk_addr = *stk_size = *tls_addr = *tls_size = 0;
#else
  GetTls(tls_addr, tls_size);

  uptr stack_top, stack_bottom;
  GetThreadStackTopAndBottom(main, &stack_top, &stack_bottom);
  *stk_addr = stack_bottom;
  *stk_size = (char *)stack_top - (char *)stack_bottom;

  if (!main) {
    // If stack and tls intersect, make them non-intersecting.
    if (*tls_addr > *stk_addr && *tls_addr < *stk_addr + *stk_size) {
      CHECK_GT(*tls_addr + *tls_size, *stk_addr);
      CHECK_LE(*tls_addr + *tls_size, *stk_addr + *stk_size);
      *stk_size -= *tls_size;
      *tls_addr = *stk_addr + *stk_size;
    }
  }
#endif
}

#if !SANITIZER_FREEBSD
typedef ElfW(Phdr) Elf_Phdr;
#elif SANITIZER_WORDSIZE == 32 && __FreeBSD_version <= 902001  // v9.2
#define Elf_Phdr XElf32_Phdr
#define dl_phdr_info xdl_phdr_info
#define dl_iterate_phdr(c, b) xdl_iterate_phdr((c), (b))
#endif  // !SANITIZER_FREEBSD

struct DlIteratePhdrData {
  InternalMmapVectorNoCtor<LoadedModule> *modules;
  bool first;
};

static int AddModuleSegments(const char *module_name, dl_phdr_info *info,
                             InternalMmapVectorNoCtor<LoadedModule> *modules) {
  if (module_name[0] == '\0')
    return 0;
  LoadedModule cur_module;
  cur_module.set(module_name, info->dlpi_addr);
  for (int i = 0; i < (int)info->dlpi_phnum; i++) {
    const Elf_Phdr *phdr = &info->dlpi_phdr[i];
    if (phdr->p_type == PT_LOAD) {
      uptr cur_beg = info->dlpi_addr + phdr->p_vaddr;
      uptr cur_end = cur_beg + phdr->p_memsz;
      bool executable = phdr->p_flags & PF_X;
      bool writable = phdr->p_flags & PF_W;
      cur_module.addAddressRange(cur_beg, cur_end, executable,
                                 writable);
    }
  }
  modules->push_back(cur_module);
  return 0;
}

static int dl_iterate_phdr_cb(dl_phdr_info *info, size_t size, void *arg) {
  DlIteratePhdrData *data = (DlIteratePhdrData *)arg;
  if (data->first) {
    InternalMmapVector<char> module_name(kMaxPathLength);
    data->first = false;
    // First module is the binary itself.
    ReadBinaryNameCached(module_name.data(), module_name.size());
    return AddModuleSegments(module_name.data(), info, data->modules);
  }

  if (info->dlpi_name) {
    InternalScopedString module_name;
    module_name.append("%s", info->dlpi_name);
    return AddModuleSegments(module_name.data(), info, data->modules);
  }

  return 0;
}

#if SANITIZER_ANDROID && __ANDROID_API__ < 21
extern "C" __attribute__((weak)) int dl_iterate_phdr(
    int (*)(struct dl_phdr_info *, size_t, void *), void *);
#endif

static bool requiresProcmaps() {
#if SANITIZER_ANDROID && __ANDROID_API__ <= 22
  // Fall back to /proc/maps if dl_iterate_phdr is unavailable or broken.
  // The runtime check allows the same library to work with
  // both K and L (and future) Android releases.
  return AndroidGetApiLevel() <= ANDROID_LOLLIPOP_MR1;
#else
  return false;
#endif
}

static void procmapsInit(InternalMmapVectorNoCtor<LoadedModule> *modules) {
  MemoryMappingLayout memory_mapping(/*cache_enabled*/true);
  memory_mapping.DumpListOfModules(modules);
}

void ListOfModules::init() {
  clearOrInit();
  if (requiresProcmaps()) {
    procmapsInit(&modules_);
  } else {
    DlIteratePhdrData data = {&modules_, true};
    dl_iterate_phdr(dl_iterate_phdr_cb, &data);
  }
}

// When a custom loader is used, dl_iterate_phdr may not contain the full
// list of modules. Allow callers to fall back to using procmaps.
void ListOfModules::fallbackInit() {
  if (!requiresProcmaps()) {
    clearOrInit();
    procmapsInit(&modules_);
  } else {
    clear();
  }
}

// getrusage does not give us the current RSS, only the max RSS.
// Still, this is better than nothing if /proc/self/statm is not available
// for some reason, e.g. due to a sandbox.
static usize GetRSSFromGetrusage() {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage))  // Failed, probably due to a sandbox.
    return 0;
  return usage.ru_maxrss << 10;  // ru_maxrss is in Kb.
}

usize GetRSS() {
  if (!common_flags()->can_use_proc_maps_statm)
    return GetRSSFromGetrusage();
  fd_t fd = OpenFile("/proc/self/statm", RdOnly);
  if (fd == kInvalidFd)
    return GetRSSFromGetrusage();
  char buf[64];
  uptr len = internal_read(fd, buf, sizeof(buf) - 1);
  internal_close(fd);
  if ((sptr)len <= 0)
    return 0;
  buf[len] = 0;
  // The format of the file is:
  // 1084 89 69 11 0 79 0
  // We need the second number which is RSS in pages.
  char *pos = buf;
  // Skip the first number.
  while (*pos >= '0' && *pos <= '9')
    pos++;
  // Skip whitespaces.
  while (!(*pos >= '0' && *pos <= '9') && *pos != 0)
    pos++;
  // Read the number.
  uptr rss = 0;
  while (*pos >= '0' && *pos <= '9')
    rss = rss * 10 + *pos++ - '0';
  return rss * GetPageSizeCached();
}

// sysconf(_SC_NPROCESSORS_{CONF,ONLN}) cannot be used on most platforms as
// they allocate memory.
u32 GetNumberOfCPUs() {
#if SANITIZER_FREEBSD || SANITIZER_NETBSD
  u32 ncpu;
  int req[2];
  usize len = sizeof(ncpu);
  req[0] = CTL_HW;
  req[1] = HW_NCPU;
  CHECK_EQ(internal_sysctl(req, 2, &ncpu, &len, NULL, 0), 0);
  return ncpu;
#elif SANITIZER_ANDROID && !defined(CPU_COUNT) && !defined(__aarch64__)
  // Fall back to /sys/devices/system/cpu on Android when cpu_set_t doesn't
  // exist in sched.h. That is the case for toolchains generated with older
  // NDKs.
  // This code doesn't work on AArch64 because internal_getdents makes use of
  // the 64bit getdents syscall, but cpu_set_t seems to always exist on AArch64.
  fd_t fd = internal_open("/sys/devices/system/cpu", O_RDONLY | O_DIRECTORY);
  if (internal_iserror(fd))
    return 0;
  InternalMmapVector<u8> buffer(4096);
  usize bytes_read = buffer.size();
  usize n_cpus = 0;
  u8 *d_type;
  struct linux_dirent *entry = (struct linux_dirent *)&buffer[bytes_read];
  while (true) {
    if ((u8 *)entry >= &buffer[bytes_read]) {
      bytes_read = internal_getdents(fd, (struct linux_dirent *)buffer.data(),
                                     buffer.size());
      if (internal_iserror(bytes_read) || !bytes_read)
        break;
      entry = (struct linux_dirent *)buffer.data();
    }
    d_type = (u8 *)entry + entry->d_reclen - 1;
    if (d_type >= &buffer[bytes_read] ||
        (u8 *)&entry->d_name[3] >= &buffer[bytes_read])
      break;
    if (entry->d_ino != 0 && *d_type == DT_DIR) {
      if (entry->d_name[0] == 'c' && entry->d_name[1] == 'p' &&
          entry->d_name[2] == 'u' &&
          entry->d_name[3] >= '0' && entry->d_name[3] <= '9')
        n_cpus++;
    }
    entry = (struct linux_dirent *)(((u8 *)entry) + entry->d_reclen);
  }
  internal_close(fd);
  return n_cpus;
#elif SANITIZER_SOLARIS
  return sysconf(_SC_NPROCESSORS_ONLN);
#else
  cpu_set_t CPUs;
  CHECK_EQ(sched_getaffinity(0, sizeof(cpu_set_t), &CPUs), 0);
  return CPU_COUNT(&CPUs);
#endif
}

#if SANITIZER_LINUX

#if SANITIZER_ANDROID
static atomic_uint8_t android_log_initialized;

void AndroidLogInit() {
  openlog(GetProcessName(), 0, LOG_USER);
  atomic_store(&android_log_initialized, 1, memory_order_release);
}

static bool ShouldLogAfterPrintf() {
  return atomic_load(&android_log_initialized, memory_order_acquire);
}

extern "C" SANITIZER_WEAK_ATTRIBUTE
int async_safe_write_log(int pri, const char* tag, const char* msg);
extern "C" SANITIZER_WEAK_ATTRIBUTE
int __android_log_write(int prio, const char* tag, const char* msg);

// ANDROID_LOG_INFO is 4, but can't be resolved at runtime.
#define SANITIZER_ANDROID_LOG_INFO 4

// async_safe_write_log is a new public version of __libc_write_log that is
// used behind syslog. It is preferable to syslog as it will not do any dynamic
// memory allocation or formatting.
// If the function is not available, syslog is preferred for L+ (it was broken
// pre-L) as __android_log_write triggers a racey behavior with the strncpy
// interceptor. Fallback to __android_log_write pre-L.
void WriteOneLineToSyslog(const char *s) {
  if (&async_safe_write_log) {
    async_safe_write_log(SANITIZER_ANDROID_LOG_INFO, GetProcessName(), s);
  } else if (AndroidGetApiLevel() > ANDROID_KITKAT) {
    syslog(LOG_INFO, "%s", s);
  } else {
    CHECK(&__android_log_write);
    __android_log_write(SANITIZER_ANDROID_LOG_INFO, nullptr, s);
  }
}

extern "C" SANITIZER_WEAK_ATTRIBUTE
void android_set_abort_message(const char *);

void SetAbortMessage(const char *str) {
  if (&android_set_abort_message)
    android_set_abort_message(str);
}
#else
void AndroidLogInit() {}

static bool ShouldLogAfterPrintf() { return true; }

void WriteOneLineToSyslog(const char *s) { syslog(LOG_INFO, "%s", s); }

void SetAbortMessage(const char *str) {}
#endif  // SANITIZER_ANDROID

void LogMessageOnPrintf(const char *str) {
  if (common_flags()->log_to_syslog && ShouldLogAfterPrintf())
    WriteToSyslog(str);
}

#endif  // SANITIZER_LINUX

#if SANITIZER_GLIBC && !SANITIZER_GO
// glibc crashes when using clock_gettime from a preinit_array function as the
// vDSO function pointers haven't been initialized yet. __progname is
// initialized after the vDSO function pointers, so if it exists, is not null
// and is not empty, we can use clock_gettime.
extern "C" SANITIZER_WEAK_ATTRIBUTE char *__progname;
inline bool CanUseVDSO() { return &__progname && __progname && *__progname; }

// MonotonicNanoTime is a timing function that can leverage the vDSO by calling
// clock_gettime. real_clock_gettime only exists if clock_gettime is
// intercepted, so define it weakly and use it if available.
extern "C" SANITIZER_WEAK_ATTRIBUTE
int real_clock_gettime(u32 clk_id, void *tp);
u64 MonotonicNanoTime() {
  timespec ts;
  if (CanUseVDSO()) {
    if (&real_clock_gettime)
      real_clock_gettime(CLOCK_MONOTONIC, &ts);
    else
      clock_gettime(CLOCK_MONOTONIC, &ts);
  } else {
    internal_clock_gettime(CLOCK_MONOTONIC, &ts);
  }
  return (u64)ts.tv_sec * (1000ULL * 1000 * 1000) + ts.tv_nsec;
}
#else
// Non-glibc & Go always use the regular function.
u64 MonotonicNanoTime() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * (1000ULL * 1000 * 1000) + ts.tv_nsec;
}
#endif  // SANITIZER_GLIBC && !SANITIZER_GO

void ReExec() {
  const char *pathname = "/proc/self/exe";

#if SANITIZER_NETBSD
  static const int name[] = {
      CTL_KERN,
      KERN_PROC_ARGS,
      -1,
      KERN_PROC_PATHNAME,
  };
  char path[400];
  usize len;

  len = sizeof(path);
  if (internal_sysctl(name, ARRAY_SIZE(name), path, &len, NULL, 0) != -1)
    pathname = path;
#elif SANITIZER_SOLARIS
  pathname = getexecname();
  CHECK_NE(pathname, NULL);
#elif SANITIZER_USE_GETAUXVAL
  // Calling execve with /proc/self/exe sets that as $EXEC_ORIGIN. Binaries that
  // rely on that will fail to load shared libraries. Query AT_EXECFN instead.
  pathname = reinterpret_cast<const char *>(getauxval(AT_EXECFN));
#endif

  uptr rv = internal_execve(pathname, GetArgv(), GetEnviron());
  int rverrno;
  CHECK_EQ(internal_iserror(rv, &rverrno), true);
  Printf("execve failed, errno %d\n", rverrno);
  Die();
}

void UnmapFromTo(uptr from, uptr to) {
  if (to == from)
    return;
  CHECK(to >= from);
  uptr res = internal_munmap(reinterpret_cast<void *>(from), to - from);
  if (UNLIKELY(internal_iserror(res))) {
    Report("ERROR: %s failed to unmap 0x%zx (%zd) bytes at address %p\n",
           SanitizerToolName, to - from, to - from, (void *)from);
    CHECK("unable to unmap" && 0);
  }
}

uptr MapDynamicShadow(uptr shadow_size_bytes, uptr shadow_scale,
                      uptr min_shadow_base_alignment,
                      UNUSED uptr &high_mem_end) {
  const uptr granularity = GetMmapGranularity();
  const uptr alignment =
      Max<uptr>(granularity << shadow_scale, 1ULL << min_shadow_base_alignment);
  const uptr left_padding =
      Max<uptr>(granularity, 1ULL << min_shadow_base_alignment);

  const uptr shadow_size = RoundUpTo(shadow_size_bytes, granularity);
  const uptr map_size = shadow_size + left_padding + alignment;

  const uptr map_start = (uptr)MmapNoAccess(map_size);
  CHECK_NE(map_start, ~(uptr)0);

  const uptr shadow_start = RoundUpTo(map_start + left_padding, alignment);

  UnmapFromTo(map_start, shadow_start - left_padding);
  UnmapFromTo(shadow_start + shadow_size, map_start + map_size);

  return shadow_start;
}

static uptr MmapSharedNoReserve(uptr addr, uptr size) {
  return internal_mmap(
      reinterpret_cast<void *>(addr), size, PROT_READ | PROT_WRITE,
      MAP_FIXED | MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

static uptr MremapCreateAlias(uptr base_addr, uptr alias_addr,
                              uptr alias_size) {
#if SANITIZER_LINUX
  return internal_mremap(reinterpret_cast<void *>(base_addr), 0, alias_size,
                         MREMAP_MAYMOVE | MREMAP_FIXED,
                         reinterpret_cast<void *>(alias_addr));
#else
  CHECK(false && "mremap is not supported outside of Linux");
#endif
}

static void CreateAliases(uptr start_addr, uptr alias_size, uptr num_aliases) {
  uptr total_size = alias_size * num_aliases;
  uptr mapped = MmapSharedNoReserve(start_addr, total_size);
  CHECK_EQ(mapped, start_addr);

  for (uptr i = 1; i < num_aliases; ++i) {
    uptr alias_addr = start_addr + i * alias_size;
    CHECK_EQ(MremapCreateAlias(start_addr, alias_addr, alias_size), alias_addr);
  }
}

uptr MapDynamicShadowAndAliases(uptr shadow_size, uptr alias_size,
                                uptr num_aliases, uptr ring_buffer_size) {
  CHECK_EQ(alias_size & (alias_size - 1), 0);
  CHECK_EQ(num_aliases & (num_aliases - 1), 0);
  CHECK_EQ(ring_buffer_size & (ring_buffer_size - 1), 0);

  const uptr granularity = GetMmapGranularity();
  shadow_size = RoundUpTo(shadow_size, granularity);
  CHECK_EQ(shadow_size & (shadow_size - 1), 0);

  const uptr alias_region_size = alias_size * num_aliases;
  const uptr alignment =
      2 * Max(Max(shadow_size, alias_region_size), ring_buffer_size);
  const uptr left_padding = ring_buffer_size;

  const uptr right_size = alignment;
  const uptr map_size = left_padding + 2 * alignment;

  const uptr map_start = reinterpret_cast<uptr>(MmapNoAccess(map_size));
  CHECK_NE(map_start, static_cast<uptr>(-1));
  const uptr right_start = RoundUpTo(map_start + left_padding, alignment);

  UnmapFromTo(map_start, right_start - left_padding);
  UnmapFromTo(right_start + right_size, map_start + map_size);

  CreateAliases(right_start + right_size / 2, alias_size, num_aliases);

  return right_start;
}

void InitializePlatformCommonFlags(CommonFlags *cf) {
#if SANITIZER_ANDROID
  if (&__libc_get_static_tls_bounds == nullptr)
    cf->detect_leaks = false;
#endif
}

} // namespace __sanitizer

#endif
