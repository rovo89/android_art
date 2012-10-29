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

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "casts.h"
#include "class_loader.h"
#include "debugger.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "heap.h"
#include "intern_table.h"
#include "interpreter/interpreter.h"
#include "leb128.h"
#include "logging.h"
#include "oat_file.h"
#include "object.h"
#include "object_utils.h"
#include "os.h"
#include "runtime.h"
#include "runtime_support.h"
#if defined(ART_USE_LLVM_COMPILER)
#include "compiler_llvm/runtime_support_llvm.h"
#endif
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "sirt_ref.h"
#include "gc/space.h"
#include "gc/space_bitmap.h"
#include "stack_indirect_reference_table.h"
#include "stl_util.h"
#include "thread.h"
#include "UniquePtr.h"
#include "utils.h"
#include "well_known_classes.h"

namespace art {

static void ThrowNoClassDefFoundError(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
static void ThrowNoClassDefFoundError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread::Current()->ThrowNewExceptionV("Ljava/lang/NoClassDefFoundError;", fmt, args);
  va_end(args);
}

static void ThrowClassFormatError(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
static void ThrowClassFormatError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread::Current()->ThrowNewExceptionV("Ljava/lang/ClassFormatError;", fmt, args);
  va_end(args);
}

static void ThrowLinkageError(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
static void ThrowLinkageError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread::Current()->ThrowNewExceptionV("Ljava/lang/LinkageError;", fmt, args);
  va_end(args);
}

static void ThrowNoSuchFieldError(const StringPiece& scope, Class* c, const StringPiece& type,
                                  const StringPiece& name)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ClassHelper kh(c);
  std::ostringstream msg;
  msg << "No " << scope << "field " << name << " of type " << type
      << " in class " << kh.GetDescriptor() << " or its superclasses";
  std::string location(kh.GetLocation());
  if (!location.empty()) {
    msg << " (defined in " << location << ")";
  }
  Thread::Current()->ThrowNewException("Ljava/lang/NoSuchFieldError;", msg.str().c_str());
}

static void ThrowNullPointerException(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
static void ThrowNullPointerException(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread::Current()->ThrowNewExceptionV("Ljava/lang/NullPointerException;", fmt, args);
  va_end(args);
}

static void ThrowEarlierClassFailure(Class* c)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // The class failed to initialize on a previous attempt, so we want to throw
  // a NoClassDefFoundError (v2 2.17.5).  The exception to this rule is if we
  // failed in verification, in which case v2 5.4.1 says we need to re-throw
  // the previous error.
  if (!Runtime::Current()->IsCompiler()) {  // Give info if this occurs at runtime.
    LOG(INFO) << "Rejecting re-init on previously-failed class " << PrettyClass(c);
  }

  CHECK(c->IsErroneous()) << PrettyClass(c) << " " << c->GetStatus();
  if (c->GetVerifyErrorClass() != NULL) {
    // TODO: change the verifier to store an _instance_, with a useful detail message?
    ClassHelper ve_ch(c->GetVerifyErrorClass());
    std::string error_descriptor(ve_ch.GetDescriptor());
    Thread::Current()->ThrowNewException(error_descriptor.c_str(), PrettyDescriptor(c).c_str());
  } else {
    ThrowNoClassDefFoundError("%s", PrettyDescriptor(c).c_str());
  }
}

static void WrapExceptionInInitializer()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  ScopedLocalRef<jthrowable> cause(env, env->ExceptionOccurred());
  CHECK(cause.get() != NULL);

  env->ExceptionClear();
  bool is_error = env->IsInstanceOf(cause.get(), WellKnownClasses::java_lang_Error);
  env->Throw(cause.get());

  // We only wrap non-Error exceptions; an Error can just be used as-is.
  if (!is_error) {
    self->ThrowNewWrappedException("Ljava/lang/ExceptionInInitializerError;", NULL);
  }
}

static size_t Hash(const char* s) {
  // This is the java.lang.String hashcode for convenience, not interoperability.
  size_t hash = 0;
  for (; *s != '\0'; ++s) {
    hash = hash * 31 + *s;
  }
  return hash;
}

const char* ClassLinker::class_roots_descriptors_[] = {
  "Ljava/lang/Class;",
  "Ljava/lang/Object;",
  "[Ljava/lang/Class;",
  "[Ljava/lang/Object;",
  "Ljava/lang/String;",
  "Ljava/lang/DexCache;",
  "Ljava/lang/ref/Reference;",
  "Ljava/lang/reflect/Constructor;",
  "Ljava/lang/reflect/Field;",
  "Ljava/lang/reflect/AbstractMethod;",
  "Ljava/lang/reflect/Method;",
  "Ljava/lang/reflect/Proxy;",
  "[Ljava/lang/String;",
  "[Ljava/lang/reflect/AbstractMethod;",
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

ClassLinker* ClassLinker::CreateFromCompiler(const std::vector<const DexFile*>& boot_class_path,
                                             InternTable* intern_table) {
  CHECK_NE(boot_class_path.size(), 0U);
  UniquePtr<ClassLinker> class_linker(new ClassLinker(intern_table));
  class_linker->InitFromCompiler(boot_class_path);
  return class_linker.release();
}

ClassLinker* ClassLinker::CreateFromImage(InternTable* intern_table) {
  UniquePtr<ClassLinker> class_linker(new ClassLinker(intern_table));
  class_linker->InitFromImage();
  return class_linker.release();
}

ClassLinker::ClassLinker(InternTable* intern_table)
    // dex_lock_ is recursive as it may be used in stack dumping.
    : dex_lock_("ClassLinker dex lock", kDefaultMutexLevel, true),
      class_roots_(NULL),
      array_iftable_(NULL),
      init_done_(false),
      is_dirty_(false),
      intern_table_(intern_table) {
  CHECK_EQ(arraysize(class_roots_descriptors_), size_t(kClassRootsMax));
}

void ClassLinker::InitFromCompiler(const std::vector<const DexFile*>& boot_class_path) {
  VLOG(startup) << "ClassLinker::Init";
  CHECK(Runtime::Current()->IsCompiler());

  CHECK(!init_done_);

  // java_lang_Class comes first, it's needed for AllocClass
  Thread* self = Thread::Current();
  Heap* heap = Runtime::Current()->GetHeap();
  SirtRef<Class>
      java_lang_Class(self, down_cast<Class*>(heap->AllocObject(self, NULL, sizeof(ClassClass))));
  CHECK(java_lang_Class.get() != NULL);
  java_lang_Class->SetClass(java_lang_Class.get());
  java_lang_Class->SetClassSize(sizeof(ClassClass));
  // AllocClass(Class*) can now be used

  // Class[] is used for reflection support.
  SirtRef<Class> class_array_class(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));
  class_array_class->SetComponentType(java_lang_Class.get());

  // java_lang_Object comes next so that object_array_class can be created.
  SirtRef<Class> java_lang_Object(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));
  CHECK(java_lang_Object.get() != NULL);
  // backfill Object as the super class of Class.
  java_lang_Class->SetSuperClass(java_lang_Object.get());
  java_lang_Object->SetStatus(Class::kStatusLoaded);

  // Object[] next to hold class roots.
  SirtRef<Class> object_array_class(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));
  object_array_class->SetComponentType(java_lang_Object.get());

  // Setup the char class to be used for char[].
  SirtRef<Class> char_class(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));

  // Setup the char[] class to be used for String.
  SirtRef<Class> char_array_class(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));
  char_array_class->SetComponentType(char_class.get());
  CharArray::SetArrayClass(char_array_class.get());

  // Setup String.
  SirtRef<Class> java_lang_String(self, AllocClass(self, java_lang_Class.get(), sizeof(StringClass)));
  String::SetClass(java_lang_String.get());
  java_lang_String->SetObjectSize(sizeof(String));
  java_lang_String->SetStatus(Class::kStatusResolved);

  // Create storage for root classes, save away our work so far (requires descriptors).
  class_roots_ = ObjectArray<Class>::Alloc(self, object_array_class.get(), kClassRootsMax);
  CHECK(class_roots_ != NULL);
  SetClassRoot(kJavaLangClass, java_lang_Class.get());
  SetClassRoot(kJavaLangObject, java_lang_Object.get());
  SetClassRoot(kClassArrayClass, class_array_class.get());
  SetClassRoot(kObjectArrayClass, object_array_class.get());
  SetClassRoot(kCharArrayClass, char_array_class.get());
  SetClassRoot(kJavaLangString, java_lang_String.get());

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
  array_iftable_ = AllocIfTable(self, 2);

  // Create int array type for AllocDexCache (done in AppendToBootClassPath).
  SirtRef<Class> int_array_class(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));
  int_array_class->SetComponentType(GetClassRoot(kPrimitiveInt));
  IntArray::SetArrayClass(int_array_class.get());
  SetClassRoot(kIntArrayClass, int_array_class.get());

  // now that these are registered, we can use AllocClass() and AllocObjectArray

  // Set up DexCache. This cannot be done later since AppendToBootClassPath calls AllocDexCache.
  SirtRef<Class>
      java_lang_DexCache(self, AllocClass(self, java_lang_Class.get(), sizeof(DexCacheClass)));
  SetClassRoot(kJavaLangDexCache, java_lang_DexCache.get());
  java_lang_DexCache->SetObjectSize(sizeof(DexCacheClass));
  java_lang_DexCache->SetStatus(Class::kStatusResolved);

  // Constructor, Field, Method, and AbstractMethod are necessary so that FindClass can link members.
  SirtRef<Class> java_lang_reflect_Field(self, AllocClass(self, java_lang_Class.get(),
                                                          sizeof(FieldClass)));
  CHECK(java_lang_reflect_Field.get() != NULL);
  java_lang_reflect_Field->SetObjectSize(sizeof(Field));
  SetClassRoot(kJavaLangReflectField, java_lang_reflect_Field.get());
  java_lang_reflect_Field->SetStatus(Class::kStatusResolved);
  Field::SetClass(java_lang_reflect_Field.get());

  SirtRef<Class> java_lang_reflect_AbstractMethod(self, AllocClass(self, java_lang_Class.get(),
                                                                   sizeof(MethodClass)));
  CHECK(java_lang_reflect_AbstractMethod.get() != NULL);
  java_lang_reflect_AbstractMethod->SetObjectSize(sizeof(AbstractMethod));
  SetClassRoot(kJavaLangReflectAbstractMethod, java_lang_reflect_AbstractMethod.get());
  java_lang_reflect_AbstractMethod->SetStatus(Class::kStatusResolved);

  SirtRef<Class> java_lang_reflect_Constructor(self, AllocClass(self, java_lang_Class.get(),
                                                                sizeof(MethodClass)));
  CHECK(java_lang_reflect_Constructor.get() != NULL);
  java_lang_reflect_Constructor->SetObjectSize(sizeof(Constructor));
  java_lang_reflect_Constructor->SetSuperClass(java_lang_reflect_AbstractMethod.get());
  SetClassRoot(kJavaLangReflectConstructor, java_lang_reflect_Constructor.get());
  java_lang_reflect_Constructor->SetStatus(Class::kStatusResolved);

  SirtRef<Class> java_lang_reflect_Method(self, AllocClass(self, java_lang_Class.get(),
                                                           sizeof(MethodClass)));
  CHECK(java_lang_reflect_Method.get() != NULL);
  java_lang_reflect_Method->SetObjectSize(sizeof(Method));
  java_lang_reflect_Method->SetSuperClass(java_lang_reflect_AbstractMethod.get());
  SetClassRoot(kJavaLangReflectMethod, java_lang_reflect_Method.get());
  java_lang_reflect_Method->SetStatus(Class::kStatusResolved);

  AbstractMethod::SetClasses(java_lang_reflect_Constructor.get(), java_lang_reflect_Method.get());

  // Set up array classes for string, field, method
  SirtRef<Class> object_array_string(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));
  object_array_string->SetComponentType(java_lang_String.get());
  SetClassRoot(kJavaLangStringArrayClass, object_array_string.get());

  SirtRef<Class> object_array_abstract_method(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));
  object_array_abstract_method->SetComponentType(java_lang_reflect_AbstractMethod.get());
  SetClassRoot(kJavaLangReflectAbstractMethodArrayClass, object_array_abstract_method.get());

  SirtRef<Class> object_array_field(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));
  object_array_field->SetComponentType(java_lang_reflect_Field.get());
  SetClassRoot(kJavaLangReflectFieldArrayClass, object_array_field.get());

  SirtRef<Class> object_array_method(self, AllocClass(self, java_lang_Class.get(), sizeof(Class)));
  object_array_method->SetComponentType(java_lang_reflect_Method.get());
  SetClassRoot(kJavaLangReflectMethodArrayClass, object_array_method.get());

  // Setup boot_class_path_ and register class_path now that we can use AllocObjectArray to create
  // DexCache instances. Needs to be after String, Field, Method arrays since AllocDexCache uses
  // these roots.
  CHECK_NE(0U, boot_class_path.size());
  for (size_t i = 0; i != boot_class_path.size(); ++i) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    AppendToBootClassPath(*dex_file);
  }

  // now we can use FindSystemClass

  // run char class through InitializePrimitiveClass to finish init
  InitializePrimitiveClass(char_class.get(), Primitive::kPrimChar);
  SetClassRoot(kPrimitiveChar, char_class.get());  // needs descriptor

  // Object, String and DexCache need to be rerun through FindSystemClass to finish init
  java_lang_Object->SetStatus(Class::kStatusNotReady);
  Class* Object_class = FindSystemClass("Ljava/lang/Object;");
  CHECK_EQ(java_lang_Object.get(), Object_class);
  CHECK_EQ(java_lang_Object->GetObjectSize(), sizeof(Object));
  java_lang_String->SetStatus(Class::kStatusNotReady);
  Class* String_class = FindSystemClass("Ljava/lang/String;");
  CHECK_EQ(java_lang_String.get(), String_class);
  CHECK_EQ(java_lang_String->GetObjectSize(), sizeof(String));
  java_lang_DexCache->SetStatus(Class::kStatusNotReady);
  Class* DexCache_class = FindSystemClass("Ljava/lang/DexCache;");
  CHECK_EQ(java_lang_String.get(), String_class);
  CHECK_EQ(java_lang_DexCache.get(), DexCache_class);
  CHECK_EQ(java_lang_DexCache->GetObjectSize(), sizeof(DexCache));

  // Setup the primitive array type classes - can't be done until Object has a vtable.
  SetClassRoot(kBooleanArrayClass, FindSystemClass("[Z"));
  BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));

  SetClassRoot(kByteArrayClass, FindSystemClass("[B"));
  ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));

  Class* found_char_array_class = FindSystemClass("[C");
  CHECK_EQ(char_array_class.get(), found_char_array_class);

  SetClassRoot(kShortArrayClass, FindSystemClass("[S"));
  ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));

  Class* found_int_array_class = FindSystemClass("[I");
  CHECK_EQ(int_array_class.get(), found_int_array_class);

  SetClassRoot(kLongArrayClass, FindSystemClass("[J"));
  LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));

  SetClassRoot(kFloatArrayClass, FindSystemClass("[F"));
  FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));

  SetClassRoot(kDoubleArrayClass, FindSystemClass("[D"));
  DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));

  Class* found_class_array_class = FindSystemClass("[Ljava/lang/Class;");
  CHECK_EQ(class_array_class.get(), found_class_array_class);

  Class* found_object_array_class = FindSystemClass("[Ljava/lang/Object;");
  CHECK_EQ(object_array_class.get(), found_object_array_class);

  // Setup the single, global copy of "iftable".
  Class* java_lang_Cloneable = FindSystemClass("Ljava/lang/Cloneable;");
  CHECK(java_lang_Cloneable != NULL);
  Class* java_io_Serializable = FindSystemClass("Ljava/io/Serializable;");
  CHECK(java_io_Serializable != NULL);
  // We assume that Cloneable/Serializable don't have superinterfaces -- normally we'd have to
  // crawl up and explicitly list all of the supers as well.
  array_iftable_->SetInterface(0, java_lang_Cloneable);
  array_iftable_->SetInterface(1, java_io_Serializable);

  // Sanity check Class[] and Object[]'s interfaces.
  ClassHelper kh(class_array_class.get(), this);
  CHECK_EQ(java_lang_Cloneable, kh.GetDirectInterface(0));
  CHECK_EQ(java_io_Serializable, kh.GetDirectInterface(1));
  kh.ChangeClass(object_array_class.get());
  CHECK_EQ(java_lang_Cloneable, kh.GetDirectInterface(0));
  CHECK_EQ(java_io_Serializable, kh.GetDirectInterface(1));
  // Run Class, Constructor, Field, and Method through FindSystemClass. This initializes their
  // dex_cache_ fields and register them in classes_.
  Class* Class_class = FindSystemClass("Ljava/lang/Class;");
  CHECK_EQ(java_lang_Class.get(), Class_class);

  java_lang_reflect_AbstractMethod->SetStatus(Class::kStatusNotReady);
  Class* Abstract_method_class = FindSystemClass("Ljava/lang/reflect/AbstractMethod;");
  CHECK_EQ(java_lang_reflect_AbstractMethod.get(), Abstract_method_class);

  // Method extends AbstractMethod so must reset after.
  java_lang_reflect_Method->SetStatus(Class::kStatusNotReady);
  Class* Method_class = FindSystemClass("Ljava/lang/reflect/Method;");
  CHECK_EQ(java_lang_reflect_Method.get(), Method_class);

  // Constructor extends AbstractMethod so must reset after.
  java_lang_reflect_Constructor->SetStatus(Class::kStatusNotReady);
  Class* Constructor_class = FindSystemClass("Ljava/lang/reflect/Constructor;");
  CHECK_EQ(java_lang_reflect_Constructor.get(), Constructor_class);

  java_lang_reflect_Field->SetStatus(Class::kStatusNotReady);
  Class* Field_class = FindSystemClass("Ljava/lang/reflect/Field;");
  CHECK_EQ(java_lang_reflect_Field.get(), Field_class);

  Class* String_array_class = FindSystemClass(class_roots_descriptors_[kJavaLangStringArrayClass]);
  CHECK_EQ(object_array_string.get(), String_array_class);

  Class* Abstract_method_array_class =
      FindSystemClass(class_roots_descriptors_[kJavaLangReflectAbstractMethodArrayClass]);
  CHECK_EQ(object_array_abstract_method.get(), Abstract_method_array_class);

  Class* Field_array_class = FindSystemClass(class_roots_descriptors_[kJavaLangReflectFieldArrayClass]);
  CHECK_EQ(object_array_field.get(), Field_array_class);

  Class* Method_array_class =
      FindSystemClass(class_roots_descriptors_[kJavaLangReflectMethodArrayClass]);
  CHECK_EQ(object_array_method.get(), Method_array_class);

  // End of special init trickery, subsequent classes may be loaded via FindSystemClass.

  // Create java.lang.reflect.Proxy root.
  Class* java_lang_reflect_Proxy = FindSystemClass("Ljava/lang/reflect/Proxy;");
  SetClassRoot(kJavaLangReflectProxy, java_lang_reflect_Proxy);

  // java.lang.ref classes need to be specially flagged, but otherwise are normal classes
  Class* java_lang_ref_Reference = FindSystemClass("Ljava/lang/ref/Reference;");
  SetClassRoot(kJavaLangRefReference, java_lang_ref_Reference);
  Class* java_lang_ref_FinalizerReference = FindSystemClass("Ljava/lang/ref/FinalizerReference;");
  java_lang_ref_FinalizerReference->SetAccessFlags(
      java_lang_ref_FinalizerReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsFinalizerReference);
  Class* java_lang_ref_PhantomReference = FindSystemClass("Ljava/lang/ref/PhantomReference;");
  java_lang_ref_PhantomReference->SetAccessFlags(
      java_lang_ref_PhantomReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsPhantomReference);
  Class* java_lang_ref_SoftReference = FindSystemClass("Ljava/lang/ref/SoftReference;");
  java_lang_ref_SoftReference->SetAccessFlags(
      java_lang_ref_SoftReference->GetAccessFlags() | kAccClassIsReference);
  Class* java_lang_ref_WeakReference = FindSystemClass("Ljava/lang/ref/WeakReference;");
  java_lang_ref_WeakReference->SetAccessFlags(
      java_lang_ref_WeakReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsWeakReference);

  // Setup the ClassLoader, verifying the object_size_.
  Class* java_lang_ClassLoader = FindSystemClass("Ljava/lang/ClassLoader;");
  CHECK_EQ(java_lang_ClassLoader->GetObjectSize(), sizeof(ClassLoader));
  SetClassRoot(kJavaLangClassLoader, java_lang_ClassLoader);

  // Set up java.lang.Throwable, java.lang.ClassNotFoundException, and
  // java.lang.StackTraceElement as a convenience.
  SetClassRoot(kJavaLangThrowable, FindSystemClass("Ljava/lang/Throwable;"));
  Throwable::SetClass(GetClassRoot(kJavaLangThrowable));
  SetClassRoot(kJavaLangClassNotFoundException, FindSystemClass("Ljava/lang/ClassNotFoundException;"));
  SetClassRoot(kJavaLangStackTraceElement, FindSystemClass("Ljava/lang/StackTraceElement;"));
  SetClassRoot(kJavaLangStackTraceElementArrayClass, FindSystemClass("[Ljava/lang/StackTraceElement;"));
  StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit();

  VLOG(startup) << "ClassLinker::InitFromCompiler exiting";
}

