// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"

#include <string>
#include <utility>
#include <vector>

#include "UniquePtr.h"
#include "casts.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "dex_verifier.h"
#include "heap.h"
#include "intern_table.h"
#include "logging.h"
#include "monitor.h"
#include "object.h"
#include "runtime.h"
#include "space.h"
#include "thread.h"
#include "utils.h"

namespace art {

const char* ClassLinker::class_roots_descriptors_[kClassRootsMax] = {
  "Ljava/lang/Class;",
  "Ljava/lang/Object;",
  "[Ljava/lang/Object;",
  "Ljava/lang/String;",
  "Ljava/lang/reflect/Field;",
  "Ljava/lang/reflect/Method;",
  "Ljava/lang/ClassLoader;",
  "Ldalvik/system/BaseDexClassLoader;",
  "Ldalvik/system/PathClassLoader;",
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

ClassLinker* ClassLinker::Create(const std::vector<const DexFile*>& boot_class_path,
    InternTable* intern_table, Space* space) {
  UniquePtr<ClassLinker> class_linker(new ClassLinker(intern_table));
  if (space == NULL) {
    class_linker->Init(boot_class_path);
  } else {
    class_linker->Init(boot_class_path, space);
  }
  // TODO: check for failure during initialization
  return class_linker.release();
}

ClassLinker::ClassLinker(InternTable* intern_table)
    : classes_lock_(Mutex::Create("ClassLinker::Lock")),
      class_roots_(NULL),
      array_interfaces_(NULL),
      array_iftable_(NULL),
      init_done_(false),
      intern_table_(intern_table) {
}

void ClassLinker::Init(const std::vector<const DexFile*>& boot_class_path) {
  CHECK(!init_done_);

  // java_lang_Class comes first, its needed for AllocClass
  Class* java_lang_Class = down_cast<Class*>(
      Heap::AllocObject(NULL, sizeof(ClassClass)));
  CHECK(java_lang_Class != NULL);
  java_lang_Class->SetClass(java_lang_Class);
  java_lang_Class->SetClassSize(sizeof(ClassClass));
  // AllocClass(Class*) can now be used

  // java_lang_Object comes next so that object_array_class can be created
  Class* java_lang_Object = AllocClass(java_lang_Class, sizeof(Class));
  CHECK(java_lang_Object != NULL);
  // backfill Object as the super class of Class
  java_lang_Class->SetSuperClass(java_lang_Object);
  java_lang_Object->SetStatus(Class::kStatusLoaded);

  // Object[] next to hold class roots
  Class* object_array_class = AllocClass(java_lang_Class, sizeof(Class));
  object_array_class->SetArrayRank(1);
  object_array_class->SetComponentType(java_lang_Object);


  // Setup the char[] class to be used for String
  Class* char_array_class = AllocClass(java_lang_Class, sizeof(Class));
  char_array_class->SetArrayRank(1);
  CharArray::SetArrayClass(char_array_class);

  // Setup String
  Class* java_lang_String = AllocClass(java_lang_Class, sizeof(StringClass));
  String::SetClass(java_lang_String);
  java_lang_String->SetObjectSize(sizeof(String));
  java_lang_String->SetStatus(Class::kStatusResolved);

  // Backfill Class descriptors missing until this point
  // TODO: intern these strings
  java_lang_Class->SetDescriptor(
      String::AllocFromModifiedUtf8("Ljava/lang/Class;"));
  java_lang_Object->SetDescriptor(
      String::AllocFromModifiedUtf8("Ljava/lang/Object;"));
  object_array_class->SetDescriptor(
      String::AllocFromModifiedUtf8("[Ljava/lang/Object;"));
  java_lang_String->SetDescriptor(
      String::AllocFromModifiedUtf8("Ljava/lang/String;"));
  char_array_class->SetDescriptor(String::AllocFromModifiedUtf8("[C"));

  // Create storage for root classes, save away our work so far (requires
  // descriptors)
  class_roots_ = ObjectArray<Class>::Alloc(object_array_class, kClassRootsMax);
  SetClassRoot(kJavaLangClass, java_lang_Class);
  SetClassRoot(kJavaLangObject, java_lang_Object);
  SetClassRoot(kObjectArrayClass, object_array_class);
  SetClassRoot(kCharArrayClass, char_array_class);
  SetClassRoot(kJavaLangString, java_lang_String);

  // Setup the primitive type classes.
  SetClassRoot(kPrimitiveBoolean, CreatePrimitiveClass("Z", Class::kPrimBoolean));
  SetClassRoot(kPrimitiveByte, CreatePrimitiveClass("B", Class::kPrimByte));
  SetClassRoot(kPrimitiveChar, CreatePrimitiveClass("C", Class::kPrimChar));
  SetClassRoot(kPrimitiveShort, CreatePrimitiveClass("S", Class::kPrimShort));
  SetClassRoot(kPrimitiveInt, CreatePrimitiveClass("I", Class::kPrimInt));
  SetClassRoot(kPrimitiveLong, CreatePrimitiveClass("J", Class::kPrimLong));
  SetClassRoot(kPrimitiveFloat, CreatePrimitiveClass("F", Class::kPrimFloat));
  SetClassRoot(kPrimitiveDouble, CreatePrimitiveClass("D", Class::kPrimDouble));
  SetClassRoot(kPrimitiveVoid, CreatePrimitiveClass("V", Class::kPrimVoid));

  // Backfill component type of char[]
  char_array_class->SetComponentType(GetClassRoot(kPrimitiveChar));

  // Create array interface entries to populate once we can load system classes
  array_interfaces_ = AllocObjectArray<Class>(2);
  array_iftable_ = new InterfaceEntry[2];

  // Create int array type for AllocDexCache (done in AppendToBootClassPath)
  Class* int_array_class = AllocClass(java_lang_Class, sizeof(Class));
  int_array_class->SetArrayRank(1);
  int_array_class->SetDescriptor(String::AllocFromModifiedUtf8("[I"));
  int_array_class->SetComponentType(GetClassRoot(kPrimitiveInt));
  IntArray::SetArrayClass(int_array_class);
  SetClassRoot(kIntArrayClass, int_array_class);

  // now that these are registered, we can use AllocClass() and AllocObjectArray

  // setup boot_class_path_ now that we can use AllocObjectArray to
  // create DexCache instances
  for (size_t i = 0; i != boot_class_path.size(); ++i) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    AppendToBootClassPath(*dex_file);
  }

  // Field and Method are necessary so that FindClass can link members
  Class* java_lang_reflect_Field = AllocClass(java_lang_Class, sizeof(FieldClass));
  CHECK(java_lang_reflect_Field != NULL);
  java_lang_reflect_Field->SetDescriptor(String::AllocFromModifiedUtf8("Ljava/lang/reflect/Field;"));
  java_lang_reflect_Field->SetObjectSize(sizeof(Field));
  SetClassRoot(kJavaLangReflectField, java_lang_reflect_Field);
  java_lang_reflect_Field->SetStatus(Class::kStatusResolved);
  Field::SetClass(java_lang_reflect_Field);

  Class* java_lang_reflect_Method = AllocClass(java_lang_Class, sizeof(MethodClass));
  java_lang_reflect_Method->SetDescriptor(String::AllocFromModifiedUtf8("Ljava/lang/reflect/Method;"));
  CHECK(java_lang_reflect_Method != NULL);
  java_lang_reflect_Method->SetObjectSize(sizeof(Method));
  SetClassRoot(kJavaLangReflectMethod, java_lang_reflect_Method);
  java_lang_reflect_Method->SetStatus(Class::kStatusResolved);
  Method::SetClass(java_lang_reflect_Method);

  // now we can use FindSystemClass

  // Object and String just need more minimal setup, since they do not have
  // extra C++ fields.
  java_lang_Object->SetStatus(Class::kStatusNotReady);
  Class* Object_class = FindSystemClass("Ljava/lang/Object;");
  CHECK_EQ(java_lang_Object, Object_class);
  CHECK_EQ(java_lang_Object->GetObjectSize(), sizeof(Object));
  java_lang_String->SetStatus(Class::kStatusNotReady);
  Class* String_class = FindSystemClass("Ljava/lang/String;");
  CHECK_EQ(java_lang_String, String_class);
  CHECK_EQ(java_lang_String->GetObjectSize(), sizeof(String));

  // Setup the primitive array type classes - can't be done until Object has
  // a vtable
  SetClassRoot(kBooleanArrayClass, FindSystemClass("[Z"));
  BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));

  SetClassRoot(kByteArrayClass, FindSystemClass("[B"));
  ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));

  Class* found_char_array_class = FindSystemClass("[C");
  CHECK_EQ(char_array_class, found_char_array_class);

  SetClassRoot(kShortArrayClass, FindSystemClass("[S"));
  ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));

  Class* found_int_array_class = FindSystemClass("[I");
  CHECK_EQ(int_array_class, found_int_array_class);

  SetClassRoot(kLongArrayClass, FindSystemClass("[J"));
  LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));

  SetClassRoot(kFloatArrayClass, FindSystemClass("[F"));
  FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));

  SetClassRoot(kDoubleArrayClass, FindSystemClass("[D"));
  DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));

  Class* found_object_array_class = FindSystemClass("[Ljava/lang/Object;");
  CHECK_EQ(object_array_class, found_object_array_class);

  // Setup the single, global copies of "interfaces" and "iftable"
  Class* java_lang_Cloneable = FindSystemClass("Ljava/lang/Cloneable;");
  CHECK(java_lang_Cloneable != NULL);
  Class* java_io_Serializable = FindSystemClass("Ljava/io/Serializable;");
  CHECK(java_io_Serializable != NULL);
  CHECK(array_interfaces_ != NULL);
  array_interfaces_->Set(0, java_lang_Cloneable);
  array_interfaces_->Set(1, java_io_Serializable);
  // We assume that Cloneable/Serializable don't have superinterfaces --
  // normally we'd have to crawl up and explicitly list all of the
  // supers as well.  These interfaces don't have any methods, so we
  // don't have to worry about the ifviPool either.
  array_iftable_[0].SetInterface(array_interfaces_->Get(0));
  array_iftable_[1].SetInterface(array_interfaces_->Get(1));

  // Sanity check Object[]'s interfaces
  CHECK_EQ(java_lang_Cloneable, object_array_class->GetInterface(0));
  CHECK_EQ(java_io_Serializable, object_array_class->GetInterface(1));

  // run Class, Field, and Method through FindSystemClass.
  // this initializes their dex_cache_ fields and register them in classes_.
  // we also override their object_size_ values to accommodate the extra C++ fields.
  Class* Class_class = FindSystemClass("Ljava/lang/Class;");
  CHECK_EQ(java_lang_Class, Class_class);
  // No sanity check on size as Class is variably sized

  java_lang_reflect_Field->SetStatus(Class::kStatusNotReady);
  Class* Field_class = FindSystemClass("Ljava/lang/reflect/Field;");
  CHECK_EQ(java_lang_reflect_Field, Field_class);
  CHECK_LT(java_lang_reflect_Field->GetObjectSize(), sizeof(Field));
  java_lang_reflect_Field->SetObjectSize(sizeof(Field));

  java_lang_reflect_Method->SetStatus(Class::kStatusNotReady);
  Class* Method_class = FindSystemClass("Ljava/lang/reflect/Method;");
  CHECK_EQ(java_lang_reflect_Method, Method_class);
  CHECK_LT(java_lang_reflect_Method->GetObjectSize(), sizeof(Method));
  java_lang_reflect_Method->SetObjectSize(sizeof(Method));

  // java.lang.ref classes need to be specially flagged, but otherwise are normal classes
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

  // Let the heap know some key offsets into java.lang.ref instances
  // NB we hard code the field indexes here rather than using FindInstanceField
  // as the types of the field can't be resolved prior to the runtime being
  // fully initialized
  Class* java_lang_ref_Reference = FindSystemClass("Ljava/lang/ref/Reference;");

  Field* pendingNext = java_lang_ref_Reference->GetInstanceField(0);
  CHECK(pendingNext->GetName()->Equals("pendingNext"));
  CHECK(ResolveType(pendingNext->GetTypeIdx(), pendingNext) ==
        java_lang_ref_Reference);

  Field* queue = java_lang_ref_Reference->GetInstanceField(1);
  CHECK(queue->GetName()->Equals("queue"));
  CHECK(ResolveType(queue->GetTypeIdx(), queue) ==
        FindSystemClass("Ljava/lang/ref/ReferenceQueue;"));

  Field* queueNext = java_lang_ref_Reference->GetInstanceField(2);
  CHECK(queueNext->GetName()->Equals("queueNext"));
  CHECK(ResolveType(queueNext->GetTypeIdx(), queueNext) ==
        java_lang_ref_Reference);

  Field* referent = java_lang_ref_Reference->GetInstanceField(3);
  CHECK(referent->GetName()->Equals("referent"));
  CHECK(ResolveType(referent->GetTypeIdx(), referent) ==
        java_lang_Object);

  Field* zombie = java_lang_ref_FinalizerReference->GetInstanceField(2);
  CHECK(zombie->GetName()->Equals("zombie"));
  CHECK(ResolveType(zombie->GetTypeIdx(), zombie) ==
        java_lang_Object);

  Heap::SetReferenceOffsets(referent->GetOffset(),
                            queue->GetOffset(),
                            queueNext->GetOffset(),
                            pendingNext->GetOffset(),
                            zombie->GetOffset());

  // Setup the ClassLoaders, adjusting the object_size_ as necessary
  Class* java_lang_ClassLoader = FindSystemClass("Ljava/lang/ClassLoader;");
  CHECK_LT(java_lang_ClassLoader->GetObjectSize(), sizeof(ClassLoader));
  java_lang_ClassLoader->SetObjectSize(sizeof(ClassLoader));
  SetClassRoot(kJavaLangClassLoader, java_lang_ClassLoader);

  Class* dalvik_system_BaseDexClassLoader = FindSystemClass("Ldalvik/system/BaseDexClassLoader;");
  CHECK_EQ(dalvik_system_BaseDexClassLoader->GetObjectSize(), sizeof(BaseDexClassLoader));
  SetClassRoot(kDalvikSystemBaseDexClassLoader, dalvik_system_BaseDexClassLoader);

  Class* dalvik_system_PathClassLoader = FindSystemClass("Ldalvik/system/PathClassLoader;");
  CHECK_EQ(dalvik_system_PathClassLoader->GetObjectSize(), sizeof(PathClassLoader));
  SetClassRoot(kDalvikSystemPathClassLoader, dalvik_system_PathClassLoader);
  PathClassLoader::SetClass(dalvik_system_PathClassLoader);

  // Set up java.lang.StackTraceElement as a convenience
  SetClassRoot(kJavaLangStackTraceElement, FindSystemClass("Ljava/lang/StackTraceElement;"));
  SetClassRoot(kJavaLangStackTraceElementArrayClass, FindSystemClass("[Ljava/lang/StackTraceElement;"));
  StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit();
}

