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

public class Main {

  /*
   * Ensure an inlined static invoke explicitly triggers the
   * initialization check of the called method's declaring class, and
   * that the corresponding load class instruction does not get
   * removed before register allocation & code generation.
   */

  /// CHECK-START: void Main.invokeStaticInlined() builder (after)
  /// CHECK-DAG:     <<LoadClass:l\d+>>    LoadClass gen_clinit_check:false
  /// CHECK-DAG:     <<ClinitCheck:l\d+>>  ClinitCheck [<<LoadClass>>]
  /// CHECK-DAG:                           InvokeStaticOrDirect [{{[ij]\d+}},<<ClinitCheck>>]

  /// CHECK-START: void Main.invokeStaticInlined() inliner (after)
  /// CHECK-DAG:     <<LoadClass:l\d+>>    LoadClass gen_clinit_check:false
  /// CHECK-DAG:     <<ClinitCheck:l\d+>>  ClinitCheck [<<LoadClass>>]

  /// CHECK-START: void Main.invokeStaticInlined() inliner (after)
  /// CHECK-NOT:                           InvokeStaticOrDirect

  // The following checks ensure the clinit check instruction added by
  // the builder is pruned by the PrepareForRegisterAllocation, while
  // the load class instruction is preserved.  As the control flow
  // graph is not dumped after (nor before) this step, we check the
  // CFG as it is before the next pass (liveness analysis) instead.

  /// CHECK-START: void Main.invokeStaticInlined() liveness (before)
  /// CHECK-DAG:                           LoadClass gen_clinit_check:true

  /// CHECK-START: void Main.invokeStaticInlined() liveness (before)
  /// CHECK-NOT:                           ClinitCheck
  /// CHECK-NOT:                           InvokeStaticOrDirect

  static void invokeStaticInlined() {
    ClassWithClinit1.$opt$inline$StaticMethod();
  }

  static class ClassWithClinit1 {
    static {
      System.out.println("Main$ClassWithClinit1's static initializer");
    }

    static void $opt$inline$StaticMethod() {
    }
  }

  /*
   * Ensure a non-inlined static invoke eventually has an implicit
   * initialization check of the called method's declaring class.
   */

  /// CHECK-START: void Main.invokeStaticNotInlined() builder (after)
  /// CHECK:         <<LoadClass:l\d+>>    LoadClass gen_clinit_check:false
  /// CHECK:         <<ClinitCheck:l\d+>>  ClinitCheck [<<LoadClass>>]
  /// CHECK:                               InvokeStaticOrDirect [{{[ij]\d+}},<<ClinitCheck>>]

  /// CHECK-START: void Main.invokeStaticNotInlined() inliner (after)
  /// CHECK:         <<LoadClass:l\d+>>    LoadClass gen_clinit_check:false
  /// CHECK:         <<ClinitCheck:l\d+>>  ClinitCheck [<<LoadClass>>]
  /// CHECK:                               InvokeStaticOrDirect [{{([ij]\d+,)?}}<<ClinitCheck>>]

  // The following checks ensure the clinit check and load class
  // instructions added by the builder are pruned by the
  // PrepareForRegisterAllocation.  As the control flow graph is not
  // dumped after (nor before) this step, we check the CFG as it is
  // before the next pass (liveness analysis) instead.

  /// CHECK-START: void Main.invokeStaticNotInlined() liveness (before)
  /// CHECK:                               InvokeStaticOrDirect

  /// CHECK-START: void Main.invokeStaticNotInlined() liveness (before)
  /// CHECK-NOT:                           LoadClass
  /// CHECK-NOT:                           ClinitCheck

  static void invokeStaticNotInlined() {
    ClassWithClinit2.$noinline$staticMethod();
  }

  static class ClassWithClinit2 {
    static {
      System.out.println("Main$ClassWithClinit2's static initializer");
    }

    static boolean doThrow = false;

