// Copyright 2012 Google Inc. All Rights Reserved.
#define FOR_DLMALLOC_C  // Avoid inclusion of src/malloc.h
#include "dlmalloc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Disable GCC diagnostics so that -Werror won't fail
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wempty-body"

// ART specific morecore implementation
#define MORECORE(x) art_heap_morecore(m, x)
extern void* art_heap_morecore(void* m, intptr_t increment);

// Ugly inclusion of C file so that ART specific #defines configure dlmalloc
#include "dlmalloc/malloc.c"
