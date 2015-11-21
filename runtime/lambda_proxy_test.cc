/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <jni.h>
#include <vector>

#include "art_field-inl.h"
#include "class_linker-inl.h"
#include "compiler_callbacks.h"
#include "common_compiler_test.h"
#include "mirror/field-inl.h"
#include "mirror/lambda_proxy.h"
#include "mirror/method.h"
#include "scoped_thread_state_change.h"

namespace art {

// The enclosing class of all the interfaces used by this test.
// -- Defined as a macro to allow for string concatenation.
#define TEST_INTERFACE_ENCLOSING_CLASS_NAME "LambdaInterfaces"
// Generate out "LLambdaInterfaces$<<iface>>;" , replacing <<iface>> with the interface name.
#define MAKE_TEST_INTERFACE_NAME(iface) ("L" TEST_INTERFACE_ENCLOSING_CLASS_NAME "$" iface ";")

#define ASSERT_NOT_NULL(x) ASSERT_TRUE((x) != nullptr)
#define ASSERT_NULL(x) ASSERT_TRUE((x) == nullptr)
#define EXPECT_NULL(x) EXPECT_TRUE((x) == nullptr)

class LambdaProxyTest  // : public CommonCompilerTest {
    : public CommonRuntimeTest {
 public:
  // Generate a lambda proxy class with the given name and interfaces. This is a simplification from what
  // libcore does to fit to our test needs. We do not check for duplicated interfaces or methods and
  // we do not declare exceptions.
  mirror::Class* GenerateProxyClass(ScopedObjectAccess& soa,
                                    jobject jclass_loader,
                                    const char* class_name,
                                    const std::vector<mirror::Class*>& interfaces)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    CHECK(class_name != nullptr);
    CHECK(jclass_loader != nullptr);

    mirror::Class* java_lang_object =
        class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;");
    CHECK(java_lang_object != nullptr);

    jclass java_lang_class = soa.AddLocalReference<jclass>(mirror::Class::GetJavaLangClass());

    // Builds the interfaces array.
    jobjectArray proxy_class_interfaces = soa.Env()->NewObjectArray(interfaces.size(),
                                                                    java_lang_class,
                                                                    nullptr);  // No initial element.
    soa.Self()->AssertNoPendingException();
    for (size_t i = 0; i < interfaces.size(); ++i) {
      soa.Env()->SetObjectArrayElement(proxy_class_interfaces,
                                       i,
                                       soa.AddLocalReference<jclass>(interfaces[i]));
    }

    // Builds the method array.
    jsize methods_count = 3;  // Object.equals, Object.hashCode and Object.toString.
    for (mirror::Class* interface : interfaces) {
      methods_count += interface->NumVirtualMethods();
    }
    jobjectArray proxy_class_methods =
        soa.Env()->NewObjectArray(methods_count,
                                  soa.AddLocalReference<jclass>(mirror::Method::StaticClass()),
                                  nullptr);  // No initial element.
    soa.Self()->AssertNoPendingException();

    jsize array_index = 0;

    //
    // Fill the method array with the Object and all the interface's virtual methods.
    //

    // Add a method to 'proxy_class_methods'
    auto add_method_to_array = [&](ArtMethod* method) SHARED_REQUIRES(Locks::mutator_lock_) {
      CHECK(method != nullptr);
      soa.Env()->SetObjectArrayElement(proxy_class_methods,
                                       array_index++,
                                       soa.AddLocalReference<jobject>(
                                           mirror::Method::CreateFromArtMethod(soa.Self(),
                                                                               method))
                                      );  // NOLINT: [whitespace/parens] [2]

      LOG(DEBUG) << "Add " << PrettyMethod(method) << " to list of methods to generate proxy";
    };
    // Add a method to 'proxy_class_methods' by looking it up from java.lang.Object
    auto add_method_to_array_by_lookup = [&](const char* name, const char* method_descriptor)
        SHARED_REQUIRES(Locks::mutator_lock_) {
      ArtMethod* method = java_lang_object->FindDeclaredVirtualMethod(name,
                                                                      method_descriptor,
                                                                      sizeof(void*));
      add_method_to_array(method);
    };

    // Add all methods from Object.
    add_method_to_array_by_lookup("equals",   "(Ljava/lang/Object;)Z");
    add_method_to_array_by_lookup("hashCode", "()I");
    add_method_to_array_by_lookup("toString", "()Ljava/lang/String;");

    // Now adds all interfaces virtual methods.
    for (mirror::Class* interface : interfaces) {
      mirror::Class* next_class = interface;
      do {
        for (ArtMethod& method : next_class->GetVirtualMethods(sizeof(void*))) {
          add_method_to_array(&method);
        }
        next_class = next_class->GetSuperClass();
      } while (!next_class->IsObjectClass());
      // Skip adding any methods from "Object".
    }
    CHECK_EQ(array_index, methods_count);

    // Builds an empty exception array.
    jobjectArray proxy_class_throws = soa.Env()->NewObjectArray(0 /* length */,
                                                                java_lang_class,
                                                                nullptr /* initial element*/);
    soa.Self()->AssertNoPendingException();

    bool already_exists;
    mirror::Class* proxy_class =
        class_linker_->CreateLambdaProxyClass(soa,
                                              soa.Env()->NewStringUTF(class_name),
                                              proxy_class_interfaces,
                                              jclass_loader,
                                              proxy_class_methods,
                                              proxy_class_throws,
                                              /*out*/&already_exists);

    CHECK(!already_exists);

    soa.Self()->AssertNoPendingException();
    return proxy_class;
  }

