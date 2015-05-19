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
#include <unistd.h>
#include <utility>
#include <vector>

#include "art_field-inl.h"
#include "base/casts.h"
#include "base/logging.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "base/value_object.h"
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
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "leb128.h"
#include "linear_alloc.h"
#include "oat.h"
#include "oat_file.h"
#include "oat_file_assistant.h"
#include "object_lock.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/field.h"
#include "mirror/iftable-inl.h"
#include "mirror/method.h"
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

static constexpr bool kSanityCheckObjects = kIsDebugBuild;

static void ThrowNoClassDefFoundError(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
static void ThrowNoClassDefFoundError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread* self = Thread::Current();
  self->ThrowNewExceptionV("Ljava/lang/NoClassDefFoundError;", fmt, args);
  va_end(args);
}

static bool HasInitWithString(Thread* self, ClassLinker* class_linker, const char* descriptor)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method = self->GetCurrentMethod(nullptr);
  StackHandleScope<1> hs(self);
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(method != nullptr ?
      method->GetDeclaringClass()->GetClassLoader()
      : nullptr));
  mirror::Class* exception_class = class_linker->FindClass(self, descriptor, class_loader);

  if (exception_class == nullptr) {
    // No exc class ~ no <init>-with-string.
    CHECK(self->IsExceptionPending());
    self->ClearException();
    return false;
  }

  mirror::ArtMethod* exception_init_method =
      exception_class->FindDeclaredDirectMethod("<init>", "(Ljava/lang/String;)V");
  return exception_init_method != nullptr;
}

void ClassLinker::ThrowEarlierClassFailure(mirror::Class* c) {
  // The class failed to initialize on a previous attempt, so we want to throw
  // a NoClassDefFoundError (v2 2.17.5).  The exception to this rule is if we
  // failed in verification, in which case v2 5.4.1 says we need to re-throw
  // the previous error.
  Runtime* const runtime = Runtime::Current();
  if (!runtime->IsAotCompiler()) {  // Give info if this occurs at runtime.
    LOG(INFO) << "Rejecting re-init on previously-failed class " << PrettyClass(c);
  }

  CHECK(c->IsErroneous()) << PrettyClass(c) << " " << c->GetStatus();
  Thread* self = Thread::Current();
  if (runtime->IsAotCompiler()) {
    // At compile time, accurate errors and NCDFE are disabled to speed compilation.
    mirror::Throwable* pre_allocated = runtime->GetPreAllocatedNoClassDefFoundError();
    self->SetException(pre_allocated);
  } else {
    if (c->GetVerifyErrorClass() != nullptr) {
      // TODO: change the verifier to store an _instance_, with a useful detail message?
      // It's possible the exception doesn't have a <init>(String).
      std::string temp;
      const char* descriptor = c->GetVerifyErrorClass()->GetDescriptor(&temp);

      if (HasInitWithString(self, this, descriptor)) {
        self->ThrowNewException(descriptor, PrettyDescriptor(c).c_str());
      } else {
        self->ThrowNewException(descriptor, nullptr);
      }
    } else {
      self->ThrowNewException("Ljava/lang/NoClassDefFoundError;",
                              PrettyDescriptor(c).c_str());
    }
  }
}

static void VlogClassInitializationFailure(Handle<mirror::Class> klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (VLOG_IS_ON(class_linker)) {
    std::string temp;
    LOG(INFO) << "Failed to initialize class " << klass->GetDescriptor(&temp) << " from "
              << klass->GetLocation() << "\n" << Thread::Current()->GetException()->Dump();
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
    self->ThrowNewWrappedException("Ljava/lang/ExceptionInInitializerError;", nullptr);
  }
  VlogClassInitializationFailure(klass);
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
    // Sort by gap size, largest first. Secondary sort by starting offset.
    return lhs.size > rhs.size || (lhs.size == rhs.size && lhs.start_offset < rhs.start_offset);
  }
};
typedef std::priority_queue<FieldGap, std::vector<FieldGap>, FieldGapsComparator> FieldGaps;

