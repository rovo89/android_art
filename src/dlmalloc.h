// Copyright 2012 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DLMALLOC_H_
#define ART_SRC_DLMALLOC_H_

#define NO_MALLINFO 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define HAVE_MORECORE 1
#define MSPACES 1
#define ONLY_MSPACES 1
#define USE_DL_PREFIX 1
#define MALLOC_INSPECT_ALL 1

// Only #include if we are not compiling dlmalloc.c (to avoid symbol redefinitions)
#ifndef FOR_DLMALLOC_C
#include "dlmalloc/malloc.h"
#endif

#endif  // ART_SRC_DLMALLOC_H_
