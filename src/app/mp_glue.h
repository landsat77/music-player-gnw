/*
 * Forced include (-include) of libxmp-lite: its allocations use the heap of
 * the player (mp_alloc.c), which spans the memory regions of the console.
 */
#ifndef MP_GLUE_H
#define MP_GLUE_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "mp_platform.h"

#define malloc(n)     mp_malloc(n)
#define calloc(n, s)  mp_calloc(n, s)
#define realloc(p, n) mp_realloc(p, n)
#define free(p)       mp_free(p)

#endif