// Adds largest aligned gaps to queue of gaps.
static void AddFieldGap(uint32_t gap_start, uint32_t gap_end, FieldGaps* gaps) {
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
static void ShuffleForward(size_t* current_field_idx,
                           MemberOffset* field_offset,
                           std::deque<ArtField*>* grouped_and_sorted_fields,
                           FieldGaps* gaps)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(current_field_idx != nullptr);
  DCHECK(grouped_and_sorted_fields != nullptr);
  DCHECK(gaps != nullptr);
  DCHECK(field_offset != nullptr);

  DCHECK(IsPowerOfTwo(n));
  while (!grouped_and_sorted_fields->empty()) {
    ArtField* field = grouped_and_sorted_fields->front();
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
      quick_resolution_trampoline_(nullptr),
      quick_imt_conflict_trampoline_(nullptr),
      quick_generic_jni_trampoline_(nullptr),
      quick_to_interpreter_bridge_trampoline_(nullptr),
      image_pointer_size_(sizeof(void*)) {
  CHECK(intern_table_ != nullptr);
  for (size_t i = 0; i < kFindArrayCacheSize; ++i) {
    find_array_class_cache_[i] = GcRoot<mirror::Class>(nullptr);
  }
}

void ClassLinker::InitWithoutImage(std::vector<std::unique_ptr<const DexFile>> boot_class_path) {
  VLOG(startup) << "ClassLinker::Init";
  CHECK(!Runtime::Current()->GetHeap()->HasImageSpace()) << "Runtime has image. We should use it.";

  CHECK(!init_done_);

  // java_lang_Class comes first, it's needed for AllocClass
  Thread* const self = Thread::Current();
  gc::Heap* const heap = Runtime::Current()->GetHeap();
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
  mirror::Class::SetStatus(java_lang_Object, mirror::Class::kStatusLoaded, self);

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
  mirror::Class::SetStatus(java_lang_String, mirror::Class::kStatusResolved, self);
  java_lang_String->SetStringClass();

  // Setup java.lang.ref.Reference.
  Handle<mirror::Class> java_lang_ref_Reference(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Reference::ClassSize())));
  mirror::Reference::SetClass(java_lang_ref_Reference.Get());
  java_lang_ref_Reference->SetObjectSize(mirror::Reference::InstanceSize());
  mirror::Class::SetStatus(java_lang_ref_Reference, mirror::Class::kStatusResolved, self);

  // Create storage for root classes, save away our work so far (requires descriptors).
  class_roots_ = GcRoot<mirror::ObjectArray<mirror::Class>>(
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

  // Create long array type for AllocDexCache (done in AppendToBootClassPath).
  Handle<mirror::Class> long_array_class(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::Array::ClassSize())));
  long_array_class->SetComponentType(GetClassRoot(kPrimitiveLong));
  mirror::LongArray::SetArrayClass(long_array_class.Get());
  SetClassRoot(kLongArrayClass, long_array_class.Get());

  // now that these are registered, we can use AllocClass() and AllocObjectArray

  // Set up DexCache. This cannot be done later since AppendToBootClassPath calls AllocDexCache.
  Handle<mirror::Class> java_lang_DexCache(hs.NewHandle(
      AllocClass(self, java_lang_Class.Get(), mirror::DexCache::ClassSize())));
  SetClassRoot(kJavaLangDexCache, java_lang_DexCache.Get());
  java_lang_DexCache->SetObjectSize(mirror::DexCache::InstanceSize());
  mirror::Class::SetStatus(java_lang_DexCache, mirror::Class::kStatusResolved, self);

  // Constructor, Method, and AbstractMethod are necessary so
  // that FindClass can link members.

  Handle<mirror::Class> java_lang_reflect_ArtMethod(hs.NewHandle(
    AllocClass(self, java_lang_Class.Get(), mirror::ArtMethod::ClassSize())));
  CHECK(java_lang_reflect_ArtMethod.Get() != nullptr);
  size_t pointer_size = GetInstructionSetPointerSize(Runtime::Current()->GetInstructionSet());
  java_lang_reflect_ArtMethod->SetObjectSize(mirror::ArtMethod::InstanceSize(pointer_size));
  SetClassRoot(kJavaLangReflectArtMethod, java_lang_reflect_ArtMethod.Get());
  mirror::Class::SetStatus(java_lang_reflect_ArtMethod, mirror::Class::kStatusResolved, self);
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

  // Setup boot_class_path_ and register class_path now that we can use AllocObjectArray to create
  // DexCache instances. Needs to be after String, Field, Method arrays since AllocDexCache uses
  // these roots.
  CHECK_NE(0U, boot_class_path.size());
  for (auto& dex_file : boot_class_path) {
    CHECK(dex_file.get() != nullptr);
    AppendToBootClassPath(self, *dex_file);
    opened_dex_files_.push_back(std::move(dex_file));
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
  if (!runtime->IsAotCompiler()) {
    // We need to set up the generic trampolines since we don't have an image.
    quick_resolution_trampoline_ = GetQuickResolutionStub();
    quick_imt_conflict_trampoline_ = GetQuickImtConflictStub();
    quick_to_interpreter_bridge_trampoline_ = GetQuickToInterpreterBridge();
  }

  // Object, String and DexCache need to be rerun through FindSystemClass to finish init
  mirror::Class::SetStatus(java_lang_Object, mirror::Class::kStatusNotReady, self);
  CHECK_EQ(java_lang_Object.Get(), FindSystemClass(self, "Ljava/lang/Object;"));
  CHECK_EQ(java_lang_Object->GetObjectSize(), mirror::Object::InstanceSize());
  mirror::Class::SetStatus(java_lang_String, mirror::Class::kStatusNotReady, self);
  mirror::Class* String_class = FindSystemClass(self, "Ljava/lang/String;");
  if (java_lang_String.Get() != String_class) {
    std::ostringstream os1, os2;
    java_lang_String->DumpClass(os1, mirror::Class::kDumpClassFullDetail);
    String_class->DumpClass(os2, mirror::Class::kDumpClassFullDetail);
    LOG(FATAL) << os1.str() << "\n\n" << os2.str();
  }
  mirror::Class::SetStatus(java_lang_DexCache, mirror::Class::kStatusNotReady, self);
  CHECK_EQ(java_lang_DexCache.Get(), FindSystemClass(self, "Ljava/lang/DexCache;"));
  CHECK_EQ(java_lang_DexCache->GetObjectSize(), mirror::DexCache::InstanceSize());

  // Setup the primitive array type classes - can't be done until Object has a vtable.
  SetClassRoot(kBooleanArrayClass, FindSystemClass(self, "[Z"));
  mirror::BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));

  SetClassRoot(kByteArrayClass, FindSystemClass(self, "[B"));
  mirror::ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));

  CHECK_EQ(char_array_class.Get(), FindSystemClass(self, "[C"));

  SetClassRoot(kShortArrayClass, FindSystemClass(self, "[S"));
  mirror::ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));

  CHECK_EQ(int_array_class.Get(), FindSystemClass(self, "[I"));

  CHECK_EQ(long_array_class.Get(), FindSystemClass(self, "[J"));

  SetClassRoot(kFloatArrayClass, FindSystemClass(self, "[F"));
  mirror::FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));

  SetClassRoot(kDoubleArrayClass, FindSystemClass(self, "[D"));
  mirror::DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));

  CHECK_EQ(class_array_class.Get(), FindSystemClass(self, "[Ljava/lang/Class;"));

  CHECK_EQ(object_array_class.Get(), FindSystemClass(self, "[Ljava/lang/Object;"));

  // Setup the single, global copy of "iftable".
  auto java_lang_Cloneable = hs.NewHandle(FindSystemClass(self, "Ljava/lang/Cloneable;"));
  CHECK(java_lang_Cloneable.Get() != nullptr);
  auto java_io_Serializable = hs.NewHandle(FindSystemClass(self, "Ljava/io/Serializable;"));
  CHECK(java_io_Serializable.Get() != nullptr);
  // We assume that Cloneable/Serializable don't have superinterfaces -- normally we'd have to
  // crawl up and explicitly list all of the supers as well.
  array_iftable_.Read()->SetInterface(0, java_lang_Cloneable.Get());
  array_iftable_.Read()->SetInterface(1, java_io_Serializable.Get());

  // Sanity check Class[] and Object[]'s interfaces. GetDirectInterface may cause thread
  // suspension.
  CHECK_EQ(java_lang_Cloneable.Get(),
           mirror::Class::GetDirectInterface(self, class_array_class, 0));
  CHECK_EQ(java_io_Serializable.Get(),
           mirror::Class::GetDirectInterface(self, class_array_class, 1));
  CHECK_EQ(java_lang_Cloneable.Get(),
           mirror::Class::GetDirectInterface(self, object_array_class, 0));
  CHECK_EQ(java_io_Serializable.Get(),
           mirror::Class::GetDirectInterface(self, object_array_class, 1));
  // Run Class, ArtField, and ArtMethod through FindSystemClass. This initializes their
  // dex_cache_ fields and register them in class_table_.
  CHECK_EQ(java_lang_Class.Get(), FindSystemClass(self, "Ljava/lang/Class;"));

  mirror::Class::SetStatus(java_lang_reflect_ArtMethod, mirror::Class::kStatusNotReady, self);
  CHECK_EQ(java_lang_reflect_ArtMethod.Get(),
           FindSystemClass(self, "Ljava/lang/reflect/ArtMethod;"));
  CHECK_EQ(object_array_string.Get(),
           FindSystemClass(self, GetClassRootDescriptor(kJavaLangStringArrayClass)));
  CHECK_EQ(object_array_art_method.Get(),
           FindSystemClass(self, GetClassRootDescriptor(kJavaLangReflectArtMethodArrayClass)));

  // End of special init trickery, subsequent classes may be loaded via FindSystemClass.

  // Create java.lang.reflect.Proxy root.
  SetClassRoot(kJavaLangReflectProxy, FindSystemClass(self, "Ljava/lang/reflect/Proxy;"));

  // Create java.lang.reflect.Field.class root.
  auto* class_root = FindSystemClass(self, "Ljava/lang/reflect/Field;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectField, class_root);
  mirror::Field::SetClass(class_root);

  // Create java.lang.reflect.Field array root.
  class_root = FindSystemClass(self, "[Ljava/lang/reflect/Field;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectFieldArrayClass, class_root);
  mirror::Field::SetArrayClass(class_root);

  // Create java.lang.reflect.Constructor.class root and array root.
  class_root = FindSystemClass(self, "Ljava/lang/reflect/Constructor;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectConstructor, class_root);
  mirror::Constructor::SetClass(class_root);
  class_root = FindSystemClass(self, "[Ljava/lang/reflect/Constructor;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectConstructorArrayClass, class_root);
  mirror::Constructor::SetArrayClass(class_root);

  // Create java.lang.reflect.Method.class root and array root.
  class_root = FindSystemClass(self, "Ljava/lang/reflect/Method;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectMethod, class_root);
  mirror::Method::SetClass(class_root);
  class_root = FindSystemClass(self, "[Ljava/lang/reflect/Method;");
  CHECK(class_root != nullptr);
  SetClassRoot(kJavaLangReflectMethodArrayClass, class_root);
  mirror::Method::SetArrayClass(class_root);

  // java.lang.ref classes need to be specially flagged, but otherwise are normal classes
  // finish initializing Reference class
  mirror::Class::SetStatus(java_lang_ref_Reference, mirror::Class::kStatusNotReady, self);
  CHECK_EQ(java_lang_ref_Reference.Get(), FindSystemClass(self, "Ljava/lang/ref/Reference;"));
  CHECK_EQ(java_lang_ref_Reference->GetObjectSize(), mirror::Reference::InstanceSize());
  CHECK_EQ(java_lang_ref_Reference->GetClassSize(), mirror::Reference::ClassSize());
  class_root = FindSystemClass(self, "Ljava/lang/ref/FinalizerReference;");
  class_root->SetAccessFlags(class_root->GetAccessFlags() |
                             kAccClassIsReference | kAccClassIsFinalizerReference);
  class_root = FindSystemClass(self, "Ljava/lang/ref/PhantomReference;");
  class_root->SetAccessFlags(class_root->GetAccessFlags() | kAccClassIsReference |
                             kAccClassIsPhantomReference);
  class_root = FindSystemClass(self, "Ljava/lang/ref/SoftReference;");
  class_root->SetAccessFlags(class_root->GetAccessFlags() | kAccClassIsReference);
  class_root = FindSystemClass(self, "Ljava/lang/ref/WeakReference;");
  class_root->SetAccessFlags(class_root->GetAccessFlags() | kAccClassIsReference |
                             kAccClassIsWeakReference);

  // Setup the ClassLoader, verifying the object_size_.
  class_root = FindSystemClass(self, "Ljava/lang/ClassLoader;");
  CHECK_EQ(class_root->GetObjectSize(), mirror::ClassLoader::InstanceSize());
  SetClassRoot(kJavaLangClassLoader, class_root);

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

  // Ensure void type is resolved in the core's dex cache so java.lang.Void is correctly
  // initialized.
  {
    const DexFile& dex_file = java_lang_Object->GetDexFile();
    const DexFile::StringId* void_string_id = dex_file.FindStringId("V");
    CHECK(void_string_id != nullptr);
    uint32_t void_string_index = dex_file.GetIndexForStringId(*void_string_id);
    const DexFile::TypeId* void_type_id = dex_file.FindTypeId(void_string_index);
    CHECK(void_type_id != nullptr);
    uint16_t void_type_idx = dex_file.GetIndexForTypeId(*void_type_id);
    // Now we resolve void type so the dex cache contains it. We use java.lang.Object class
    // as referrer so the used dex cache is core's one.
    mirror::Class* resolved_type = ResolveType(dex_file, void_type_idx, java_lang_Object.Get());
    CHECK_EQ(resolved_type, GetClassRoot(kPrimitiveVoid));
    self->AssertNoPendingException();
  }

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

  ArtField* pendingNext = java_lang_ref_Reference->GetInstanceField(0);
  CHECK_STREQ(pendingNext->GetName(), "pendingNext");
  CHECK_STREQ(pendingNext->GetTypeDescriptor(), "Ljava/lang/ref/Reference;");

  ArtField* queue = java_lang_ref_Reference->GetInstanceField(1);
  CHECK_STREQ(queue->GetName(), "queue");
  CHECK_STREQ(queue->GetTypeDescriptor(), "Ljava/lang/ref/ReferenceQueue;");

  ArtField* queueNext = java_lang_ref_Reference->GetInstanceField(2);
  CHECK_STREQ(queueNext->GetName(), "queueNext");
  CHECK_STREQ(queueNext->GetTypeDescriptor(), "Ljava/lang/ref/Reference;");

  ArtField* referent = java_lang_ref_Reference->GetInstanceField(3);
  CHECK_STREQ(referent->GetName(), "referent");
  CHECK_STREQ(referent->GetTypeDescriptor(), "Ljava/lang/Object;");

  ArtField* zombie = java_lang_ref_FinalizerReference->GetInstanceField(2);
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

class DexFileAndClassPair : ValueObject {
 public:
  DexFileAndClassPair(const DexFile* dex_file, size_t current_class_index, bool from_loaded_oat)
     : cached_descriptor_(GetClassDescriptor(dex_file, current_class_index)),
       dex_file_(dex_file),
       current_class_index_(current_class_index),
       from_loaded_oat_(from_loaded_oat) {}

  DexFileAndClassPair(const DexFileAndClassPair&) = default;

  DexFileAndClassPair& operator=(const DexFileAndClassPair& rhs) {
    cached_descriptor_ = rhs.cached_descriptor_;
    dex_file_ = rhs.dex_file_;
    current_class_index_ = rhs.current_class_index_;
    from_loaded_oat_ = rhs.from_loaded_oat_;
    return *this;
  }

  const char* GetCachedDescriptor() const {
    return cached_descriptor_;
  }

  bool operator<(const DexFileAndClassPair& rhs) const {
    const char* lhsDescriptor = cached_descriptor_;
    const char* rhsDescriptor = rhs.cached_descriptor_;
    int cmp = strcmp(lhsDescriptor, rhsDescriptor);
    if (cmp != 0) {
      // Note that the order must be reversed. We want to iterate over the classes in dex files.
      // They are sorted lexicographically. Thus, the priority-queue must be a min-queue.
      return cmp > 0;
    }
    return dex_file_ < rhs.dex_file_;
  }

  bool DexFileHasMoreClasses() const {
    return current_class_index_ + 1 < dex_file_->NumClassDefs();
  }

  DexFileAndClassPair GetNext() const {
    return DexFileAndClassPair(dex_file_, current_class_index_ + 1, from_loaded_oat_);
  }

  size_t GetCurrentClassIndex() const {
    return current_class_index_;
  }

  bool FromLoadedOat() const {
    return from_loaded_oat_;
  }

  const DexFile* GetDexFile() const {
    return dex_file_;
  }

  void DeleteDexFile() {
    delete dex_file_;
    dex_file_ = nullptr;
  }

 private:
  static const char* GetClassDescriptor(const DexFile* dex_file, size_t index) {
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(static_cast<uint16_t>(index));
    return dex_file->StringByTypeIdx(class_def.class_idx_);
  }

  const char* cached_descriptor_;
  const DexFile* dex_file_;
  size_t current_class_index_;
  bool from_loaded_oat_;  // We only need to compare mismatches between what we load now
                          // and what was loaded before. Any old duplicates must have been
                          // OK, and any new "internal" duplicates are as well (they must
                          // be from multidex, which resolves correctly).
};

static void AddDexFilesFromOat(const OatFile* oat_file, bool already_loaded,
                               std::priority_queue<DexFileAndClassPair>* heap) {
  const std::vector<const OatDexFile*>& oat_dex_files = oat_file->GetOatDexFiles();
  for (const OatDexFile* oat_dex_file : oat_dex_files) {
    std::string error;
    std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error);
    if (dex_file.get() == nullptr) {
      LOG(WARNING) << "Could not create dex file from oat file: " << error;
    } else {
      if (dex_file->NumClassDefs() > 0U) {
        heap->emplace(dex_file.release(), 0U, already_loaded);
      }
    }
  }
}

static void AddNext(DexFileAndClassPair* original,
                    std::priority_queue<DexFileAndClassPair>* heap) {
  if (original->DexFileHasMoreClasses()) {
    heap->push(original->GetNext());
  } else {
    // Need to delete the dex file.
    original->DeleteDexFile();
  }
}

static void FreeDexFilesInHeap(std::priority_queue<DexFileAndClassPair>* heap) {
  while (!heap->empty()) {
    delete heap->top().GetDexFile();
    heap->pop();
  }
}

const OatFile* ClassLinker::GetBootOatFile() {
  // To grab the boot oat, look at the dex files in the boot classpath. Any of those is fine, as
  // they were all compiled into the same oat file. So grab the first one, which is guaranteed to
  // exist if the boot class-path isn't empty.
  if (boot_class_path_.empty()) {
    return nullptr;
  }
  const DexFile* boot_dex_file = boot_class_path_[0];
  // Is it from an oat file?
  if (boot_dex_file->GetOatDexFile() != nullptr) {
    return boot_dex_file->GetOatDexFile()->GetOatFile();
  }
  return nullptr;
}

const OatFile* ClassLinker::GetPrimaryOatFile() {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  const OatFile* boot_oat_file = GetBootOatFile();
  if (boot_oat_file != nullptr) {
    for (const OatFile* oat_file : oat_files_) {
      if (oat_file != boot_oat_file) {
        return oat_file;
      }
    }
  }
  return nullptr;
}

// Check for class-def collisions in dex files.
//
// This works by maintaining a heap with one class from each dex file, sorted by the class
// descriptor. Then a dex-file/class pair is continually removed from the heap and compared
// against the following top element. If the descriptor is the same, it is now checked whether
// the two elements agree on whether their dex file was from an already-loaded oat-file or the
// new oat file. Any disagreement indicates a collision.
bool ClassLinker::HasCollisions(const OatFile* oat_file, std::string* error_msg) {
  // Dex files are registered late - once a class is actually being loaded. We have to compare
  // against the open oat files. Take the dex_lock_ that protects oat_files_ accesses.
  ReaderMutexLock mu(Thread::Current(), dex_lock_);

  std::priority_queue<DexFileAndClassPair> queue;

  // Add dex files from already loaded oat files, but skip boot.
  {
    const OatFile* boot_oat = GetBootOatFile();
    for (const OatFile* loaded_oat_file : oat_files_) {
      if (loaded_oat_file == boot_oat) {
        continue;
      }
      AddDexFilesFromOat(loaded_oat_file, true, &queue);
    }
  }

  if (queue.empty()) {
    // No other oat files, return early.
    return false;
  }

  // Add dex files from the oat file to check.
  AddDexFilesFromOat(oat_file, false, &queue);

  // Now drain the queue.
  while (!queue.empty()) {
    DexFileAndClassPair compare_pop = queue.top();
    queue.pop();

    // Compare against the following elements.
    while (!queue.empty()) {
      DexFileAndClassPair top = queue.top();

      if (strcmp(compare_pop.GetCachedDescriptor(), top.GetCachedDescriptor()) == 0) {
        // Same descriptor. Check whether it's crossing old-oat-files to new-oat-files.
        if (compare_pop.FromLoadedOat() != top.FromLoadedOat()) {
          *error_msg =
              StringPrintf("Found duplicated class when checking oat files: '%s' in %s and %s",
                           compare_pop.GetCachedDescriptor(),
                           compare_pop.GetDexFile()->GetLocation().c_str(),
                           top.GetDexFile()->GetLocation().c_str());
          FreeDexFilesInHeap(&queue);
          return true;
        }
        // Pop it.
        queue.pop();
        AddNext(&top, &queue);
      } else {
        // Something else. Done here.
        break;
      }
    }
    AddNext(&compare_pop, &queue);
  }

  return false;
}

std::vector<std::unique_ptr<const DexFile>> ClassLinker::OpenDexFilesFromOat(
    const char* dex_location, const char* oat_location,
    std::vector<std::string>* error_msgs) {
  CHECK(error_msgs != nullptr);

  // Verify we aren't holding the mutator lock, which could starve GC if we
  // have to generate or relocate an oat file.
  Locks::mutator_lock_->AssertNotHeld(Thread::Current());

  OatFileAssistant oat_file_assistant(dex_location, oat_location, kRuntimeISA,
     !Runtime::Current()->IsAotCompiler());

  // Lock the target oat location to avoid races generating and loading the
  // oat file.
  std::string error_msg;
  if (!oat_file_assistant.Lock(&error_msg)) {
    // Don't worry too much if this fails. If it does fail, it's unlikely we
    // can generate an oat file anyway.
    VLOG(class_linker) << "OatFileAssistant::Lock: " << error_msg;
  }

  // Check if we already have an up-to-date oat file open.
  const OatFile* source_oat_file = nullptr;
  {
    ReaderMutexLock mu(Thread::Current(), dex_lock_);
    for (const OatFile* oat_file : oat_files_) {
      CHECK(oat_file != nullptr);
      if (oat_file_assistant.GivenOatFileIsUpToDate(*oat_file)) {
        source_oat_file = oat_file;
        break;
      }
    }
  }

  // If we didn't have an up-to-date oat file open, try to load one from disk.
  if (source_oat_file == nullptr) {
    // Update the oat file on disk if we can. This may fail, but that's okay.
    // Best effort is all that matters here.
    if (!oat_file_assistant.MakeUpToDate(&error_msg)) {
      LOG(WARNING) << error_msg;
    }

    // Get the oat file on disk.
    std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
    if (oat_file.get() != nullptr) {
      // Take the file only if it has no collisions, or we must take it because of preopting.
      bool accept_oat_file = !HasCollisions(oat_file.get(), &error_msg);
      if (!accept_oat_file) {
        // Failed the collision check. Print warning.
        if (Runtime::Current()->IsDexFileFallbackEnabled()) {
          LOG(WARNING) << "Found duplicate classes, falling back to interpreter mode for "
                       << dex_location;
        } else {
          LOG(WARNING) << "Found duplicate classes, dex-file-fallback disabled, will be failing to "
                          " load classes for " << dex_location;
        }
        LOG(WARNING) << error_msg;

        // However, if the app was part of /system and preopted, there is no original dex file
        // available. In that case grudgingly accept the oat file.
        if (!DexFile::MaybeDex(dex_location)) {
          accept_oat_file = true;
          LOG(WARNING) << "Dex location " << dex_location << " does not seem to include dex file. "
                       << "Allow oat file use. This is potentially dangerous.";
        }
      }

      if (accept_oat_file) {
        source_oat_file = oat_file.release();
        RegisterOatFile(source_oat_file);
      }
    }
  }

  std::vector<std::unique_ptr<const DexFile>> dex_files;

  // Load the dex files from the oat file.
  if (source_oat_file != nullptr) {
    dex_files = oat_file_assistant.LoadDexFiles(*source_oat_file, dex_location);
    if (dex_files.empty()) {
      error_msgs->push_back("Failed to open dex files from "
          + source_oat_file->GetLocation());
    }
  }

  // Fall back to running out of the original dex file if we couldn't load any
  // dex_files from the oat file.
  if (dex_files.empty()) {
    if (Runtime::Current()->IsDexFileFallbackEnabled()) {
      if (!DexFile::Open(dex_location, dex_location, &error_msg, &dex_files)) {
        LOG(WARNING) << error_msg;
        error_msgs->push_back("Failed to open dex files from " + std::string(dex_location));
      }
    } else {
      error_msgs->push_back("Fallback mode disabled, skipping dex files.");
    }
  }
  return dex_files;
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

void ClassLinker::InitFromImageInterpretOnlyCallback(mirror::Object* obj, void* arg) {
  ClassLinker* class_linker = reinterpret_cast<ClassLinker*>(arg);
  DCHECK(obj != nullptr);
  DCHECK(class_linker != nullptr);
  if (obj->IsArtMethod()) {
    mirror::ArtMethod* method = obj->AsArtMethod();
    if (!method->IsNative()) {
      const size_t pointer_size = class_linker->image_pointer_size_;
      method->SetEntryPointFromInterpreterPtrSize(artInterpreterToInterpreterBridge, pointer_size);
      if (!method->IsRuntimeMethod() && method != Runtime::Current()->GetResolutionMethod()) {
        method->SetEntryPointFromQuickCompiledCodePtrSize(GetQuickToInterpreterBridge(),
                                                          pointer_size);
      }
    }
  }
}

void SanityCheckObjectsCallback(mirror::Object* obj, void* arg ATTRIBUTE_UNUSED)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(obj != nullptr);
  CHECK(obj->GetClass() != nullptr) << "Null class " << obj;
  CHECK(obj->GetClass()->GetClass() != nullptr) << "Null class class " << obj;
  if (obj->IsClass()) {
    auto klass = obj->AsClass();
    ArtField* fields[2] = { klass->GetSFields(), klass->GetIFields() };
    size_t num_fields[2] = { klass->NumStaticFields(), klass->NumInstanceFields() };
    for (size_t i = 0; i < 2; ++i) {
      for (size_t j = 0; j < num_fields[i]; ++j) {
        CHECK_EQ(fields[i][j].GetDeclaringClass(), klass);
      }
    }
  }
}

void ClassLinker::InitFromImage() {
  VLOG(startup) << "ClassLinker::InitFromImage entering";
  CHECK(!init_done_);

  Runtime* const runtime = Runtime::Current();
  Thread* const self = Thread::Current();
  gc::Heap* const heap = runtime->GetHeap();
  gc::space::ImageSpace* const space = heap->GetImageSpace();
  dex_cache_image_class_lookup_required_ = true;
  CHECK(space != nullptr);
  OatFile& oat_file = GetImageOatFile(space);
  CHECK_EQ(oat_file.GetOatHeader().GetImageFileLocationOatChecksum(), 0U);
  CHECK_EQ(oat_file.GetOatHeader().GetImageFileLocationOatDataBegin(), 0U);
  const char* image_file_location = oat_file.GetOatHeader().
      GetStoreValueByKey(OatHeader::kImageLocationKey);
  CHECK(image_file_location == nullptr || *image_file_location == 0);
  quick_resolution_trampoline_ = oat_file.GetOatHeader().GetQuickResolutionTrampoline();
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
    StackHandleScope<1> hs2(self);
    Handle<mirror::DexCache> dex_cache(hs2.NewHandle(dex_caches->Get(i)));
    const std::string& dex_file_location(dex_cache->GetLocation()->ToModifiedUtf8());
    const OatFile::OatDexFile* oat_dex_file = oat_file.GetOatDexFile(dex_file_location.c_str(),
                                                                     nullptr);
    CHECK(oat_dex_file != nullptr) << oat_file.GetLocation() << " " << dex_file_location;
    std::string error_msg;
    std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error_msg);
    if (dex_file.get() == nullptr) {
      LOG(FATAL) << "Failed to open dex file " << dex_file_location
                 << " from within oat file " << oat_file.GetLocation()
                 << " error '" << error_msg << "'";
      UNREACHABLE();
    }

    CHECK_EQ(dex_file->GetLocationChecksum(), oat_dex_file->GetDexFileLocationChecksum());

    AppendToBootClassPath(*dex_file.get(), dex_cache);
    opened_dex_files_.push_back(std::move(dex_file));
  }

  // Set classes on AbstractMethod early so that IsMethod tests can be performed during the live
  // bitmap walk.
  mirror::ArtMethod::SetClass(GetClassRoot(kJavaLangReflectArtMethod));
  size_t art_method_object_size = mirror::ArtMethod::GetJavaLangReflectArtMethod()->GetObjectSize();
  if (!runtime->IsAotCompiler()) {
    // Aot compiler supports having an image with a different pointer size than the runtime. This
    // happens on the host for compile 32 bit tests since we use a 64 bit libart compiler. We may
    // also use 32 bit dex2oat on a system with 64 bit apps.
    CHECK_EQ(art_method_object_size, mirror::ArtMethod::InstanceSize(sizeof(void*)))
        << sizeof(void*);
  }
  if (art_method_object_size == mirror::ArtMethod::InstanceSize(4)) {
    image_pointer_size_ = 4;
  } else {
    CHECK_EQ(art_method_object_size, mirror::ArtMethod::InstanceSize(8));
    image_pointer_size_ = 8;
  }

  // Set entry point to interpreter if in InterpretOnly mode.
  if (!runtime->IsAotCompiler() && runtime->GetInstrumentation()->InterpretOnly()) {
    heap->VisitObjects(InitFromImageInterpretOnlyCallback, this);
  }
  if (kSanityCheckObjects) {
    for (int32_t i = 0; i < dex_caches->GetLength(); i++) {
      auto* dex_cache = dex_caches->Get(i);
      for (size_t j = 0; j < dex_cache->NumResolvedFields(); ++j) {
        auto* field = dex_cache->GetResolvedField(j, image_pointer_size_);
        if (field != nullptr) {
          CHECK(field->GetDeclaringClass()->GetClass() != nullptr);
        }
      }
    }
    heap->VisitObjects(SanityCheckObjectsCallback, nullptr);
  }

  // reinit class_roots_
  mirror::Class::SetClassClass(class_roots->Get(kJavaLangClass));
  class_roots_ = GcRoot<mirror::ObjectArray<mirror::Class>>(class_roots.Get());

  // reinit array_iftable_ from any array class instance, they should be ==
  array_iftable_ = GcRoot<mirror::IfTable>(GetClassRoot(kObjectArrayClass)->GetIfTable());
  DCHECK_EQ(array_iftable_.Read(), GetClassRoot(kBooleanArrayClass)->GetIfTable());
  // String class root was set above
  mirror::Field::SetClass(GetClassRoot(kJavaLangReflectField));
  mirror::Field::SetArrayClass(GetClassRoot(kJavaLangReflectFieldArrayClass));
  mirror::Constructor::SetClass(GetClassRoot(kJavaLangReflectConstructor));
  mirror::Constructor::SetArrayClass(GetClassRoot(kJavaLangReflectConstructorArrayClass));
  mirror::Method::SetClass(GetClassRoot(kJavaLangReflectMethod));
  mirror::Method::SetArrayClass(GetClassRoot(kJavaLangReflectMethodArrayClass));
  mirror::Reference::SetClass(GetClassRoot(kJavaLangRefReference));
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

void ClassLinker::VisitClassRoots(RootVisitor* visitor, VisitRootFlags flags) {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(
      visitor, RootInfo(kRootStickyClass));
  if ((flags & kVisitRootFlagAllRoots) != 0) {
    for (GcRoot<mirror::Class>& root : class_table_) {
      buffered_visitor.VisitRoot(root);
      root.Read()->VisitFieldRoots(buffered_visitor);
    }
    // PreZygote classes can't move so we won't need to update fields' declaring classes.
    for (GcRoot<mirror::Class>& root : pre_zygote_class_table_) {
      buffered_visitor.VisitRoot(root);
      root.Read()->VisitFieldRoots(buffered_visitor);
    }
  } else if ((flags & kVisitRootFlagNewRoots) != 0) {
    for (auto& root : new_class_roots_) {
      mirror::Class* old_ref = root.Read<kWithoutReadBarrier>();
      old_ref->VisitFieldRoots(buffered_visitor);
      root.VisitRoot(visitor, RootInfo(kRootStickyClass));
      mirror::Class* new_ref = root.Read<kWithoutReadBarrier>();
      if (UNLIKELY(new_ref != old_ref)) {
        // Uh ohes, GC moved a root in the log. Need to search the class_table and update the
        // corresponding object. This is slow, but luckily for us, this may only happen with a
        // concurrent moving GC.
        auto it = class_table_.Find(GcRoot<mirror::Class>(old_ref));
        DCHECK(it != class_table_.end());
        *it = GcRoot<mirror::Class>(new_ref);
      }
    }
  }
  buffered_visitor.Flush();  // Flush before clearing new_class_roots_.
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
void ClassLinker::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) {
  class_roots_.VisitRoot(visitor, RootInfo(kRootVMInternal));
  Thread* const self = Thread::Current();
  {
    ReaderMutexLock mu(self, dex_lock_);
    if ((flags & kVisitRootFlagAllRoots) != 0) {
      for (GcRoot<mirror::DexCache>& dex_cache : dex_caches_) {
        dex_cache.VisitRoot(visitor, RootInfo(kRootVMInternal));
      }
    } else if ((flags & kVisitRootFlagNewRoots) != 0) {
      for (size_t index : new_dex_cache_roots_) {
        dex_caches_[index].VisitRoot(visitor, RootInfo(kRootVMInternal));
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
  VisitClassRoots(visitor, flags);
  array_iftable_.VisitRoot(visitor, RootInfo(kRootVMInternal));
  for (size_t i = 0; i < kFindArrayCacheSize; ++i) {
    find_array_class_cache_[i].VisitRootIfNonNull(visitor, RootInfo(kRootVMInternal));
  }
}

void ClassLinker::VisitClasses(ClassVisitor* visitor, void* arg) {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  // TODO: why isn't this a ReaderMutexLock?
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  for (GcRoot<mirror::Class>& root : class_table_) {
    if (!visitor(root.Read(), arg)) {
      return;
    }
  }
  for (GcRoot<mirror::Class>& root : pre_zygote_class_table_) {
    if (!visitor(root.Read(), arg)) {
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
        class_table_size = class_table_.Size() + pre_zygote_class_table_.Size();
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
  mirror::ArtMethod::ResetClass();
  mirror::Class::ResetClass();
  mirror::Constructor::ResetClass();
  mirror::Field::ResetClass();
  mirror::Method::ResetClass();
  mirror::Reference::ResetClass();
  mirror::StackTraceElement::ResetClass();
  mirror::String::ResetClass();
  mirror::Throwable::ResetClass();
  mirror::BooleanArray::ResetArrayClass();
  mirror::ByteArray::ResetArrayClass();
  mirror::CharArray::ResetArrayClass();
  mirror::Constructor::ResetArrayClass();
  mirror::DoubleArray::ResetArrayClass();
  mirror::Field::ResetArrayClass();
  mirror::FloatArray::ResetArrayClass();
  mirror::Method::ResetArrayClass();
  mirror::IntArray::ResetArrayClass();
  mirror::LongArray::ResetArrayClass();
  mirror::ShortArray::ResetArrayClass();
  STLDeleteElements(&oat_files_);
}

mirror::DexCache* ClassLinker::AllocDexCache(Thread* self, const DexFile& dex_file) {
  gc::Heap* const heap = Runtime::Current()->GetHeap();
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
  Handle<mirror::Array> fields;
  if (image_pointer_size_ == 8) {
    fields = hs.NewHandle<mirror::Array>(mirror::LongArray::Alloc(self, dex_file.NumFieldIds()));
  } else {
    fields = hs.NewHandle<mirror::Array>(mirror::IntArray::Alloc(self, dex_file.NumFieldIds()));
  }
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
    klass = LookupClass(self, descriptor, ComputeModifiedUtf8Hash(descriptor),
                        h_class.Get()->GetClassLoader());
  }

  // Wait for the class if it has not already been linked.
  if (!klass->IsResolved() && !klass->IsErroneous()) {
    StackHandleScope<1> hs(self);
    HandleWrapper<mirror::Class> h_class(hs.NewHandleWrapper(&klass));
    ObjectLock<mirror::Class> lock(self, h_class);
    // Check for circular dependencies between classes.
    if (!h_class->IsResolved() && h_class->GetClinitThreadId() == self->GetTid()) {
      ThrowClassCircularityError(h_class.Get());
      mirror::Class::SetStatus(h_class, mirror::Class::kStatusError, self);
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
                               size_t hash, const std::vector<const DexFile*>& class_path) {
  for (const DexFile* dex_file : class_path) {
    const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor, hash);
    if (dex_class_def != nullptr) {
      return ClassPathEntry(dex_file, dex_class_def);
    }
  }
  return ClassPathEntry(nullptr, nullptr);
}

static bool IsBootClassLoader(ScopedObjectAccessAlreadyRunnable& soa,
                              mirror::ClassLoader* class_loader)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return class_loader == nullptr ||
      class_loader->GetClass() ==
          soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_BootClassLoader);
}

bool ClassLinker::FindClassInPathClassLoader(ScopedObjectAccessAlreadyRunnable& soa,
                                             Thread* self, const char* descriptor,
                                             size_t hash,
                                             Handle<mirror::ClassLoader> class_loader,
                                             mirror::Class** result) {
  // Termination case: boot class-loader.
  if (IsBootClassLoader(soa, class_loader.Get())) {
    // The boot class loader, search the boot class path.
    ClassPathEntry pair = FindInClassPath(descriptor, hash, boot_class_path_);
    if (pair.second != nullptr) {
      mirror::Class* klass = LookupClass(self, descriptor, hash, nullptr);
      if (klass != nullptr) {
        *result = EnsureResolved(self, descriptor, klass);
      } else {
        *result = DefineClass(self, descriptor, hash, NullHandle<mirror::ClassLoader>(),
                              *pair.first, *pair.second);
      }
      if (*result == nullptr) {
        CHECK(self->IsExceptionPending()) << descriptor;
        self->ClearException();
      }
    } else {
      *result = nullptr;
    }
    return true;
  }

  // Unsupported class-loader?
  if (class_loader->GetClass() !=
      soa.Decode<mirror::Class*>(WellKnownClasses::dalvik_system_PathClassLoader)) {
    *result = nullptr;
    return false;
  }

  // Handles as RegisterDexFile may allocate dex caches (and cause thread suspension).
  StackHandleScope<4> hs(self);
  Handle<mirror::ClassLoader> h_parent(hs.NewHandle(class_loader->GetParent()));
  bool recursive_result = FindClassInPathClassLoader(soa, self, descriptor, hash, h_parent, result);

  if (!recursive_result) {
    // Something wrong up the chain.
    return false;
  }

  if (*result != nullptr) {
    // Found the class up the chain.
    return true;
  }

  // Handle this step.
  // Handle as if this is the child PathClassLoader.
  // The class loader is a PathClassLoader which inherits from BaseDexClassLoader.
  // We need to get the DexPathList and loop through it.
  ArtField* const cookie_field = soa.DecodeField(WellKnownClasses::dalvik_system_DexFile_cookie);
  ArtField* const dex_file_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList__Element_dexFile);
  mirror::Object* dex_path_list =
      soa.DecodeField(WellKnownClasses::dalvik_system_PathClassLoader_pathList)->
      GetObject(class_loader.Get());
  if (dex_path_list != nullptr && dex_file_field != nullptr && cookie_field != nullptr) {
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
          mirror::LongArray* long_array = cookie_field->GetObject(dex_file)->AsLongArray();
          if (long_array == nullptr) {
            // This should never happen so log a warning.
            LOG(WARNING) << "Null DexFile::mCookie for " << descriptor;
            break;
          }
          int32_t long_array_size = long_array->GetLength();
          for (int32_t j = 0; j < long_array_size; ++j) {
            const DexFile* cp_dex_file = reinterpret_cast<const DexFile*>(static_cast<uintptr_t>(
                long_array->GetWithoutChecks(j)));
            const DexFile::ClassDef* dex_class_def = cp_dex_file->FindClassDef(descriptor, hash);
            if (dex_class_def != nullptr) {
              RegisterDexFile(*cp_dex_file);
              mirror::Class* klass = DefineClass(self, descriptor, hash, class_loader,
                                                 *cp_dex_file, *dex_class_def);
              if (klass == nullptr) {
                CHECK(self->IsExceptionPending()) << descriptor;
                self->ClearException();
                // TODO: Is it really right to break here, and not check the other dex files?
                return true;
              }
              *result = klass;
              return true;
            }
          }
        }
      }
    }
    self->AssertNoPendingException();
  }

  // Result is still null from the parent call, no need to set it again...
  return true;
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
  const size_t hash = ComputeModifiedUtf8Hash(descriptor);
  // Find the class in the loaded classes table.
  mirror::Class* klass = LookupClass(self, descriptor, hash, class_loader.Get());
  if (klass != nullptr) {
    return EnsureResolved(self, descriptor, klass);
  }
  // Class is not yet loaded.
  if (descriptor[0] == '[') {
    return CreateArrayClass(self, descriptor, hash, class_loader);
  } else if (class_loader.Get() == nullptr) {
    // The boot class loader, search the boot class path.
    ClassPathEntry pair = FindInClassPath(descriptor, hash, boot_class_path_);
    if (pair.second != nullptr) {
      return DefineClass(self, descriptor, hash, NullHandle<mirror::ClassLoader>(), *pair.first,
                         *pair.second);
    } else {
      // The boot class loader is searched ahead of the application class loader, failures are
      // expected and will be wrapped in a ClassNotFoundException. Use the pre-allocated error to
      // trigger the chaining with a proper stack trace.
      mirror::Throwable* pre_allocated = Runtime::Current()->GetPreAllocatedNoClassDefFoundError();
      self->SetException(pre_allocated);
      return nullptr;
    }
  } else {
    ScopedObjectAccessUnchecked soa(self);
    mirror::Class* cp_klass;
    if (FindClassInPathClassLoader(soa, self, descriptor, hash, class_loader, &cp_klass)) {
      // The chain was understood. So the value in cp_klass is either the class we were looking
      // for, or not found.
      if (cp_klass != nullptr) {
        return cp_klass;
      }
      // TODO: We handle the boot classpath loader in FindClassInPathClassLoader. Try to unify this
      //       and the branch above. TODO: throw the right exception here.

      // We'll let the Java-side rediscover all this and throw the exception with the right stack
      // trace.
    }

    if (Runtime::Current()->IsAotCompiler()) {
      // Oops, compile-time, can't run actual class-loader code.
      mirror::Throwable* pre_allocated = Runtime::Current()->GetPreAllocatedNoClassDefFoundError();
      self->SetException(pre_allocated);
      return nullptr;
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
      ThrowNullPointerException(StringPrintf("ClassLoader.loadClass returned null for %s",
                                             class_name_string.c_str()).c_str());
      return nullptr;
    } else {
      // success, return mirror::Class*
      return soa.Decode<mirror::Class*>(result.get());
    }
  }
  UNREACHABLE();
}

mirror::Class* ClassLinker::DefineClass(Thread* self, const char* descriptor, size_t hash,
                                        Handle<mirror::ClassLoader> class_loader,
                                        const DexFile& dex_file,
                                        const DexFile::ClassDef& dex_class_def) {
  StackHandleScope<3> hs(self);
  auto klass = hs.NewHandle<mirror::Class>(nullptr);

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
    } else if (strcmp(descriptor, "Ljava/lang/reflect/ArtMethod;") == 0) {
      klass.Assign(GetClassRoot(kJavaLangReflectArtMethod));
    }
  }

  if (klass.Get() == nullptr) {
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

  SetupClass(dex_file, dex_class_def, klass, class_loader.Get());

  // Mark the string class by setting its access flag.
  if (UNLIKELY(!init_done_)) {
    if (strcmp(descriptor, "Ljava/lang/String;") == 0) {
      klass->SetStringClass();
    }
  }

  ObjectLock<mirror::Class> lock(self, klass);
  klass->SetClinitThreadId(self->GetTid());

  // Add the newly loaded class to the loaded classes table.
  mirror::Class* existing = InsertClass(descriptor, klass.Get(), hash);
  if (existing != nullptr) {
    // We failed to insert because we raced with another thread. Calling EnsureResolved may cause
    // this thread to block.
    return EnsureResolved(self, descriptor, existing);
  }

  // Load the fields and other things after we are inserted in the table. This is so that we don't
  // end up allocating unfree-able linear alloc resources and then lose the race condition. The
  // other reason is that the field roots are only visited from the class table. So we need to be
  // inserted before we allocate / fill in these fields.
  LoadClass(self, dex_file, dex_class_def, klass);
  if (self->IsExceptionPending()) {
    // An exception occured during load, set status to erroneous while holding klass' lock in case
    // notification is necessary.
    if (!klass->IsErroneous()) {
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
    }
    return nullptr;
  }

  // Finish loading (if necessary) by finding parents
  CHECK(!klass->IsLoaded());
  if (!LoadSuperAndInterfaces(klass, dex_file)) {
    // Loading failed.
    if (!klass->IsErroneous()) {
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
    }
    return nullptr;
  }
  CHECK(klass->IsLoaded());
  // Link the class (if necessary)
  CHECK(!klass->IsResolved());
  // TODO: Use fast jobjects?
  auto interfaces = hs.NewHandle<mirror::ObjectArray<mirror::Class>>(nullptr);

  MutableHandle<mirror::Class> h_new_class = hs.NewHandle<mirror::Class>(nullptr);
  if (!LinkClass(self, descriptor, klass, interfaces, &h_new_class)) {
    // Linking failed.
    if (!klass->IsErroneous()) {
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
    }
    return nullptr;
  }
  self->AssertNoPendingException();
  CHECK(h_new_class.Get() != nullptr) << descriptor;
  CHECK(h_new_class->IsResolved()) << descriptor;

  // Instrumentation may have updated entrypoints for all methods of all
  // classes. However it could not update methods of this class while we
  // were loading it. Now the class is resolved, we can update entrypoints
  // as required by instrumentation.
  if (Runtime::Current()->GetInstrumentation()->AreExitStubsInstalled()) {
    // We must be in the kRunnable state to prevent instrumentation from
    // suspending all threads to update entrypoints while we are doing it
    // for this class.
    DCHECK_EQ(self->GetState(), kRunnable);
    Runtime::Current()->GetInstrumentation()->InstallStubsForClass(h_new_class.Get());
  }

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
  Dbg::PostClassPrepare(h_new_class.Get());

  return h_new_class.Get();
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
          UNREACHABLE();
      }
    }
  }
  return mirror::Class::ComputeClassSize(false, 0, num_8, num_16, num_32, num_64, num_ref);
}

