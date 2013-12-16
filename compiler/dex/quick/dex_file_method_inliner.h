/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_DEX_QUICK_DEX_FILE_METHOD_INLINER_H_
#define ART_COMPILER_DEX_QUICK_DEX_FILE_METHOD_INLINER_H_

#include <stdint.h>
#include <map>
#include "base/mutex.h"
#include "base/macros.h"
#include "locks.h"

namespace art {

class CallInfo;
class DexFile;
class Mir2Lir;

enum IntrinsicOpcode {
  kIntrinsicDoubleCvt,
  kIntrinsicFloatCvt,
  kIntrinsicReverseBytes,
  kIntrinsicAbsInt,
  kIntrinsicAbsLong,
  kIntrinsicMinMaxInt,
  kIntrinsicSqrt,
  kIntrinsicCharAt,
  kIntrinsicCompareTo,
  kIntrinsicIsEmptyOrLength,
  kIntrinsicIndexOf,
  kIntrinsicCurrentThread,
  kIntrinsicPeek,
  kIntrinsicPoke,
  kIntrinsicCas,
  kIntrinsicUnsafeGet,
  kIntrinsicUnsafePut,
};

enum IntrinsicFlags {
  kIntrinsicFlagNone = 0,

  // kIntrinsicMinMaxInt
  kIntrinsicFlagMax = kIntrinsicFlagNone,
  kIntrinsicFlagMin = 1,

  // kIntrinsicIsEmptyOrLength
  kIntrinsicFlagLength  = kIntrinsicFlagNone,
  kIntrinsicFlagIsEmpty = 1,

  // kIntrinsicIndexOf
  kIntrinsicFlagBase0 = 1,

  // kIntrinsicUnsafeGet, kIntrinsicUnsafePut, kIntrinsicUnsafeCas
  kIntrinsicFlagIsLong     = 1,
  // kIntrinsicUnsafeGet, kIntrinsicUnsafePut
  kIntrinsicFlagIsVolatile = 2,
  // kIntrinsicUnsafePut, kIntrinsicUnsafeCas
  kIntrinsicFlagIsObject   = 4,
  // kIntrinsicUnsafePut
  kIntrinsicFlagIsOrdered  = 8,
};

struct Intrinsic {
  IntrinsicOpcode opcode;
  uint32_t data;
};

/**
 * Handles inlining of methods from a particular DexFile.
 *
 * Intrinsics are a special case of inline methods. The DexFile indices for
 * all the supported intrinsic methods are looked up once by the FindIntrinsics
 * function and cached by this class for quick lookup by the method index.
 *
 * TODO: Detect short methods (at least getters, setters and empty functions)
 * from the verifier and mark them for inlining. Inline these methods early
 * during compilation to allow further optimizations. Similarly, provide
 * additional information about intrinsics to the early phases of compilation.
 */
class DexFileMethodInliner {
  public:
    DexFileMethodInliner();
    ~DexFileMethodInliner();

    /**
     * Check whether a particular method index corresponds to an intrinsic function.
     */
    bool IsIntrinsic(uint32_t method_index) LOCKS_EXCLUDED(lock_);

    /**
     * Generate code for an intrinsic function invocation.
     */
    bool GenIntrinsic(Mir2Lir* backend, CallInfo* info) LOCKS_EXCLUDED(lock_);

  private:
    /**
     * To avoid multiple lookups of a class by its descriptor, we cache its
     * type index in the IndexCache. These are the indexes into the IndexCache
     * class_indexes array.
     */
    enum ClassCacheIndex : uint8_t {  // unit8_t to save space, make larger if needed
      kClassCacheFirst = 0,
      kClassCacheBoolean = kClassCacheFirst,
      kClassCacheByte,
      kClassCacheChar,
      kClassCacheShort,
      kClassCacheInt,
      kClassCacheLong,
      kClassCacheFloat,
      kClassCacheDouble,
      kClassCacheVoid,
      kClassCacheJavaLangObject,
      kClassCacheJavaLangString,
      kClassCacheJavaLangDouble,
      kClassCacheJavaLangFloat,
      kClassCacheJavaLangInteger,
      kClassCacheJavaLangLong,
      kClassCacheJavaLangShort,
      kClassCacheJavaLangMath,
      kClassCacheJavaLangStrictMath,
      kClassCacheJavaLangThread,
      kClassCacheLibcoreIoMemory,
      kClassCacheSunMiscUnsafe,
      kClassCacheLast
    };

    /**
     * To avoid multiple lookups of a method name string, we cache its string
     * index in the IndexCache. These are the indexes into the IndexCache
     * name_indexes array.
     */
    enum NameCacheIndex : uint8_t {  // unit8_t to save space, make larger if needed
      kNameCacheFirst = 0,
      kNameCacheReverseBytes = kNameCacheFirst,
      kNameCacheDoubleToRawLongBits,
      kNameCacheLongBitsToDouble,
      kNameCacheFloatToRawIntBits,
      kNameCacheIntBitsToFloat,
      kNameCacheAbs,
      kNameCacheMax,
      kNameCacheMin,
      kNameCacheSqrt,
      kNameCacheCharAt,
      kNameCacheCompareTo,
      kNameCacheIsEmpty,
      kNameCacheIndexOf,
      kNameCacheLength,
      kNameCacheCurrentThread,
      kNameCachePeekByte,
      kNameCachePeekIntNative,
      kNameCachePeekLongNative,
      kNameCachePeekShortNative,
      kNameCachePokeByte,
      kNameCachePokeIntNative,
      kNameCachePokeLongNative,
      kNameCachePokeShortNative,
      kNameCacheCompareAndSwapInt,
      kNameCacheCompareAndSwapLong,
      kNameCacheCompareAndSwapObject,
      kNameCacheGetInt,
      kNameCacheGetIntVolatile,
      kNameCachePutInt,
      kNameCachePutIntVolatile,
      kNameCachePutOrderedInt,
      kNameCacheGetLong,
      kNameCacheGetLongVolatile,
      kNameCachePutLong,
      kNameCachePutLongVolatile,
      kNameCachePutOrderedLong,
      kNameCacheGetObject,
      kNameCacheGetObjectVolatile,
      kNameCachePutObject,
      kNameCachePutObjectVolatile,
      kNameCachePutOrderedObject,
      kNameCacheLast
    };