    static void $noinline$staticMethod() {
      if (doThrow) {
        // Try defeating inlining.
        throw new Error();
      }
    }
  }

  /*
   * Ensure an inlined call to a static method whose declaring class
   * is statically known to have been initialized does not require an
   * explicit clinit check.
   */

  /// CHECK-START: void Main$ClassWithClinit3.invokeStaticInlined() builder (after)
  /// CHECK-DAG:                           InvokeStaticOrDirect

  /// CHECK-START: void Main$ClassWithClinit3.invokeStaticInlined() builder (after)
  /// CHECK-NOT:                           LoadClass
  /// CHECK-NOT:                           ClinitCheck

  /// CHECK-START: void Main$ClassWithClinit3.invokeStaticInlined() inliner (after)
  /// CHECK-NOT:                           LoadClass
  /// CHECK-NOT:                           ClinitCheck
  /// CHECK-NOT:                           InvokeStaticOrDirect

  static class ClassWithClinit3 {
    static void invokeStaticInlined() {
      // The invocation of invokeStaticInlined triggers the
      // initialization of ClassWithClinit3, meaning that the
      // hereinbelow call to $opt$inline$StaticMethod does not need a
      // clinit check.
      $opt$inline$StaticMethod();
    }

    static {
      System.out.println("Main$ClassWithClinit3's static initializer");
    }

    static void $opt$inline$StaticMethod() {
    }
  }

  /*
   * Ensure an non-inlined call to a static method whose declaring
   * class is statically known to have been initialized does not
   * require an explicit clinit check.
   */

  /// CHECK-START: void Main$ClassWithClinit4.invokeStaticNotInlined() builder (after)
  /// CHECK-DAG:                           InvokeStaticOrDirect

  /// CHECK-START: void Main$ClassWithClinit4.invokeStaticNotInlined() builder (after)
  /// CHECK-NOT:                           LoadClass
  /// CHECK-NOT:                           ClinitCheck

  /// CHECK-START: void Main$ClassWithClinit4.invokeStaticNotInlined() inliner (after)
  /// CHECK-DAG:                           InvokeStaticOrDirect

  /// CHECK-START: void Main$ClassWithClinit4.invokeStaticNotInlined() inliner (after)
  /// CHECK-NOT:                           LoadClass
  /// CHECK-NOT:                           ClinitCheck

  static class ClassWithClinit4 {
    static void invokeStaticNotInlined() {
      // The invocation of invokeStaticNotInlined triggers the
      // initialization of ClassWithClinit4, meaning that the
      // call to staticMethod below does not need a clinit
      // check.
      $noinline$staticMethod();
    }

    static {
      System.out.println("Main$ClassWithClinit4's static initializer");
    }

    static boolean doThrow = false;

    static void $noinline$staticMethod() {
      if (doThrow) {
        // Try defeating inlining.
        throw new Error();
      }
    }
  }

  /*
   * Ensure an inlined call to a static method whose declaring class
   * is a super class of the caller's class does not require an
   * explicit clinit check.
   */

  /// CHECK-START: void Main$SubClassOfClassWithClinit5.invokeStaticInlined() builder (after)
  /// CHECK-DAG:                           InvokeStaticOrDirect

  /// CHECK-START: void Main$SubClassOfClassWithClinit5.invokeStaticInlined() builder (after)
  /// CHECK-NOT:                           LoadClass
  /// CHECK-NOT:                           ClinitCheck

  /// CHECK-START: void Main$SubClassOfClassWithClinit5.invokeStaticInlined() inliner (after)
  /// CHECK-NOT:                           LoadClass
  /// CHECK-NOT:                           ClinitCheck
  /// CHECK-NOT:                           InvokeStaticOrDirect

  static class ClassWithClinit5 {
    static void $opt$inline$StaticMethod() {
    }

    static {
      System.out.println("Main$ClassWithClinit5's static initializer");
    }
  }

  static class SubClassOfClassWithClinit5 extends ClassWithClinit5 {
    static void invokeStaticInlined() {
      ClassWithClinit5.$opt$inline$StaticMethod();
    }
  }