OatFile::OatClass ClassLinker::FindOatClass(const DexFile& dex_file, uint16_t class_def_idx,
                                            bool* found) {
  DCHECK_NE(class_def_idx, DexFile::kDexNoIndex16);
  const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
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
  UNREACHABLE();
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
    bool found_virtual = false;
    for (size_t i = 0; i < end; i++) {
      // Check method index instead of identity in case of duplicate method definitions.
      if (method->GetDexMethodIndex() ==
          declaring_class->GetVirtualMethod(i)->GetDexMethodIndex()) {
        found_virtual = true;
        break;
      }
      oat_method_index++;
    }
    CHECK(found_virtual) << "Didn't find oat method index for virtual method: "
                         << PrettyMethod(method);
  }
  DCHECK_EQ(oat_method_index,
            GetOatMethodIndexFromMethodIndex(*declaring_class->GetDexCache()->GetDexFile(),
                                             method->GetDeclaringClass()->GetDexClassDefIndex(),
                                             method->GetDexMethodIndex()));
  OatFile::OatClass oat_class = FindOatClass(*declaring_class->GetDexCache()->GetDexFile(),
                                             declaring_class->GetDexClassDefIndex(),
                                             found);
  if (!(*found)) {
    return OatFile::OatMethod::Invalid();
  }
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
  if (found) {
    auto* code = oat_method.GetQuickCode();
    if (code != nullptr) {
      return code;
    }
  }
  jit::Jit* const jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    auto* code = jit->GetCodeCache()->GetCodeFor(method);
    if (code != nullptr) {
      return code;
    }
  }
  if (method->IsNative()) {
    // No code and native? Use generic trampoline.
    return GetQuickGenericJniStub();
  }
  return GetQuickToInterpreterBridge();
}

