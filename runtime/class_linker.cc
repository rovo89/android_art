/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "class_linker.h"

#include <deque>
#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "base/casts.h"
#include "base/logging.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "class_linker-inl.h"
#include "compiler_callbacks.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc_root-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "handle_scope.h"
#include "intern_table.h"
#include "interpreter/interpreter.h"
#include "leb128.h"
#include "method_helper-inl.h"
#include "oat.h"
#include "oat_file.h"
#include "object_lock.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/iftable-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/proxy.h"
#include "mirror/reference-inl.h"
#include "mirror/stack_trace_element.h"
#include "mirror/string-inl.h"
#include "os.h"
#include "runtime.h"
#include "entrypoints/entrypoint_utils.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"
#include "thread-inl.h"
#include "utils.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

namespace art {

static void ThrowNoClassDefFoundError(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
static void ThrowNoClassDefFoundError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread* self = Thread::Current();
  ThrowLocation throw_location = self->GetCurrentLocationForThrow();
  self->ThrowNewExceptionV(throw_location, "Ljava/lang/NoClassDefFoundError;", fmt, args);
  va_end(args);
}

static void ThrowEarlierClassFailure(mirror::Class* c)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // The class failed to initialize on a previous attempt, so we want to throw
  // a NoClassDefFoundError (v2 2.17.5).  The exception to this rule is if we
  // failed in verification, in which case v2 5.4.1 says we need to re-throw
  // the previous error.
  Runtime* runtime = Runtime::Current();
  bool is_compiler = runtime->IsCompiler();
  if (!is_compiler) {  // Give info if this occurs at runtime.
    LOG(INFO) << "Rejecting re-init on previously-failed class " << PrettyClass(c);
  }

  CHECK(c->IsErroneous()) << PrettyClass(c) << " " << c->GetStatus();
  Thread* self = Thread::Current();
  if (is_compiler) {
    // At compile time, accurate errors and NCDFE are disabled to speed compilation.
    mirror::Throwable* pre_allocated = runtime->GetPreAllocatedNoClassDefFoundError();
    self->SetException(ThrowLocation(), pre_allocated);
  } else {
    ThrowLocation throw_location = self->GetCurrentLocationForThrow();
    if (c->GetVerifyErrorClass() != NULL) {
      // TODO: change the verifier to store an _instance_, with a useful detail message?
      std::string temp;
      self->ThrowNewException(throw_location, c->GetVerifyErrorClass()->GetDescriptor(&temp),
                              PrettyDescriptor(c).c_str());
    } else {
      self->ThrowNewException(throw_location, "Ljava/lang/NoClassDefFoundError;",
                              PrettyDescriptor(c).c_str());
    }
  }
}

static void VlogClassInitializationFailure(Handle<mirror::Class> klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (VLOG_IS_ON(class_linker)) {
    std::string temp;
    LOG(INFO) << "Failed to initialize class " << klass->GetDescriptor(&temp) << " from "
              << klass->GetLocation() << "\n" << Thread::Current()->GetException(nullptr)->Dump();
  }
}

static void WrapExceptionInInitializer(Handle<mirror::Class> klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  ScopedLocalRef<jthrowable> cause(env, env->ExceptionOccurred());
  CHECK(cause.get() != nullptr);

  env->ExceptionClear();
  bool is_error = env->IsInstanceOf(cause.get(), WellKnownClasses::java_lang_Error);
  env->Throw(cause.get());

  // We only wrap non-Error exceptions; an Error can just be used as-is.
  if (!is_error) {
    ThrowLocation throw_location = self->GetCurrentLocationForThrow();
    self->ThrowNewWrappedException(throw_location, "Ljava/lang/ExceptionInInitializerError;",
                                   nullptr);
  }
  VlogClassInitializationFailure(klass);
}

static size_t Hash(const char* s) {
  // This is the java.lang.String hashcode for convenience, not interoperability.
  size_t hash = 0;
  for (; *s != '\0'; ++s) {
    hash = hash * 31 + *s;
  }
  return hash;
}

// Gap between two fields in object layout.
struct FieldGap {
  uint32_t start_offset;  // The offset from the start of the object.
  uint32_t size;  // The gap size of 1, 2, or 4 bytes.
};
struct FieldGapsComparator {
  explicit FieldGapsComparator() {
  }
  bool operator() (const FieldGap& lhs, const FieldGap& rhs)
      NO_THREAD_SAFETY_ANALYSIS {
    // Sort by gap size, largest first.
    return lhs.size > rhs.size;
  }
};
typedef std::priority_queue<FieldGap, std::vector<FieldGap>, FieldGapsComparator> FieldGaps;

// Adds largest aligned gaps to queue of gaps.
void AddFieldGap(uint32_t gap_start, uint32_t gap_end, FieldGaps* gaps) {
  DCHECK(gaps != nullptr);

  uint32_t current_offset = gap_start;
  while (current_offset != gap_end) {
    size_t remaining = gap_end - current_offset;
    if (remaining >= sizeof(uint32_t) && IsAligned<4>(current_offset)) {
      gaps->push(FieldGap {current_offset, sizeof(uint32_t)});
      current_offset += sizeof(uint32_t);
    } else if (remaining >= sizeof(uint16_t) && IsAligned<2>(current_offset)) {
      gaps->push(FieldGap {current_offset, sizeof(uint16_t)});
      current_offset += sizeof(uint16_t);
    } else {
      gaps->push(FieldGap {current_offset, sizeof(uint8_t)});
      current_offset += sizeof(uint8_t);
    }
    DCHECK_LE(current_offset, gap_end) << "Overran gap";
  }
}
// Shuffle fields forward, making use of gaps whenever possible.
template<int n>
static void ShuffleForward(const size_t num_fields, size_t* current_field_idx,
                           MemberOffset* field_offset,
                           mirror::ObjectArray<mirror::ArtField>* fields,
                           std::deque<mirror::ArtField*>* grouped_and_sorted_fields,
                           FieldGaps* gaps)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(current_field_idx != nullptr);
  DCHECK(grouped_and_sorted_fields != nullptr);
  DCHECK(fields != nullptr || (num_fields == 0 && grouped_and_sorted_fields->empty()));
  DCHECK(gaps != nullptr);
  DCHECK(field_offset != nullptr);

  DCHECK(IsPowerOfTwo(n));
  while (!grouped_and_sorted_fields->empty()) {
    mirror::ArtField* field = grouped_and_sorted_fields->front();
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    if (Primitive::ComponentSize(type) < n) {
      break;
    }
    if (!IsAligned<n>(field_offset->Uint32Value())) {
      MemberOffset old_offset = *field_offset;
      *field_offset = MemberOffset(RoundUp(field_offset->Uint32Value(), n));
      AddFieldGap(old_offset.Uint32Value(), field_offset->Uint32Value(), gaps);
    }
    CHECK(type != Primitive::kPrimNot) << PrettyField(field);  // should be primitive types
    grouped_and_sorted_fields->pop_front();
    fields->Set<false>(*current_field_idx, field);
    if (!gaps->empty() && gaps->top().size >= n) {
      FieldGap gap = gaps->top();
      gaps->pop();
      DCHECK(IsAligned<n>(gap.start_offset));
      field->SetOffset(MemberOffset(gap.start_offset));
      if (gap.size > n) {
        AddFieldGap(gap.start_offset + n, gap.start_offset + gap.size, gaps);
      }
    } else {
      DCHECK(IsAligned<n>(field_offset->Uint32Value()));
      field->SetOffset(*field_offset);
      *field_offset = MemberOffset(field_offset->Uint32Value() + n);
    }
    ++(*current_field_idx);
  }
}

ClassLinker::ClassLinker(InternTable* intern_table)
    // dex_lock_ is recursive as it may be used in stack dumping.
    : dex_lock_("ClassLinker dex lock", kDefaultMutexLevel),
      dex_cache_image_class_lookup_required_(false),
      failed_dex_cache_class_lookups_(0),
      class_roots_(nullptr),
      array_iftable_(nullptr),
      find_array_class_cache_next_victim_(0),
      init_done_(false),
      log_new_dex_caches_roots_(false),
      log_new_class_table_roots_(false),
      intern_table_(intern_table),
      portable_resolution_trampoline_(nullptr),
      quick_resolution_trampoline_(nullptr),
      portable_imt_conflict_trampoline_(nullptr),
      quick_imt_conflict_trampoline_(nullptr),
      quick_generic_jni_trampoline_(nullptr),
      quick_to_interpreter_bridge_trampoline_(nullptr) {
  memset(find_array_class_cache_, 0, kFindArrayCacheSize * sizeof(mirror::Class*));
}

void ClassLinker::InitWithoutImage(const std::vector<const DexFile*>& boot_class_path) {
  VLOG(startup) << "ClassLinker::Init";
  CHECK(!Runtime::Current()->GetHeap()->HasImageSpace()) << "Runtime has image. We should use it.";

  CHECK(!init_done_);

  // java_lang_Class comes first, it's needed for AllocClass
  Thread* self = Thread::Current();
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // The GC can't handle an object with a null class since we can't get the size of this object.
  heap->IncrementDisableMovingGC(self);
  StackHandleScope<64> hs(self);  // 64 is picked arbitrarily.
  Handle<mirror::Class> java_lang_Class(hs.NewHandle(down_cast<mirror::Class*>(
      heap->AllocNonMovableObject<true>(self, nullptr,
                                        mirror::Class::ClassClassSize(),
                                        VoidFunctor()))));
  CHECK(java_lang_Class.Get() != nullptr);
  mirror::Class::SetClassClass(java_lang_Class.Get());
  java_lang_Class->SetClass(java_lang_Class.Get());
  if (kUseBakerOrBrooksReadBarrier) {
    java_lang_Class->AssertReadBarrierPointer();
  }
  java_lang_Class->SetClassSize(mirror::Class::ClassClassSize());
  java_lang_Class->SetPrimitiveType(Primitive::kPrimNot);
  heap->DecrementDisableMovingGC(self);
  // AllocClass(mirror::Class*) can now be used

  // Class[] is used for reflection support.
  Handle<mirror::Class> class_array_class(hs.NewHandle(
     AllocClass(self, java_lang_Class.Get(), mirror::ObjectArray<mirror::Class>::ClassSize())));
  class_array_class->SetComponentType(java_lang_Class.Get());

  // java_lang_Object comes next so that object_array_class can be created.
  Handle<mirror::Class> java_lang_Object(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Object::ClassSize())));
  CHECK(java_lang_Object.Get() != nullptr);
  // backfill Object as the super class of Class.
  java_lang_Class->SetSuperClass(java_lang_Object.Get());
  java_lang_Object->SetStatus(mirror::Class::kStatusLoaded, self);

  // Object[] next to hold class roots.
  Handle<mirror::Class> object_array_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::ObjectArray<mirror::Object>::ClassSize())));
  object_array_class->SetComponentType(java_lang_Object.Get());

  // Setup the char (primitive) class to be used for char[].
  Handle<mirror::Class> char_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Class::PrimitiveClassSize())));
  // The primitive char class won't be initialized by
  // InitializePrimitiveClass until line 459, but strings (and
  // internal char arrays) will be allocated before that and the
  // component size, which is computed from the primitive type, needs
  // to be set here.
  char_class->SetPrimitiveType(Primitive::kPrimChar);

  // Setup the char[] class to be used for String.
  Handle<mirror::Class> char_array_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(),
                 mirror::Array::ClassSize())));
  char_array_class->SetComponentType(char_class.Get());
  mirror::CharArray::SetArrayClass(char_array_class.Get());

  // Setup String.
  Handle<mirror::Class> java_lang_String(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::String::ClassSize())));
  mirror::String::SetClass(java_lang_String.Get());
  java_lang_String->SetObjectSize(mirror::String::InstanceSize());
  java_lang_String->SetStatus(mirror::Class::kStatusResolved, self);

  // Setup Reference.
  Handle<mirror::Class> java_lang_ref_Reference(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Reference::ClassSize())));
  mirror::Reference::SetClass(java_lang_ref_Reference.Get());
  java_lang_ref_Reference->SetObjectSize(mirror::Reference::InstanceSize());
  java_lang_ref_Reference->SetStatus(mirror::Class::kStatusResolved, self);

  // Create storage for root classes, save away our work so far (requires descriptors).
  class_roots_ = GcRoot<mirror::ObjectArray<mirror::Class> >(
      mirror::ObjectArray<mirror::Class>::Alloc(self, object_array_class.Get(),
                                                kClassRootsMax));
  CHECK(!class_roots_.IsNull());
  SetClassRoot(kJavaLangClass, java_lang_Class.Get());
  SetClassRoot(kJavaLangObject, java_lang_Object.Get());
  SetClassRoot(kClassArrayClass, class_array_class.Get());
  SetClassRoot(kObjectArrayClass, object_array_class.Get());
  SetClassRoot(kCharArrayClass, char_array_class.Get());
  SetClassRoot(kJavaLangString, java_lang_String.Get());
  SetClassRoot(kJavaLangRefReference, java_lang_ref_Reference.Get());

  // Setup the primitive type classes.
  SetClassRoot(kPrimitiveBoolean, CreatePrimitiveClass(self, Primitive::kPrimBoolean));
  SetClassRoot(kPrimitiveByte, CreatePrimitiveClass(self, Primitive::kPrimByte));
  SetClassRoot(kPrimitiveShort, CreatePrimitiveClass(self, Primitive::kPrimShort));
  SetClassRoot(kPrimitiveInt, CreatePrimitiveClass(self, Primitive::kPrimInt));
  SetClassRoot(kPrimitiveLong, CreatePrimitiveClass(self, Primitive::kPrimLong));
  SetClassRoot(kPrimitiveFloat, CreatePrimitiveClass(self, Primitive::kPrimFloat));
  SetClassRoot(kPrimitiveDouble, CreatePrimitiveClass(self, Primitive::kPrimDouble));
  SetClassRoot(kPrimitiveVoid, CreatePrimitiveClass(self, Primitive::kPrimVoid));

  // Create array interface entries to populate once we can load system classes.
  array_iftable_ = GcRoot<mirror::IfTable>(AllocIfTable(self, 2));

  // Create int array type for AllocDexCache (done in AppendToBootClassPath).
  Handle<mirror::Class> int_array_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Array::ClassSize())));
  int_array_class->SetComponentType(GetClassRoot(kPrimitiveInt));
  mirror::IntArray::SetArrayClass(int_array_class.Get());
  SetClassRoot(kIntArrayClass, int_array_class.Get());

  // now that these are registered, we can use AllocClass() and AllocObjectArray

  // Set up DexCache. This cannot be done later since AppendToBootClassPath calls AllocDexCache.
  Handle<mirror::Class> java_lang_DexCache(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::DexCache::ClassSize())));
  SetClassRoot(kJavaLangDexCache, java_lang_DexCache.Get());
  java_lang_DexCache->SetObjectSize(mirror::DexCache::InstanceSize());
  java_lang_DexCache->SetStatus(mirror::Class::kStatusResolved, self);

  // Constructor, Field, Method, and AbstractMethod are necessary so
  // that FindClass can link members.
  Handle<mirror::Class> java_lang_reflect_ArtField(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::ArtField::ClassSize())));
  CHECK(java_lang_reflect_ArtField.Get() != nullptr);
  java_lang_reflect_ArtField->SetObjectSize(mirror::ArtField::InstanceSize());
  SetClassRoot(kJavaLangReflectArtField, java_lang_reflect_ArtField.Get());
  java_lang_reflect_ArtField->SetStatus(mirror::Class::kStatusResolved, self);
  mirror::ArtField::SetClass(java_lang_reflect_ArtField.Get());

  Handle<mirror::Class> java_lang_reflect_ArtMethod(hs.NewHandle(
    AllocClass(self, java_lang_Class.Get(), mirror::ArtMethod::ClassSize())));
  CHECK(java_lang_reflect_ArtMethod.Get() != nullptr);
  java_lang_reflect_ArtMethod->SetObjectSize(mirror::ArtMethod::InstanceSize());
  SetClassRoot(kJavaLangReflectArtMethod, java_lang_reflect_ArtMethod.Get());
  java_lang_reflect_ArtMethod->SetStatus(mirror::Class::kStatusResolved, self);

  mirror::ArtMethod::SetClass(java_lang_reflect_ArtMethod.Get());

  // Set up array classes for string, field, method
  Handle<mirror::Class> object_array_string(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(),
                 mirror::ObjectArray<mirror::String>::ClassSize())));
  object_array_string->SetComponentType(java_lang_String.Get());
  SetClassRoot(kJavaLangStringArrayClass, object_array_string.Get());

  Handle<mirror::Class> object_array_art_method(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(),
                 mirror::ObjectArray<mirror::ArtMethod>::ClassSize())));
  object_array_art_method->SetComponentType(java_lang_reflect_ArtMethod.Get());
  SetClassRoot(kJavaLangReflectArtMethodArrayClass, object_array_art_method.Get());

  Handle<mirror::Class> object_array_art_field(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(),
                 mirror::ObjectArray<mirror::ArtField>::ClassSize())));
  object_array_art_field->SetComponentType(java_lang_reflect_ArtField.Get());
  SetClassRoot(kJavaLangReflectArtFieldArrayClass, object_array_art_field.Get());

  // Setup boot_class_path_ and register class_path now that we can use AllocObjectArray to create
  // DexCache instances. Needs to be after String, Field, Method arrays since AllocDexCache uses
  // these roots.
  CHECK_NE(0U, boot_class_path.size());
  for (size_t i = 0; i != boot_class_path.size(); ++i) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != nullptr);
    AppendToBootClassPath(self, *dex_file);
  }

  // now we can use FindSystemClass

  // run char class through InitializePrimitiveClass to finish init
  InitializePrimitiveClass(char_class.Get(), Primitive::kPrimChar);
  SetClassRoot(kPrimitiveChar, char_class.Get());  // needs descriptor

  // Create runtime resolution and imt conflict methods. Also setup the default imt.
  Runtime* runtime = Runtime::Current();
  runtime->SetResolutionMethod(runtime->CreateResolutionMethod());
  runtime->SetImtConflictMethod(runtime->CreateImtConflictMethod());
  runtime->SetImtUnimplementedMethod(runtime->CreateImtConflictMethod());
  runtime->SetDefaultImt(runtime->CreateDefaultImt(this));

  // Set up GenericJNI entrypoint. That is mainly a hack for common_compiler_test.h so that
  // we do not need friend classes or a publicly exposed setter.
  quick_generic_jni_trampoline_ = GetQuickGenericJniStub();
  if (!runtime->IsCompiler()) {
    // We need to set up the generic trampolines since we don't have an image.
    quick_resolution_trampoline_ = GetQuickResolutionStub();
    quick_imt_conflict_trampoline_ = GetQuickImtConflictStub();
    quick_to_interpreter_bridge_trampoline_ = GetQuickToInterpreterBridge();
  }

  // Object, String and DexCache need to be rerun through FindSystemClass to finish init
  java_lang_Object->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* Object_class = FindSystemClass(self, "Ljava/lang/Object;");
  CHECK_EQ(java_lang_Object.Get(), Object_class);
  CHECK_EQ(java_lang_Object->GetObjectSize(), mirror::Object::InstanceSize());
  java_lang_String->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* String_class = FindSystemClass(self, "Ljava/lang/String;");
  std::ostringstream os1, os2;
  java_lang_String->DumpClass(os1, mirror::Class::kDumpClassFullDetail);
  String_class->DumpClass(os2, mirror::Class::kDumpClassFullDetail);
  CHECK_EQ(java_lang_String.Get(), String_class) << os1.str() << "\n\n" << os2.str();
  CHECK_EQ(java_lang_String->GetObjectSize(), mirror::String::InstanceSize());
  java_lang_DexCache->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* DexCache_class = FindSystemClass(self, "Ljava/lang/DexCache;");
  CHECK_EQ(java_lang_String.Get(), String_class);
  CHECK_EQ(java_lang_DexCache.Get(), DexCache_class);
  CHECK_EQ(java_lang_DexCache->GetObjectSize(), mirror::DexCache::InstanceSize());

  // Setup the primitive array type classes - can't be done until Object has a vtable.
  SetClassRoot(kBooleanArrayClass, FindSystemClass(self, "[Z"));
  mirror::BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));

  SetClassRoot(kByteArrayClass, FindSystemClass(self, "[B"));
  mirror::ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));

  mirror::Class* found_char_array_class = FindSystemClass(self, "[C");
  CHECK_EQ(char_array_class.Get(), found_char_array_class);

  SetClassRoot(kShortArrayClass, FindSystemClass(self, "[S"));
  mirror::ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));

  mirror::Class* found_int_array_class = FindSystemClass(self, "[I");
  CHECK_EQ(int_array_class.Get(), found_int_array_class);

  SetClassRoot(kLongArrayClass, FindSystemClass(self, "[J"));
  mirror::LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));

  SetClassRoot(kFloatArrayClass, FindSystemClass(self, "[F"));
  mirror::FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));

  SetClassRoot(kDoubleArrayClass, FindSystemClass(self, "[D"));
  mirror::DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));

  mirror::Class* found_class_array_class = FindSystemClass(self, "[Ljava/lang/Class;");
  CHECK_EQ(class_array_class.Get(), found_class_array_class);

  mirror::Class* found_object_array_class = FindSystemClass(self, "[Ljava/lang/Object;");
  CHECK_EQ(object_array_class.Get(), found_object_array_class);

  // Setup the single, global copy of "iftable".
  mirror::Class* java_lang_Cloneable = FindSystemClass(self, "Ljava/lang/Cloneable;");
  CHECK(java_lang_Cloneable != nullptr);
  mirror::Class* java_io_Serializable = FindSystemClass(self, "Ljava/io/Serializable;");
  CHECK(java_io_Serializable != nullptr);
  // We assume that Cloneable/Serializable don't have superinterfaces -- normally we'd have to
  // crawl up and explicitly list all of the supers as well.
  {
    mirror::IfTable* array_iftable = array_iftable_.Read();
    array_iftable->SetInterface(0, java_lang_Cloneable);
    array_iftable->SetInterface(1, java_io_Serializable);
  }

  // Sanity check Class[] and Object[]'s interfaces.
  CHECK_EQ(java_lang_Cloneable, mirror::Class::GetDirectInterface(self, class_array_class, 0));
  CHECK_EQ(java_io_Serializable, mirror::Class::GetDirectInterface(self, class_array_class, 1));
  CHECK_EQ(java_lang_Cloneable, mirror::Class::GetDirectInterface(self, object_array_class, 0));
  CHECK_EQ(java_io_Serializable, mirror::Class::GetDirectInterface(self, object_array_class, 1));
  // Run Class, ArtField, and ArtMethod through FindSystemClass. This initializes their
  // dex_cache_ fields and register them in class_table_.
  mirror::Class* Class_class = FindSystemClass(self, "Ljava/lang/Class;");
  CHECK_EQ(java_lang_Class.Get(), Class_class);

  java_lang_reflect_ArtMethod->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* Art_method_class = FindSystemClass(self, "Ljava/lang/reflect/ArtMethod;");
  CHECK_EQ(java_lang_reflect_ArtMethod.Get(), Art_method_class);

  java_lang_reflect_ArtField->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* Art_field_class = FindSystemClass(self, "Ljava/lang/reflect/ArtField;");
  CHECK_EQ(java_lang_reflect_ArtField.Get(), Art_field_class);

  mirror::Class* String_array_class =
      FindSystemClass(self, GetClassRootDescriptor(kJavaLangStringArrayClass));
  CHECK_EQ(object_array_string.Get(), String_array_class);

  mirror::Class* Art_method_array_class =
      FindSystemClass(self, GetClassRootDescriptor(kJavaLangReflectArtMethodArrayClass));
  CHECK_EQ(object_array_art_method.Get(), Art_method_array_class);

  mirror::Class* Art_field_array_class =
      FindSystemClass(self, GetClassRootDescriptor(kJavaLangReflectArtFieldArrayClass));
  CHECK_EQ(object_array_art_field.Get(), Art_field_array_class);

  // End of special init trickery, subsequent classes may be loaded via FindSystemClass.

  // Create java.lang.reflect.Proxy root.
  mirror::Class* java_lang_reflect_Proxy = FindSystemClass(self, "Ljava/lang/reflect/Proxy;");
  SetClassRoot(kJavaLangReflectProxy, java_lang_reflect_Proxy);

  // java.lang.ref classes need to be specially flagged, but otherwise are normal classes
  // finish initializing Reference class
  java_lang_ref_Reference->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* Reference_class = FindSystemClass(self, "Ljava/lang/ref/Reference;");
  CHECK_EQ(java_lang_ref_Reference.Get(), Reference_class);
  CHECK_EQ(java_lang_ref_Reference->GetObjectSize(), mirror::Reference::InstanceSize());
  CHECK_EQ(java_lang_ref_Reference->GetClassSize(), mirror::Reference::ClassSize());
  mirror::Class* java_lang_ref_FinalizerReference =
      FindSystemClass(self, "Ljava/lang/ref/FinalizerReference;");
  java_lang_ref_FinalizerReference->SetAccessFlags(
      java_lang_ref_FinalizerReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsFinalizerReference);
  mirror::Class* java_lang_ref_PhantomReference =
      FindSystemClass(self, "Ljava/lang/ref/PhantomReference;");
  java_lang_ref_PhantomReference->SetAccessFlags(
      java_lang_ref_PhantomReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsPhantomReference);
  mirror::Class* java_lang_ref_SoftReference =
      FindSystemClass(self, "Ljava/lang/ref/SoftReference;");
  java_lang_ref_SoftReference->SetAccessFlags(
      java_lang_ref_SoftReference->GetAccessFlags() | kAccClassIsReference);
  mirror::Class* java_lang_ref_WeakReference =
      FindSystemClass(self, "Ljava/lang/ref/WeakReference;");
  java_lang_ref_WeakReference->SetAccessFlags(
      java_lang_ref_WeakReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsWeakReference);

  // Setup the ClassLoader, verifying the object_size_.
  mirror::Class* java_lang_ClassLoader = FindSystemClass(self, "Ljava/lang/ClassLoader;");
  CHECK_EQ(java_lang_ClassLoader->GetObjectSize(), mirror::ClassLoader::InstanceSize());
  SetClassRoot(kJavaLangClassLoader, java_lang_ClassLoader);

  // Set up java.lang.Throwable, java.lang.ClassNotFoundException, and
  // java.lang.StackTraceElement as a convenience.
  SetClassRoot(kJavaLangThrowable, FindSystemClass(self, "Ljava/lang/Throwable;"));
  mirror::Throwable::SetClass(GetClassRoot(kJavaLangThrowable));
  SetClassRoot(kJavaLangClassNotFoundException,
               FindSystemClass(self, "Ljava/lang/ClassNotFoundException;"));
  SetClassRoot(kJavaLangStackTraceElement, FindSystemClass(self, "Ljava/lang/StackTraceElement;"));
  SetClassRoot(kJavaLangStackTraceElementArrayClass,
               FindSystemClass(self, "[Ljava/lang/StackTraceElement;"));
  mirror::StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit(self);

  VLOG(startup) << "ClassLinker::InitFromCompiler exiting";
}

void ClassLinker::FinishInit(Thread* self) {
  VLOG(startup) << "ClassLinker::FinishInit entering";

  // Let the heap know some key offsets into java.lang.ref instances
  // Note: we hard code the field indexes here rather than using FindInstanceField
  // as the types of the field can't be resolved prior to the runtime being
  // fully initialized
  mirror::Class* java_lang_ref_Reference = GetClassRoot(kJavaLangRefReference);
  mirror::Class* java_lang_ref_FinalizerReference =
      FindSystemClass(self, "Ljava/lang/ref/FinalizerReference;");

  mirror::ArtField* pendingNext = java_lang_ref_Reference->GetInstanceField(0);
  CHECK_STREQ(pendingNext->GetName(), "pendingNext");
  CHECK_STREQ(pendingNext->GetTypeDescriptor(), "Ljava/lang/ref/Reference;");

  mirror::ArtField* queue = java_lang_ref_Reference->GetInstanceField(1);
  CHECK_STREQ(queue->GetName(), "queue");
  CHECK_STREQ(queue->GetTypeDescriptor(), "Ljava/lang/ref/ReferenceQueue;");

  mirror::ArtField* queueNext = java_lang_ref_Reference->GetInstanceField(2);
  CHECK_STREQ(queueNext->GetName(), "queueNext");
  CHECK_STREQ(queueNext->GetTypeDescriptor(), "Ljava/lang/ref/Reference;");

  mirror::ArtField* referent = java_lang_ref_Reference->GetInstanceField(3);
  CHECK_STREQ(referent->GetName(), "referent");
  CHECK_STREQ(referent->GetTypeDescriptor(), "Ljava/lang/Object;");

  mirror::ArtField* zombie = java_lang_ref_FinalizerReference->GetInstanceField(2);
  CHECK_STREQ(zombie->GetName(), "zombie");
  CHECK_STREQ(zombie->GetTypeDescriptor(), "Ljava/lang/Object;");

  // ensure all class_roots_ are initialized
  for (size_t i = 0; i < kClassRootsMax; i++) {
    ClassRoot class_root = static_cast<ClassRoot>(i);
    mirror::Class* klass = GetClassRoot(class_root);
    CHECK(klass != nullptr);
    DCHECK(klass->IsArrayClass() || klass->IsPrimitive() || klass->GetDexCache() != nullptr);
    // note SetClassRoot does additional validation.
    // if possible add new checks there to catch errors early
  }

  CHECK(!array_iftable_.IsNull());

  // disable the slow paths in FindClass and CreatePrimitiveClass now
  // that Object, Class, and Object[] are setup
  init_done_ = true;

  VLOG(startup) << "ClassLinker::FinishInit exiting";
}

void ClassLinker::RunRootClinits() {
  Thread* self = Thread::Current();
  for (size_t i = 0; i < ClassLinker::kClassRootsMax; ++i) {
    mirror::Class* c = GetClassRoot(ClassRoot(i));
    if (!c->IsArrayClass() && !c->IsPrimitive()) {
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(GetClassRoot(ClassRoot(i))));
      EnsureInitialized(self, h_class, true, true);
      self->AssertNoPendingException();
    }
  }
}