void ClassLinker::FinishInit() {
  VLOG(startup) << "ClassLinker::FinishInit entering";

  // Let the heap know some key offsets into java.lang.ref instances
  // Note: we hard code the field indexes here rather than using FindInstanceField
  // as the types of the field can't be resolved prior to the runtime being
  // fully initialized
  Class* java_lang_ref_Reference = GetClassRoot(kJavaLangRefReference);
  Class* java_lang_ref_ReferenceQueue = FindSystemClass("Ljava/lang/ref/ReferenceQueue;");
  Class* java_lang_ref_FinalizerReference = FindSystemClass("Ljava/lang/ref/FinalizerReference;");

  const DexFile& java_lang_dex = *java_lang_ref_Reference->GetDexCache()->GetDexFile();

  Field* pendingNext = java_lang_ref_Reference->GetInstanceField(0);
  FieldHelper fh(pendingNext, this);
  CHECK_STREQ(fh.GetName(), "pendingNext");
  CHECK_EQ(java_lang_dex.GetFieldId(pendingNext->GetDexFieldIndex()).type_idx_,
           java_lang_ref_Reference->GetDexTypeIndex());

  Field* queue = java_lang_ref_Reference->GetInstanceField(1);
  fh.ChangeField(queue);
  CHECK_STREQ(fh.GetName(), "queue");
  CHECK_EQ(java_lang_dex.GetFieldId(queue->GetDexFieldIndex()).type_idx_,
           java_lang_ref_ReferenceQueue->GetDexTypeIndex());

  Field* queueNext = java_lang_ref_Reference->GetInstanceField(2);
  fh.ChangeField(queueNext);
  CHECK_STREQ(fh.GetName(), "queueNext");
  CHECK_EQ(java_lang_dex.GetFieldId(queueNext->GetDexFieldIndex()).type_idx_,
           java_lang_ref_Reference->GetDexTypeIndex());

  Field* referent = java_lang_ref_Reference->GetInstanceField(3);
  fh.ChangeField(referent);
  CHECK_STREQ(fh.GetName(), "referent");
  CHECK_EQ(java_lang_dex.GetFieldId(referent->GetDexFieldIndex()).type_idx_,
           GetClassRoot(kJavaLangObject)->GetDexTypeIndex());

  Field* zombie = java_lang_ref_FinalizerReference->GetInstanceField(2);
  fh.ChangeField(zombie);
  CHECK_STREQ(fh.GetName(), "zombie");
  CHECK_EQ(java_lang_dex.GetFieldId(zombie->GetDexFieldIndex()).type_idx_,
           GetClassRoot(kJavaLangObject)->GetDexTypeIndex());

  Heap* heap = Runtime::Current()->GetHeap();
  heap->SetReferenceOffsets(referent->GetOffset(),
                            queue->GetOffset(),
                            queueNext->GetOffset(),
                            pendingNext->GetOffset(),
                            zombie->GetOffset());

  // ensure all class_roots_ are initialized
  for (size_t i = 0; i < kClassRootsMax; i++) {
    ClassRoot class_root = static_cast<ClassRoot>(i);
    Class* klass = GetClassRoot(class_root);
    CHECK(klass != NULL);
    DCHECK(klass->IsArrayClass() || klass->IsPrimitive() || klass->GetDexCache() != NULL);
    // note SetClassRoot does additional validation.
    // if possible add new checks there to catch errors early
  }

  CHECK(array_iftable_ != NULL);

  // disable the slow paths in FindClass and CreatePrimitiveClass now
  // that Object, Class, and Object[] are setup
  init_done_ = true;

  VLOG(startup) << "ClassLinker::FinishInit exiting";
}

void ClassLinker::RunRootClinits() {
  Thread* self = Thread::Current();
  for (size_t i = 0; i < ClassLinker::kClassRootsMax; ++i) {
    Class* c = GetClassRoot(ClassRoot(i));
    if (!c->IsArrayClass() && !c->IsPrimitive()) {
      EnsureInitialized(GetClassRoot(ClassRoot(i)), true, true);
      self->AssertNoPendingException();
    }
  }
}

bool ClassLinker::GenerateOatFile(const std::string& dex_filename,
                                  int oat_fd,
                                  const std::string& oat_cache_filename) {
  std::string dex2oat_string(GetAndroidRoot());
  dex2oat_string += (kIsDebugBuild ? "/bin/dex2oatd" : "/bin/dex2oat");
  const char* dex2oat = dex2oat_string.c_str();

  const char* class_path = Runtime::Current()->GetClassPathString().c_str();

  Heap* heap = Runtime::Current()->GetHeap();
  std::string boot_image_option_string("--boot-image=");
  boot_image_option_string += heap->GetImageSpace()->GetImageFilename();
  const char* boot_image_option = boot_image_option_string.c_str();

  std::string dex_file_option_string("--dex-file=");
  dex_file_option_string += dex_filename;
  const char* dex_file_option = dex_file_option_string.c_str();

  std::string oat_fd_option_string("--oat-fd=");
  StringAppendF(&oat_fd_option_string, "%d", oat_fd);
  const char* oat_fd_option = oat_fd_option_string.c_str();

  std::string oat_location_option_string("--oat-location=");
  oat_location_option_string += oat_cache_filename;
  const char* oat_location_option = oat_location_option_string.c_str();

  // fork and exec dex2oat
  pid_t pid = fork();
  if (pid == 0) {
    // no allocation allowed between fork and exec

    // change process groups, so we don't get reaped by ProcessManager
    setpgid(0, 0);

    VLOG(class_linker) << dex2oat
                       << " --runtime-arg -Xms64m"
                       << " --runtime-arg -Xmx64m"
                       << " --runtime-arg -classpath"
                       << " --runtime-arg " << class_path
                       << " " << boot_image_option
                       << " " << dex_file_option
                       << " " << oat_fd_option
                       << " " << oat_location_option;

    execl(dex2oat, dex2oat,
          "--runtime-arg", "-Xms64m",
          "--runtime-arg", "-Xmx64m",
          "--runtime-arg", "-classpath",
          "--runtime-arg", class_path,
          boot_image_option,
          dex_file_option,
          oat_fd_option,
          oat_location_option,
          NULL);

    PLOG(FATAL) << "execl(" << dex2oat << ") failed";
    return false;
  } else {
    // wait for dex2oat to finish
    int status;
    pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
    if (got_pid != pid) {
      PLOG(ERROR) << "waitpid failed: wanted " << pid << ", got " << got_pid;
      return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      LOG(ERROR) << dex2oat << " failed with dex-file=" << dex_filename;
      return false;
    }
  }
  return true;
}

void ClassLinker::RegisterOatFile(const OatFile& oat_file) {
  MutexLock mu(Thread::Current(), dex_lock_);
  RegisterOatFileLocked(oat_file);
}

void ClassLinker::RegisterOatFileLocked(const OatFile& oat_file) {
  dex_lock_.AssertHeld(Thread::Current());
  oat_files_.push_back(&oat_file);
}

OatFile* ClassLinker::OpenOat(const ImageSpace* space) {
  MutexLock mu(Thread::Current(), dex_lock_);
  const Runtime* runtime = Runtime::Current();
  const ImageHeader& image_header = space->GetImageHeader();
  // Grab location but don't use Object::AsString as we haven't yet initialized the roots to
  // check the down cast
  String* oat_location = down_cast<String*>(image_header.GetImageRoot(ImageHeader::kOatLocation));
  std::string oat_filename;
  oat_filename += runtime->GetHostPrefix();
  oat_filename += oat_location->ToModifiedUtf8();
  runtime->GetHeap()->UnReserveOatFileAddressRange();
  OatFile* oat_file = OatFile::Open(oat_filename, oat_filename,
                                    image_header.GetOatBegin(),
                                    OatFile::kRelocNone);
  VLOG(startup) << "ClassLinker::OpenOat entering oat_filename=" << oat_filename;
  if (oat_file == NULL) {
    LOG(ERROR) << "Failed to open oat file " << oat_filename << " referenced from image.";
    return NULL;
  }
  uint32_t oat_checksum = oat_file->GetOatHeader().GetChecksum();
  uint32_t image_oat_checksum = image_header.GetOatChecksum();
  if (oat_checksum != image_oat_checksum) {
    LOG(ERROR) << "Failed to match oat file checksum " << std::hex << oat_checksum
               << " to expected oat checksum " << std::hex << image_oat_checksum
               << " in image";
    return NULL;
  }
  RegisterOatFileLocked(*oat_file);
  VLOG(startup) << "ClassLinker::OpenOat exiting";
  return oat_file;
}

const OatFile* ClassLinker::FindOpenedOatFileForDexFile(const DexFile& dex_file) {
  MutexLock mu(Thread::Current(), dex_lock_);
  return FindOpenedOatFileFromDexLocation(dex_file.GetLocation());
}

const OatFile* ClassLinker::FindOpenedOatFileFromDexLocation(const std::string& dex_location) {
  for (size_t i = 0; i < oat_files_.size(); i++) {
    const OatFile* oat_file = oat_files_[i];
    DCHECK(oat_file != NULL);
    const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location, false);
    if (oat_dex_file != NULL) {
      return oat_file;
    }
  }
  return NULL;
}

static const DexFile* FindDexFileInOatLocation(const std::string& dex_location,
                                               uint32_t dex_location_checksum,
                                               const std::string& oat_location) {
  UniquePtr<OatFile> oat_file(
      OatFile::Open(oat_location, oat_location, NULL, OatFile::kRelocAll));
  if (oat_file.get() == NULL) {
    return NULL;
  }
  Runtime* runtime = Runtime::Current();
  const ImageHeader& image_header = runtime->GetHeap()->GetImageSpace()->GetImageHeader();
  if (oat_file->GetOatHeader().GetImageFileLocationOatChecksum() != image_header.GetOatChecksum()) {
    return NULL;
  }
  if (oat_file->GetOatHeader().GetImageFileLocationOatBegin()
      != reinterpret_cast<uint32_t>(image_header.GetOatBegin())) {
    return NULL;
  }
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location);
  if (oat_dex_file == NULL) {
    return NULL;
  }
  if (oat_dex_file->GetDexFileLocationChecksum() != dex_location_checksum) {
    return NULL;
  }
  runtime->GetClassLinker()->RegisterOatFile(*oat_file.release());
  return oat_dex_file->OpenDexFile();
}

const DexFile* ClassLinker::FindOrCreateOatFileForDexLocation(const std::string& dex_location,
                                                              const std::string& oat_location) {
  MutexLock mu(Thread::Current(), dex_lock_);
  return FindOrCreateOatFileForDexLocationLocked(dex_location, oat_location);
}

const DexFile* ClassLinker::FindOrCreateOatFileForDexLocationLocked(const std::string& dex_location,
                                                                    const std::string& oat_location) {
  uint32_t dex_location_checksum;
  if (!DexFile::GetChecksum(dex_location, dex_location_checksum)) {
    LOG(ERROR) << "Failed to compute checksum '" << dex_location << "'";
    return NULL;
  }

  // Check if we already have an up-to-date output file
  const DexFile* dex_file = FindDexFileInOatLocation(dex_location,
                                                     dex_location_checksum,
                                                     oat_location);
  if (dex_file != NULL) {
    return dex_file;
  }

  // Generate the output oat file for the dex file
  UniquePtr<File> file(OS::OpenFile(oat_location.c_str(), true));
  if (file.get() == NULL) {
    LOG(ERROR) << "Failed to create oat file: " << oat_location;
    return NULL;
  }
  if (!GenerateOatFile(dex_location, file->Fd(), oat_location)) {
    LOG(ERROR) << "Failed to generate oat file: " << oat_location;
    return NULL;
  }
  // Open the oat from file descriptor we passed to GenerateOatFile
  if (lseek(file->Fd(), 0, SEEK_SET) != 0) {
    LOG(ERROR) << "Failed to seek to start of generated oat file: " << oat_location;
    return NULL;
  }
  const OatFile* oat_file =
      OatFile::Open(*file.get(), oat_location, NULL, OatFile::kRelocAll);
  if (oat_file == NULL) {
    LOG(ERROR) << "Failed to open generated oat file: " << oat_location;
    return NULL;
  }
  RegisterOatFileLocked(*oat_file);
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location);
  if (oat_dex_file == NULL) {
    LOG(ERROR) << "Failed to find dex file in generated oat file: " << oat_location;
    return NULL;
  }
  return oat_dex_file->OpenDexFile();
}

bool ClassLinker::VerifyOatFileChecksums(const OatFile* oat_file,
                                         const std::string& dex_location,
                                         uint32_t dex_location_checksum) {
  Runtime* runtime = Runtime::Current();
  const ImageHeader& image_header = runtime->GetHeap()->GetImageSpace()->GetImageHeader();
  uint32_t image_oat_checksum = image_header.GetOatChecksum();
  uint32_t image_oat_begin = reinterpret_cast<uint32_t>(image_header.GetOatBegin());
  bool image_check = ((oat_file->GetOatHeader().GetImageFileLocationOatChecksum() == image_oat_checksum)
                      && (oat_file->GetOatHeader().GetImageFileLocationOatBegin() == image_oat_begin));

  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location);
  if (oat_dex_file == NULL) {
    LOG(ERROR) << ".oat file " << oat_file->GetLocation()
               << " does not contain contents for " << dex_location;
    std::vector<const OatFile::OatDexFile*> oat_dex_files = oat_file->GetOatDexFiles();
    for (size_t i = 0; i < oat_dex_files.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files[i];
      LOG(ERROR) << ".oat file " << oat_file->GetLocation()
                 << " contains contents for " << oat_dex_file->GetDexFileLocation();
    }
    return false;
  }
  bool dex_check = dex_location_checksum == oat_dex_file->GetDexFileLocationChecksum();

  if (image_check && dex_check) {
    return true;
  }

  if (!image_check) {
    std::string image_file(image_header.GetImageRoot(
        ImageHeader::kOatLocation)->AsString()->ToModifiedUtf8());
    LOG(WARNING) << ".oat file " << oat_file->GetLocation()
                 << " mismatch ( " << std::hex << oat_file->GetOatHeader().GetImageFileLocationOatChecksum()
                 << ", " << oat_file->GetOatHeader().GetImageFileLocationOatBegin()
                 << ") with " << image_file
                 << " (" << image_oat_checksum << ", " << std::hex << image_oat_begin << ")";
  }
  if (!dex_check) {
    LOG(WARNING) << ".oat file " << oat_file->GetLocation()
                 << " mismatch ( " << std::hex << oat_dex_file->GetDexFileLocationChecksum()
                 << ") with " << dex_location
                 << " (" << std::hex << dex_location_checksum << ")";
  }
  return false;
}

const DexFile* ClassLinker::VerifyAndOpenDexFileFromOatFile(const OatFile* oat_file,
                                                            const std::string& dex_location,
                                                            uint32_t dex_location_checksum) {
  bool verified = VerifyOatFileChecksums(oat_file, dex_location, dex_location_checksum);
  if (!verified) {
    return NULL;
  }
  RegisterOatFileLocked(*oat_file);
  return oat_file->GetOatDexFile(dex_location)->OpenDexFile();
}