  LambdaProxyTest() {
  }

  virtual void SetUp() {
    CommonRuntimeTest::SetUp();
  }

  virtual void SetUpRuntimeOptions(RuntimeOptions* options ATTRIBUTE_UNUSED) {
    // Do not have any compiler options because we don't want to run as an AOT
    // (In particular the lambda proxy class generation isn't currently supported for AOT).
    this->callbacks_.reset();
  }

  template <typename THandleScope>
  Handle<mirror::Class> GenerateProxyClass(THandleScope& hs,
                                           const char* name,
                                           const std::vector<mirror::Class*>& interfaces)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    return hs.NewHandle(GenerateProxyClass(*soa_, jclass_loader_, name, interfaces));
  }

 protected:
  ScopedObjectAccess* soa_ = nullptr;
  jobject jclass_loader_ = nullptr;
};

// Creates a lambda proxy class and check ClassHelper works correctly.
TEST_F(LambdaProxyTest, ProxyClassHelper) {
  // gLogVerbosity.class_linker = true;  // Uncomment to enable class linker logging.

  ASSERT_NOT_NULL(Thread::Current());

  ScopedObjectAccess soa(Thread::Current());
  soa_ = &soa;

  // Must happen after CommonRuntimeTest finishes constructing the runtime.
  jclass_loader_ = LoadDex(TEST_INTERFACE_ENCLOSING_CLASS_NAME);
  jobject jclass_loader = jclass_loader_;

  StackHandleScope<4> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));

  Handle<mirror::Class> J(hs.NewHandle(
      class_linker_->FindClass(soa.Self(), MAKE_TEST_INTERFACE_NAME("J"), class_loader)));
  ASSERT_TRUE(J.Get() != nullptr);

  std::vector<mirror::Class*> interfaces;
  interfaces.push_back(J.Get());
  Handle<mirror::Class> proxy_class(hs.NewHandle(
      GenerateProxyClass(soa, jclass_loader, "$Proxy1234", interfaces)));
  interfaces.clear();  // Don't least possibly stale objects in the array as good practice.
  ASSERT_TRUE(proxy_class.Get() != nullptr);
  ASSERT_TRUE(proxy_class->IsLambdaProxyClass());
  ASSERT_TRUE(proxy_class->IsInitialized());

  EXPECT_EQ(1U, proxy_class->NumDirectInterfaces());  // LambdaInterfaces$J.
  EXPECT_EQ(J.Get(), mirror::Class::GetDirectInterface(soa.Self(), proxy_class, 0));
  std::string temp;
  const char* proxy_class_descriptor = proxy_class->GetDescriptor(&temp);
  EXPECT_STREQ("L$Proxy1234;", proxy_class_descriptor);
  EXPECT_EQ(nullptr, proxy_class->GetSourceFile());

  // Make sure all the virtual methods are marked as a proxy
  for (ArtMethod& method : proxy_class->GetVirtualMethods(sizeof(void*))) {
    SCOPED_TRACE(PrettyMethod(&method, /* with_signature */true));
    EXPECT_TRUE(method.IsProxyMethod());
    EXPECT_TRUE(method.IsLambdaProxyMethod());
    EXPECT_FALSE(method.IsReflectProxyMethod());
  }
}