bool ClassLinker::GenerateOatFile(const char* dex_filename,
                                  int oat_fd,
                                  const char* oat_cache_filename,
                                  std::string* error_msg) {
  Locks::mutator_lock_->AssertNotHeld(Thread::Current());  // Avoid starving GC.
  std::string dex2oat(Runtime::Current()->GetCompilerExecutable());

  gc::Heap* heap = Runtime::Current()->GetHeap();
  std::string boot_image_option("--boot-image=");
  if (heap->GetImageSpace() == nullptr) {
    // TODO If we get a dex2dex compiler working we could maybe use that, OTOH since we are likely
    // out of space anyway it might not matter.
    *error_msg = StringPrintf("Cannot create oat file for '%s' because we are running "
                              "without an image.", dex_filename);
    return false;
  }
  boot_image_option += heap->GetImageSpace()->GetImageLocation();

  std::string dex_file_option("--dex-file=");
  dex_file_option += dex_filename;

  std::string oat_fd_option("--oat-fd=");
  StringAppendF(&oat_fd_option, "%d", oat_fd);

  std::string oat_location_option("--oat-location=");
  oat_location_option += oat_cache_filename;

  std::vector<std::string> argv;
  argv.push_back(dex2oat);
  argv.push_back("--runtime-arg");
  argv.push_back("-classpath");
  argv.push_back("--runtime-arg");
  argv.push_back(Runtime::Current()->GetClassPathString());

  Runtime::Current()->AddCurrentRuntimeFeaturesAsDex2OatArguments(&argv);

  if (!Runtime::Current()->IsVerificationEnabled()) {
    argv.push_back("--compiler-filter=verify-none");
  }

  if (Runtime::Current()->MustRelocateIfPossible()) {
    argv.push_back("--runtime-arg");
    argv.push_back("-Xrelocate");
  } else {
    argv.push_back("--runtime-arg");
    argv.push_back("-Xnorelocate");
  }

  if (!kIsTargetBuild) {
    argv.push_back("--host");
  }

  argv.push_back(boot_image_option);
  argv.push_back(dex_file_option);
  argv.push_back(oat_fd_option);
  argv.push_back(oat_location_option);
  const std::vector<std::string>& compiler_options = Runtime::Current()->GetCompilerOptions();
  for (size_t i = 0; i < compiler_options.size(); ++i) {
    argv.push_back(compiler_options[i].c_str());
  }

  return Exec(argv, error_msg);
}

const OatFile* ClassLinker::RegisterOatFile(const OatFile* oat_file) {
  WriterMutexLock mu(Thread::Current(), dex_lock_);
  if (kIsDebugBuild) {
    for (size_t i = 0; i < oat_files_.size(); ++i) {
      CHECK_NE(oat_file, oat_files_[i]) << oat_file->GetLocation();
    }
  }
  VLOG(class_linker) << "Registering " << oat_file->GetLocation();
  oat_files_.push_back(oat_file);
  return oat_file;
}

OatFile& ClassLinker::GetImageOatFile(gc::space::ImageSpace* space) {
  VLOG(startup) << "ClassLinker::GetImageOatFile entering";
  OatFile* oat_file = space->ReleaseOatFile();
  CHECK_EQ(RegisterOatFile(oat_file), oat_file);
  VLOG(startup) << "ClassLinker::GetImageOatFile exiting";
  return *oat_file;
}

const OatFile::OatDexFile* ClassLinker::FindOpenedOatDexFileForDexFile(const DexFile& dex_file) {
  const char* dex_location = dex_file.GetLocation().c_str();
  uint32_t dex_location_checksum = dex_file.GetLocationChecksum();
  return FindOpenedOatDexFile(nullptr, dex_location, &dex_location_checksum);
}

const OatFile::OatDexFile* ClassLinker::FindOpenedOatDexFile(const char* oat_location,
                                                             const char* dex_location,
                                                             const uint32_t* dex_location_checksum) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  for (const OatFile* oat_file : oat_files_) {
    DCHECK(oat_file != nullptr);

    if (oat_location != nullptr) {
      if (oat_file->GetLocation() != oat_location) {
        continue;
      }
    }

    const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location,
                                                                      dex_location_checksum,
                                                                      false);
    if (oat_dex_file != nullptr) {
      return oat_dex_file;
    }
  }
  return nullptr;
}


// Loads all multi dex files from the given oat file returning true on success.
//
// Parameters:
//   oat_file - the oat file to load from
//   dex_location - the dex location used to generate the oat file
//   dex_location_checksum - the checksum of the dex_location (may be null for pre-opted files)
//   generated - whether or not the oat_file existed before or was just (re)generated
//   error_msgs - any error messages will be appended here
//   dex_files - the loaded dex_files will be appended here (only if the loading succeeds)
static bool LoadMultiDexFilesFromOatFile(const OatFile* oat_file,
                                         const char* dex_location,
                                         const uint32_t* dex_location_checksum,
                                         bool generated,
                                         std::vector<std::string>* error_msgs,
                                         std::vector<const DexFile*>* dex_files) {
  if (oat_file == nullptr) {
    return false;
  }

  size_t old_size = dex_files->size();  // To rollback on error.

  bool success = true;
  for (size_t i = 0; success; ++i) {
    std::string next_name_str = DexFile::GetMultiDexClassesDexName(i, dex_location);
    const char* next_name = next_name_str.c_str();

    uint32_t next_location_checksum;
    uint32_t* next_location_checksum_pointer = &next_location_checksum;
    std::string error_msg;
    if ((i == 0) && (strcmp(next_name, dex_location) == 0)) {
      // When i=0 the multidex name should be the same as the location name. We already have the
      // checksum it so we don't need to recompute it.
      if (dex_location_checksum == nullptr) {
        next_location_checksum_pointer = nullptr;
      } else {
        next_location_checksum = *dex_location_checksum;
      }
    } else if (!DexFile::GetChecksum(next_name, next_location_checksum_pointer, &error_msg)) {
      DCHECK_EQ(false, i == 0 && generated);
      next_location_checksum_pointer = nullptr;
    }

    const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(next_name, nullptr, false);

    if (oat_dex_file == nullptr) {
      if (i == 0 && generated) {
        std::string error_msg;
        error_msg = StringPrintf("\nFailed to find dex file '%s' (checksum 0x%x) in generated out "
                                 " file'%s'", dex_location, next_location_checksum,
                                 oat_file->GetLocation().c_str());
        error_msgs->push_back(error_msg);
      }
      break;  // Not found, done.
    }

    // Checksum test. Test must succeed when generated.
    success = !generated;
    if (next_location_checksum_pointer != nullptr) {
      success = next_location_checksum == oat_dex_file->GetDexFileLocationChecksum();
    }

    if (success) {
      const DexFile* dex_file = oat_dex_file->OpenDexFile(&error_msg);
      if (dex_file == nullptr) {
        success = false;
        error_msgs->push_back(error_msg);
      } else {
        dex_files->push_back(dex_file);
      }
    }

    // When we generated the file, we expect success, or something is terribly wrong.
    CHECK_EQ(false, generated && !success)
        << "dex_location=" << next_name << " oat_location=" << oat_file->GetLocation().c_str()
        << std::hex << " dex_location_checksum=" << next_location_checksum
        << " OatDexFile::GetLocationChecksum()=" << oat_dex_file->GetDexFileLocationChecksum();
  }

  if (dex_files->size() == old_size) {
    success = false;  // We did not even find classes.dex
  }

  if (success) {
    return true;
  } else {
    // Free all the dex files we have loaded.
    auto it = dex_files->begin() + old_size;
    auto it_end = dex_files->end();
    for (; it != it_end; it++) {
      delete *it;
    }
    dex_files->erase(dex_files->begin() + old_size, it_end);

    return false;
  }
}

// Multidex files make it possible that some, but not all, dex files can be broken/outdated. This
// complicates the loading process, as we should not use an iterative loading process, because that
// would register the oat file and dex files that come before the broken one. Instead, check all
// multidex ahead of time.
bool ClassLinker::OpenDexFilesFromOat(const char* dex_location, const char* oat_location,
                                      std::vector<std::string>* error_msgs,
                                      std::vector<const DexFile*>* dex_files) {
  // 1) Check whether we have an open oat file.
  // This requires a dex checksum, use the "primary" one.
  uint32_t dex_location_checksum;
  uint32_t* dex_location_checksum_pointer = &dex_location_checksum;
  bool have_checksum = true;
  std::string checksum_error_msg;
  if (!DexFile::GetChecksum(dex_location, dex_location_checksum_pointer, &checksum_error_msg)) {
    // This happens for pre-opted files since the corresponding dex files are no longer on disk.
    dex_location_checksum_pointer = nullptr;
    have_checksum = false;
  }

  bool needs_registering = false;

  const OatFile::OatDexFile* oat_dex_file = FindOpenedOatDexFile(oat_location, dex_location,
                                                                 dex_location_checksum_pointer);
  std::unique_ptr<const OatFile> open_oat_file(
      oat_dex_file != nullptr ? oat_dex_file->GetOatFile() : nullptr);

  // 2) If we do not have an open one, maybe there's one on disk already.

  // In case the oat file is not open, we play a locking game here so
  // that if two different processes race to load and register or generate
  // (or worse, one tries to open a partial generated file) we will be okay.
  // This is actually common with apps that use DexClassLoader to work
  // around the dex method reference limit and that have a background
  // service running in a separate process.
  ScopedFlock scoped_flock;

  if (open_oat_file.get() == nullptr) {
    if (oat_location != nullptr) {
      // Can only do this if we have a checksum, else error.
      if (!have_checksum) {
        error_msgs->push_back(checksum_error_msg);
        return false;
      }

      std::string error_msg;

      // We are loading or creating one in the future. Time to set up the file lock.
      if (!scoped_flock.Init(oat_location, &error_msg)) {
        error_msgs->push_back(error_msg);
        return false;
      }

      // TODO Caller specifically asks for this oat_location. We should honor it. Probably?
      open_oat_file.reset(FindOatFileInOatLocationForDexFile(dex_location, dex_location_checksum,
                                                             oat_location, &error_msg));

      if (open_oat_file.get() == nullptr) {
        std::string compound_msg = StringPrintf("Failed to find dex file '%s' in oat location '%s': %s",
                                                dex_location, oat_location, error_msg.c_str());
        VLOG(class_linker) << compound_msg;
        error_msgs->push_back(compound_msg);
      }
    } else {
      // TODO: What to lock here?
      bool obsolete_file_cleanup_failed;
      open_oat_file.reset(FindOatFileContainingDexFileFromDexLocation(dex_location,
                                                                      dex_location_checksum_pointer,
                                                                      kRuntimeISA, error_msgs,
                                                                      &obsolete_file_cleanup_failed));
      // There's no point in going forward and eventually try to regenerate the
      // file if we couldn't remove the obsolete one. Mostly likely we will fail
      // with the same error when trying to write the new file.
      // TODO: should we maybe do this only when we get permission issues? (i.e. EACCESS).
      if (obsolete_file_cleanup_failed) {
        return false;
      }
    }
    needs_registering = true;
  }

  // 3) If we have an oat file, check all contained multidex files for our dex_location.
  // Note: LoadMultiDexFilesFromOatFile will check for nullptr in the first argument.
  bool success = LoadMultiDexFilesFromOatFile(open_oat_file.get(), dex_location,
                                              dex_location_checksum_pointer,
                                              false, error_msgs, dex_files);
  if (success) {
    const OatFile* oat_file = open_oat_file.release();  // Avoid deleting it.
    if (needs_registering) {
      // We opened the oat file, so we must register it.
      RegisterOatFile(oat_file);
    }
    // If the file isn't executable we failed patchoat but did manage to get the dex files.
    return oat_file->IsExecutable();
  } else {
    if (needs_registering) {
      // We opened it, delete it.
      open_oat_file.reset();
    } else {
      open_oat_file.release();  // Do not delete open oat files.
    }
  }

  // 4) If it's not the case (either no oat file or mismatches), regenerate and load.

  // Need a checksum, fail else.
  if (!have_checksum) {
    error_msgs->push_back(checksum_error_msg);
    return false;
  }

  // Look in cache location if no oat_location is given.
  std::string cache_location;
  if (oat_location == nullptr) {
    // Use the dalvik cache.
    const std::string dalvik_cache(GetDalvikCacheOrDie(GetInstructionSetString(kRuntimeISA)));
    cache_location = GetDalvikCacheFilenameOrDie(dex_location, dalvik_cache.c_str());
    oat_location = cache_location.c_str();
  }

  bool has_flock = true;
  // Definitely need to lock now.
  if (!scoped_flock.HasFile()) {
    std::string error_msg;
    if (!scoped_flock.Init(oat_location, &error_msg)) {
      error_msgs->push_back(error_msg);
      has_flock = false;
    }
  }

  if (Runtime::Current()->IsDex2OatEnabled() && has_flock && scoped_flock.HasFile()) {
    // Create the oat file.
    open_oat_file.reset(CreateOatFileForDexLocation(dex_location, scoped_flock.GetFile()->Fd(),
                                                    oat_location, error_msgs));
  }

  // Failed, bail.
  if (open_oat_file.get() == nullptr) {
    std::string error_msg;
    // dex2oat was disabled or crashed. Add the dex file in the list of dex_files to make progress.
    DexFile::Open(dex_location, dex_location, &error_msg, dex_files);
    error_msgs->push_back(error_msg);
    return false;
  }

  // Try to load again, but stronger checks.
  success = LoadMultiDexFilesFromOatFile(open_oat_file.get(), dex_location,
                                         dex_location_checksum_pointer,
                                         true, error_msgs, dex_files);
  if (success) {
    RegisterOatFile(open_oat_file.release());
    return true;
  } else {
    return false;
  }
}

const OatFile* ClassLinker::FindOatFileInOatLocationForDexFile(const char* dex_location,
                                                               uint32_t dex_location_checksum,
                                                               const char* oat_location,
                                                               std::string* error_msg) {
  std::unique_ptr<OatFile> oat_file(OatFile::Open(oat_location, oat_location, nullptr, nullptr,
                                            !Runtime::Current()->IsCompiler(),
                                            error_msg));
  if (oat_file.get() == nullptr) {
    *error_msg = StringPrintf("Failed to find existing oat file at %s: %s", oat_location,
                              error_msg->c_str());
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  const gc::space::ImageSpace* image_space = runtime->GetHeap()->GetImageSpace();
  if (image_space != nullptr) {
    const ImageHeader& image_header = image_space->GetImageHeader();
    uint32_t expected_image_oat_checksum = image_header.GetOatChecksum();
    uint32_t actual_image_oat_checksum = oat_file->GetOatHeader().GetImageFileLocationOatChecksum();
    if (expected_image_oat_checksum != actual_image_oat_checksum) {
      *error_msg = StringPrintf("Failed to find oat file at '%s' with expected image oat checksum of "
                                "0x%x, found 0x%x", oat_location, expected_image_oat_checksum,
                                actual_image_oat_checksum);
      return nullptr;
    }

    uintptr_t expected_image_oat_offset = reinterpret_cast<uintptr_t>(image_header.GetOatDataBegin());
    uint32_t actual_image_oat_offset = oat_file->GetOatHeader().GetImageFileLocationOatDataBegin();
    if (expected_image_oat_offset != actual_image_oat_offset) {
      *error_msg = StringPrintf("Failed to find oat file at '%s' with expected image oat offset %"
                                PRIuPTR ", found %ud", oat_location, expected_image_oat_offset,
                                actual_image_oat_offset);
      return nullptr;
    }
    int32_t expected_patch_delta = image_header.GetPatchDelta();
    int32_t actual_patch_delta = oat_file->GetOatHeader().GetImagePatchDelta();
    if (expected_patch_delta != actual_patch_delta) {
      *error_msg = StringPrintf("Failed to find oat file at '%s' with expected patch delta %d, "
                                " found %d", oat_location, expected_patch_delta, actual_patch_delta);
      return nullptr;
    }
  }

  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location,
                                                                    &dex_location_checksum);
  if (oat_dex_file == nullptr) {
    *error_msg = StringPrintf("Failed to find oat file at '%s' containing '%s'", oat_location,
                              dex_location);
    return nullptr;
  }
  uint32_t expected_dex_checksum = dex_location_checksum;
  uint32_t actual_dex_checksum = oat_dex_file->GetDexFileLocationChecksum();
  if (expected_dex_checksum != actual_dex_checksum) {
    *error_msg = StringPrintf("Failed to find oat file at '%s' with expected dex checksum of 0x%x, "
                              "found 0x%x", oat_location, expected_dex_checksum,
                              actual_dex_checksum);
    return nullptr;
  }
  std::unique_ptr<const DexFile> dex_file(oat_dex_file->OpenDexFile(error_msg));
  if (dex_file.get() != nullptr) {
    return oat_file.release();
  } else {
    return nullptr;
  }
}

const OatFile* ClassLinker::CreateOatFileForDexLocation(const char* dex_location,
                                                        int fd, const char* oat_location,
                                                        std::vector<std::string>* error_msgs) {
  // Generate the output oat file for the dex file
  VLOG(class_linker) << "Generating oat file " << oat_location << " for " << dex_location;
  std::string error_msg;
  if (!GenerateOatFile(dex_location, fd, oat_location, &error_msg)) {
    CHECK(!error_msg.empty());
    error_msgs->push_back(error_msg);
    return nullptr;
  }
  std::unique_ptr<OatFile> oat_file(OatFile::Open(oat_location, oat_location, nullptr, nullptr,
                                            !Runtime::Current()->IsCompiler(),
                                            &error_msg));
  if (oat_file.get() == nullptr) {
    std::string compound_msg = StringPrintf("\nFailed to open generated oat file '%s': %s",
                                            oat_location, error_msg.c_str());
    error_msgs->push_back(compound_msg);
    return nullptr;
  }

  return oat_file.release();
}

bool ClassLinker::VerifyOatImageChecksum(const OatFile* oat_file,
                                         const InstructionSet instruction_set) {
  Runtime* runtime = Runtime::Current();
  const gc::space::ImageSpace* image_space = runtime->GetHeap()->GetImageSpace();
  if (image_space == nullptr) {
    return false;
  }
  uint32_t image_oat_checksum = 0;
  if (instruction_set == kRuntimeISA) {
    const ImageHeader& image_header = image_space->GetImageHeader();
    image_oat_checksum = image_header.GetOatChecksum();
  } else {
    std::unique_ptr<ImageHeader> image_header(gc::space::ImageSpace::ReadImageHeaderOrDie(
        image_space->GetImageLocation().c_str(), instruction_set));
    image_oat_checksum = image_header->GetOatChecksum();
  }
  return oat_file->GetOatHeader().GetImageFileLocationOatChecksum() == image_oat_checksum;
}

bool ClassLinker::VerifyOatChecksums(const OatFile* oat_file,
                                     const InstructionSet instruction_set,
                                     std::string* error_msg) {
  Runtime* runtime = Runtime::Current();
  const gc::space::ImageSpace* image_space = runtime->GetHeap()->GetImageSpace();
  if (image_space == nullptr) {
    *error_msg = "No image space for verification against";
    return false;
  }

  // If the requested instruction set is the same as the current runtime,
  // we can use the checksums directly. If it isn't, we'll have to read the
  // image header from the image for the right instruction set.
  uint32_t image_oat_checksum = 0;
  uintptr_t image_oat_data_begin = 0;
  int32_t image_patch_delta = 0;
  if (instruction_set == runtime->GetInstructionSet()) {
    const ImageHeader& image_header = image_space->GetImageHeader();
    image_oat_checksum = image_header.GetOatChecksum();
    image_oat_data_begin = reinterpret_cast<uintptr_t>(image_header.GetOatDataBegin());
    image_patch_delta = image_header.GetPatchDelta();
  } else {
    std::unique_ptr<ImageHeader> image_header(gc::space::ImageSpace::ReadImageHeaderOrDie(
        image_space->GetImageLocation().c_str(), instruction_set));
    image_oat_checksum = image_header->GetOatChecksum();
    image_oat_data_begin = reinterpret_cast<uintptr_t>(image_header->GetOatDataBegin());
    image_patch_delta = image_header->GetPatchDelta();
  }
  const OatHeader& oat_header = oat_file->GetOatHeader();
  bool ret = (oat_header.GetImageFileLocationOatChecksum() == image_oat_checksum);

  // If the oat file is PIC, it doesn't care if/how image was relocated. Ignore these checks.
  if (!oat_file->IsPic()) {
    ret = ret && (oat_header.GetImagePatchDelta() == image_patch_delta)
              && (oat_header.GetImageFileLocationOatDataBegin() == image_oat_data_begin);
  }
  if (!ret) {
    *error_msg = StringPrintf("oat file '%s' mismatch (0x%x, %d, %d) with (0x%x, %" PRIdPTR ", %d)",
                              oat_file->GetLocation().c_str(),
                              oat_file->GetOatHeader().GetImageFileLocationOatChecksum(),
                              oat_file->GetOatHeader().GetImageFileLocationOatDataBegin(),
                              oat_file->GetOatHeader().GetImagePatchDelta(),
                              image_oat_checksum, image_oat_data_begin, image_patch_delta);
  }
  return ret;
}

bool ClassLinker::VerifyOatAndDexFileChecksums(const OatFile* oat_file,
                                               const char* dex_location,
                                               uint32_t dex_location_checksum,
                                               const InstructionSet instruction_set,
                                               std::string* error_msg) {
  if (!VerifyOatChecksums(oat_file, instruction_set, error_msg)) {
    return false;
  }

  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location,
                                                                    &dex_location_checksum);
  if (oat_dex_file == nullptr) {
    *error_msg = StringPrintf("oat file '%s' does not contain contents for '%s' with checksum 0x%x",
                              oat_file->GetLocation().c_str(), dex_location, dex_location_checksum);
    for (const OatFile::OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
      *error_msg  += StringPrintf("\noat file '%s' contains contents for '%s' with checksum 0x%x",
                                  oat_file->GetLocation().c_str(),
                                  oat_dex_file->GetDexFileLocation().c_str(),
                                  oat_dex_file->GetDexFileLocationChecksum());
    }
    return false;
  }

  DCHECK_EQ(dex_location_checksum, oat_dex_file->GetDexFileLocationChecksum());
  return true;
}

bool ClassLinker::VerifyOatWithDexFile(const OatFile* oat_file,
                                       const char* dex_location,
                                       const uint32_t* dex_location_checksum,
                                       std::string* error_msg) {
  CHECK(oat_file != nullptr);
  CHECK(dex_location != nullptr);
  std::unique_ptr<const DexFile> dex_file;
  if (dex_location_checksum == nullptr) {
    // If no classes.dex found in dex_location, it has been stripped or is corrupt, assume oat is
    // up-to-date. This is the common case in user builds for jar's and apk's in the /system
    // directory.
    const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location, nullptr);
    if (oat_dex_file == nullptr) {
      *error_msg = StringPrintf("Dex checksum mismatch for location '%s' and failed to find oat "
                                "dex file '%s': %s", oat_file->GetLocation().c_str(), dex_location,
                                error_msg->c_str());
      return false;
    }
    dex_file.reset(oat_dex_file->OpenDexFile(error_msg));
  } else {
    bool verified = VerifyOatAndDexFileChecksums(oat_file, dex_location, *dex_location_checksum,
                                                 kRuntimeISA, error_msg);
    if (!verified) {
      return false;
    }
    dex_file.reset(oat_file->GetOatDexFile(dex_location,
                                           dex_location_checksum)->OpenDexFile(error_msg));
  }
  return dex_file.get() != nullptr;
}

const OatFile* ClassLinker::FindOatFileContainingDexFileFromDexLocation(
    const char* dex_location,
    const uint32_t* dex_location_checksum,
    InstructionSet isa,
    std::vector<std::string>* error_msgs,
    bool* obsolete_file_cleanup_failed) {
  *obsolete_file_cleanup_failed = false;
  bool already_opened = false;
  std::string dex_location_str(dex_location);
  std::unique_ptr<const OatFile> oat_file(OpenOatFileFromDexLocation(dex_location_str, isa,
                                                                     &already_opened,
                                                                     obsolete_file_cleanup_failed,
                                                                     error_msgs));
  std::string error_msg;
  if (oat_file.get() == nullptr) {
    error_msgs->push_back(StringPrintf("Failed to open oat file from dex location '%s'",
                                       dex_location));
    return nullptr;
  } else if (oat_file->IsExecutable() &&
             !VerifyOatWithDexFile(oat_file.get(), dex_location,
                                   dex_location_checksum, &error_msg)) {
    error_msgs->push_back(StringPrintf("Failed to verify oat file '%s' found for dex location "
                                       "'%s': %s", oat_file->GetLocation().c_str(), dex_location,
                                       error_msg.c_str()));
    return nullptr;
  } else if (!oat_file->IsExecutable() &&
             Runtime::Current()->GetHeap()->HasImageSpace() &&
             !VerifyOatImageChecksum(oat_file.get(), isa)) {
    error_msgs->push_back(StringPrintf("Failed to verify non-executable oat file '%s' found for "
                                       "dex location '%s'. Image checksum incorrect.",
                                       oat_file->GetLocation().c_str(), dex_location));
    return nullptr;
  } else {
    return oat_file.release();
  }
}

const OatFile* ClassLinker::FindOpenedOatFileFromOatLocation(const std::string& oat_location) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  for (size_t i = 0; i < oat_files_.size(); i++) {
    const OatFile* oat_file = oat_files_[i];
    DCHECK(oat_file != nullptr);
    if (oat_file->GetLocation() == oat_location) {
      return oat_file;
    }
  }
  return nullptr;
}

const OatFile* ClassLinker::OpenOatFileFromDexLocation(const std::string& dex_location,
                                                       InstructionSet isa,
                                                       bool *already_opened,
                                                       bool *obsolete_file_cleanup_failed,
                                                       std::vector<std::string>* error_msgs) {
  // Find out if we've already opened the file
  const OatFile* ret = nullptr;
  std::string odex_filename(DexFilenameToOdexFilename(dex_location, isa));
  ret = FindOpenedOatFileFromOatLocation(odex_filename);
  if (ret != nullptr) {
    *already_opened = true;
    return ret;
  }

  std::string dalvik_cache;
  bool have_android_data = false;
  bool have_dalvik_cache = false;
  bool is_global_cache = false;
  GetDalvikCache(GetInstructionSetString(kRuntimeISA), false, &dalvik_cache,
                 &have_android_data, &have_dalvik_cache, &is_global_cache);
  std::string cache_filename;
  if (have_dalvik_cache) {
    cache_filename = GetDalvikCacheFilenameOrDie(dex_location.c_str(), dalvik_cache.c_str());
    ret = FindOpenedOatFileFromOatLocation(cache_filename);
    if (ret != nullptr) {
      *already_opened = true;
      return ret;
    }
  } else {
    // If we need to relocate we should just place odex back where it started.
    cache_filename = odex_filename;
  }

  ret = nullptr;

  // We know that neither the odex nor the cache'd version is already in use, if it even exists.
  //
  // Now we do the following:
  // 1) Try and open the odex version
  // 2) If present, checksum-verified & relocated correctly return it
  // 3) Close the odex version to free up its address space.
  // 4) Try and open the cache version
  // 5) If present, checksum-verified & relocated correctly return it
  // 6) Close the cache version to free up its address space.
  // 7) If we should relocate:
  //   a) If we have opened and checksum-verified the odex version relocate it to
  //      'cache_filename' and return it
  //   b) If we have opened and checksum-verified the cache version relocate it in place and return
  //      it. This should not happen often (I think only the run-test's will hit this case).
  // 8) If the cache-version was present we should delete it since it must be obsolete if we get to
  //    this point.
  // 9) Return nullptr

  *already_opened = false;
  const Runtime* runtime = Runtime::Current();
  CHECK(runtime != nullptr);
  bool executable = !runtime->IsCompiler();

  std::string odex_error_msg;
  bool should_patch_system = false;
  bool odex_checksum_verified = false;
  bool have_system_odex = false;
  {
    // There is a high probability that both these oat files map similar/the same address
    // spaces so we must scope them like this so they each gets its turn.
    std::unique_ptr<OatFile> odex_oat_file(OatFile::Open(odex_filename, odex_filename, nullptr,
                                                         nullptr,
                                                         executable, &odex_error_msg));
    if (odex_oat_file.get() != nullptr && CheckOatFile(runtime, odex_oat_file.get(), isa,
                                                       &odex_checksum_verified,
                                                       &odex_error_msg)) {
      return odex_oat_file.release();
    } else {
      if (odex_checksum_verified) {
        // We can just relocate
        should_patch_system = true;
        odex_error_msg = "Image Patches are incorrect";
      }
      if (odex_oat_file.get() != nullptr) {
        have_system_odex = true;
      }
    }
  }

  std::string cache_error_msg;
  bool should_patch_cache = false;
  bool cache_checksum_verified = false;
  if (have_dalvik_cache) {
    std::unique_ptr<OatFile> cache_oat_file(OatFile::Open(cache_filename, cache_filename, nullptr,
                                                          nullptr,
                                                          executable, &cache_error_msg));
    if (cache_oat_file.get() != nullptr && CheckOatFile(runtime, cache_oat_file.get(), isa,
                                                        &cache_checksum_verified,
                                                        &cache_error_msg)) {
      return cache_oat_file.release();
    } else if (cache_checksum_verified) {
      // We can just relocate
      should_patch_cache = true;
      cache_error_msg = "Image Patches are incorrect";
    }
  } else if (have_android_data) {
    // dalvik_cache does not exist but android data does. This means we should be able to create
    // it, so we should try.
    GetDalvikCacheOrDie(GetInstructionSetString(kRuntimeISA), true);
  }

  ret = nullptr;
  std::string error_msg;
  if (runtime->CanRelocate()) {
    // Run relocation
    gc::space::ImageSpace* space = Runtime::Current()->GetHeap()->GetImageSpace();
    if (space != nullptr) {
      const std::string& image_location = space->GetImageLocation();
      if (odex_checksum_verified && should_patch_system) {
        ret = PatchAndRetrieveOat(odex_filename, cache_filename, image_location, isa, &error_msg);
      } else if (cache_checksum_verified && should_patch_cache) {
        CHECK(have_dalvik_cache);
        ret = PatchAndRetrieveOat(cache_filename, cache_filename, image_location, isa, &error_msg);
      }
    } else if (have_system_odex) {
      ret = GetInterpretedOnlyOat(odex_filename, isa, &error_msg);
    }
  }
  if (ret == nullptr && have_dalvik_cache && OS::FileExists(cache_filename.c_str())) {
    // implicitly: were able to fine where the cached version is but we were unable to use it,
    // either as a destination for relocation or to open a file. We should delete it if it is
    // there.
    if (TEMP_FAILURE_RETRY(unlink(cache_filename.c_str())) != 0) {
      std::string rm_error_msg = StringPrintf("Failed to remove obsolete file from %s when "
                                              "searching for dex file %s: %s",
                                              cache_filename.c_str(), dex_location.c_str(),
                                              strerror(errno));
      error_msgs->push_back(rm_error_msg);
      VLOG(class_linker) << rm_error_msg;
      // Let the caller know that we couldn't remove the obsolete file.
      // This is a good indication that further writes may fail as well.
      *obsolete_file_cleanup_failed = true;
    }
  }
  if (ret == nullptr) {
    VLOG(class_linker) << error_msg;
    error_msgs->push_back(error_msg);
    std::string relocation_msg;
    if (runtime->CanRelocate()) {
      relocation_msg = StringPrintf(" and relocation failed");
    }
    if (have_dalvik_cache && cache_checksum_verified) {
      error_msg = StringPrintf("Failed to open oat file from %s (error %s) or %s "
                                "(error %s)%s.", odex_filename.c_str(), odex_error_msg.c_str(),
                                cache_filename.c_str(), cache_error_msg.c_str(),
                                relocation_msg.c_str());
    } else {
      error_msg = StringPrintf("Failed to open oat file from %s (error %s) (no "
                               "dalvik_cache availible)%s.", odex_filename.c_str(),
                               odex_error_msg.c_str(), relocation_msg.c_str());
    }
    VLOG(class_linker) << error_msg;
    error_msgs->push_back(error_msg);
  }
  return ret;
}