const DexFile* ClassLinker::FindDexFileInOatFileFromDexLocation(const std::string& dex_location) {
  MutexLock mu(Thread::Current(), dex_lock_);

  const OatFile* open_oat_file = FindOpenedOatFileFromDexLocation(dex_location);
  if (open_oat_file != NULL) {
    return open_oat_file->GetOatDexFile(dex_location)->OpenDexFile();
  }

  // Look for an existing file next to dex. for example, for
  // /foo/bar/baz.jar, look for /foo/bar/baz.jar.oat.
  std::string oat_filename(OatFile::DexFilenameToOatFilename(dex_location));
  const OatFile* oat_file = FindOatFileFromOatLocationLocked(oat_filename);
  if (oat_file != NULL) {
    uint32_t dex_location_checksum;
    if (!DexFile::GetChecksum(dex_location, dex_location_checksum)) {
      // If no classes.dex found in dex_location, it has been stripped, assume oat is up-to-date.
      // This is the common case in user builds for jar's and apk's in the /system directory.
      const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location);
      CHECK(oat_dex_file != NULL) << oat_filename << " " << dex_location;
      RegisterOatFileLocked(*oat_file);
      return oat_dex_file->OpenDexFile();
    }
    const DexFile* dex_file = VerifyAndOpenDexFileFromOatFile(oat_file,
                                                              dex_location,
                                                              dex_location_checksum);
    if (dex_file != NULL) {
      return dex_file;
    }
  }
  // Look for an existing file in the art-cache, validating the result if found
  // not found in /foo/bar/baz.oat? try /data/art-cache/foo@bar@baz.oat
  std::string cache_location(GetArtCacheFilenameOrDie(oat_filename));
  oat_file = FindOatFileFromOatLocationLocked(cache_location);
  if (oat_file != NULL) {
    uint32_t dex_location_checksum;
    if (!DexFile::GetChecksum(dex_location, dex_location_checksum)) {
      LOG(WARNING) << "Failed to compute checksum: " << dex_location;
      return NULL;
    }
    const DexFile* dex_file = VerifyAndOpenDexFileFromOatFile(oat_file,
                                                              dex_location,
                                                              dex_location_checksum);
    if (dex_file != NULL) {
      return dex_file;
    }
    if (TEMP_FAILURE_RETRY(unlink(oat_file->GetLocation().c_str())) != 0) {
      PLOG(FATAL) << "Failed to remove obsolete .oat file " << oat_file->GetLocation();
    }
  }
  LOG(INFO) << "Failed to open oat file from " << oat_filename << " or " << cache_location << ".";

  // Try to generate oat file if it wasn't found or was obsolete.
  std::string oat_cache_filename(GetArtCacheFilenameOrDie(oat_filename));
  return FindOrCreateOatFileForDexLocationLocked(dex_location, oat_cache_filename);
}

const OatFile* ClassLinker::FindOpenedOatFileFromOatLocation(const std::string& oat_location) {
  for (size_t i = 0; i < oat_files_.size(); i++) {
    const OatFile* oat_file = oat_files_[i];
    DCHECK(oat_file != NULL);
    if (oat_file->GetLocation() == oat_location) {
      return oat_file;
    }
  }
  return NULL;
}

const OatFile* ClassLinker::FindOatFileFromOatLocation(const std::string& oat_location) {
  MutexLock mu(Thread::Current(), dex_lock_);
  return FindOatFileFromOatLocationLocked(oat_location);
}

const OatFile* ClassLinker::FindOatFileFromOatLocationLocked(const std::string& oat_location) {
  const OatFile* oat_file = FindOpenedOatFileFromOatLocation(oat_location);
  if (oat_file != NULL) {
    return oat_file;
  }

  oat_file = OatFile::Open(oat_location, oat_location, NULL,
                           OatFile::kRelocAll);
  if (oat_file == NULL) {
    return NULL;
  }
  CHECK(oat_file != NULL) << oat_location;
  return oat_file;
}

void ClassLinker::InitFromImage() {
  VLOG(startup) << "ClassLinker::InitFromImage entering";
  CHECK(!init_done_);

  Heap* heap = Runtime::Current()->GetHeap();
  ImageSpace* space = heap->GetImageSpace();
  OatFile* oat_file = OpenOat(space);
  CHECK(oat_file != NULL) << "Failed to open oat file for image";
  CHECK_EQ(oat_file->GetOatHeader().GetImageFileLocationOatChecksum(), 0U);
  CHECK_EQ(oat_file->GetOatHeader().GetImageFileLocationOatBegin(), 0U);
  CHECK(oat_file->GetOatHeader().GetImageFileLocation().empty());
  Object* dex_caches_object = space->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
  ObjectArray<DexCache>* dex_caches = dex_caches_object->AsObjectArray<DexCache>();

  // Special case of setting up the String class early so that we can test arbitrary objects
  // as being Strings or not
  Class* java_lang_String = space->GetImageHeader().GetImageRoot(ImageHeader::kClassRoots)
      ->AsObjectArray<Class>()->Get(kJavaLangString);
  String::SetClass(java_lang_String);

  CHECK_EQ(oat_file->GetOatHeader().GetDexFileCount(),
           static_cast<uint32_t>(dex_caches->GetLength()));
  Thread* self = Thread::Current();
  for (int i = 0; i < dex_caches->GetLength(); i++) {
    SirtRef<DexCache> dex_cache(self, dex_caches->Get(i));
    const std::string& dex_file_location(dex_cache->GetLocation()->ToModifiedUtf8());
    const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file_location);
    const DexFile* dex_file = oat_dex_file->OpenDexFile();
    if (dex_file == NULL) {
      LOG(FATAL) << "Failed to open dex file " << dex_file_location
                 << " from within oat file " << oat_file->GetLocation();
    }

    CHECK_EQ(dex_file->GetLocationChecksum(), oat_dex_file->GetDexFileLocationChecksum());

    AppendToBootClassPath(*dex_file, dex_cache);
  }

  // reinit clases_ table
  {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    heap->FlushAllocStack();
    heap->GetLiveBitmap()->Walk(InitFromImageCallback, this);
  }

  // reinit class_roots_
  Object* class_roots_object =
      heap->GetImageSpace()->GetImageHeader().GetImageRoot(ImageHeader::kClassRoots);
  class_roots_ = class_roots_object->AsObjectArray<Class>();

  // reinit array_iftable_ from any array class instance, they should be ==
  array_iftable_ = GetClassRoot(kObjectArrayClass)->GetIfTable();
  DCHECK(array_iftable_ == GetClassRoot(kBooleanArrayClass)->GetIfTable());
  // String class root was set above
  Field::SetClass(GetClassRoot(kJavaLangReflectField));
  AbstractMethod::SetClasses(GetClassRoot(kJavaLangReflectConstructor),
                             GetClassRoot(kJavaLangReflectMethod));
  BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));
  ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));
  CharArray::SetArrayClass(GetClassRoot(kCharArrayClass));
  DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));
  FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));
  IntArray::SetArrayClass(GetClassRoot(kIntArrayClass));
  LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));
  ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));
  Throwable::SetClass(GetClassRoot(kJavaLangThrowable));
  StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit();

  VLOG(startup) << "ClassLinker::InitFromImage exiting";
}

void ClassLinker::InitFromImageCallback(Object* obj, void* arg) {
  DCHECK(obj != NULL);
  DCHECK(arg != NULL);
  ClassLinker* class_linker = reinterpret_cast<ClassLinker*>(arg);

  if (obj->GetClass()->IsStringClass()) {
    class_linker->intern_table_->RegisterStrong(obj->AsString());
    return;
  }
  if (obj->IsClass()) {
    // restore class to ClassLinker::classes_ table
    Class* klass = obj->AsClass();
    ClassHelper kh(klass, class_linker);
    Class* existing = class_linker->InsertClass(kh.GetDescriptor(), klass, true);
    DCHECK(existing == NULL) << kh.GetDescriptor();
    return;
  }
}

// Keep in sync with InitCallback. Anything we visit, we need to
// reinit references to when reinitializing a ClassLinker from a
// mapped image.
void ClassLinker::VisitRoots(Heap::RootVisitor* visitor, void* arg) {
  visitor(class_roots_, arg);
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, dex_lock_);
    for (size_t i = 0; i < dex_caches_.size(); i++) {
      visitor(dex_caches_[i], arg);
    }
  }

  {
    MutexLock mu(self, *Locks::classlinker_classes_lock_);
    typedef Table::const_iterator It;  // TODO: C++0x auto
    for (It it = classes_.begin(), end = classes_.end(); it != end; ++it) {
      visitor(it->second, arg);
    }

    // We deliberately ignore the class roots in the image since we
    // handle image roots by using the MS/CMS rescanning of dirty cards.
  }

  visitor(array_iftable_, arg);
  is_dirty_ = false;
}

void ClassLinker::VisitClasses(ClassVisitor* visitor, void* arg) const {
  MutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  typedef Table::const_iterator It;  // TODO: C++0x auto
  for (It it = classes_.begin(), end = classes_.end(); it != end; ++it) {
    if (!visitor(it->second, arg)) {
      return;
    }
  }
  for (It it = image_classes_.begin(), end = image_classes_.end(); it != end; ++it) {
    if (!visitor(it->second, arg)) {
      return;
    }
  }
}

static bool GetClassesVisitor(Class* c, void* arg) {
  std::set<Class*>* classes = reinterpret_cast<std::set<Class*>*>(arg);
  classes->insert(c);
  return true;
}

void ClassLinker::VisitClassesWithoutClassesLock(ClassVisitor* visitor, void* arg) const {
  std::set<Class*> classes;
  VisitClasses(GetClassesVisitor, &classes);
  typedef std::set<Class*>::const_iterator It;  // TODO: C++0x auto
  for (It it = classes.begin(), end = classes.end(); it != end; ++it) {
    if (!visitor(*it, arg)) {
      return;
    }
  }
}


ClassLinker::~ClassLinker() {
  String::ResetClass();
  Field::ResetClass();
  AbstractMethod::ResetClasses();
  BooleanArray::ResetArrayClass();
  ByteArray::ResetArrayClass();
  CharArray::ResetArrayClass();
  DoubleArray::ResetArrayClass();
  FloatArray::ResetArrayClass();
  IntArray::ResetArrayClass();
  LongArray::ResetArrayClass();
  ShortArray::ResetArrayClass();
  Throwable::ResetClass();
  StackTraceElement::ResetClass();
  STLDeleteElements(&boot_class_path_);
  STLDeleteElements(&oat_files_);
}

DexCache* ClassLinker::AllocDexCache(Thread* self, const DexFile& dex_file) {
  Heap* heap = Runtime::Current()->GetHeap();
  Class* dex_cache_class = GetClassRoot(kJavaLangDexCache);
  SirtRef<DexCache> dex_cache(self,
                              down_cast<DexCache*>(heap->AllocObject(self, dex_cache_class,
                              dex_cache_class->GetObjectSize())));
  if (dex_cache.get() == NULL) {
    return NULL;
  }
  SirtRef<String> location(self, intern_table_->InternStrong(dex_file.GetLocation().c_str()));
  if (location.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<String> > strings(self, AllocStringArray(self, dex_file.NumStringIds()));
  if (strings.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<Class> > types(self, AllocClassArray(self, dex_file.NumTypeIds()));
  if (types.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<AbstractMethod> >
      methods(self, AllocAbstractMethodArray(self, dex_file.NumMethodIds()));
  if (methods.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<Field> > fields(self, AllocFieldArray(self, dex_file.NumFieldIds()));
  if (fields.get() == NULL) {
    return NULL;
  }
  SirtRef<ObjectArray<StaticStorageBase> >
      initialized_static_storage(self,
                                 AllocObjectArray<StaticStorageBase>(self, dex_file.NumTypeIds()));
  if (initialized_static_storage.get() == NULL) {
    return NULL;
  }

  dex_cache->Init(&dex_file,
                  location.get(),
                  strings.get(),
                  types.get(),
                  methods.get(),
                  fields.get(),
                  initialized_static_storage.get());
  return dex_cache.get();
}

Class* ClassLinker::AllocClass(Thread* self, Class* java_lang_Class, size_t class_size) {
  DCHECK_GE(class_size, sizeof(Class));
  Heap* heap = Runtime::Current()->GetHeap();
  SirtRef<Class> klass(self,
                       heap->AllocObject(self, java_lang_Class, class_size)->AsClass());
  klass->SetPrimitiveType(Primitive::kPrimNot);  // default to not being primitive
  klass->SetClassSize(class_size);
  return klass.get();
}

Class* ClassLinker::AllocClass(Thread* self, size_t class_size) {
  return AllocClass(self, GetClassRoot(kJavaLangClass), class_size);
}

Field* ClassLinker::AllocField(Thread* self) {
  return down_cast<Field*>(GetClassRoot(kJavaLangReflectField)->AllocObject(self));
}

Method* ClassLinker::AllocMethod(Thread* self) {
  return down_cast<Method*>(GetClassRoot(kJavaLangReflectMethod)->AllocObject(self));
}

Constructor* ClassLinker::AllocConstructor(Thread* self) {
  return down_cast<Constructor*>(GetClassRoot(kJavaLangReflectConstructor)->AllocObject(self));
}

ObjectArray<StackTraceElement>* ClassLinker::AllocStackTraceElementArray(Thread* self,
                                                                         size_t length) {
  return ObjectArray<StackTraceElement>::Alloc(self,
                                               GetClassRoot(kJavaLangStackTraceElementArrayClass),
                                               length);
}

static Class* EnsureResolved(Thread* self, Class* klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(klass != NULL);
  // Wait for the class if it has not already been linked.
  if (!klass->IsResolved() && !klass->IsErroneous()) {
    ObjectLock lock(self, klass);
    // Check for circular dependencies between classes.
    if (!klass->IsResolved() && klass->GetClinitThreadId() == self->GetTid()) {
      self->ThrowNewException("Ljava/lang/ClassCircularityError;",
          PrettyDescriptor(klass).c_str());
      klass->SetStatus(Class::kStatusError);
      return NULL;
    }
    // Wait for the pending initialization to complete.
    while (!klass->IsResolved() && !klass->IsErroneous()) {
      lock.Wait();
    }
  }
  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass);
    return NULL;
  }
  // Return the loaded class.  No exceptions should be pending.
  CHECK(klass->IsResolved()) << PrettyClass(klass);
  CHECK(!self->IsExceptionPending())
      << PrettyClass(klass) << " " << PrettyTypeOf(self->GetException()) << "\n"
      << self->GetException()->Dump();
  return klass;
}

Class* ClassLinker::FindSystemClass(const char* descriptor) {
  return FindClass(descriptor, NULL);
}

Class* ClassLinker::FindClass(const char* descriptor, ClassLoader* class_loader) {
  DCHECK_NE(*descriptor, '\0') << "descriptor is empty string";
  Thread* self = Thread::Current();
  DCHECK(self != NULL);
  self->AssertNoPendingException();
  if (descriptor[1] == '\0') {
    // only the descriptors of primitive types should be 1 character long, also avoid class lookup
    // for primitive classes that aren't backed by dex files.
    return FindPrimitiveClass(descriptor[0]);
  }
  // Find the class in the loaded classes table.
  Class* klass = LookupClass(descriptor, class_loader);
  if (klass != NULL) {
    return EnsureResolved(self, klass);
  }
  // Class is not yet loaded.
  if (descriptor[0] == '[') {
    return CreateArrayClass(descriptor, class_loader);

  } else if (class_loader == NULL) {
    DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, boot_class_path_);
    if (pair.second != NULL) {
      return DefineClass(descriptor, NULL, *pair.first, *pair.second);
    }

  } else if (Runtime::Current()->UseCompileTimeClassPath()) {
    // first try the boot class path
    Class* system_class = FindSystemClass(descriptor);
    if (system_class != NULL) {
      return system_class;
    }
    CHECK(self->IsExceptionPending());
    self->ClearException();

    // next try the compile time class path
    const std::vector<const DexFile*>* class_path;
    {
      ScopedObjectAccessUnchecked soa(Thread::Current());
      ScopedLocalRef<jobject> jclass_loader(soa.Env(), soa.AddLocalReference<jobject>(class_loader));
      class_path = &Runtime::Current()->GetCompileTimeClassPath(jclass_loader.get());
    }

    DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, *class_path);
    if (pair.second != NULL) {
      return DefineClass(descriptor, class_loader, *pair.first, *pair.second);
    }

  } else {
    ScopedObjectAccessUnchecked soa(self->GetJniEnv());
    ScopedLocalRef<jobject> class_loader_object(soa.Env(),
                                                soa.AddLocalReference<jobject>(class_loader));
    std::string class_name_string(DescriptorToDot(descriptor));
    ScopedLocalRef<jobject> result(soa.Env(), NULL);
    {
      ScopedThreadStateChange tsc(self, kNative);
      ScopedLocalRef<jobject> class_name_object(soa.Env(),
                                                soa.Env()->NewStringUTF(class_name_string.c_str()));
      if (class_name_object.get() == NULL) {
        return NULL;
      }
      CHECK(class_loader_object.get() != NULL);
      result.reset(soa.Env()->CallObjectMethod(class_loader_object.get(),
                                               WellKnownClasses::java_lang_ClassLoader_loadClass,
                                               class_name_object.get()));
    }
    if (soa.Env()->ExceptionCheck()) {
      // If the ClassLoader threw, pass that exception up.
      return NULL;
    } else if (result.get() == NULL) {
      // broken loader - throw NPE to be compatible with Dalvik
      ThrowNullPointerException("ClassLoader.loadClass returned null for %s",
                                class_name_string.c_str());
      return NULL;
    } else {
      // success, return Class*
      return soa.Decode<Class*>(result.get());
    }
  }

  ThrowNoClassDefFoundError("Class %s not found", PrintableString(descriptor).c_str());
  return NULL;
}

Class* ClassLinker::DefineClass(const StringPiece& descriptor,
                                ClassLoader* class_loader,
                                const DexFile& dex_file,
                                const DexFile::ClassDef& dex_class_def) {
  Thread* self = Thread::Current();
  SirtRef<Class> klass(self, NULL);
  // Load the class from the dex file.
  if (!init_done_) {
    // finish up init of hand crafted class_roots_
    if (descriptor == "Ljava/lang/Object;") {
      klass.reset(GetClassRoot(kJavaLangObject));
    } else if (descriptor == "Ljava/lang/Class;") {
      klass.reset(GetClassRoot(kJavaLangClass));
    } else if (descriptor == "Ljava/lang/String;") {
      klass.reset(GetClassRoot(kJavaLangString));
    } else if (descriptor == "Ljava/lang/DexCache;") {
      klass.reset(GetClassRoot(kJavaLangDexCache));
    } else if (descriptor == "Ljava/lang/reflect/Field;") {
      klass.reset(GetClassRoot(kJavaLangReflectField));
    } else if (descriptor == "Ljava/lang/reflect/AbstractMethod;") {
      klass.reset(GetClassRoot(kJavaLangReflectAbstractMethod));
    } else if (descriptor == "Ljava/lang/reflect/Constructor;") {
      klass.reset(GetClassRoot(kJavaLangReflectConstructor));
    } else if (descriptor == "Ljava/lang/reflect/Method;") {
      klass.reset(GetClassRoot(kJavaLangReflectMethod));
    } else {
      klass.reset(AllocClass(self, SizeOfClass(dex_file, dex_class_def)));
    }
  } else {
    klass.reset(AllocClass(self, SizeOfClass(dex_file, dex_class_def)));
  }
  klass->SetDexCache(FindDexCache(dex_file));
  LoadClass(dex_file, dex_class_def, klass, class_loader);
  // Check for a pending exception during load
  if (self->IsExceptionPending()) {
    klass->SetStatus(Class::kStatusError);
    return NULL;
  }
  ObjectLock lock(self, klass.get());
  klass->SetClinitThreadId(self->GetTid());
  // Add the newly loaded class to the loaded classes table.
  SirtRef<Class> existing(self, InsertClass(descriptor, klass.get(), false));
  if (existing.get() != NULL) {
    // We failed to insert because we raced with another thread.
    return EnsureResolved(self, existing.get());
  }
  // Finish loading (if necessary) by finding parents
  CHECK(!klass->IsLoaded());
  if (!LoadSuperAndInterfaces(klass, dex_file)) {
    // Loading failed.
    klass->SetStatus(Class::kStatusError);
    lock.NotifyAll();
    return NULL;
  }
  CHECK(klass->IsLoaded());
  // Link the class (if necessary)
  CHECK(!klass->IsResolved());
  if (!LinkClass(klass, NULL)) {
    // Linking failed.
    klass->SetStatus(Class::kStatusError);
    lock.NotifyAll();
    return NULL;
  }
  CHECK(klass->IsResolved());

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
  Dbg::PostClassPrepare(klass.get());

  return klass.get();
}