void ClassLinker::FinishInit() {
  // ensure all class_roots_ are initialized
  for (size_t i = 0; i < kClassRootsMax; i++) {
    ClassRoot class_root = static_cast<ClassRoot>(i);
    Class* klass = GetClassRoot(class_root);
    CHECK(klass != NULL);
    DCHECK(klass->IsArrayClass() || klass->IsPrimitive() || klass->GetDexCache() != NULL);
    // note SetClassRoot does additional validation.
    // if possible add new checks there to catch errors early
  }

  // disable the slow paths in FindClass and CreatePrimitiveClass now
  // that Object, Class, and Object[] are setup
  init_done_ = true;
}

struct ClassLinker::InitCallbackState {
  ClassLinker* class_linker;

  Class* class_roots[kClassRootsMax];

  typedef std::tr1::unordered_map<std::string, ClassRoot> Table;
  Table descriptor_to_class_root;

  struct DexCacheHash {
    size_t operator()(art::DexCache* const& obj) const {
      return reinterpret_cast<size_t>(&obj);
    }
  };
  typedef std::tr1::unordered_set<DexCache*, DexCacheHash> Set;
  Set dex_caches;
};

void ClassLinker::Init(const std::vector<const DexFile*>& boot_class_path, Space* space) {
  CHECK(!init_done_);

  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);

  InitCallbackState state;
  state.class_linker = this;
  for (size_t i = 0; i < kClassRootsMax; i++) {
    ClassRoot class_root = static_cast<ClassRoot>(i);
    state.descriptor_to_class_root[GetClassRootDescriptor(class_root)] = class_root;
  }

  // reinit clases_ table
  heap_bitmap->Walk(InitCallback, &state);

  // reinit class_roots_
  Class* object_array_class = state.class_roots[kObjectArrayClass];
  class_roots_ = ObjectArray<Class>::Alloc(object_array_class, kClassRootsMax);
  for (size_t i = 0; i < kClassRootsMax; i++) {
    ClassRoot class_root = static_cast<ClassRoot>(i);
    SetClassRoot(class_root, state.class_roots[class_root]);
  }

  // reinit intern table
  ObjectArray<Object>* interned_array = space->GetImageHeader().GetInternedArray();
  for (int32_t i = 0; i < interned_array->GetLength(); i++) {
    String* string = interned_array->Get(i)->AsString();
    intern_table_->RegisterStrong(string);
  }

  // reinit array_interfaces_ from any array class instance, they should all be ==
  array_interfaces_ = GetClassRoot(kObjectArrayClass)->GetInterfaces();
  DCHECK(array_interfaces_ == GetClassRoot(kBooleanArrayClass)->GetInterfaces());

  // build a map from location to DexCache to match up with DexFile::GetLocation
  std::tr1::unordered_map<std::string, DexCache*> location_to_dex_cache;
  typedef InitCallbackState::Set::const_iterator It;  // TODO: C++0x auto
  for (It it = state.dex_caches.begin(), end = state.dex_caches.end(); it != end; ++it) {
    DexCache* dex_cache = *it;
    std::string location = dex_cache->GetLocation()->ToModifiedUtf8();
    location_to_dex_cache[location] = dex_cache;
  }

  // reinit boot_class_path with DexFile arguments and found DexCaches
  for (size_t i = 0; i != boot_class_path.size(); ++i) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    DexCache* dex_cache = location_to_dex_cache[dex_file->GetLocation()];
    AppendToBootClassPath(*dex_file, dex_cache);
  }
  String::SetClass(GetClassRoot(kJavaLangString));
  Field::SetClass(GetClassRoot(kJavaLangReflectField));
  Method::SetClass(GetClassRoot(kJavaLangReflectMethod));
  BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));
  ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));
  CharArray::SetArrayClass(GetClassRoot(kCharArrayClass));
  DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));
  FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));
  IntArray::SetArrayClass(GetClassRoot(kIntArrayClass));
  LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));
  ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));
  PathClassLoader::SetClass(GetClassRoot(kDalvikSystemPathClassLoader));
  StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit();
}