const void* ClassLinker::GetOatMethodQuickCodeFor(mirror::ArtMethod* method) {
  if (method->IsNative() || method->IsAbstract() || method->IsProxyMethod()) {
    return nullptr;
  }
  bool found;
  OatFile::OatMethod oat_method = FindOatMethodFor(method, &found);
  if (found) {
    return oat_method.GetQuickCode();
  }
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    auto* code = jit->GetCodeCache()->GetCodeFor(method);
    if (code != nullptr) {
      return code;
    }
  }
  return nullptr;
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

// Returns true if the method must run with interpreter, false otherwise.
static bool NeedsInterpreter(mirror::ArtMethod* method, const void* quick_code)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (quick_code == nullptr) {
    // No code: need interpreter.
    // May return true for native code, in the case of generic JNI
    // DCHECK(!method->IsNative());
    return true;
  }
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
  if (!runtime->IsStarted()) {
    if (runtime->IsAotCompiler() || runtime->GetHeap()->HasImageSpace()) {
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
    const void* quick_code = nullptr;
    if (has_oat_class) {
      OatFile::OatMethod oat_method = oat_class.GetOatMethod(method_index);
      quick_code = oat_method.GetQuickCode();
    }
    const bool enter_interpreter = NeedsInterpreter(method, quick_code);
    if (enter_interpreter) {
      // Use interpreter entry point.
      // Check whether the method is native, in which case it's generic JNI.
      if (quick_code == nullptr && method->IsNative()) {
        quick_code = GetQuickGenericJniStub();
      } else {
        quick_code = GetQuickToInterpreterBridge();
      }
    }
    runtime->GetInstrumentation()->UpdateMethodsCode(method, quick_code);
  }
  // Ignore virtual methods on the iterator.
}

void ClassLinker::LinkCode(Handle<mirror::ArtMethod> method,
                           const OatFile::OatClass* oat_class,
                           uint32_t class_def_method_index) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsAotCompiler()) {
    // The following code only applies to a non-compiler runtime.
    return;
  }
  // Method shouldn't have already been linked.
  DCHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr);
  if (oat_class != nullptr) {
    // Every kind of method should at least get an invoke stub from the oat_method.
    // non-abstract methods also get their code pointers.
    const OatFile::OatMethod oat_method = oat_class->GetOatMethod(class_def_method_index);
    oat_method.LinkMethod(method.Get());
  }

  // Install entry point from interpreter.
  bool enter_interpreter = NeedsInterpreter(method.Get(),
                                            method->GetEntryPointFromQuickCompiledCode());
  if (enter_interpreter && !method->IsNative()) {
    method->SetEntryPointFromInterpreter(artInterpreterToInterpreterBridge);
  } else {
    method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
  }

  if (method->IsAbstract()) {
    method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
    return;
  }

  if (method->IsStatic() && !method->IsConstructor()) {
    // For static methods excluding the class initializer, install the trampoline.
    // It will be replaced by the proper entry point by ClassLinker::FixupStaticTrampolines
    // after initializing class (see ClassLinker::InitializeClass method).
    method->SetEntryPointFromQuickCompiledCode(GetQuickResolutionStub());
  } else if (enter_interpreter) {
    if (!method->IsNative()) {
      // Set entry point from compiled code if there's no code or in interpreter only mode.
      method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
    } else {
      method->SetEntryPointFromQuickCompiledCode(GetQuickGenericJniStub());
    }
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
}

void ClassLinker::SetupClass(const DexFile& dex_file, const DexFile::ClassDef& dex_class_def,
                             Handle<mirror::Class> klass, mirror::ClassLoader* class_loader) {
  CHECK(klass.Get() != nullptr);
  CHECK(klass->GetDexCache() != nullptr);
  CHECK_EQ(mirror::Class::kStatusNotReady, klass->GetStatus());
  const char* descriptor = dex_file.GetClassDescriptor(dex_class_def);
  CHECK(descriptor != nullptr);

  klass->SetClass(GetClassRoot(kJavaLangClass));
  uint32_t access_flags = dex_class_def.GetJavaAccessFlags();
  CHECK_EQ(access_flags & ~kAccJavaFlagsMask, 0U);
  klass->SetAccessFlags(access_flags);
  klass->SetClassLoader(class_loader);
  DCHECK_EQ(klass->GetPrimitiveType(), Primitive::kPrimNot);
  mirror::Class::SetStatus(klass, mirror::Class::kStatusIdx, nullptr);

  klass->SetDexClassDefIndex(dex_file.GetIndexForClassDef(dex_class_def));
  klass->SetDexTypeIndex(dex_class_def.class_idx_);
  CHECK(klass->GetDexCacheStrings() != nullptr);
}

void ClassLinker::LoadClass(Thread* self, const DexFile& dex_file,
                            const DexFile::ClassDef& dex_class_def,
                            Handle<mirror::Class> klass) {
  const uint8_t* class_data = dex_file.GetClassData(dex_class_def);
  if (class_data == nullptr) {
    return;  // no fields or methods - for example a marker interface
  }
  bool has_oat_class = false;
  if (Runtime::Current()->IsStarted() && !Runtime::Current()->IsAotCompiler()) {
    OatFile::OatClass oat_class = FindOatClass(dex_file, klass->GetDexClassDefIndex(),
                                               &has_oat_class);
    if (has_oat_class) {
      LoadClassMembers(self, dex_file, class_data, klass, &oat_class);
    }
  }
  if (!has_oat_class) {
    LoadClassMembers(self, dex_file, class_data, klass, nullptr);
  }
}

ArtField* ClassLinker::AllocArtFieldArray(Thread* self, size_t length) {
  auto* const la = Runtime::Current()->GetLinearAlloc();
  auto* ptr = reinterpret_cast<ArtField*>(la->AllocArray<ArtField>(self, length));
  CHECK(ptr!= nullptr);
  std::uninitialized_fill_n(ptr, length, ArtField());
  return ptr;
}

void ClassLinker::LoadClassMembers(Thread* self, const DexFile& dex_file,
                                   const uint8_t* class_data,
                                   Handle<mirror::Class> klass,
                                   const OatFile::OatClass* oat_class) {
  // Load static fields.
  ClassDataItemIterator it(dex_file, class_data);
  const size_t num_sfields = it.NumStaticFields();
  ArtField* sfields = num_sfields != 0 ? AllocArtFieldArray(self, num_sfields) : nullptr;
  for (size_t i = 0; it.HasNextStaticField(); i++, it.Next()) {
    CHECK_LT(i, num_sfields);
    LoadField(it, klass, &sfields[i]);
  }
  klass->SetSFields(sfields);
  klass->SetNumStaticFields(num_sfields);
  DCHECK_EQ(klass->NumStaticFields(), num_sfields);
  // Load instance fields.
  const size_t num_ifields = it.NumInstanceFields();
  ArtField* ifields = num_ifields != 0 ? AllocArtFieldArray(self, num_ifields) : nullptr;
  for (size_t i = 0; it.HasNextInstanceField(); i++, it.Next()) {
    CHECK_LT(i, num_ifields);
    LoadField(it, klass, &ifields[i]);
  }
  klass->SetIFields(ifields);
  klass->SetNumInstanceFields(num_ifields);
  DCHECK_EQ(klass->NumInstanceFields(), num_ifields);
  // Note: We cannot have thread suspension until the field arrays are setup or else
  // Class::VisitFieldRoots may miss some fields.
  self->AllowThreadSuspension();
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
    LinkCode(method, oat_class, class_def_method_index);
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
    LinkCode(method, oat_class, class_def_method_index);
    class_def_method_index++;
  }
  DCHECK(!it.HasNext());
}