const OatFile* ClassLinker::GetInterpretedOnlyOat(const std::string& oat_path,
                                                  InstructionSet isa,
                                                  std::string* error_msg) {
  // We open it non-executable
  std::unique_ptr<OatFile> output(OatFile::Open(oat_path, oat_path, nullptr, nullptr, false, error_msg));
  if (output.get() == nullptr) {
    return nullptr;
  }
  if (!Runtime::Current()->GetHeap()->HasImageSpace() ||
      VerifyOatImageChecksum(output.get(), isa)) {
    return output.release();
  } else {
    *error_msg = StringPrintf("Could not use oat file '%s', image checksum failed to verify.",
                              oat_path.c_str());
    return nullptr;
  }
}

const OatFile* ClassLinker::PatchAndRetrieveOat(const std::string& input_oat,
                                                const std::string& output_oat,
                                                const std::string& image_location,
                                                InstructionSet isa,
                                                std::string* error_msg) {
  Runtime* runtime = Runtime::Current();
  DCHECK(runtime != nullptr);
  if (!runtime->GetHeap()->HasImageSpace()) {
    // We don't have an image space so there is no point in trying to patchoat.
    LOG(WARNING) << "Patching of oat file '" << input_oat << "' not attempted because we are "
                 << "running without an image. Attempting to use oat file for interpretation.";
    return GetInterpretedOnlyOat(input_oat, isa, error_msg);
  }
  if (!runtime->IsDex2OatEnabled()) {
    // We don't have dex2oat so we can assume we don't have patchoat either. We should just use the
    // input_oat but make sure we only do interpretation on it's dex files.
    LOG(WARNING) << "Patching of oat file '" << input_oat << "' not attempted due to dex2oat being "
                 << "disabled. Attempting to use oat file for interpretation";
    return GetInterpretedOnlyOat(input_oat, isa, error_msg);
  }
  Locks::mutator_lock_->AssertNotHeld(Thread::Current());  // Avoid starving GC.
  std::string patchoat(runtime->GetPatchoatExecutable());

  std::string isa_arg("--instruction-set=");
  isa_arg += GetInstructionSetString(isa);
  std::string input_oat_filename_arg("--input-oat-file=");
  input_oat_filename_arg += input_oat;
  std::string output_oat_filename_arg("--output-oat-file=");
  output_oat_filename_arg += output_oat;
  std::string patched_image_arg("--patched-image-location=");
  patched_image_arg += image_location;

  std::vector<std::string> argv;
  argv.push_back(patchoat);
  argv.push_back(isa_arg);
  argv.push_back(input_oat_filename_arg);
  argv.push_back(output_oat_filename_arg);
  argv.push_back(patched_image_arg);

  std::string command_line(Join(argv, ' '));
  LOG(INFO) << "Relocate Oat File: " << command_line;
  bool success = Exec(argv, error_msg);
  if (success) {
    std::unique_ptr<OatFile> output(OatFile::Open(output_oat, output_oat, nullptr, nullptr,
                                                  !runtime->IsCompiler(), error_msg));
    bool checksum_verified = false;
    if (output.get() != nullptr && CheckOatFile(runtime, output.get(), isa, &checksum_verified,
                                                error_msg)) {
      return output.release();
    } else if (output.get() != nullptr) {
      *error_msg = StringPrintf("Patching of oat file '%s' succeeded "
                                "but output file '%s' failed verifcation: %s",
                                input_oat.c_str(), output_oat.c_str(), error_msg->c_str());
    } else {
      *error_msg = StringPrintf("Patching of oat file '%s' succeeded "
                                "but was unable to open output file '%s': %s",
                                input_oat.c_str(), output_oat.c_str(), error_msg->c_str());
    }
  } else if (!runtime->IsCompiler()) {
    // patchoat failed which means we probably don't have enough room to place the output oat file,
    // instead of failing we should just run the interpreter from the dex files in the input oat.
    LOG(WARNING) << "Patching of oat file '" << input_oat << "' failed. Attempting to use oat file "
                 << "for interpretation. patchoat failure was: " << *error_msg;
    return GetInterpretedOnlyOat(input_oat, isa, error_msg);
  } else {
    *error_msg = StringPrintf("Patching of oat file '%s to '%s' "
                              "failed: %s", input_oat.c_str(), output_oat.c_str(),
                              error_msg->c_str());
  }
  return nullptr;
}

bool ClassLinker::CheckOatFile(const Runtime* runtime, const OatFile* oat_file, InstructionSet isa,
                               bool* checksum_verified,
                               std::string* error_msg) {
  const gc::space::ImageSpace* image_space = runtime->GetHeap()->GetImageSpace();
  if (image_space == nullptr) {
    *error_msg = "No image space present";
    return false;
  }
  uint32_t real_image_checksum;
  void* real_image_oat_offset;
  int32_t real_patch_delta;
  if (isa == runtime->GetInstructionSet()) {
    const ImageHeader& image_header = image_space->GetImageHeader();
    real_image_checksum = image_header.GetOatChecksum();
    real_image_oat_offset = image_header.GetOatDataBegin();
    real_patch_delta = image_header.GetPatchDelta();
  } else {
    std::unique_ptr<ImageHeader> image_header(gc::space::ImageSpace::ReadImageHeaderOrDie(
        image_space->GetImageLocation().c_str(), isa));
    real_image_checksum = image_header->GetOatChecksum();
    real_image_oat_offset = image_header->GetOatDataBegin();
    real_patch_delta = image_header->GetPatchDelta();
  }

  const OatHeader& oat_header = oat_file->GetOatHeader();
  std::string compound_msg;

  uint32_t oat_image_checksum = oat_header.GetImageFileLocationOatChecksum();
  *checksum_verified = oat_image_checksum == real_image_checksum;
  if (!*checksum_verified) {
    StringAppendF(&compound_msg, " Oat Image Checksum Incorrect (expected 0x%x, received 0x%x)",
                  real_image_checksum, oat_image_checksum);
  }

  bool offset_verified;
  bool patch_delta_verified;

  if (!oat_file->IsPic()) {
    // If an oat file is not PIC, we need to check that the image is at the expected location and
    // patched in the same way.
    void* oat_image_oat_offset =
        reinterpret_cast<void*>(oat_header.GetImageFileLocationOatDataBegin());
    offset_verified = oat_image_oat_offset == real_image_oat_offset;
    if (!offset_verified) {
      StringAppendF(&compound_msg, " Oat Image oat offset incorrect (expected 0x%p, received 0x%p)",
                    real_image_oat_offset, oat_image_oat_offset);
    }

    int32_t oat_patch_delta = oat_header.GetImagePatchDelta();
    patch_delta_verified = oat_patch_delta == real_patch_delta;
    if (!patch_delta_verified) {
      StringAppendF(&compound_msg, " Oat image patch delta incorrect (expected 0x%x, "
                    "received 0x%x)", real_patch_delta, oat_patch_delta);
    }
  } else {
    // If an oat file is PIC, we ignore offset and patching delta.
    offset_verified = true;
    patch_delta_verified = true;
  }

  bool ret = (*checksum_verified && offset_verified && patch_delta_verified);
  if (!ret) {
    *error_msg = "Oat file failed to verify:" + compound_msg;
  }
  return ret;
}

const OatFile* ClassLinker::FindOatFileFromOatLocation(const std::string& oat_location,
                                                       std::string* error_msg) {
  const OatFile* oat_file = FindOpenedOatFileFromOatLocation(oat_location);
  if (oat_file != nullptr) {
    return oat_file;
  }

  return OatFile::Open(oat_location, oat_location, nullptr, nullptr, !Runtime::Current()->IsCompiler(),
                       error_msg);
}

static void InitFromImageInterpretOnlyCallback(mirror::Object* obj, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ClassLinker* class_linker = reinterpret_cast<ClassLinker*>(arg);

  DCHECK(obj != nullptr);
  DCHECK(class_linker != nullptr);

  if (obj->IsArtMethod()) {
    mirror::ArtMethod* method = obj->AsArtMethod();
    if (!method->IsNative()) {
      method->SetEntryPointFromInterpreter(artInterpreterToInterpreterBridge);
      if (method != Runtime::Current()->GetResolutionMethod()) {
        method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
        method->SetEntryPointFromPortableCompiledCode(GetPortableToInterpreterBridge());
      }
    }
  }
}

void ClassLinker::InitFromImage() {
  VLOG(startup) << "ClassLinker::InitFromImage entering";
  CHECK(!init_done_);

  Thread* self = Thread::Current();
  gc::Heap* heap = Runtime::Current()->GetHeap();
  gc::space::ImageSpace* space = heap->GetImageSpace();
  dex_cache_image_class_lookup_required_ = true;
  CHECK(space != nullptr);
  OatFile& oat_file = GetImageOatFile(space);
  CHECK_EQ(oat_file.GetOatHeader().GetImageFileLocationOatChecksum(), 0U);
  CHECK_EQ(oat_file.GetOatHeader().GetImageFileLocationOatDataBegin(), 0U);
  const char* image_file_location = oat_file.GetOatHeader().
      GetStoreValueByKey(OatHeader::kImageLocationKey);
  CHECK(image_file_location == nullptr || *image_file_location == 0);
  portable_resolution_trampoline_ = oat_file.GetOatHeader().GetPortableResolutionTrampoline();
  quick_resolution_trampoline_ = oat_file.GetOatHeader().GetQuickResolutionTrampoline();
  portable_imt_conflict_trampoline_ = oat_file.GetOatHeader().GetPortableImtConflictTrampoline();
  quick_imt_conflict_trampoline_ = oat_file.GetOatHeader().GetQuickImtConflictTrampoline();
  quick_generic_jni_trampoline_ = oat_file.GetOatHeader().GetQuickGenericJniTrampoline();
  quick_to_interpreter_bridge_trampoline_ = oat_file.GetOatHeader().GetQuickToInterpreterBridge();
  mirror::Object* dex_caches_object = space->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
  mirror::ObjectArray<mirror::DexCache>* dex_caches =
      dex_caches_object->AsObjectArray<mirror::DexCache>();

  StackHandleScope<1> hs(self);
  Handle<mirror::ObjectArray<mirror::Class>> class_roots(hs.NewHandle(
          space->GetImageHeader().GetImageRoot(ImageHeader::kClassRoots)->
          AsObjectArray<mirror::Class>()));
  class_roots_ = GcRoot<mirror::ObjectArray<mirror::Class>>(class_roots.Get());

  // Special case of setting up the String class early so that we can test arbitrary objects
  // as being Strings or not
  mirror::String::SetClass(GetClassRoot(kJavaLangString));

  CHECK_EQ(oat_file.GetOatHeader().GetDexFileCount(),
           static_cast<uint32_t>(dex_caches->GetLength()));
  for (int32_t i = 0; i < dex_caches->GetLength(); i++) {
    StackHandleScope<1> hs(self);
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(dex_caches->Get(i)));
    const std::string& dex_file_location(dex_cache->GetLocation()->ToModifiedUtf8());
    const OatFile::OatDexFile* oat_dex_file = oat_file.GetOatDexFile(dex_file_location.c_str(),
                                                                     nullptr);
    CHECK(oat_dex_file != nullptr) << oat_file.GetLocation() << " " << dex_file_location;
    std::string error_msg;
    const DexFile* dex_file = oat_dex_file->OpenDexFile(&error_msg);
    if (dex_file == nullptr) {
      LOG(FATAL) << "Failed to open dex file " << dex_file_location
                 << " from within oat file " << oat_file.GetLocation()
                 << " error '" << error_msg << "'";
    }

    CHECK_EQ(dex_file->GetLocationChecksum(), oat_dex_file->GetDexFileLocationChecksum());

    AppendToBootClassPath(*dex_file, dex_cache);
  }

  // Set classes on AbstractMethod early so that IsMethod tests can be performed during the live
  // bitmap walk.
  mirror::ArtMethod::SetClass(GetClassRoot(kJavaLangReflectArtMethod));

  // Set entry point to interpreter if in InterpretOnly mode.
  if (Runtime::Current()->GetInstrumentation()->InterpretOnly()) {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    heap->VisitObjects(InitFromImageInterpretOnlyCallback, this);
  }

  // reinit class_roots_
  mirror::Class::SetClassClass(class_roots->Get(kJavaLangClass));
  class_roots_ = GcRoot<mirror::ObjectArray<mirror::Class>>(class_roots.Get());

  // reinit array_iftable_ from any array class instance, they should be ==
  array_iftable_ = GcRoot<mirror::IfTable>(GetClassRoot(kObjectArrayClass)->GetIfTable());
  DCHECK(array_iftable_.Read() == GetClassRoot(kBooleanArrayClass)->GetIfTable());
  // String class root was set above
  mirror::Reference::SetClass(GetClassRoot(kJavaLangRefReference));
  mirror::ArtField::SetClass(GetClassRoot(kJavaLangReflectArtField));
  mirror::BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));
  mirror::ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));
  mirror::CharArray::SetArrayClass(GetClassRoot(kCharArrayClass));
  mirror::DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));
  mirror::FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));
  mirror::IntArray::SetArrayClass(GetClassRoot(kIntArrayClass));
  mirror::LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));
  mirror::ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));
  mirror::Throwable::SetClass(GetClassRoot(kJavaLangThrowable));
  mirror::StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit(self);

  VLOG(startup) << "ClassLinker::InitFromImage exiting";
}

void ClassLinker::VisitClassRoots(RootCallback* callback, void* arg, VisitRootFlags flags) {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  if ((flags & kVisitRootFlagAllRoots) != 0) {
    for (std::pair<const size_t, GcRoot<mirror::Class> >& it : class_table_) {
      it.second.VisitRoot(callback, arg, 0, kRootStickyClass);
    }
  } else if ((flags & kVisitRootFlagNewRoots) != 0) {
    for (auto& pair : new_class_roots_) {
      mirror::Class* old_ref = pair.second.Read<kWithoutReadBarrier>();
      pair.second.VisitRoot(callback, arg, 0, kRootStickyClass);
      mirror::Class* new_ref = pair.second.Read<kWithoutReadBarrier>();
      if (UNLIKELY(new_ref != old_ref)) {
        // Uh ohes, GC moved a root in the log. Need to search the class_table and update the
        // corresponding object. This is slow, but luckily for us, this may only happen with a
        // concurrent moving GC.
        for (auto it = class_table_.lower_bound(pair.first), end = class_table_.end();
            it != end && it->first == pair.first; ++it) {
          // If the class stored matches the old class, update it to the new value.
          if (old_ref == it->second.Read<kWithoutReadBarrier>()) {
            it->second = GcRoot<mirror::Class>(new_ref);
          }
        }
      }
    }
  }
  if ((flags & kVisitRootFlagClearRootLog) != 0) {
    new_class_roots_.clear();
  }
  if ((flags & kVisitRootFlagStartLoggingNewRoots) != 0) {
    log_new_class_table_roots_ = true;
  } else if ((flags & kVisitRootFlagStopLoggingNewRoots) != 0) {
    log_new_class_table_roots_ = false;
  }
  // We deliberately ignore the class roots in the image since we
  // handle image roots by using the MS/CMS rescanning of dirty cards.
}

// Keep in sync with InitCallback. Anything we visit, we need to
// reinit references to when reinitializing a ClassLinker from a
// mapped image.
void ClassLinker::VisitRoots(RootCallback* callback, void* arg, VisitRootFlags flags) {
  class_roots_.VisitRoot(callback, arg, 0, kRootVMInternal);
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, dex_lock_);
    if ((flags & kVisitRootFlagAllRoots) != 0) {
      for (GcRoot<mirror::DexCache>& dex_cache : dex_caches_) {
        dex_cache.VisitRoot(callback, arg, 0, kRootVMInternal);
      }
    } else if ((flags & kVisitRootFlagNewRoots) != 0) {
      for (size_t index : new_dex_cache_roots_) {
        dex_caches_[index].VisitRoot(callback, arg, 0, kRootVMInternal);
      }
    }
    if ((flags & kVisitRootFlagClearRootLog) != 0) {
      new_dex_cache_roots_.clear();
    }
    if ((flags & kVisitRootFlagStartLoggingNewRoots) != 0) {
      log_new_dex_caches_roots_ = true;
    } else if ((flags & kVisitRootFlagStopLoggingNewRoots) != 0) {
      log_new_dex_caches_roots_ = false;
    }
  }
  VisitClassRoots(callback, arg, flags);
  array_iftable_.VisitRoot(callback, arg, 0, kRootVMInternal);
  DCHECK(!array_iftable_.IsNull());
  for (size_t i = 0; i < kFindArrayCacheSize; ++i) {
    if (!find_array_class_cache_[i].IsNull()) {
      find_array_class_cache_[i].VisitRoot(callback, arg, 0, kRootVMInternal);
    }
  }
}

void ClassLinker::VisitClasses(ClassVisitor* visitor, void* arg) {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  // TODO: why isn't this a ReaderMutexLock?
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  for (std::pair<const size_t, GcRoot<mirror::Class> >& it : class_table_) {
    mirror::Class* c = it.second.Read();
    if (!visitor(c, arg)) {
      return;
    }
  }
}

static bool GetClassesVisitorSet(mirror::Class* c, void* arg) {
  std::set<mirror::Class*>* classes = reinterpret_cast<std::set<mirror::Class*>*>(arg);
  classes->insert(c);
  return true;
}

struct GetClassesVisitorArrayArg {
  Handle<mirror::ObjectArray<mirror::Class>>* classes;
  int32_t index;
  bool success;
};

static bool GetClassesVisitorArray(mirror::Class* c, void* varg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  GetClassesVisitorArrayArg* arg = reinterpret_cast<GetClassesVisitorArrayArg*>(varg);
  if (arg->index < (*arg->classes)->GetLength()) {
    (*arg->classes)->Set(arg->index, c);
    arg->index++;
    return true;
  } else {
    arg->success = false;
    return false;
  }
}

void ClassLinker::VisitClassesWithoutClassesLock(ClassVisitor* visitor, void* arg) {
  // TODO: it may be possible to avoid secondary storage if we iterate over dex caches. The problem
  // is avoiding duplicates.
  if (!kMovingClasses) {
    std::set<mirror::Class*> classes;
    VisitClasses(GetClassesVisitorSet, &classes);
    for (mirror::Class* klass : classes) {
      if (!visitor(klass, arg)) {
        return;
      }
    }
  } else {
    Thread* self = Thread::Current();
    StackHandleScope<1> hs(self);
    MutableHandle<mirror::ObjectArray<mirror::Class>> classes =
        hs.NewHandle<mirror::ObjectArray<mirror::Class>>(nullptr);
    GetClassesVisitorArrayArg local_arg;
    local_arg.classes = &classes;
    local_arg.success = false;
    // We size the array assuming classes won't be added to the class table during the visit.
    // If this assumption fails we iterate again.
    while (!local_arg.success) {
      size_t class_table_size;
      {
        ReaderMutexLock mu(self, *Locks::classlinker_classes_lock_);
        class_table_size = class_table_.size();
      }
      mirror::Class* class_type = mirror::Class::GetJavaLangClass();
      mirror::Class* array_of_class = FindArrayClass(self, &class_type);
      classes.Assign(
          mirror::ObjectArray<mirror::Class>::Alloc(self, array_of_class, class_table_size));
      CHECK(classes.Get() != nullptr);  // OOME.
      local_arg.index = 0;
      local_arg.success = true;
      VisitClasses(GetClassesVisitorArray, &local_arg);
    }
    for (int32_t i = 0; i < classes->GetLength(); ++i) {
      // If the class table shrank during creation of the clases array we expect null elements. If
      // the class table grew then the loop repeats. If classes are created after the loop has
      // finished then we don't visit.
      mirror::Class* klass = classes->Get(i);
      if (klass != nullptr && !visitor(klass, arg)) {
        return;
      }
    }
  }
}

ClassLinker::~ClassLinker() {
  mirror::Class::ResetClass();
  mirror::String::ResetClass();
  mirror::Reference::ResetClass();
  mirror::ArtField::ResetClass();
  mirror::ArtMethod::ResetClass();
  mirror::BooleanArray::ResetArrayClass();
  mirror::ByteArray::ResetArrayClass();
  mirror::CharArray::ResetArrayClass();
  mirror::DoubleArray::ResetArrayClass();
  mirror::FloatArray::ResetArrayClass();
  mirror::IntArray::ResetArrayClass();
  mirror::LongArray::ResetArrayClass();
  mirror::ShortArray::ResetArrayClass();
  mirror::Throwable::ResetClass();
  mirror::StackTraceElement::ResetClass();
  STLDeleteElements(&boot_class_path_);
  STLDeleteElements(&oat_files_);
}

mirror::DexCache* ClassLinker::AllocDexCache(Thread* self, const DexFile& dex_file) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  StackHandleScope<16> hs(self);
  Handle<mirror::Class> dex_cache_class(hs.NewHandle(GetClassRoot(kJavaLangDexCache)));
  Handle<mirror::DexCache> dex_cache(
      hs.NewHandle(down_cast<mirror::DexCache*>(
          heap->AllocObject<true>(self, dex_cache_class.Get(), dex_cache_class->GetObjectSize(),
                                  VoidFunctor()))));
  if (dex_cache.Get() == nullptr) {
    return nullptr;
  }
  Handle<mirror::String>
      location(hs.NewHandle(intern_table_->InternStrong(dex_file.GetLocation().c_str())));
  if (location.Get() == nullptr) {
    return nullptr;
  }
  Handle<mirror::ObjectArray<mirror::String>>
      strings(hs.NewHandle(AllocStringArray(self, dex_file.NumStringIds())));
  if (strings.Get() == nullptr) {
    return nullptr;
  }
  Handle<mirror::ObjectArray<mirror::Class>>
      types(hs.NewHandle(AllocClassArray(self, dex_file.NumTypeIds())));
  if (types.Get() == nullptr) {
    return nullptr;
  }
  Handle<mirror::ObjectArray<mirror::ArtMethod>>
      methods(hs.NewHandle(AllocArtMethodArray(self, dex_file.NumMethodIds())));
  if (methods.Get() == nullptr) {
    return nullptr;
  }
  Handle<mirror::ObjectArray<mirror::ArtField>>
      fields(hs.NewHandle(AllocArtFieldArray(self, dex_file.NumFieldIds())));
  if (fields.Get() == nullptr) {
    return nullptr;
  }
  dex_cache->Init(&dex_file, location.Get(), strings.Get(), types.Get(), methods.Get(),
                  fields.Get());
  return dex_cache.Get();
}

mirror::Class* ClassLinker::AllocClass(Thread* self, mirror::Class* java_lang_Class,
                                       uint32_t class_size) {
  DCHECK_GE(class_size, sizeof(mirror::Class));
  gc::Heap* heap = Runtime::Current()->GetHeap();
  mirror::Class::InitializeClassVisitor visitor(class_size);
  mirror::Object* k = kMovingClasses ?
      heap->AllocObject<true>(self, java_lang_Class, class_size, visitor) :
      heap->AllocNonMovableObject<true>(self, java_lang_Class, class_size, visitor);
  if (UNLIKELY(k == nullptr)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return nullptr;
  }
  return k->AsClass();
}

mirror::Class* ClassLinker::AllocClass(Thread* self, uint32_t class_size) {
  return AllocClass(self, GetClassRoot(kJavaLangClass), class_size);
}

mirror::ArtField* ClassLinker::AllocArtField(Thread* self) {
  return down_cast<mirror::ArtField*>(
      GetClassRoot(kJavaLangReflectArtField)->AllocNonMovableObject(self));
}

mirror::ArtMethod* ClassLinker::AllocArtMethod(Thread* self) {
  return down_cast<mirror::ArtMethod*>(
      GetClassRoot(kJavaLangReflectArtMethod)->AllocNonMovableObject(self));
}

mirror::ObjectArray<mirror::StackTraceElement>* ClassLinker::AllocStackTraceElementArray(
    Thread* self, size_t length) {
  return mirror::ObjectArray<mirror::StackTraceElement>::Alloc(
      self, GetClassRoot(kJavaLangStackTraceElementArrayClass), length);
}

mirror::Class* ClassLinker::EnsureResolved(Thread* self, const char* descriptor,
                                           mirror::Class* klass) {
  DCHECK(klass != nullptr);

  // For temporary classes we must wait for them to be retired.
  if (init_done_ && klass->IsTemp()) {
    CHECK(!klass->IsResolved());
    if (klass->IsErroneous()) {
      ThrowEarlierClassFailure(klass);
      return nullptr;
    }
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(klass));
    ObjectLock<mirror::Class> lock(self, h_class);
    // Loop and wait for the resolving thread to retire this class.
    while (!h_class->IsRetired() && !h_class->IsErroneous()) {
      lock.WaitIgnoringInterrupts();
    }
    if (h_class->IsErroneous()) {
      ThrowEarlierClassFailure(h_class.Get());
      return nullptr;
    }
    CHECK(h_class->IsRetired());
    // Get the updated class from class table.
    klass = LookupClass(self, descriptor, h_class.Get()->GetClassLoader());
  }

  // Wait for the class if it has not already been linked.
  if (!klass->IsResolved() && !klass->IsErroneous()) {
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Class> h_class(hs.NewHandleWrapper(&klass));
    ObjectLock<mirror::Class> lock(self, h_class);
    // Check for circular dependencies between classes.
    if (!h_class->IsResolved() && h_class->GetClinitThreadId() == self->GetTid()) {
      ThrowClassCircularityError(h_class.Get());
      h_class->SetStatus(mirror::Class::kStatusError, self);
      return nullptr;
    }
    // Wait for the pending initialization to complete.
    while (!h_class->IsResolved() && !h_class->IsErroneous()) {
      lock.WaitIgnoringInterrupts();
    }
  }

  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass);
    return nullptr;
  }
  // Return the loaded class.  No exceptions should be pending.
  CHECK(klass->IsResolved()) << PrettyClass(klass);
  self->AssertNoPendingException();
  return klass;
}

typedef std::pair<const DexFile*, const DexFile::ClassDef*> ClassPathEntry;

// Search a collection of DexFiles for a descriptor
ClassPathEntry FindInClassPath(const char* descriptor,
                               const std::vector<const DexFile*>& class_path) {
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor);
    if (dex_class_def != nullptr) {
      return ClassPathEntry(dex_file, dex_class_def);
    }
  }
  // TODO: remove reinterpret_cast when issue with -std=gnu++0x host issue resolved
  return ClassPathEntry(static_cast<const DexFile*>(nullptr),
                        static_cast<const DexFile::ClassDef*>(nullptr));
}

mirror::Class* ClassLinker::FindClassInPathClassLoader(ScopedObjectAccessAlreadyRunnable& soa,
                                                       Thread* self, const char* descriptor,
                                                       Handle<mirror::ClassLoader> class_loader) {
  if (class_loader->GetClass() !=
      soa.Decode<mirror::Class*>(WellKnownClasses::dalvik_system_PathClassLoader) ||
      class_loader->GetParent()->GetClass() !=
          soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_BootClassLoader)) {
    return nullptr;
  }
  ClassPathEntry pair = FindInClassPath(descriptor, boot_class_path_);
  // Check if this would be found in the parent boot class loader.
  if (pair.second != nullptr) {
    mirror::Class* klass = LookupClass(self, descriptor, nullptr);
    if (klass != nullptr) {
      return EnsureResolved(self, descriptor, klass);
    }
    klass = DefineClass(self, descriptor, NullHandle<mirror::ClassLoader>(), *pair.first,
                        *pair.second);
    if (klass != nullptr) {
      return klass;
    }
    CHECK(self->IsExceptionPending()) << descriptor;
    self->ClearException();
  } else {
    // RegisterDexFile may allocate dex caches (and cause thread suspension).
    StackHandleScope<3> hs(self);
    // The class loader is a PathClassLoader which inherits from BaseDexClassLoader.
    // We need to get the DexPathList and loop through it.
    Handle<mirror::ArtField> cookie_field =
        hs.NewHandle(soa.DecodeField(WellKnownClasses::dalvik_system_DexFile_cookie));
    Handle<mirror::ArtField> dex_file_field =
        hs.NewHandle(
            soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList__Element_dexFile));
    mirror::Object* dex_path_list =
        soa.DecodeField(WellKnownClasses::dalvik_system_PathClassLoader_pathList)->
        GetObject(class_loader.Get());
    if (dex_path_list != nullptr && dex_file_field.Get() != nullptr &&
        cookie_field.Get() != nullptr) {
      // DexPathList has an array dexElements of Elements[] which each contain a dex file.
      mirror::Object* dex_elements_obj =
          soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList_dexElements)->
          GetObject(dex_path_list);
      // Loop through each dalvik.system.DexPathList$Element's dalvik.system.DexFile and look
      // at the mCookie which is a DexFile vector.
      if (dex_elements_obj != nullptr) {
        Handle<mirror::ObjectArray<mirror::Object>> dex_elements =
            hs.NewHandle(dex_elements_obj->AsObjectArray<mirror::Object>());
        for (int32_t i = 0; i < dex_elements->GetLength(); ++i) {
          mirror::Object* element = dex_elements->GetWithoutChecks(i);
          if (element == nullptr) {
            // Should never happen, fall back to java code to throw a NPE.
            break;
          }
          mirror::Object* dex_file = dex_file_field->GetObject(element);
          if (dex_file != nullptr) {
            const uint64_t cookie = cookie_field->GetLong(dex_file);
            auto* dex_files =
                reinterpret_cast<std::vector<const DexFile*>*>(static_cast<uintptr_t>(cookie));
            if (dex_files == nullptr) {
              // This should never happen so log a warning.
              LOG(WARNING) << "Null DexFile::mCookie for " << descriptor;
              break;
            }
            for (const DexFile* dex_file : *dex_files) {
              const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor);
              if (dex_class_def != nullptr) {
                RegisterDexFile(*dex_file);
                mirror::Class* klass =
                    DefineClass(self, descriptor, class_loader, *dex_file, *dex_class_def);
                if (klass == nullptr) {
                  CHECK(self->IsExceptionPending()) << descriptor;
                  self->ClearException();
                  return nullptr;
                }
                return klass;
              }
            }
          }
        }
      }
    }
  }
  return nullptr;
}