  /*
   * Ensure an non-inlined call to a static method whose declaring
   * class is a super class of the caller's class does not require an
   * explicit clinit check.
   */

  /// CHECK-START: void Main$SubClassOfClassWithClinit6.invokeStaticNotInlined() builder (after)
  /// CHECK-DAG:                           InvokeStaticOrDirect

  /// CHECK-START: void Main$SubClassOfClassWithClinit6.invokeStaticNotInlined() builder (after)
  /// CHECK-NOT:                           LoadClass
  /// CHECK-NOT:                           ClinitCheck

  /// CHECK-START: void Main$SubClassOfClassWithClinit6.invokeStaticNotInlined() inliner (after)
  /// CHECK-DAG:                           InvokeStaticOrDirect

  /// CHECK-START: void Main$SubClassOfClassWithClinit6.invokeStaticNotInlined() inliner (after)
  /// CHECK-NOT:                           LoadClass
  /// CHECK-NOT:                           ClinitCheck

  static class ClassWithClinit6 {
    static boolean doThrow = false;

    static void $noinline$staticMethod() {
      if (doThrow) {
        // Try defeating inlining.
        throw new Error();
      }
    }

    static {
      System.out.println("Main$ClassWithClinit6's static initializer");
    }
  }

  static class SubClassOfClassWithClinit6 extends ClassWithClinit6 {
    static void invokeStaticNotInlined() {
      ClassWithClinit6.$noinline$staticMethod();
    }
  }


  /*
   * Verify that if we have a static call immediately after the load class
   * we don't do generate a clinit check.
   */

  /// CHECK-START: void Main.noClinitBecauseOfInvokeStatic() liveness (before)
  /// CHECK-DAG:     <<IntConstant:i\d+>>  IntConstant 0
  /// CHECK-DAG:     <<LoadClass:l\d+>>    LoadClass gen_clinit_check:false
  /// CHECK-DAG:                           InvokeStaticOrDirect
  /// CHECK-DAG:                           StaticFieldSet [<<LoadClass>>,<<IntConstant>>]

  /// CHECK-START: void Main.noClinitBecauseOfInvokeStatic() liveness (before)
  /// CHECK-NOT:                           ClinitCheck

  static void noClinitBecauseOfInvokeStatic() {
    ClassWithClinit2.$noinline$staticMethod();
    ClassWithClinit2.doThrow = false;
  }

  /*
   * Verify that if the static call is after a field access, the load class
   * will generate a clinit check.
   */

  /// CHECK-START: void Main.clinitBecauseOfFieldAccess() liveness (before)
  /// CHECK-DAG:     <<IntConstant:i\d+>>  IntConstant 0
  /// CHECK-DAG:     <<LoadClass:l\d+>>    LoadClass gen_clinit_check:true
  /// CHECK-DAG:                           StaticFieldSet [<<LoadClass>>,<<IntConstant>>]
  /// CHECK-DAG:                           InvokeStaticOrDirect

  /// CHECK-START: void Main.clinitBecauseOfFieldAccess() liveness (before)
  /// CHECK-NOT:                           ClinitCheck
  static void clinitBecauseOfFieldAccess() {
    ClassWithClinit2.doThrow = false;
    ClassWithClinit2.$noinline$staticMethod();
  }

  // TODO: Add a test for the case of a static method whose declaring
  // class type index is not available (i.e. when `storage_index`
  // equals `DexFile::kDexNoIndex` in
  // art::HGraphBuilder::BuildInvoke).

  public static void main(String[] args) {
    invokeStaticInlined();
    invokeStaticNotInlined();
    ClassWithClinit3.invokeStaticInlined();
    ClassWithClinit4.invokeStaticNotInlined();
    SubClassOfClassWithClinit5.invokeStaticInlined();
    SubClassOfClassWithClinit6.invokeStaticNotInlined();
  }
}
