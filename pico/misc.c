/*
 * rarely used EEPROM code
 * (C) notaz, 2006-2008
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include "pico_int.h"


#ifndef _ASM_MISC_C
PICO_INTERNAL_ASM void memcpy16bswap(unsigned short *dest, void *src, int count)
{
	unsigned char *src_ = src;

	for (; count; count--, src_ += 2)
		*dest++ = (src_[0] << 8) | src_[1];
}

#ifndef _ASM_MISC_C_AMIPS
#if defined(RENDER_GSKIT_PS2)
/* AURORA_PD_MD_PERF_V7_20260822
 *
 * R5900 bulk fill. Preserve the original memset32 contract exactly.
 * SQ silently aligns addresses down on the EE, so scalar-align first.
 * Two SQ stores replace eight scalar SW stores in the common body.
 */
PICO_INTERNAL_ASM void memset32(void *dest_in, int c, int count)
{
	int *dest = dest_in;
	u64 pat;

	if (count <= 0)
		return;

	/* Do not pay alignment/MMI setup on short utility fills. */
	if (count < 16)
		goto scalar;

	while (((uptr)dest & 15) && count > 0) {
		*dest++ = c;
		count--;
	}

	pat = (u32)c;
	pat |= pat << 32;

	while (count >= 8) {
		__asm__ __volatile__(
			"pcpyld %0, %0, %1\n"
			"sq %0, 0(%2)\n"
			"sq %0, 16(%2)\n"
			:
			: "r"(pat), "r"(pat), "r"(dest)
			: "memory");
		dest += 8;
		count -= 8;
	}

	if (count >= 4) {
		__asm__ __volatile__(
			"pcpyld %0, %0, %1\n"
			"sq %0, 0(%2)\n"
			:
			: "r"(pat), "r"(pat), "r"(dest)
			: "memory");
		dest += 4;
		count -= 4;
	}

scalar:
	for (; count >= 8; count -= 8, dest += 8)
		dest[0] = dest[1] = dest[2] = dest[3] =
		dest[4] = dest[5] = dest[6] = dest[7] = c;

	switch (count) {
		case 7: *dest++ = c;
		case 6: *dest++ = c;
		case 5: *dest++ = c;
		case 4: *dest++ = c;
		case 3: *dest++ = c;
		case 2: *dest++ = c;
		case 1: *dest++ = c;
	}
}
#else
PICO_INTERNAL_ASM void memset32(void *dest_in, int c, int count)
{
	int *dest = dest_in;

	for (; count >= 8; count -= 8, dest += 8)
		dest[0] = dest[1] = dest[2] = dest[3] =
		dest[4] = dest[5] = dest[6] = dest[7] = c;

	switch (count) {
		case 7: *dest++ = c;
		case 6: *dest++ = c;
		case 5: *dest++ = c;
		case 4: *dest++ = c;
		case 3: *dest++ = c;
		case 2: *dest++ = c;
		case 1: *dest++ = c;
	}
}
#endif
void memset32_uncached(int *dest, int c, int count) { memset32(dest, c, count); }
#endif
#endif