// Precomputes size that will be needed for Class, matching LinkStaticFields
size_t ClassLinker::SizeOfClass(const DexFile& dex_file,
                                const DexFile::ClassDef& dex_class_def) {
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  size_t num_ref = 0;
  size_t num_32 = 0;
  size_t num_64 = 0;
  if (class_data != NULL) {
    for (ClassDataItemIterator it(dex_file, class_data); it.HasNextStaticField(); it.Next()) {
      const DexFile::FieldId& field_id = dex_file.GetFieldId(it.GetMemberIndex());
      const char* descriptor = dex_file.GetFieldTypeDescriptor(field_id);
      char c = descriptor[0];
      if (c == 'L' || c == '[') {
        num_ref++;
      } else if (c == 'J' || c == 'D') {
        num_64++;
      } else {
        num_32++;
      }
    }
  }
  // start with generic class data
  size_t size = sizeof(Class);
  // follow with reference fields which must be contiguous at start
  size += (num_ref * sizeof(uint32_t));
  // if there are 64-bit fields to add, make sure they are aligned
  if (num_64 != 0 && size != RoundUp(size, 8)) { // for 64-bit alignment
    if (num_32 != 0) {
      // use an available 32-bit field for padding
      num_32--;
    }
    size += sizeof(uint32_t);  // either way, we are adding a word
    DCHECK_EQ(size, RoundUp(size, 8));
  }
  // tack on any 64-bit fields now that alignment is assured
  size += (num_64 * sizeof(uint64_t));
  // tack on any remaining 32-bit fields
  size += (num_32 * sizeof(uint32_t));
  return size;
}

const OatFile::OatClass* ClassLinker::GetOatClass(const DexFile& dex_file, const char* descriptor) {
  DCHECK(descriptor != NULL);
  const OatFile* oat_file = FindOpenedOatFileForDexFile(dex_file);
  CHECK(oat_file != NULL) << dex_file.GetLocation() << " " << descriptor;
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation());
  CHECK(oat_dex_file != NULL) << dex_file.GetLocation() << " " << descriptor;
  uint32_t class_def_index;
  bool found = dex_file.FindClassDefIndex(descriptor, class_def_index);
  CHECK(found) << dex_file.GetLocation() << " " << descriptor;
  const OatFile::OatClass* oat_class = oat_dex_file->GetOatClass(class_def_index);
  CHECK(oat_class != NULL) << dex_file.GetLocation() << " " << descriptor;
  return oat_class;
}

static uint32_t GetOatMethodIndexFromMethodIndex(const DexFile& dex_file, uint32_t method_idx) {
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  const DexFile::TypeId& type_id = dex_file.GetTypeId(method_id.class_idx_);
  const DexFile::ClassDef* class_def = dex_file.FindClassDef(dex_file.GetTypeDescriptor(type_id));
  CHECK(class_def != NULL);
  const byte* class_data = dex_file.GetClassData(*class_def);
  CHECK(class_data != NULL);
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

const OatFile::OatMethod ClassLinker::GetOatMethodFor(const AbstractMethod* method) {
  // Although we overwrite the trampoline of non-static methods, we may get here via the resolution
  // method for direct methods (or virtual methods made direct).
  Class* declaring_class = method->GetDeclaringClass();
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
      if (declaring_class->GetVirtualMethod(i) == method) {
        found = true;
        break;
      }
      oat_method_index++;
    }
    CHECK(found) << "Didn't find oat method index for virtual method: " << PrettyMethod(method);
  }
  ClassHelper kh(declaring_class);
  UniquePtr<const OatFile::OatClass> oat_class(GetOatClass(kh.GetDexFile(), kh.GetDescriptor()));
  CHECK(oat_class.get() != NULL);
  DCHECK_EQ(oat_method_index,
            GetOatMethodIndexFromMethodIndex(*declaring_class->GetDexCache()->GetDexFile(),
                                             method->GetDexMethodIndex()));

  return oat_class->GetOatMethod(oat_method_index);
}

// Special case to get oat code without overwriting a trampoline.
const void* ClassLinker::GetOatCodeFor(const AbstractMethod* method) {
  CHECK(Runtime::Current()->IsCompiler() || method->GetDeclaringClass()->IsInitializing());
  return GetOatMethodFor(method).GetCode();
}

const void* ClassLinker::GetOatCodeFor(const DexFile& dex_file, uint32_t method_idx) {
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  const char* descriptor = dex_file.GetTypeDescriptor(dex_file.GetTypeId(method_id.class_idx_));
  uint32_t oat_method_idx = GetOatMethodIndexFromMethodIndex(dex_file, method_idx);
  UniquePtr<const OatFile::OatClass> oat_class(GetOatClass(dex_file, descriptor));
  CHECK(oat_class.get() != NULL);
  return oat_class->GetOatMethod(oat_method_idx).GetCode();
}

void ClassLinker::FixupStaticTrampolines(Class* klass) {
  ClassHelper kh(klass);
  const DexFile::ClassDef* dex_class_def = kh.GetClassDef();
  CHECK(dex_class_def != NULL);
  const DexFile& dex_file = kh.GetDexFile();
  const byte* class_data = dex_file.GetClassData(*dex_class_def);
  if (class_data == NULL) {
    return;  // no fields or methods - for example a marker interface
  }
  if (!Runtime::Current()->IsStarted() || Runtime::Current()->UseCompileTimeClassPath()) {
    // OAT file unavailable
    return;
  }
  UniquePtr<const OatFile::OatClass> oat_class(GetOatClass(dex_file, kh.GetDescriptor()));
  CHECK(oat_class.get() != NULL);
  ClassDataItemIterator it(dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  size_t method_index = 0;
  // Link the code of methods skipped by LinkCode
  const void* trampoline = Runtime::Current()->GetResolutionStubArray(Runtime::kStaticMethod)->GetData();
  for (size_t i = 0; it.HasNextDirectMethod(); i++, it.Next()) {
    AbstractMethod* method = klass->GetDirectMethod(i);
    if (Runtime::Current()->IsMethodTracingActive()) {
      Trace* tracer = Runtime::Current()->GetTracer();
      if (tracer->GetSavedCodeFromMap(method) == trampoline) {
        const void* code = oat_class->GetOatMethod(method_index).GetCode();
        tracer->ResetSavedCode(method);
        method->SetCode(code);
        tracer->SaveAndUpdateCode(method);
      }
    } else if (method->GetCode() == trampoline) {
      const void* code = oat_class->GetOatMethod(method_index).GetCode();
      CHECK(code != NULL)
          << "Resolving a static trampoline but found no code for: " << PrettyMethod(method);
      method->SetCode(code);
    }
    method_index++;
  }
}

static void LinkCode(SirtRef<AbstractMethod>& method, const OatFile::OatClass* oat_class,
                     uint32_t method_index)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Every kind of method should at least get an invoke stub from the oat_method.
  // non-abstract methods also get their code pointers.
  const OatFile::OatMethod oat_method = oat_class->GetOatMethod(method_index);
  oat_method.LinkMethodPointers(method.get());

  Runtime* runtime = Runtime::Current();
  if (method->IsAbstract()) {
    method->SetCode(runtime->GetAbstractMethodErrorStubArray()->GetData());
    return;
  }

  if (method->IsStatic() && !method->IsConstructor()) {
    // For static methods excluding the class initializer, install the trampoline
    method->SetCode(runtime->GetResolutionStubArray(Runtime::kStaticMethod)->GetData());
  }
  if (method->IsNative()) {
    // unregistering restores the dlsym lookup stub
    method->UnregisterNative(Thread::Current());
  }

  if (Runtime::Current()->IsMethodTracingActive()) {
    Trace* tracer = Runtime::Current()->GetTracer();
    tracer->SaveAndUpdateCode(method.get());
  }
}

void ClassLinker::LoadClass(const DexFile& dex_file,
                            const DexFile::ClassDef& dex_class_def,
                            SirtRef<Class>& klass,
                            ClassLoader* class_loader) {
  CHECK(klass.get() != NULL);
  CHECK(klass->GetDexCache() != NULL);
  CHECK_EQ(Class::kStatusNotReady, klass->GetStatus());
  const char* descriptor = dex_file.GetClassDescriptor(dex_class_def);
  CHECK(descriptor != NULL);

  klass->SetClass(GetClassRoot(kJavaLangClass));
  uint32_t access_flags = dex_class_def.access_flags_;
  // Make sure that none of our runtime-only flags are set.
  // TODO: JACK CLASS ACCESS (HACK TO BE REMOVED)
  CHECK_EQ(access_flags & ~(kAccJavaFlagsMask | kAccClassJack), 0U);
  klass->SetAccessFlags(access_flags);
  klass->SetClassLoader(class_loader);
  DCHECK_EQ(klass->GetPrimitiveType(), Primitive::kPrimNot);
  klass->SetStatus(Class::kStatusIdx);

  klass->SetDexTypeIndex(dex_class_def.class_idx_);

  // Load fields fields.
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  if (class_data == NULL) {
    return;  // no fields or methods - for example a marker interface
  }
  ClassDataItemIterator it(dex_file, class_data);
  Thread* self = Thread::Current();
  if (it.NumStaticFields() != 0) {
    klass->SetSFields(AllocFieldArray(self, it.NumStaticFields()));
  }
  if (it.NumInstanceFields() != 0) {
    klass->SetIFields(AllocFieldArray(self, it.NumInstanceFields()));
  }
  for (size_t i = 0; it.HasNextStaticField(); i++, it.Next()) {
    SirtRef<Field> sfield(self, AllocField(self));
    klass->SetStaticField(i, sfield.get());
    LoadField(dex_file, it, klass, sfield);
  }
  for (size_t i = 0; it.HasNextInstanceField(); i++, it.Next()) {
    SirtRef<Field> ifield(self, AllocField(self));
    klass->SetInstanceField(i, ifield.get());
    LoadField(dex_file, it, klass, ifield);
  }

  UniquePtr<const OatFile::OatClass> oat_class;
  if (Runtime::Current()->IsStarted() && !Runtime::Current()->UseCompileTimeClassPath()) {
    oat_class.reset(GetOatClass(dex_file, descriptor));
  }

  // Load methods.
  if (it.NumDirectMethods() != 0) {
    // TODO: append direct methods to class object
    klass->SetDirectMethods(AllocAbstractMethodArray(self, it.NumDirectMethods()));
  }
  if (it.NumVirtualMethods() != 0) {
    // TODO: append direct methods to class object
    klass->SetVirtualMethods(AllocMethodArray(self, it.NumVirtualMethods()));
  }
  size_t class_def_method_index = 0;
  for (size_t i = 0; it.HasNextDirectMethod(); i++, it.Next()) {
    SirtRef<AbstractMethod> method(self, LoadMethod(self, dex_file, it, klass));
    klass->SetDirectMethod(i, method.get());
    if (oat_class.get() != NULL) {
      LinkCode(method, oat_class.get(), class_def_method_index);
    }
    method->SetMethodIndex(class_def_method_index);
    class_def_method_index++;
  }
  for (size_t i = 0; it.HasNextVirtualMethod(); i++, it.Next()) {
    SirtRef<AbstractMethod> method(self, LoadMethod(self, dex_file, it, klass));
    klass->SetVirtualMethod(i, method.get());
    DCHECK_EQ(class_def_method_index, it.NumDirectMethods() + i);
    if (oat_class.get() != NULL) {
      LinkCode(method, oat_class.get(), class_def_method_index);
    }
    class_def_method_index++;
  }
  DCHECK(!it.HasNext());
}

void ClassLinker::LoadField(const DexFile& /*dex_file*/, const ClassDataItemIterator& it,
                            SirtRef<Class>& klass, SirtRef<Field>& dst) {
  uint32_t field_idx = it.GetMemberIndex();
  dst->SetDexFieldIndex(field_idx);
  dst->SetDeclaringClass(klass.get());
  dst->SetAccessFlags(it.GetMemberAccessFlags());
}

AbstractMethod* ClassLinker::LoadMethod(Thread* self, const DexFile& dex_file,
                                        const ClassDataItemIterator& it,
                                        SirtRef<Class>& klass) {
  uint32_t dex_method_idx = it.GetMemberIndex();
  const DexFile::MethodId& method_id = dex_file.GetMethodId(dex_method_idx);
  StringPiece method_name(dex_file.GetMethodName(method_id));

  AbstractMethod* dst = NULL;
  if (method_name == "<init>") {
    dst = AllocConstructor(self);
  } else {
    dst = AllocMethod(self);
  }
  DCHECK(dst->IsMethod()) << PrettyDescriptor(dst->GetClass());

  const char* old_cause = self->StartAssertNoThreadSuspension("LoadMethod");
  dst->SetDexMethodIndex(dex_method_idx);
  dst->SetDeclaringClass(klass.get());

  if (method_name == "finalize") {
    // Create the prototype for a signature of "()V"
    const DexFile::StringId* void_string_id = dex_file.FindStringId("V");
    if (void_string_id != NULL) {
      const DexFile::TypeId* void_type_id =
          dex_file.FindTypeId(dex_file.GetIndexForStringId(*void_string_id));
      if (void_type_id != NULL) {
        std::vector<uint16_t> no_args;
        const DexFile::ProtoId* finalizer_proto =
            dex_file.FindProtoId(dex_file.GetIndexForTypeId(*void_type_id), no_args);
        if (finalizer_proto != NULL) {
          // We have the prototype in the dex file
          if (klass->GetClassLoader() != NULL) {  // All non-boot finalizer methods are flagged
            klass->SetFinalizable();
          } else {
            StringPiece klass_descriptor(dex_file.StringByTypeIdx(klass->GetDexTypeIndex()));
            // The Enum class declares a "final" finalize() method to prevent subclasses from
            // introducing a finalizer. We don't want to set the finalizable flag for Enum or its
            // subclasses, so we exclude it here.
            // We also want to avoid setting the flag on Object, where we know that finalize() is
            // empty.
            if (klass_descriptor != "Ljava/lang/Object;" &&
                klass_descriptor != "Ljava/lang/Enum;") {
              klass->SetFinalizable();
            }
          }
        }
      }
    }
  }
  dst->SetCodeItemOffset(it.GetMethodCodeItemOffset());
  dst->SetAccessFlags(it.GetMemberAccessFlags());

  dst->SetDexCacheStrings(klass->GetDexCache()->GetStrings());
  dst->SetDexCacheResolvedMethods(klass->GetDexCache()->GetResolvedMethods());
  dst->SetDexCacheResolvedTypes(klass->GetDexCache()->GetResolvedTypes());
  dst->SetDexCacheInitializedStaticStorage(klass->GetDexCache()->GetInitializedStaticStorage());

  CHECK(dst->IsMethod());

  self->EndAssertNoThreadSuspension(old_cause);
  return dst;
}

void ClassLinker::AppendToBootClassPath(const DexFile& dex_file) {
  Thread* self = Thread::Current();
  SirtRef<DexCache> dex_cache(self, AllocDexCache(self, dex_file));
  AppendToBootClassPath(dex_file, dex_cache);
}

void ClassLinker::AppendToBootClassPath(const DexFile& dex_file, SirtRef<DexCache>& dex_cache) {
  CHECK(dex_cache.get() != NULL) << dex_file.GetLocation();
  boot_class_path_.push_back(&dex_file);
  RegisterDexFile(dex_file, dex_cache);
}

bool ClassLinker::IsDexFileRegisteredLocked(const DexFile& dex_file) const {
  dex_lock_.AssertHeld(Thread::Current());
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    if (dex_caches_[i]->GetDexFile() == &dex_file) {
      return true;
    }
  }
  return false;
}

bool ClassLinker::IsDexFileRegistered(const DexFile& dex_file) const {
  MutexLock mu(Thread::Current(), dex_lock_);
  return IsDexFileRegisteredLocked(dex_file);
}

void ClassLinker::RegisterDexFileLocked(const DexFile& dex_file, SirtRef<DexCache>& dex_cache) {
  dex_lock_.AssertHeld(Thread::Current());
  CHECK(dex_cache.get() != NULL) << dex_file.GetLocation();
  CHECK(dex_cache->GetLocation()->Equals(dex_file.GetLocation()));
  dex_caches_.push_back(dex_cache.get());
  dex_cache->SetDexFile(&dex_file);
  Dirty();
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file) {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
  }
  // Don't alloc while holding the lock, since allocation may need to
  // suspend all threads and another thread may need the dex_lock_ to
  // get to a suspend point.
  SirtRef<DexCache> dex_cache(self, AllocDexCache(self, dex_file));
  {
    MutexLock mu(self, dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
    RegisterDexFileLocked(dex_file, dex_cache);
  }
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file, SirtRef<DexCache>& dex_cache) {
  MutexLock mu(Thread::Current(), dex_lock_);
  RegisterDexFileLocked(dex_file, dex_cache);
}

DexCache* ClassLinker::FindDexCache(const DexFile& dex_file) const {
  MutexLock mu(Thread::Current(), dex_lock_);
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    DexCache* dex_cache = dex_caches_[i];
    if (dex_cache->GetDexFile() == &dex_file) {
      return dex_cache;
    }
  }
  LOG(FATAL) << "Failed to find DexCache for DexFile " << dex_file.GetLocation();
  return NULL;
}

void ClassLinker::FixupDexCaches(AbstractMethod* resolution_method) const {
  MutexLock mu(Thread::Current(), dex_lock_);
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    dex_caches_[i]->Fixup(resolution_method);
  }
}

