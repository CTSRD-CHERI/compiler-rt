set(ARM64 aarch64)
set(ARM32 arm armhf)
set(HEXAGON hexagon)
set(X86 i386)
set(X86_64 x86_64)
set(MIPS32 mips mipsel)
set(MIPS64 mips64 mips64el)
set(PPC32 powerpc)
set(PPC64 powerpc64 powerpc64le)
set(RISCV32 riscv32)
set(RISCV64 riscv64)
set(S390X s390x)
set(SPARC sparc)
set(SPARCV9 sparcv9)
set(WASM32 wasm32)
set(WASM64 wasm64)
set(VE ve)

if(APPLE)
  set(ARM64 arm64)
  set(ARM32 armv7 armv7s armv7k)
  set(X86_64 x86_64 x86_64h)
endif()

set(ALL_SANITIZER_COMMON_SUPPORTED_ARCH ${X86} ${X86_64} ${PPC64} ${RISCV64}
    ${ARM32} ${ARM64} ${MIPS32} ${MIPS64} ${S390X} ${SPARC} ${SPARCV9}
    ${HEXAGON})
set(ALL_ASAN_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM32} ${ARM64} ${RISCV64}
    ${MIPS32} ${MIPS64} ${PPC64} ${S390X} ${SPARC} ${SPARCV9} ${HEXAGON})
set(ALL_CRT_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM32} ${ARM64} ${PPC32}
    ${PPC64} ${RISCV32} ${RISCV64} ${VE} ${HEXAGON})
set(ALL_DFSAN_SUPPORTED_ARCH ${X86_64} ${MIPS64} ${ARM64})

if(ANDROID)
  set(OS_NAME "Android")
else()
  set(OS_NAME "${CMAKE_SYSTEM_NAME}")
endif()

if(OS_NAME MATCHES "Linux")
  set(ALL_FUZZER_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM64} ${S390X})
elseif (OS_NAME MATCHES "Windows")
  set(ALL_FUZZER_SUPPORTED_ARCH ${X86} ${X86_64})
elseif(OS_NAME MATCHES "Android")
  set(ALL_FUZZER_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM32} ${ARM64})
else()
  set(ALL_FUZZER_SUPPORTED_ARCH ${X86_64} ${ARM64})
endif()

set(ALL_GWP_ASAN_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM32} ${ARM64})
if(APPLE)
  set(ALL_LSAN_SUPPORTED_ARCH ${X86} ${X86_64} ${MIPS64} ${ARM64})
else()
  set(ALL_LSAN_SUPPORTED_ARCH ${X86} ${X86_64} ${MIPS64} ${ARM64} ${ARM32}
      ${PPC64} ${S390X} ${RISCV64} ${HEXAGON})
endif()
set(ALL_MSAN_SUPPORTED_ARCH ${X86_64} ${MIPS64} ${ARM64} ${PPC64} ${S390X})
set(ALL_HWASAN_SUPPORTED_ARCH ${X86_64} ${ARM64})
set(ALL_MEMPROF_SUPPORTED_ARCH ${X86_64})
set(ALL_PROFILE_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM32} ${ARM64} ${PPC32} ${PPC64}
    ${MIPS32} ${MIPS64} ${S390X} ${SPARC} ${SPARCV9} ${HEXAGON})
set(ALL_TSAN_SUPPORTED_ARCH ${X86_64} ${MIPS64} ${ARM64} ${PPC64} ${S390X})
set(ALL_UBSAN_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM32} ${ARM64} ${RISCV64}
    ${MIPS32} ${MIPS64} ${PPC64} ${S390X} ${SPARC} ${SPARCV9} ${HEXAGON})
set(ALL_SAFESTACK_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM64} ${MIPS32} ${MIPS64}
    ${HEXAGON})
set(ALL_CFI_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM32} ${ARM64} ${MIPS64}
    ${HEXAGON})
set(ALL_SCUDO_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM32} ${ARM64} ${MIPS32}
    ${MIPS64} ${PPC64} ${HEXAGON})
set(ALL_SCUDO_STANDALONE_SUPPORTED_ARCH ${X86} ${X86_64} ${ARM32} ${ARM64}
    ${MIPS32} ${MIPS64} ${PPC64} ${HEXAGON})
if(APPLE)
set(ALL_XRAY_SUPPORTED_ARCH ${X86_64})
else()
set(ALL_XRAY_SUPPORTED_ARCH ${X86_64} ${ARM32} ${ARM64} ${MIPS32} ${MIPS64} powerpc64le)
endif()
set(ALL_SHADOWCALLSTACK_SUPPORTED_ARCH ${ARM64})

if (UNIX)
set(ALL_ORC_SUPPORTED_ARCH ${X86_64} ${ARM64} ${ARM32})
endif()