void ClassLinker::LoadField(const ClassDataItemIterator& it, Handle<mirror::Class> klass,
                            ArtField* dst) {
  const uint32_t field_idx = it.GetMemberIndex();
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
  UNREACHABLE();
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
  h_class->SetAccessFlags(kAccPublic | kAccFinal | kAccAbstract);
  h_class->SetPrimitiveType(type);
  mirror::Class::SetStatus(h_class, mirror::Class::kStatusInitialized, self);
  const char* descriptor = Primitive::Descriptor(type);
  mirror::Class* existing = InsertClass(descriptor, h_class.Get(),
                                        ComputeModifiedUtf8Hash(descriptor));
  CHECK(existing == nullptr) << "InitPrimitiveClass(" << type << ") failed";
  return h_class.Get();
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
// Returns null with an exception raised on failure.
mirror::Class* ClassLinker::CreateArrayClass(Thread* self, const char* descriptor, size_t hash,
                                             Handle<mirror::ClassLoader> class_loader) {
  // Identify the underlying component type
  CHECK_EQ('[', descriptor[0]);
  StackHandleScope<2> hs(self);
  MutableHandle<mirror::Class> component_type(hs.NewHandle(FindClass(self, descriptor + 1,
                                                                     class_loader)));
  if (component_type.Get() == nullptr) {
    DCHECK(self->IsExceptionPending());
    // We need to accept erroneous classes as component types.
    const size_t component_hash = ComputeModifiedUtf8Hash(descriptor + 1);
    component_type.Assign(LookupClass(self, descriptor + 1, component_hash, class_loader.Get()));
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
    mirror::Class* new_class = LookupClass(self, descriptor, hash, component_type->GetClassLoader());
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
    } else if (strcmp(descriptor, "[C") == 0) {
      new_class.Assign(GetClassRoot(kCharArrayClass));
    } else if (strcmp(descriptor, "[I") == 0) {
      new_class.Assign(GetClassRoot(kIntArrayClass));
    } else if (strcmp(descriptor, "[J") == 0) {
      new_class.Assign(GetClassRoot(kLongArrayClass));
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
  mirror::Class::SetStatus(new_class, mirror::Class::kStatusLoaded, self);
  {
    StackHandleScope<mirror::Class::kImtSize> hs2(self,
                                                  Runtime::Current()->GetImtUnimplementedMethod());
    new_class->PopulateEmbeddedImtAndVTable(&hs2);
  }
  mirror::Class::SetStatus(new_class, mirror::Class::kStatusInitialized, self);
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

  mirror::Class* existing = InsertClass(descriptor, new_class.Get(), hash);
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
  mirror::Class* existing = LookupClassFromTableLocked(descriptor, klass->GetClassLoader(), hash);
  if (existing != nullptr) {
    return existing;
  }
  if (kIsDebugBuild && !klass->IsTemp() && klass->GetClassLoader() == nullptr &&
      dex_cache_image_class_lookup_required_) {
    // Check a class loaded with the system class loader matches one in the image if the class
    // is in the image.
    existing = LookupClassFromImage(descriptor);
    if (existing != nullptr) {
      CHECK_EQ(klass, existing);
    }
  }
  VerifyObject(klass);
  class_table_.InsertWithHash(GcRoot<mirror::Class>(klass), hash);
  if (log_new_class_table_roots_) {
    new_class_roots_.push_back(GcRoot<mirror::Class>(klass));
  }
  return nullptr;
}

mirror::Class* ClassLinker::UpdateClass(const char* descriptor, mirror::Class* klass,
                                        size_t hash) {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  auto existing_it = class_table_.FindWithHash(std::make_pair(descriptor, klass->GetClassLoader()),
                                               hash);
  CHECK(existing_it != class_table_.end());
  mirror::Class* existing = existing_it->Read();
  CHECK_NE(existing, klass) << descriptor;
  CHECK(!existing->IsResolved()) << descriptor;
  CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusResolving) << descriptor;

  CHECK(!klass->IsTemp()) << descriptor;
  if (kIsDebugBuild && klass->GetClassLoader() == nullptr &&
      dex_cache_image_class_lookup_required_) {
    // Check a class loaded with the system class loader matches one in the image if the class
    // is in the image.
    existing = LookupClassFromImage(descriptor);
    if (existing != nullptr) {
      CHECK_EQ(klass, existing) << descriptor;
    }
  }
  VerifyObject(klass);

  // Update the element in the hash set.
  *existing_it = GcRoot<mirror::Class>(klass);
  if (log_new_class_table_roots_) {
    new_class_roots_.push_back(GcRoot<mirror::Class>(klass));
  }

  return existing;
}

bool ClassLinker::RemoveClass(const char* descriptor, mirror::ClassLoader* class_loader) {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  auto pair = std::make_pair(descriptor, class_loader);
  auto it = class_table_.Find(pair);
  if (it != class_table_.end()) {
    class_table_.Erase(it);
    return true;
  }
  it = pre_zygote_class_table_.Find(pair);
  if (it != pre_zygote_class_table_.end()) {
    pre_zygote_class_table_.Erase(it);
    return true;
  }
  return false;
}

mirror::Class* ClassLinker::LookupClass(Thread* self, const char* descriptor, size_t hash,
                                        mirror::ClassLoader* class_loader) {
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
                                                       mirror::ClassLoader* class_loader,
                                                       size_t hash) {
  auto descriptor_pair = std::make_pair(descriptor, class_loader);
  auto it = pre_zygote_class_table_.FindWithHash(descriptor_pair, hash);
  if (it == pre_zygote_class_table_.end()) {
    it = class_table_.FindWithHash(descriptor_pair, hash);
    if (it == class_table_.end()) {
      return nullptr;
    }
  }
  return it->Read();
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
        size_t hash = ComputeModifiedUtf8Hash(descriptor);
        mirror::Class* existing = LookupClassFromTableLocked(descriptor, nullptr, hash);
        if (existing != nullptr) {
          CHECK_EQ(existing, klass) << PrettyClassAndClassLoader(existing) << " != "
              << PrettyClassAndClassLoader(klass);
        } else {
          class_table_.Insert(GcRoot<mirror::Class>(klass));
          if (log_new_class_table_roots_) {
            new_class_roots_.push_back(GcRoot<mirror::Class>(klass));
          }
        }
      }
    }
  }
  dex_cache_image_class_lookup_required_ = false;
}