void ClassLinker::InitCallback(Object* obj, void *arg) {
  DCHECK(obj != NULL);
  DCHECK(arg != NULL);
  InitCallbackState* state = reinterpret_cast<InitCallbackState*>(arg);

  if (!obj->IsClass()) {
    return;
  }
  Class* klass = obj->AsClass();
  CHECK(klass->GetClassLoader() == NULL);

  std::string descriptor = klass->GetDescriptor()->ToModifiedUtf8();

  // restore class to ClassLinker::classes_ table
  state->class_linker->InsertClass(descriptor, klass);

  // note DexCache to match with DexFile later
  DexCache* dex_cache = klass->GetDexCache();
  if (dex_cache != NULL) {
    state->dex_caches.insert(dex_cache);
  } else {
    DCHECK(klass->IsArrayClass() || klass->IsPrimitive());
  }

  // check if this is a root, if so, register it
  typedef InitCallbackState::Table::const_iterator It;  // TODO: C++0x auto
  It it = state->descriptor_to_class_root.find(descriptor);
  if (it != state->descriptor_to_class_root.end()) {
    ClassRoot class_root = it->second;
    state->class_roots[class_root] = klass;
  }
}

// Keep in sync with InitCallback. Anything we visit, we need to
// reinit references to when reinitializing a ClassLinker from a
// mapped image.
void ClassLinker::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  visitor(class_roots_, arg);

  for (size_t i = 0; i < dex_caches_.size(); i++) {
    visitor(dex_caches_[i], arg);
  }

  {
    MutexLock mu(classes_lock_);
    typedef Table::const_iterator It;  // TODO: C++0x auto
    for (It it = classes_.begin(), end = classes_.end(); it != end; ++it) {
      visitor(it->second, arg);
    }
  }

  visitor(array_interfaces_, arg);
}

ClassLinker::~ClassLinker() {
  delete classes_lock_;
  String::ResetClass();
  Field::ResetClass();
  Method::ResetClass();
  BooleanArray::ResetArrayClass();
  ByteArray::ResetArrayClass();
  CharArray::ResetArrayClass();
  DoubleArray::ResetArrayClass();
  FloatArray::ResetArrayClass();
  IntArray::ResetArrayClass();
  LongArray::ResetArrayClass();
  ShortArray::ResetArrayClass();
  PathClassLoader::ResetClass();
  StackTraceElement::ResetClass();
}

DexCache* ClassLinker::AllocDexCache(const DexFile& dex_file) {
  DexCache* dex_cache = down_cast<DexCache*>(AllocObjectArray<Object>(DexCache::LengthAsArray()));
  dex_cache->Init(String::AllocFromModifiedUtf8(dex_file.GetLocation().c_str()),
                  AllocObjectArray<String>(dex_file.NumStringIds()),
                  AllocObjectArray<Class>(dex_file.NumTypeIds()),
                  AllocObjectArray<Method>(dex_file.NumMethodIds()),
                  AllocObjectArray<Field>(dex_file.NumFieldIds()),
                  AllocCodeAndDirectMethods(dex_file.NumMethodIds()),
                  AllocObjectArray<StaticStorageBase>(dex_file.NumTypeIds()));
  return dex_cache;
}

CodeAndDirectMethods* ClassLinker::AllocCodeAndDirectMethods(size_t length) {
  return down_cast<CodeAndDirectMethods*>(IntArray::Alloc(CodeAndDirectMethods::LengthAsArray(length)));
}

Class* ClassLinker::AllocClass(Class* java_lang_Class, size_t class_size) {
  DCHECK_GE(class_size, sizeof(Class));
  Class* klass = Heap::AllocObject(java_lang_Class, class_size)->AsClass();
  klass->SetPrimitiveType(Class::kPrimNot);  // default to not being primitive
  klass->SetClassSize(class_size);
  return klass;
}

Class* ClassLinker::AllocClass(size_t class_size) {
  return AllocClass(GetClassRoot(kJavaLangClass), class_size);
}

Field* ClassLinker::AllocField() {
  return down_cast<Field*>(GetClassRoot(kJavaLangReflectField)->AllocObject());
}

Method* ClassLinker::AllocMethod() {
  return down_cast<Method*>(GetClassRoot(kJavaLangReflectMethod)->AllocObject());
}

ObjectArray<StackTraceElement>* ClassLinker::AllocStackTraceElementArray(size_t length) {
  return ObjectArray<StackTraceElement>::Alloc(
      GetClassRoot(kJavaLangStackTraceElementArrayClass),
      length);
}

Class* ClassLinker::FindClass(const StringPiece& descriptor,
                              const ClassLoader* class_loader) {
  // TODO: remove this contrived parent class loader check when we have a real ClassLoader.
  if (class_loader != NULL) {
    Class* klass = FindClass(descriptor, NULL);
    if (klass != NULL) {
      return klass;
    }
    Thread::Current()->ClearException();
  }

  Thread* self = Thread::Current();
  DCHECK(self != NULL);
  CHECK(!self->IsExceptionPending());
  // Find the class in the loaded classes table.
  Class* klass = LookupClass(descriptor, class_loader);
  if (klass == NULL) {
    // Class is not yet loaded.
    if (descriptor[0] == '[') {
      return CreateArrayClass(descriptor, class_loader);
    }
    const DexFile::ClassPath& class_path = ((class_loader != NULL)
                                            ? ClassLoader::GetClassPath(class_loader)
                                            : boot_class_path_);
    DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, class_path);
    if (pair.second == NULL) {
      std::string name(PrintableString(descriptor));
      self->ThrowNewException("Ljava/lang/NoClassDefFoundError;",
          "Class %s not found in class loader %p", name.c_str(), class_loader);
      return NULL;
    }
    const DexFile& dex_file = *pair.first;
    const DexFile::ClassDef& dex_class_def = *pair.second;
    DexCache* dex_cache = FindDexCache(dex_file);
    // Load the class from the dex file.
    if (!init_done_) {
      // finish up init of hand crafted class_roots_
      if (descriptor == "Ljava/lang/Object;") {
        klass = GetClassRoot(kJavaLangObject);
      } else if (descriptor == "Ljava/lang/Class;") {
        klass = GetClassRoot(kJavaLangClass);
      } else if (descriptor == "Ljava/lang/String;") {
        klass = GetClassRoot(kJavaLangString);
      } else if (descriptor == "Ljava/lang/reflect/Field;") {
        klass = GetClassRoot(kJavaLangReflectField);
      } else if (descriptor == "Ljava/lang/reflect/Method;") {
        klass = GetClassRoot(kJavaLangReflectMethod);
      } else {
        klass = AllocClass(SizeOfClass(dex_file, dex_class_def));
      }
    } else {
      klass = AllocClass(SizeOfClass(dex_file, dex_class_def));
    }
    if (!klass->IsLinked()) {
      klass->SetDexCache(dex_cache);
      LoadClass(dex_file, dex_class_def, klass, class_loader);
      // Check for a pending exception during load
      if (self->IsExceptionPending()) {
        // TODO: free native allocations in klass
        return NULL;
      }
      ObjectLock lock(klass);
      klass->SetClinitThreadId(self->GetId());
      // Add the newly loaded class to the loaded classes table.
      bool success = InsertClass(descriptor, klass);  // TODO: just return collision
      if (!success) {
        // We may fail to insert if we raced with another thread.
        klass->SetClinitThreadId(0);
        // TODO: free native allocations in klass
        klass = LookupClass(descriptor, class_loader);
        CHECK(klass != NULL);
        return klass;
      } else {
        // Finish loading (if necessary) by finding parents
        CHECK(!klass->IsLoaded());
        if (!LoadSuperAndInterfaces(klass, dex_file)) {
          // Loading failed.
          // TODO: CHECK(self->IsExceptionPending());
          lock.NotifyAll();
          return NULL;
        }
        CHECK(klass->IsLoaded());
        // Link the class (if necessary)
        CHECK(!klass->IsLinked());
        if (!LinkClass(klass)) {
          // Linking failed.
          // TODO: CHECK(self->IsExceptionPending());
          lock.NotifyAll();
          return NULL;
        }
        CHECK(klass->IsLinked());
      }
    }
  }
  // Link the class if it has not already been linked.
  if (!klass->IsLinked() && !klass->IsErroneous()) {
    ObjectLock lock(klass);
    // Check for circular dependencies between classes.
    if (!klass->IsLinked() && klass->GetClinitThreadId() == self->GetId()) {
      self->ThrowNewException("Ljava/lang/ClassCircularityError;", NULL); // TODO: detail
      return NULL;
    }
    // Wait for the pending initialization to complete.
    while (!klass->IsLinked() && !klass->IsErroneous()) {
      lock.Wait();
    }
  }
  if (klass->IsErroneous()) {
    LG << "EarlierClassFailure";  // TODO: EarlierClassFailure
    return NULL;
  }
  // Return the loaded class.  No exceptions should be pending.
  CHECK(klass->IsLinked());
  CHECK(!self->IsExceptionPending());
  return klass;
}