mirror::Class* ClassLinker::FindClass(Thread* self, const char* descriptor,
                                      Handle<mirror::ClassLoader> class_loader) {
  DCHECK_NE(*descriptor, '\0') << "descriptor is empty string";
  DCHECK(self != nullptr);
  self->AssertNoPendingException();
  if (descriptor[1] == '\0') {
    // only the descriptors of primitive types should be 1 character long, also avoid class lookup
    // for primitive classes that aren't backed by dex files.
    return FindPrimitiveClass(descriptor[0]);
  }
  // Find the class in the loaded classes table.
  mirror::Class* klass = LookupClass(self, descriptor, class_loader.Get());
  if (klass != nullptr) {
    return EnsureResolved(self, descriptor, klass);
  }
  // Class is not yet loaded.
  if (descriptor[0] == '[') {
    return CreateArrayClass(self, descriptor, class_loader);
  } else if (class_loader.Get() == nullptr) {
    // The boot class loader, search the boot class path.
    ClassPathEntry pair = FindInClassPath(descriptor, boot_class_path_);
    if (pair.second != nullptr) {
      return DefineClass(self, descriptor, NullHandle<mirror::ClassLoader>(), *pair.first,
                         *pair.second);
    } else {
      // The boot class loader is searched ahead of the application class loader, failures are
      // expected and will be wrapped in a ClassNotFoundException. Use the pre-allocated error to
      // trigger the chaining with a proper stack trace.
      mirror::Throwable* pre_allocated = Runtime::Current()->GetPreAllocatedNoClassDefFoundError();
      self->SetException(ThrowLocation(), pre_allocated);
      return nullptr;
    }
  } else if (Runtime::Current()->UseCompileTimeClassPath()) {
    // First try with the bootstrap class loader.
    if (class_loader.Get() != nullptr) {
      klass = LookupClass(self, descriptor, nullptr);
      if (klass != nullptr) {
        return EnsureResolved(self, descriptor, klass);
      }
    }
    // If the lookup failed search the boot class path. We don't perform a recursive call to avoid
    // a NoClassDefFoundError being allocated.
    ClassPathEntry pair = FindInClassPath(descriptor, boot_class_path_);
    if (pair.second != nullptr) {
      return DefineClass(self, descriptor, NullHandle<mirror::ClassLoader>(), *pair.first,
                         *pair.second);
    }
    // Next try the compile time class path.
    const std::vector<const DexFile*>* class_path;
    {
      ScopedObjectAccessUnchecked soa(self);
      ScopedLocalRef<jobject> jclass_loader(soa.Env(),
                                            soa.AddLocalReference<jobject>(class_loader.Get()));
      class_path = &Runtime::Current()->GetCompileTimeClassPath(jclass_loader.get());
    }
    pair = FindInClassPath(descriptor, *class_path);
    if (pair.second != nullptr) {
      return DefineClass(self, descriptor, class_loader, *pair.first, *pair.second);
    } else {
      // Use the pre-allocated NCDFE at compile time to avoid wasting time constructing exceptions.
      mirror::Throwable* pre_allocated = Runtime::Current()->GetPreAllocatedNoClassDefFoundError();
      self->SetException(ThrowLocation(), pre_allocated);
      return nullptr;
    }
  } else {
    ScopedObjectAccessUnchecked soa(self);
    mirror::Class* klass = FindClassInPathClassLoader(soa, self, descriptor, class_loader);
    if (klass != nullptr) {
      return klass;
    }
    ScopedLocalRef<jobject> class_loader_object(soa.Env(),
                                                soa.AddLocalReference<jobject>(class_loader.Get()));
    std::string class_name_string(DescriptorToDot(descriptor));
    ScopedLocalRef<jobject> result(soa.Env(), nullptr);
    {
      ScopedThreadStateChange tsc(self, kNative);
      ScopedLocalRef<jobject> class_name_object(soa.Env(),
                                                soa.Env()->NewStringUTF(class_name_string.c_str()));
      if (class_name_object.get() == nullptr) {
        DCHECK(self->IsExceptionPending());  // OOME.
        return nullptr;
      }
      CHECK(class_loader_object.get() != nullptr);
      result.reset(soa.Env()->CallObjectMethod(class_loader_object.get(),
                                               WellKnownClasses::java_lang_ClassLoader_loadClass,
                                               class_name_object.get()));
    }
    if (self->IsExceptionPending()) {
      // If the ClassLoader threw, pass that exception up.
      return nullptr;
    } else if (result.get() == nullptr) {
      // broken loader - throw NPE to be compatible with Dalvik
      ThrowNullPointerException(nullptr, StringPrintf("ClassLoader.loadClass returned null for %s",
                                                      class_name_string.c_str()).c_str());
      return nullptr;
    } else {
      // success, return mirror::Class*
      return soa.Decode<mirror::Class*>(result.get());
    }
  }
  UNREACHABLE();
}