void ClassLinker::MoveClassTableToPreZygote() {
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  DCHECK(pre_zygote_class_table_.Empty());
  pre_zygote_class_table_ = std::move(class_table_);
  class_table_.Clear();
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
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  while (true) {
    auto it = class_table_.Find(descriptor);
    if (it == class_table_.end()) {
      break;
    }
    result.push_back(it->Read());
    class_table_.Erase(it);
  }
  for (mirror::Class* k : result) {
    class_table_.Insert(GcRoot<mirror::Class>(k));
  }
  size_t pre_zygote_start = result.size();
  // Now handle the pre zygote table.
  // Note: This dirties the pre-zygote table but shouldn't be an issue since LookupClasses is only
  // called from the debugger.
  while (true) {
    auto it = pre_zygote_class_table_.Find(descriptor);
    if (it == pre_zygote_class_table_.end()) {
      break;
    }
    result.push_back(it->Read());
    pre_zygote_class_table_.Erase(it);
  }
  for (size_t i = pre_zygote_start; i < result.size(); ++i) {
    pre_zygote_class_table_.Insert(GcRoot<mirror::Class>(result[i]));
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
  if (klass->IsCompileTimeVerified() && Runtime::Current()->IsAotCompiler()) {
    return;
  }

  // The class might already be erroneous, for example at compile time if we attempted to verify
  // this class as a parent to another.
  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass.Get());
    return;
  }

  if (klass->GetStatus() == mirror::Class::kStatusResolved) {
    mirror::Class::SetStatus(klass, mirror::Class::kStatusVerifying, self);
  } else {
    CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime)
        << PrettyClass(klass.Get());
    CHECK(!Runtime::Current()->IsAotCompiler());
    mirror::Class::SetStatus(klass, mirror::Class::kStatusVerifyingAtRuntime, self);
  }

  // Skip verification if disabled.
  if (!Runtime::Current()->IsVerificationEnabled()) {
    mirror::Class::SetStatus(klass, mirror::Class::kStatusVerified, self);
    EnsurePreverifiedMethods(klass);
    return;
  }

  // Verify super class.
  StackHandleScope<2> hs(self);
  Handle<mirror::Class> super(hs.NewHandle(klass->GetSuperClass()));
  if (super.Get() != nullptr) {
    // Acquire lock to prevent races on verifying the super class.
    ObjectLock<mirror::Class> super_lock(self, super);

    if (!super->IsVerified() && !super->IsErroneous()) {
      VerifyClass(self, super);
    }
    if (!super->IsCompileTimeVerified()) {
      std::string error_msg(
          StringPrintf("Rejecting class %s that attempts to sub-class erroneous class %s",
                       PrettyDescriptor(klass.Get()).c_str(),
                       PrettyDescriptor(super.Get()).c_str()));
      LOG(WARNING) << error_msg  << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8();
      Handle<mirror::Throwable> cause(hs.NewHandle(self->GetException()));
      if (cause.Get() != nullptr) {
        self->ClearException();
      }
      ThrowVerifyError(klass.Get(), "%s", error_msg.c_str());
      if (cause.Get() != nullptr) {
        self->GetException()->SetCause(cause.Get());
      }
      ClassReference ref(klass->GetDexCache()->GetDexFile(), klass->GetDexClassDefIndex());
      if (Runtime::Current()->IsAotCompiler()) {
        Runtime::Current()->GetCompilerCallbacks()->ClassRejected(ref);
      }
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
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
    mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
    return;
  }
  verifier::MethodVerifier::FailureKind verifier_failure = verifier::MethodVerifier::kNoFailure;
  std::string error_msg;
  if (!preverified) {
    verifier_failure = verifier::MethodVerifier::VerifyClass(self, klass.Get(),
                                                             Runtime::Current()->IsAotCompiler(),
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
        mirror::Class::SetStatus(klass, mirror::Class::kStatusVerified, self);
      } else {
        CHECK_EQ(super->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime);
        mirror::Class::SetStatus(klass, mirror::Class::kStatusRetryVerificationAtRuntime, self);
        // Pretend a soft failure occured so that we don't consider the class verified below.
        verifier_failure = verifier::MethodVerifier::kSoftFailure;
      }
    } else {
      CHECK_EQ(verifier_failure, verifier::MethodVerifier::kSoftFailure);
      // Soft failures at compile time should be retried at runtime. Soft
      // failures at runtime will be handled by slow paths in the generated
      // code. Set status accordingly.
      if (Runtime::Current()->IsAotCompiler()) {
        mirror::Class::SetStatus(klass, mirror::Class::kStatusRetryVerificationAtRuntime, self);
      } else {
        mirror::Class::SetStatus(klass, mirror::Class::kStatusVerified, self);
        // As this is a fake verified status, make sure the methods are _not_ marked preverified
        // later.
        klass->SetPreverified();
      }
    }
  } else {
    LOG(WARNING) << "Verification failed on class " << PrettyDescriptor(klass.Get())
        << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8()
        << " because: " << error_msg;
    self->AssertNoPendingException();
    ThrowVerifyError(klass.Get(), "%s", error_msg.c_str());
    mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
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
  if (Runtime::Current()->IsAotCompiler()) {
    // Are we compiling the bootclasspath?
    if (Runtime::Current()->GetCompilerCallbacks()->IsBootImage()) {
      return false;
    }
    // We are compiling an app (not the image).

    // Is this an app class? (I.e. not a bootclasspath class)
    if (klass->GetClassLoader() != nullptr) {
      return false;
    }
  }

  const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  // In case we run without an image there won't be a backing oat file.
  if (oat_dex_file == nullptr) {
    return false;
  }

  // We may be running with a preopted oat file but without image. In this case,
  // we don't skip verification of preverified classes to ensure we initialize
  // dex caches with all types resolved during verification.
  // We need to trust image classes, as these might be coming out of a pre-opted, quickened boot
  // image (that we just failed loading), and the verifier can't be run on quickened opcodes when
  // the runtime isn't started. On the other hand, app classes can be re-verified even if they are
  // already pre-opted, as then the runtime is started.
  if (!Runtime::Current()->IsAotCompiler() &&
      !Runtime::Current()->GetHeap()->HasImageSpace() &&
      klass->GetClassLoader() != nullptr) {
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
  UNREACHABLE();
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
  StackHandleScope<10> hs(self);
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
  klass->SetDexCache(GetClassRoot(kJavaLangReflectProxy)->GetDexCache());
  mirror::Class::SetStatus(klass, mirror::Class::kStatusIdx, self);
  std::string descriptor(GetDescriptorForProxy(klass.Get()));
  size_t hash = ComputeModifiedUtf8Hash(descriptor.c_str());

  // Insert the class before loading the fields as the field roots
  // (ArtField::declaring_class_) are only visited from the class
  // table. There can't be any suspend points between inserting the
  // class and setting the field arrays below.
  mirror::Class* existing = InsertClass(descriptor.c_str(), klass.Get(), hash);
  CHECK(existing == nullptr);

  // Instance fields are inherited, but we add a couple of static fields...
  const size_t num_fields = 2;
  ArtField* sfields = AllocArtFieldArray(self, num_fields);
  klass->SetSFields(sfields);
  klass->SetNumStaticFields(num_fields);

  // 1. Create a static field 'interfaces' that holds the _declared_ interfaces implemented by
  // our proxy, so Class.getInterfaces doesn't return the flattened set.
  ArtField* interfaces_sfield = &sfields[0];
  interfaces_sfield->SetDexFieldIndex(0);
  interfaces_sfield->SetDeclaringClass(klass.Get());
  interfaces_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);

  // 2. Create a static field 'throws' that holds exceptions thrown by our methods.
  ArtField* throws_sfield = &sfields[1];
  throws_sfield->SetDexFieldIndex(1);
  throws_sfield->SetDeclaringClass(klass.Get());
  throws_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);

  // Proxies have 1 direct method, the constructor
  {
    StackHandleScope<2> hs2(self);
    Handle<mirror::ObjectArray<mirror::ArtMethod>> directs =
        hs2.NewHandle(AllocArtMethodArray(self, 1));
    if (UNLIKELY(directs.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return nullptr;
    }
    klass->SetDirectMethods(directs.Get());
    Handle<mirror::ArtMethod> constructor =
        hs2.NewHandle(CreateProxyConstructor(self, klass));
    if (UNLIKELY(constructor.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return nullptr;
    }
    klass->SetDirectMethod(0, constructor.Get());
  }

  // Create virtual method using specified prototypes.
  auto h_methods = hs.NewHandle(soa.Decode<mirror::ObjectArray<mirror::Method>*>(methods));
  DCHECK_EQ(h_methods->GetClass(), mirror::Method::ArrayClass())
    << PrettyClass(h_methods->GetClass());
  const size_t num_virtual_methods = h_methods->GetLength();
  {
    StackHandleScope<1> hs2(self);
    Handle<mirror::ObjectArray<mirror::ArtMethod>> virtuals =
        hs2.NewHandle(AllocArtMethodArray(self, num_virtual_methods));
    if (UNLIKELY(virtuals.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return nullptr;
    }
    klass->SetVirtualMethods(virtuals.Get());
  }
  for (size_t i = 0; i < num_virtual_methods; ++i) {
    StackHandleScope<2> hs2(self);
    Handle<mirror::ArtMethod> prototype(hs2.NewHandle(h_methods->Get(i)->GetArtMethod()));
    Handle<mirror::ArtMethod> clone(hs2.NewHandle(CreateProxyMethod(self, klass, prototype)));
    if (UNLIKELY(clone.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return nullptr;
    }
    klass->SetVirtualMethod(i, clone.Get());
  }

  // The super class is java.lang.reflect.Proxy
  klass->SetSuperClass(GetClassRoot(kJavaLangReflectProxy));
  // Now effectively in the loaded state.
  mirror::Class::SetStatus(klass, mirror::Class::kStatusLoaded, self);
  self->AssertNoPendingException();

  MutableHandle<mirror::Class> new_class = hs.NewHandle<mirror::Class>(nullptr);
  {
    // Must hold lock on object when resolved.
    ObjectLock<mirror::Class> resolution_lock(self, klass);
    // Link the fields and virtual methods, creating vtable and iftables.
    // The new class will replace the old one in the class table.
    Handle<mirror::ObjectArray<mirror::Class> > h_interfaces(
        hs.NewHandle(soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces)));
    if (!LinkClass(self, descriptor.c_str(), klass, h_interfaces, &new_class)) {
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      return nullptr;
    }
  }
  CHECK(klass->IsRetired());
  CHECK_NE(klass.Get(), new_class.Get());
  klass.Assign(new_class.Get());

  CHECK_EQ(interfaces_sfield->GetDeclaringClass(), klass.Get());
  interfaces_sfield->SetObject<false>(klass.Get(),
                                      soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces));
  CHECK_EQ(throws_sfield->GetDeclaringClass(), klass.Get());
  throws_sfield->SetObject<false>(klass.Get(),
      soa.Decode<mirror::ObjectArray<mirror::ObjectArray<mirror::Class> >*>(throws));

  {
    // Lock on klass is released. Lock new class object.
    ObjectLock<mirror::Class> initialization_lock(self, klass);
    mirror::Class::SetStatus(klass, mirror::Class::kStatusInitialized, self);
  }

  // sanity checks
  if (kIsDebugBuild) {
    CHECK(klass->GetIFields() == nullptr);
    CheckProxyConstructor(klass->GetDirectMethod(0));
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      StackHandleScope<2> hs2(self);
      Handle<mirror::ArtMethod> prototype(hs2.NewHandle(h_methods->Get(i)->GetArtMethod()));
      Handle<mirror::ArtMethod> virtual_method(hs2.NewHandle(klass->GetVirtualMethod(i)));
      CheckProxyMethod(virtual_method, prototype);
    }

    StackHandleScope<1> hs2(self);
    Handle<mirror::String> decoded_name = hs2.NewHandle(soa.Decode<mirror::String*>(name));
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
  {
    ReaderMutexLock mu(Thread::Current(), dex_lock_);
    // Locate the dex cache of the original interface/Object
    for (const GcRoot<mirror::DexCache>& root : dex_caches_) {
      auto* dex_cache = root.Read();
      if (proxy_method->HasSameDexCacheResolvedTypes(dex_cache->GetResolvedTypes())) {
        mirror::ArtMethod* resolved_method = dex_cache->GetResolvedMethod(
            proxy_method->GetDexMethodIndex());
        CHECK(resolved_method != nullptr);
        return resolved_method;
      }
    }
  }
  LOG(FATAL) << "Didn't find dex cache for " << PrettyClass(proxy_class) << " "
      << PrettyMethod(proxy_method);
  UNREACHABLE();
}


mirror::ArtMethod* ClassLinker::CreateProxyConstructor(Thread* self,
                                                       Handle<mirror::Class> klass) {
  // Create constructor for Proxy that must initialize h
  mirror::ObjectArray<mirror::ArtMethod>* proxy_direct_methods =
      GetClassRoot(kJavaLangReflectProxy)->GetDirectMethods();
  CHECK_EQ(proxy_direct_methods->GetLength(), 16);
  mirror::ArtMethod* proxy_constructor = proxy_direct_methods->Get(2);
  // Ensure constructor is in dex cache so that we can use the dex cache to look up the overridden
  // constructor method.
  GetClassRoot(kJavaLangReflectProxy)->GetDexCache()->SetResolvedMethod(
      proxy_constructor->GetDexMethodIndex(), proxy_constructor);
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
  auto* dex_cache = prototype->GetDeclaringClass()->GetDexCache();
  // Avoid dirtying the dex cache unless we need to.
  if (dex_cache->GetResolvedMethod(prototype->GetDexMethodIndex()) != prototype.Get()) {
    dex_cache->SetResolvedMethod(prototype->GetDexMethodIndex(), prototype.Get());
  }
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
  CHECK(prototype->HasSameDexCacheResolvedMethods(method.Get()));
  CHECK(prototype->HasSameDexCacheResolvedTypes(method.Get()));
  CHECK_EQ(prototype->GetDeclaringClass()->GetDexCache(), method->GetDexCache());
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
          CHECK(Runtime::Current()->IsAotCompiler());
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
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      return false;
    }
    self->AllowThreadSuspension();

    CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusVerified) << PrettyClass(klass.Get());

    // From here out other threads may observe that we're initializing and so changes of state
    // require the a notification.
    klass->SetClinitThreadId(self->GetTid());
    mirror::Class::SetStatus(klass, mirror::Class::kStatusInitializing, self);

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
            << (self->GetException() != nullptr ? self->GetException()->Dump() : "");
        ObjectLock<mirror::Class> lock(self, klass);
        // Initialization failed because the super-class is erroneous.
        mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
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
      ArtField* field = klass->GetStaticField(i);
      const uint32_t field_idx = field->GetDexFieldIndex();
      ArtField* resolved_field = dex_cache->GetResolvedField(field_idx, image_pointer_size_);
      if (resolved_field == nullptr) {
        dex_cache->SetResolvedField(field_idx, field, image_pointer_size_);
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
        ArtField* field = ResolveField(
            dex_file, field_it.GetMemberIndex(), dex_cache, class_loader, true);
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
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      success = false;
    } else if (Runtime::Current()->IsTransactionAborted()) {
      // The exception thrown when the transaction aborted has been caught and cleared
      // so we need to throw it again now.
      VLOG(compiler) << "Return from class initializer of " << PrettyDescriptor(klass.Get())
                     << " without exception while transaction was aborted: re-throw it now.";
      Runtime::Current()->ThrowTransactionAbortError(self);
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      success = false;
    } else {
      RuntimeStats* global_stats = Runtime::Current()->GetStats();
      RuntimeStats* thread_stats = self->GetStats();
      ++global_stats->class_init_count;
      ++thread_stats->class_init_count;
      global_stats->class_init_time_ns += (t1 - t0);
      thread_stats->class_init_time_ns += (t1 - t0);
      // Set the class as initialized except if failed to initialize static fields.
      mirror::Class::SetStatus(klass, mirror::Class::kStatusInitialized, self);
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
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      return false;
    }
    // Spurious wakeup? Go back to waiting.
    if (klass->GetStatus() == mirror::Class::kStatusInitializing) {
      continue;
    }
    if (klass->GetStatus() == mirror::Class::kStatusVerified &&
        Runtime::Current()->IsAotCompiler()) {
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

static bool HasSameSignatureWithDifferentClassLoaders(Thread* self,
                                                      Handle<mirror::ArtMethod> method1,
                                                      Handle<mirror::ArtMethod> method2,
                                                      std::string* error_msg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> return_type(hs.NewHandle(method1->GetReturnType()));
    mirror::Class* other_return_type = method2->GetReturnType();
    // NOTE: return_type.Get() must be sequenced after method2->GetReturnType().
    if (UNLIKELY(other_return_type != return_type.Get())) {
      *error_msg = StringPrintf("Return types mismatch: %s(%p) vs %s(%p)",
                                PrettyClassAndClassLoader(return_type.Get()).c_str(),
                                return_type.Get(),
                                PrettyClassAndClassLoader(other_return_type).c_str(),
                                other_return_type);
      return false;
    }
  }
  const DexFile::TypeList* types1 = method1->GetParameterTypeList();
  const DexFile::TypeList* types2 = method2->GetParameterTypeList();
  if (types1 == nullptr) {
    if (types2 != nullptr && types2->Size() != 0) {
      *error_msg = StringPrintf("Type list mismatch with %s",
                                PrettyMethod(method2.Get(), true).c_str());
      return false;
    }
    return true;
  } else if (UNLIKELY(types2 == nullptr)) {
    if (types1->Size() != 0) {
      *error_msg = StringPrintf("Type list mismatch with %s",
                                PrettyMethod(method2.Get(), true).c_str());
      return false;
    }
    return true;
  }
  uint32_t num_types = types1->Size();
  if (UNLIKELY(num_types != types2->Size())) {
    *error_msg = StringPrintf("Type list mismatch with %s",
                              PrettyMethod(method2.Get(), true).c_str());
    return false;
  }
  for (uint32_t i = 0; i < num_types; ++i) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> param_type(hs.NewHandle(
        method1->GetClassFromTypeIndex(types1->GetTypeItem(i).type_idx_, true)));
    mirror::Class* other_param_type =
        method2->GetClassFromTypeIndex(types2->GetTypeItem(i).type_idx_, true);
    // NOTE: param_type.Get() must be sequenced after method2->GetClassFromTypeIndex(...).
    if (UNLIKELY(param_type.Get() != other_param_type)) {
      *error_msg = StringPrintf("Parameter %u type mismatch: %s(%p) vs %s(%p)",
                                i,
                                PrettyClassAndClassLoader(param_type.Get()).c_str(),
                                param_type.Get(),
                                PrettyClassAndClassLoader(other_param_type).c_str(),
                                other_param_type);
      return false;
    }
  }
  return true;
}


