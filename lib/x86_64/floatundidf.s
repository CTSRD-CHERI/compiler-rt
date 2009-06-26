//===-- floatundidf.s - Implement __floatundidf for x86_64 ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements __floatundidf for the compiler_rt library.
//
//===----------------------------------------------------------------------===//

// double __floatundidf(du_int a);

#ifdef __x86_64__

.const
.align 4
twop52: .quad 0x4330000000000000
twop84_plus_twop52:
		.quad 0x4530000000100000
twop84: .quad 0x4530000000000000

#define REL_ADDR(_a)	(_a)(%rip)

.text
.align 4
.globl ___floatundidf
___floatundidf:
	movd	%edi,							%xmm0 // low 32 bits of a
	shrq	$32,							%rdi  // high 32 bits of a
	orq		REL_ADDR(twop84),				%rdi  // 0x1p84 + a_hi (no rounding occurs)
	orpd	REL_ADDR(twop52),				%xmm0 // 0x1p52 + a_lo (no rounding occurs)
	movd	%rdi,							%xmm1
	subsd	REL_ADDR(twop84_plus_twop52),	%xmm1 // a_hi - 0x1p52 (no rounding occurs)
	addsd	%xmm1,							%xmm0 // a_hi + a_lo   (round happens here)
	ret
	
#endif // __x86_64__