Class* ClassLinker::InitializePrimitiveClass(Class* primitive_class, Primitive::Type type) {
  CHECK(primitive_class != NULL);
  // Must hold lock on object when initializing.
  ObjectLock lock(Thread::Current(), primitive_class);
  primitive_class->SetAccessFlags(kAccPublic | kAccFinal | kAccAbstract);
  primitive_class->SetPrimitiveType(type);
  primitive_class->SetStatus(Class::kStatusInitialized);
  Class* existing = InsertClass(Primitive::Descriptor(type), primitive_class, false);
  CHECK(existing == NULL) << "InitPrimitiveClass(" << type << ") failed";
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
// Returns NULL with an exception raised on failure.
Class* ClassLinker::CreateArrayClass(const std::string& descriptor, ClassLoader* class_loader) {
  CHECK_EQ('[', descriptor[0]);

  // Identify the underlying component type
  Class* component_type = FindClass(descriptor.substr(1).c_str(), class_loader);
  if (component_type == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
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
  if (class_loader != component_type->GetClassLoader()) {
    Class* new_class = LookupClass(descriptor.c_str(), component_type->GetClassLoader());
    if (new_class != NULL) {
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
  Thread* self = Thread::Current();
  SirtRef<Class> new_class(self, NULL);
  if (!init_done_) {
    // Classes that were hand created, ie not by FindSystemClass
    if (descriptor == "[Ljava/lang/Class;") {
      new_class.reset(GetClassRoot(kClassArrayClass));
    } else if (descriptor == "[Ljava/lang/Object;") {
      new_class.reset(GetClassRoot(kObjectArrayClass));
    } else if (descriptor == class_roots_descriptors_[kJavaLangStringArrayClass]) {
      new_class.reset(GetClassRoot(kJavaLangStringArrayClass));
    } else if (descriptor == class_roots_descriptors_[kJavaLangReflectAbstractMethodArrayClass]) {
      new_class.reset(GetClassRoot(kJavaLangReflectAbstractMethodArrayClass));
    } else if (descriptor == class_roots_descriptors_[kJavaLangReflectFieldArrayClass]) {
      new_class.reset(GetClassRoot(kJavaLangReflectFieldArrayClass));
    } else if (descriptor == class_roots_descriptors_[kJavaLangReflectMethodArrayClass]) {
      new_class.reset(GetClassRoot(kJavaLangReflectMethodArrayClass));
    } else if (descriptor == "[C") {
      new_class.reset(GetClassRoot(kCharArrayClass));
    } else if (descriptor == "[I") {
      new_class.reset(GetClassRoot(kIntArrayClass));
    }
  }
  if (new_class.get() == NULL) {
    new_class.reset(AllocClass(self, sizeof(Class)));
    if (new_class.get() == NULL) {
      return NULL;
    }
    new_class->SetComponentType(component_type);
  }
  ObjectLock lock(self, new_class.get());  // Must hold lock on object when initializing.
  DCHECK(new_class->GetComponentType() != NULL);
  Class* java_lang_Object = GetClassRoot(kJavaLangObject);
  new_class->SetSuperClass(java_lang_Object);
  new_class->SetVTable(java_lang_Object->GetVTable());
  new_class->SetPrimitiveType(Primitive::kPrimNot);
  new_class->SetClassLoader(component_type->GetClassLoader());
  new_class->SetStatus(Class::kStatusInitialized);
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
  CHECK(array_iftable_ != NULL);
  new_class->SetIfTable(array_iftable_);

  // Inherit access flags from the component type.  Arrays can't be
  // used as a superclass or interface, so we want to add "final"
  // and remove "interface".
  //
  // Don't inherit any non-standard flags (e.g., kAccFinal)
  // from component_type.  We assume that the array class does not
  // override finalize().
  new_class->SetAccessFlags(((new_class->GetComponentType()->GetAccessFlags() &
                             ~kAccInterface) | kAccFinal) & kAccJavaFlagsMask);

  Class* existing = InsertClass(descriptor, new_class.get(), false);
  if (existing == NULL) {
    return new_class.get();
  }
  // Another thread must have loaded the class after we
  // started but before we finished.  Abandon what we've
  // done.
  //
  // (Yes, this happens.)

  return existing;
}

Class* ClassLinker::FindPrimitiveClass(char type) {
  switch (Primitive::GetType(type)) {
    case Primitive::kPrimByte:
      return GetClassRoot(kPrimitiveByte);
    case Primitive::kPrimChar:
      return GetClassRoot(kPrimitiveChar);
    case Primitive::kPrimDouble:
      return GetClassRoot(kPrimitiveDouble);
    case Primitive::kPrimFloat:
      return GetClassRoot(kPrimitiveFloat);
    case Primitive::kPrimInt:
      return GetClassRoot(kPrimitiveInt);
    case Primitive::kPrimLong:
      return GetClassRoot(kPrimitiveLong);
    case Primitive::kPrimShort:
      return GetClassRoot(kPrimitiveShort);
    case Primitive::kPrimBoolean:
      return GetClassRoot(kPrimitiveBoolean);
    case Primitive::kPrimVoid:
      return GetClassRoot(kPrimitiveVoid);
    case Primitive::kPrimNot:
      break;
  }
  std::string printable_type(PrintableChar(type));
  ThrowNoClassDefFoundError("Not a primitive type: %s", printable_type.c_str());
  return NULL;
}

Class* ClassLinker::InsertClass(const StringPiece& descriptor, Class* klass, bool image_class) {
  if (VLOG_IS_ON(class_linker)) {
    DexCache* dex_cache = klass->GetDexCache();
    std::string source;
    if (dex_cache != NULL) {
      source += " from ";
      source += dex_cache->GetLocation()->ToModifiedUtf8();
    }
    LOG(INFO) << "Loaded class " << descriptor << source;
  }
  size_t hash = StringPieceHash()(descriptor);
  MutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  Table& classes = image_class ? image_classes_ : classes_;
  Class* existing = LookupClassLocked(descriptor.data(), klass->GetClassLoader(), hash, classes);
#ifndef NDEBUG
  // Check we don't have the class in the other table in error
  Table& other_classes = image_class ? classes_ : image_classes_;
  CHECK(LookupClassLocked(descriptor.data(), klass->GetClassLoader(), hash, other_classes) == NULL);
#endif
  if (existing != NULL) {
    return existing;
  }
  classes.insert(std::make_pair(hash, klass));
  Dirty();
  return NULL;
}

bool ClassLinker::RemoveClass(const char* descriptor, const ClassLoader* class_loader) {
  size_t hash = Hash(descriptor);
  MutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  typedef Table::iterator It;  // TODO: C++0x auto
  // TODO: determine if its better to search classes_ or image_classes_ first
  ClassHelper kh;
  for (It it = classes_.lower_bound(hash), end = classes_.end(); it != end && it->first == hash; ++it) {
    Class* klass = it->second;
    kh.ChangeClass(klass);
    if (strcmp(kh.GetDescriptor(), descriptor) == 0 && klass->GetClassLoader() == class_loader) {
      classes_.erase(it);
      return true;
    }
  }
  for (It it = image_classes_.lower_bound(hash), end = classes_.end(); it != end && it->first == hash; ++it) {
    Class* klass = it->second;
    kh.ChangeClass(klass);
    if (strcmp(kh.GetDescriptor(), descriptor) == 0 && klass->GetClassLoader() == class_loader) {
      image_classes_.erase(it);
      return true;
    }
  }
  return false;
}

Class* ClassLinker::LookupClass(const char* descriptor, const ClassLoader* class_loader) {
  size_t hash = Hash(descriptor);
  MutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  // TODO: determine if its better to search classes_ or image_classes_ first
  Class* klass = LookupClassLocked(descriptor, class_loader, hash, classes_);
  if (klass != NULL) {
    return klass;
  }
  return LookupClassLocked(descriptor, class_loader, hash, image_classes_);
}

Class* ClassLinker::LookupClassLocked(const char* descriptor, const ClassLoader* class_loader,
                                      size_t hash, const Table& classes) {
  ClassHelper kh(NULL, this);
  typedef Table::const_iterator It;  // TODO: C++0x auto
  for (It it = classes.lower_bound(hash), end = classes_.end(); it != end && it->first == hash; ++it) {
    Class* klass = it->second;
    kh.ChangeClass(klass);
    if (strcmp(descriptor, kh.GetDescriptor()) == 0 && klass->GetClassLoader() == class_loader) {
#ifndef NDEBUG
      for (++it; it != end && it->first == hash; ++it) {
        Class* klass2 = it->second;
        kh.ChangeClass(klass2);
        CHECK(!(strcmp(descriptor, kh.GetDescriptor()) == 0 && klass2->GetClassLoader() == class_loader))
                << PrettyClass(klass) << " " << klass << " " << klass->GetClassLoader() << " "
                << PrettyClass(klass2) << " " << klass2 << " " << klass2->GetClassLoader();
      }
#endif
      return klass;
    }
  }
  return NULL;
}

void ClassLinker::LookupClasses(const char* descriptor, std::vector<Class*>& classes) {
  classes.clear();
  size_t hash = Hash(descriptor);
  MutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  typedef Table::const_iterator It;  // TODO: C++0x auto
  // TODO: determine if its better to search classes_ or image_classes_ first
  ClassHelper kh(NULL, this);
  for (It it = classes_.lower_bound(hash), end = classes_.end(); it != end && it->first == hash; ++it) {
    Class* klass = it->second;
    kh.ChangeClass(klass);
    if (strcmp(descriptor, kh.GetDescriptor()) == 0) {
      classes.push_back(klass);
    }
  }
  for (It it = image_classes_.lower_bound(hash), end = classes_.end(); it != end && it->first == hash; ++it) {
    Class* klass = it->second;
    kh.ChangeClass(klass);
    if (strcmp(descriptor, kh.GetDescriptor()) == 0) {
      classes.push_back(klass);
    }
  }
}

void ClassLinker::VerifyClass(Class* klass) {
  // TODO: assert that the monitor on the Class is held
  Thread* self = Thread::Current();
  ObjectLock lock(self, klass);

  // Don't attempt to re-verify if already sufficiently verified.
  if (klass->IsVerified() ||
      (klass->IsCompileTimeVerified() && Runtime::Current()->IsCompiler())) {
    return;
  }

  // The class might already be erroneous, for example at compile time if we attempted to verify
  // this class as a parent to another.
  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass);
    return;
  }

  if (klass->GetStatus() == Class::kStatusResolved) {
    klass->SetStatus(Class::kStatusVerifying);
  } else {
    CHECK_EQ(klass->GetStatus(), Class::kStatusRetryVerificationAtRuntime) << PrettyClass(klass);
    CHECK(!Runtime::Current()->IsCompiler());
    klass->SetStatus(Class::kStatusVerifyingAtRuntime);
  }

  // Verify super class.
  Class* super = klass->GetSuperClass();
  std::string error_msg;
  if (super != NULL) {
    // Acquire lock to prevent races on verifying the super class.
    ObjectLock lock(self, super);

    if (!super->IsVerified() && !super->IsErroneous()) {
      Runtime::Current()->GetClassLinker()->VerifyClass(super);
    }
    if (!super->IsCompileTimeVerified()) {
      error_msg = "Rejecting class ";
      error_msg += PrettyDescriptor(klass);
      error_msg += " that attempts to sub-class erroneous class ";
      error_msg += PrettyDescriptor(super);
      LOG(ERROR) << error_msg  << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8();
      SirtRef<Throwable> cause(self, self->GetException());
      if (cause.get() != NULL) {
        self->ClearException();
      }
      self->ThrowNewException("Ljava/lang/VerifyError;", error_msg.c_str());
      if (cause.get() != NULL) {
        self->GetException()->SetCause(cause.get());
      }
      klass->SetStatus(Class::kStatusError);
      return;
    }
  }

  // Try to use verification information from the oat file, otherwise do runtime verification.
  const DexFile& dex_file = *klass->GetDexCache()->GetDexFile();
  Class::Status oat_file_class_status(Class::kStatusNotReady);
  bool preverified = VerifyClassUsingOatFile(dex_file, klass, oat_file_class_status);
  verifier::MethodVerifier::FailureKind verifier_failure = verifier::MethodVerifier::kNoFailure;
  if (oat_file_class_status == Class::kStatusError) {
    LOG(WARNING) << "Skipping runtime verification of erroneous class " << PrettyDescriptor(klass)
                 << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8();
    error_msg = "Rejecting class ";
    error_msg += PrettyDescriptor(klass);
    error_msg += " because it failed compile-time verification";
    Thread::Current()->ThrowNewException("Ljava/lang/VerifyError;", error_msg.c_str());
    klass->SetStatus(Class::kStatusError);
    return;
  }
  if (!preverified) {
    verifier_failure = verifier::MethodVerifier::VerifyClass(klass, error_msg);
  }
  if (preverified || verifier_failure != verifier::MethodVerifier::kHardFailure) {
    if (!preverified && verifier_failure != verifier::MethodVerifier::kNoFailure) {
      LOG(WARNING) << "Soft verification failure in class " << PrettyDescriptor(klass)
          << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8()
          << " because: " << error_msg;
    }
    self->AssertNoPendingException();
    // Make sure all classes referenced by catch blocks are resolved.
    ResolveClassExceptionHandlerTypes(dex_file, klass);
    if (verifier_failure == verifier::MethodVerifier::kNoFailure) {
      klass->SetStatus(Class::kStatusVerified);
    } else {
      CHECK_EQ(verifier_failure, verifier::MethodVerifier::kSoftFailure);
      // Soft failures at compile time should be retried at runtime. Soft
      // failures at runtime will be handled by slow paths in the generated
      // code. Set status accordingly.
      if (Runtime::Current()->IsCompiler()) {
        klass->SetStatus(Class::kStatusRetryVerificationAtRuntime);
      } else {
        klass->SetStatus(Class::kStatusVerified);
      }
    }
  } else {
    LOG(ERROR) << "Verification failed on class " << PrettyDescriptor(klass)
        << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8()
        << " because: " << error_msg;
    self->AssertNoPendingException();
    self->ThrowNewException("Ljava/lang/VerifyError;", error_msg.c_str());
    klass->SetStatus(Class::kStatusError);
  }
}

bool ClassLinker::VerifyClassUsingOatFile(const DexFile& dex_file, Class* klass,
                                          Class::Status& oat_file_class_status) {
  if (!Runtime::Current()->IsStarted()) {
    return false;
  }
  if (Runtime::Current()->UseCompileTimeClassPath()) {
    return false;
  }
  const OatFile* oat_file = FindOpenedOatFileForDexFile(dex_file);
  CHECK(oat_file != NULL) << dex_file.GetLocation() << " " << PrettyClass(klass);
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation());
  CHECK(oat_dex_file != NULL) << dex_file.GetLocation() << " " << PrettyClass(klass);
  const char* descriptor = ClassHelper(klass).GetDescriptor();
  uint32_t class_def_index;
  bool found = dex_file.FindClassDefIndex(descriptor, class_def_index);
  CHECK(found) << dex_file.GetLocation() << " " << PrettyClass(klass) << " " << descriptor;
  UniquePtr<const OatFile::OatClass> oat_class(oat_dex_file->GetOatClass(class_def_index));
  CHECK(oat_class.get() != NULL)
          << dex_file.GetLocation() << " " << PrettyClass(klass) << " " << descriptor;
  oat_file_class_status = oat_class->GetStatus();
  if (oat_file_class_status == Class::kStatusVerified ||
      oat_file_class_status == Class::kStatusInitialized) {
    return true;
  }
  if (oat_file_class_status == Class::kStatusRetryVerificationAtRuntime) {
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
  if (oat_file_class_status == Class::kStatusError) {
    // Compile time verification failed with a hard error. This is caused by invalid instructions
    // in the class. These errors are unrecoverable.
    return false;
  }
  if (oat_file_class_status == Class::kStatusNotReady) {
    // Status is uninitialized if we couldn't determine the status at compile time, for example,
    // not loading the class.
    // TODO: when the verifier doesn't rely on Class-es failing to resolve/load the type hierarchy
    // isn't a problem and this case shouldn't occur
    return false;
  }
  LOG(FATAL) << "Unexpected class status: " << oat_file_class_status
             << " " << dex_file.GetLocation() << " " << PrettyClass(klass) << " " << descriptor;

  return false;
}

void ClassLinker::ResolveClassExceptionHandlerTypes(const DexFile& dex_file, Class* klass) {
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    ResolveMethodExceptionHandlerTypes(dex_file, klass->GetDirectMethod(i));
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    ResolveMethodExceptionHandlerTypes(dex_file, klass->GetVirtualMethod(i));
  }
}

void ClassLinker::ResolveMethodExceptionHandlerTypes(const DexFile& dex_file, AbstractMethod* method) {
  // similar to DexVerifier::ScanTryCatchBlocks and dex2oat's ResolveExceptionsForMethod.
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());
  if (code_item == NULL) {
    return;  // native or abstract method
  }
  if (code_item->tries_size_ == 0) {
    return;  // nothing to process
  }
  const byte* handlers_ptr = DexFile::GetCatchHandlerData(*code_item, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      // Ensure exception types are resolved so that they don't need resolution to be delivered,
      // unresolved exception types will be ignored by exception delivery
      if (iterator.GetHandlerTypeIndex() != DexFile::kDexNoIndex16) {
        Class* exception_type = linker->ResolveType(iterator.GetHandlerTypeIndex(), method);
        if (exception_type == NULL) {
          DCHECK(Thread::Current()->IsExceptionPending());
          Thread::Current()->ClearException();
        }
      }
    }
    handlers_ptr = iterator.EndDataPointer();
  }
}

static void CheckProxyConstructor(AbstractMethod* constructor);
static void CheckProxyMethod(AbstractMethod* method, SirtRef<AbstractMethod>& prototype);