mirror::Class* ClassLinker::DefineClass(Thread* self, const char* descriptor,
                                        Handle<mirror::ClassLoader> class_loader,
                                        const DexFile& dex_file,
                                        const DexFile::ClassDef& dex_class_def) {
  StackHandleScope<3> hs(self);
  auto klass = hs.NewHandle<mirror::Class>(nullptr);
  bool should_allocate = false;

  // Load the class from the dex file.
  if (UNLIKELY(!init_done_)) {
    // finish up init of hand crafted class_roots_
    if (strcmp(descriptor, "Ljava/lang/Object;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangObject));
    } else if (strcmp(descriptor, "Ljava/lang/Class;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangClass));
    } else if (strcmp(descriptor, "Ljava/lang/String;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangString));
    } else if (strcmp(descriptor, "Ljava/lang/ref/Reference;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangRefReference));
    } else if (strcmp(descriptor, "Ljava/lang/DexCache;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangDexCache));
    } else if (strcmp(descriptor, "Ljava/lang/reflect/ArtField;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangReflectArtField));
    } else if (strcmp(descriptor, "Ljava/lang/reflect/ArtMethod;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangReflectArtMethod));
    } else {
      should_allocate = true;
    }
  } else {
    should_allocate = true;
  }

  if (should_allocate) {
    // Allocate a class with the status of not ready.
    // Interface object should get the right size here. Regular class will
    // figure out the right size later and be replaced with one of the right
    // size when the class becomes resolved.
    klass.Assign(AllocClass(self, SizeOfClassWithoutEmbeddedTables(dex_file, dex_class_def)));
  }
  if (UNLIKELY(klass.Get() == nullptr)) {
    CHECK(self->IsExceptionPending());  // Expect an OOME.
    return nullptr;
  }
  klass->SetDexCache(FindDexCache(dex_file));
  LoadClass(self, dex_file, dex_class_def, klass, class_loader.Get());
  ObjectLock<mirror::Class> lock(self, klass);
  if (self->IsExceptionPending()) {
    // An exception occured during load, set status to erroneous while holding klass' lock in case
    // notification is necessary.
    if (!klass->IsErroneous()) {
      klass->SetStatus(mirror::Class::kStatusError, self);
    }
    return nullptr;
  }
  klass->SetClinitThreadId(self->GetTid());

  // Add the newly loaded class to the loaded classes table.
  mirror::Class* existing = InsertClass(descriptor, klass.Get(), Hash(descriptor));
  if (existing != nullptr) {
    // We failed to insert because we raced with another thread. Calling EnsureResolved may cause
    // this thread to block.
    return EnsureResolved(self, descriptor, existing);
  }

  // Finish loading (if necessary) by finding parents
  CHECK(!klass->IsLoaded());
  if (!LoadSuperAndInterfaces(klass, dex_file)) {
    // Loading failed.
    if (!klass->IsErroneous()) {
      klass->SetStatus(mirror::Class::kStatusError, self);
    }
    return nullptr;
  }
  CHECK(klass->IsLoaded());
  // Link the class (if necessary)
  CHECK(!klass->IsResolved());
  // TODO: Use fast jobjects?
  auto interfaces = hs.NewHandle<mirror::ObjectArray<mirror::Class>>(nullptr);

  mirror::Class* new_class = nullptr;
  if (!LinkClass(self, descriptor, klass, interfaces, &new_class)) {
    // Linking failed.
    if (!klass->IsErroneous()) {
      klass->SetStatus(mirror::Class::kStatusError, self);
    }
    return nullptr;
  }
  self->AssertNoPendingException();
  CHECK(new_class != nullptr) << descriptor;
  CHECK(new_class->IsResolved()) << descriptor;

  Handle<mirror::Class> new_class_h(hs.NewHandle(new_class));

  /*
   * We send CLASS_PREPARE events to the debugger from here.  The
   * definition of "preparation" is creating the static fields for a
   * class and initializing them to the standard default values, but not
   * executing any code (that comes later, during "initialization").
   *
   * We did the static preparation in LinkClass.
   *
   * The class has been prepared and resolved but possibly not yet verified
   * at this point.
   */
  Dbg::PostClassPrepare(new_class_h.Get());

  return new_class_h.Get();
}

uint32_t ClassLinker::SizeOfClassWithoutEmbeddedTables(const DexFile& dex_file,
                                                       const DexFile::ClassDef& dex_class_def) {
  const uint8_t* class_data = dex_file.GetClassData(dex_class_def);
  size_t num_ref = 0;
  size_t num_8 = 0;
  size_t num_16 = 0;
  size_t num_32 = 0;
  size_t num_64 = 0;
  if (class_data != nullptr) {
    for (ClassDataItemIterator it(dex_file, class_data); it.HasNextStaticField(); it.Next()) {
      const DexFile::FieldId& field_id = dex_file.GetFieldId(it.GetMemberIndex());
      const char* descriptor = dex_file.GetFieldTypeDescriptor(field_id);
      char c = descriptor[0];
      switch (c) {
        case 'L':
        case '[':
          num_ref++;
          break;
        case 'J':
        case 'D':
          num_64++;
          break;
        case 'I':
        case 'F':
          num_32++;
          break;
        case 'S':
        case 'C':
          num_16++;
          break;
        case 'B':
        case 'Z':
          num_8++;
          break;
        default:
          LOG(FATAL) << "Unknown descriptor: " << c;
      }
    }
  }
  return mirror::Class::ComputeClassSize(false, 0, num_8, num_16, num_32, num_64, num_ref);
}

OatFile::OatClass ClassLinker::FindOatClass(const DexFile& dex_file, uint16_t class_def_idx,
                                            bool* found) {
  DCHECK_NE(class_def_idx, DexFile::kDexNoIndex16);
  const OatFile::OatDexFile* oat_dex_file = FindOpenedOatDexFileForDexFile(dex_file);
  if (oat_dex_file == nullptr) {
    *found = false;
    return OatFile::OatClass::Invalid();
  }
  *found = true;
  return oat_dex_file->GetOatClass(class_def_idx);
}

static uint32_t GetOatMethodIndexFromMethodIndex(const DexFile& dex_file, uint16_t class_def_idx,
                                                 uint32_t method_idx) {
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_idx);
  const uint8_t* class_data = dex_file.GetClassData(class_def);
  CHECK(class_data != nullptr);
  ClassDataItemIterator it(dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  // Process methods
  size_t class_def_method_index = 0;
  while (it.HasNextDirectMethod()) {
    if (it.GetMemberIndex() == method_idx) {
      return class_def_method_index;
    }
    class_def_method_index++;
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    if (it.GetMemberIndex() == method_idx) {
      return class_def_method_index;
    }
    class_def_method_index++;
    it.Next();
  }
  DCHECK(!it.HasNext());
  LOG(FATAL) << "Failed to find method index " << method_idx << " in " << dex_file.GetLocation();
  return 0;
}

const OatFile::OatMethod ClassLinker::FindOatMethodFor(mirror::ArtMethod* method, bool* found) {
  // Although we overwrite the trampoline of non-static methods, we may get here via the resolution
  // method for direct methods (or virtual methods made direct).
  mirror::Class* declaring_class = method->GetDeclaringClass();
  size_t oat_method_index;
  if (method->IsStatic() || method->IsDirect()) {
    // Simple case where the oat method index was stashed at load time.
    oat_method_index = method->GetMethodIndex();
  } else {
    // We're invoking a virtual method directly (thanks to sharpening), compute the oat_method_index
    // by search for its position in the declared virtual methods.
    oat_method_index = declaring_class->NumDirectMethods();
    size_t end = declaring_class->NumVirtualMethods();
    bool found = false;
    for (size_t i = 0; i < end; i++) {
      // Check method index instead of identity in case of duplicate method definitions.
      if (method->GetDexMethodIndex() ==
          declaring_class->GetVirtualMethod(i)->GetDexMethodIndex()) {
        found = true;
        break;
      }
      oat_method_index++;
    }
    CHECK(found) << "Didn't find oat method index for virtual method: " << PrettyMethod(method);
  }
  DCHECK_EQ(oat_method_index,
            GetOatMethodIndexFromMethodIndex(*declaring_class->GetDexCache()->GetDexFile(),
                                             method->GetDeclaringClass()->GetDexClassDefIndex(),
                                             method->GetDexMethodIndex()));
  OatFile::OatClass oat_class = FindOatClass(*declaring_class->GetDexCache()->GetDexFile(),
                                             declaring_class->GetDexClassDefIndex(),
                                             found);
  if (!found) {
    return OatFile::OatMethod::Invalid();
  }
  *found = true;
  return oat_class.GetOatMethod(oat_method_index);
}

// Special case to get oat code without overwriting a trampoline.
const void* ClassLinker::GetQuickOatCodeFor(mirror::ArtMethod* method) {
  CHECK(!method->IsAbstract()) << PrettyMethod(method);
  if (method->IsProxyMethod()) {
    return GetQuickProxyInvokeHandler();
  }
  bool found;
  OatFile::OatMethod oat_method = FindOatMethodFor(method, &found);
  const void* result = nullptr;
  if (found) {
    result = oat_method.GetQuickCode();
  }

  if (result == nullptr) {
    if (method->IsNative()) {
      // No code and native? Use generic trampoline.
      result = GetQuickGenericJniStub();
    } else if (method->IsPortableCompiled()) {
      // No code? Do we expect portable code?
      result = GetQuickToPortableBridge();
    } else {
      // No code? You must mean to go into the interpreter.
      result = GetQuickToInterpreterBridge();
    }
  }
  return result;
}

const void* ClassLinker::GetPortableOatCodeFor(mirror::ArtMethod* method,
                                               bool* have_portable_code) {
  CHECK(!method->IsAbstract()) << PrettyMethod(method);
  *have_portable_code = false;
  if (method->IsProxyMethod()) {
    return GetPortableProxyInvokeHandler();
  }
  bool found;
  OatFile::OatMethod oat_method = FindOatMethodFor(method, &found);
  const void* result = nullptr;
  const void* quick_code = nullptr;
  if (found) {
    result = oat_method.GetPortableCode();
    quick_code = oat_method.GetQuickCode();
  }

  if (result == nullptr) {
    if (quick_code == nullptr) {
      // No code? You must mean to go into the interpreter.
      result = GetPortableToInterpreterBridge();
    } else {
      // No code? But there's quick code, so use a bridge.
      result = GetPortableToQuickBridge();
    }
  } else {
    *have_portable_code = true;
  }
  return result;
}

const void* ClassLinker::GetOatMethodQuickCodeFor(mirror::ArtMethod* method) {
  if (method->IsNative() || method->IsAbstract() || method->IsProxyMethod()) {
    return nullptr;
  }
  bool found;
  OatFile::OatMethod oat_method = FindOatMethodFor(method, &found);
  return found ? oat_method.GetQuickCode() : nullptr;
}

const void* ClassLinker::GetOatMethodPortableCodeFor(mirror::ArtMethod* method) {
  if (method->IsNative() || method->IsAbstract() || method->IsProxyMethod()) {
    return nullptr;
  }
  bool found;
  OatFile::OatMethod oat_method = FindOatMethodFor(method, &found);
  return found ? oat_method.GetPortableCode() : nullptr;
}

const void* ClassLinker::GetQuickOatCodeFor(const DexFile& dex_file, uint16_t class_def_idx,
                                            uint32_t method_idx) {
  bool found;
  OatFile::OatClass oat_class = FindOatClass(dex_file, class_def_idx, &found);
  if (!found) {
    return nullptr;
  }
  uint32_t oat_method_idx = GetOatMethodIndexFromMethodIndex(dex_file, class_def_idx, method_idx);
  return oat_class.GetOatMethod(oat_method_idx).GetQuickCode();
}

const void* ClassLinker::GetPortableOatCodeFor(const DexFile& dex_file, uint16_t class_def_idx,
                                               uint32_t method_idx) {
  bool found;
  OatFile::OatClass oat_class = FindOatClass(dex_file, class_def_idx, &found);
  if (!found) {
    return nullptr;
  }
  uint32_t oat_method_idx = GetOatMethodIndexFromMethodIndex(dex_file, class_def_idx, method_idx);
  return oat_class.GetOatMethod(oat_method_idx).GetPortableCode();
}

// Returns true if the method must run with interpreter, false otherwise.
static bool NeedsInterpreter(
    mirror::ArtMethod* method, const void* quick_code, const void* portable_code)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if ((quick_code == nullptr) && (portable_code == nullptr)) {
    // No code: need interpreter.
    // May return true for native code, in the case of generic JNI
    // DCHECK(!method->IsNative());
    return true;
  }
#ifdef ART_SEA_IR_MODE
  ScopedObjectAccess soa(Thread::Current());
  if (std::string::npos != PrettyMethod(method).find("fibonacci")) {
    LOG(INFO) << "Found " << PrettyMethod(method);
    return false;
  }
#endif
  // If interpreter mode is enabled, every method (except native and proxy) must
  // be run with interpreter.
  return Runtime::Current()->GetInstrumentation()->InterpretOnly() &&
         !method->IsNative() && !method->IsProxyMethod();
}

void ClassLinker::FixupStaticTrampolines(mirror::Class* klass) {
  DCHECK(klass->IsInitialized()) << PrettyDescriptor(klass);
  if (klass->NumDirectMethods() == 0) {
    return;  // No direct methods => no static methods.
  }
  Runtime* runtime = Runtime::Current();
  if (!runtime->IsStarted() || runtime->UseCompileTimeClassPath()) {
    if (runtime->IsCompiler() || runtime->GetHeap()->HasImageSpace()) {
      return;  // OAT file unavailable.
    }
  }

  const DexFile& dex_file = klass->GetDexFile();
  const DexFile::ClassDef* dex_class_def = klass->GetClassDef();
  CHECK(dex_class_def != nullptr);
  const uint8_t* class_data = dex_file.GetClassData(*dex_class_def);
  // There should always be class data if there were direct methods.
  CHECK(class_data != nullptr) << PrettyDescriptor(klass);
  ClassDataItemIterator it(dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  bool has_oat_class;
  OatFile::OatClass oat_class = FindOatClass(dex_file, klass->GetDexClassDefIndex(),
                                             &has_oat_class);
  // Link the code of methods skipped by LinkCode.
  for (size_t method_index = 0; it.HasNextDirectMethod(); ++method_index, it.Next()) {
    mirror::ArtMethod* method = klass->GetDirectMethod(method_index);
    if (!method->IsStatic()) {
      // Only update static methods.
      continue;
    }
    const void* portable_code = nullptr;
    const void* quick_code = nullptr;
    if (has_oat_class) {
      OatFile::OatMethod oat_method = oat_class.GetOatMethod(method_index);
      portable_code = oat_method.GetPortableCode();
      quick_code = oat_method.GetQuickCode();
    }
    const bool enter_interpreter = NeedsInterpreter(method, quick_code, portable_code);
    bool have_portable_code = false;
    if (enter_interpreter) {
      // Use interpreter entry point.
      // Check whether the method is native, in which case it's generic JNI.
      if (quick_code == nullptr && portable_code == nullptr && method->IsNative()) {
        quick_code = GetQuickGenericJniStub();
        portable_code = GetPortableToQuickBridge();
      } else {
        portable_code = GetPortableToInterpreterBridge();
        quick_code = GetQuickToInterpreterBridge();
      }
    } else {
      if (portable_code == nullptr) {
        portable_code = GetPortableToQuickBridge();
      } else {
        have_portable_code = true;
      }
      if (quick_code == nullptr) {
        quick_code = GetQuickToPortableBridge();
      }
    }
    runtime->GetInstrumentation()->UpdateMethodsCode(method, quick_code, portable_code,
                                                     have_portable_code);
  }
  // Ignore virtual methods on the iterator.
}

void ClassLinker::LinkCode(Handle<mirror::ArtMethod> method,
                           const OatFile::OatClass* oat_class,
                           const DexFile& dex_file, uint32_t dex_method_index,
                           uint32_t method_index) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsCompiler()) {
    // The following code only applies to a non-compiler runtime.
    return;
  }
  // Method shouldn't have already been linked.
  DCHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr);
  DCHECK(method->GetEntryPointFromPortableCompiledCode() == nullptr);
  if (oat_class != nullptr) {
    // Every kind of method should at least get an invoke stub from the oat_method.
    // non-abstract methods also get their code pointers.
    const OatFile::OatMethod oat_method = oat_class->GetOatMethod(method_index);
    oat_method.LinkMethod(method.Get());
  }

  // Install entry point from interpreter.
  bool enter_interpreter = NeedsInterpreter(method.Get(),
                                            method->GetEntryPointFromQuickCompiledCode(),
                                            method->GetEntryPointFromPortableCompiledCode());
  if (enter_interpreter && !method->IsNative()) {
    method->SetEntryPointFromInterpreter(artInterpreterToInterpreterBridge);
  } else {
    method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
  }

  if (method->IsAbstract()) {
    method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
    method->SetEntryPointFromPortableCompiledCode(GetPortableToInterpreterBridge());
    return;
  }

  bool have_portable_code = false;
  if (method->IsStatic() && !method->IsConstructor()) {
    // For static methods excluding the class initializer, install the trampoline.
    // It will be replaced by the proper entry point by ClassLinker::FixupStaticTrampolines
    // after initializing class (see ClassLinker::InitializeClass method).
    method->SetEntryPointFromQuickCompiledCode(GetQuickResolutionStub());
    method->SetEntryPointFromPortableCompiledCode(GetPortableResolutionStub());
  } else if (enter_interpreter) {
    if (!method->IsNative()) {
      // Set entry point from compiled code if there's no code or in interpreter only mode.
      method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
      method->SetEntryPointFromPortableCompiledCode(GetPortableToInterpreterBridge());
    } else {
      method->SetEntryPointFromQuickCompiledCode(GetQuickGenericJniStub());
      method->SetEntryPointFromPortableCompiledCode(GetPortableToQuickBridge());
    }
  } else if (method->GetEntryPointFromPortableCompiledCode() != nullptr) {
    DCHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr);
    have_portable_code = true;
    method->SetEntryPointFromQuickCompiledCode(GetQuickToPortableBridge());
  } else {
    DCHECK(method->GetEntryPointFromQuickCompiledCode() != nullptr);
    method->SetEntryPointFromPortableCompiledCode(GetPortableToQuickBridge());
  }

  if (method->IsNative()) {
    // Unregistering restores the dlsym lookup stub.
    method->UnregisterNative();

    if (enter_interpreter) {
      // We have a native method here without code. Then it should have either the generic JNI
      // trampoline as entrypoint (non-static), or the resolution trampoline (static).
      // TODO: this doesn't handle all the cases where trampolines may be installed.
      const void* entry_point = method->GetEntryPointFromQuickCompiledCode();
      DCHECK(IsQuickGenericJniStub(entry_point) || IsQuickResolutionStub(entry_point));
    }
  }

  // Allow instrumentation its chance to hijack code.
  runtime->GetInstrumentation()->UpdateMethodsCode(method.Get(),
                                                   method->GetEntryPointFromQuickCompiledCode(),
                                                   method->GetEntryPointFromPortableCompiledCode(),
                                                   have_portable_code);
}



void ClassLinker::LoadClass(Thread* self, const DexFile& dex_file,
                            const DexFile::ClassDef& dex_class_def,
                            Handle<mirror::Class> klass,
                            mirror::ClassLoader* class_loader) {
  CHECK(klass.Get() != nullptr);
  CHECK(klass->GetDexCache() != nullptr);
  CHECK_EQ(mirror::Class::kStatusNotReady, klass->GetStatus());
  const char* descriptor = dex_file.GetClassDescriptor(dex_class_def);
  CHECK(descriptor != nullptr);

  klass->SetClass(GetClassRoot(kJavaLangClass));
  if (kUseBakerOrBrooksReadBarrier) {
    klass->AssertReadBarrierPointer();
  }
  uint32_t access_flags = dex_class_def.GetJavaAccessFlags();
  CHECK_EQ(access_flags & ~kAccJavaFlagsMask, 0U);
  klass->SetAccessFlags(access_flags);
  klass->SetClassLoader(class_loader);
  DCHECK_EQ(klass->GetPrimitiveType(), Primitive::kPrimNot);
  klass->SetStatus(mirror::Class::kStatusIdx, nullptr);

  klass->SetDexClassDefIndex(dex_file.GetIndexForClassDef(dex_class_def));
  klass->SetDexTypeIndex(dex_class_def.class_idx_);

  const uint8_t* class_data = dex_file.GetClassData(dex_class_def);
  if (class_data == nullptr) {
    return;  // no fields or methods - for example a marker interface
  }


  bool has_oat_class = false;
  if (Runtime::Current()->IsStarted() && !Runtime::Current()->UseCompileTimeClassPath()) {
    OatFile::OatClass oat_class = FindOatClass(dex_file, klass->GetDexClassDefIndex(),
                                               &has_oat_class);
    if (has_oat_class) {
      LoadClassMembers(self, dex_file, class_data, klass, class_loader, &oat_class);
    }
  }
  if (!has_oat_class) {
    LoadClassMembers(self, dex_file, class_data, klass, class_loader, nullptr);
  }
}

void ClassLinker::LoadClassMembers(Thread* self, const DexFile& dex_file,
                                   const uint8_t* class_data,
                                   Handle<mirror::Class> klass,
                                   mirror::ClassLoader* class_loader,
                                   const OatFile::OatClass* oat_class) {
  // Load fields.
  ClassDataItemIterator it(dex_file, class_data);
  if (it.NumStaticFields() != 0) {
    mirror::ObjectArray<mirror::ArtField>* statics = AllocArtFieldArray(self, it.NumStaticFields());
    if (UNLIKELY(statics == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetSFields(statics);
  }
  if (it.NumInstanceFields() != 0) {
    mirror::ObjectArray<mirror::ArtField>* fields =
        AllocArtFieldArray(self, it.NumInstanceFields());
    if (UNLIKELY(fields == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetIFields(fields);
  }
  for (size_t i = 0; it.HasNextStaticField(); i++, it.Next()) {
    self->AllowThreadSuspension();
    StackHandleScope<1> hs(self);
    Handle<mirror::ArtField> sfield(hs.NewHandle(AllocArtField(self)));
    if (UNLIKELY(sfield.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetStaticField(i, sfield.Get());
    LoadField(dex_file, it, klass, sfield);
  }
  for (size_t i = 0; it.HasNextInstanceField(); i++, it.Next()) {
    self->AllowThreadSuspension();
    StackHandleScope<1> hs(self);
    Handle<mirror::ArtField> ifield(hs.NewHandle(AllocArtField(self)));
    if (UNLIKELY(ifield.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetInstanceField(i, ifield.Get());
    LoadField(dex_file, it, klass, ifield);
  }

  // Load methods.
  if (it.NumDirectMethods() != 0) {
    // TODO: append direct methods to class object
    mirror::ObjectArray<mirror::ArtMethod>* directs =
         AllocArtMethodArray(self, it.NumDirectMethods());
    if (UNLIKELY(directs == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetDirectMethods(directs);
  }
  if (it.NumVirtualMethods() != 0) {
    // TODO: append direct methods to class object
    mirror::ObjectArray<mirror::ArtMethod>* virtuals =
        AllocArtMethodArray(self, it.NumVirtualMethods());
    if (UNLIKELY(virtuals == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetVirtualMethods(virtuals);
  }
  size_t class_def_method_index = 0;
  uint32_t last_dex_method_index = DexFile::kDexNoIndex;
  size_t last_class_def_method_index = 0;
  for (size_t i = 0; it.HasNextDirectMethod(); i++, it.Next()) {
    self->AllowThreadSuspension();
    StackHandleScope<1> hs(self);
    Handle<mirror::ArtMethod> method(hs.NewHandle(LoadMethod(self, dex_file, it, klass)));
    if (UNLIKELY(method.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetDirectMethod(i, method.Get());
    LinkCode(method, oat_class, dex_file, it.GetMemberIndex(), class_def_method_index);
    uint32_t it_method_index = it.GetMemberIndex();
    if (last_dex_method_index == it_method_index) {
      // duplicate case
      method->SetMethodIndex(last_class_def_method_index);
    } else {
      method->SetMethodIndex(class_def_method_index);
      last_dex_method_index = it_method_index;
      last_class_def_method_index = class_def_method_index;
    }
    class_def_method_index++;
  }
  for (size_t i = 0; it.HasNextVirtualMethod(); i++, it.Next()) {
    self->AllowThreadSuspension();
    StackHandleScope<1> hs(self);
    Handle<mirror::ArtMethod> method(hs.NewHandle(LoadMethod(self, dex_file, it, klass)));
    if (UNLIKELY(method.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetVirtualMethod(i, method.Get());
    DCHECK_EQ(class_def_method_index, it.NumDirectMethods() + i);
    LinkCode(method, oat_class, dex_file, it.GetMemberIndex(), class_def_method_index);
    class_def_method_index++;
  }
  DCHECK(!it.HasNext());
}

void ClassLinker::LoadField(const DexFile& /*dex_file*/, const ClassDataItemIterator& it,
                            Handle<mirror::Class> klass,
                            Handle<mirror::ArtField> dst) {
  uint32_t field_idx = it.GetMemberIndex();
  dst->SetDexFieldIndex(field_idx);
  dst->SetDeclaringClass(klass.Get());
  dst->SetAccessFlags(it.GetFieldAccessFlags());
}

mirror::ArtMethod* ClassLinker::LoadMethod(Thread* self, const DexFile& dex_file,
                                           const ClassDataItemIterator& it,
                                           Handle<mirror::Class> klass) {
  uint32_t dex_method_idx = it.GetMemberIndex();
  const DexFile::MethodId& method_id = dex_file.GetMethodId(dex_method_idx);
  const char* method_name = dex_file.StringDataByIdx(method_id.name_idx_);

  mirror::ArtMethod* dst = AllocArtMethod(self);
  if (UNLIKELY(dst == nullptr)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return nullptr;
  }
  DCHECK(dst->IsArtMethod()) << PrettyDescriptor(dst->GetClass());

  ScopedAssertNoThreadSuspension ants(self, "LoadMethod");
  dst->SetDexMethodIndex(dex_method_idx);
  dst->SetDeclaringClass(klass.Get());
  dst->SetCodeItemOffset(it.GetMethodCodeItemOffset());

  dst->SetDexCacheStrings(klass->GetDexCache()->GetStrings());
  dst->SetDexCacheResolvedMethods(klass->GetDexCache()->GetResolvedMethods());
  dst->SetDexCacheResolvedTypes(klass->GetDexCache()->GetResolvedTypes());

  uint32_t access_flags = it.GetMethodAccessFlags();

  if (UNLIKELY(strcmp("finalize", method_name) == 0)) {
    // Set finalizable flag on declaring class.
    if (strcmp("V", dex_file.GetShorty(method_id.proto_idx_)) == 0) {
      // Void return type.
      if (klass->GetClassLoader() != nullptr) {  // All non-boot finalizer methods are flagged.
        klass->SetFinalizable();
      } else {
        std::string temp;
        const char* klass_descriptor = klass->GetDescriptor(&temp);
        // The Enum class declares a "final" finalize() method to prevent subclasses from
        // introducing a finalizer. We don't want to set the finalizable flag for Enum or its
        // subclasses, so we exclude it here.
        // We also want to avoid setting the flag on Object, where we know that finalize() is
        // empty.
        if (strcmp(klass_descriptor, "Ljava/lang/Object;") != 0 &&
            strcmp(klass_descriptor, "Ljava/lang/Enum;") != 0) {
          klass->SetFinalizable();
        }
      }
    }
  } else if (method_name[0] == '<') {
    // Fix broken access flags for initializers. Bug 11157540.
    bool is_init = (strcmp("<init>", method_name) == 0);
    bool is_clinit = !is_init && (strcmp("<clinit>", method_name) == 0);
    if (UNLIKELY(!is_init && !is_clinit)) {
      LOG(WARNING) << "Unexpected '<' at start of method name " << method_name;
    } else {
      if (UNLIKELY((access_flags & kAccConstructor) == 0)) {
        LOG(WARNING) << method_name << " didn't have expected constructor access flag in class "
            << PrettyDescriptor(klass.Get()) << " in dex file " << dex_file.GetLocation();
        access_flags |= kAccConstructor;
      }
    }
  }
  dst->SetAccessFlags(access_flags);

  return dst;
}

void ClassLinker::AppendToBootClassPath(Thread* self, const DexFile& dex_file) {
  StackHandleScope<1> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(AllocDexCache(self, dex_file)));
  CHECK(dex_cache.Get() != nullptr) << "Failed to allocate dex cache for "
                                    << dex_file.GetLocation();
  AppendToBootClassPath(dex_file, dex_cache);
}

void ClassLinker::AppendToBootClassPath(const DexFile& dex_file,
                                        Handle<mirror::DexCache> dex_cache) {
  CHECK(dex_cache.Get() != nullptr) << dex_file.GetLocation();
  boot_class_path_.push_back(&dex_file);
  RegisterDexFile(dex_file, dex_cache);
}

bool ClassLinker::IsDexFileRegisteredLocked(const DexFile& dex_file) {
  dex_lock_.AssertSharedHeld(Thread::Current());
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    mirror::DexCache* dex_cache = GetDexCache(i);
    if (dex_cache->GetDexFile() == &dex_file) {
      return true;
    }
  }
  return false;
}

bool ClassLinker::IsDexFileRegistered(const DexFile& dex_file) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  return IsDexFileRegisteredLocked(dex_file);
}

void ClassLinker::RegisterDexFileLocked(const DexFile& dex_file,
                                        Handle<mirror::DexCache> dex_cache) {
  dex_lock_.AssertExclusiveHeld(Thread::Current());
  CHECK(dex_cache.Get() != nullptr) << dex_file.GetLocation();
  CHECK(dex_cache->GetLocation()->Equals(dex_file.GetLocation()))
      << dex_cache->GetLocation()->ToModifiedUtf8() << " " << dex_file.GetLocation();
  dex_caches_.push_back(GcRoot<mirror::DexCache>(dex_cache.Get()));
  dex_cache->SetDexFile(&dex_file);
  if (log_new_dex_caches_roots_) {
    // TODO: This is not safe if we can remove dex caches.
    new_dex_cache_roots_.push_back(dex_caches_.size() - 1);
  }
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file) {
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
  }
  // Don't alloc while holding the lock, since allocation may need to
  // suspend all threads and another thread may need the dex_lock_ to
  // get to a suspend point.
  StackHandleScope<1> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(AllocDexCache(self, dex_file)));
  CHECK(dex_cache.Get() != nullptr) << "Failed to allocate dex cache for "
                                    << dex_file.GetLocation();
  {
    WriterMutexLock mu(self, dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
    RegisterDexFileLocked(dex_file, dex_cache);
  }
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file,
                                  Handle<mirror::DexCache> dex_cache) {
  WriterMutexLock mu(Thread::Current(), dex_lock_);
  RegisterDexFileLocked(dex_file, dex_cache);
}

mirror::DexCache* ClassLinker::FindDexCache(const DexFile& dex_file) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  // Search assuming unique-ness of dex file.
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    mirror::DexCache* dex_cache = GetDexCache(i);
    if (dex_cache->GetDexFile() == &dex_file) {
      return dex_cache;
    }
  }
  // Search matching by location name.
  std::string location(dex_file.GetLocation());
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    mirror::DexCache* dex_cache = GetDexCache(i);
    if (dex_cache->GetDexFile()->GetLocation() == location) {
      return dex_cache;
    }
  }
  // Failure, dump diagnostic and abort.
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    mirror::DexCache* dex_cache = GetDexCache(i);
    LOG(ERROR) << "Registered dex file " << i << " = " << dex_cache->GetDexFile()->GetLocation();
  }
  LOG(FATAL) << "Failed to find DexCache for DexFile " << location;
  return nullptr;
}

void ClassLinker::FixupDexCaches(mirror::ArtMethod* resolution_method) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    mirror::DexCache* dex_cache = GetDexCache(i);
    dex_cache->Fixup(resolution_method);
  }
}

mirror::Class* ClassLinker::CreatePrimitiveClass(Thread* self, Primitive::Type type) {
  mirror::Class* klass = AllocClass(self, mirror::Class::PrimitiveClassSize());
  if (UNLIKELY(klass == nullptr)) {
    return nullptr;
  }
  return InitializePrimitiveClass(klass, type);
}

mirror::Class* ClassLinker::InitializePrimitiveClass(mirror::Class* primitive_class,
                                                     Primitive::Type type) {
  CHECK(primitive_class != nullptr);
  // Must hold lock on object when initializing.
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> h_class(hs.NewHandle(primitive_class));
  ObjectLock<mirror::Class> lock(self, h_class);
  primitive_class->SetAccessFlags(kAccPublic | kAccFinal | kAccAbstract);
  primitive_class->SetPrimitiveType(type);
  primitive_class->SetStatus(mirror::Class::kStatusInitialized, self);
  const char* descriptor = Primitive::Descriptor(type);
  mirror::Class* existing = InsertClass(descriptor, primitive_class, Hash(descriptor));
  CHECK(existing == nullptr) << "InitPrimitiveClass(" << type << ") failed";
  return primitive_class;
}

// Create an array class (i.e. the class object for the array, not the
// array itself).  "descriptor" looks like "[C" or "[[[[B" or
// "[Ljava/lang/String;".
//
// If "descriptor" refers to an array of primitives, look up the
// primitive type's internally-generated class object.
//
// "class_loader" is the class loader of the class that's referring to
// us.  It's used to ensure that we're looking for the element type in
// the right context.  It does NOT become the class loader for the
// array class; that always comes from the base element class.
//
// Returns nullptr with an exception raised on failure.
mirror::Class* ClassLinker::CreateArrayClass(Thread* self, const char* descriptor,
                                             Handle<mirror::ClassLoader> class_loader) {
  // Identify the underlying component type
  CHECK_EQ('[', descriptor[0]);
  StackHandleScope<2> hs(self);
  MutableHandle<mirror::Class> component_type(hs.NewHandle(FindClass(self, descriptor + 1,
                                                                     class_loader)));
  if (component_type.Get() == nullptr) {
    DCHECK(self->IsExceptionPending());
    // We need to accept erroneous classes as component types.
    component_type.Assign(LookupClass(self, descriptor + 1, class_loader.Get()));
    if (component_type.Get() == nullptr) {
      DCHECK(self->IsExceptionPending());
      return nullptr;
    } else {
      self->ClearException();
    }
  }
  if (UNLIKELY(component_type->IsPrimitiveVoid())) {
    ThrowNoClassDefFoundError("Attempt to create array of void primitive type");
    return nullptr;
  }
  // See if the component type is already loaded.  Array classes are
  // always associated with the class loader of their underlying
  // element type -- an array of Strings goes with the loader for
  // java/lang/String -- so we need to look for it there.  (The
  // caller should have checked for the existence of the class
  // before calling here, but they did so with *their* class loader,
  // not the component type's loader.)
  //
  // If we find it, the caller adds "loader" to the class' initiating
  // loader list, which should prevent us from going through this again.
  //
  // This call is unnecessary if "loader" and "component_type->GetClassLoader()"
  // are the same, because our caller (FindClass) just did the
  // lookup.  (Even if we get this wrong we still have correct behavior,
  // because we effectively do this lookup again when we add the new
  // class to the hash table --- necessary because of possible races with
  // other threads.)
  if (class_loader.Get() != component_type->GetClassLoader()) {
    mirror::Class* new_class = LookupClass(self, descriptor, component_type->GetClassLoader());
    if (new_class != nullptr) {
      return new_class;
    }
  }

  // Fill out the fields in the Class.
  //
  // It is possible to execute some methods against arrays, because
  // all arrays are subclasses of java_lang_Object_, so we need to set
  // up a vtable.  We can just point at the one in java_lang_Object_.
  //
  // Array classes are simple enough that we don't need to do a full
  // link step.
  auto new_class = hs.NewHandle<mirror::Class>(nullptr);
  if (UNLIKELY(!init_done_)) {
    // Classes that were hand created, ie not by FindSystemClass
    if (strcmp(descriptor, "[Ljava/lang/Class;") == 0) {
      new_class.Assign(GetClassRoot(kClassArrayClass));
    } else if (strcmp(descriptor, "[Ljava/lang/Object;") == 0) {
      new_class.Assign(GetClassRoot(kObjectArrayClass));
    } else if (strcmp(descriptor, GetClassRootDescriptor(kJavaLangStringArrayClass)) == 0) {
      new_class.Assign(GetClassRoot(kJavaLangStringArrayClass));
    } else if (strcmp(descriptor,
                      GetClassRootDescriptor(kJavaLangReflectArtMethodArrayClass)) == 0) {
      new_class.Assign(GetClassRoot(kJavaLangReflectArtMethodArrayClass));
    } else if (strcmp(descriptor,
                      GetClassRootDescriptor(kJavaLangReflectArtFieldArrayClass)) == 0) {
      new_class.Assign(GetClassRoot(kJavaLangReflectArtFieldArrayClass));
    } else if (strcmp(descriptor, "[C") == 0) {
      new_class.Assign(GetClassRoot(kCharArrayClass));
    } else if (strcmp(descriptor, "[I") == 0) {
      new_class.Assign(GetClassRoot(kIntArrayClass));
    }
  }
  if (new_class.Get() == nullptr) {
    new_class.Assign(AllocClass(self, mirror::Array::ClassSize()));
    if (new_class.Get() == nullptr) {
      return nullptr;
    }
    new_class->SetComponentType(component_type.Get());
  }
  ObjectLock<mirror::Class> lock(self, new_class);  // Must hold lock on object when initializing.
  DCHECK(new_class->GetComponentType() != nullptr);
  mirror::Class* java_lang_Object = GetClassRoot(kJavaLangObject);
  new_class->SetSuperClass(java_lang_Object);
  new_class->SetVTable(java_lang_Object->GetVTable());
  new_class->SetPrimitiveType(Primitive::kPrimNot);
  new_class->SetClassLoader(component_type->GetClassLoader());
  new_class->SetStatus(mirror::Class::kStatusLoaded, self);
  {
    StackHandleScope<mirror::Class::kImtSize> hs(self,
                                                 Runtime::Current()->GetImtUnimplementedMethod());
    new_class->PopulateEmbeddedImtAndVTable(&hs);
  }
  new_class->SetStatus(mirror::Class::kStatusInitialized, self);
  // don't need to set new_class->SetObjectSize(..)
  // because Object::SizeOf delegates to Array::SizeOf


  // All arrays have java/lang/Cloneable and java/io/Serializable as
  // interfaces.  We need to set that up here, so that stuff like
  // "instanceof" works right.
  //
  // Note: The GC could run during the call to FindSystemClass,
  // so we need to make sure the class object is GC-valid while we're in
  // there.  Do this by clearing the interface list so the GC will just
  // think that the entries are null.


  // Use the single, global copies of "interfaces" and "iftable"
  // (remember not to free them for arrays).
  {
    mirror::IfTable* array_iftable = array_iftable_.Read();
    CHECK(array_iftable != nullptr);
    new_class->SetIfTable(array_iftable);
  }

  // Inherit access flags from the component type.
  int access_flags = new_class->GetComponentType()->GetAccessFlags();
  // Lose any implementation detail flags; in particular, arrays aren't finalizable.
  access_flags &= kAccJavaFlagsMask;
  // Arrays can't be used as a superclass or interface, so we want to add "abstract final"
  // and remove "interface".
  access_flags |= kAccAbstract | kAccFinal;
  access_flags &= ~kAccInterface;

  new_class->SetAccessFlags(access_flags);

  mirror::Class* existing = InsertClass(descriptor, new_class.Get(), Hash(descriptor));
  if (existing == nullptr) {
    return new_class.Get();
  }
  // Another thread must have loaded the class after we
  // started but before we finished.  Abandon what we've
  // done.
  //
  // (Yes, this happens.)

  return existing;
}

mirror::Class* ClassLinker::FindPrimitiveClass(char type) {
  switch (type) {
    case 'B':
      return GetClassRoot(kPrimitiveByte);
    case 'C':
      return GetClassRoot(kPrimitiveChar);
    case 'D':
      return GetClassRoot(kPrimitiveDouble);
    case 'F':
      return GetClassRoot(kPrimitiveFloat);
    case 'I':
      return GetClassRoot(kPrimitiveInt);
    case 'J':
      return GetClassRoot(kPrimitiveLong);
    case 'S':
      return GetClassRoot(kPrimitiveShort);
    case 'Z':
      return GetClassRoot(kPrimitiveBoolean);
    case 'V':
      return GetClassRoot(kPrimitiveVoid);
    default:
      break;
  }
  std::string printable_type(PrintableChar(type));
  ThrowNoClassDefFoundError("Not a primitive type: %s", printable_type.c_str());
  return nullptr;
}

mirror::Class* ClassLinker::InsertClass(const char* descriptor, mirror::Class* klass,
                                        size_t hash) {
  if (VLOG_IS_ON(class_linker)) {
    mirror::DexCache* dex_cache = klass->GetDexCache();
    std::string source;
    if (dex_cache != nullptr) {
      source += " from ";
      source += dex_cache->GetLocation()->ToModifiedUtf8();
    }
    LOG(INFO) << "Loaded class " << descriptor << source;
  }
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  mirror::Class* existing =
      LookupClassFromTableLocked(descriptor, klass->GetClassLoader(), hash);
  if (existing != nullptr) {
    return existing;
  }
  if (kIsDebugBuild && !klass->IsTemp() && klass->GetClassLoader() == nullptr &&
      dex_cache_image_class_lookup_required_) {
    // Check a class loaded with the system class loader matches one in the image if the class
    // is in the image.
    existing = LookupClassFromImage(descriptor);
    if (existing != nullptr) {
      CHECK(klass == existing);
    }
  }
  VerifyObject(klass);
  class_table_.insert(std::make_pair(hash, GcRoot<mirror::Class>(klass)));
  if (log_new_class_table_roots_) {
    new_class_roots_.push_back(std::make_pair(hash, GcRoot<mirror::Class>(klass)));
  }
  return nullptr;
}

mirror::Class* ClassLinker::UpdateClass(const char* descriptor, mirror::Class* klass,
                                        size_t hash) {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  mirror::Class* existing =
      LookupClassFromTableLocked(descriptor, klass->GetClassLoader(), hash);

  if (existing == nullptr) {
    CHECK(klass->IsProxyClass());
    return nullptr;
  }

  CHECK_NE(existing, klass) << descriptor;
  CHECK(!existing->IsResolved()) << descriptor;
  CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusResolving) << descriptor;

  for (auto it = class_table_.lower_bound(hash), end = class_table_.end();
       it != end && it->first == hash; ++it) {
    mirror::Class* klass = it->second.Read();
    if (klass == existing) {
      class_table_.erase(it);
      break;
    }
  }

  CHECK(!klass->IsTemp()) << descriptor;
  if (kIsDebugBuild && klass->GetClassLoader() == nullptr &&
      dex_cache_image_class_lookup_required_) {
    // Check a class loaded with the system class loader matches one in the image if the class
    // is in the image.
    existing = LookupClassFromImage(descriptor);
    if (existing != nullptr) {
      CHECK(klass == existing) << descriptor;
    }
  }
  VerifyObject(klass);

  class_table_.insert(std::make_pair(hash, GcRoot<mirror::Class>(klass)));
  if (log_new_class_table_roots_) {
    new_class_roots_.push_back(std::make_pair(hash, GcRoot<mirror::Class>(klass)));
  }

  return existing;
}

bool ClassLinker::RemoveClass(const char* descriptor, const mirror::ClassLoader* class_loader) {
  size_t hash = Hash(descriptor);
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  for (auto it = class_table_.lower_bound(hash), end = class_table_.end();
       it != end && it->first == hash;
       ++it) {
    mirror::Class* klass = it->second.Read();
    if (klass->GetClassLoader() == class_loader && klass->DescriptorEquals(descriptor)) {
      class_table_.erase(it);
      return true;
    }
  }
  return false;
}

mirror::Class* ClassLinker::LookupClass(Thread* self, const char* descriptor,
                                        const mirror::ClassLoader* class_loader) {
  size_t hash = Hash(descriptor);
  {
    ReaderMutexLock mu(self, *Locks::classlinker_classes_lock_);
    mirror::Class* result = LookupClassFromTableLocked(descriptor, class_loader, hash);
    if (result != nullptr) {
      return result;
    }
  }
  if (class_loader != nullptr || !dex_cache_image_class_lookup_required_) {
    return nullptr;
  } else {
    // Lookup failed but need to search dex_caches_.
    mirror::Class* result = LookupClassFromImage(descriptor);
    if (result != nullptr) {
      InsertClass(descriptor, result, hash);
    } else {
      // Searching the image dex files/caches failed, we don't want to get into this situation
      // often as map searches are faster, so after kMaxFailedDexCacheLookups move all image
      // classes into the class table.
      constexpr uint32_t kMaxFailedDexCacheLookups = 1000;
      if (++failed_dex_cache_class_lookups_ > kMaxFailedDexCacheLookups) {
        MoveImageClassesToClassTable();
      }
    }
    return result;
  }
}

mirror::Class* ClassLinker::LookupClassFromTableLocked(const char* descriptor,
                                                       const mirror::ClassLoader* class_loader,
                                                       size_t hash) {
  auto end = class_table_.end();
  for (auto it = class_table_.lower_bound(hash); it != end && it->first == hash; ++it) {
    mirror::Class* klass = it->second.Read();
    if (klass->GetClassLoader() == class_loader && klass->DescriptorEquals(descriptor)) {
      if (kIsDebugBuild) {
        // Check for duplicates in the table.
        for (++it; it != end && it->first == hash; ++it) {
          mirror::Class* klass2 = it->second.Read();
          CHECK(!(klass2->GetClassLoader() == class_loader &&
              klass2->DescriptorEquals(descriptor)))
              << PrettyClass(klass) << " " << klass << " " << klass->GetClassLoader() << " "
              << PrettyClass(klass2) << " " << klass2 << " " << klass2->GetClassLoader();
        }
      }
      return klass;
    }
  }
  return nullptr;
}

static mirror::ObjectArray<mirror::DexCache>* GetImageDexCaches()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  gc::space::ImageSpace* image = Runtime::Current()->GetHeap()->GetImageSpace();
  CHECK(image != nullptr);
  mirror::Object* root = image->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
  return root->AsObjectArray<mirror::DexCache>();
}

void ClassLinker::MoveImageClassesToClassTable() {
  Thread* self = Thread::Current();
  WriterMutexLock mu(self, *Locks::classlinker_classes_lock_);
  if (!dex_cache_image_class_lookup_required_) {
    return;  // All dex cache classes are already in the class table.
  }
  ScopedAssertNoThreadSuspension ants(self, "Moving image classes to class table");
  mirror::ObjectArray<mirror::DexCache>* dex_caches = GetImageDexCaches();
  std::string temp;
  for (int32_t i = 0; i < dex_caches->GetLength(); i++) {
    mirror::DexCache* dex_cache = dex_caches->Get(i);
    mirror::ObjectArray<mirror::Class>* types = dex_cache->GetResolvedTypes();
    for (int32_t j = 0; j < types->GetLength(); j++) {
      mirror::Class* klass = types->Get(j);
      if (klass != nullptr) {
        DCHECK(klass->GetClassLoader() == nullptr);
        const char* descriptor = klass->GetDescriptor(&temp);
        size_t hash = Hash(descriptor);
        mirror::Class* existing = LookupClassFromTableLocked(descriptor, nullptr, hash);
        if (existing != nullptr) {
          CHECK(existing == klass) << PrettyClassAndClassLoader(existing) << " != "
              << PrettyClassAndClassLoader(klass);
        } else {
          class_table_.insert(std::make_pair(hash, GcRoot<mirror::Class>(klass)));
          if (log_new_class_table_roots_) {
            new_class_roots_.push_back(std::make_pair(hash, GcRoot<mirror::Class>(klass)));
          }
        }
      }
    }
  }
  dex_cache_image_class_lookup_required_ = false;
}

mirror::Class* ClassLinker::LookupClassFromImage(const char* descriptor) {
  ScopedAssertNoThreadSuspension ants(Thread::Current(), "Image class lookup");
  mirror::ObjectArray<mirror::DexCache>* dex_caches = GetImageDexCaches();
  for (int32_t i = 0; i < dex_caches->GetLength(); ++i) {
    mirror::DexCache* dex_cache = dex_caches->Get(i);
    const DexFile* dex_file = dex_cache->GetDexFile();
    // Try binary searching the string/type index.
    const DexFile::StringId* string_id = dex_file->FindStringId(descriptor);
    if (string_id != nullptr) {
      const DexFile::TypeId* type_id =
          dex_file->FindTypeId(dex_file->GetIndexForStringId(*string_id));
      if (type_id != nullptr) {
        uint16_t type_idx = dex_file->GetIndexForTypeId(*type_id);
        mirror::Class* klass = dex_cache->GetResolvedType(type_idx);
        if (klass != nullptr) {
          return klass;
        }
      }
    }
  }
  return nullptr;
}

void ClassLinker::LookupClasses(const char* descriptor, std::vector<mirror::Class*>& result) {
  result.clear();
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  size_t hash = Hash(descriptor);
  ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  for (auto it = class_table_.lower_bound(hash), end = class_table_.end();
      it != end && it->first == hash; ++it) {
    mirror::Class* klass = it->second.Read();
    if (klass->DescriptorEquals(descriptor)) {
      result.push_back(klass);
    }
  }
}

void ClassLinker::VerifyClass(Thread* self, Handle<mirror::Class> klass) {
  // TODO: assert that the monitor on the Class is held
  ObjectLock<mirror::Class> lock(self, klass);

  // Don't attempt to re-verify if already sufficiently verified.
  if (klass->IsVerified()) {
    EnsurePreverifiedMethods(klass);
    return;
  }
  if (klass->IsCompileTimeVerified() && Runtime::Current()->IsCompiler()) {
    return;
  }

  // The class might already be erroneous, for example at compile time if we attempted to verify
  // this class as a parent to another.
  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass.Get());
    return;
  }

  if (klass->GetStatus() == mirror::Class::kStatusResolved) {
    klass->SetStatus(mirror::Class::kStatusVerifying, self);
  } else {
    CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime)
        << PrettyClass(klass.Get());
    CHECK(!Runtime::Current()->IsCompiler());
    klass->SetStatus(mirror::Class::kStatusVerifyingAtRuntime, self);
  }

  // Skip verification if disabled.
  if (!Runtime::Current()->IsVerificationEnabled()) {
    klass->SetStatus(mirror::Class::kStatusVerified, self);
    EnsurePreverifiedMethods(klass);
    return;
  }

  // Verify super class.
  StackHandleScope<2> hs(self);
  Handle<mirror::Class> super(hs.NewHandle(klass->GetSuperClass()));
  if (super.Get() != nullptr) {
    // Acquire lock to prevent races on verifying the super class.
    ObjectLock<mirror::Class> lock(self, super);

    if (!super->IsVerified() && !super->IsErroneous()) {
      VerifyClass(self, super);
    }
    if (!super->IsCompileTimeVerified()) {
      std::string error_msg(
          StringPrintf("Rejecting class %s that attempts to sub-class erroneous class %s",
                       PrettyDescriptor(klass.Get()).c_str(),
                       PrettyDescriptor(super.Get()).c_str()));
      LOG(ERROR) << error_msg  << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8();
      Handle<mirror::Throwable> cause(hs.NewHandle(self->GetException(nullptr)));
      if (cause.Get() != nullptr) {
        self->ClearException();
      }
      ThrowVerifyError(klass.Get(), "%s", error_msg.c_str());
      if (cause.Get() != nullptr) {
        self->GetException(nullptr)->SetCause(cause.Get());
      }
      ClassReference ref(klass->GetDexCache()->GetDexFile(), klass->GetDexClassDefIndex());
      if (Runtime::Current()->IsCompiler()) {
        Runtime::Current()->GetCompilerCallbacks()->ClassRejected(ref);
      }
      klass->SetStatus(mirror::Class::kStatusError, self);
      return;
    }
  }

  // Try to use verification information from the oat file, otherwise do runtime verification.
  const DexFile& dex_file = *klass->GetDexCache()->GetDexFile();
  mirror::Class::Status oat_file_class_status(mirror::Class::kStatusNotReady);
  bool preverified = VerifyClassUsingOatFile(dex_file, klass.Get(), oat_file_class_status);
  if (oat_file_class_status == mirror::Class::kStatusError) {
    VLOG(class_linker) << "Skipping runtime verification of erroneous class "
        << PrettyDescriptor(klass.Get()) << " in "
        << klass->GetDexCache()->GetLocation()->ToModifiedUtf8();
    ThrowVerifyError(klass.Get(), "Rejecting class %s because it failed compile-time verification",
                     PrettyDescriptor(klass.Get()).c_str());
    klass->SetStatus(mirror::Class::kStatusError, self);
    return;
  }
  verifier::MethodVerifier::FailureKind verifier_failure = verifier::MethodVerifier::kNoFailure;
  std::string error_msg;
  if (!preverified) {
    verifier_failure = verifier::MethodVerifier::VerifyClass(self, klass.Get(),
                                                             Runtime::Current()->IsCompiler(),
                                                             &error_msg);
  }
  if (preverified || verifier_failure != verifier::MethodVerifier::kHardFailure) {
    if (!preverified && verifier_failure != verifier::MethodVerifier::kNoFailure) {
      VLOG(class_linker) << "Soft verification failure in class " << PrettyDescriptor(klass.Get())
          << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8()
          << " because: " << error_msg;
    }
    self->AssertNoPendingException();
    // Make sure all classes referenced by catch blocks are resolved.
    ResolveClassExceptionHandlerTypes(dex_file, klass);
    if (verifier_failure == verifier::MethodVerifier::kNoFailure) {
      // Even though there were no verifier failures we need to respect whether the super-class
      // was verified or requiring runtime reverification.
      if (super.Get() == nullptr || super->IsVerified()) {
        klass->SetStatus(mirror::Class::kStatusVerified, self);
      } else {
        CHECK_EQ(super->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime);
        klass->SetStatus(mirror::Class::kStatusRetryVerificationAtRuntime, self);
        // Pretend a soft failure occured so that we don't consider the class verified below.
        verifier_failure = verifier::MethodVerifier::kSoftFailure;
      }
    } else {
      CHECK_EQ(verifier_failure, verifier::MethodVerifier::kSoftFailure);
      // Soft failures at compile time should be retried at runtime. Soft
      // failures at runtime will be handled by slow paths in the generated
      // code. Set status accordingly.
      if (Runtime::Current()->IsCompiler()) {
        klass->SetStatus(mirror::Class::kStatusRetryVerificationAtRuntime, self);
      } else {
        klass->SetStatus(mirror::Class::kStatusVerified, self);
        // As this is a fake verified status, make sure the methods are _not_ marked preverified
        // later.
        klass->SetPreverified();
      }
    }
  } else {
    LOG(ERROR) << "Verification failed on class " << PrettyDescriptor(klass.Get())
        << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8()
        << " because: " << error_msg;
    self->AssertNoPendingException();
    ThrowVerifyError(klass.Get(), "%s", error_msg.c_str());
    klass->SetStatus(mirror::Class::kStatusError, self);
  }
  if (preverified || verifier_failure == verifier::MethodVerifier::kNoFailure) {
    // Class is verified so we don't need to do any access check on its methods.
    // Let the interpreter know it by setting the kAccPreverified flag onto each
    // method.
    // Note: we're going here during compilation and at runtime. When we set the
    // kAccPreverified flag when compiling image classes, the flag is recorded
    // in the image and is set when loading the image.
    EnsurePreverifiedMethods(klass);
  }
}

void ClassLinker::EnsurePreverifiedMethods(Handle<mirror::Class> klass) {
  if (!klass->IsPreverified()) {
    klass->SetPreverifiedFlagOnAllMethods();
    klass->SetPreverified();
  }
}

bool ClassLinker::VerifyClassUsingOatFile(const DexFile& dex_file, mirror::Class* klass,
                                          mirror::Class::Status& oat_file_class_status) {
  // If we're compiling, we can only verify the class using the oat file if
  // we are not compiling the image or if the class we're verifying is not part of
  // the app.  In other words, we will only check for preverification of bootclasspath
  // classes.
  if (Runtime::Current()->IsCompiler()) {
    // Are we compiling the bootclasspath?
    if (!Runtime::Current()->UseCompileTimeClassPath()) {
      return false;
    }
    // We are compiling an app (not the image).

    // Is this an app class? (I.e. not a bootclasspath class)
    if (klass->GetClassLoader() != nullptr) {
      return false;
    }
  }

  const OatFile::OatDexFile* oat_dex_file = FindOpenedOatDexFileForDexFile(dex_file);
  // In case we run without an image there won't be a backing oat file.
  if (oat_dex_file == nullptr) {
    return false;
  }

  uint16_t class_def_index = klass->GetDexClassDefIndex();
  oat_file_class_status = oat_dex_file->GetOatClass(class_def_index).GetStatus();
  if (oat_file_class_status == mirror::Class::kStatusVerified ||
      oat_file_class_status == mirror::Class::kStatusInitialized) {
      return true;
  }
  if (oat_file_class_status == mirror::Class::kStatusRetryVerificationAtRuntime) {
    // Compile time verification failed with a soft error. Compile time verification can fail
    // because we have incomplete type information. Consider the following:
    // class ... {
    //   Foo x;
    //   .... () {
    //     if (...) {
    //       v1 gets assigned a type of resolved class Foo
    //     } else {
    //       v1 gets assigned a type of unresolved class Bar
    //     }
    //     iput x = v1
    // } }
    // when we merge v1 following the if-the-else it results in Conflict
    // (see verifier::RegType::Merge) as we can't know the type of Bar and we could possibly be
    // allowing an unsafe assignment to the field x in the iput (javac may have compiled this as
    // it knew Bar was a sub-class of Foo, but for us this may have been moved into a separate apk
    // at compile time).
    return false;
  }
  if (oat_file_class_status == mirror::Class::kStatusError) {
    // Compile time verification failed with a hard error. This is caused by invalid instructions
    // in the class. These errors are unrecoverable.
    return false;
  }
  if (oat_file_class_status == mirror::Class::kStatusNotReady) {
    // Status is uninitialized if we couldn't determine the status at compile time, for example,
    // not loading the class.
    // TODO: when the verifier doesn't rely on Class-es failing to resolve/load the type hierarchy
    // isn't a problem and this case shouldn't occur
    return false;
  }
  std::string temp;
  LOG(FATAL) << "Unexpected class status: " << oat_file_class_status
             << " " << dex_file.GetLocation() << " " << PrettyClass(klass) << " "
             << klass->GetDescriptor(&temp);

  return false;
}

void ClassLinker::ResolveClassExceptionHandlerTypes(const DexFile& dex_file,
                                                    Handle<mirror::Class> klass) {
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    ResolveMethodExceptionHandlerTypes(dex_file, klass->GetDirectMethod(i));
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    ResolveMethodExceptionHandlerTypes(dex_file, klass->GetVirtualMethod(i));
  }
}

void ClassLinker::ResolveMethodExceptionHandlerTypes(const DexFile& dex_file,
                                                     mirror::ArtMethod* method) {
  // similar to DexVerifier::ScanTryCatchBlocks and dex2oat's ResolveExceptionsForMethod.
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());
  if (code_item == nullptr) {
    return;  // native or abstract method
  }
  if (code_item->tries_size_ == 0) {
    return;  // nothing to process
  }
  const uint8_t* handlers_ptr = DexFile::GetCatchHandlerData(*code_item, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      // Ensure exception types are resolved so that they don't need resolution to be delivered,
      // unresolved exception types will be ignored by exception delivery
      if (iterator.GetHandlerTypeIndex() != DexFile::kDexNoIndex16) {
        mirror::Class* exception_type = linker->ResolveType(iterator.GetHandlerTypeIndex(), method);
        if (exception_type == nullptr) {
          DCHECK(Thread::Current()->IsExceptionPending());
          Thread::Current()->ClearException();
        }
      }
    }
    handlers_ptr = iterator.EndDataPointer();
  }
}

static void CheckProxyConstructor(mirror::ArtMethod* constructor);
static void CheckProxyMethod(Handle<mirror::ArtMethod> method,
                             Handle<mirror::ArtMethod> prototype);

mirror::Class* ClassLinker::CreateProxyClass(ScopedObjectAccessAlreadyRunnable& soa, jstring name,
                                             jobjectArray interfaces, jobject loader,
                                             jobjectArray methods, jobjectArray throws) {
  Thread* self = soa.Self();
  StackHandleScope<8> hs(self);
  MutableHandle<mirror::Class> klass(hs.NewHandle(
      AllocClass(self, GetClassRoot(kJavaLangClass), sizeof(mirror::Class))));
  if (klass.Get() == nullptr) {
    CHECK(self->IsExceptionPending());  // OOME.
    return nullptr;
  }
  DCHECK(klass->GetClass() != nullptr);
  klass->SetObjectSize(sizeof(mirror::Proxy));
  // Set the class access flags incl. preverified, so we do not try to set the flag on the methods.
  klass->SetAccessFlags(kAccClassIsProxy | kAccPublic | kAccFinal | kAccPreverified);
  klass->SetClassLoader(soa.Decode<mirror::ClassLoader*>(loader));
  DCHECK_EQ(klass->GetPrimitiveType(), Primitive::kPrimNot);
  klass->SetName(soa.Decode<mirror::String*>(name));
  mirror::Class* proxy_class = GetClassRoot(kJavaLangReflectProxy);
  klass->SetDexCache(proxy_class->GetDexCache());
  klass->SetStatus(mirror::Class::kStatusIdx, self);

  // Instance fields are inherited, but we add a couple of static fields...
  {
    mirror::ObjectArray<mirror::ArtField>* sfields = AllocArtFieldArray(self, 2);
    if (UNLIKELY(sfields == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return nullptr;
    }
    klass->SetSFields(sfields);
  }
  // 1. Create a static field 'interfaces' that holds the _declared_ interfaces implemented by
  // our proxy, so Class.getInterfaces doesn't return the flattened set.
  Handle<mirror::ArtField> interfaces_sfield(hs.NewHandle(AllocArtField(self)));
  if (UNLIKELY(interfaces_sfield.Get() == nullptr)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return nullptr;
  }
  klass->SetStaticField(0, interfaces_sfield.Get());
  interfaces_sfield->SetDexFieldIndex(0);
  interfaces_sfield->SetDeclaringClass(klass.Get());
  interfaces_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);
  // 2. Create a static field 'throws' that holds exceptions thrown by our methods.
  Handle<mirror::ArtField> throws_sfield(hs.NewHandle(AllocArtField(self)));
  if (UNLIKELY(throws_sfield.Get() == nullptr)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return nullptr;
  }
  klass->SetStaticField(1, throws_sfield.Get());
  throws_sfield->SetDexFieldIndex(1);
  throws_sfield->SetDeclaringClass(klass.Get());
  throws_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);

  // Proxies have 1 direct method, the constructor
  {
    mirror::ObjectArray<mirror::ArtMethod>* directs = AllocArtMethodArray(self, 1);
    if (UNLIKELY(directs == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return nullptr;
    }
    klass->SetDirectMethods(directs);
    mirror::ArtMethod* constructor = CreateProxyConstructor(self, klass, proxy_class);
    if (UNLIKELY(constructor == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return nullptr;
    }
    klass->SetDirectMethod(0, constructor);
  }

  // Create virtual method using specified prototypes.
  size_t num_virtual_methods =
      soa.Decode<mirror::ObjectArray<mirror::ArtMethod>*>(methods)->GetLength();
  {
    mirror::ObjectArray<mirror::ArtMethod>* virtuals = AllocArtMethodArray(self,
                                                                           num_virtual_methods);
    if (UNLIKELY(virtuals == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return nullptr;
    }
    klass->SetVirtualMethods(virtuals);
  }
  for (size_t i = 0; i < num_virtual_methods; ++i) {
    StackHandleScope<1> hs(self);
    mirror::ObjectArray<mirror::ArtMethod>* decoded_methods =
        soa.Decode<mirror::ObjectArray<mirror::ArtMethod>*>(methods);
    Handle<mirror::ArtMethod> prototype(hs.NewHandle(decoded_methods->Get(i)));
    mirror::ArtMethod* clone = CreateProxyMethod(self, klass, prototype);
    if (UNLIKELY(clone == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return nullptr;
    }
    klass->SetVirtualMethod(i, clone);
  }

  klass->SetSuperClass(proxy_class);  // The super class is java.lang.reflect.Proxy
  klass->SetStatus(mirror::Class::kStatusLoaded, self);  // Now effectively in the loaded state.
  self->AssertNoPendingException();

  std::string descriptor(GetDescriptorForProxy(klass.Get()));
  mirror::Class* new_class = nullptr;
  {
    // Must hold lock on object when resolved.
    ObjectLock<mirror::Class> resolution_lock(self, klass);
    // Link the fields and virtual methods, creating vtable and iftables
    Handle<mirror::ObjectArray<mirror::Class> > h_interfaces(
        hs.NewHandle(soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces)));
    if (!LinkClass(self, descriptor.c_str(), klass, h_interfaces, &new_class)) {
      klass->SetStatus(mirror::Class::kStatusError, self);
      return nullptr;
    }
  }

  CHECK(klass->IsRetired());
  CHECK_NE(klass.Get(), new_class);
  klass.Assign(new_class);

  CHECK_EQ(interfaces_sfield->GetDeclaringClass(), new_class);
  interfaces_sfield->SetObject<false>(klass.Get(),
                                      soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces));
  CHECK_EQ(throws_sfield->GetDeclaringClass(), new_class);
  throws_sfield->SetObject<false>(klass.Get(),
      soa.Decode<mirror::ObjectArray<mirror::ObjectArray<mirror::Class> >*>(throws));

  {
    // Lock on klass is released. Lock new class object.
    ObjectLock<mirror::Class> initialization_lock(self, klass);
    klass->SetStatus(mirror::Class::kStatusInitialized, self);
  }

  // sanity checks
  if (kIsDebugBuild) {
    CHECK(klass->GetIFields() == nullptr);
    CheckProxyConstructor(klass->GetDirectMethod(0));
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      StackHandleScope<2> hs(self);
      mirror::ObjectArray<mirror::ArtMethod>* decoded_methods =
          soa.Decode<mirror::ObjectArray<mirror::ArtMethod>*>(methods);
      Handle<mirror::ArtMethod> prototype(hs.NewHandle(decoded_methods->Get(i)));
      Handle<mirror::ArtMethod> virtual_method(hs.NewHandle(klass->GetVirtualMethod(i)));
      CheckProxyMethod(virtual_method, prototype);
    }

    mirror::String* decoded_name = soa.Decode<mirror::String*>(name);
    std::string interfaces_field_name(StringPrintf("java.lang.Class[] %s.interfaces",
                                                   decoded_name->ToModifiedUtf8().c_str()));
    CHECK_EQ(PrettyField(klass->GetStaticField(0)), interfaces_field_name);

    std::string throws_field_name(StringPrintf("java.lang.Class[][] %s.throws",
                                               decoded_name->ToModifiedUtf8().c_str()));
    CHECK_EQ(PrettyField(klass->GetStaticField(1)), throws_field_name);

    CHECK_EQ(klass.Get()->GetInterfaces(),
             soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces));
    CHECK_EQ(klass.Get()->GetThrows(),
             soa.Decode<mirror::ObjectArray<mirror::ObjectArray<mirror::Class>>*>(throws));
  }
  mirror::Class* existing = InsertClass(descriptor.c_str(), klass.Get(), Hash(descriptor.c_str()));
  CHECK(existing == nullptr);
  return klass.Get();
}