// Precomputes size that will be needed for Class, matching LinkStaticFields
size_t ClassLinker::SizeOfClass(const DexFile& dex_file,
                                const DexFile::ClassDef& dex_class_def) {
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  DexFile::ClassDataHeader header = dex_file.ReadClassDataHeader(&class_data);
  size_t num_static_fields = header.static_fields_size_;
  size_t num_ref = 0;
  size_t num_32 = 0;
  size_t num_64 = 0;
  if (num_static_fields != 0) {
    uint32_t last_idx = 0;
    for (size_t i = 0; i < num_static_fields; ++i) {
      DexFile::Field dex_field;
      dex_file.dexReadClassDataField(&class_data, &dex_field, &last_idx);
      const DexFile::FieldId& field_id = dex_file.GetFieldId(dex_field.field_idx_);
      const char* descriptor = dex_file.dexStringByTypeIdx(field_id.type_idx_);
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

void ClassLinker::LoadClass(const DexFile& dex_file,
                            const DexFile::ClassDef& dex_class_def,
                            Class* klass,
                            const ClassLoader* class_loader) {
  CHECK(klass != NULL);
  CHECK(klass->GetDexCache() != NULL);
  CHECK_EQ(Class::kStatusNotReady, klass->GetStatus());
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  DexFile::ClassDataHeader header = dex_file.ReadClassDataHeader(&class_data);

  const char* descriptor = dex_file.GetClassDescriptor(dex_class_def);
  CHECK(descriptor != NULL);

  klass->SetClass(GetClassRoot(kJavaLangClass));
  if (klass->GetDescriptor() != NULL) {
    DCHECK(klass->GetDescriptor()->Equals(descriptor));
  } else {
    klass->SetDescriptor(String::AllocFromModifiedUtf8(descriptor));
  }
  uint32_t access_flags = dex_class_def.access_flags_;
  // Make sure there aren't any "bonus" flags set, since we use them for runtime
  // state.
  CHECK_EQ(access_flags & ~kAccClassFlagsMask, 0U);
  klass->SetAccessFlags(access_flags);
  klass->SetClassLoader(class_loader);
  DCHECK(klass->GetPrimitiveType() == Class::kPrimNot);
  klass->SetStatus(Class::kStatusIdx);

  klass->SetSuperClassTypeIdx(dex_class_def.superclass_idx_);

  size_t num_static_fields = header.static_fields_size_;
  size_t num_instance_fields = header.instance_fields_size_;
  size_t num_direct_methods = header.direct_methods_size_;
  size_t num_virtual_methods = header.virtual_methods_size_;

  klass->SetSourceFile(dex_file.dexGetSourceFile(dex_class_def));

  // Load class interfaces.
  LoadInterfaces(dex_file, dex_class_def, klass);

  // Load static fields.
  if (num_static_fields != 0) {
    klass->SetSFields(AllocObjectArray<Field>(num_static_fields));
    uint32_t last_idx = 0;
    for (size_t i = 0; i < num_static_fields; ++i) {
      DexFile::Field dex_field;
      dex_file.dexReadClassDataField(&class_data, &dex_field, &last_idx);
      Field* sfield = AllocField();
      klass->SetStaticField(i, sfield);
      LoadField(dex_file, dex_field, klass, sfield);
    }
  }

  // Load instance fields.
  if (num_instance_fields != 0) {
    klass->SetIFields(AllocObjectArray<Field>(num_instance_fields));
    uint32_t last_idx = 0;
    for (size_t i = 0; i < num_instance_fields; ++i) {
      DexFile::Field dex_field;
      dex_file.dexReadClassDataField(&class_data, &dex_field, &last_idx);
      Field* ifield = AllocField();
      klass->SetInstanceField(i, ifield);
      LoadField(dex_file, dex_field, klass, ifield);
    }
  }

  // Load direct methods.
  if (num_direct_methods != 0) {
    // TODO: append direct methods to class object
    klass->SetDirectMethods(AllocObjectArray<Method>(num_direct_methods));
    uint32_t last_idx = 0;
    for (size_t i = 0; i < num_direct_methods; ++i) {
      DexFile::Method dex_method;
      dex_file.dexReadClassDataMethod(&class_data, &dex_method, &last_idx);
      Method* meth = AllocMethod();
      klass->SetDirectMethod(i, meth);
      LoadMethod(dex_file, dex_method, klass, meth);
      // TODO: register maps
    }
  }

  // Load virtual methods.
  if (num_virtual_methods != 0) {
    // TODO: append virtual methods to class object
    klass->SetVirtualMethods(AllocObjectArray<Method>(num_virtual_methods));
    uint32_t last_idx = 0;
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      DexFile::Method dex_method;
      dex_file.dexReadClassDataMethod(&class_data, &dex_method, &last_idx);
      Method* meth = AllocMethod();
      klass->SetVirtualMethod(i, meth);
      LoadMethod(dex_file, dex_method, klass, meth);
      // TODO: register maps
    }
  }
}

void ClassLinker::LoadInterfaces(const DexFile& dex_file,
                                 const DexFile::ClassDef& dex_class_def,
                                 Class* klass) {
  const DexFile::TypeList* list = dex_file.GetInterfacesList(dex_class_def);
  if (list != NULL) {
    klass->SetInterfaces(AllocObjectArray<Class>(list->Size()));
    IntArray* interfaces_idx = IntArray::Alloc(list->Size());
    klass->SetInterfacesTypeIdx(interfaces_idx);
    for (size_t i = 0; i < list->Size(); ++i) {
      const DexFile::TypeItem& type_item = list->GetTypeItem(i);
      interfaces_idx->Set(i, type_item.type_idx_);
    }
  }
}

void ClassLinker::LoadField(const DexFile& dex_file,
                            const DexFile::Field& src,
                            Class* klass,
                            Field* dst) {
  const DexFile::FieldId& field_id = dex_file.GetFieldId(src.field_idx_);
  dst->SetDeclaringClass(klass);
  dst->SetName(ResolveString(dex_file, field_id.name_idx_, klass->GetDexCache()));
  dst->SetTypeIdx(field_id.type_idx_);
  dst->SetAccessFlags(src.access_flags_);

  // In order to access primitive types using GetTypeDuringLinking we need to
  // ensure they are resolved into the dex cache
  const char* descriptor = dex_file.dexStringByTypeIdx(field_id.type_idx_);
  if (descriptor[1] == '\0') {
    // only the descriptors of primitive types should be 1 character long
    Class* resolved = ResolveType(dex_file, field_id.type_idx_, klass);
    DCHECK(resolved->IsPrimitive());
  }
}

void ClassLinker::LoadMethod(const DexFile& dex_file,
                             const DexFile::Method& src,
                             Class* klass,
                             Method* dst) {
  const DexFile::MethodId& method_id = dex_file.GetMethodId(src.method_idx_);
  dst->SetDeclaringClass(klass);
  dst->SetName(ResolveString(dex_file, method_id.name_idx_, klass->GetDexCache()));
  {
    int32_t utf16_length;
    std::string utf8(dex_file.CreateMethodDescriptor(method_id.proto_idx_, &utf16_length));
    dst->SetSignature(String::AllocFromModifiedUtf8(utf16_length, utf8.c_str()));
  }
  dst->SetProtoIdx(method_id.proto_idx_);
  dst->SetCodeItemOffset(src.code_off_);
  const char* shorty = dex_file.GetShorty(method_id.proto_idx_);
  dst->SetShorty(shorty);
  dst->SetAccessFlags(src.access_flags_);
  dst->SetReturnTypeIdx(dex_file.GetProtoId(method_id.proto_idx_).return_type_idx_);

  dst->SetDexCacheStrings(klass->GetDexCache()->GetStrings());
  dst->SetDexCacheResolvedTypes(klass->GetDexCache()->GetResolvedTypes());
  dst->SetDexCacheResolvedMethods(klass->GetDexCache()->GetResolvedMethods());
  dst->SetDexCacheResolvedFields(klass->GetDexCache()->GetResolvedFields());
  dst->SetDexCacheCodeAndDirectMethods(klass->GetDexCache()->GetCodeAndDirectMethods());
  dst->SetDexCacheInitializedStaticStorage(klass->GetDexCache()->GetInitializedStaticStorage());

  // TODO: check for finalize method

  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(src);
  if (code_item != NULL) {
    dst->SetNumRegisters(code_item->registers_size_);
    dst->SetNumIns(code_item->ins_size_);
    dst->SetNumOuts(code_item->outs_size_);
  } else {
    uint16_t num_args = Method::NumArgRegisters(shorty);
    if ((src.access_flags_ & kAccStatic) != 0) {
      ++num_args;
    }
    dst->SetNumRegisters(num_args);
    // TODO: native methods
  }
}

void ClassLinker::AppendToBootClassPath(const DexFile& dex_file) {
  AppendToBootClassPath(dex_file, AllocDexCache(dex_file));
}

void ClassLinker::AppendToBootClassPath(const DexFile& dex_file, DexCache* dex_cache) {
  CHECK(dex_cache != NULL);
  boot_class_path_.push_back(&dex_file);
  RegisterDexFile(dex_file, dex_cache);
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file) {
  RegisterDexFile(dex_file, AllocDexCache(dex_file));
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file, DexCache* dex_cache) {
  CHECK(dex_cache != NULL);
  dex_files_.push_back(&dex_file);
  dex_caches_.push_back(dex_cache);
}

const DexFile& ClassLinker::FindDexFile(const DexCache* dex_cache) const {
  for (size_t i = 0; i != dex_caches_.size(); ++i) {
    if (dex_caches_[i] == dex_cache) {
        return *dex_files_[i];
    }
  }
  CHECK(false) << "Could not find DexFile";
  return *dex_files_[-1];
}

DexCache* ClassLinker::FindDexCache(const DexFile& dex_file) const {
  for (size_t i = 0; i != dex_files_.size(); ++i) {
    if (dex_files_[i] == &dex_file) {
        return dex_caches_[i];
    }
  }
  CHECK(false) << "Could not find DexCache";
  return NULL;
}

Class* ClassLinker::CreatePrimitiveClass(const char* descriptor,
                                         Class::PrimitiveType type) {
  // TODO: deduce one argument from the other
  Class* klass = AllocClass(sizeof(Class));
  CHECK(klass != NULL);
  klass->SetAccessFlags(kAccPublic | kAccFinal | kAccAbstract);
  klass->SetDescriptor(String::AllocFromModifiedUtf8(descriptor));
  klass->SetPrimitiveType(type);
  klass->SetStatus(Class::kStatusInitialized);
  bool success = InsertClass(descriptor, klass);
  CHECK(success) << "CreatePrimitiveClass(" << descriptor << ") failed";
  return klass;
}

// Create an array class (i.e. the class object for the array, not the
// array itself).  "descriptor" looks like "[C" or "[[[[B" or
// "[Ljava/lang/String;".
//
// If "descriptor" refers to an array of primitives, look up the
// primitive type's internally-generated class object.
//
// "loader" is the class loader of the class that's referring to us.  It's
// used to ensure that we're looking for the element type in the right
// context.  It does NOT become the class loader for the array class; that
// always comes from the base element class.
//
// Returns NULL with an exception raised on failure.
Class* ClassLinker::CreateArrayClass(const StringPiece& descriptor,
                                     const ClassLoader* class_loader) {
  CHECK_EQ('[', descriptor[0]);

  // Identify the underlying element class and the array dimension depth.
  Class* component_type = NULL;
  int array_rank;
  if (descriptor[1] == '[') {
    // array of arrays; keep descriptor and grab stuff from parent
    Class* outer = FindClass(descriptor.substr(1), class_loader);
    if (outer != NULL) {
      // want the base class, not "outer", in our component_type
      component_type = outer->GetComponentType();
      array_rank = outer->GetArrayRank() + 1;
    } else {
      DCHECK(component_type == NULL);  // make sure we fail
    }
  } else {
    array_rank = 1;
    if (descriptor[1] == 'L') {
      // array of objects; strip off "[" and look up descriptor.
      const StringPiece subDescriptor = descriptor.substr(1);
      component_type = FindClass(subDescriptor, class_loader);
    } else {
      // array of a primitive type
      component_type = FindPrimitiveClass(descriptor[1]);
    }
  }

  if (component_type == NULL) {
    // failed
    // DCHECK(Thread::Current()->IsExceptionPending());  // TODO
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
    Class* new_class = LookupClass(descriptor, component_type->GetClassLoader());
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

  Class* new_class = NULL;
  if (!init_done_) {
    // Classes that were hand created, ie not by FindSystemClass
    if (descriptor == "[Ljava/lang/Object;") {
      new_class = GetClassRoot(kObjectArrayClass);
    } else if (descriptor == "[C") {
      new_class = GetClassRoot(kCharArrayClass);
    } else if (descriptor == "[I") {
      new_class = GetClassRoot(kIntArrayClass);
    }
  }
  if (new_class == NULL) {
    new_class = AllocClass(sizeof(Class));
    if (new_class == NULL) {
      return NULL;
    }
    new_class->SetArrayRank(array_rank);
    new_class->SetComponentType(component_type);
  }
  DCHECK_LE(1, new_class->GetArrayRank());
  DCHECK(new_class->GetComponentType() != NULL);
  new_class->SetDescriptor(String::AllocFromModifiedUtf8(descriptor.ToString().c_str()));
  Class* java_lang_Object = GetClassRoot(kJavaLangObject);
  new_class->SetSuperClass(java_lang_Object);
  new_class->SetVTable(java_lang_Object->GetVTable());
  new_class->SetPrimitiveType(Class::kPrimNot);
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
  new_class->SetInterfaces(array_interfaces_);
  new_class->SetIFTableCount(2);
  new_class->SetIFTable(array_iftable_);

  // Inherit access flags from the component type.  Arrays can't be
  // used as a superclass or interface, so we want to add "final"
  // and remove "interface".
  //
  // Don't inherit any non-standard flags (e.g., kAccFinal)
  // from component_type.  We assume that the array class does not
  // override finalize().
  new_class->SetAccessFlags(((new_class->GetComponentType()->GetAccessFlags() &
                             ~kAccInterface) | kAccFinal) & kAccJavaFlagsMask);

  if (InsertClass(descriptor, new_class)) {
    return new_class;
  }
  // Another thread must have loaded the class after we
  // started but before we finished.  Abandon what we've
  // done.
  //
  // (Yes, this happens.)

  // Grab the winning class.
  Class* other_class = LookupClass(descriptor, component_type->GetClassLoader());
  DCHECK(other_class != NULL);
  return other_class;
}

Class* ClassLinker::FindPrimitiveClass(char type) {
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
  }
  std::string printable_type(PrintableChar(type));
  Thread::Current()->ThrowNewException("Ljava/lang/NoClassDefFoundError;",
      "Not a primitive type: %s", printable_type.c_str());
  return NULL;
}

bool ClassLinker::InsertClass(const StringPiece& descriptor, Class* klass) {
  size_t hash = StringPieceHash()(descriptor);
  MutexLock mu(classes_lock_);
  Table::iterator it = classes_.insert(std::make_pair(hash, klass));
  return ((*it).second == klass);
}

Class* ClassLinker::LookupClass(const StringPiece& descriptor, const ClassLoader* class_loader) {
  size_t hash = StringPieceHash()(descriptor);
  MutexLock mu(classes_lock_);
  typedef Table::const_iterator It;  // TODO: C++0x auto
  for (It it = classes_.find(hash), end = classes_.end(); it != end; ++it) {
    Class* klass = it->second;
    if (klass->GetDescriptor()->Equals(descriptor) && klass->GetClassLoader() == class_loader) {
      return klass;
    }
  }
  return NULL;
}

bool ClassLinker::InitializeClass(Class* klass) {
  CHECK(klass->GetStatus() == Class::kStatusResolved ||
        klass->GetStatus() == Class::kStatusError);

  Thread* self = Thread::Current();

  {
    ObjectLock lock(klass);

    if (klass->GetStatus() < Class::kStatusVerified) {
      if (klass->IsErroneous()) {
        LG << "re-initializing failed class";  // TODO: throw
        return false;
      }

      CHECK(klass->GetStatus() == Class::kStatusResolved);

      klass->SetStatus(Class::kStatusVerifying);
      if (!DexVerify::VerifyClass(klass)) {
        LG << "Verification failed";  // TODO: ThrowVerifyError
        Object* exception = self->GetException();
        klass->SetVerifyErrorClass(exception->GetClass());
        klass->SetStatus(Class::kStatusError);
        return false;
      }

      klass->SetStatus(Class::kStatusVerified);
    }

    if (klass->GetStatus() == Class::kStatusInitialized) {
      return true;
    }

    while (klass->GetStatus() == Class::kStatusInitializing) {
      // we caught somebody else in the act; was it us?
      if (klass->GetClinitThreadId() == self->GetId()) {
        LG << "recursive <clinit>";
        return true;
      }

      CHECK(!self->IsExceptionPending());

      lock.Wait();  // TODO: check for interruption

      // When we wake up, repeat the test for init-in-progress.  If
      // there's an exception pending (only possible if
      // "interruptShouldThrow" was set), bail out.
      if (self->IsExceptionPending()) {
        CHECK(false);
        LG << "Exception in initialization.";  // TODO: ExceptionInInitializerError
        klass->SetStatus(Class::kStatusError);
        return false;
      }
      if (klass->GetStatus() == Class::kStatusInitializing) {
        continue;
      }
      DCHECK(klass->GetStatus() == Class::kStatusInitialized ||
             klass->GetStatus() == Class::kStatusError);
      if (klass->IsErroneous()) {
        // The caller wants an exception, but it was thrown in a
        // different thread.  Synthesize one here.
        LG << "<clinit> failed";  // TODO: throw UnsatisfiedLinkError
        return false;
      }
      return true;  // otherwise, initialized
    }

    // see if we failed previously
    if (klass->IsErroneous()) {
      // might be wise to unlock before throwing; depends on which class
      // it is that we have locked

      // TODO: throwEarlierClassFailure(klass);
      return false;
    }

    if (!ValidateSuperClassDescriptors(klass)) {
      klass->SetStatus(Class::kStatusError);
      return false;
    }

    DCHECK(klass->GetStatus() < Class::kStatusInitializing);

    klass->SetClinitThreadId(self->GetId());
    klass->SetStatus(Class::kStatusInitializing);
  }

  if (!InitializeSuperClass(klass)) {
    return false;
  }

  InitializeStaticFields(klass);

  Method* clinit = klass->FindDeclaredDirectMethod("<clinit>", "()V");
  if (clinit != NULL) {
    // JValue unused;
    // TODO: dvmCallMethod(self, method, NULL, &unused);
    // UNIMPLEMENTED(FATAL);
  }

  {
    ObjectLock lock(klass);

    if (self->IsExceptionPending()) {
      klass->SetStatus(Class::kStatusError);
    } else {
      klass->SetStatus(Class::kStatusInitialized);
    }
    lock.NotifyAll();
  }

  return true;
}

bool ClassLinker::ValidateSuperClassDescriptors(const Class* klass) {
  if (klass->IsInterface()) {
    return true;
  }
  // begin with the methods local to the superclass
  if (klass->HasSuperClass() &&
      klass->GetClassLoader() != klass->GetSuperClass()->GetClassLoader()) {
    const Class* super = klass->GetSuperClass();
    for (int i = super->NumVirtualMethods() - 1; i >= 0; --i) {
      const Method* method = super->GetVirtualMethod(i);
      if (method != super->GetVirtualMethod(i) &&
          !HasSameMethodDescriptorClasses(method, super, klass)) {
        LG << "Classes resolve differently in superclass";
        return false;
      }
    }
  }
  for (size_t i = 0; i < klass->GetIFTableCount(); ++i) {
    const InterfaceEntry* iftable = &klass->GetIFTable()[i];
    Class* interface = iftable->GetInterface();
    if (klass->GetClassLoader() != interface->GetClassLoader()) {
      for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
        uint32_t vtable_index = iftable->GetMethodIndexArray()[j];
        const Method* method = klass->GetVirtualMethod(vtable_index);
        if (!HasSameMethodDescriptorClasses(method, interface,
                                            method->GetClass())) {
          LG << "Classes resolve differently in interface";  // TODO: LinkageError
          return false;
        }
      }
    }
  }
  return true;
}

bool ClassLinker::HasSameMethodDescriptorClasses(const Method* method,
                                                 const Class* klass1,
                                                 const Class* klass2) {
  const DexFile& dex_file = FindDexFile(method->GetClass()->GetDexCache());
  const DexFile::ProtoId& proto_id = dex_file.GetProtoId(method->GetProtoIdx());
  DexFile::ParameterIterator *it;
  for (it = dex_file.GetParameterIterator(proto_id); it->HasNext(); it->Next()) {
    const char* descriptor = it->GetDescriptor();
    if (descriptor == NULL) {
      break;
    }
    if (descriptor[0] == 'L' || descriptor[0] == '[') {
      // Found a non-primitive type.
      if (!HasSameDescriptorClasses(descriptor, klass1, klass2)) {
        return false;
      }
    }
  }
  // Check the return type
  const char* descriptor = dex_file.GetReturnTypeDescriptor(proto_id);
  if (descriptor[0] == 'L' || descriptor[0] == '[') {
    if (HasSameDescriptorClasses(descriptor, klass1, klass2)) {
      return false;
    }
  }
  return true;
}

// Returns true if classes referenced by the descriptor are the
// same classes in klass1 as they are in klass2.
bool ClassLinker::HasSameDescriptorClasses(const char* descriptor,
                                           const Class* klass1,
                                           const Class* klass2) {
  CHECK(descriptor != NULL);
  CHECK(klass1 != NULL);
  CHECK(klass2 != NULL);
  Class* found1 = FindClass(descriptor, klass1->GetClassLoader());
  // TODO: found1 == NULL
  Class* found2 = FindClass(descriptor, klass2->GetClassLoader());
  // TODO: found2 == NULL
  // TODO: lookup found1 in initiating loader list
  if (found1 == NULL || found2 == NULL) {
    Thread::Current()->ClearException();
    if (found1 == found2) {
      return true;
    } else {
      return false;
    }
  }
  return true;
}

bool ClassLinker::InitializeSuperClass(Class* klass) {
  CHECK(klass != NULL);
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    Class* super_class = klass->GetSuperClass();
    if (super_class->GetStatus() != Class::kStatusInitialized) {
      CHECK(!super_class->IsInterface());
      klass->MonitorExit();
      bool super_initialized = InitializeClass(super_class);
      klass->MonitorEnter();
      // TODO: check for a pending exception
      if (!super_initialized) {
        klass->SetStatus(Class::kStatusError);
        klass->NotifyAll();
        return false;
      }
    }
  }
  return true;
}