Class* ClassLinker::CreateProxyClass(String* name, ObjectArray<Class>* interfaces,
                                     ClassLoader* loader, ObjectArray<AbstractMethod>* methods,
                                     ObjectArray<ObjectArray<Class> >* throws) {
  Thread* self = Thread::Current();
  SirtRef<Class> klass(self, AllocClass(self, GetClassRoot(kJavaLangClass),
                                        sizeof(SynthesizedProxyClass)));
  CHECK(klass.get() != NULL);
  DCHECK(klass->GetClass() != NULL);
  klass->SetObjectSize(sizeof(Proxy));
  klass->SetAccessFlags(kAccClassIsProxy | kAccPublic | kAccFinal);
  klass->SetClassLoader(loader);
  DCHECK_EQ(klass->GetPrimitiveType(), Primitive::kPrimNot);
  klass->SetName(name);
  Class* proxy_class = GetClassRoot(kJavaLangReflectProxy);
  klass->SetDexCache(proxy_class->GetDexCache());

  klass->SetStatus(Class::kStatusIdx);

  klass->SetDexTypeIndex(DexFile::kDexNoIndex16);

  // Instance fields are inherited, but we add a couple of static fields...
  klass->SetSFields(AllocFieldArray(self, 2));
  // 1. Create a static field 'interfaces' that holds the _declared_ interfaces implemented by
  // our proxy, so Class.getInterfaces doesn't return the flattened set.
  SirtRef<Field> interfaces_sfield(self, AllocField(self));
  klass->SetStaticField(0, interfaces_sfield.get());
  interfaces_sfield->SetDexFieldIndex(0);
  interfaces_sfield->SetDeclaringClass(klass.get());
  interfaces_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);
  // 2. Create a static field 'throws' that holds exceptions thrown by our methods.
  SirtRef<Field> throws_sfield(self, AllocField(self));
  klass->SetStaticField(1, throws_sfield.get());
  throws_sfield->SetDexFieldIndex(1);
  throws_sfield->SetDeclaringClass(klass.get());
  throws_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);

  // Proxies have 1 direct method, the constructor
  klass->SetDirectMethods(AllocAbstractMethodArray(self, 1));
  klass->SetDirectMethod(0, CreateProxyConstructor(self, klass, proxy_class));

  // Create virtual method using specified prototypes
  size_t num_virtual_methods = methods->GetLength();
  klass->SetVirtualMethods(AllocMethodArray(self, num_virtual_methods));
  for (size_t i = 0; i < num_virtual_methods; ++i) {
    SirtRef<AbstractMethod> prototype(self, methods->Get(i));
    klass->SetVirtualMethod(i, CreateProxyMethod(self, klass, prototype));
  }

  klass->SetSuperClass(proxy_class);  // The super class is java.lang.reflect.Proxy
  klass->SetStatus(Class::kStatusLoaded);  // Class is now effectively in the loaded state
  DCHECK(!Thread::Current()->IsExceptionPending());

  // Link the fields and virtual methods, creating vtable and iftables
  if (!LinkClass(klass, interfaces)) {
    klass->SetStatus(Class::kStatusError);
    return NULL;
  }
  {
    ObjectLock lock(self, klass.get());  // Must hold lock on object when initializing.
    interfaces_sfield->SetObject(klass.get(), interfaces);
    throws_sfield->SetObject(klass.get(), throws);
    klass->SetStatus(Class::kStatusInitialized);
  }

  // sanity checks
  if (kIsDebugBuild) {
    CHECK(klass->GetIFields() == NULL);
    CheckProxyConstructor(klass->GetDirectMethod(0));
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      SirtRef<AbstractMethod> prototype(self, methods->Get(i));
      CheckProxyMethod(klass->GetVirtualMethod(i), prototype);
    }

    std::string interfaces_field_name(StringPrintf("java.lang.Class[] %s.interfaces",
                                                   name->ToModifiedUtf8().c_str()));
    CHECK_EQ(PrettyField(klass->GetStaticField(0)), interfaces_field_name);

    std::string throws_field_name(StringPrintf("java.lang.Class[][] %s.throws",
                                               name->ToModifiedUtf8().c_str()));
    CHECK_EQ(PrettyField(klass->GetStaticField(1)), throws_field_name);

    SynthesizedProxyClass* synth_proxy_class = down_cast<SynthesizedProxyClass*>(klass.get());
    CHECK_EQ(synth_proxy_class->GetInterfaces(), interfaces);
    CHECK_EQ(synth_proxy_class->GetThrows(), throws);
  }
  return klass.get();
}

std::string ClassLinker::GetDescriptorForProxy(const Class* proxy_class) {
  DCHECK(proxy_class->IsProxyClass());
  String* name = proxy_class->GetName();
  DCHECK(name != NULL);
  return DotToDescriptor(name->ToModifiedUtf8().c_str());
}

AbstractMethod* ClassLinker::FindMethodForProxy(const Class* proxy_class, const AbstractMethod* proxy_method) {
  DCHECK(proxy_class->IsProxyClass());
  DCHECK(proxy_method->IsProxyMethod());
  // Locate the dex cache of the original interface/Object
  DexCache* dex_cache = NULL;
  {
    ObjectArray<Class>* resolved_types = proxy_method->GetDexCacheResolvedTypes();
    MutexLock mu(Thread::Current(), dex_lock_);
    for (size_t i = 0; i != dex_caches_.size(); ++i) {
      if (dex_caches_[i]->GetResolvedTypes() == resolved_types) {
        dex_cache = dex_caches_[i];
        break;
      }
    }
  }
  CHECK(dex_cache != NULL);
  uint32_t method_idx = proxy_method->GetDexMethodIndex();
  AbstractMethod* resolved_method = dex_cache->GetResolvedMethod(method_idx);
  CHECK(resolved_method != NULL);
  return resolved_method;
}


AbstractMethod* ClassLinker::CreateProxyConstructor(Thread* self, SirtRef<Class>& klass, Class* proxy_class) {
  // Create constructor for Proxy that must initialize h
  ObjectArray<AbstractMethod>* proxy_direct_methods = proxy_class->GetDirectMethods();
  CHECK_EQ(proxy_direct_methods->GetLength(), 15);
  AbstractMethod* proxy_constructor = proxy_direct_methods->Get(2);
  // Clone the existing constructor of Proxy (our constructor would just invoke it so steal its
  // code_ too)
  AbstractMethod* constructor = down_cast<AbstractMethod*>(proxy_constructor->Clone(self));
  // Make this constructor public and fix the class to be our Proxy version
  constructor->SetAccessFlags((constructor->GetAccessFlags() & ~kAccProtected) | kAccPublic);
  constructor->SetDeclaringClass(klass.get());
  return constructor;
}

static void CheckProxyConstructor(AbstractMethod* constructor)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(constructor->IsConstructor());
  MethodHelper mh(constructor);
  CHECK_STREQ(mh.GetName(), "<init>");
  CHECK_EQ(mh.GetSignature(), std::string("(Ljava/lang/reflect/InvocationHandler;)V"));
  DCHECK(constructor->IsPublic());
}

AbstractMethod* ClassLinker::CreateProxyMethod(Thread* self, SirtRef<Class>& klass,
                                               SirtRef<AbstractMethod>& prototype) {
  // Ensure prototype is in dex cache so that we can use the dex cache to look up the overridden
  // prototype method
  prototype->GetDeclaringClass()->GetDexCache()->SetResolvedMethod(prototype->GetDexMethodIndex(),
                                                                   prototype.get());
  // We steal everything from the prototype (such as DexCache, invoke stub, etc.) then specialize
  // as necessary
  AbstractMethod* method = down_cast<AbstractMethod*>(prototype->Clone(self));

  // Set class to be the concrete proxy class and clear the abstract flag, modify exceptions to
  // the intersection of throw exceptions as defined in Proxy
  method->SetDeclaringClass(klass.get());
  method->SetAccessFlags((method->GetAccessFlags() & ~kAccAbstract) | kAccFinal);

  // At runtime the method looks like a reference and argument saving method, clone the code
  // related parameters from this method.
  AbstractMethod* refs_and_args = Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs);
  method->SetCoreSpillMask(refs_and_args->GetCoreSpillMask());
  method->SetFpSpillMask(refs_and_args->GetFpSpillMask());
  method->SetFrameSizeInBytes(refs_and_args->GetFrameSizeInBytes());
#if !defined(ART_USE_LLVM_COMPILER)
  method->SetCode(reinterpret_cast<void*>(art_proxy_invoke_handler));
#else
  OatFile::OatMethod oat_method = GetOatMethodFor(prototype.get());
  method->SetCode(oat_method.GetProxyStub());
#endif

  return method;
}

static void CheckProxyMethod(AbstractMethod* method, SirtRef<AbstractMethod>& prototype)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Basic sanity
  CHECK(!prototype->IsFinal());
  CHECK(method->IsFinal());
  CHECK(!method->IsAbstract());

  // The proxy method doesn't have its own dex cache or dex file and so it steals those of its
  // interface prototype. The exception to this are Constructors and the Class of the Proxy itself.
  CHECK_EQ(prototype->GetDexCacheStrings(), method->GetDexCacheStrings());
  CHECK_EQ(prototype->GetDexCacheResolvedMethods(), method->GetDexCacheResolvedMethods());
  CHECK_EQ(prototype->GetDexCacheResolvedTypes(), method->GetDexCacheResolvedTypes());
  CHECK_EQ(prototype->GetDexCacheInitializedStaticStorage(),
           method->GetDexCacheInitializedStaticStorage());
  CHECK_EQ(prototype->GetDexMethodIndex(), method->GetDexMethodIndex());

  MethodHelper mh(method);
  MethodHelper mh2(prototype.get());
  CHECK_STREQ(mh.GetName(), mh2.GetName());
  CHECK_STREQ(mh.GetShorty(), mh2.GetShorty());
  // More complex sanity - via dex cache
  CHECK_EQ(mh.GetReturnType(), mh2.GetReturnType());
}

bool ClassLinker::InitializeClass(Class* klass, bool can_run_clinit, bool can_init_statics) {
  CHECK(klass->IsResolved() || klass->IsErroneous())
      << PrettyClass(klass) << ": state=" << klass->GetStatus();

  Thread* self = Thread::Current();

  AbstractMethod* clinit = NULL;
  {
    // see JLS 3rd edition, 12.4.2 "Detailed Initialization Procedure" for the locking protocol
    ObjectLock lock(self, klass);

    if (klass->GetStatus() == Class::kStatusInitialized) {
      return true;
    }

    if (klass->IsErroneous()) {
      ThrowEarlierClassFailure(klass);
      return false;
    }

    if (klass->GetStatus() == Class::kStatusResolved ||
        klass->GetStatus() == Class::kStatusRetryVerificationAtRuntime) {
      VerifyClass(klass);
      if (klass->GetStatus() != Class::kStatusVerified) {
        if (klass->GetStatus() == Class::kStatusError) {
          CHECK(self->IsExceptionPending());
        }
        return false;
      }
    }

    clinit = klass->FindDeclaredDirectMethod("<clinit>", "()V");
    if (clinit != NULL && !can_run_clinit) {
      // if the class has a <clinit> but we can't run it during compilation,
      // don't bother going to kStatusInitializing. We return false so that
      // sub-classes don't believe this class is initialized.
      // Opportunistically link non-static methods, TODO: don't initialize and dirty pages
      // in second pass.
      return false;
    }

    // If the class is kStatusInitializing, either this thread is
    // initializing higher up the stack or another thread has beat us
    // to initializing and we need to wait. Either way, this
    // invocation of InitializeClass will not be responsible for
    // running <clinit> and will return.
    if (klass->GetStatus() == Class::kStatusInitializing) {
      // We caught somebody else in the act; was it us?
      if (klass->GetClinitThreadId() == self->GetTid()) {
        // Yes. That's fine. Return so we can continue initializing.
        return true;
      }
      // No. That's fine. Wait for another thread to finish initializing.
      return WaitForInitializeClass(klass, self, lock);
    }

    if (!ValidateSuperClassDescriptors(klass)) {
      klass->SetStatus(Class::kStatusError);
      lock.NotifyAll();
      return false;
    }

    DCHECK_EQ(klass->GetStatus(), Class::kStatusVerified) << PrettyClass(klass);

    klass->SetClinitThreadId(self->GetTid());
    klass->SetStatus(Class::kStatusInitializing);
  }

  uint64_t t0 = NanoTime();

  if (!InitializeSuperClass(klass, can_run_clinit, can_init_statics)) {
    // Super class initialization failed, this can be because we can't run
    // super-class class initializers in which case we'll be verified.
    // Otherwise this class is erroneous.
    if (!can_run_clinit) {
      CHECK(klass->IsVerified());
    } else {
      CHECK(klass->IsErroneous());
    }
    // Signal to any waiting threads that saw this class as initializing.
    ObjectLock lock(self, klass);
    lock.NotifyAll();
    return false;
  }

  bool has_static_field_initializers = InitializeStaticFields(klass);

  if (clinit != NULL) {
    if (Runtime::Current()->IsStarted()) {
      clinit->Invoke(self, NULL, NULL, NULL);
    } else {
      art::interpreter::EnterInterpreterFromInvoke(self, clinit, NULL, NULL, NULL);
    }
  }

  FixupStaticTrampolines(klass);

  uint64_t t1 = NanoTime();

  bool success = true;
  {
    ObjectLock lock(self, klass);

    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer();
      klass->SetStatus(Class::kStatusError);
      success = false;
    } else {
      RuntimeStats* global_stats = Runtime::Current()->GetStats();
      RuntimeStats* thread_stats = self->GetStats();
      ++global_stats->class_init_count;
      ++thread_stats->class_init_count;
      global_stats->class_init_time_ns += (t1 - t0);
      thread_stats->class_init_time_ns += (t1 - t0);
      // Set the class as initialized except if we can't initialize static fields and static field
      // initialization is necessary.
      if (!can_init_statics && has_static_field_initializers) {
        klass->SetStatus(Class::kStatusVerified);  // Don't leave class in initializing state.
        success = false;
      } else {
        klass->SetStatus(Class::kStatusInitialized);
      }
      if (VLOG_IS_ON(class_linker)) {
        ClassHelper kh(klass);
        LOG(INFO) << "Initialized class " << kh.GetDescriptor() << " from " << kh.GetLocation();
      }
    }
    lock.NotifyAll();
  }
  return success;
}

bool ClassLinker::WaitForInitializeClass(Class* klass, Thread* self, ObjectLock& lock)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  while (true) {
    self->AssertNoPendingException();
    lock.Wait();

    // When we wake up, repeat the test for init-in-progress.  If
    // there's an exception pending (only possible if
    // "interruptShouldThrow" was set), bail out.
    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer();
      klass->SetStatus(Class::kStatusError);
      return false;
    }
    // Spurious wakeup? Go back to waiting.
    if (klass->GetStatus() == Class::kStatusInitializing) {
      continue;
    }
    if (klass->GetStatus() == Class::kStatusVerified && Runtime::Current()->IsCompiler()) {
      // Compile time initialization failed.
      return false;
    }
    if (klass->IsErroneous()) {
      // The caller wants an exception, but it was thrown in a
      // different thread.  Synthesize one here.
      ThrowNoClassDefFoundError("<clinit> failed for class %s; see exception in other thread",
                                PrettyDescriptor(klass).c_str());
      return false;
    }
    if (klass->IsInitialized()) {
      return true;
    }
    LOG(FATAL) << "Unexpected class status. " << PrettyClass(klass) << " is " << klass->GetStatus();
  }
  LOG(FATAL) << "Not Reached" << PrettyClass(klass);
}

bool ClassLinker::ValidateSuperClassDescriptors(const Class* klass) {
  if (klass->IsInterface()) {
    return true;
  }
  // begin with the methods local to the superclass
  if (klass->HasSuperClass() &&
      klass->GetClassLoader() != klass->GetSuperClass()->GetClassLoader()) {
    const Class* super = klass->GetSuperClass();
    for (int i = super->GetVTable()->GetLength() - 1; i >= 0; --i) {
      const AbstractMethod* method = klass->GetVTable()->Get(i);
      if (method != super->GetVTable()->Get(i) &&
          !IsSameMethodSignatureInDifferentClassContexts(method, super, klass)) {
        ThrowLinkageError("Class %s method %s resolves differently in superclass %s",
                          PrettyDescriptor(klass).c_str(), PrettyMethod(method).c_str(),
                          PrettyDescriptor(super).c_str());
        return false;
      }
    }
  }
  IfTable* iftable = klass->GetIfTable();
  for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
    Class* interface = iftable->GetInterface(i);
    if (klass->GetClassLoader() != interface->GetClassLoader()) {
      for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
        const AbstractMethod* method = iftable->GetMethodArray(i)->Get(j);
        if (!IsSameMethodSignatureInDifferentClassContexts(method, interface,
                                                           method->GetDeclaringClass())) {
          ThrowLinkageError("Class %s method %s resolves differently in interface %s",
                            PrettyDescriptor(method->GetDeclaringClass()).c_str(),
                            PrettyMethod(method).c_str(),
                            PrettyDescriptor(interface).c_str());
          return false;
        }
      }
    }
  }
  return true;
}

// Returns true if classes referenced by the signature of the method are the
// same classes in klass1 as they are in klass2.
bool ClassLinker::IsSameMethodSignatureInDifferentClassContexts(const AbstractMethod* method,
                                                                const Class* klass1,
                                                                const Class* klass2) {
  if (klass1 == klass2) {
    return true;
  }
  const DexFile& dex_file = *method->GetDeclaringClass()->GetDexCache()->GetDexFile();
  const DexFile::ProtoId& proto_id =
      dex_file.GetMethodPrototype(dex_file.GetMethodId(method->GetDexMethodIndex()));
  for (DexFileParameterIterator it(dex_file, proto_id); it.HasNext(); it.Next()) {
    const char* descriptor = it.GetDescriptor();
    if (descriptor == NULL) {
      break;
    }
    if (descriptor[0] == 'L' || descriptor[0] == '[') {
      // Found a non-primitive type.
      if (!IsSameDescriptorInDifferentClassContexts(descriptor, klass1, klass2)) {
        return false;
      }
    }
  }
  // Check the return type
  const char* descriptor = dex_file.GetReturnTypeDescriptor(proto_id);
  if (descriptor[0] == 'L' || descriptor[0] == '[') {
    if (!IsSameDescriptorInDifferentClassContexts(descriptor, klass1, klass2)) {
      return false;
    }
  }
  return true;
}

// Returns true if the descriptor resolves to the same class in the context of klass1 and klass2.
bool ClassLinker::IsSameDescriptorInDifferentClassContexts(const char* descriptor,
                                                           const Class* klass1,
                                                           const Class* klass2) {
  CHECK(descriptor != NULL);
  CHECK(klass1 != NULL);
  CHECK(klass2 != NULL);
  if (klass1 == klass2) {
    return true;
  }
  Class* found1 = FindClass(descriptor, klass1->GetClassLoader());
  if (found1 == NULL) {
    Thread::Current()->ClearException();
  }
  Class* found2 = FindClass(descriptor, klass2->GetClassLoader());
  if (found2 == NULL) {
    Thread::Current()->ClearException();
  }
  return found1 == found2;
}

bool ClassLinker::InitializeSuperClass(Class* klass, bool can_run_clinit, bool can_init_fields) {
  CHECK(klass != NULL);
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    Class* super_class = klass->GetSuperClass();
    if (!super_class->IsInitialized()) {
      CHECK(!super_class->IsInterface());
      // Must hold lock on object when initializing and setting status.
      ObjectLock lock(Thread::Current(), klass);
      bool super_initialized = InitializeClass(super_class, can_run_clinit, can_init_fields);
      // TODO: check for a pending exception
      if (!super_initialized) {
        if (!can_run_clinit) {
          // Don't set status to error when we can't run <clinit>.
          CHECK_EQ(klass->GetStatus(), Class::kStatusInitializing) << PrettyClass(klass);
          klass->SetStatus(Class::kStatusVerified);
          return false;
        }
        klass->SetStatus(Class::kStatusError);
        klass->NotifyAll();
        return false;
      }
    }
  }
  return true;
}

bool ClassLinker::EnsureInitialized(Class* c, bool can_run_clinit, bool can_init_fields) {
  DCHECK(c != NULL);
  if (c->IsInitialized()) {
    return true;
  }

  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, kRunnable);
  bool success = InitializeClass(c, can_run_clinit, can_init_fields);
  if (!success) {
    CHECK(self->IsExceptionPending() || !can_run_clinit) << PrettyClass(c);
  }
  return success;
}