std::string ClassLinker::GetDescriptorForProxy(mirror::Class* proxy_class) {
  DCHECK(proxy_class->IsProxyClass());
  mirror::String* name = proxy_class->GetName();
  DCHECK(name != nullptr);
  return DotToDescriptor(name->ToModifiedUtf8().c_str());
}

mirror::ArtMethod* ClassLinker::FindMethodForProxy(mirror::Class* proxy_class,
                                                   mirror::ArtMethod* proxy_method) {
  DCHECK(proxy_class->IsProxyClass());
  DCHECK(proxy_method->IsProxyMethod());
  // Locate the dex cache of the original interface/Object
  mirror::DexCache* dex_cache = nullptr;
  {
    ReaderMutexLock mu(Thread::Current(), dex_lock_);
    for (size_t i = 0; i != dex_caches_.size(); ++i) {
      mirror::DexCache* a_dex_cache = GetDexCache(i);
      if (proxy_method->HasSameDexCacheResolvedTypes(a_dex_cache->GetResolvedTypes())) {
        dex_cache = a_dex_cache;
        break;
      }
    }
  }
  CHECK(dex_cache != nullptr);
  uint32_t method_idx = proxy_method->GetDexMethodIndex();
  mirror::ArtMethod* resolved_method = dex_cache->GetResolvedMethod(method_idx);
  CHECK(resolved_method != nullptr);
  return resolved_method;
}


mirror::ArtMethod* ClassLinker::CreateProxyConstructor(Thread* self,
                                                       Handle<mirror::Class> klass,
                                                       mirror::Class* proxy_class) {
  // Create constructor for Proxy that must initialize h
  mirror::ObjectArray<mirror::ArtMethod>* proxy_direct_methods =
      proxy_class->GetDirectMethods();
  CHECK_EQ(proxy_direct_methods->GetLength(), 16);
  mirror::ArtMethod* proxy_constructor = proxy_direct_methods->Get(2);
  // Ensure constructor is in dex cache so that we can use the dex cache to look up the overridden
  // constructor method.
  proxy_class->GetDexCache()->SetResolvedMethod(proxy_constructor->GetDexMethodIndex(),
                                                proxy_constructor);
  // Clone the existing constructor of Proxy (our constructor would just invoke it so steal its
  // code_ too)
  mirror::ArtMethod* constructor = down_cast<mirror::ArtMethod*>(proxy_constructor->Clone(self));
  if (constructor == nullptr) {
    CHECK(self->IsExceptionPending());  // OOME.
    return nullptr;
  }
  // Make this constructor public and fix the class to be our Proxy version
  constructor->SetAccessFlags((constructor->GetAccessFlags() & ~kAccProtected) | kAccPublic);
  constructor->SetDeclaringClass(klass.Get());
  return constructor;
}

static void CheckProxyConstructor(mirror::ArtMethod* constructor)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(constructor->IsConstructor());
  CHECK_STREQ(constructor->GetName(), "<init>");
  CHECK_STREQ(constructor->GetSignature().ToString().c_str(),
              "(Ljava/lang/reflect/InvocationHandler;)V");
  DCHECK(constructor->IsPublic());
}

mirror::ArtMethod* ClassLinker::CreateProxyMethod(Thread* self,
                                                  Handle<mirror::Class> klass,
                                                  Handle<mirror::ArtMethod> prototype) {
  // Ensure prototype is in dex cache so that we can use the dex cache to look up the overridden
  // prototype method
  prototype->GetDeclaringClass()->GetDexCache()->SetResolvedMethod(prototype->GetDexMethodIndex(),
                                                                   prototype.Get());
  // We steal everything from the prototype (such as DexCache, invoke stub, etc.) then specialize
  // as necessary
  mirror::ArtMethod* method = down_cast<mirror::ArtMethod*>(prototype->Clone(self));
  if (UNLIKELY(method == nullptr)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return nullptr;
  }

  // Set class to be the concrete proxy class and clear the abstract flag, modify exceptions to
  // the intersection of throw exceptions as defined in Proxy
  method->SetDeclaringClass(klass.Get());
  method->SetAccessFlags((method->GetAccessFlags() & ~kAccAbstract) | kAccFinal);

  // At runtime the method looks like a reference and argument saving method, clone the code
  // related parameters from this method.
  method->SetEntryPointFromQuickCompiledCode(GetQuickProxyInvokeHandler());
  method->SetEntryPointFromPortableCompiledCode(GetPortableProxyInvokeHandler());
  method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);

  return method;
}

static void CheckProxyMethod(Handle<mirror::ArtMethod> method,
                             Handle<mirror::ArtMethod> prototype)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Basic sanity
  CHECK(!prototype->IsFinal());
  CHECK(method->IsFinal());
  CHECK(!method->IsAbstract());

  // The proxy method doesn't have its own dex cache or dex file and so it steals those of its
  // interface prototype. The exception to this are Constructors and the Class of the Proxy itself.
  CHECK_EQ(prototype->GetDexCacheStrings(), method->GetDexCacheStrings());
  CHECK(prototype->HasSameDexCacheResolvedMethods(method.Get()));
  CHECK(prototype->HasSameDexCacheResolvedTypes(method.Get()));
  CHECK_EQ(prototype->GetDexMethodIndex(), method->GetDexMethodIndex());

  CHECK_STREQ(method->GetName(), prototype->GetName());
  CHECK_STREQ(method->GetShorty(), prototype->GetShorty());
  // More complex sanity - via dex cache
  CHECK_EQ(method->GetInterfaceMethodIfProxy()->GetReturnType(), prototype->GetReturnType());
}

static bool CanWeInitializeClass(mirror::Class* klass, bool can_init_statics,
                                 bool can_init_parents)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (can_init_statics && can_init_parents) {
    return true;
  }
  if (!can_init_statics) {
    // Check if there's a class initializer.
    mirror::ArtMethod* clinit = klass->FindClassInitializer();
    if (clinit != nullptr) {
      return false;
    }
    // Check if there are encoded static values needing initialization.
    if (klass->NumStaticFields() != 0) {
      const DexFile::ClassDef* dex_class_def = klass->GetClassDef();
      DCHECK(dex_class_def != nullptr);
      if (dex_class_def->static_values_off_ != 0) {
        return false;
      }
    }
  }
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    mirror::Class* super_class = klass->GetSuperClass();
    if (!can_init_parents && !super_class->IsInitialized()) {
      return false;
    } else {
      if (!CanWeInitializeClass(super_class, can_init_statics, can_init_parents)) {
        return false;
      }
    }
  }
  return true;
}

bool ClassLinker::InitializeClass(Thread* self, Handle<mirror::Class> klass,
                                  bool can_init_statics, bool can_init_parents) {
  // see JLS 3rd edition, 12.4.2 "Detailed Initialization Procedure" for the locking protocol

  // Are we already initialized and therefore done?
  // Note: we differ from the JLS here as we don't do this under the lock, this is benign as
  // an initialized class will never change its state.
  if (klass->IsInitialized()) {
    return true;
  }

  // Fast fail if initialization requires a full runtime. Not part of the JLS.
  if (!CanWeInitializeClass(klass.Get(), can_init_statics, can_init_parents)) {
    return false;
  }

  self->AllowThreadSuspension();
  uint64_t t0;
  {
    ObjectLock<mirror::Class> lock(self, klass);

    // Re-check under the lock in case another thread initialized ahead of us.
    if (klass->IsInitialized()) {
      return true;
    }

    // Was the class already found to be erroneous? Done under the lock to match the JLS.
    if (klass->IsErroneous()) {
      ThrowEarlierClassFailure(klass.Get());
      VlogClassInitializationFailure(klass);
      return false;
    }

    CHECK(klass->IsResolved()) << PrettyClass(klass.Get()) << ": state=" << klass->GetStatus();

    if (!klass->IsVerified()) {
      VerifyClass(self, klass);
      if (!klass->IsVerified()) {
        // We failed to verify, expect either the klass to be erroneous or verification failed at
        // compile time.
        if (klass->IsErroneous()) {
          CHECK(self->IsExceptionPending());
          VlogClassInitializationFailure(klass);
        } else {
          CHECK(Runtime::Current()->IsCompiler());
          CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime);
        }
        return false;
      } else {
        self->AssertNoPendingException();
      }
    }

    // If the class is kStatusInitializing, either this thread is
    // initializing higher up the stack or another thread has beat us
    // to initializing and we need to wait. Either way, this
    // invocation of InitializeClass will not be responsible for
    // running <clinit> and will return.
    if (klass->GetStatus() == mirror::Class::kStatusInitializing) {
      // Could have got an exception during verification.
      if (self->IsExceptionPending()) {
        VlogClassInitializationFailure(klass);
        return false;
      }
      // We caught somebody else in the act; was it us?
      if (klass->GetClinitThreadId() == self->GetTid()) {
        // Yes. That's fine. Return so we can continue initializing.
        return true;
      }
      // No. That's fine. Wait for another thread to finish initializing.
      return WaitForInitializeClass(klass, self, lock);
    }

    if (!ValidateSuperClassDescriptors(klass)) {
      klass->SetStatus(mirror::Class::kStatusError, self);
      return false;
    }
    self->AllowThreadSuspension();

    CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusVerified) << PrettyClass(klass.Get());

    // From here out other threads may observe that we're initializing and so changes of state
    // require the a notification.
    klass->SetClinitThreadId(self->GetTid());
    klass->SetStatus(mirror::Class::kStatusInitializing, self);

    t0 = NanoTime();
  }

  // Initialize super classes, must be done while initializing for the JLS.
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    mirror::Class* super_class = klass->GetSuperClass();
    if (!super_class->IsInitialized()) {
      CHECK(!super_class->IsInterface());
      CHECK(can_init_parents);
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> handle_scope_super(hs.NewHandle(super_class));
      bool super_initialized = InitializeClass(self, handle_scope_super, can_init_statics, true);
      if (!super_initialized) {
        // The super class was verified ahead of entering initializing, we should only be here if
        // the super class became erroneous due to initialization.
        CHECK(handle_scope_super->IsErroneous() && self->IsExceptionPending())
            << "Super class initialization failed for "
            << PrettyDescriptor(handle_scope_super.Get())
            << " that has unexpected status " << handle_scope_super->GetStatus()
            << "\nPending exception:\n"
            << (self->GetException(nullptr) != nullptr ? self->GetException(nullptr)->Dump() : "");
        ObjectLock<mirror::Class> lock(self, klass);
        // Initialization failed because the super-class is erroneous.
        klass->SetStatus(mirror::Class::kStatusError, self);
        return false;
      }
    }
  }

  const size_t num_static_fields = klass->NumStaticFields();
  if (num_static_fields > 0) {
    const DexFile::ClassDef* dex_class_def = klass->GetClassDef();
    CHECK(dex_class_def != nullptr);
    const DexFile& dex_file = klass->GetDexFile();
    StackHandleScope<3> hs(self);
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(klass->GetClassLoader()));
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(klass->GetDexCache()));

    // Eagerly fill in static fields so that the we don't have to do as many expensive
    // Class::FindStaticField in ResolveField.
    for (size_t i = 0; i < num_static_fields; ++i) {
      mirror::ArtField* field = klass->GetStaticField(i);
      const uint32_t field_idx = field->GetDexFieldIndex();
      mirror::ArtField* resolved_field = dex_cache->GetResolvedField(field_idx);
      if (resolved_field == nullptr) {
        dex_cache->SetResolvedField(field_idx, field);
      } else {
        DCHECK_EQ(field, resolved_field);
      }
    }

    EncodedStaticFieldValueIterator value_it(dex_file, &dex_cache, &class_loader,
                                             this, *dex_class_def);
    const uint8_t* class_data = dex_file.GetClassData(*dex_class_def);
    ClassDataItemIterator field_it(dex_file, class_data);
    if (value_it.HasNext()) {
      DCHECK(field_it.HasNextStaticField());
      CHECK(can_init_statics);
      for ( ; value_it.HasNext(); value_it.Next(), field_it.Next()) {
        StackHandleScope<1> hs(self);
        Handle<mirror::ArtField> field(hs.NewHandle(
            ResolveField(dex_file, field_it.GetMemberIndex(), dex_cache, class_loader, true)));
        if (Runtime::Current()->IsActiveTransaction()) {
          value_it.ReadValueToField<true>(field);
        } else {
          value_it.ReadValueToField<false>(field);
        }
        DCHECK(!value_it.HasNext() || field_it.HasNextStaticField());
      }
    }
  }

  mirror::ArtMethod* clinit = klass->FindClassInitializer();
  if (clinit != nullptr) {
    CHECK(can_init_statics);
    JValue result;
    clinit->Invoke(self, nullptr, 0, &result, "V");
  }

  self->AllowThreadSuspension();
  uint64_t t1 = NanoTime();

  bool success = true;
  {
    ObjectLock<mirror::Class> lock(self, klass);

    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer(klass);
      klass->SetStatus(mirror::Class::kStatusError, self);
      success = false;
    } else {
      RuntimeStats* global_stats = Runtime::Current()->GetStats();
      RuntimeStats* thread_stats = self->GetStats();
      ++global_stats->class_init_count;
      ++thread_stats->class_init_count;
      global_stats->class_init_time_ns += (t1 - t0);
      thread_stats->class_init_time_ns += (t1 - t0);
      // Set the class as initialized except if failed to initialize static fields.
      klass->SetStatus(mirror::Class::kStatusInitialized, self);
      if (VLOG_IS_ON(class_linker)) {
        std::string temp;
        LOG(INFO) << "Initialized class " << klass->GetDescriptor(&temp) << " from " <<
            klass->GetLocation();
      }
      // Opportunistically set static method trampolines to their destination.
      FixupStaticTrampolines(klass.Get());
    }
  }
  return success;
}

bool ClassLinker::WaitForInitializeClass(Handle<mirror::Class> klass, Thread* self,
                                         ObjectLock<mirror::Class>& lock)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  while (true) {
    self->AssertNoPendingException();
    CHECK(!klass->IsInitialized());
    lock.WaitIgnoringInterrupts();

    // When we wake up, repeat the test for init-in-progress.  If
    // there's an exception pending (only possible if
    // we were not using WaitIgnoringInterrupts), bail out.
    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer(klass);
      klass->SetStatus(mirror::Class::kStatusError, self);
      return false;
    }
    // Spurious wakeup? Go back to waiting.
    if (klass->GetStatus() == mirror::Class::kStatusInitializing) {
      continue;
    }
    if (klass->GetStatus() == mirror::Class::kStatusVerified && Runtime::Current()->IsCompiler()) {
      // Compile time initialization failed.
      return false;
    }
    if (klass->IsErroneous()) {
      // The caller wants an exception, but it was thrown in a
      // different thread.  Synthesize one here.
      ThrowNoClassDefFoundError("<clinit> failed for class %s; see exception in other thread",
                                PrettyDescriptor(klass.Get()).c_str());
      VlogClassInitializationFailure(klass);
      return false;
    }
    if (klass->IsInitialized()) {
      return true;
    }
    LOG(FATAL) << "Unexpected class status. " << PrettyClass(klass.Get()) << " is "
        << klass->GetStatus();
  }
  UNREACHABLE();
}

bool ClassLinker::ValidateSuperClassDescriptors(Handle<mirror::Class> klass) {
  if (klass->IsInterface()) {
    return true;
  }
  // Begin with the methods local to the superclass.
  Thread* self = Thread::Current();
  StackHandleScope<2> hs(self);
  MutableMethodHelper mh(hs.NewHandle<mirror::ArtMethod>(nullptr));
  MutableMethodHelper super_mh(hs.NewHandle<mirror::ArtMethod>(nullptr));
  if (klass->HasSuperClass() &&
      klass->GetClassLoader() != klass->GetSuperClass()->GetClassLoader()) {
    for (int i = klass->GetSuperClass()->GetVTableLength() - 1; i >= 0; --i) {
      mh.ChangeMethod(klass->GetVTableEntry(i));
      super_mh.ChangeMethod(klass->GetSuperClass()->GetVTableEntry(i));
      if (mh.GetMethod() != super_mh.GetMethod() &&
          !mh.HasSameSignatureWithDifferentClassLoaders(self, &super_mh)) {
        ThrowLinkageError(klass.Get(),
                          "Class %s method %s resolves differently in superclass %s",
                          PrettyDescriptor(klass.Get()).c_str(),
                          PrettyMethod(mh.GetMethod()).c_str(),
                          PrettyDescriptor(klass->GetSuperClass()).c_str());
        return false;
      }
    }
  }
  for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
    if (klass->GetClassLoader() != klass->GetIfTable()->GetInterface(i)->GetClassLoader()) {
      uint32_t num_methods = klass->GetIfTable()->GetInterface(i)->NumVirtualMethods();
      for (uint32_t j = 0; j < num_methods; ++j) {
        mh.ChangeMethod(klass->GetIfTable()->GetMethodArray(i)->GetWithoutChecks(j));
        super_mh.ChangeMethod(klass->GetIfTable()->GetInterface(i)->GetVirtualMethod(j));
        if (mh.GetMethod() != super_mh.GetMethod() &&
            !mh.HasSameSignatureWithDifferentClassLoaders(self, &super_mh)) {
          ThrowLinkageError(klass.Get(),
                            "Class %s method %s resolves differently in interface %s",
                            PrettyDescriptor(klass.Get()).c_str(),
                            PrettyMethod(mh.GetMethod()).c_str(),
                            PrettyDescriptor(klass->GetIfTable()->GetInterface(i)).c_str());
          return false;
        }
      }
    }
  }
  return true;
}