// Creates a proxy class and check FieldHelper works correctly.
TEST_F(LambdaProxyTest, ProxyFieldHelper) {
  // gLogVerbosity.class_linker = true;  // Uncomment to enable class linker logging.

  ASSERT_NOT_NULL(Thread::Current());

  ScopedObjectAccess soa(Thread::Current());
  soa_ = &soa;

  // Must happen after CommonRuntimeTest finishes constructing the runtime.
  jclass_loader_ = LoadDex(TEST_INTERFACE_ENCLOSING_CLASS_NAME);
  jobject jclass_loader = jclass_loader_;

  StackHandleScope<9> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));

  Handle<mirror::Class> I(hs.NewHandle(
      class_linker_->FindClass(soa.Self(), MAKE_TEST_INTERFACE_NAME("I"), class_loader)));
  ASSERT_NOT_NULL(I.Get());

  // Create the lambda proxy which implements interfaces "I".
  Handle<mirror::Class> proxy_class = GenerateProxyClass(hs,
                                                         "$Proxy1234",
                                                         { I.Get() });  // Interfaces.

  ASSERT_NOT_NULL(proxy_class.Get());
  EXPECT_TRUE(proxy_class->IsLambdaProxyClass());
  EXPECT_TRUE(proxy_class->IsInitialized());
  EXPECT_NULL(proxy_class->GetIFieldsPtr());

  LengthPrefixedArray<ArtField>* static_fields = proxy_class->GetSFieldsPtr();
  ASSERT_NOT_NULL(static_fields);

  // Must have "throws" and "interfaces" static fields.
  ASSERT_EQ(+mirror::LambdaProxy::kStaticFieldCount, proxy_class->NumStaticFields());

  static constexpr const char* kInterfacesClassName = "[Ljava/lang/Class;";
  static constexpr const char* kThrowsClassName     = "[[Ljava/lang/Class;";

  // Class for "interfaces" field.
  Handle<mirror::Class> interfaces_field_class =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), kInterfacesClassName));
  ASSERT_NOT_NULL(interfaces_field_class.Get());

  // Class for "throws" field.
  Handle<mirror::Class> throws_field_class =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), kThrowsClassName));
  ASSERT_NOT_NULL(throws_field_class.Get());

  // Helper to test the static fields for correctness.
  auto test_static_field = [&](size_t index,
                               const char* field_name,
                               Handle<mirror::Class>& handle_class,
                               const char* class_name)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtField* field = &static_fields->At(index);
    EXPECT_STREQ(field_name, field->GetName());
    EXPECT_STREQ(class_name, field->GetTypeDescriptor());
    EXPECT_EQ(handle_class.Get(), field->GetType</*kResolve*/true>())
        << "Expected: " << PrettyClass(interfaces_field_class.Get()) << ", "
        << "Actual: " << PrettyClass(field->GetType</*kResolve*/true>()) << ", "
        << "field_name: " << field_name;
    std::string temp;
    EXPECT_STREQ("L$Proxy1234;", field->GetDeclaringClass()->GetDescriptor(&temp));
    EXPECT_FALSE(field->IsPrimitiveType());
  };

  // Test "Class[] interfaces" field.
  test_static_field(mirror::LambdaProxy::kStaticFieldIndexInterfaces,
                    "interfaces",
                    interfaces_field_class,
                    kInterfacesClassName);

  // Test "Class[][] throws" field.
  test_static_field(mirror::LambdaProxy::kStaticFieldIndexThrows,
                    "throws",
                    throws_field_class,
                    kThrowsClassName);
}

