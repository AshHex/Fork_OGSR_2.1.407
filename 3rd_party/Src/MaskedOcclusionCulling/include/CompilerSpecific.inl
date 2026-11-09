////////////////////////////////////////////////////////////////////////////////
// Compiler/OS specific helpers for Intel Masked Occlusion Culling.
//
// The upstream file (GameTechDev/MaskedOcclusionCulling, Apache-2.0) was
// missing from this source drop; this is a minimal equivalent providing the
// symbols the library uses: __MICROSOFT_COMPILER, FORCE_INLINE,
// find_clear_lsb, aligned_alloc and aligned_free.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <new> // placement new used by the Create*() factories

#if defined(_MSC_VER) && !defined(__clang__)
	#define __MICROSOFT_COMPILER
#endif

#if defined(_WIN32) && (defined(_MSC_VER) || defined(__INTEL_COMPILER) || defined(__clang__))
	#include <intrin.h>
	#include <malloc.h>

	#define FORCE_INLINE __forceinline

	// Return index of the lowest set bit and clear it from the mask.
	static FORCE_INLINE unsigned long find_clear_lsb(unsigned int *mask)
	{
		unsigned long idx;
		_BitScanForward(&idx, *mask);
		*mask &= *mask - 1;
		return idx;
	}

	static FORCE_INLINE void *aligned_alloc(size_t alignment, size_t size)
	{
		return _aligned_malloc(size, alignment);
	}

	static FORCE_INLINE void aligned_free(void *ptr)
	{
		_aligned_free(ptr);
	}

#elif defined(__GNUG__) || defined(__clang__)
	#include <immintrin.h>
	#include <cpuid.h>
	#include <mm_malloc.h>

	#define FORCE_INLINE inline __attribute__((always_inline))

	static FORCE_INLINE unsigned long find_clear_lsb(unsigned int *mask)
	{
		unsigned long idx = (unsigned long)__builtin_ctz(*mask);
		*mask &= *mask - 1;
		return idx;
	}

	static FORCE_INLINE void *aligned_alloc(size_t alignment, size_t size)
	{
		return _mm_malloc(size, alignment);
	}

	static FORCE_INLINE void aligned_free(void *ptr)
	{
		_mm_free(ptr);
	}
#endif