bool ClassLinker::EnsureInitialized(Thread* self, Handle<mirror::Class> c, bool can_init_fields,
                                    bool can_init_parents) {
  DCHECK(c.Get() != nullptr);
  if (c->IsInitialized()) {
    EnsurePreverifiedMethods(c);
    return true;
  }
  const bool success = InitializeClass(self, c, can_init_fields, can_init_parents);
  if (!success) {
    if (can_init_fields && can_init_parents) {
      CHECK(self->IsExceptionPending()) << PrettyClass(c.Get());
    }
  } else {
    self->AssertNoPendingException();
  }
  return success;
}

void ClassLinker::FixupTemporaryDeclaringClass(mirror::Class* temp_class, mirror::Class* new_class) {
  mirror::ObjectArray<mirror::ArtField>* fields = new_class->GetIFields();
  if (fields != nullptr) {
    for (int index = 0; index < fields->GetLength(); index ++) {
      if (fields->Get(index)->GetDeclaringClass() == temp_class) {
        fields->Get(index)->SetDeclaringClass(new_class);
      }
    }
  }

  fields = new_class->GetSFields();
  if (fields != nullptr) {
    for (int index = 0; index < fields->GetLength(); index ++) {
      if (fields->Get(index)->GetDeclaringClass() == temp_class) {
        fields->Get(index)->SetDeclaringClass(new_class);
      }
    }
  }

  mirror::ObjectArray<mirror::ArtMethod>* methods = new_class->GetDirectMethods();
  if (methods != nullptr) {
    for (int index = 0; index < methods->GetLength(); index ++) {
      if (methods->Get(index)->GetDeclaringClass() == temp_class) {
        methods->Get(index)->SetDeclaringClass(new_class);
      }
    }
  }

  methods = new_class->GetVirtualMethods();
  if (methods != nullptr) {
    for (int index = 0; index < methods->GetLength(); index ++) {
      if (methods->Get(index)->GetDeclaringClass() == temp_class) {
        methods->Get(index)->SetDeclaringClass(new_class);
      }
    }
  }
}

bool ClassLinker::LinkClass(Thread* self, const char* descriptor, Handle<mirror::Class> klass,
                            Handle<mirror::ObjectArray<mirror::Class>> interfaces,
                            mirror::Class** new_class) {
  CHECK_EQ(mirror::Class::kStatusLoaded, klass->GetStatus());

  if (!LinkSuperClass(klass)) {
    return false;
  }
  StackHandleScope<mirror::Class::kImtSize> imt_handle_scope(
      self, Runtime::Current()->GetImtUnimplementedMethod());
  if (!LinkMethods(self, klass, interfaces, &imt_handle_scope)) {
    return false;
  }
  if (!LinkInstanceFields(self, klass)) {
    return false;
  }
  size_t class_size;
  if (!LinkStaticFields(self, klass, &class_size)) {
    return false;
  }
  CreateReferenceInstanceOffsets(klass);
  CHECK_EQ(mirror::Class::kStatusLoaded, klass->GetStatus());

  if (!klass->IsTemp() || (!init_done_ && klass->GetClassSize() == class_size)) {
    // We don't need to retire this class as it has no embedded tables or it was created the
    // correct size during class linker initialization.
    CHECK_EQ(klass->GetClassSize(), class_size) << PrettyDescriptor(klass.Get());

    if (klass->ShouldHaveEmbeddedImtAndVTable()) {
      klass->PopulateEmbeddedImtAndVTable(&imt_handle_scope);
    }

    // This will notify waiters on klass that saw the not yet resolved
    // class in the class_table_ during EnsureResolved.
    klass->SetStatus(mirror::Class::kStatusResolved, self);
    *new_class = klass.Get();
  } else {
    CHECK(!klass->IsResolved());
    // Retire the temporary class and create the correctly sized resolved class.
    *new_class = klass->CopyOf(self, class_size, &imt_handle_scope);
    if (UNLIKELY(*new_class == nullptr)) {
      CHECK(self->IsExceptionPending());  // Expect an OOME.
      klass->SetStatus(mirror::Class::kStatusError, self);
      return false;
    }

    CHECK_EQ((*new_class)->GetClassSize(), class_size);
    StackHandleScope<1> hs(self);
    auto new_class_h = hs.NewHandleWrapper<mirror::Class>(new_class);
    ObjectLock<mirror::Class> lock(self, new_class_h);

    FixupTemporaryDeclaringClass(klass.Get(), new_class_h.Get());

    mirror::Class* existing = UpdateClass(descriptor, new_class_h.Get(), Hash(descriptor));
    CHECK(existing == nullptr || existing == klass.Get());

    // This will notify waiters on temp class that saw the not yet resolved class in the
    // class_table_ during EnsureResolved.
    klass->SetStatus(mirror::Class::kStatusRetired, self);

    CHECK_EQ(new_class_h->GetStatus(), mirror::Class::kStatusResolving);
    // This will notify waiters on new_class that saw the not yet resolved
    // class in the class_table_ during EnsureResolved.
    new_class_h->SetStatus(mirror::Class::kStatusResolved, self);
  }
  return true;
}

bool ClassLinker::LoadSuperAndInterfaces(Handle<mirror::Class> klass, const DexFile& dex_file) {
  CHECK_EQ(mirror::Class::kStatusIdx, klass->GetStatus());
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(klass->GetDexClassDefIndex());
  uint16_t super_class_idx = class_def.superclass_idx_;
  if (super_class_idx != DexFile::kDexNoIndex16) {
    mirror::Class* super_class = ResolveType(dex_file, super_class_idx, klass.Get());
    if (super_class == nullptr) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return false;
    }
    // Verify
    if (!klass->CanAccess(super_class)) {
      ThrowIllegalAccessError(klass.Get(), "Class %s extended by class %s is inaccessible",
                              PrettyDescriptor(super_class).c_str(),
                              PrettyDescriptor(klass.Get()).c_str());
      return false;
    }
    CHECK(super_class->IsResolved());
    klass->SetSuperClass(super_class);
  }
  const DexFile::TypeList* interfaces = dex_file.GetInterfacesList(class_def);
  if (interfaces != nullptr) {
    for (size_t i = 0; i < interfaces->Size(); i++) {
      uint16_t idx = interfaces->GetTypeItem(i).type_idx_;
      mirror::Class* interface = ResolveType(dex_file, idx, klass.Get());
      if (interface == nullptr) {
        DCHECK(Thread::Current()->IsExceptionPending());
        return false;
      }
      // Verify
      if (!klass->CanAccess(interface)) {
        // TODO: the RI seemed to ignore this in my testing.
        ThrowIllegalAccessError(klass.Get(), "Interface %s implemented by class %s is inaccessible",
                                PrettyDescriptor(interface).c_str(),
                                PrettyDescriptor(klass.Get()).c_str());
        return false;
      }
    }
  }
  // Mark the class as loaded.
  klass->SetStatus(mirror::Class::kStatusLoaded, nullptr);
  return true;
}

bool ClassLinker::LinkSuperClass(Handle<mirror::Class> klass) {
  CHECK(!klass->IsPrimitive());
  mirror::Class* super = klass->GetSuperClass();
  if (klass.Get() == GetClassRoot(kJavaLangObject)) {
    if (super != nullptr) {
      ThrowClassFormatError(klass.Get(), "java.lang.Object must not have a superclass");
      return false;
    }
    return true;
  }
  if (super == nullptr) {
    ThrowLinkageError(klass.Get(), "No superclass defined for class %s",
                      PrettyDescriptor(klass.Get()).c_str());
    return false;
  }
  // Verify
  if (super->IsFinal() || super->IsInterface()) {
    ThrowIncompatibleClassChangeError(klass.Get(), "Superclass %s of %s is %s",
                                      PrettyDescriptor(super).c_str(),
                                      PrettyDescriptor(klass.Get()).c_str(),
                                      super->IsFinal() ? "declared final" : "an interface");
    return false;
  }
  if (!klass->CanAccess(super)) {
    ThrowIllegalAccessError(klass.Get(), "Superclass %s is inaccessible to class %s",
                            PrettyDescriptor(super).c_str(),
                            PrettyDescriptor(klass.Get()).c_str());
    return false;
  }

  // Inherit kAccClassIsFinalizable from the superclass in case this
  // class doesn't override finalize.
  if (super->IsFinalizable()) {
    klass->SetFinalizable();
  }

  // Inherit reference flags (if any) from the superclass.
  int reference_flags = (super->GetAccessFlags() & kAccReferenceFlagsMask);
  if (reference_flags != 0) {
    klass->SetAccessFlags(klass->GetAccessFlags() | reference_flags);
  }
  // Disallow custom direct subclasses of java.lang.ref.Reference.
  if (init_done_ && super == GetClassRoot(kJavaLangRefReference)) {
    ThrowLinkageError(klass.Get(),
                      "Class %s attempts to subclass java.lang.ref.Reference, which is not allowed",
                      PrettyDescriptor(klass.Get()).c_str());
    return false;
  }

  if (kIsDebugBuild) {
    // Ensure super classes are fully resolved prior to resolving fields..
    while (super != nullptr) {
      CHECK(super->IsResolved());
      super = super->GetSuperClass();
    }
  }
  return true;
}

// Populate the class vtable and itable. Compute return type indices.
bool ClassLinker::LinkMethods(Thread* self, Handle<mirror::Class> klass,
                              Handle<mirror::ObjectArray<mirror::Class>> interfaces,
                              StackHandleScope<mirror::Class::kImtSize>* out_imt) {
  self->AllowThreadSuspension();
  if (klass->IsInterface()) {
    // No vtable.
    size_t count = klass->NumVirtualMethods();
    if (!IsUint(16, count)) {
      ThrowClassFormatError(klass.Get(), "Too many methods on interface: %zd", count);
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      klass->GetVirtualMethodDuringLinking(i)->SetMethodIndex(i);
    }
  } else if (!LinkVirtualMethods(self, klass)) {  // Link virtual methods first.
    return false;
  }
  return LinkInterfaceMethods(self, klass, interfaces, out_imt);  // Link interface method last.
}

// Comparator for name and signature of a method, used in finding overriding methods. Implementation
// avoids the use of handles, if it didn't then rather than compare dex files we could compare dex
// caches in the implementation below.
class MethodNameAndSignatureComparator FINAL : public ValueObject {
 public:
  explicit MethodNameAndSignatureComparator(mirror::ArtMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) :
      dex_file_(method->GetDexFile()), mid_(&dex_file_->GetMethodId(method->GetDexMethodIndex())),
      name_(nullptr), name_len_(0) {
    DCHECK(!method->IsProxyMethod()) << PrettyMethod(method);
  }

  const char* GetName() {
    if (name_ == nullptr) {
      name_ = dex_file_->StringDataAndUtf16LengthByIdx(mid_->name_idx_, &name_len_);
    }
    return name_;
  }

  bool HasSameNameAndSignature(mirror::ArtMethod* other)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(!other->IsProxyMethod()) << PrettyMethod(other);
    const DexFile* other_dex_file = other->GetDexFile();
    const DexFile::MethodId& other_mid = other_dex_file->GetMethodId(other->GetDexMethodIndex());
    if (dex_file_ == other_dex_file) {
      return mid_->name_idx_ == other_mid.name_idx_ && mid_->proto_idx_ == other_mid.proto_idx_;
    }
    GetName();  // Only used to make sure its calculated.
    uint32_t other_name_len;
    const char* other_name = other_dex_file->StringDataAndUtf16LengthByIdx(other_mid.name_idx_,
                                                                           &other_name_len);
    if (name_len_ != other_name_len || strcmp(name_, other_name) != 0) {
      return false;
    }
    return dex_file_->GetMethodSignature(*mid_) == other_dex_file->GetMethodSignature(other_mid);
  }

 private:
  // Dex file for the method to compare against.
  const DexFile* const dex_file_;
  // MethodId for the method to compare against.
  const DexFile::MethodId* const mid_;
  // Lazily computed name from the dex file's strings.
  const char* name_;
  // Lazily computed name length.
  uint32_t name_len_;
};

class LinkVirtualHashTable {
 public:
  LinkVirtualHashTable(Handle<mirror::Class> klass, size_t hash_size, uint32_t* hash_table)
     : klass_(klass), hash_size_(hash_size), hash_table_(hash_table) {
    std::fill(hash_table_, hash_table_ + hash_size_, invalid_index_);
  }
  void Add(uint32_t virtual_method_index) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* local_method = klass_->GetVirtualMethodDuringLinking(virtual_method_index);
    const char* name = local_method->GetName();
    uint32_t hash = Hash(name);
    uint32_t index = hash % hash_size_;
    // Linear probe until we have an empty slot.
    while (hash_table_[index] != invalid_index_) {
      if (++index == hash_size_) {
        index = 0;
      }
    }
    hash_table_[index] = virtual_method_index;
  }
  uint32_t FindAndRemove(MethodNameAndSignatureComparator* comparator)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const char* name = comparator->GetName();
    uint32_t hash = Hash(name);
    size_t index = hash % hash_size_;
    while (true) {
      const uint32_t value = hash_table_[index];
      // Since linear probe makes continuous blocks, hitting an invalid index means we are done
      // the block and can safely assume not found.
      if (value == invalid_index_) {
        break;
      }
      if (value != removed_index_) {  // This signifies not already overriden.
        mirror::ArtMethod* virtual_method =
            klass_->GetVirtualMethodDuringLinking(value);
        if (comparator->HasSameNameAndSignature(virtual_method->GetInterfaceMethodIfProxy())) {
          hash_table_[index] = removed_index_;
          return value;
        }
      }
      if (++index == hash_size_) {
        index = 0;
      }
    }
    return GetNotFoundIndex();
  }
  static uint32_t GetNotFoundIndex() {
    return invalid_index_;
  }

 private:
  static const uint32_t invalid_index_;
  static const uint32_t removed_index_;

  Handle<mirror::Class> klass_;
  const size_t hash_size_;
  uint32_t* const hash_table_;
};

const uint32_t LinkVirtualHashTable::invalid_index_ = std::numeric_limits<uint32_t>::max();
const uint32_t LinkVirtualHashTable::removed_index_ = std::numeric_limits<uint32_t>::max() - 1;

bool ClassLinker::LinkVirtualMethods(Thread* self, Handle<mirror::Class> klass) {
  const size_t num_virtual_methods = klass->NumVirtualMethods();
  if (klass->HasSuperClass()) {
    const size_t super_vtable_length = klass->GetSuperClass()->GetVTableLength();
    const size_t max_count = num_virtual_methods + super_vtable_length;
    StackHandleScope<2> hs(self);
    Handle<mirror::Class> super_class(hs.NewHandle(klass->GetSuperClass()));
    MutableHandle<mirror::ObjectArray<mirror::ArtMethod>> vtable;
    if (super_class->ShouldHaveEmbeddedImtAndVTable()) {
      vtable = hs.NewHandle(AllocArtMethodArray(self, max_count));
      if (UNLIKELY(vtable.Get() == nullptr)) {
        CHECK(self->IsExceptionPending());  // OOME.
        return false;
      }
      for (size_t i = 0; i < super_vtable_length; i++) {
        vtable->SetWithoutChecks<false>(i, super_class->GetEmbeddedVTableEntry(i));
      }
      if (num_virtual_methods == 0) {
        klass->SetVTable(vtable.Get());
        return true;
      }
    } else {
      mirror::ObjectArray<mirror::ArtMethod>* super_vtable = super_class->GetVTable();
      CHECK(super_vtable != nullptr) << PrettyClass(super_class.Get());
      if (num_virtual_methods == 0) {
        klass->SetVTable(super_vtable);
        return true;
      }
      vtable = hs.NewHandle(super_vtable->CopyOf(self, max_count));
      if (UNLIKELY(vtable.Get() == nullptr)) {
        CHECK(self->IsExceptionPending());  // OOME.
        return false;
      }
    }
    // How the algorithm works:
    // 1. Populate hash table by adding num_virtual_methods from klass. The values in the hash
    // table are: invalid_index for unused slots, index super_vtable_length + i for a virtual
    // method which has not been matched to a vtable method, and j if the virtual method at the
    // index overrode the super virtual method at index j.
    // 2. Loop through super virtual methods, if they overwrite, update hash table to j
    // (j < super_vtable_length) to avoid redundant checks. (TODO maybe use this info for reducing
    // the need for the initial vtable which we later shrink back down).
    // 3. Add non overridden methods to the end of the vtable.
    static constexpr size_t kMaxStackHash = 250;
    const size_t hash_table_size = num_virtual_methods * 3;
    uint32_t* hash_table_ptr;
    std::unique_ptr<uint32_t[]> hash_heap_storage;
    if (hash_table_size <= kMaxStackHash) {
      hash_table_ptr = reinterpret_cast<uint32_t*>(
          alloca(hash_table_size * sizeof(*hash_table_ptr)));
    } else {
      hash_heap_storage.reset(new uint32_t[hash_table_size]);
      hash_table_ptr = hash_heap_storage.get();
    }
    LinkVirtualHashTable hash_table(klass, hash_table_size, hash_table_ptr);
    // Add virtual methods to the hash table.
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      hash_table.Add(i);
    }
    // Loop through each super vtable method and see if they are overriden by a method we added to
    // the hash table.
    for (size_t j = 0; j < super_vtable_length; ++j) {
      // Search the hash table to see if we are overidden by any method.
      mirror::ArtMethod* super_method = vtable->GetWithoutChecks(j);
      MethodNameAndSignatureComparator super_method_name_comparator(
          super_method->GetInterfaceMethodIfProxy());
      uint32_t hash_index = hash_table.FindAndRemove(&super_method_name_comparator);
      if (hash_index != hash_table.GetNotFoundIndex()) {
        mirror::ArtMethod* virtual_method = klass->GetVirtualMethodDuringLinking(hash_index);
        if (klass->CanAccessMember(super_method->GetDeclaringClass(),
                                   super_method->GetAccessFlags())) {
          if (super_method->IsFinal()) {
            ThrowLinkageError(klass.Get(), "Method %s overrides final method in class %s",
                              PrettyMethod(virtual_method).c_str(),
                              super_method->GetDeclaringClassDescriptor());
            return false;
          }
          vtable->SetWithoutChecks<false>(j, virtual_method);
          virtual_method->SetMethodIndex(j);
        } else {
          LOG(WARNING) << "Before Android 4.1, method " << PrettyMethod(virtual_method)
                       << " would have incorrectly overridden the package-private method in "
                       << PrettyDescriptor(super_method->GetDeclaringClassDescriptor());
        }
      }
    }
    // Add the non overridden methods at the end.
    size_t actual_count = super_vtable_length;
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      mirror::ArtMethod* local_method = klass->GetVirtualMethodDuringLinking(i);
      size_t method_idx = local_method->GetMethodIndexDuringLinking();
      if (method_idx < super_vtable_length &&
          local_method == vtable->GetWithoutChecks(method_idx)) {
        continue;
      }
      vtable->SetWithoutChecks<false>(actual_count, local_method);
      local_method->SetMethodIndex(actual_count);
      ++actual_count;
    }
    if (!IsUint(16, actual_count)) {
      ThrowClassFormatError(klass.Get(), "Too many methods defined on class: %zd", actual_count);
      return false;
    }
    // Shrink vtable if possible
    CHECK_LE(actual_count, max_count);
    if (actual_count < max_count) {
      vtable.Assign(vtable->CopyOf(self, actual_count));
      if (UNLIKELY(vtable.Get() == nullptr)) {
        CHECK(self->IsExceptionPending());  // OOME.
        return false;
      }
    }
    klass->SetVTable(vtable.Get());
  } else {
    CHECK_EQ(klass.Get(), GetClassRoot(kJavaLangObject));
    if (!IsUint(16, num_virtual_methods)) {
      ThrowClassFormatError(klass.Get(), "Too many methods: %d",
                            static_cast<int>(num_virtual_methods));
      return false;
    }
    mirror::ObjectArray<mirror::ArtMethod>* vtable = AllocArtMethodArray(self, num_virtual_methods);
    if (UNLIKELY(vtable == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return false;
    }
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      mirror::ArtMethod* virtual_method = klass->GetVirtualMethodDuringLinking(i);
      vtable->SetWithoutChecks<false>(i, virtual_method);
      virtual_method->SetMethodIndex(i & 0xFFFF);
    }
    klass->SetVTable(vtable);
  }
  return true;
}