// Creates two proxy classes and check the art/mirror fields of their static fields.
TEST_F(LambdaProxyTest, CheckArtMirrorFieldsOfProxyStaticFields) {
  // gLogVerbosity.class_linker = true;  // Uncomment to enable class linker logging.

  ASSERT_NOT_NULL(Thread::Current());

  ScopedObjectAccess soa(Thread::Current());
  soa_ = &soa;

  // Must happen after CommonRuntimeTest finishes constructing the runtime.
  jclass_loader_ = LoadDex(TEST_INTERFACE_ENCLOSING_CLASS_NAME);
  jobject jclass_loader = jclass_loader_;

  StackHandleScope<8> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));

  Handle<mirror::Class> proxyClass0;
  Handle<mirror::Class> proxyClass1;
  {
    Handle<mirror::Class> L(hs.NewHandle(
        class_linker_->FindClass(soa.Self(), MAKE_TEST_INTERFACE_NAME("L"), class_loader)));
    ASSERT_TRUE(L.Get() != nullptr);

    std::vector<mirror::Class*> interfaces = { L.Get() };
    proxyClass0 = hs.NewHandle(GenerateProxyClass(soa, jclass_loader, "$Proxy0", interfaces));
    proxyClass1 = hs.NewHandle(GenerateProxyClass(soa, jclass_loader, "$Proxy1", interfaces));
  }

  ASSERT_TRUE(proxyClass0.Get() != nullptr);
  ASSERT_TRUE(proxyClass0->IsLambdaProxyClass());
  ASSERT_TRUE(proxyClass0->IsInitialized());
  ASSERT_TRUE(proxyClass1.Get() != nullptr);
  ASSERT_TRUE(proxyClass1->IsLambdaProxyClass());
  ASSERT_TRUE(proxyClass1->IsInitialized());

  LengthPrefixedArray<ArtField>* static_fields0 = proxyClass0->GetSFieldsPtr();
  ASSERT_TRUE(static_fields0 != nullptr);
  ASSERT_EQ(2u, static_fields0->size());
  LengthPrefixedArray<ArtField>* static_fields1 = proxyClass1->GetSFieldsPtr();
  ASSERT_TRUE(static_fields1 != nullptr);
  ASSERT_EQ(2u, static_fields1->size());

  EXPECT_EQ(static_fields0->At(0).GetDeclaringClass(), proxyClass0.Get());
  EXPECT_EQ(static_fields0->At(1).GetDeclaringClass(), proxyClass0.Get());
  EXPECT_EQ(static_fields1->At(0).GetDeclaringClass(), proxyClass1.Get());
  EXPECT_EQ(static_fields1->At(1).GetDeclaringClass(), proxyClass1.Get());

  Handle<mirror::Field> field00 =
      hs.NewHandle(mirror::Field::CreateFromArtField(soa.Self(), &static_fields0->At(0), true));
  Handle<mirror::Field> field01 =
      hs.NewHandle(mirror::Field::CreateFromArtField(soa.Self(), &static_fields0->At(1), true));
  Handle<mirror::Field> field10 =
      hs.NewHandle(mirror::Field::CreateFromArtField(soa.Self(), &static_fields1->At(0), true));
  Handle<mirror::Field> field11 =
      hs.NewHandle(mirror::Field::CreateFromArtField(soa.Self(), &static_fields1->At(1), true));
  EXPECT_EQ(field00->GetArtField(), &static_fields0->At(0));
  EXPECT_EQ(field01->GetArtField(), &static_fields0->At(1));
  EXPECT_EQ(field10->GetArtField(), &static_fields1->At(0));
  EXPECT_EQ(field11->GetArtField(), &static_fields1->At(1));
}

// TODO: make sure there's a non-abstract implementation of the single-abstract-method on the class.

}  // namespace art
