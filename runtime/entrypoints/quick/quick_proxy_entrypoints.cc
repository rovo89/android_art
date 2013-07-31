/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "quick_argument_visitor.h"
#include "dex_file-inl.h"
#include "entrypoints/entrypoint_utils.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "object_utils.h"
#include "reflection.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "well_known_classes.h"

#include "ScopedLocalRef.h"

namespace art {

// Visits arguments on the stack placing them into the args vector, Object* arguments are converted
// to jobjects.
class BuildQuickArgumentVisitor : public QuickArgumentVisitor {
 public:
  BuildQuickArgumentVisitor(MethodHelper& caller_mh, mirror::AbstractMethod** sp,
                            ScopedObjectAccessUnchecked& soa, std::vector<jvalue>& args) :
    QuickArgumentVisitor(caller_mh, sp), soa_(soa), args_(args) {}

  virtual void Visit() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    jvalue val;
    Primitive::Type type = GetParamPrimitiveType();
    switch (type) {
      case Primitive::kPrimNot: {
        mirror::Object* obj = *reinterpret_cast<mirror::Object**>(GetParamAddress());
        val.l = soa_.AddLocalReference<jobject>(obj);
        break;
      }
      case Primitive::kPrimLong:  // Fall-through.
      case Primitive::kPrimDouble:
        if (IsSplitLongOrDouble()) {
          val.j = ReadSplitLongParam();
        } else {
          val.j = *reinterpret_cast<jlong*>(GetParamAddress());
        }
        break;
      case Primitive::kPrimBoolean:  // Fall-through.
      case Primitive::kPrimByte:     // Fall-through.
      case Primitive::kPrimChar:     // Fall-through.
      case Primitive::kPrimShort:    // Fall-through.
      case Primitive::kPrimInt:      // Fall-through.
      case Primitive::kPrimFloat:
        val.i =  *reinterpret_cast<jint*>(GetParamAddress());
        break;
      case Primitive::kPrimVoid:
        LOG(FATAL) << "UNREACHABLE";
        val.j = 0;
        break;
    }
    args_.push_back(val);
  }

 private:
  ScopedObjectAccessUnchecked& soa_;
  std::vector<jvalue>& args_;

  DISALLOW_COPY_AND_ASSIGN(BuildQuickArgumentVisitor);
};

// Handler for invocation on proxy methods. On entry a frame will exist for the proxy object method
// which is responsible for recording callee save registers. We explicitly place into jobjects the
// incoming reference arguments (so they survive GC). We invoke the invocation handler, which is a
// field within the proxy object, which will box the primitive arguments and deal with error cases.
extern "C" uint64_t artQuickProxyInvokeHandler(mirror::AbstractMethod* proxy_method,
                                          mirror::Object* receiver,
                                          Thread* self, mirror::AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Ensure we don't get thread suspension until the object arguments are safely in jobjects.
  const char* old_cause =
      self->StartAssertNoThreadSuspension("Adding to IRT proxy object arguments");
  // Register the top of the managed stack, making stack crawlable.
  DCHECK_EQ(*sp, proxy_method);
  self->SetTopOfStack(sp, 0);
  DCHECK_EQ(proxy_method->GetFrameSizeInBytes(),
            Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
  self->VerifyStack();
  // Start new JNI local reference state.
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);
  // Create local ref. copies of proxy method and the receiver.
  jobject rcvr_jobj = soa.AddLocalReference<jobject>(receiver);

  // Placing arguments into args vector and remove the receiver.
  MethodHelper proxy_mh(proxy_method);
  std::vector<jvalue> args;
  BuildQuickArgumentVisitor local_ref_visitor(proxy_mh, sp, soa, args);
  local_ref_visitor.VisitArguments();
  args.erase(args.begin());

  // Convert proxy method into expected interface method.
  mirror::AbstractMethod* interface_method = proxy_method->FindOverriddenMethod();
  DCHECK(interface_method != NULL);
  DCHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);
  jobject interface_method_jobj = soa.AddLocalReference<jobject>(interface_method);

  // All naked Object*s should now be in jobjects, so its safe to go into the main invoke code
  // that performs allocations.
  self->EndAssertNoThreadSuspension(old_cause);
  JValue result = InvokeProxyInvocationHandler(soa, proxy_mh.GetShorty(),
                                               rcvr_jobj, interface_method_jobj, args);
  return result.GetJ();
}

}  // namespace art
