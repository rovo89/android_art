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

#include "dex_file_method_inliner.h"

#include <algorithm>

#include "base/macros.h"
#include "base/mutex.h"
#include "base/mutex-inl.h"
#include "thread.h"
#include "thread-inl.h"
#include "dex/mir_graph.h"
#include "dex_instruction.h"
#include "dex_instruction-inl.h"
#include "verifier/method_verifier.h"
#include "verifier/method_verifier-inl.h"

namespace art {

const uint32_t DexFileMethodInliner::kIndexUnresolved;
const char* const DexFileMethodInliner::kClassCacheNames[] = {
    "Z",                       // kClassCacheBoolean
    "B",                       // kClassCacheByte
    "C",                       // kClassCacheChar
    "S",                       // kClassCacheShort
    "I",                       // kClassCacheInt
    "J",                       // kClassCacheLong
    "F",                       // kClassCacheFloat
    "D",                       // kClassCacheDouble
    "V",                       // kClassCacheVoid
    "Ljava/lang/Object;",      // kClassCacheJavaLangObject
    "Ljava/lang/String;",      // kClassCacheJavaLangString
    "Ljava/lang/Double;",      // kClassCacheJavaLangDouble
    "Ljava/lang/Float;",       // kClassCacheJavaLangFloat
    "Ljava/lang/Integer;",     // kClassCacheJavaLangInteger
    "Ljava/lang/Long;",        // kClassCacheJavaLangLong
    "Ljava/lang/Short;",       // kClassCacheJavaLangShort
    "Ljava/lang/Math;",        // kClassCacheJavaLangMath
    "Ljava/lang/StrictMath;",  // kClassCacheJavaLangStrictMath
    "Ljava/lang/Thread;",      // kClassCacheJavaLangThread
    "Llibcore/io/Memory;",     // kClassCacheLibcoreIoMemory
    "Lsun/misc/Unsafe;",       // kClassCacheSunMiscUnsafe
};

const char* const DexFileMethodInliner::kNameCacheNames[] = {
    "reverseBytes",          // kNameCacheReverseBytes
    "doubleToRawLongBits",   // kNameCacheDoubleToRawLongBits
    "longBitsToDouble",      // kNameCacheLongBitsToDouble
    "floatToRawIntBits",     // kNameCacheFloatToRawIntBits
    "intBitsToFloat",        // kNameCacheIntBitsToFloat
    "abs",                   // kNameCacheAbs
    "max",                   // kNameCacheMax
    "min",                   // kNameCacheMin
    "sqrt",                  // kNameCacheSqrt
    "charAt",                // kNameCacheCharAt
    "compareTo",             // kNameCacheCompareTo
    "isEmpty",               // kNameCacheIsEmpty
    "indexOf",               // kNameCacheIndexOf
    "length",                // kNameCacheLength
    "currentThread",         // kNameCacheCurrentThread
    "peekByte",              // kNameCachePeekByte
    "peekIntNative",         // kNameCachePeekIntNative
    "peekLongNative",        // kNameCachePeekLongNative
    "peekShortNative",       // kNameCachePeekShortNative
    "pokeByte",              // kNameCachePokeByte
    "pokeIntNative",         // kNameCachePokeIntNative
    "pokeLongNative",        // kNameCachePokeLongNative
    "pokeShortNative",       // kNameCachePokeShortNative
    "compareAndSwapInt",     // kNameCacheCompareAndSwapInt
    "compareAndSwapLong",    // kNameCacheCompareAndSwapLong
    "compareAndSwapObject",  // kNameCacheCompareAndSwapObject
    "getInt",                // kNameCacheGetInt
    "getIntVolatile",        // kNameCacheGetIntVolatile
    "putInt",                // kNameCachePutInt
    "putIntVolatile",        // kNameCachePutIntVolatile
    "putOrderedInt",         // kNameCachePutOrderedInt
    "getLong",               // kNameCacheGetLong
    "getLongVolatile",       // kNameCacheGetLongVolatile
    "putLong",               // kNameCachePutLong
    "putLongVolatile",       // kNameCachePutLongVolatile
    "putOrderedLong",        // kNameCachePutOrderedLong
    "getObject",             // kNameCacheGetObject
    "getObjectVolatile",     // kNameCacheGetObjectVolatile
    "putObject",             // kNameCachePutObject
    "putObjectVolatile",     // kNameCachePutObjectVolatile
    "putOrderedObject",      // kNameCachePutOrderedObject
};

const DexFileMethodInliner::ProtoDef DexFileMethodInliner::kProtoCacheDefs[] = {
    // kProtoCacheI_I
    { kClassCacheInt, 1, { kClassCacheInt } },
    // kProtoCacheJ_J
    { kClassCacheLong, 1, { kClassCacheLong } },
    // kProtoCacheS_S
    { kClassCacheShort, 1, { kClassCacheShort } },
    // kProtoCacheD_D
    { kClassCacheDouble, 1, { kClassCacheDouble } },
    // kProtoCacheF_F
    { kClassCacheFloat, 1, { kClassCacheFloat } },
    // kProtoCacheD_J
    { kClassCacheLong, 1, { kClassCacheDouble } },
    // kProtoCacheJ_D
    { kClassCacheDouble, 1, { kClassCacheLong } },
    // kProtoCacheF_I
    { kClassCacheInt, 1, { kClassCacheFloat } },
    // kProtoCacheI_F
    { kClassCacheFloat, 1, { kClassCacheInt } },
    // kProtoCacheII_I
    { kClassCacheInt, 2, { kClassCacheInt, kClassCacheInt } },
    // kProtoCacheI_C
    { kClassCacheChar, 1, { kClassCacheInt } },
    // kProtoCacheString_I
    { kClassCacheInt, 1, { kClassCacheJavaLangString } },
    // kProtoCache_Z
    { kClassCacheBoolean, 0, { } },
    // kProtoCache_I
    { kClassCacheInt, 0, { } },
    // kProtoCache_Thread
    { kClassCacheJavaLangThread, 0, { } },
    // kProtoCacheJ_B
    { kClassCacheByte, 1, { kClassCacheLong } },
    // kProtoCacheJ_I
    { kClassCacheInt, 1, { kClassCacheLong } },
    // kProtoCacheJ_S
    { kClassCacheShort, 1, { kClassCacheLong } },
    // kProtoCacheJB_V
    { kClassCacheVoid, 2, { kClassCacheLong, kClassCacheByte } },
    // kProtoCacheJI_V
    { kClassCacheVoid, 2, { kClassCacheLong, kClassCacheInt } },
    // kProtoCacheJJ_V
    { kClassCacheVoid, 2, { kClassCacheLong, kClassCacheLong } },
    // kProtoCacheJS_V
    { kClassCacheVoid, 2, { kClassCacheLong, kClassCacheShort } },
    // kProtoCacheObjectJII_Z
    { kClassCacheBoolean, 4, { kClassCacheJavaLangObject, kClassCacheLong,
        kClassCacheInt, kClassCacheInt } },
    // kProtoCacheObjectJJJ_Z
    { kClassCacheBoolean, 4, { kClassCacheJavaLangObject, kClassCacheLong,
        kClassCacheLong, kClassCacheLong } },
    // kProtoCacheObjectJObjectObject_Z
    { kClassCacheBoolean, 4, { kClassCacheJavaLangObject, kClassCacheLong,
        kClassCacheJavaLangObject, kClassCacheJavaLangObject } },
    // kProtoCacheObjectJ_I
    { kClassCacheInt, 2, { kClassCacheJavaLangObject, kClassCacheLong } },
    // kProtoCacheObjectJI_V
    { kClassCacheVoid, 3, { kClassCacheJavaLangObject, kClassCacheLong, kClassCacheInt } },
    // kProtoCacheObjectJ_J
    { kClassCacheLong, 2, { kClassCacheJavaLangObject, kClassCacheLong } },
    // kProtoCacheObjectJJ_V
    { kClassCacheVoid, 3, { kClassCacheJavaLangObject, kClassCacheLong, kClassCacheLong } },
    // kProtoCacheObjectJ_Object
    { kClassCacheJavaLangObject, 2, { kClassCacheJavaLangObject, kClassCacheLong } },
    // kProtoCacheObjectJObject_V
    { kClassCacheVoid, 3, { kClassCacheJavaLangObject, kClassCacheLong,
        kClassCacheJavaLangObject } },
};

const DexFileMethodInliner::IntrinsicDef DexFileMethodInliner::kIntrinsicMethods[] = {
#define INTRINSIC(c, n, p, o, d) \
    { { kClassCache ## c, kNameCache ## n, kProtoCache ## p }, { o, kInlineIntrinsic, { d } } }

    INTRINSIC(JavaLangDouble, DoubleToRawLongBits, D_J, kIntrinsicDoubleCvt, 0),
    INTRINSIC(JavaLangDouble, LongBitsToDouble, J_D, kIntrinsicDoubleCvt, 0),
    INTRINSIC(JavaLangFloat, FloatToRawIntBits, F_I, kIntrinsicFloatCvt, 0),
    INTRINSIC(JavaLangFloat, IntBitsToFloat, I_F, kIntrinsicFloatCvt, 0),

    INTRINSIC(JavaLangInteger, ReverseBytes, I_I, kIntrinsicReverseBytes, kWord),
    INTRINSIC(JavaLangLong, ReverseBytes, J_J, kIntrinsicReverseBytes, kLong),
    INTRINSIC(JavaLangShort, ReverseBytes, S_S, kIntrinsicReverseBytes, kSignedHalf),

    INTRINSIC(JavaLangMath,       Abs, I_I, kIntrinsicAbsInt, 0),
    INTRINSIC(JavaLangStrictMath, Abs, I_I, kIntrinsicAbsInt, 0),
    INTRINSIC(JavaLangMath,       Abs, J_J, kIntrinsicAbsLong, 0),
    INTRINSIC(JavaLangStrictMath, Abs, J_J, kIntrinsicAbsLong, 0),
    INTRINSIC(JavaLangMath,       Abs, F_F, kIntrinsicAbsFloat, 0),
    INTRINSIC(JavaLangStrictMath, Abs, F_F, kIntrinsicAbsFloat, 0),
    INTRINSIC(JavaLangMath,       Abs, D_D, kIntrinsicAbsDouble, 0),
    INTRINSIC(JavaLangStrictMath, Abs, D_D, kIntrinsicAbsDouble, 0),
    INTRINSIC(JavaLangMath,       Min, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMin),
    INTRINSIC(JavaLangStrictMath, Min, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMin),
    INTRINSIC(JavaLangMath,       Max, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMax),
    INTRINSIC(JavaLangStrictMath, Max, II_I, kIntrinsicMinMaxInt, kIntrinsicFlagMax),
    INTRINSIC(JavaLangMath,       Sqrt, D_D, kIntrinsicSqrt, 0),
    INTRINSIC(JavaLangStrictMath, Sqrt, D_D, kIntrinsicSqrt, 0),

    INTRINSIC(JavaLangString, CharAt, I_C, kIntrinsicCharAt, 0),
    INTRINSIC(JavaLangString, CompareTo, String_I, kIntrinsicCompareTo, 0),
    INTRINSIC(JavaLangString, IsEmpty, _Z, kIntrinsicIsEmptyOrLength, kIntrinsicFlagIsEmpty),
    INTRINSIC(JavaLangString, IndexOf, II_I, kIntrinsicIndexOf, kIntrinsicFlagNone),
    INTRINSIC(JavaLangString, IndexOf, I_I, kIntrinsicIndexOf, kIntrinsicFlagBase0),
    INTRINSIC(JavaLangString, Length, _I, kIntrinsicIsEmptyOrLength, kIntrinsicFlagLength),

    INTRINSIC(JavaLangThread, CurrentThread, _Thread, kIntrinsicCurrentThread, 0),

    INTRINSIC(LibcoreIoMemory, PeekByte, J_B, kIntrinsicPeek, kSignedByte),
    INTRINSIC(LibcoreIoMemory, PeekIntNative, J_I, kIntrinsicPeek, kWord),
    INTRINSIC(LibcoreIoMemory, PeekLongNative, J_J, kIntrinsicPeek, kLong),
    INTRINSIC(LibcoreIoMemory, PeekShortNative, J_S, kIntrinsicPeek, kSignedHalf),
    INTRINSIC(LibcoreIoMemory, PokeByte, JB_V, kIntrinsicPoke, kSignedByte),
    INTRINSIC(LibcoreIoMemory, PokeIntNative, JI_V, kIntrinsicPoke, kWord),
    INTRINSIC(LibcoreIoMemory, PokeLongNative, JJ_V, kIntrinsicPoke, kLong),
    INTRINSIC(LibcoreIoMemory, PokeShortNative, JS_V, kIntrinsicPoke, kSignedHalf),

    INTRINSIC(SunMiscUnsafe, CompareAndSwapInt, ObjectJII_Z, kIntrinsicCas,
              kIntrinsicFlagNone),
    INTRINSIC(SunMiscUnsafe, CompareAndSwapLong, ObjectJJJ_Z, kIntrinsicCas,
              kIntrinsicFlagIsLong),
    INTRINSIC(SunMiscUnsafe, CompareAndSwapObject, ObjectJObjectObject_Z, kIntrinsicCas,
              kIntrinsicFlagIsObject),

#define UNSAFE_GET_PUT(type, code, type_flags) \
    INTRINSIC(SunMiscUnsafe, Get ## type, ObjectJ_ ## code, kIntrinsicUnsafeGet, \
              type_flags & ~kIntrinsicFlagIsObject), \
    INTRINSIC(SunMiscUnsafe, Get ## type ## Volatile, ObjectJ_ ## code, kIntrinsicUnsafeGet, \
              (type_flags | kIntrinsicFlagIsVolatile) & ~kIntrinsicFlagIsObject), \
    INTRINSIC(SunMiscUnsafe, Put ## type, ObjectJ ## code ## _V, kIntrinsicUnsafePut, \
              type_flags), \
    INTRINSIC(SunMiscUnsafe, Put ## type ## Volatile, ObjectJ ## code ## _V, kIntrinsicUnsafePut, \
              type_flags | kIntrinsicFlagIsVolatile), \
    INTRINSIC(SunMiscUnsafe, PutOrdered ## type, ObjectJ ## code ## _V, kIntrinsicUnsafePut, \
              type_flags | kIntrinsicFlagIsOrdered)

    UNSAFE_GET_PUT(Int, I, kIntrinsicFlagNone),
    UNSAFE_GET_PUT(Long, J, kIntrinsicFlagIsLong),
    UNSAFE_GET_PUT(Object, Object, kIntrinsicFlagIsObject),
#undef UNSAFE_GET_PUT

#undef INTRINSIC
};

DexFileMethodInliner::DexFileMethodInliner()
    : lock_("DexFileMethodInliner lock", kDexFileMethodInlinerLock),
      dex_file_(NULL) {
  COMPILE_ASSERT(kClassCacheFirst == 0, kClassCacheFirst_not_0);
  COMPILE_ASSERT(arraysize(kClassCacheNames) == kClassCacheLast, bad_arraysize_kClassCacheNames);
  COMPILE_ASSERT(kNameCacheFirst == 0, kNameCacheFirst_not_0);
  COMPILE_ASSERT(arraysize(kNameCacheNames) == kNameCacheLast, bad_arraysize_kNameCacheNames);
  COMPILE_ASSERT(kProtoCacheFirst == 0, kProtoCacheFirst_not_0);
  COMPILE_ASSERT(arraysize(kProtoCacheDefs) == kProtoCacheLast, bad_arraysize_kProtoCacheNames);
}

DexFileMethodInliner::~DexFileMethodInliner() {
}

bool DexFileMethodInliner::AnalyseMethodCode(verifier::MethodVerifier* verifier) {
  InlineMethod method;
  bool success = InlineMethodAnalyser::AnalyseMethodCode(verifier, &method);
  return success && AddInlineMethod(verifier->GetMethodReference().dex_method_index, method);
}

bool DexFileMethodInliner::IsIntrinsic(uint32_t method_index) {
  ReaderMutexLock mu(Thread::Current(), lock_);
  auto it = inline_methods_.find(method_index);
  return it != inline_methods_.end() && (it->second.flags & kInlineIntrinsic) != 0;
}

bool DexFileMethodInliner::GenIntrinsic(Mir2Lir* backend, CallInfo* info) {
  InlineMethod intrinsic;
  {
    ReaderMutexLock mu(Thread::Current(), lock_);
    auto it = inline_methods_.find(info->index);
    if (it == inline_methods_.end() || (it->second.flags & kInlineIntrinsic) == 0) {
      return false;
    }
    intrinsic = it->second;
  }
  switch (intrinsic.opcode) {
    case kIntrinsicDoubleCvt:
      return backend->GenInlinedDoubleCvt(info);
    case kIntrinsicFloatCvt:
      return backend->GenInlinedFloatCvt(info);
    case kIntrinsicReverseBytes:
      return backend->GenInlinedReverseBytes(info, static_cast<OpSize>(intrinsic.d.data));
    case kIntrinsicAbsInt:
      return backend->GenInlinedAbsInt(info);
    case kIntrinsicAbsLong:
      return backend->GenInlinedAbsLong(info);
    case kIntrinsicAbsFloat:
      return backend->GenInlinedAbsFloat(info);
    case kIntrinsicAbsDouble:
      return backend->GenInlinedAbsDouble(info);
    case kIntrinsicMinMaxInt:
      return backend->GenInlinedMinMaxInt(info, intrinsic.d.data & kIntrinsicFlagMin);
    case kIntrinsicSqrt:
      return backend->GenInlinedSqrt(info);
    case kIntrinsicCharAt:
      return backend->GenInlinedCharAt(info);
    case kIntrinsicCompareTo:
      return backend->GenInlinedStringCompareTo(info);
    case kIntrinsicIsEmptyOrLength:
      return backend->GenInlinedStringIsEmptyOrLength(
          info, intrinsic.d.data & kIntrinsicFlagIsEmpty);
    case kIntrinsicIndexOf:
      return backend->GenInlinedIndexOf(info, intrinsic.d.data & kIntrinsicFlagBase0);
    case kIntrinsicCurrentThread:
      return backend->GenInlinedCurrentThread(info);
    case kIntrinsicPeek:
      return backend->GenInlinedPeek(info, static_cast<OpSize>(intrinsic.d.data));
    case kIntrinsicPoke:
      return backend->GenInlinedPoke(info, static_cast<OpSize>(intrinsic.d.data));
    case kIntrinsicCas:
      return backend->GenInlinedCas(info, intrinsic.d.data & kIntrinsicFlagIsLong,
                                    intrinsic.d.data & kIntrinsicFlagIsObject);
    case kIntrinsicUnsafeGet:
      return backend->GenInlinedUnsafeGet(info, intrinsic.d.data & kIntrinsicFlagIsLong,
                                          intrinsic.d.data & kIntrinsicFlagIsVolatile);
    case kIntrinsicUnsafePut:
      return backend->GenInlinedUnsafePut(info, intrinsic.d.data & kIntrinsicFlagIsLong,
                                          intrinsic.d.data & kIntrinsicFlagIsObject,
                                          intrinsic.d.data & kIntrinsicFlagIsVolatile,
                                          intrinsic.d.data & kIntrinsicFlagIsOrdered);
    default:
      LOG(FATAL) << "Unexpected intrinsic opcode: " << intrinsic.opcode;
      return false;  // avoid warning "control reaches end of non-void function"
  }
}

bool DexFileMethodInliner::IsSpecial(uint32_t method_index) {
  ReaderMutexLock mu(Thread::Current(), lock_);
  auto it = inline_methods_.find(method_index);
  return it != inline_methods_.end() && (it->second.flags & kInlineSpecial) != 0;
}

bool DexFileMethodInliner::GenSpecial(Mir2Lir* backend, uint32_t method_idx) {
  InlineMethod special;
  {
    ReaderMutexLock mu(Thread::Current(), lock_);
    auto it = inline_methods_.find(method_idx);
    if (it == inline_methods_.end() || (it->second.flags & kInlineSpecial) == 0) {
      return false;
    }
    special = it->second;
  }
  return backend->SpecialMIR2LIR(special);
}

uint32_t DexFileMethodInliner::FindClassIndex(const DexFile* dex_file, IndexCache* cache,
                                              ClassCacheIndex index) {
  uint32_t* class_index = &cache->class_indexes[index];
  if (*class_index != kIndexUnresolved) {
    return *class_index;
  }

  const DexFile::StringId* string_id = dex_file->FindStringId(kClassCacheNames[index]);
  if (string_id == nullptr) {
    *class_index = kIndexNotFound;
    return *class_index;
  }
  uint32_t string_index = dex_file->GetIndexForStringId(*string_id);

  const DexFile::TypeId* type_id = dex_file->FindTypeId(string_index);
  if (type_id == nullptr) {
    *class_index = kIndexNotFound;
    return *class_index;
  }
  *class_index = dex_file->GetIndexForTypeId(*type_id);
  return *class_index;
}

uint32_t DexFileMethodInliner::FindNameIndex(const DexFile* dex_file, IndexCache* cache,
                                             NameCacheIndex index) {
  uint32_t* name_index = &cache->name_indexes[index];
  if (*name_index != kIndexUnresolved) {
    return *name_index;
  }

  const DexFile::StringId* string_id = dex_file->FindStringId(kNameCacheNames[index]);
  if (string_id == nullptr) {
    *name_index = kIndexNotFound;
    return *name_index;
  }
  *name_index = dex_file->GetIndexForStringId(*string_id);
  return *name_index;
}

uint32_t DexFileMethodInliner::FindProtoIndex(const DexFile* dex_file, IndexCache* cache,
                                              ProtoCacheIndex index) {
  uint32_t* proto_index = &cache->proto_indexes[index];
  if (*proto_index != kIndexUnresolved) {
    return *proto_index;
  }

  const ProtoDef& proto_def = kProtoCacheDefs[index];
  uint32_t return_index = FindClassIndex(dex_file, cache, proto_def.return_type);
  if (return_index == kIndexNotFound) {
    *proto_index = kIndexNotFound;
    return *proto_index;
  }
  uint16_t return_type = static_cast<uint16_t>(return_index);
  DCHECK_EQ(static_cast<uint32_t>(return_type), return_index);

  uint32_t signature_length = proto_def.param_count;
  uint16_t signature_type_idxs[kProtoMaxParams];
  for (uint32_t i = 0; i != signature_length; ++i) {
    uint32_t param_index = FindClassIndex(dex_file, cache, proto_def.params[i]);
    if (param_index == kIndexNotFound) {
      *proto_index = kIndexNotFound;
      return *proto_index;
    }
    signature_type_idxs[i] = static_cast<uint16_t>(param_index);
    DCHECK_EQ(static_cast<uint32_t>(signature_type_idxs[i]), param_index);
  }

  const DexFile::ProtoId* proto_id = dex_file->FindProtoId(return_type, signature_type_idxs,
                                                           signature_length);
  if (proto_id == nullptr) {
    *proto_index = kIndexNotFound;
    return *proto_index;
  }
  *proto_index = dex_file->GetIndexForProtoId(*proto_id);
  return *proto_index;
}

uint32_t DexFileMethodInliner::FindMethodIndex(const DexFile* dex_file, IndexCache* cache,
                                               const MethodDef& method_def) {
  uint32_t declaring_class_index = FindClassIndex(dex_file, cache, method_def.declaring_class);
  if (declaring_class_index == kIndexNotFound) {
    return kIndexNotFound;
  }
  uint32_t name_index = FindNameIndex(dex_file, cache, method_def.name);
  if (name_index == kIndexNotFound) {
    return kIndexNotFound;
  }
  uint32_t proto_index = FindProtoIndex(dex_file, cache, method_def.proto);
  if (proto_index == kIndexNotFound) {
    return kIndexNotFound;
  }
  const DexFile::MethodId* method_id =
      dex_file->FindMethodId(dex_file->GetTypeId(declaring_class_index),
                             dex_file->GetStringId(name_index),
                             dex_file->GetProtoId(proto_index));
  if (method_id == nullptr) {
    return kIndexNotFound;
  }
  return dex_file->GetIndexForMethodId(*method_id);
}

DexFileMethodInliner::IndexCache::IndexCache() {
  std::fill_n(class_indexes, arraysize(class_indexes), kIndexUnresolved);
  std::fill_n(name_indexes, arraysize(name_indexes), kIndexUnresolved);
  std::fill_n(proto_indexes, arraysize(proto_indexes), kIndexUnresolved);
}

void DexFileMethodInliner::FindIntrinsics(const DexFile* dex_file) {
  DCHECK(dex_file != nullptr);
  DCHECK(dex_file_ == nullptr);
  IndexCache cache;
  for (const IntrinsicDef& def : kIntrinsicMethods) {
    uint32_t method_idx = FindMethodIndex(dex_file, &cache, def.method_def);
    if (method_idx != kIndexNotFound) {
      DCHECK(inline_methods_.find(method_idx) == inline_methods_.end());
      inline_methods_.Put(method_idx, def.intrinsic);
    }
  }
  dex_file_ = dex_file;
}

bool DexFileMethodInliner::AddInlineMethod(int32_t method_idx, const InlineMethod& method) {
  WriterMutexLock mu(Thread::Current(), lock_);
  if (LIKELY(inline_methods_.find(method_idx) == inline_methods_.end())) {
    inline_methods_.Put(method_idx, method);
    return true;
  } else {
    if (PrettyMethod(method_idx, *dex_file_) == "int java.lang.String.length()") {
      // TODO: String.length is both kIntrinsicIsEmptyOrLength and kInlineOpIGet.
    } else {
      LOG(ERROR) << "Inliner: " << PrettyMethod(method_idx, *dex_file_) << " already inline";
    }
    return false;
  }
}

}  // namespace art
