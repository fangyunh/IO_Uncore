#ifndef _RTE_MEMCPY_X86_64_H_
#define _RTE_MEMCPY_X86_64_H_
#define GEM5_GENERIC_MEMCPY 1
#include <string.h>
#include <rte_common.h>
#ifdef __cplusplus
extern "C" {
#endif
static __rte_always_inline void *
rte_memcpy(void *dst, const void *src, size_t n)
{
	return memcpy(dst, src, n);
}
static __rte_always_inline void *
rte_memcpy_aligned(void *dst, const void *src, size_t n)
{ return memcpy(dst, src, n); }
#ifdef __cplusplus
}
#endif
#endif