bool ClassLinker::EnsureInitialized(Class* c) {
  CHECK(c != NULL);
  if (c->IsInitialized()) {
    return true;
  }

  c->MonitorExit();
  InitializeClass(c);
  c->MonitorEnter();
  return !Thread::Current()->IsExceptionPending();
}

StaticStorageBase* ClassLinker::InitializeStaticStorageFromCode(uint32_t type_idx,
                                                                const Method* referrer) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* klass = class_linker->ResolveType(type_idx, referrer);
  if (klass == NULL) {
    UNIMPLEMENTED(FATAL) << "throw exception due to unresolved class";
  }
  if (!class_linker->EnsureInitialized(klass)) {
    CHECK(Thread::Current()->IsExceptionPending());
    UNIMPLEMENTED(FATAL) << "throw exception due to class initializtion problem";
  }
  return klass;
}

void ClassLinker::InitializeStaticFields(Class* klass) {
  size_t num_static_fields = klass->NumStaticFields();
  if (num_static_fields == 0) {
    return;
  }
  DexCache* dex_cache = klass->GetDexCache();
  // TODO: this seems like the wrong check. do we really want !IsPrimitive && !IsArray?
  if (dex_cache == NULL) {
    return;
  }
  const std::string descriptor(klass->GetDescriptor()->ToModifiedUtf8());
  const DexFile& dex_file = FindDexFile(dex_cache);
  const DexFile::ClassDef* dex_class_def = dex_file.FindClassDef(descriptor);
  CHECK(dex_class_def != NULL);
  const byte* addr = dex_file.GetEncodedArray(*dex_class_def);
  if (addr == NULL) {
    // All this class' static fields have default values.
    return;
  }
  size_t array_size = DecodeUnsignedLeb128(&addr);
  for (size_t i = 0; i < array_size; ++i) {
    Field* field = klass->GetStaticField(i);
    JValue value;
    DexFile::ValueType type = dex_file.ReadEncodedValue(&addr, &value);
    switch (type) {
      case DexFile::kByte:
        field->SetByte(NULL, value.b);
        break;
      case DexFile::kShort:
        field->SetShort(NULL, value.s);
        break;
      case DexFile::kChar:
        field->SetChar(NULL, value.c);
        break;
      case DexFile::kInt:
        field->SetInt(NULL, value.i);
        break;
      case DexFile::kLong:
        field->SetLong(NULL, value.j);
        break;
      case DexFile::kFloat:
        field->SetFloat(NULL, value.f);
        break;
      case DexFile::kDouble:
        field->SetDouble(NULL, value.d);
        break;
      case DexFile::kString: {
        uint32_t string_idx = value.i;
        const String* resolved = ResolveString(dex_file, string_idx, klass->GetDexCache());
        field->SetObject(NULL, resolved);
        break;
      }
      case DexFile::kBoolean:
        field->SetBoolean(NULL, value.z);
        break;
      case DexFile::kNull:
        field->SetObject(NULL, value.l);
        break;
      default:
        LOG(FATAL) << "Unknown type " << static_cast<int>(type);
    }
  }
}