void ClassLinker::ConstructFieldMap(const DexFile& dex_file, const DexFile::ClassDef& dex_class_def,
                                    Class* c, SafeMap<uint32_t, Field*>& field_map) {
  ClassLoader* cl = c->GetClassLoader();
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  ClassDataItemIterator it(dex_file, class_data);
  for (size_t i = 0; it.HasNextStaticField(); i++, it.Next()) {
    field_map.Put(i, ResolveField(dex_file, it.GetMemberIndex(), c->GetDexCache(), cl, true));
  }
}

bool ClassLinker::InitializeStaticFields(Class* klass) {
  size_t num_static_fields = klass->NumStaticFields();
  if (num_static_fields == 0) {
    return false;
  }
  DexCache* dex_cache = klass->GetDexCache();
  // TODO: this seems like the wrong check. do we really want !IsPrimitive && !IsArray?
  if (dex_cache == NULL) {
    return false;
  }
  ClassHelper kh(klass);
  const DexFile::ClassDef* dex_class_def = kh.GetClassDef();
  CHECK(dex_class_def != NULL);
  const DexFile& dex_file = kh.GetDexFile();
  EncodedStaticFieldValueIterator it(dex_file, dex_cache, klass->GetClassLoader(),
                                     this, *dex_class_def);

  if (it.HasNext()) {
    // We reordered the fields, so we need to be able to map the field indexes to the right fields.
    SafeMap<uint32_t, Field*> field_map;
    ConstructFieldMap(dex_file, *dex_class_def, klass, field_map);
    for (size_t i = 0; it.HasNext(); i++, it.Next()) {
      it.ReadValueToField(field_map.Get(i));
    }
    return true;
  }
  return false;
}

bool ClassLinker::LinkClass(SirtRef<Class>& klass, ObjectArray<Class>* interfaces) {
  CHECK_EQ(Class::kStatusLoaded, klass->GetStatus());
  if (!LinkSuperClass(klass)) {
    return false;
  }
  if (!LinkMethods(klass, interfaces)) {
    return false;
  }
  if (!LinkInstanceFields(klass)) {
    return false;
  }
  if (!LinkStaticFields(klass)) {
    return false;
  }
  CreateReferenceInstanceOffsets(klass);
  CreateReferenceStaticOffsets(klass);
  CHECK_EQ(Class::kStatusLoaded, klass->GetStatus());
  klass->SetStatus(Class::kStatusResolved);
  return true;
}

bool ClassLinker::LoadSuperAndInterfaces(SirtRef<Class>& klass, const DexFile& dex_file) {
  CHECK_EQ(Class::kStatusIdx, klass->GetStatus());
  StringPiece descriptor(dex_file.StringByTypeIdx(klass->GetDexTypeIndex()));
  const DexFile::ClassDef* class_def = dex_file.FindClassDef(descriptor);
  CHECK(class_def != NULL);
  uint16_t super_class_idx = class_def->superclass_idx_;
  if (super_class_idx != DexFile::kDexNoIndex16) {
    Class* super_class = ResolveType(dex_file, super_class_idx, klass.get());
    if (super_class == NULL) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return false;
    }
    // Verify
    if (!klass->CanAccess(super_class)) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
          "Class %s extended by class %s is inaccessible",
          PrettyDescriptor(super_class).c_str(),
          PrettyDescriptor(klass.get()).c_str());
      return false;
    }
    klass->SetSuperClass(super_class);
  }
  const DexFile::TypeList* interfaces = dex_file.GetInterfacesList(*class_def);
  if (interfaces != NULL) {
    for (size_t i = 0; i < interfaces->Size(); i++) {
      uint16_t idx = interfaces->GetTypeItem(i).type_idx_;
      Class* interface = ResolveType(dex_file, idx, klass.get());
      if (interface == NULL) {
        DCHECK(Thread::Current()->IsExceptionPending());
        return false;
      }
      // Verify
      if (!klass->CanAccess(interface)) {
        // TODO: the RI seemed to ignore this in my testing.
        Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
            "Interface %s implemented by class %s is inaccessible",
            PrettyDescriptor(interface).c_str(),
            PrettyDescriptor(klass.get()).c_str());
        return false;
      }
    }
  }
  // Mark the class as loaded.
  klass->SetStatus(Class::kStatusLoaded);
  return true;
}

bool ClassLinker::LinkSuperClass(SirtRef<Class>& klass) {
  CHECK(!klass->IsPrimitive());
  Class* super = klass->GetSuperClass();
  if (klass.get() == GetClassRoot(kJavaLangObject)) {
    if (super != NULL) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/ClassFormatError;",
          "java.lang.Object must not have a superclass");
      return false;
    }
    return true;
  }
  if (super == NULL) {
    ThrowLinkageError("No superclass defined for class %s", PrettyDescriptor(klass.get()).c_str());
    return false;
  }
  // Verify
  if (super->IsFinal() || super->IsInterface()) {
    Thread* self = Thread::Current();
    self->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
        "Superclass %s of %s is %s",
        PrettyDescriptor(super).c_str(),
        PrettyDescriptor(klass.get()).c_str(),
        super->IsFinal() ? "declared final" : "an interface");
    return false;
  }
  if (!klass->CanAccess(super)) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
        "Superclass %s is inaccessible by %s",
        PrettyDescriptor(super).c_str(),
        PrettyDescriptor(klass.get()).c_str());
    return false;
  }

  // Inherit kAccClassIsFinalizable from the superclass in case this class doesn't override finalize.
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
    ThrowLinkageError("Class %s attempts to subclass java.lang.ref.Reference, which is not allowed",
        PrettyDescriptor(klass.get()).c_str());
    return false;
  }

#ifndef NDEBUG
  // Ensure super classes are fully resolved prior to resolving fields..
  while (super != NULL) {
    CHECK(super->IsResolved());
    super = super->GetSuperClass();
  }
#endif
  return true;
}

// Populate the class vtable and itable. Compute return type indices.
bool ClassLinker::LinkMethods(SirtRef<Class>& klass, ObjectArray<Class>* interfaces) {
  if (klass->IsInterface()) {
    // No vtable.
    size_t count = klass->NumVirtualMethods();
    if (!IsUint(16, count)) {
      ThrowClassFormatError("Too many methods on interface: %zd", count);
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      klass->GetVirtualMethodDuringLinking(i)->SetMethodIndex(i);
    }
    // Link interface method tables
    return LinkInterfaceMethods(klass, interfaces);
  } else {
    // Link virtual and interface method tables
    return LinkVirtualMethods(klass) && LinkInterfaceMethods(klass, interfaces);
  }
  return true;
}

bool ClassLinker::LinkVirtualMethods(SirtRef<Class>& klass) {
  Thread* self = Thread::Current();
  if (klass->HasSuperClass()) {
    uint32_t max_count = klass->NumVirtualMethods() + klass->GetSuperClass()->GetVTable()->GetLength();
    size_t actual_count = klass->GetSuperClass()->GetVTable()->GetLength();
    CHECK_LE(actual_count, max_count);
    // TODO: do not assign to the vtable field until it is fully constructed.
    SirtRef<ObjectArray<AbstractMethod> >
      vtable(self, klass->GetSuperClass()->GetVTable()->CopyOf(self, max_count));
    // See if any of our virtual methods override the superclass.
    MethodHelper local_mh(NULL, this);
    MethodHelper super_mh(NULL, this);
    for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
      AbstractMethod* local_method = klass->GetVirtualMethodDuringLinking(i);
      local_mh.ChangeMethod(local_method);
      size_t j = 0;
      for (; j < actual_count; ++j) {
        AbstractMethod* super_method = vtable->Get(j);
        super_mh.ChangeMethod(super_method);
        if (local_mh.HasSameNameAndSignature(&super_mh)) {
          if (klass->CanAccessMember(super_method->GetDeclaringClass(), super_method->GetAccessFlags())) {
            if (super_method->IsFinal()) {
              ThrowLinkageError("Method %s overrides final method in class %s",
                                PrettyMethod(local_method).c_str(),
                                super_mh.GetDeclaringClassDescriptor());
              return false;
            }
            vtable->Set(j, local_method);
            local_method->SetMethodIndex(j);
            break;
          } else {
            LOG(WARNING) << "Before Android 4.1, method " << PrettyMethod(local_method)
                         << " would have incorrectly overridden the package-private method in "
                         << PrettyDescriptor(super_mh.GetDeclaringClassDescriptor());
          }
        }
      }
      if (j == actual_count) {
        // Not overriding, append.
        vtable->Set(actual_count, local_method);
        local_method->SetMethodIndex(actual_count);
        actual_count += 1;
      }
    }
    if (!IsUint(16, actual_count)) {
      ThrowClassFormatError("Too many methods defined on class: %zd", actual_count);
      return false;
    }
    // Shrink vtable if possible
    CHECK_LE(actual_count, max_count);
    if (actual_count < max_count) {
      vtable.reset(vtable->CopyOf(self, actual_count));
    }
    klass->SetVTable(vtable.get());
  } else {
    CHECK(klass.get() == GetClassRoot(kJavaLangObject));
    uint32_t num_virtual_methods = klass->NumVirtualMethods();
    if (!IsUint(16, num_virtual_methods)) {
      ThrowClassFormatError("Too many methods: %d", num_virtual_methods);
      return false;
    }
    SirtRef<ObjectArray<AbstractMethod> >
        vtable(self, AllocMethodArray(self, num_virtual_methods));
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      AbstractMethod* virtual_method = klass->GetVirtualMethodDuringLinking(i);
      vtable->Set(i, virtual_method);
      virtual_method->SetMethodIndex(i & 0xFFFF);
    }
    klass->SetVTable(vtable.get());
  }
  return true;
}

bool ClassLinker::LinkInterfaceMethods(SirtRef<Class>& klass, ObjectArray<Class>* interfaces) {
  size_t super_ifcount;
  if (klass->HasSuperClass()) {
    super_ifcount = klass->GetSuperClass()->GetIfTableCount();
  } else {
    super_ifcount = 0;
  }
  size_t ifcount = super_ifcount;
  ClassHelper kh(klass.get(), this);
  uint32_t num_interfaces = interfaces == NULL ? kh.NumDirectInterfaces() : interfaces->GetLength();
  ifcount += num_interfaces;
  for (size_t i = 0; i < num_interfaces; i++) {
    Class* interface = interfaces == NULL ? kh.GetDirectInterface(i) : interfaces->Get(i);
    ifcount += interface->GetIfTableCount();
  }
  if (ifcount == 0) {
    // Class implements no interfaces.
    DCHECK_EQ(klass->GetIfTableCount(), 0);
    DCHECK(klass->GetIfTable() == NULL);
    return true;
  }
  if (ifcount == super_ifcount) {
    // Class implements same interfaces as parent, are any of these not marker interfaces?
    bool has_non_marker_interface = false;
    IfTable* super_iftable = klass->GetSuperClass()->GetIfTable();
    for (size_t i = 0; i < ifcount; ++i) {
      if (super_iftable->GetMethodArrayCount(i) > 0) {
        has_non_marker_interface = true;
        break;
      }
    }
    if (!has_non_marker_interface) {
      // Class just inherits marker interfaces from parent so recycle parent's iftable.
      klass->SetIfTable(super_iftable);
      return true;
    }
  }
  Thread* self = Thread::Current();
  SirtRef<IfTable> iftable(self, AllocIfTable(self, ifcount));
  if (super_ifcount != 0) {
    IfTable* super_iftable = klass->GetSuperClass()->GetIfTable();
    for (size_t i = 0; i < super_ifcount; i++) {
      Class* super_interface = super_iftable->GetInterface(i);
      iftable->SetInterface(i, super_interface);
    }
  }
  // Flatten the interface inheritance hierarchy.
  size_t idx = super_ifcount;
  for (size_t i = 0; i < num_interfaces; i++) {
    Class* interface = interfaces == NULL ? kh.GetDirectInterface(i) : interfaces->Get(i);
    DCHECK(interface != NULL);
    if (!interface->IsInterface()) {
      ClassHelper ih(interface);
      self->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
          "Class %s implements non-interface class %s",
          PrettyDescriptor(klass.get()).c_str(),
          PrettyDescriptor(ih.GetDescriptor()).c_str());
      return false;
    }
    // Check if interface is already in iftable
    bool duplicate = false;
    for (size_t j = 0; j < idx; j++) {
      Class* existing_interface = iftable->GetInterface(j);
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
        Class* super_interface = interface->GetIfTable()->GetInterface(j);
        bool super_duplicate = false;
        for (size_t k = 0; k < idx; k++) {
          Class* existing_interface = iftable->GetInterface(k);
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
  // Shrink iftable in case duplicates were found
  if (idx < ifcount) {
    iftable.reset(down_cast<IfTable*>(iftable->CopyOf(self, idx * IfTable::kMax)));
    ifcount = idx;
  } else {
    CHECK_EQ(idx, ifcount);
  }
  klass->SetIfTable(iftable.get());

  // If we're an interface, we don't need the vtable pointers, so we're done.
  if (klass->IsInterface()) {
    return true;
  }
  std::vector<AbstractMethod*> miranda_list;
  MethodHelper vtable_mh(NULL, this);
  MethodHelper interface_mh(NULL, this);
  for (size_t i = 0; i < ifcount; ++i) {
    Class* interface = iftable->GetInterface(i);
    size_t num_methods = interface->NumVirtualMethods();
    if (num_methods > 0) {
      ObjectArray<AbstractMethod>* method_array = AllocMethodArray(self, num_methods);
      iftable->SetMethodArray(i, method_array);
      ObjectArray<AbstractMethod>* vtable = klass->GetVTableDuringLinking();
      for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
        AbstractMethod* interface_method = interface->GetVirtualMethod(j);
        interface_mh.ChangeMethod(interface_method);
        int32_t k;
        // For each method listed in the interface's method list, find the
        // matching method in our class's method list.  We want to favor the
        // subclass over the superclass, which just requires walking
        // back from the end of the vtable.  (This only matters if the
        // superclass defines a private method and this class redefines
        // it -- otherwise it would use the same vtable slot.  In .dex files
        // those don't end up in the virtual method table, so it shouldn't
        // matter which direction we go.  We walk it backward anyway.)
        for (k = vtable->GetLength() - 1; k >= 0; --k) {
          AbstractMethod* vtable_method = vtable->Get(k);
          vtable_mh.ChangeMethod(vtable_method);
          if (interface_mh.HasSameNameAndSignature(&vtable_mh)) {
            if (!vtable_method->IsPublic()) {
              self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                                       "Implementation not public: %s",
                                       PrettyMethod(vtable_method).c_str());
              return false;
            }
            method_array->Set(j, vtable_method);
            break;
          }
        }
        if (k < 0) {
          SirtRef<AbstractMethod> miranda_method(self, NULL);
          for (size_t mir = 0; mir < miranda_list.size(); mir++) {
            AbstractMethod* mir_method = miranda_list[mir];
            vtable_mh.ChangeMethod(mir_method);
            if (interface_mh.HasSameNameAndSignature(&vtable_mh)) {
              miranda_method.reset(miranda_list[mir]);
              break;
            }
          }
          if (miranda_method.get() == NULL) {
            // point the interface table at a phantom slot
            miranda_method.reset(down_cast<AbstractMethod*>(interface_method->Clone(self)));
            miranda_list.push_back(miranda_method.get());
          }
          method_array->Set(j, miranda_method.get());
        }
      }
    }
  }
  if (!miranda_list.empty()) {
    int old_method_count = klass->NumVirtualMethods();
    int new_method_count = old_method_count + miranda_list.size();
    klass->SetVirtualMethods((old_method_count == 0)
                             ? AllocMethodArray(self, new_method_count)
                             : klass->GetVirtualMethods()->CopyOf(self, new_method_count));

    SirtRef<ObjectArray<AbstractMethod> > vtable(self, klass->GetVTableDuringLinking());
    CHECK(vtable.get() != NULL);
    int old_vtable_count = vtable->GetLength();
    int new_vtable_count = old_vtable_count + miranda_list.size();
    vtable.reset(vtable->CopyOf(self, new_vtable_count));
    for (size_t i = 0; i < miranda_list.size(); ++i) {
      AbstractMethod* method = miranda_list[i];
      // Leave the declaring class alone as type indices are relative to it
      method->SetAccessFlags(method->GetAccessFlags() | kAccMiranda);
      method->SetMethodIndex(0xFFFF & (old_vtable_count + i));
      klass->SetVirtualMethod(old_method_count + i, method);
      vtable->Set(old_vtable_count + i, method);
    }
    // TODO: do not assign to the vtable field until it is fully constructed.
    klass->SetVTable(vtable.get());
  }

  ObjectArray<AbstractMethod>* vtable = klass->GetVTableDuringLinking();
  for (int i = 0; i < vtable->GetLength(); ++i) {
    CHECK(vtable->Get(i) != NULL);
  }

//  klass->DumpClass(std::cerr, Class::kDumpClassFullDetail);

  return true;
}

bool ClassLinker::LinkInstanceFields(SirtRef<Class>& klass) {
  CHECK(klass.get() != NULL);
  return LinkFields(klass, false);
}

bool ClassLinker::LinkStaticFields(SirtRef<Class>& klass) {
  CHECK(klass.get() != NULL);
  size_t allocated_class_size = klass->GetClassSize();
  bool success = LinkFields(klass, true);
  CHECK_EQ(allocated_class_size, klass->GetClassSize());
  return success;
}

struct LinkFieldsComparator {
  explicit LinkFieldsComparator(FieldHelper* fh)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : fh_(fh) {}
  // No thread safety analysis as will be called from STL. Checked lock held in constructor.
  bool operator()(const Field* field1, const Field* field2) NO_THREAD_SAFETY_ANALYSIS {
    // First come reference fields, then 64-bit, and finally 32-bit
    fh_->ChangeField(field1);
    Primitive::Type type1 = fh_->GetTypeAsPrimitiveType();
    fh_->ChangeField(field2);
    Primitive::Type type2 = fh_->GetTypeAsPrimitiveType();
    bool isPrimitive1 = type1 != Primitive::kPrimNot;
    bool isPrimitive2 = type2 != Primitive::kPrimNot;
    bool is64bit1 = isPrimitive1 && (type1 == Primitive::kPrimLong || type1 == Primitive::kPrimDouble);
    bool is64bit2 = isPrimitive2 && (type2 == Primitive::kPrimLong || type2 == Primitive::kPrimDouble);
    int order1 = (!isPrimitive1 ? 0 : (is64bit1 ? 1 : 2));
    int order2 = (!isPrimitive2 ? 0 : (is64bit2 ? 1 : 2));
    if (order1 != order2) {
      return order1 < order2;
    }

    // same basic group? then sort by string.
    fh_->ChangeField(field1);
    StringPiece name1(fh_->GetName());
    fh_->ChangeField(field2);
    StringPiece name2(fh_->GetName());
    return name1 < name2;
  }