bool ClassLinker::ValidateSuperClassDescriptors(Handle<mirror::Class> klass) {
  if (klass->IsInterface()) {
    return true;
  }
  // Begin with the methods local to the superclass.
  Thread* self = Thread::Current();
  StackHandleScope<2> hs(self);
  MutableHandle<mirror::ArtMethod> h_m(hs.NewHandle<mirror::ArtMethod>(nullptr));
  MutableHandle<mirror::ArtMethod> super_h_m(hs.NewHandle<mirror::ArtMethod>(nullptr));
  if (klass->HasSuperClass() &&
      klass->GetClassLoader() != klass->GetSuperClass()->GetClassLoader()) {
    for (int i = klass->GetSuperClass()->GetVTableLength() - 1; i >= 0; --i) {
      h_m.Assign(klass->GetVTableEntry(i));
      super_h_m.Assign(klass->GetSuperClass()->GetVTableEntry(i));
      if (h_m.Get() != super_h_m.Get()) {
        std::string error_msg;
        if (!HasSameSignatureWithDifferentClassLoaders(self, h_m, super_h_m, &error_msg)) {
          ThrowLinkageError(klass.Get(),
                            "Class %s method %s resolves differently in superclass %s: %s",
                            PrettyDescriptor(klass.Get()).c_str(),
                            PrettyMethod(h_m.Get()).c_str(),
                            PrettyDescriptor(klass->GetSuperClass()).c_str(),
                            error_msg.c_str());
          return false;
        }
      }
    }
  }
  for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
    if (klass->GetClassLoader() != klass->GetIfTable()->GetInterface(i)->GetClassLoader()) {
      uint32_t num_methods = klass->GetIfTable()->GetInterface(i)->NumVirtualMethods();
      for (uint32_t j = 0; j < num_methods; ++j) {
        h_m.Assign(klass->GetIfTable()->GetMethodArray(i)->GetWithoutChecks(j));
        super_h_m.Assign(klass->GetIfTable()->GetInterface(i)->GetVirtualMethod(j));
        if (h_m.Get() != super_h_m.Get()) {
          std::string error_msg;
          if (!HasSameSignatureWithDifferentClassLoaders(self, h_m, super_h_m, &error_msg)) {
            ThrowLinkageError(klass.Get(),
                              "Class %s method %s resolves differently in interface %s: %s",
                              PrettyDescriptor(klass.Get()).c_str(),
                              PrettyMethod(h_m.Get()).c_str(),
                              PrettyDescriptor(klass->GetIfTable()->GetInterface(i)).c_str(),
                              error_msg.c_str());
            return false;
          }
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
  ArtField* fields = new_class->GetIFields();
  for (size_t i = 0, count = new_class->NumInstanceFields(); i < count; i++) {
    if (fields[i].GetDeclaringClass() == temp_class) {
      fields[i].SetDeclaringClass(new_class);
    }
  }

  fields = new_class->GetSFields();
  for (size_t i = 0, count = new_class->NumStaticFields(); i < count; i++) {
    if (fields[i].GetDeclaringClass() == temp_class) {
      fields[i].SetDeclaringClass(new_class);
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
                            MutableHandle<mirror::Class>* h_new_class_out) {
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
    mirror::Class::SetStatus(klass, mirror::Class::kStatusResolved, self);
    h_new_class_out->Assign(klass.Get());
  } else {
    CHECK(!klass->IsResolved());
    // Retire the temporary class and create the correctly sized resolved class.
    StackHandleScope<1> hs(self);
    auto h_new_class = hs.NewHandle<mirror::Class>(
        klass->CopyOf(self, class_size, &imt_handle_scope));
    if (UNLIKELY(h_new_class.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());  // Expect an OOME.
      mirror::Class::SetStatus(klass, mirror::Class::kStatusError, self);
      return false;
    }

    CHECK_EQ(h_new_class->GetClassSize(), class_size);
    ObjectLock<mirror::Class> lock(self, h_new_class);
    FixupTemporaryDeclaringClass(klass.Get(), h_new_class.Get());
    mirror::Class* existing = UpdateClass(descriptor, h_new_class.Get(),
                                          ComputeModifiedUtf8Hash(descriptor));
    CHECK(existing == nullptr || existing == klass.Get());

    // This will notify waiters on temp class that saw the not yet resolved class in the
    // class_table_ during EnsureResolved.
    mirror::Class::SetStatus(klass, mirror::Class::kStatusRetired, self);

    CHECK_EQ(h_new_class->GetStatus(), mirror::Class::kStatusResolving);
    // This will notify waiters on new_class that saw the not yet resolved
    // class in the class_table_ during EnsureResolved.
    mirror::Class::SetStatus(h_new_class, mirror::Class::kStatusResolved, self);
    // Return the new class.
    h_new_class_out->Assign(h_new_class.Get());
  }
  return true;
}

static void CountMethodsAndFields(ClassDataItemIterator& dex_data,
                                  size_t* virtual_methods,
                                  size_t* direct_methods,
                                  size_t* static_fields,
                                  size_t* instance_fields) {
  *virtual_methods = *direct_methods = *static_fields = *instance_fields = 0;

  while (dex_data.HasNextStaticField()) {
    dex_data.Next();
    (*static_fields)++;
  }
  while (dex_data.HasNextInstanceField()) {
    dex_data.Next();
    (*instance_fields)++;
  }
  while (dex_data.HasNextDirectMethod()) {
    (*direct_methods)++;
    dex_data.Next();
  }
  while (dex_data.HasNextVirtualMethod()) {
    (*virtual_methods)++;
    dex_data.Next();
  }
  DCHECK(!dex_data.HasNext());
}

static void DumpClass(std::ostream& os,
                      const DexFile& dex_file, const DexFile::ClassDef& dex_class_def,
                      const char* suffix) {
  ClassDataItemIterator dex_data(dex_file, dex_file.GetClassData(dex_class_def));
  os << dex_file.GetClassDescriptor(dex_class_def) << suffix << ":\n";
  os << " Static fields:\n";
  while (dex_data.HasNextStaticField()) {
    const DexFile::FieldId& id = dex_file.GetFieldId(dex_data.GetMemberIndex());
    os << "  " << dex_file.GetFieldTypeDescriptor(id) << " " << dex_file.GetFieldName(id) << "\n";
    dex_data.Next();
  }
  os << " Instance fields:\n";
  while (dex_data.HasNextInstanceField()) {
    const DexFile::FieldId& id = dex_file.GetFieldId(dex_data.GetMemberIndex());
    os << "  " << dex_file.GetFieldTypeDescriptor(id) << " " << dex_file.GetFieldName(id) << "\n";
    dex_data.Next();
  }
  os << " Direct methods:\n";
  while (dex_data.HasNextDirectMethod()) {
    const DexFile::MethodId& id = dex_file.GetMethodId(dex_data.GetMemberIndex());
    os << "  " << dex_file.GetMethodName(id) << dex_file.GetMethodSignature(id).ToString() << "\n";
    dex_data.Next();
  }
  os << " Virtual methods:\n";
  while (dex_data.HasNextVirtualMethod()) {
    const DexFile::MethodId& id = dex_file.GetMethodId(dex_data.GetMemberIndex());
    os << "  " << dex_file.GetMethodName(id) << dex_file.GetMethodSignature(id).ToString() << "\n";
    dex_data.Next();
  }
}

static std::string DumpClasses(const DexFile& dex_file1, const DexFile::ClassDef& dex_class_def1,
                               const DexFile& dex_file2, const DexFile::ClassDef& dex_class_def2) {
  std::ostringstream os;
  DumpClass(os, dex_file1, dex_class_def1, " (Compile time)");
  DumpClass(os, dex_file2, dex_class_def2, " (Runtime)");
  return os.str();
}


// Very simple structural check on whether the classes match. Only compares the number of
// methods and fields.
static bool SimpleStructuralCheck(const DexFile& dex_file1, const DexFile::ClassDef& dex_class_def1,
                                  const DexFile& dex_file2, const DexFile::ClassDef& dex_class_def2,
                                  std::string* error_msg) {
  ClassDataItemIterator dex_data1(dex_file1, dex_file1.GetClassData(dex_class_def1));
  ClassDataItemIterator dex_data2(dex_file2, dex_file2.GetClassData(dex_class_def2));

  // Counters for current dex file.
  size_t dex_virtual_methods1, dex_direct_methods1, dex_static_fields1, dex_instance_fields1;
  CountMethodsAndFields(dex_data1, &dex_virtual_methods1, &dex_direct_methods1, &dex_static_fields1,
                        &dex_instance_fields1);
  // Counters for compile-time dex file.
  size_t dex_virtual_methods2, dex_direct_methods2, dex_static_fields2, dex_instance_fields2;
  CountMethodsAndFields(dex_data2, &dex_virtual_methods2, &dex_direct_methods2, &dex_static_fields2,
                        &dex_instance_fields2);

  if (dex_virtual_methods1 != dex_virtual_methods2) {
    std::string class_dump = DumpClasses(dex_file1, dex_class_def1, dex_file2, dex_class_def2);
    *error_msg = StringPrintf("Virtual method count off: %zu vs %zu\n%s", dex_virtual_methods1,
                              dex_virtual_methods2, class_dump.c_str());
    return false;
  }
  if (dex_direct_methods1 != dex_direct_methods2) {
    std::string class_dump = DumpClasses(dex_file1, dex_class_def1, dex_file2, dex_class_def2);
    *error_msg = StringPrintf("Direct method count off: %zu vs %zu\n%s", dex_direct_methods1,
                              dex_direct_methods2, class_dump.c_str());
    return false;
  }
  if (dex_static_fields1 != dex_static_fields2) {
    std::string class_dump = DumpClasses(dex_file1, dex_class_def1, dex_file2, dex_class_def2);
    *error_msg = StringPrintf("Static field count off: %zu vs %zu\n%s", dex_static_fields1,
                              dex_static_fields2, class_dump.c_str());
    return false;
  }
  if (dex_instance_fields1 != dex_instance_fields2) {
    std::string class_dump = DumpClasses(dex_file1, dex_class_def1, dex_file2, dex_class_def2);
    *error_msg = StringPrintf("Instance field count off: %zu vs %zu\n%s", dex_instance_fields1,
                              dex_instance_fields2, class_dump.c_str());
    return false;
  }

  return true;
}

// Checks whether a the super-class changed from what we had at compile-time. This would
// invalidate quickening.
static bool CheckSuperClassChange(Handle<mirror::Class> klass,
                                  const DexFile& dex_file,
                                  const DexFile::ClassDef& class_def,
                                  mirror::Class* super_class)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Check for unexpected changes in the superclass.
  // Quick check 1) is the super_class class-loader the boot class loader? This always has
  // precedence.
  if (super_class->GetClassLoader() != nullptr &&
      // Quick check 2) different dex cache? Breaks can only occur for different dex files,
      // which is implied by different dex cache.
      klass->GetDexCache() != super_class->GetDexCache()) {
    // Now comes the expensive part: things can be broken if (a) the klass' dex file has a
    // definition for the super-class, and (b) the files are in separate oat files. The oat files
    // are referenced from the dex file, so do (b) first. Only relevant if we have oat files.
    const OatDexFile* class_oat_dex_file = dex_file.GetOatDexFile();
    const OatFile* class_oat_file = nullptr;
    if (class_oat_dex_file != nullptr) {
      class_oat_file = class_oat_dex_file->GetOatFile();
    }

    if (class_oat_file != nullptr) {
      const OatDexFile* loaded_super_oat_dex_file = super_class->GetDexFile().GetOatDexFile();
      const OatFile* loaded_super_oat_file = nullptr;
      if (loaded_super_oat_dex_file != nullptr) {
        loaded_super_oat_file = loaded_super_oat_dex_file->GetOatFile();
      }

      if (loaded_super_oat_file != nullptr && class_oat_file != loaded_super_oat_file) {
        // Now check (a).
        const DexFile::ClassDef* super_class_def = dex_file.FindClassDef(class_def.superclass_idx_);
        if (super_class_def != nullptr) {
          // Uh-oh, we found something. Do our check.
          std::string error_msg;
          if (!SimpleStructuralCheck(dex_file, *super_class_def,
                                     super_class->GetDexFile(), *super_class->GetClassDef(),
                                     &error_msg)) {
            // Print a warning to the log. This exception might be caught, e.g., as common in test
            // drivers. When the class is later tried to be used, we re-throw a new instance, as we
            // only save the type of the exception.
            LOG(WARNING) << "Incompatible structural change detected: " <<
                StringPrintf(
                    "Structural change of %s is hazardous (%s at compile time, %s at runtime): %s",
                    PrettyType(super_class_def->class_idx_, dex_file).c_str(),
                    class_oat_file->GetLocation().c_str(),
                    loaded_super_oat_file->GetLocation().c_str(),
                    error_msg.c_str());
            ThrowIncompatibleClassChangeError(klass.Get(),
                "Structural change of %s is hazardous (%s at compile time, %s at runtime): %s",
                PrettyType(super_class_def->class_idx_, dex_file).c_str(),
                class_oat_file->GetLocation().c_str(),
                loaded_super_oat_file->GetLocation().c_str(),
                error_msg.c_str());
            return false;
          }
        }
      }
    }
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

    if (!CheckSuperClassChange(klass, dex_file, class_def, super_class)) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return false;
    }
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
  mirror::Class::SetStatus(klass, mirror::Class::kStatusLoaded, nullptr);
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
    if (!IsUint<16>(count)) {
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
    uint32_t hash = ComputeModifiedUtf8Hash(name);
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
    uint32_t hash = ComputeModifiedUtf8Hash(name);
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
    if (!IsUint<16>(actual_count)) {
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
    if (!IsUint<16>(num_virtual_methods)) {
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
      StackHandleScope<2> hs2(self);
      const bool is_super = i < super_ifcount;
      const bool super_interface = is_super && extend_super_iftable;
      Handle<mirror::ObjectArray<mirror::ArtMethod>> method_array;
      Handle<mirror::ObjectArray<mirror::ArtMethod>> input_array;
      if (super_interface) {
        mirror::IfTable* if_table = klass->GetSuperClass()->GetIfTable();
        DCHECK(if_table != nullptr);
        DCHECK(if_table->GetMethodArray(i) != nullptr);
        // If we are working on a super interface, try extending the existing method array.
        method_array = hs2.NewHandle(if_table->GetMethodArray(i)->Clone(self)->
            AsObjectArray<mirror::ArtMethod>());
        // We are overwriting a super class interface, try to only virtual methods instead of the
        // whole vtable.
        input_array = hs2.NewHandle(klass->GetVirtualMethods());
      } else {
        method_array = hs2.NewHandle(AllocArtMethodArray(self, num_methods));
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
    mirror::ObjectArray<mirror::ArtMethod>* check_vtable = klass->GetVTableDuringLinking();
    for (int i = 0; i < check_vtable->GetLength(); ++i) {
      CHECK(check_vtable->GetWithoutChecks(i) != nullptr);
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
  bool operator()(ArtField* field1, ArtField* field2)
      NO_THREAD_SAFETY_ANALYSIS {
    // First come reference fields, then 64-bit, then 32-bit, and then 16-bit, then finally 8-bit.
    Primitive::Type type1 = field1->GetTypeAsPrimitiveType();
    Primitive::Type type2 = field2->GetTypeAsPrimitiveType();
    if (type1 != type2) {
      if (type1 == Primitive::kPrimNot) {
        // Reference always goes first.
        return true;
      }
      if (type2 == Primitive::kPrimNot) {
        // Reference always goes first.
        return false;
      }
      size_t size1 = Primitive::ComponentSize(type1);
      size_t size2 = Primitive::ComponentSize(type2);
      if (size1 != size2) {
        // Larger primitive types go first.
        return size1 > size2;
      }
      // Primitive types differ but sizes match. Arbitrarily order by primitive type.
      return type1 < type2;
    }
    // Same basic group? Then sort by dex field index. This is guaranteed to be sorted
    // by name and for equal names by type id index.
    // NOTE: This works also for proxies. Their static fields are assigned appropriate indexes.
    return field1->GetDexFieldIndex() < field2->GetDexFieldIndex();
  }
};

bool ClassLinker::LinkFields(Thread* self, Handle<mirror::Class> klass, bool is_static,
                             size_t* class_size) {
  self->AllowThreadSuspension();
  const size_t num_fields = is_static ? klass->NumStaticFields() : klass->NumInstanceFields();
  ArtField* const fields = is_static ? klass->GetSFields() : klass->GetIFields();

  // Initialize field_offset
  MemberOffset field_offset(0);
  if (is_static) {
    field_offset = klass->GetFirstReferenceStaticFieldOffsetDuringLinking();
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
  std::deque<ArtField*> grouped_and_sorted_fields;
  const char* old_no_suspend_cause = self->StartAssertNoThreadSuspension(
      "Naked ArtField references in deque");
  for (size_t i = 0; i < num_fields; i++) {
    grouped_and_sorted_fields.push_back(&fields[i]);
  }
  std::sort(grouped_and_sorted_fields.begin(), grouped_and_sorted_fields.end(),
            LinkFieldsComparator());

  // References should be at the front.
  size_t current_field = 0;
  size_t num_reference_fields = 0;
  FieldGaps gaps;

  for (; current_field < num_fields; current_field++) {
    ArtField* field = grouped_and_sorted_fields.front();
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    bool isPrimitive = type != Primitive::kPrimNot;
    if (isPrimitive) {
      break;  // past last reference, move on to the next phase
    }
    if (UNLIKELY(!IsAligned<sizeof(mirror::HeapReference<mirror::Object>)>(
        field_offset.Uint32Value()))) {
      MemberOffset old_offset = field_offset;
      field_offset = MemberOffset(RoundUp(field_offset.Uint32Value(), 4));
      AddFieldGap(old_offset.Uint32Value(), field_offset.Uint32Value(), &gaps);
    }
    DCHECK(IsAligned<sizeof(mirror::HeapReference<mirror::Object>)>(field_offset.Uint32Value()));
    grouped_and_sorted_fields.pop_front();
    num_reference_fields++;
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() +
                                sizeof(mirror::HeapReference<mirror::Object>));
  }
  // Gaps are stored as a max heap which means that we must shuffle from largest to smallest
  // otherwise we could end up with suboptimal gap fills.
  ShuffleForward<8>(&current_field, &field_offset, &grouped_and_sorted_fields, &gaps);
  ShuffleForward<4>(&current_field, &field_offset, &grouped_and_sorted_fields, &gaps);
  ShuffleForward<2>(&current_field, &field_offset, &grouped_and_sorted_fields, &gaps);
  ShuffleForward<1>(&current_field, &field_offset, &grouped_and_sorted_fields, &gaps);
  CHECK(grouped_and_sorted_fields.empty()) << "Missed " << grouped_and_sorted_fields.size() <<
      " fields.";
  self->EndAssertNoThreadSuspension(old_no_suspend_cause);

  // We lie to the GC about the java.lang.ref.Reference.referent field, so it doesn't scan it.
  if (!is_static && klass->DescriptorEquals("Ljava/lang/ref/Reference;")) {
    // We know there are no non-reference fields in the Reference classes, and we know
    // that 'referent' is alphabetically last, so this is easy...
    CHECK_EQ(num_reference_fields, num_fields) << PrettyClass(klass.Get());
    CHECK_STREQ(fields[num_fields - 1].GetName(), "referent") << PrettyClass(klass.Get());
    --num_reference_fields;
  }

  size_t size = field_offset.Uint32Value();
  // Update klass
  if (is_static) {
    klass->SetNumReferenceStaticFields(num_reference_fields);
    *class_size = size;
  } else {
    klass->SetNumReferenceInstanceFields(num_reference_fields);
    if (!klass->IsVariableSize()) {
      if (klass->DescriptorEquals("Ljava/lang/reflect/ArtMethod;")) {
        size_t pointer_size = GetInstructionSetPointerSize(Runtime::Current()->GetInstructionSet());
        klass->SetObjectSize(mirror::ArtMethod::InstanceSize(pointer_size));
      } else {
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
  }

  if (kIsDebugBuild) {
    // Make sure that the fields array is ordered by name but all reference
    // offsets are at the beginning as far as alignment allows.
    MemberOffset start_ref_offset = is_static
        ? klass->GetFirstReferenceStaticFieldOffsetDuringLinking()
        : klass->GetFirstReferenceInstanceFieldOffset();
    MemberOffset end_ref_offset(start_ref_offset.Uint32Value() +
                                num_reference_fields *
                                    sizeof(mirror::HeapReference<mirror::Object>));
    MemberOffset current_ref_offset = start_ref_offset;
    for (size_t i = 0; i < num_fields; i++) {
      ArtField* field = &fields[i];
      VLOG(class_linker) << "LinkFields: " << (is_static ? "static" : "instance")
          << " class=" << PrettyClass(klass.Get()) << " field=" << PrettyField(field) << " offset="
          << field->GetOffset();
      if (i != 0) {
        ArtField* const prev_field = &fields[i - 1];
        // NOTE: The field names can be the same. This is not possible in the Java language
        // but it's valid Java/dex bytecode and for example proguard can generate such bytecode.
        CHECK_LE(strcmp(prev_field->GetName(), field->GetName()), 0);
      }
      Primitive::Type type = field->GetTypeAsPrimitiveType();
      bool is_primitive = type != Primitive::kPrimNot;
      if (klass->DescriptorEquals("Ljava/lang/ref/Reference;") &&
          strcmp("referent", field->GetName()) == 0) {
        is_primitive = true;  // We lied above, so we have to expect a lie here.
      }
      MemberOffset offset = field->GetOffsetDuringLinking();
      if (is_primitive) {
        if (offset.Uint32Value() < end_ref_offset.Uint32Value()) {
          // Shuffled before references.
          size_t type_size = Primitive::ComponentSize(type);
          CHECK_LT(type_size, sizeof(mirror::HeapReference<mirror::Object>));
          CHECK_LT(offset.Uint32Value(), start_ref_offset.Uint32Value());
          CHECK_LE(offset.Uint32Value() + type_size, start_ref_offset.Uint32Value());
          CHECK(!IsAligned<sizeof(mirror::HeapReference<mirror::Object>)>(offset.Uint32Value()));
        }
      } else {
        CHECK_EQ(current_ref_offset.Uint32Value(), offset.Uint32Value());
        current_ref_offset = MemberOffset(current_ref_offset.Uint32Value() +
                                          sizeof(mirror::HeapReference<mirror::Object>));
      }
    }
    CHECK_EQ(current_ref_offset.Uint32Value(), end_ref_offset.Uint32Value());
  }
  return true;
}

//  Set the bitmap of reference instance field offsets.
void ClassLinker::CreateReferenceInstanceOffsets(Handle<mirror::Class> klass) {
  uint32_t reference_offsets = 0;
  mirror::Class* super_class = klass->GetSuperClass();
  // Leave the reference offsets as 0 for mirror::Object (the class field is handled specially).
  if (super_class != nullptr) {
    reference_offsets = super_class->GetReferenceInstanceOffsets();
    // Compute reference offsets unless our superclass overflowed.
    if (reference_offsets != mirror::Class::kClassWalkSuper) {
      size_t num_reference_fields = klass->NumReferenceInstanceFieldsDuringLinking();
      if (num_reference_fields != 0u) {
        // All of the fields that contain object references are guaranteed be grouped in memory
        // starting at an appropriately aligned address after super class object data.
        uint32_t start_offset = RoundUp(super_class->GetObjectSize(),
                                        sizeof(mirror::HeapReference<mirror::Object>));
        uint32_t start_bit = (start_offset - mirror::kObjectHeaderSize) /
            sizeof(mirror::HeapReference<mirror::Object>);
        if (start_bit + num_reference_fields > 32) {
          reference_offsets = mirror::Class::kClassWalkSuper;
        } else {
          reference_offsets |= (0xffffffffu << start_bit) &
                               (0xffffffffu >> (32 - (start_bit + num_reference_fields)));
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
      Handle<mirror::Throwable> cause(hs.NewHandle(self->GetException()));
      if (cause->InstanceOf(GetClassRoot(kJavaLangClassNotFoundException))) {
        DCHECK(resolved == nullptr);  // No Handle needed to preserve resolved.
        self->ClearException();
        ThrowNoClassDefFoundError("Failed resolution of: %s", descriptor);
        self->GetException()->SetCause(cause.Get());
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
      bool exception_generated = false;
      if (resolved != nullptr && referrer.Get() != nullptr) {
        mirror::Class* methods_class = resolved->GetDeclaringClass();
        mirror::Class* referring_class = referrer->GetDeclaringClass();
        if (!referring_class->CanAccess(methods_class)) {
          ThrowIllegalAccessErrorClassForMethodDispatch(referring_class, methods_class,
                                                        resolved, type);
          exception_generated = true;
        } else if (!referring_class->CanAccessMember(methods_class,
                                                     resolved->GetAccessFlags())) {
          ThrowIllegalAccessErrorMethod(referring_class, resolved);
          exception_generated = true;
        }
      }
      if (!exception_generated) {
        // Otherwise, throw an IncompatibleClassChangeError if we found something, and check
        // interface methods and throw if we find the method there. If we find nothing, throw a
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
    }
    Thread::Current()->AssertPendingException();
    return nullptr;
  }
}

ArtField* ClassLinker::ResolveField(const DexFile& dex_file, uint32_t field_idx,
                                    Handle<mirror::DexCache> dex_cache,
                                    Handle<mirror::ClassLoader> class_loader, bool is_static) {
  DCHECK(dex_cache.Get() != nullptr);
  ArtField* resolved = dex_cache->GetResolvedField(field_idx, image_pointer_size_);
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
  dex_cache->SetResolvedField(field_idx, resolved, image_pointer_size_);
  return resolved;
}

ArtField* ClassLinker::ResolveFieldJLS(const DexFile& dex_file, uint32_t field_idx,
                                       Handle<mirror::DexCache> dex_cache,
                                       Handle<mirror::ClassLoader> class_loader) {
  DCHECK(dex_cache.Get() != nullptr);
  ArtField* resolved = dex_cache->GetResolvedField(field_idx, image_pointer_size_);
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
    dex_cache->SetResolvedField(field_idx, resolved, image_pointer_size_);
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
    for (GcRoot<mirror::Class>& it : class_table_) {
      all_classes.push_back(it.Read());
    }
  }

  for (size_t i = 0; i < all_classes.size(); ++i) {
    all_classes[i]->DumpClass(std::cerr, flags);
  }
}

static OatFile::OatMethod CreateOatMethod(const void* code) {
  CHECK(code != nullptr);
  const uint8_t* base = reinterpret_cast<const uint8_t*>(code);  // Base of data points at code.
  base -= sizeof(void*);  // Move backward so that code_offset != 0.
  const uint32_t code_offset = sizeof(void*);
  return OatFile::OatMethod(base, code_offset);
}

bool ClassLinker::IsQuickResolutionStub(const void* entry_point) const {
  return (entry_point == GetQuickResolutionStub()) ||
      (quick_resolution_trampoline_ == entry_point);
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

void ClassLinker::SetEntryPointsToCompiledCode(mirror::ArtMethod* method,
                                               const void* method_code) const {
  OatFile::OatMethod oat_method = CreateOatMethod(method_code);
  oat_method.LinkMethod(method);
  method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
}

void ClassLinker::SetEntryPointsToInterpreter(mirror::ArtMethod* method) const {
  if (!method->IsNative()) {
    method->SetEntryPointFromInterpreter(artInterpreterToInterpreterBridge);
    method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
  } else {
    const void* quick_method_code = GetQuickGenericJniStub();
    OatFile::OatMethod oat_method = CreateOatMethod(quick_method_code);
    oat_method.LinkMethod(method);
    method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
  }
}

void ClassLinker::DumpForSigQuit(std::ostream& os) {
  Thread* self = Thread::Current();
  if (dex_cache_image_class_lookup_required_) {
    ScopedObjectAccess soa(self);
    MoveImageClassesToClassTable();
  }
  ReaderMutexLock mu(self, *Locks::classlinker_classes_lock_);
  os << "Zygote loaded classes=" << pre_zygote_class_table_.Size() << " post zygote classes="
     << class_table_.Size() << "\n";
}

size_t ClassLinker::NumLoadedClasses() {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  // Only return non zygote classes since these are the ones which apps which care about.
  return class_table_.Size();
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
    "Ljava/lang/reflect/ArtMethod;",
    "Ljava/lang/reflect/Constructor;",
    "Ljava/lang/reflect/Field;",
    "Ljava/lang/reflect/Method;",
    "Ljava/lang/reflect/Proxy;",
    "[Ljava/lang/String;",
    "[Ljava/lang/reflect/ArtMethod;",
    "[Ljava/lang/reflect/Constructor;",
    "[Ljava/lang/reflect/Field;",
    "[Ljava/lang/reflect/Method;",
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
  static_assert(arraysize(class_roots_descriptors) == size_t(kClassRootsMax),
                "Mismatch between class descriptors and class-root enum");

  const char* descriptor = class_roots_descriptors[class_root];
  CHECK(descriptor != nullptr);
  return descriptor;
}

std::size_t ClassLinker::ClassDescriptorHashEquals::operator()(const GcRoot<mirror::Class>& root)
    const {
  std::string temp;
  return ComputeModifiedUtf8Hash(root.Read()->GetDescriptor(&temp));
}

bool ClassLinker::ClassDescriptorHashEquals::operator()(const GcRoot<mirror::Class>& a,
                                                        const GcRoot<mirror::Class>& b) const {
  if (a.Read()->GetClassLoader() != b.Read()->GetClassLoader()) {
    return false;
  }
  std::string temp;
  return a.Read()->DescriptorEquals(b.Read()->GetDescriptor(&temp));
}

std::size_t ClassLinker::ClassDescriptorHashEquals::operator()(
    const std::pair<const char*, mirror::ClassLoader*>& element) const {
  return ComputeModifiedUtf8Hash(element.first);
}

bool ClassLinker::ClassDescriptorHashEquals::operator()(
    const GcRoot<mirror::Class>& a, const std::pair<const char*, mirror::ClassLoader*>& b) const {
  if (a.Read()->GetClassLoader() != b.second) {
    return false;
  }
  return a.Read()->DescriptorEquals(b.first);
}

bool ClassLinker::ClassDescriptorHashEquals::operator()(const GcRoot<mirror::Class>& a,
                                                        const char* descriptor) const {
  return a.Read()->DescriptorEquals(descriptor);
}

std::size_t ClassLinker::ClassDescriptorHashEquals::operator()(const char* descriptor) const {
  return ComputeModifiedUtf8Hash(descriptor);
}

bool ClassLinker::MayBeCalledWithDirectCodePointer(mirror::ArtMethod* m) {
  if (Runtime::Current()->UseJit()) {
    // JIT can have direct code pointers from any method to any other method.
    return true;
  }
  // Non-image methods don't use direct code pointer.
  if (!m->GetDeclaringClass()->IsBootStrapClassLoaded()) {
    return false;
  }
  if (m->IsPrivate()) {
    // The method can only be called inside its own oat file. Therefore it won't be called using
    // its direct code if the oat file has been compiled in PIC mode.
    const DexFile& dex_file = m->GetDeclaringClass()->GetDexFile();
    const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
    if (oat_dex_file == nullptr) {
      // No oat file: the method has not been compiled.
      return false;
    }
    const OatFile* oat_file = oat_dex_file->GetOatFile();
    return oat_file != nullptr && !oat_file->IsPic();
  } else {
    // The method can be called outside its own oat file. Therefore it won't be called using its
    // direct code pointer only if all loaded oat files have been compiled in PIC mode.
    ReaderMutexLock mu(Thread::Current(), dex_lock_);
    for (const OatFile* oat_file : oat_files_) {
      if (!oat_file->IsPic()) {
        return true;
      }
    }
    return false;
  }
}

jobject ClassLinker::CreatePathClassLoader(Thread* self, std::vector<const DexFile*>& dex_files) {
  // SOAAlreadyRunnable is protected, and we need something to add a global reference.
  // We could move the jobject to the callers, but all call-sites do this...
  ScopedObjectAccessUnchecked soa(self);

  // Register the dex files.
  for (const DexFile* dex_file : dex_files) {
    RegisterDexFile(*dex_file);
  }

  // For now, create a libcore-level DexFile for each ART DexFile. This "explodes" multidex.
  StackHandleScope<10> hs(self);

  ArtField* dex_elements_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList_dexElements);

  mirror::Class* dex_elements_class = dex_elements_field->GetType<true>();
  DCHECK(dex_elements_class != nullptr);
  DCHECK(dex_elements_class->IsArrayClass());
  Handle<mirror::ObjectArray<mirror::Object>> h_dex_elements(hs.NewHandle(
      mirror::ObjectArray<mirror::Object>::Alloc(self, dex_elements_class, dex_files.size())));
  Handle<mirror::Class> h_dex_element_class =
      hs.NewHandle(dex_elements_class->GetComponentType());

  ArtField* element_file_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList__Element_dexFile);
  DCHECK_EQ(h_dex_element_class.Get(), element_file_field->GetDeclaringClass());

  ArtField* cookie_field = soa.DecodeField(WellKnownClasses::dalvik_system_DexFile_cookie);
  DCHECK_EQ(cookie_field->GetDeclaringClass(), element_file_field->GetType<false>());

  // Fill the elements array.
  int32_t index = 0;
  for (const DexFile* dex_file : dex_files) {
    StackHandleScope<3> hs2(self);

    Handle<mirror::LongArray> h_long_array = hs2.NewHandle(mirror::LongArray::Alloc(self, 1));
    DCHECK(h_long_array.Get() != nullptr);
    h_long_array->Set(0, reinterpret_cast<intptr_t>(dex_file));

    Handle<mirror::Object> h_dex_file = hs2.NewHandle(
        cookie_field->GetDeclaringClass()->AllocObject(self));
    DCHECK(h_dex_file.Get() != nullptr);
    cookie_field->SetObject<false>(h_dex_file.Get(), h_long_array.Get());

    Handle<mirror::Object> h_element = hs2.NewHandle(h_dex_element_class->AllocObject(self));
    DCHECK(h_element.Get() != nullptr);
    element_file_field->SetObject<false>(h_element.Get(), h_dex_file.Get());

    h_dex_elements->Set(index, h_element.Get());
    index++;
  }
  DCHECK_EQ(index, h_dex_elements->GetLength());

  // Create DexPathList.
  Handle<mirror::Object> h_dex_path_list = hs.NewHandle(
      dex_elements_field->GetDeclaringClass()->AllocObject(self));
  DCHECK(h_dex_path_list.Get() != nullptr);
  // Set elements.
  dex_elements_field->SetObject<false>(h_dex_path_list.Get(), h_dex_elements.Get());

  // Create PathClassLoader.
  Handle<mirror::Class> h_path_class_class = hs.NewHandle(
      soa.Decode<mirror::Class*>(WellKnownClasses::dalvik_system_PathClassLoader));
  Handle<mirror::Object> h_path_class_loader = hs.NewHandle(
      h_path_class_class->AllocObject(self));
  DCHECK(h_path_class_loader.Get() != nullptr);
  // Set DexPathList.
  ArtField* path_list_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_PathClassLoader_pathList);
  DCHECK(path_list_field != nullptr);
  path_list_field->SetObject<false>(h_path_class_loader.Get(), h_dex_path_list.Get());

  // Make a pretend boot-classpath.
  // TODO: Should we scan the image?
  ArtField* const parent_field =
      mirror::Class::FindField(self, hs.NewHandle(h_path_class_loader->GetClass()), "parent",
                               "Ljava/lang/ClassLoader;");
  DCHECK(parent_field!= nullptr);
  mirror::Object* boot_cl =
      soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_BootClassLoader)->AllocObject(self);
  parent_field->SetObject<false>(h_path_class_loader.Get(), boot_cl);

  // Make it a global ref and return.
  ScopedLocalRef<jobject> local_ref(
      soa.Env(), soa.Env()->AddLocalReference<jobject>(h_path_class_loader.Get()));
  return soa.Env()->NewGlobalRef(local_ref.get());
}

}  // namespace art
