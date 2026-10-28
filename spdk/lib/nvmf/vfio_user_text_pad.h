/* SPDX-License-Identifier: BSD-3-Clause
 * IAU-next P4.4 (review I1): VU_TEXT_PAD64() pads this object's .text to a multiple of 64 B, so every object linked
 * after it in libspdk_nvmf (and the libraries after it in nvmf_tgt) keeps its 64 B phase. Use it once per
 * translation unit, at file scope. GCC emits top-level asm before the functions (-ftoplevel-reorder), so the
 * begin label sits at offset 0 of .text; the padding goes to .text subsection 1, which the assembler appends after
 * all of the compiler's code (subsection 0) and sizes at the end of assembly. The code itself is unchanged.
 * Other compilers: no padding (tests/realcore/test_nvmf_hot_layout.sh checks the GCC build).
 */
#ifndef SPDK_VFIO_USER_TEXT_PAD_H
#define SPDK_VFIO_USER_TEXT_PAD_H

#if defined(__GNUC__) && !defined(__clang__)
#define VU_TEXT_PAD64() \
	__asm__(".pushsection .text,\"ax\",@progbits\n.Lvu_text_pad64_begin:\n\t.popsection\n" \
		".pushsection .text,\"ax\",@progbits\n\t.subsection 1\n" \
		"\t.skip (-(. - .Lvu_text_pad64_begin)) & 63, 0xcc\n\t.popsection")
#else
#define VU_TEXT_PAD64() struct vu_text_pad64_unused
#endif

#endif /* SPDK_VFIO_USER_TEXT_PAD_H */