    /**
     * To avoid multiple lookups of a method signature, we cache its proto
     * index in the IndexCache. These are the indexes into the IndexCache
     * proto_indexes array.
     */
    enum ProtoCacheIndex : uint8_t {  // unit8_t to save space, make larger if needed
      kProtoCacheFirst = 0,
      kProtoCacheI_I = kProtoCacheFirst,
      kProtoCacheJ_J,
      kProtoCacheS_S,
      kProtoCacheD_D,
      kProtoCacheD_J,
      kProtoCacheJ_D,
      kProtoCacheF_I,
      kProtoCacheI_F,
      kProtoCacheII_I,
      kProtoCacheI_C,
      kProtoCacheString_I,
      kProtoCache_Z,
      kProtoCache_I,
      kProtoCache_Thread,
      kProtoCacheJ_B,
      kProtoCacheJ_I,
      kProtoCacheJ_S,
      kProtoCacheJB_V,
      kProtoCacheJI_V,
      kProtoCacheJJ_V,
      kProtoCacheJS_V,
      kProtoCacheObjectJII_Z,
      kProtoCacheObjectJJJ_Z,
      kProtoCacheObjectJObjectObject_Z,
      kProtoCacheObjectJ_I,
      kProtoCacheObjectJI_V,
      kProtoCacheObjectJ_J,
      kProtoCacheObjectJJ_V,
      kProtoCacheObjectJ_Object,
      kProtoCacheObjectJObject_V,
      kProtoCacheLast
    };

    /**
     * The maximum number of method parameters we support in the ProtoDef.
     */
    static constexpr uint32_t kProtoMaxParams = 6;

    /**
     * The method signature (proto) definition using cached class indexes.
     * The return_type and params are used with the IndexCache to look up
     * appropriate class indexes to be passed to DexFile::FindProtoId().
     */
    struct ProtoDef {
      ClassCacheIndex return_type;
      uint8_t param_count;
      ClassCacheIndex params[kProtoMaxParams];
    };

    /**
     * The method definition using cached class, name and proto indexes.
     * The class index, method name index and proto index are used with
     * IndexCache to look up appropriate parameters for DexFile::FindMethodId().
     */
    struct MethodDef {
      ClassCacheIndex declaring_class;
      NameCacheIndex name;
      ProtoCacheIndex proto;
    };

    /**
     * The definition of an intrinsic function binds the method definition
     * to an Intrinsic.
     */
    struct IntrinsicDef {
      MethodDef method_def;
      Intrinsic intrinsic;
    };

    /**
     * Cache for class, method name and method signature indexes used during
     * intrinsic function lookup to avoid multiple lookups of the same items.
     *
     * Many classes have multiple intrinsics and/or they are used in multiple
     * method signatures and we want to avoid repeated lookups since they are
     * not exactly cheap. The method names and method signatures are sometimes
     * reused and therefore cached as well.
     */
    struct IndexCache {
      IndexCache();

      uint32_t class_indexes[kClassCacheLast - kClassCacheFirst];
      uint32_t name_indexes[kNameCacheLast - kNameCacheFirst];
      uint32_t proto_indexes[kProtoCacheLast - kProtoCacheFirst];
    };

    static const char* kClassCacheNames[];
    static const char* kNameCacheNames[];
    static const ProtoDef kProtoCacheDefs[];
    static const IntrinsicDef kIntrinsicMethods[];

    static const uint32_t kIndexNotFound = static_cast<uint32_t>(-1);
    static const uint32_t kIndexUnresolved = static_cast<uint32_t>(-2);

    static uint32_t FindClassIndex(const DexFile* dex_file, IndexCache* cache,
                                   ClassCacheIndex index);
    static uint32_t FindNameIndex(const DexFile* dex_file, IndexCache* cache,
                                  NameCacheIndex index);
    static uint32_t FindProtoIndex(const DexFile* dex_file, IndexCache* cache,
                                   ProtoCacheIndex index);
    static uint32_t FindMethodIndex(const DexFile* dex_file, IndexCache* cache,
                                    const MethodDef& method_def);

    /**
     * Find all known intrinsic methods in the dex_file and cache their indices.
     *
     * Only DexFileToMethodInlinerMap may call this function to initialize the inliner.
     */
    void FindIntrinsics(const DexFile* dex_file) EXCLUSIVE_LOCKS_REQUIRED(lock_);

    friend class DexFileToMethodInlinerMap;

    ReaderWriterMutex lock_;
    /*
     * Maps method indexes (for the particular DexFile) to Intrinsic defintions.
     */
    std::map<uint32_t, Intrinsic> intrinsics_ GUARDED_BY(lock_);
    const DexFile* dex_file_;

    DISALLOW_COPY_AND_ASSIGN(DexFileMethodInliner);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_DEX_FILE_METHOD_INLINER_H_