bool ClassLinker::LinkClass(Class* klass) {
  CHECK_EQ(Class::kStatusLoaded, klass->GetStatus());
  if (!LinkSuperClass(klass)) {
    return false;
  }
  if (!LinkMethods(klass)) {
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

bool ClassLinker::LoadSuperAndInterfaces(Class* klass, const DexFile& dex_file) {
  CHECK_EQ(Class::kStatusIdx, klass->GetStatus());
  if (klass->GetSuperClassTypeIdx() != DexFile::kDexNoIndex) {
    Class* super_class = ResolveType(dex_file, klass->GetSuperClassTypeIdx(), klass);
    if (super_class == NULL) {
      LG << "Failed to resolve superclass";
      return false;
    }
    klass->SetSuperClass(super_class);
  }
  for (size_t i = 0; i < klass->NumInterfaces(); ++i) {
    uint32_t idx = klass->GetInterfacesTypeIdx()->Get(i);
    Class *interface = ResolveType(dex_file, idx, klass);
    klass->SetInterface(i, interface);
    if (interface == NULL) {
      LG << "Failed to resolve interface";
      return false;
    }
    // Verify
    if (!klass->CanAccess(interface)) {
      LG << "Inaccessible interface";
      return false;
    }
  }
  // Mark the class as loaded.
  klass->SetStatus(Class::kStatusLoaded);
  return true;
}

bool ClassLinker::LinkSuperClass(Class* klass) {
  CHECK(!klass->IsPrimitive());
  Class* super = klass->GetSuperClass();
  if (klass->GetDescriptor()->Equals("Ljava/lang/Object;")) {
    if (super != NULL) {
      LG << "Superclass must not be defined";  // TODO: ClassFormatError
      return false;
    }
    // TODO: clear finalize attribute
    return true;
  }
  if (super == NULL) {
    LG << "No superclass defined";  // TODO: LinkageError
    return false;
  }
  // Verify
  if (super->IsFinal()) {
    LG << "Superclass " << super->GetDescriptor()->ToModifiedUtf8() << " is declared final";  // TODO: IncompatibleClassChangeError
    return false;
  }
  if (super->IsInterface()) {
    LG << "Superclass " << super->GetDescriptor()->ToModifiedUtf8() << " is an interface";  // TODO: IncompatibleClassChangeError
    return false;
  }
  if (!klass->CanAccess(super)) {
    LG << "Superclass " << super->GetDescriptor()->ToModifiedUtf8() << " is  inaccessible";  // TODO: IllegalAccessError
    return false;
  }
#ifndef NDEBUG
  // Ensure super classes are fully resolved prior to resolving fields..
  while (super != NULL) {
    CHECK(super->IsLinked());
    super = super->GetSuperClass();
  }
#endif
  return true;
}

// Populate the class vtable and itable. Compute return type indices.
bool ClassLinker::LinkMethods(Class* klass) {
  if (klass->IsInterface()) {
    // No vtable.
    size_t count = klass->NumVirtualMethods();
    if (!IsUint(16, count)) {
      LG << "Too many methods on interface";  // TODO: VirtualMachineError
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      klass->GetVirtualMethodDuringLinking(i)->SetMethodIndex(i);
    }
  } else {
    // Link virtual method tables
    LinkVirtualMethods(klass);

    // Link interface method tables
    LinkInterfaceMethods(klass);

    // Insert stubs.
    LinkAbstractMethods(klass);
  }
  return true;
}

bool ClassLinker::LinkVirtualMethods(Class* klass) {
  if (klass->HasSuperClass()) {
    uint32_t max_count = klass->NumVirtualMethods() + klass->GetSuperClass()->GetVTable()->GetLength();
    size_t actual_count = klass->GetSuperClass()->GetVTable()->GetLength();
    CHECK_LE(actual_count, max_count);
    // TODO: do not assign to the vtable field until it is fully constructed.
    ObjectArray<Method>* vtable = klass->GetSuperClass()->GetVTable()->CopyOf(max_count);
    // See if any of our virtual methods override the superclass.
    for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
      Method* local_method = klass->GetVirtualMethodDuringLinking(i);
      size_t j = 0;
      for (; j < actual_count; ++j) {
        Method* super_method = vtable->Get(j);
        if (local_method->HasSameNameAndDescriptor(super_method)) {
          // Verify
          if (super_method->IsFinal()) {
            LG << "Method overrides final method";  // TODO: VirtualMachineError
            return false;
          }
          vtable->Set(j, local_method);
          local_method->SetMethodIndex(j);
          break;
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
      LG << "Too many methods defined on class";  // TODO: VirtualMachineError
      return false;
    }
    // Shrink vtable if possible
    CHECK_LE(actual_count, max_count);
    if (actual_count < max_count) {
      vtable = vtable->CopyOf(actual_count);
    }
    klass->SetVTable(vtable);
  } else {
    CHECK(klass->GetDescriptor()->Equals("Ljava/lang/Object;"));
    uint32_t num_virtual_methods = klass->NumVirtualMethods();
    if (!IsUint(16, num_virtual_methods)) {
      LG << "Too many methods";  // TODO: VirtualMachineError
      return false;
    }
    ObjectArray<Method>* vtable = AllocObjectArray<Method>(num_virtual_methods);
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      Method* virtual_method = klass->GetVirtualMethodDuringLinking(i);
      vtable->Set(i, virtual_method);
      virtual_method->SetMethodIndex(i & 0xFFFF);
    }
    klass->SetVTable(vtable);
  }
  return true;
}

bool ClassLinker::LinkInterfaceMethods(Class* klass) {
  int pool_offset = 0;
  int pool_size = 0;
  int miranda_count = 0;
  int miranda_alloc = 0;
  size_t super_ifcount;
  if (klass->HasSuperClass()) {
    super_ifcount = klass->GetSuperClass()->GetIFTableCount();
  } else {
    super_ifcount = 0;
  }
  size_t ifcount = super_ifcount;
  ifcount += klass->NumInterfaces();
  for (size_t i = 0; i < klass->NumInterfaces(); i++) {
    ifcount += klass->GetInterface(i)->GetIFTableCount();
  }
  if (ifcount == 0) {
    // TODO: enable these asserts with klass status validation
    // DCHECK(klass->GetIFTableCount() == 0);
    // DCHECK(klass->GetIFTable() == NULL);
    return true;
  }
  InterfaceEntry* iftable = new InterfaceEntry[ifcount];
  memset(iftable, 0x00, sizeof(InterfaceEntry) * ifcount);
  if (super_ifcount != 0) {
    memcpy(iftable, klass->GetSuperClass()->GetIFTable(),
           sizeof(InterfaceEntry) * super_ifcount);
  }
  // Flatten the interface inheritance hierarchy.
  size_t idx = super_ifcount;
  for (size_t i = 0; i < klass->NumInterfaces(); i++) {
    Class* interf = klass->GetInterface(i);
    DCHECK(interf != NULL);
    if (!interf->IsInterface()) {
      LG << "Class implements non-interface class";  // TODO: IncompatibleClassChangeError
      return false;
    }
    iftable[idx++].SetInterface(interf);
    for (size_t j = 0; j < interf->GetIFTableCount(); j++) {
      iftable[idx++].SetInterface(interf->GetIFTable()[j].GetInterface());
    }
  }
  klass->SetIFTable(iftable);
  CHECK_EQ(idx, ifcount);
  klass->SetIFTableCount(ifcount);
  if (klass->IsInterface() || super_ifcount == ifcount) {
    return true;
  }
  for (size_t i = super_ifcount; i < ifcount; i++) {
    pool_size += iftable[i].GetInterface()->NumVirtualMethods();
  }
  if (pool_size == 0) {
    return true;
  }
  klass->SetIfviPoolCount(pool_size);
  uint32_t* ifvi_pool = new uint32_t[pool_size];
  klass->SetIfviPool(ifvi_pool);
  std::vector<Method*> miranda_list;
  for (size_t i = super_ifcount; i < ifcount; ++i) {
    iftable[i].SetMethodIndexArray(ifvi_pool + pool_offset);
    Class* interface = iftable[i].GetInterface();
    pool_offset += interface->NumVirtualMethods();    // end here
    ObjectArray<Method>* vtable = klass->GetVTableDuringLinking();
    for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
      Method* interface_method = interface->GetVirtualMethod(j);
      int k;  // must be signed
      for (k = vtable->GetLength() - 1; k >= 0; --k) {
        Method* vtable_method = vtable->Get(k);
        if (interface_method->HasSameNameAndDescriptor(vtable_method)) {
          if (!vtable_method->IsPublic()) {
            LG << "Implementation not public";
            return false;
          }
          iftable[i].GetMethodIndexArray()[j] = k;
          break;
        }
      }
      if (k < 0) {
        if (miranda_count == miranda_alloc) {
          miranda_alloc += 8;
          if (miranda_list.empty()) {
            miranda_list.resize(miranda_alloc);
          } else {
            miranda_list.resize(miranda_alloc);
          }
        }
        int mir;
        for (mir = 0; mir < miranda_count; mir++) {
          Method* miranda_method = miranda_list[mir];
          if (miranda_method->HasSameNameAndDescriptor(interface_method)) {
            break;
          }
        }
        // point the interface table at a phantom slot index
        iftable[i].GetMethodIndexArray()[j] =
            vtable->GetLength() + mir;
        if (mir == miranda_count) {
          miranda_list[miranda_count++] = interface_method;
        }
      }
    }
  }
  if (miranda_count != 0) {
    int old_method_count = klass->NumVirtualMethods();
    int new_method_count = old_method_count + miranda_count;
    klass->SetVirtualMethods(
        klass->GetVirtualMethods()->CopyOf(new_method_count));

    ObjectArray<Method>* vtable = klass->GetVTableDuringLinking();
    CHECK(vtable != NULL);
    int old_vtable_count = vtable->GetLength();
    int new_vtable_count = old_vtable_count + miranda_count;
    vtable = vtable->CopyOf(new_vtable_count);
    for (int i = 0; i < miranda_count; i++) {
      Method* meth = AllocMethod();
      // TODO: this shouldn't be a memcpy
      memcpy(meth, miranda_list[i], sizeof(Method));
      meth->SetDeclaringClass(klass);
      meth->SetAccessFlags(meth->GetAccessFlags() | kAccMiranda);
      meth->SetMethodIndex(0xFFFF & (old_vtable_count + i));
      klass->SetVirtualMethod(old_method_count + i, meth);
      vtable->Set(old_vtable_count + i, meth);
    }
    // TODO: do not assign to the vtable field until it is fully constructed.
    klass->SetVTable(vtable);
  }
  return true;
}

void ClassLinker::LinkAbstractMethods(Class* klass) {
  for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
    Method* method = klass->GetVirtualMethodDuringLinking(i);
    if (method->IsAbstract()) {
      LG << "AbstractMethodError";
      // TODO: throw AbstractMethodError
    }
  }
}