bool ClassLinker::LinkInterfaceMethods(Thread* self, Handle<mirror::Class> klass,
                                       Handle<mirror::ObjectArray<mirror::Class>> interfaces,
                                       StackHandleScope<mirror::Class::kImtSize>* out_imt) {
  StackHandleScope<3> hs(self);
  Runtime* const runtime = Runtime::Current();
  const bool has_superclass = klass->HasSuperClass();
  const size_t super_ifcount = has_superclass ? klass->GetSuperClass()->GetIfTableCount() : 0U;
  const bool have_interfaces = interfaces.Get() != nullptr;
  const size_t num_interfaces =
      have_interfaces ? interfaces->GetLength() : klass->NumDirectInterfaces();
  if (num_interfaces == 0) {
    if (super_ifcount == 0) {
      // Class implements no interfaces.
      DCHECK_EQ(klass->GetIfTableCount(), 0);
      DCHECK(klass->GetIfTable() == nullptr);
      return true;
    }
    // Class implements same interfaces as parent, are any of these not marker interfaces?
    bool has_non_marker_interface = false;
    mirror::IfTable* super_iftable = klass->GetSuperClass()->GetIfTable();
    for (size_t i = 0; i < super_ifcount; ++i) {
      if (super_iftable->GetMethodArrayCount(i) > 0) {
        has_non_marker_interface = true;
        break;
      }
    }
    // Class just inherits marker interfaces from parent so recycle parent's iftable.
    if (!has_non_marker_interface) {
      klass->SetIfTable(super_iftable);
      return true;
    }
  }
  size_t ifcount = super_ifcount + num_interfaces;
  for (size_t i = 0; i < num_interfaces; i++) {
    mirror::Class* interface = have_interfaces ?
        interfaces->GetWithoutChecks(i) : mirror::Class::GetDirectInterface(self, klass, i);
    DCHECK(interface != nullptr);
    if (UNLIKELY(!interface->IsInterface())) {
      std::string temp;
      ThrowIncompatibleClassChangeError(klass.Get(), "Class %s implements non-interface class %s",
                                        PrettyDescriptor(klass.Get()).c_str(),
                                        PrettyDescriptor(interface->GetDescriptor(&temp)).c_str());
      return false;
    }
    ifcount += interface->GetIfTableCount();
  }
  MutableHandle<mirror::IfTable> iftable(hs.NewHandle(AllocIfTable(self, ifcount)));
  if (UNLIKELY(iftable.Get() == nullptr)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return false;
  }
  if (super_ifcount != 0) {
    mirror::IfTable* super_iftable = klass->GetSuperClass()->GetIfTable();
    for (size_t i = 0; i < super_ifcount; i++) {
      mirror::Class* super_interface = super_iftable->GetInterface(i);
      iftable->SetInterface(i, super_interface);
    }
  }
  self->AllowThreadSuspension();
  // Flatten the interface inheritance hierarchy.
  size_t idx = super_ifcount;
  for (size_t i = 0; i < num_interfaces; i++) {
    mirror::Class* interface = have_interfaces ? interfaces->Get(i) :
        mirror::Class::GetDirectInterface(self, klass, i);
    // Check if interface is already in iftable
    bool duplicate = false;
    for (size_t j = 0; j < idx; j++) {
      mirror::Class* existing_interface = iftable->GetInterface(j);
      if (existing_interface == interface) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      // Add this non-duplicate interface.
      iftable->SetInterface(idx++, interface);
      // Add this interface's non-duplicate super-interfaces.
      for (int32_t j = 0; j < interface->GetIfTableCount(); j++) {
        mirror::Class* super_interface = interface->GetIfTable()->GetInterface(j);
        bool super_duplicate = false;
        for (size_t k = 0; k < idx; k++) {
          mirror::Class* existing_interface = iftable->GetInterface(k);
          if (existing_interface == super_interface) {
            super_duplicate = true;
            break;
          }
        }
        if (!super_duplicate) {
          iftable->SetInterface(idx++, super_interface);
        }
      }
    }
  }
  self->AllowThreadSuspension();
  // Shrink iftable in case duplicates were found
  if (idx < ifcount) {
    DCHECK_NE(num_interfaces, 0U);
    iftable.Assign(down_cast<mirror::IfTable*>(iftable->CopyOf(self, idx * mirror::IfTable::kMax)));
    if (UNLIKELY(iftable.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return false;
    }
    ifcount = idx;
  } else {
    DCHECK_EQ(idx, ifcount);
  }
  klass->SetIfTable(iftable.Get());
  // If we're an interface, we don't need the vtable pointers, so we're done.
  if (klass->IsInterface()) {
    return true;
  }
  size_t miranda_list_size = 0;
  size_t max_miranda_methods = 0;  // The max size of miranda_list.
  for (size_t i = 0; i < ifcount; ++i) {
    max_miranda_methods += iftable->GetInterface(i)->NumVirtualMethods();
  }
  MutableHandle<mirror::ObjectArray<mirror::ArtMethod>>
      miranda_list(hs.NewHandle(AllocArtMethodArray(self, max_miranda_methods)));
  MutableHandle<mirror::ObjectArray<mirror::ArtMethod>> vtable(
      hs.NewHandle(klass->GetVTableDuringLinking()));
  // Copy the IMT from the super class if possible.
  bool extend_super_iftable = false;
  if (has_superclass) {
    mirror::Class* super_class = klass->GetSuperClass();
    extend_super_iftable = true;
    if (super_class->ShouldHaveEmbeddedImtAndVTable()) {
      for (size_t i = 0; i < mirror::Class::kImtSize; ++i) {
        out_imt->SetReference(i, super_class->GetEmbeddedImTableEntry(i));
      }
    } else {
      // No imt in the super class, need to reconstruct from the iftable.
      mirror::IfTable* if_table = super_class->GetIfTable();
      mirror::ArtMethod* conflict_method = runtime->GetImtConflictMethod();
      const size_t length = super_class->GetIfTableCount();
      for (size_t i = 0; i < length; ++i) {
        mirror::Class* interface = iftable->GetInterface(i);
        const size_t num_virtuals = interface->NumVirtualMethods();
        const size_t method_array_count = if_table->GetMethodArrayCount(i);
        DCHECK_EQ(num_virtuals, method_array_count);
        if (method_array_count == 0) {
          continue;
        }
        mirror::ObjectArray<mirror::ArtMethod>* method_array = if_table->GetMethodArray(i);
        for (size_t j = 0; j < num_virtuals; ++j) {
          mirror::ArtMethod* method = method_array->GetWithoutChecks(j);
          if (method->IsMiranda()) {
            continue;
          }
          mirror::ArtMethod* interface_method = interface->GetVirtualMethod(j);
          uint32_t imt_index = interface_method->GetDexMethodIndex() % mirror::Class::kImtSize;
          mirror::ArtMethod* imt_ref = out_imt->GetReference(imt_index)->AsArtMethod();
          if (imt_ref == runtime->GetImtUnimplementedMethod()) {
            out_imt->SetReference(imt_index, method);
          } else if (imt_ref != conflict_method) {
            out_imt->SetReference(imt_index, conflict_method);
          }
        }
      }
    }
  }
  for (size_t i = 0; i < ifcount; ++i) {
    self->AllowThreadSuspension();
    size_t num_methods = iftable->GetInterface(i)->NumVirtualMethods();
    if (num_methods > 0) {
      StackHandleScope<2> hs(self);
      const bool is_super = i < super_ifcount;
      const bool super_interface = is_super && extend_super_iftable;
      Handle<mirror::ObjectArray<mirror::ArtMethod>> method_array;
      Handle<mirror::ObjectArray<mirror::ArtMethod>> input_array;
      if (super_interface) {
        mirror::IfTable* if_table = klass->GetSuperClass()->GetIfTable();
        DCHECK(if_table != nullptr);
        DCHECK(if_table->GetMethodArray(i) != nullptr);
        // If we are working on a super interface, try extending the existing method array.
        method_array = hs.NewHandle(if_table->GetMethodArray(i)->Clone(self)->
            AsObjectArray<mirror::ArtMethod>());
        // We are overwriting a super class interface, try to only virtual methods instead of the
        // whole vtable.
        input_array = hs.NewHandle(klass->GetVirtualMethods());
      } else {
        method_array = hs.NewHandle(AllocArtMethodArray(self, num_methods));
        // A new interface, we need the whole vtable incase a new interface method is implemented
        // in the whole superclass.
        input_array = vtable;
      }
      if (UNLIKELY(method_array.Get() == nullptr)) {
        CHECK(self->IsExceptionPending());  // OOME.
        return false;
      }
      iftable->SetMethodArray(i, method_array.Get());
      if (input_array.Get() == nullptr) {
        // If the added virtual methods is empty, do nothing.
        DCHECK(super_interface);
        continue;
      }
      for (size_t j = 0; j < num_methods; ++j) {
        mirror::ArtMethod* interface_method = iftable->GetInterface(i)->GetVirtualMethod(j);
        MethodNameAndSignatureComparator interface_name_comparator(
            interface_method->GetInterfaceMethodIfProxy());
        int32_t k;
        // For each method listed in the interface's method list, find the
        // matching method in our class's method list.  We want to favor the
        // subclass over the superclass, which just requires walking
        // back from the end of the vtable.  (This only matters if the
        // superclass defines a private method and this class redefines
        // it -- otherwise it would use the same vtable slot.  In .dex files
        // those don't end up in the virtual method table, so it shouldn't
        // matter which direction we go.  We walk it backward anyway.)
        for (k = input_array->GetLength() - 1; k >= 0; --k) {
          mirror::ArtMethod* vtable_method = input_array->GetWithoutChecks(k);
          mirror::ArtMethod* vtable_method_for_name_comparison =
              vtable_method->GetInterfaceMethodIfProxy();
          if (interface_name_comparator.HasSameNameAndSignature(
              vtable_method_for_name_comparison)) {
            if (!vtable_method->IsAbstract() && !vtable_method->IsPublic()) {
              ThrowIllegalAccessError(
                  klass.Get(),
                  "Method '%s' implementing interface method '%s' is not public",
                  PrettyMethod(vtable_method).c_str(),
                  PrettyMethod(interface_method).c_str());
              return false;
            }
            method_array->SetWithoutChecks<false>(j, vtable_method);
            // Place method in imt if entry is empty, place conflict otherwise.
            uint32_t imt_index = interface_method->GetDexMethodIndex() % mirror::Class::kImtSize;
            mirror::ArtMethod* imt_ref = out_imt->GetReference(imt_index)->AsArtMethod();
            mirror::ArtMethod* conflict_method = runtime->GetImtConflictMethod();
            if (imt_ref == runtime->GetImtUnimplementedMethod()) {
              out_imt->SetReference(imt_index, vtable_method);
            } else if (imt_ref != conflict_method) {
              // If we are not a conflict and we have the same signature and name as the imt entry,
              // it must be that we overwrote a superclass vtable entry.
              MethodNameAndSignatureComparator imt_ref_name_comparator(
                  imt_ref->GetInterfaceMethodIfProxy());
              if (imt_ref_name_comparator.HasSameNameAndSignature(
                  vtable_method_for_name_comparison)) {
                out_imt->SetReference(imt_index, vtable_method);
              } else {
                out_imt->SetReference(imt_index, conflict_method);
              }
            }
            break;
          }
        }
        if (k < 0 && !super_interface) {
          mirror::ArtMethod* miranda_method = nullptr;
          for (size_t l = 0; l < miranda_list_size; ++l) {
            mirror::ArtMethod* mir_method = miranda_list->Get(l);
            if (interface_name_comparator.HasSameNameAndSignature(mir_method)) {
              miranda_method = mir_method;
              break;
            }
          }
          if (miranda_method == nullptr) {
            // Point the interface table at a phantom slot.
            miranda_method = interface_method->Clone(self)->AsArtMethod();
            if (UNLIKELY(miranda_method == nullptr)) {
              CHECK(self->IsExceptionPending());  // OOME.
              return false;
            }
            DCHECK_LT(miranda_list_size, max_miranda_methods);
            miranda_list->Set<false>(miranda_list_size++, miranda_method);
          }
          method_array->SetWithoutChecks<false>(j, miranda_method);
        }
      }
    }
  }
  if (miranda_list_size > 0) {
    int old_method_count = klass->NumVirtualMethods();
    int new_method_count = old_method_count + miranda_list_size;
    mirror::ObjectArray<mirror::ArtMethod>* virtuals;
    if (old_method_count == 0) {
      virtuals = AllocArtMethodArray(self, new_method_count);
    } else {
      virtuals = klass->GetVirtualMethods()->CopyOf(self, new_method_count);
    }
    if (UNLIKELY(virtuals == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return false;
    }
    klass->SetVirtualMethods(virtuals);

    int old_vtable_count = vtable->GetLength();
    int new_vtable_count = old_vtable_count + miranda_list_size;
    vtable.Assign(vtable->CopyOf(self, new_vtable_count));
    if (UNLIKELY(vtable.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return false;
    }
    for (size_t i = 0; i < miranda_list_size; ++i) {
      mirror::ArtMethod* method = miranda_list->Get(i);
      // Leave the declaring class alone as type indices are relative to it
      method->SetAccessFlags(method->GetAccessFlags() | kAccMiranda);
      method->SetMethodIndex(0xFFFF & (old_vtable_count + i));
      klass->SetVirtualMethod(old_method_count + i, method);
      vtable->SetWithoutChecks<false>(old_vtable_count + i, method);
    }
    // TODO: do not assign to the vtable field until it is fully constructed.
    klass->SetVTable(vtable.Get());
  }

  if (kIsDebugBuild) {
    mirror::ObjectArray<mirror::ArtMethod>* vtable = klass->GetVTableDuringLinking();
    for (int i = 0; i < vtable->GetLength(); ++i) {
      CHECK(vtable->GetWithoutChecks(i) != nullptr);
    }
  }

  self->AllowThreadSuspension();
  return true;
}

bool ClassLinker::LinkInstanceFields(Thread* self, Handle<mirror::Class> klass) {
  CHECK(klass.Get() != nullptr);
  return LinkFields(self, klass, false, nullptr);
}

bool ClassLinker::LinkStaticFields(Thread* self, Handle<mirror::Class> klass, size_t* class_size) {
  CHECK(klass.Get() != nullptr);
  return LinkFields(self, klass, true, class_size);
}

struct LinkFieldsComparator {
  explicit LinkFieldsComparator() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  }
  // No thread safety analysis as will be called from STL. Checked lock held in constructor.
  bool operator()(mirror::ArtField* field1, mirror::ArtField* field2)
      NO_THREAD_SAFETY_ANALYSIS {
    // First come reference fields, then 64-bit, then 32-bit, and then 16-bit, then finally 8-bit.
    Primitive::Type type1 = field1->GetTypeAsPrimitiveType();
    Primitive::Type type2 = field2->GetTypeAsPrimitiveType();
    if (type1 != type2) {
      bool is_primitive1 = type1 != Primitive::kPrimNot;
      bool is_primitive2 = type2 != Primitive::kPrimNot;
      if (type1 != type2) {
        if (is_primitive1 && is_primitive2) {
          // Larger primitive types go first.
          return Primitive::ComponentSize(type1) > Primitive::ComponentSize(type2);
        } else {
          // Reference always goes first.
          return !is_primitive1;
        }
      }
    }
    // same basic group? then sort by string.
    return strcmp(field1->GetName(), field2->GetName()) < 0;
  }
};

bool ClassLinker::LinkFields(Thread* self, Handle<mirror::Class> klass, bool is_static,
                             size_t* class_size) {
  self->AllowThreadSuspension();
  size_t num_fields =
      is_static ? klass->NumStaticFields() : klass->NumInstanceFields();

  mirror::ObjectArray<mirror::ArtField>* fields =
      is_static ? klass->GetSFields() : klass->GetIFields();

  // Initialize field_offset
  MemberOffset field_offset(0);
  if (is_static) {
    uint32_t base = sizeof(mirror::Class);  // Static fields come after the class.
    if (klass->ShouldHaveEmbeddedImtAndVTable()) {
      // Static fields come after the embedded tables.
      base = mirror::Class::ComputeClassSize(true, klass->GetVTableDuringLinking()->GetLength(),
                                             0, 0, 0, 0, 0);
    }
    field_offset = MemberOffset(base);
  } else {
    mirror::Class* super_class = klass->GetSuperClass();
    if (super_class != nullptr) {
      CHECK(super_class->IsResolved())
          << PrettyClass(klass.Get()) << " " << PrettyClass(super_class);
      field_offset = MemberOffset(super_class->GetObjectSize());
    }
  }

  CHECK_EQ(num_fields == 0, fields == nullptr) << PrettyClass(klass.Get());

  // we want a relatively stable order so that adding new fields
  // minimizes disruption of C++ version such as Class and Method.
  std::deque<mirror::ArtField*> grouped_and_sorted_fields;
  const char* old_no_suspend_cause = self->StartAssertNoThreadSuspension(
      "Naked ArtField references in deque");
  for (size_t i = 0; i < num_fields; i++) {
    mirror::ArtField* f = fields->Get(i);
    CHECK(f != nullptr) << PrettyClass(klass.Get());
    grouped_and_sorted_fields.push_back(f);
  }
  std::sort(grouped_and_sorted_fields.begin(), grouped_and_sorted_fields.end(),
            LinkFieldsComparator());

  // References should be at the front.
  size_t current_field = 0;
  size_t num_reference_fields = 0;
  FieldGaps gaps;

  for (; current_field < num_fields; current_field++) {
    mirror::ArtField* field = grouped_and_sorted_fields.front();
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    bool isPrimitive = type != Primitive::kPrimNot;
    if (isPrimitive) {
      break;  // past last reference, move on to the next phase
    }
    if (UNLIKELY(!IsAligned<4>(field_offset.Uint32Value()))) {
      MemberOffset old_offset = field_offset;
      field_offset = MemberOffset(RoundUp(field_offset.Uint32Value(), 4));
      AddFieldGap(old_offset.Uint32Value(), field_offset.Uint32Value(), &gaps);
    }
    DCHECK(IsAligned<4>(field_offset.Uint32Value()));
    grouped_and_sorted_fields.pop_front();
    num_reference_fields++;
    fields->Set<false>(current_field, field);
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() + sizeof(uint32_t));
  }
  // Gaps are stored as a max heap which means that we must shuffle from largest to smallest
  // otherwise we could end up with suboptimal gap fills.
  ShuffleForward<8>(num_fields, &current_field, &field_offset,
                    fields, &grouped_and_sorted_fields, &gaps);
  ShuffleForward<4>(num_fields, &current_field, &field_offset,
                    fields, &grouped_and_sorted_fields, &gaps);
  ShuffleForward<2>(num_fields, &current_field, &field_offset,
                    fields, &grouped_and_sorted_fields, &gaps);
  ShuffleForward<1>(num_fields, &current_field, &field_offset,
                    fields, &grouped_and_sorted_fields, &gaps);
  CHECK(grouped_and_sorted_fields.empty()) << "Missed " << grouped_and_sorted_fields.size() <<
      " fields.";
  self->EndAssertNoThreadSuspension(old_no_suspend_cause);

  // We lie to the GC about the java.lang.ref.Reference.referent field, so it doesn't scan it.
  if (!is_static && klass->DescriptorEquals("Ljava/lang/ref/Reference;")) {
    // We know there are no non-reference fields in the Reference classes, and we know
    // that 'referent' is alphabetically last, so this is easy...
    CHECK_EQ(num_reference_fields, num_fields) << PrettyClass(klass.Get());
    CHECK_STREQ(fields->Get(num_fields - 1)->GetName(), "referent") << PrettyClass(klass.Get());
    --num_reference_fields;
  }

  if (kIsDebugBuild) {
    // Make sure that all reference fields appear before
    // non-reference fields, and all double-wide fields are aligned.
    bool seen_non_ref = false;
    for (size_t i = 0; i < num_fields; i++) {
      mirror::ArtField* field = fields->Get(i);
      if ((false)) {  // enable to debug field layout
        LOG(INFO) << "LinkFields: " << (is_static ? "static" : "instance")
                    << " class=" << PrettyClass(klass.Get())
                    << " field=" << PrettyField(field)
                    << " offset="
                    << field->GetField32(MemberOffset(mirror::ArtField::OffsetOffset()));
      }
      Primitive::Type type = field->GetTypeAsPrimitiveType();
      bool is_primitive = type != Primitive::kPrimNot;
      if (klass->DescriptorEquals("Ljava/lang/ref/Reference;") &&
          strcmp("referent", field->GetName()) == 0) {
        is_primitive = true;  // We lied above, so we have to expect a lie here.
      }
      if (is_primitive) {
        if (!seen_non_ref) {
          seen_non_ref = true;
          DCHECK_EQ(num_reference_fields, i) << PrettyField(field);
        }
      } else {
        DCHECK(!seen_non_ref) << PrettyField(field);
      }
    }
    if (!seen_non_ref) {
      DCHECK_EQ(num_fields, num_reference_fields) << PrettyClass(klass.Get());
    }
  }

  size_t size = field_offset.Uint32Value();
  // Update klass
  if (is_static) {
    klass->SetNumReferenceStaticFields(num_reference_fields);
    *class_size = size;
  } else {
    klass->SetNumReferenceInstanceFields(num_reference_fields);
    if (!klass->IsVariableSize()) {
      std::string temp;
      DCHECK_GE(size, sizeof(mirror::Object)) << klass->GetDescriptor(&temp);
      size_t previous_size = klass->GetObjectSize();
      if (previous_size != 0) {
        // Make sure that we didn't originally have an incorrect size.
        CHECK_EQ(previous_size, size) << klass->GetDescriptor(&temp);
      }
      klass->SetObjectSize(size);
    }
  }
  return true;
}

//  Set the bitmap of reference offsets, refOffsets, from the ifields
//  list.
void ClassLinker::CreateReferenceInstanceOffsets(Handle<mirror::Class> klass) {
  uint32_t reference_offsets = 0;
  mirror::Class* super_class = klass->GetSuperClass();
  // Leave the reference offsets as 0 for mirror::Object (the class field is handled specially).
  if (super_class != nullptr) {
    reference_offsets = super_class->GetReferenceInstanceOffsets();
    // Compute reference offsets unless our superclass overflowed.
    if (reference_offsets != mirror::Class::kClassWalkSuper) {
      size_t num_reference_fields = klass->NumReferenceInstanceFieldsDuringLinking();
      mirror::ObjectArray<mirror::ArtField>* fields = klass->GetIFields();
      // All of the fields that contain object references are guaranteed
      // to be at the beginning of the fields list.
      for (size_t i = 0; i < num_reference_fields; ++i) {
        // Note that byte_offset is the offset from the beginning of
        // object, not the offset into instance data
        mirror::ArtField* field = fields->Get(i);
        MemberOffset byte_offset = field->GetOffsetDuringLinking();
        uint32_t displaced_bitmap_position =
            (byte_offset.Uint32Value() - mirror::kObjectHeaderSize) /
            sizeof(mirror::HeapReference<mirror::Object>);
        if (displaced_bitmap_position >= 32) {
          // Can't encode offset so fall back on slow-path.
          reference_offsets = mirror::Class::kClassWalkSuper;
          break;
        } else {
          reference_offsets |= (1 << displaced_bitmap_position);
        }
      }
    }
  }
  klass->SetReferenceInstanceOffsets(reference_offsets);
}

mirror::String* ClassLinker::ResolveString(const DexFile& dex_file, uint32_t string_idx,
                                           Handle<mirror::DexCache> dex_cache) {
  DCHECK(dex_cache.Get() != nullptr);
  mirror::String* resolved = dex_cache->GetResolvedString(string_idx);
  if (resolved != nullptr) {
    return resolved;
  }
  uint32_t utf16_length;
  const char* utf8_data = dex_file.StringDataAndUtf16LengthByIdx(string_idx, &utf16_length);
  mirror::String* string = intern_table_->InternStrong(utf16_length, utf8_data);
  dex_cache->SetResolvedString(string_idx, string);
  return string;
}

mirror::Class* ClassLinker::ResolveType(const DexFile& dex_file, uint16_t type_idx,
                                        mirror::Class* referrer) {
  StackHandleScope<2> hs(Thread::Current());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(referrer->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(referrer->GetClassLoader()));
  return ResolveType(dex_file, type_idx, dex_cache, class_loader);
}

mirror::Class* ClassLinker::ResolveType(const DexFile& dex_file, uint16_t type_idx,
                                        Handle<mirror::DexCache> dex_cache,
                                        Handle<mirror::ClassLoader> class_loader) {
  DCHECK(dex_cache.Get() != nullptr);
  mirror::Class* resolved = dex_cache->GetResolvedType(type_idx);
  if (resolved == nullptr) {
    Thread* self = Thread::Current();
    const char* descriptor = dex_file.StringByTypeIdx(type_idx);
    resolved = FindClass(self, descriptor, class_loader);
    if (resolved != nullptr) {
      // TODO: we used to throw here if resolved's class loader was not the
      //       boot class loader. This was to permit different classes with the
      //       same name to be loaded simultaneously by different loaders
      dex_cache->SetResolvedType(type_idx, resolved);
    } else {
      CHECK(self->IsExceptionPending())
          << "Expected pending exception for failed resolution of: " << descriptor;
      // Convert a ClassNotFoundException to a NoClassDefFoundError.
      StackHandleScope<1> hs(self);
      Handle<mirror::Throwable> cause(hs.NewHandle(self->GetException(nullptr)));
      if (cause->InstanceOf(GetClassRoot(kJavaLangClassNotFoundException))) {
        DCHECK(resolved == nullptr);  // No Handle needed to preserve resolved.
        self->ClearException();
        ThrowNoClassDefFoundError("Failed resolution of: %s", descriptor);
        self->GetException(nullptr)->SetCause(cause.Get());
      }
    }
  }
  DCHECK((resolved == nullptr) || resolved->IsResolved() || resolved->IsErroneous())
          << PrettyDescriptor(resolved) << " " << resolved->GetStatus();
  return resolved;
}

mirror::ArtMethod* ClassLinker::ResolveMethod(const DexFile& dex_file, uint32_t method_idx,
                                              Handle<mirror::DexCache> dex_cache,
                                              Handle<mirror::ClassLoader> class_loader,
                                              Handle<mirror::ArtMethod> referrer,
                                              InvokeType type) {
  DCHECK(dex_cache.Get() != nullptr);
  // Check for hit in the dex cache.
  mirror::ArtMethod* resolved = dex_cache->GetResolvedMethod(method_idx);
  if (resolved != nullptr && !resolved->IsRuntimeMethod()) {
    return resolved;
  }
  // Fail, get the declaring class.
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  mirror::Class* klass = ResolveType(dex_file, method_id.class_idx_, dex_cache, class_loader);
  if (klass == nullptr) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }
  // Scan using method_idx, this saves string compares but will only hit for matching dex
  // caches/files.
  switch (type) {
    case kDirect:  // Fall-through.
    case kStatic:
      resolved = klass->FindDirectMethod(dex_cache.Get(), method_idx);
      break;
    case kInterface:
      resolved = klass->FindInterfaceMethod(dex_cache.Get(), method_idx);
      DCHECK(resolved == nullptr || resolved->GetDeclaringClass()->IsInterface());
      break;
    case kSuper:  // Fall-through.
    case kVirtual:
      resolved = klass->FindVirtualMethod(dex_cache.Get(), method_idx);
      break;
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << type;
      UNREACHABLE();
  }
  if (resolved == nullptr) {
    // Search by name, which works across dex files.
    const char* name = dex_file.StringDataByIdx(method_id.name_idx_);
    const Signature signature = dex_file.GetMethodSignature(method_id);
    switch (type) {
      case kDirect:  // Fall-through.
      case kStatic:
        resolved = klass->FindDirectMethod(name, signature);
        break;
      case kInterface:
        resolved = klass->FindInterfaceMethod(name, signature);
        DCHECK(resolved == nullptr || resolved->GetDeclaringClass()->IsInterface());
        break;
      case kSuper:  // Fall-through.
      case kVirtual:
        resolved = klass->FindVirtualMethod(name, signature);
        break;
    }
  }
  // If we found a method, check for incompatible class changes.
  if (LIKELY(resolved != nullptr && !resolved->CheckIncompatibleClassChange(type))) {
    // Be a good citizen and update the dex cache to speed subsequent calls.
    dex_cache->SetResolvedMethod(method_idx, resolved);
    return resolved;
  } else {
    // If we had a method, it's an incompatible-class-change error.
    if (resolved != nullptr) {
      ThrowIncompatibleClassChangeError(type, resolved->GetInvokeType(), resolved, referrer.Get());
    } else {
      // We failed to find the method which means either an access error, an incompatible class
      // change, or no such method. First try to find the method among direct and virtual methods.
      const char* name = dex_file.StringDataByIdx(method_id.name_idx_);
      const Signature signature = dex_file.GetMethodSignature(method_id);
      switch (type) {
        case kDirect:
        case kStatic:
          resolved = klass->FindVirtualMethod(name, signature);
          // Note: kDirect and kStatic are also mutually exclusive, but in that case we would
          //       have had a resolved method before, which triggers the "true" branch above.
          break;
        case kInterface:
        case kVirtual:
        case kSuper:
          resolved = klass->FindDirectMethod(name, signature);
          break;
      }

      // If we found something, check that it can be accessed by the referrer.
      if (resolved != nullptr && referrer.Get() != nullptr) {
        mirror::Class* methods_class = resolved->GetDeclaringClass();
        mirror::Class* referring_class = referrer->GetDeclaringClass();
        if (!referring_class->CanAccess(methods_class)) {
          ThrowIllegalAccessErrorClassForMethodDispatch(referring_class, methods_class,
                                                        resolved, type);
          return nullptr;
        } else if (!referring_class->CanAccessMember(methods_class,
                                                     resolved->GetAccessFlags())) {
          ThrowIllegalAccessErrorMethod(referring_class, resolved);
          return nullptr;
        }
      }

      // Otherwise, throw an IncompatibleClassChangeError if we found something, and check interface
      // methods and throw if we find the method there. If we find nothing, throw a
      // NoSuchMethodError.
      switch (type) {
        case kDirect:
        case kStatic:
          if (resolved != nullptr) {
            ThrowIncompatibleClassChangeError(type, kVirtual, resolved, referrer.Get());
          } else {
            resolved = klass->FindInterfaceMethod(name, signature);
            if (resolved != nullptr) {
              ThrowIncompatibleClassChangeError(type, kInterface, resolved, referrer.Get());
            } else {
              ThrowNoSuchMethodError(type, klass, name, signature);
            }
          }
          break;
        case kInterface:
          if (resolved != nullptr) {
            ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer.Get());
          } else {
            resolved = klass->FindVirtualMethod(name, signature);
            if (resolved != nullptr) {
              ThrowIncompatibleClassChangeError(type, kVirtual, resolved, referrer.Get());
            } else {
              ThrowNoSuchMethodError(type, klass, name, signature);
            }
          }
          break;
        case kSuper:
          if (resolved != nullptr) {
            ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer.Get());
          } else {
            ThrowNoSuchMethodError(type, klass, name, signature);
          }
          break;
        case kVirtual:
          if (resolved != nullptr) {
            ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer.Get());
          } else {
            resolved = klass->FindInterfaceMethod(name, signature);
            if (resolved != nullptr) {
              ThrowIncompatibleClassChangeError(type, kInterface, resolved, referrer.Get());
            } else {
              ThrowNoSuchMethodError(type, klass, name, signature);
            }
          }
          break;
      }
    }
    DCHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }
}

mirror::ArtField* ClassLinker::ResolveField(const DexFile& dex_file, uint32_t field_idx,
                                            Handle<mirror::DexCache> dex_cache,
                                            Handle<mirror::ClassLoader> class_loader,
                                            bool is_static) {
  DCHECK(dex_cache.Get() != nullptr);
  mirror::ArtField* resolved = dex_cache->GetResolvedField(field_idx);
  if (resolved != nullptr) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  Thread* const self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(
      hs.NewHandle(ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader)));
  if (klass.Get() == nullptr) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }

  if (is_static) {
    resolved = mirror::Class::FindStaticField(self, klass, dex_cache.Get(), field_idx);
  } else {
    resolved = klass->FindInstanceField(dex_cache.Get(), field_idx);
  }

  if (resolved == nullptr) {
    const char* name = dex_file.GetFieldName(field_id);
    const char* type = dex_file.GetFieldTypeDescriptor(field_id);
    if (is_static) {
      resolved = mirror::Class::FindStaticField(self, klass, name, type);
    } else {
      resolved = klass->FindInstanceField(name, type);
    }
    if (resolved == nullptr) {
      ThrowNoSuchFieldError(is_static ? "static " : "instance ", klass.Get(), type, name);
      return nullptr;
    }
  }
  dex_cache->SetResolvedField(field_idx, resolved);
  return resolved;
}

mirror::ArtField* ClassLinker::ResolveFieldJLS(const DexFile& dex_file,
                                               uint32_t field_idx,
                                               Handle<mirror::DexCache> dex_cache,
                                               Handle<mirror::ClassLoader> class_loader) {
  DCHECK(dex_cache.Get() != nullptr);
  mirror::ArtField* resolved = dex_cache->GetResolvedField(field_idx);
  if (resolved != nullptr) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(
      hs.NewHandle(ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader)));
  if (klass.Get() == nullptr) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return nullptr;
  }

  StringPiece name(dex_file.StringDataByIdx(field_id.name_idx_));
  StringPiece type(dex_file.StringDataByIdx(
      dex_file.GetTypeId(field_id.type_idx_).descriptor_idx_));
  resolved = mirror::Class::FindField(self, klass, name, type);
  if (resolved != nullptr) {
    dex_cache->SetResolvedField(field_idx, resolved);
  } else {
    ThrowNoSuchFieldError("", klass.Get(), type, name);
  }
  return resolved;
}

const char* ClassLinker::MethodShorty(uint32_t method_idx, mirror::ArtMethod* referrer,
                                      uint32_t* length) {
  mirror::Class* declaring_class = referrer->GetDeclaringClass();
  mirror::DexCache* dex_cache = declaring_class->GetDexCache();
  const DexFile& dex_file = *dex_cache->GetDexFile();
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  return dex_file.GetMethodShorty(method_id, length);
}

void ClassLinker::DumpAllClasses(int flags) {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  // TODO: at the time this was written, it wasn't safe to call PrettyField with the ClassLinker
  // lock held, because it might need to resolve a field's type, which would try to take the lock.
  std::vector<mirror::Class*> all_classes;
  {
    ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
    for (std::pair<const size_t, GcRoot<mirror::Class> >& it : class_table_) {
      mirror::Class* klass = it.second.Read();
      all_classes.push_back(klass);
    }
  }

  for (size_t i = 0; i < all_classes.size(); ++i) {
    all_classes[i]->DumpClass(std::cerr, flags);
  }
}

static OatFile::OatMethod CreateOatMethod(const void* code, const uint8_t* gc_map,
                                          bool is_portable) {
  CHECK_EQ(kUsePortableCompiler, is_portable);
  CHECK(code != nullptr);
  const uint8_t* base;
  uint32_t code_offset, gc_map_offset;
  if (gc_map == nullptr) {
    base = reinterpret_cast<const uint8_t*>(code);  // Base of data points at code.
    base -= sizeof(void*);  // Move backward so that code_offset != 0.
    code_offset = sizeof(void*);
    gc_map_offset = 0;
  } else {
    // TODO: 64bit support.
    base = nullptr;  // Base of data in oat file, ie 0.
    code_offset = PointerToLowMemUInt32(code);
    gc_map_offset = PointerToLowMemUInt32(gc_map);
  }
  return OatFile::OatMethod(base, code_offset, gc_map_offset);
}

bool ClassLinker::IsPortableResolutionStub(const void* entry_point) const {
  return (entry_point == GetPortableResolutionStub()) ||
      (portable_resolution_trampoline_ == entry_point);
}

bool ClassLinker::IsQuickResolutionStub(const void* entry_point) const {
  return (entry_point == GetQuickResolutionStub()) ||
      (quick_resolution_trampoline_ == entry_point);
}

bool ClassLinker::IsPortableToInterpreterBridge(const void* entry_point) const {
  return (entry_point == GetPortableToInterpreterBridge());
  // TODO: portable_to_interpreter_bridge_trampoline_ == entry_point;
}

bool ClassLinker::IsQuickToInterpreterBridge(const void* entry_point) const {
  return (entry_point == GetQuickToInterpreterBridge()) ||
      (quick_to_interpreter_bridge_trampoline_ == entry_point);
}

bool ClassLinker::IsQuickGenericJniStub(const void* entry_point) const {
  return (entry_point == GetQuickGenericJniStub()) ||
      (quick_generic_jni_trampoline_ == entry_point);
}

const void* ClassLinker::GetRuntimeQuickGenericJniStub() const {
  return GetQuickGenericJniStub();
}

void ClassLinker::SetEntryPointsToCompiledCode(mirror::ArtMethod* method, const void* method_code,
                                               bool is_portable) const {
  OatFile::OatMethod oat_method = CreateOatMethod(method_code, nullptr, is_portable);
  oat_method.LinkMethod(method);
  method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
  // Create bridges to transition between different kinds of compiled bridge.
  if (method->GetEntryPointFromPortableCompiledCode() == nullptr) {
    method->SetEntryPointFromPortableCompiledCode(GetPortableToQuickBridge());
  } else {
    CHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr);
    method->SetEntryPointFromQuickCompiledCode(GetQuickToPortableBridge());
    method->SetIsPortableCompiled();
  }
}

void ClassLinker::SetEntryPointsToInterpreter(mirror::ArtMethod* method) const {
  if (!method->IsNative()) {
    method->SetEntryPointFromInterpreter(artInterpreterToInterpreterBridge);
    method->SetEntryPointFromPortableCompiledCode(GetPortableToInterpreterBridge());
    method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
  } else {
    const void* quick_method_code = GetQuickGenericJniStub();
    OatFile::OatMethod oat_method = CreateOatMethod(quick_method_code, nullptr, false);
    oat_method.LinkMethod(method);
    method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
    method->SetEntryPointFromPortableCompiledCode(GetPortableToQuickBridge());
  }
}

void ClassLinker::DumpForSigQuit(std::ostream& os) {
  Thread* self = Thread::Current();
  if (dex_cache_image_class_lookup_required_) {
    ScopedObjectAccess soa(self);
    MoveImageClassesToClassTable();
  }
  ReaderMutexLock mu(self, *Locks::classlinker_classes_lock_);
  os << "Loaded classes: " << class_table_.size() << " allocated classes\n";
}

size_t ClassLinker::NumLoadedClasses() {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  return class_table_.size();
}

pid_t ClassLinker::GetClassesLockOwner() {
  return Locks::classlinker_classes_lock_->GetExclusiveOwnerTid();
}

pid_t ClassLinker::GetDexLockOwner() {
  return dex_lock_.GetExclusiveOwnerTid();
}

void ClassLinker::SetClassRoot(ClassRoot class_root, mirror::Class* klass) {
  DCHECK(!init_done_);

  DCHECK(klass != nullptr);
  DCHECK(klass->GetClassLoader() == nullptr);

  mirror::ObjectArray<mirror::Class>* class_roots = class_roots_.Read();
  DCHECK(class_roots != nullptr);
  DCHECK(class_roots->Get(class_root) == nullptr);
  class_roots->Set<false>(class_root, klass);
}

const char* ClassLinker::GetClassRootDescriptor(ClassRoot class_root) {
  static const char* class_roots_descriptors[] = {
    "Ljava/lang/Class;",
    "Ljava/lang/Object;",
    "[Ljava/lang/Class;",
    "[Ljava/lang/Object;",
    "Ljava/lang/String;",
    "Ljava/lang/DexCache;",
    "Ljava/lang/ref/Reference;",
    "Ljava/lang/reflect/ArtField;",
    "Ljava/lang/reflect/ArtMethod;",
    "Ljava/lang/reflect/Proxy;",
    "[Ljava/lang/String;",
    "[Ljava/lang/reflect/ArtField;",
    "[Ljava/lang/reflect/ArtMethod;",
    "Ljava/lang/ClassLoader;",
    "Ljava/lang/Throwable;",
    "Ljava/lang/ClassNotFoundException;",
    "Ljava/lang/StackTraceElement;",
    "Z",
    "B",
    "C",
    "D",
    "F",
    "I",
    "J",
    "S",
    "V",
    "[Z",
    "[B",
    "[C",
    "[D",
    "[F",
    "[I",
    "[J",
    "[S",
    "[Ljava/lang/StackTraceElement;",
  };
  COMPILE_ASSERT(arraysize(class_roots_descriptors) == size_t(kClassRootsMax),
                 mismatch_between_class_descriptors_and_class_root_enum);

  const char* descriptor = class_roots_descriptors[class_root];
  CHECK(descriptor != nullptr);
  return descriptor;
}

}  // namespace art
