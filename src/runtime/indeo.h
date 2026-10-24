/* The videos' codec, lifted: indeo.c. docs/movies.md. */
#pragma once
#include "native32.h"

#define INDEO_BASE 0x10000000u   /* IR41_32.DLL's own base: its lift has its addresses built in */
#define INDEO_SPAN 0x00100000u   /* covers its 0xBE000 bytes */

void indeo_hold(void);           /* first thing in main: keep the range free for it */
/* Map, bind and start the codec, and install it with Video for Windows.
 * native: run the DLL's original machine code instead (the reference).
 * 0 when it is missing or does not start (the videos are then skipped). */
int indeo_start(const char* dll, native32_shim_t* shims, int nshims, int native);
/* --decode-test: every frame of an AVI's video as 24-bit RGB into a file. */
int indeo_decode_test(const char* avi, const char* out);