  FieldHelper* fh_;
};

bool ClassLinker::LinkFields(SirtRef<Class>& klass, bool is_static) {
  size_t num_fields =
      is_static ? klass->NumStaticFields() : klass->NumInstanceFields();

  ObjectArray<Field>* fields =
      is_static ? klass->GetSFields() : klass->GetIFields();

  // Initialize size and field_offset
  size_t size;
  MemberOffset field_offset(0);
  if (is_static) {
    size = klass->GetClassSize();
    field_offset = Class::FieldsOffset();
  } else {
    Class* super_class = klass->GetSuperClass();
    if (super_class != NULL) {
      CHECK(super_class->IsResolved());
      field_offset = MemberOffset(super_class->GetObjectSize());
    }
    size = field_offset.Uint32Value();
  }

  CHECK_EQ(num_fields == 0, fields == NULL);

  // we want a relatively stable order so that adding new fields
  // minimizes disruption of C++ version such as Class and Method.
  std::deque<Field*> grouped_and_sorted_fields;
  for (size_t i = 0; i < num_fields; i++) {
    grouped_and_sorted_fields.push_back(fields->Get(i));
  }
  FieldHelper fh(NULL, this);
  std::sort(grouped_and_sorted_fields.begin(),
            grouped_and_sorted_fields.end(),
            LinkFieldsComparator(&fh));

  // References should be at the front.
  size_t current_field = 0;
  size_t num_reference_fields = 0;
  for (; current_field < num_fields; current_field++) {
    Field* field = grouped_and_sorted_fields.front();
    fh.ChangeField(field);
    Primitive::Type type = fh.GetTypeAsPrimitiveType();
    bool isPrimitive = type != Primitive::kPrimNot;
    if (isPrimitive) {
      break; // past last reference, move on to the next phase
    }
    grouped_and_sorted_fields.pop_front();
    num_reference_fields++;
    fields->Set(current_field, field);
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() + sizeof(uint32_t));
  }

  // Now we want to pack all of the double-wide fields together.  If
  // we're not aligned, though, we want to shuffle one 32-bit field
  // into place.  If we can't find one, we'll have to pad it.
  if (current_field != num_fields && !IsAligned<8>(field_offset.Uint32Value())) {
    for (size_t i = 0; i < grouped_and_sorted_fields.size(); i++) {
      Field* field = grouped_and_sorted_fields[i];
      fh.ChangeField(field);
      Primitive::Type type = fh.GetTypeAsPrimitiveType();
      CHECK(type != Primitive::kPrimNot);  // should only be working on primitive types
      if (type == Primitive::kPrimLong || type == Primitive::kPrimDouble) {
        continue;
      }
      fields->Set(current_field++, field);
      field->SetOffset(field_offset);
      // drop the consumed field
      grouped_and_sorted_fields.erase(grouped_and_sorted_fields.begin() + i);
      break;
    }
    // whether we found a 32-bit field for padding or not, we advance
    field_offset = MemberOffset(field_offset.Uint32Value() + sizeof(uint32_t));
  }

  // Alignment is good, shuffle any double-wide fields forward, and
  // finish assigning field offsets to all fields.
  DCHECK(current_field == num_fields || IsAligned<8>(field_offset.Uint32Value()));
  while (!grouped_and_sorted_fields.empty()) {
    Field* field = grouped_and_sorted_fields.front();
    grouped_and_sorted_fields.pop_front();
    fh.ChangeField(field);
    Primitive::Type type = fh.GetTypeAsPrimitiveType();
    CHECK(type != Primitive::kPrimNot);  // should only be working on primitive types
    fields->Set(current_field, field);
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() +
                                ((type == Primitive::kPrimLong || type == Primitive::kPrimDouble)
                                 ? sizeof(uint64_t)
                                 : sizeof(uint32_t)));
    current_field++;
  }

  // We lie to the GC about the java.lang.ref.Reference.referent field, so it doesn't scan it.
  if (!is_static &&
      StringPiece(ClassHelper(klass.get(), this).GetDescriptor()) == "Ljava/lang/ref/Reference;") {
    // We know there are no non-reference fields in the Reference classes, and we know
    // that 'referent' is alphabetically last, so this is easy...
    CHECK_EQ(num_reference_fields, num_fields);
    fh.ChangeField(fields->Get(num_fields - 1));
    CHECK_STREQ(fh.GetName(), "referent");
    --num_reference_fields;
  }

#ifndef NDEBUG
  // Make sure that all reference fields appear before
  // non-reference fields, and all double-wide fields are aligned.
  bool seen_non_ref = false;
  for (size_t i = 0; i < num_fields; i++) {
    Field* field = fields->Get(i);
    if (false) {  // enable to debug field layout
      LOG(INFO) << "LinkFields: " << (is_static ? "static" : "instance")
                << " class=" << PrettyClass(klass.get())
                << " field=" << PrettyField(field)
                << " offset=" << field->GetField32(MemberOffset(Field::OffsetOffset()), false);
    }
    fh.ChangeField(field);
    Primitive::Type type = fh.GetTypeAsPrimitiveType();
    bool is_primitive = type != Primitive::kPrimNot;
    if (StringPiece(ClassHelper(klass.get(), this).GetDescriptor()) == "Ljava/lang/ref/Reference;" &&
        StringPiece(fh.GetName()) == "referent") {
      is_primitive = true; // We lied above, so we have to expect a lie here.
    }
    if (is_primitive) {
      if (!seen_non_ref) {
        seen_non_ref = true;
        DCHECK_EQ(num_reference_fields, i);
      }
    } else {
      DCHECK(!seen_non_ref);
    }
  }
  if (!seen_non_ref) {
    DCHECK_EQ(num_fields, num_reference_fields);
  }
#endif
  size = field_offset.Uint32Value();
  // Update klass
  if (is_static) {
    klass->SetNumReferenceStaticFields(num_reference_fields);
    klass->SetClassSize(size);
  } else {
    klass->SetNumReferenceInstanceFields(num_reference_fields);
    if (!klass->IsVariableSize()) {
      klass->SetObjectSize(size);
    }
  }
  return true;
}

//  Set the bitmap of reference offsets, refOffsets, from the ifields
//  list.
void ClassLinker::CreateReferenceInstanceOffsets(SirtRef<Class>& klass) {
  uint32_t reference_offsets = 0;
  Class* super_class = klass->GetSuperClass();
  if (super_class != NULL) {
    reference_offsets = super_class->GetReferenceInstanceOffsets();
    // If our superclass overflowed, we don't stand a chance.
    if (reference_offsets == CLASS_WALK_SUPER) {
      klass->SetReferenceInstanceOffsets(reference_offsets);
      return;
    }
  }
  CreateReferenceOffsets(klass, false, reference_offsets);
}

void ClassLinker::CreateReferenceStaticOffsets(SirtRef<Class>& klass) {
  CreateReferenceOffsets(klass, true, 0);
}

void ClassLinker::CreateReferenceOffsets(SirtRef<Class>& klass, bool is_static,
                                         uint32_t reference_offsets) {
  size_t num_reference_fields =
      is_static ? klass->NumReferenceStaticFieldsDuringLinking()
                : klass->NumReferenceInstanceFieldsDuringLinking();
  const ObjectArray<Field>* fields =
      is_static ? klass->GetSFields() : klass->GetIFields();
  // All of the fields that contain object references are guaranteed
  // to be at the beginning of the fields list.
  for (size_t i = 0; i < num_reference_fields; ++i) {
    // Note that byte_offset is the offset from the beginning of
    // object, not the offset into instance data
    const Field* field = fields->Get(i);
    MemberOffset byte_offset = field->GetOffsetDuringLinking();
    CHECK_EQ(byte_offset.Uint32Value() & (CLASS_OFFSET_ALIGNMENT - 1), 0U);
    if (CLASS_CAN_ENCODE_OFFSET(byte_offset.Uint32Value())) {
      uint32_t new_bit = CLASS_BIT_FROM_OFFSET(byte_offset.Uint32Value());
      CHECK_NE(new_bit, 0U);
      reference_offsets |= new_bit;
    } else {
      reference_offsets = CLASS_WALK_SUPER;
      break;
    }
  }
  // Update fields in klass
  if (is_static) {
    klass->SetReferenceStaticOffsets(reference_offsets);
  } else {
    klass->SetReferenceInstanceOffsets(reference_offsets);
  }
}

String* ClassLinker::ResolveString(const DexFile& dex_file,
    uint32_t string_idx, DexCache* dex_cache) {
  DCHECK(dex_cache != NULL);
  String* resolved = dex_cache->GetResolvedString(string_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::StringId& string_id = dex_file.GetStringId(string_idx);
  int32_t utf16_length = dex_file.GetStringLength(string_id);
  const char* utf8_data = dex_file.GetStringData(string_id);
  String* string = intern_table_->InternStrong(utf16_length, utf8_data);
  dex_cache->SetResolvedString(string_idx, string);
  return string;
}

Class* ClassLinker::ResolveType(const DexFile& dex_file,
                                uint16_t type_idx,
                                DexCache* dex_cache,
                                ClassLoader* class_loader) {
  DCHECK(dex_cache != NULL);
  Class* resolved = dex_cache->GetResolvedType(type_idx);
  if (resolved == NULL) {
    const char* descriptor = dex_file.StringByTypeIdx(type_idx);
    resolved = FindClass(descriptor, class_loader);
    if (resolved != NULL) {
      // TODO: we used to throw here if resolved's class loader was not the
      //       boot class loader. This was to permit different classes with the
      //       same name to be loaded simultaneously by different loaders
      dex_cache->SetResolvedType(type_idx, resolved);
    } else {
      CHECK(Thread::Current()->IsExceptionPending())
          << "Expected pending exception for failed resolution of: " << descriptor;
      // Convert a ClassNotFoundException to a NoClassDefFoundError
      if (Thread::Current()->GetException()->InstanceOf(GetClassRoot(kJavaLangClassNotFoundException))) {
        Thread::Current()->ClearException();
        ThrowNoClassDefFoundError("Failed resolution of: %s", descriptor);
      }
    }
  }
  return resolved;
}

AbstractMethod* ClassLinker::ResolveMethod(const DexFile& dex_file,
                                   uint32_t method_idx,
                                   DexCache* dex_cache,
                                   ClassLoader* class_loader,
                                   const AbstractMethod* referrer,
                                   InvokeType type) {
  DCHECK(dex_cache != NULL);
  // Check for hit in the dex cache.
  AbstractMethod* resolved = dex_cache->GetResolvedMethod(method_idx);
  if (resolved != NULL) {
    return resolved;
  }
  // Fail, get the declaring class.
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  Class* klass = ResolveType(dex_file, method_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  // Scan using method_idx, this saves string compares but will only hit for matching dex
  // caches/files.
  switch (type) {
    case kDirect:  // Fall-through.
    case kStatic:
      resolved = klass->FindDirectMethod(dex_cache, method_idx);
      break;
    case kInterface:
      resolved = klass->FindInterfaceMethod(dex_cache, method_idx);
      break;
    case kSuper:  // Fall-through.
    case kVirtual:
      resolved = klass->FindVirtualMethod(dex_cache, method_idx);
      break;
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << type;
  }
  if (resolved == NULL) {
    // Search by name, which works across dex files.
    const char* name = dex_file.StringDataByIdx(method_id.name_idx_);
    std::string signature(dex_file.CreateMethodSignature(method_id.proto_idx_, NULL));
    switch (type) {
      case kDirect:  // Fall-through.
      case kStatic:
        resolved = klass->FindDirectMethod(name, signature);
        break;
      case kInterface:
        resolved = klass->FindInterfaceMethod(name, signature);
        break;
      case kSuper:  // Fall-through.
      case kVirtual:
        resolved = klass->FindVirtualMethod(name, signature);
        break;
    }
  }
  if (resolved != NULL) {
    // We found a method, check for incompatible class changes.
    if (resolved->CheckIncompatibleClassChange(type)) {
      resolved = NULL;
    }
  }
  if (resolved != NULL) {
    // Be a good citizen and update the dex cache to speed subsequent calls.
    dex_cache->SetResolvedMethod(method_idx, resolved);
    return resolved;
  } else {
    // We failed to find the method which means either an access error, an incompatible class
    // change, or no such method. First try to find the method among direct and virtual methods.
    const char* name = dex_file.StringDataByIdx(method_id.name_idx_);
    std::string signature(dex_file.CreateMethodSignature(method_id.proto_idx_, NULL));
    switch (type) {
      case kDirect:
      case kStatic:
        resolved = klass->FindVirtualMethod(name, signature);
        break;
      case kInterface:
      case kVirtual:
      case kSuper:
        resolved = klass->FindDirectMethod(name, signature);
        break;
    }

    // If we found something, check that it can be accessed by the referrer.
    if (resolved != NULL && referrer != NULL) {
      Class* methods_class = resolved->GetDeclaringClass();
      Class* referring_class = referrer->GetDeclaringClass();
      if (!referring_class->CanAccess(methods_class)) {
        ThrowIllegalAccessErrorClassForMethodDispatch(referring_class, methods_class,
                                                      referrer, resolved, type);
        return NULL;
      } else if (!referring_class->CanAccessMember(methods_class,
                                                   resolved->GetAccessFlags())) {
        ThrowIllegalAccessErrorMethod(referring_class, resolved);
        return NULL;
      }
    }

    // Otherwise, throw an IncompatibleClassChangeError if we found something, and check interface
    // methods and throw if we find the method there. If we find nothing, throw a NoSuchMethodError.
    switch (type) {
      case kDirect:
      case kStatic:
        if (resolved != NULL) {
          ThrowIncompatibleClassChangeError(type, kVirtual, resolved, referrer);
        } else {
          resolved = klass->FindInterfaceMethod(name, signature);
          if (resolved != NULL) {
            ThrowIncompatibleClassChangeError(type, kInterface, resolved, referrer);
          } else {
            ThrowNoSuchMethodError(type, klass, name, signature, referrer);
          }
        }
        break;
      case kInterface:
        if (resolved != NULL) {
          ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer);
        } else {
          resolved = klass->FindVirtualMethod(name, signature);
          if (resolved != NULL) {
            ThrowIncompatibleClassChangeError(type, kVirtual, resolved, referrer);
          } else {
            ThrowNoSuchMethodError(type, klass, name, signature, referrer);
          }
        }
        break;
      case kSuper:
        ThrowNoSuchMethodError(type, klass, name, signature, referrer);
        break;
      case kVirtual:
        if (resolved != NULL) {
          ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer);
        } else {
          resolved = klass->FindInterfaceMethod(name, signature);
          if (resolved != NULL) {
            ThrowIncompatibleClassChangeError(type, kInterface, resolved, referrer);
          } else {
            ThrowNoSuchMethodError(type, klass, name, signature, referrer);
          }
        }
        break;
    }
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
}

Field* ClassLinker::ResolveField(const DexFile& dex_file,
                                 uint32_t field_idx,
                                 DexCache* dex_cache,
                                 ClassLoader* class_loader,
                                 bool is_static) {
  DCHECK(dex_cache != NULL);
  Field* resolved = dex_cache->GetResolvedField(field_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  Class* klass = ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }

  if (is_static) {
    resolved = klass->FindStaticField(dex_cache, field_idx);
  } else {
    resolved = klass->FindInstanceField(dex_cache, field_idx);
  }

  if (resolved == NULL) {
    const char* name = dex_file.GetFieldName(field_id);
    const char* type = dex_file.GetFieldTypeDescriptor(field_id);
    if (is_static) {
      resolved = klass->FindStaticField(name, type);
    } else {
      resolved = klass->FindInstanceField(name, type);
    }
    if (resolved == NULL) {
      ThrowNoSuchFieldError(is_static ? "static " : "instance ", klass, type, name);
      return NULL;
    }
  }
  dex_cache->SetResolvedField(field_idx, resolved);
  return resolved;
}

Field* ClassLinker::ResolveFieldJLS(const DexFile& dex_file,
                                    uint32_t field_idx,
                                    DexCache* dex_cache,
                                    ClassLoader* class_loader) {
  DCHECK(dex_cache != NULL);
  Field* resolved = dex_cache->GetResolvedField(field_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  Class* klass = ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }

  const char* name = dex_file.GetFieldName(field_id);
  const char* type = dex_file.GetFieldTypeDescriptor(field_id);
  resolved = klass->FindField(name, type);
  if (resolved != NULL) {
    dex_cache->SetResolvedField(field_idx, resolved);
  } else {
    ThrowNoSuchFieldError("", klass, type, name);
  }
  return resolved;
}

const char* ClassLinker::MethodShorty(uint32_t method_idx, AbstractMethod* referrer, uint32_t* length) {
  Class* declaring_class = referrer->GetDeclaringClass();
  DexCache* dex_cache = declaring_class->GetDexCache();
  const DexFile& dex_file = *dex_cache->GetDexFile();
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  return dex_file.GetMethodShorty(method_id, length);
}

void ClassLinker::DumpAllClasses(int flags) const {
  // TODO: at the time this was written, it wasn't safe to call PrettyField with the ClassLinker
  // lock held, because it might need to resolve a field's type, which would try to take the lock.
  std::vector<Class*> all_classes;
  {
    MutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
    typedef Table::const_iterator It;  // TODO: C++0x auto
    for (It it = classes_.begin(), end = classes_.end(); it != end; ++it) {
      all_classes.push_back(it->second);
    }
    for (It it = image_classes_.begin(), end = image_classes_.end(); it != end; ++it) {
      all_classes.push_back(it->second);
    }
  }

  for (size_t i = 0; i < all_classes.size(); ++i) {
    all_classes[i]->DumpClass(std::cerr, flags);
  }
}

void ClassLinker::DumpForSigQuit(std::ostream& os) const {
  MutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  os << "Loaded classes: " << image_classes_.size() << " image classes; "
     << classes_.size() << " allocated classes\n";
}

size_t ClassLinker::NumLoadedClasses() const {
  MutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  return classes_.size() + image_classes_.size();
}

pid_t ClassLinker::GetClassesLockOwner() {
  return Locks::classlinker_classes_lock_->GetExclusiveOwnerTid();
}

pid_t ClassLinker::GetDexLockOwner() {
  return dex_lock_.GetExclusiveOwnerTid();
}

void ClassLinker::SetClassRoot(ClassRoot class_root, Class* klass) {
  DCHECK(!init_done_);

  DCHECK(klass != NULL);
  DCHECK(klass->GetClassLoader() == NULL);

  DCHECK(class_roots_ != NULL);
  DCHECK(class_roots_->Get(class_root) == NULL);
  class_roots_->Set(class_root, klass);
}

void ClassLinker::RelocateExecutable() {
  MutexLock mu(Thread::Current(), dex_lock_);
  for (size_t i = 0; i < oat_files_.size(); ++i) {
    const_cast<OatFile*>(oat_files_[i])->RelocateExecutable();
  }
}

}  // namespace art