bool ClassLinker::LinkInstanceFields(Class* klass) {
  CHECK(klass != NULL);
  return LinkFields(klass, true);
}

bool ClassLinker::LinkStaticFields(Class* klass) {
  CHECK(klass != NULL);
  size_t allocated_class_size = klass->GetClassSize();
  bool success = LinkFields(klass, false);
  CHECK_EQ(allocated_class_size, klass->GetClassSize());
  return success;
}

bool ClassLinker::LinkFields(Class *klass, bool instance) {
  size_t num_fields =
      instance ? klass->NumInstanceFields() : klass->NumStaticFields();

  ObjectArray<Field>* fields =
      instance ? klass->GetIFields() : klass->GetSFields();
  // Fields updated at end of LinkFields
  size_t num_reference_fields;
  size_t size;

  // Initialize size and field_offset
  MemberOffset field_offset = Class::FieldsOffset();
  if (instance) {
    Class* super_class = klass->GetSuperClass();
    if (super_class != NULL) {
      CHECK(super_class->IsLinked());
      field_offset = MemberOffset(super_class->GetObjectSize());
      if (field_offset.Uint32Value() == 0u) {
        field_offset = OFFSET_OF_OBJECT_MEMBER(DataObject, fields_);
      }
    } else {
      field_offset = OFFSET_OF_OBJECT_MEMBER(DataObject, fields_);
    }
    size = field_offset.Uint32Value();
  } else {
    size = klass->GetClassSize();
  }
  DCHECK_LE(CLASS_SMALLEST_OFFSET, size);

  CHECK((num_fields == 0) == (fields == NULL));

  // Move references to the front.
  size_t i = 0;
  num_reference_fields = 0;
  for (; i < num_fields; i++) {
    Field* field = fields->Get(i);
    const Class* field_type = field->GetTypeDuringLinking();
    // if a field's type at this point is NULL it isn't primitive
    if (field_type != NULL && field_type->IsPrimitive()) {
      for (size_t j = num_fields - 1; j > i; j--) {
        Field* ref_field = fields->Get(j);
        const Class* ref_field_type = ref_field->GetTypeDuringLinking();
        if (ref_field_type == NULL || !ref_field_type->IsPrimitive()) {
          fields->Set(i, ref_field);
          fields->Set(j, field);
          field = ref_field;
          field_type = ref_field_type;
          num_reference_fields++;
          break;
        }
      }
    } else {
      num_reference_fields++;
    }
    if (field_type != NULL && field_type->IsPrimitive()) {
      break;
    }
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() + sizeof(uint32_t));
  }

  // Now we want to pack all of the double-wide fields together.  If
  // we're not aligned, though, we want to shuffle one 32-bit field
  // into place.  If we can't find one, we'll have to pad it.
  if (i != num_fields && !IsAligned(field_offset.Uint32Value(), 8)) {
    Field* field = fields->Get(i);
    const Class* c = field->GetTypeDuringLinking();
    CHECK(c != NULL);  // should only be working on primitive types
    if (!c->IsPrimitiveLong() && !c->IsPrimitiveDouble()) {
      // The field that comes next is 32-bit, so just advance past it.
      DCHECK(c->IsPrimitive());
      field->SetOffset(field_offset);
      field_offset = MemberOffset(field_offset.Uint32Value() +
                                  sizeof(uint32_t));
      i++;
    } else {
      // Next field is 64-bit, so search for a 32-bit field we can
      // swap into it.
      bool found = false;
      for (size_t j = num_fields - 1; j > i; j--) {
        Field* single_field = fields->Get(j);
        const Class* rc = single_field->GetTypeDuringLinking();
        CHECK(rc != NULL);  // should only be working on primitive types
        if (!rc->IsPrimitiveLong() && !rc->IsPrimitiveDouble()) {
          fields->Set(i, single_field);
          fields->Set(j, field);
          field = single_field;
          field->SetOffset(field_offset);
          field_offset = MemberOffset(field_offset.Uint32Value() +
                                      sizeof(uint32_t));
          found = true;
          i++;
          break;
        }
      }
      if (!found) {
        field_offset = MemberOffset(field_offset.Uint32Value() +
                                    sizeof(uint32_t));
      }
    }
  }

  // Alignment is good, shuffle any double-wide fields forward, and
  // finish assigning field offsets to all fields.
  DCHECK(i == num_fields || IsAligned(field_offset.Uint32Value(), 4));
  for ( ; i < num_fields; i++) {
    Field* field = fields->Get(i);
    const Class* c = field->GetTypeDuringLinking();
    CHECK(c != NULL);  // should only be working on primitive types
    if (!c->IsPrimitiveDouble() && !c->IsPrimitiveLong()) {
      for (size_t j = num_fields - 1; j > i; j--) {
        Field* double_field = fields->Get(j);
        const Class* rc = double_field->GetTypeDuringLinking();
        CHECK(rc != NULL);  // should only be working on primitive types
        if (rc->IsPrimitiveDouble() || rc->IsPrimitiveLong()) {
          fields->Set(i, double_field);
          fields->Set(j, field);
          field = double_field;
          c = rc;
          break;
        }
      }
    } else {
      // This is a double-wide field, leave it be.
    }

    field->SetOffset(field_offset);
    if (c->IsPrimitiveLong() || c->IsPrimitiveDouble()) {
      field_offset = MemberOffset(field_offset.Uint32Value() +
                                  sizeof(uint64_t));
    } else {
      field_offset = MemberOffset(field_offset.Uint32Value() +
                                  sizeof(uint32_t));
    }
  }

#ifndef NDEBUG
  // Make sure that all reference fields appear before
  // non-reference fields, and all double-wide fields are aligned.
  bool seen_non_ref = false;
  for (i = 0; i < num_fields; i++) {
    Field* field = fields->Get(i);
    const Class* c = field->GetTypeDuringLinking();
    if (c != NULL && c->IsPrimitive()) {
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
  DCHECK_LE(CLASS_SMALLEST_OFFSET, size);
  // Update klass
  if(instance) {
    klass->SetNumReferenceInstanceFields(num_reference_fields);
    if(!klass->IsVariableSize()) {
      klass->SetObjectSize(size);
    }
  } else {
    klass->SetNumReferenceStaticFields(num_reference_fields);
    klass->SetClassSize(size);
  }
  return true;
}

//  Set the bitmap of reference offsets, refOffsets, from the ifields
//  list.
void ClassLinker::CreateReferenceInstanceOffsets(Class* klass) {
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
  CreateReferenceOffsets(klass, true, reference_offsets);
}

void ClassLinker::CreateReferenceStaticOffsets(Class* klass) {
  CreateReferenceOffsets(klass, false, 0);
}

void ClassLinker::CreateReferenceOffsets(Class* klass, bool instance,
                                         uint32_t reference_offsets) {
  size_t num_reference_fields =
      instance ? klass->NumReferenceInstanceFieldsDuringLinking()
               : klass->NumReferenceStaticFieldsDuringLinking();
  const ObjectArray<Field>* fields =
      instance ? klass->GetIFields() : klass->GetSFields();
  // All of the fields that contain object references are guaranteed
  // to be at the beginning of the fields list.
  for (size_t i = 0; i < num_reference_fields; ++i) {
    // Note that byte_offset is the offset from the beginning of
    // object, not the offset into instance data
    const Field* field = fields->Get(i);
    MemberOffset byte_offset = field->GetOffsetDuringLinking();
    CHECK_GE(byte_offset.Uint32Value(), CLASS_SMALLEST_OFFSET);
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
  if (instance) {
    klass->SetReferenceInstanceOffsets(reference_offsets);
  } else {
    klass->SetReferenceStaticOffsets(reference_offsets);
  }
}

String* ClassLinker::ResolveString(const DexFile& dex_file,
    uint32_t string_idx, DexCache* dex_cache) {
  String* resolved = dex_cache->GetResolvedString(string_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::StringId& string_id = dex_file.GetStringId(string_idx);
  int32_t utf16_length = dex_file.GetStringLength(string_id);
  const char* utf8_data = dex_file.GetStringData(string_id);
  // TODO: remote the const_cast below
  String* string = const_cast<String*>(intern_table_->InternStrong(utf16_length, utf8_data));
  dex_cache->SetResolvedString(string_idx, string);
  return string;
}

Class* ClassLinker::ResolveType(const DexFile& dex_file,
                                uint32_t type_idx,
                                DexCache* dex_cache,
                                const ClassLoader* class_loader) {
  Class* resolved = dex_cache->GetResolvedType(type_idx);
  if (resolved == NULL) {
    const char* descriptor = dex_file.dexStringByTypeIdx(type_idx);
    if (descriptor[1] == '\0') {
      // only the descriptors of primitive types should be 1 character long
      resolved = FindPrimitiveClass(descriptor[0]);
    } else {
      resolved = FindClass(descriptor, class_loader);
    }
    if (resolved != NULL) {
      Class* check = resolved->IsArrayClass() ? resolved->GetComponentType() : resolved;
      if (dex_cache != check->GetDexCache()) {
        if (check->GetClassLoader() != NULL) {
          LG << "Class resolved by unexpected DEX";  // TODO: IllegalAccessError
          resolved = NULL;
        }
      }
    }
    if (resolved != NULL) {
      dex_cache->SetResolvedType(type_idx, resolved);
    } else {
      DCHECK(Thread::Current()->IsExceptionPending());
    }
  }
  return resolved;
}

Method* ClassLinker::ResolveMethod(const DexFile& dex_file,
                                   uint32_t method_idx,
                                   DexCache* dex_cache,
                                   const ClassLoader* class_loader,
                                   bool is_direct) {
  Method* resolved = dex_cache->GetResolvedMethod(method_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  Class* klass = ResolveType(dex_file, method_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    return NULL;
  }

  const char* name = dex_file.dexStringById(method_id.name_idx_);
  std::string signature(dex_file.CreateMethodDescriptor(method_id.proto_idx_, NULL));
  if (is_direct) {
    resolved = klass->FindDirectMethod(name, signature);
  } else {
    resolved = klass->FindVirtualMethod(name, signature);
  }
  if (resolved != NULL) {
    dex_cache->SetResolvedMethod(method_idx, resolved);
  } else {
    // DCHECK(Thread::Current()->IsExceptionPending());
  }
  return resolved;
}

Field* ClassLinker::ResolveField(const DexFile& dex_file,
                                 uint32_t field_idx,
                                 DexCache* dex_cache,
                                 const ClassLoader* class_loader,
                                 bool is_static) {
  Field* resolved = dex_cache->GetResolvedField(field_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  Class* klass = ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    return NULL;
  }

  const char* name = dex_file.dexStringById(field_id.name_idx_);
  Class* field_type = ResolveType(dex_file, field_id.type_idx_, dex_cache, class_loader);
  // TODO: LinkageError?
  CHECK(field_type != NULL);
  if (is_static) {
    resolved = klass->FindStaticField(name, field_type);
  } else {
    resolved = klass->FindInstanceField(name, field_type);
  }
  if (resolved != NULL) {
    dex_cache->SetResolvedfield(field_idx, resolved);
  } else {
    // TODO: DCHECK(Thread::Current()->IsExceptionPending());
  }
  return resolved;
}

size_t ClassLinker::NumLoadedClasses() const {
  MutexLock mu(classes_lock_);
  return classes_.size();
}

}  // namespace art
