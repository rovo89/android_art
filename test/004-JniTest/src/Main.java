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

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

public class Main {
    public static void main(String[] args) {
        System.loadLibrary("arttest");
        testFindClassOnAttachedNativeThread();
        testFindFieldOnAttachedNativeThread();
        testReflectFieldGetFromAttachedNativeThreadNative();
        testCallStaticVoidMethodOnSubClass();
        testGetMirandaMethod();
        testZeroLengthByteBuffers();
        testByteMethod();
        testShortMethod();
        testBooleanMethod();
        testCharMethod();
        testIsAssignableFromOnPrimitiveTypes();
        testShallowGetCallingClassLoader();
        testShallowGetStackClass2();
        testCallNonvirtual();
        testNewStringObject();
        testRemoveLocalObject();
        testProxyGetMethodID();
    }

    private static native void testFindClassOnAttachedNativeThread();

    private static boolean testFindFieldOnAttachedNativeThreadField;

    private static native void testReflectFieldGetFromAttachedNativeThreadNative();

    public static boolean testReflectFieldGetFromAttachedNativeThreadField;

    private static void testFindFieldOnAttachedNativeThread() {
      testFindFieldOnAttachedNativeThreadNative();
      if (!testFindFieldOnAttachedNativeThreadField) {
            throw new AssertionError();
        }
    }

    private static native void testFindFieldOnAttachedNativeThreadNative();

    private static void testCallStaticVoidMethodOnSubClass() {
        testCallStaticVoidMethodOnSubClassNative();
        if (!testCallStaticVoidMethodOnSubClass_SuperClass.executed) {
            throw new AssertionError();
        }
    }

    private static native void testCallStaticVoidMethodOnSubClassNative();

    private static class testCallStaticVoidMethodOnSubClass_SuperClass {
        private static boolean executed = false;
        private static void execute() {
            executed = true;
        }
    }

    private static class testCallStaticVoidMethodOnSubClass_SubClass
        extends testCallStaticVoidMethodOnSubClass_SuperClass {
    }

    private static native Method testGetMirandaMethodNative();

    private static void testGetMirandaMethod() {
        Method m = testGetMirandaMethodNative();
        if (m.getDeclaringClass() != testGetMirandaMethod_MirandaInterface.class) {
            throw new AssertionError();
        }
    }

    private static native void testZeroLengthByteBuffers();

    private static abstract class testGetMirandaMethod_MirandaAbstract implements testGetMirandaMethod_MirandaInterface {
        public boolean inAbstract() {
            return true;
        }
    }

    private static interface testGetMirandaMethod_MirandaInterface {
        public boolean inInterface();
    }

    // Test sign-extension for values < 32b

    static native byte byteMethod(byte b1, byte b2, byte b3, byte b4, byte b5, byte b6, byte b7,
        byte b8, byte b9, byte b10);

    private static void testByteMethod() {
      byte returns[] = { 0, 1, 2, 127, -1, -2, -128 };
      for (int i = 0; i < returns.length; i++) {
        byte result = byteMethod((byte)i, (byte)2, (byte)(-3), (byte)4, (byte)(-5), (byte)6,
            (byte)(-7), (byte)8, (byte)(-9), (byte)10);
        if (returns[i] != result) {
          System.out.println("Run " + i + " with " + returns[i] + " vs " + result);
          throw new AssertionError();
        }
      }
    }

    private static native void removeLocalObject(Object o);

    private static void testRemoveLocalObject() {
        removeLocalObject(new Object());
    }
    
    private static native short shortMethod(short s1, short s2, short s3, short s4, short s5, short s6, short s7,
        short s8, short s9, short s10);

    private static void testShortMethod() {
      short returns[] = { 0, 1, 2, 127, 32767, -1, -2, -128, -32768 };
      for (int i = 0; i < returns.length; i++) {
        short result = shortMethod((short)i, (short)2, (short)(-3), (short)4, (short)(-5), (short)6,
            (short)(-7), (short)8, (short)(-9), (short)10);
        if (returns[i] != result) {
          System.out.println("Run " + i + " with " + returns[i] + " vs " + result);
          throw new AssertionError();
        }
      }
    }

    // Test zero-extension for values < 32b

    private static native boolean booleanMethod(boolean b1, boolean b2, boolean b3, boolean b4, boolean b5, boolean b6, boolean b7,
        boolean b8, boolean b9, boolean b10);

    private static void testBooleanMethod() {
      if (booleanMethod(false, true, false, true, false, true, false, true, false, true)) {
        throw new AssertionError();
      }

      if (!booleanMethod(true, true, false, true, false, true, false, true, false, true)) {
        throw new AssertionError();
      }
    }

    private static native char charMethod(char c1, char c2, char c3, char c4, char c5, char c6, char c7,
        char c8, char c9, char c10);

    private static void testCharMethod() {
      char returns[] = { (char)0, (char)1, (char)2, (char)127, (char)255, (char)256, (char)15000,
          (char)34000 };
      for (int i = 0; i < returns.length; i++) {
        char result = charMethod((char)i, 'a', 'b', 'c', '0', '1', '2', (char)1234, (char)2345,
            (char)3456);
        if (returns[i] != result) {
          System.out.println("Run " + i + " with " + (int)returns[i] + " vs " + (int)result);
          throw new AssertionError();
        }
      }
    }

    // http://b/16531674
    private static void testIsAssignableFromOnPrimitiveTypes() {
      if (!nativeIsAssignableFrom(int.class, Integer.TYPE)) {
        System.out.println("IsAssignableFrom(int.class, Integer.TYPE) returned false, expected true");
        throw new AssertionError();
      }

      if (!nativeIsAssignableFrom(Integer.TYPE, int.class)) {
        System.out.println("IsAssignableFrom(Integer.TYPE, int.class) returned false, expected true");
        throw new AssertionError();
      }
    }

    private static native boolean nativeIsAssignableFrom(Class<?> from, Class<?> to);

    private static void testShallowGetCallingClassLoader() {
        nativeTestShallowGetCallingClassLoader();
    }

    private native static void nativeTestShallowGetCallingClassLoader();

    private static void testShallowGetStackClass2() {
        nativeTestShallowGetStackClass2();
    }

    private static native void nativeTestShallowGetStackClass2();

    private static native void testCallNonvirtual();

    private static native void testNewStringObject();

    private interface SimpleInterface {
        void a();
    }

    private static class DummyInvocationHandler implements InvocationHandler {
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            return null;
        }
    }

    private static void testProxyGetMethodID() {
        InvocationHandler handler = new DummyInvocationHandler();
        SimpleInterface proxy =
                (SimpleInterface) Proxy.newProxyInstance(SimpleInterface.class.getClassLoader(),
                        new Class[] {SimpleInterface.class}, handler);
        if (testGetMethodID(SimpleInterface.class) == 0) {
            throw new AssertionError();
        }
        if (testGetMethodID(proxy.getClass()) == 0) {
            throw new AssertionError();
        }
    }

    private static native long testGetMethodID(Class<?> c);
}

class JniCallNonvirtualTest {
    public boolean nonstaticMethodSuperCalled = false;
    public boolean nonstaticMethodSubCalled = false;

    private static native void testCallNonvirtual();

    public JniCallNonvirtualTest() {
        System.out.println("Super.<init>");
    }

    public static void staticMethod() {
        System.out.println("Super.staticMethod");
    }

    public void nonstaticMethod() {
        System.out.println("Super.nonstaticMethod");
        nonstaticMethodSuperCalled = true;
    }
}

class JniCallNonvirtualTestSubclass extends JniCallNonvirtualTest {

    public JniCallNonvirtualTestSubclass() {
        System.out.println("Subclass.<init>");
    }

    public static void staticMethod() {
        System.out.println("Subclass.staticMethod");
    }

    public void nonstaticMethod() {
        System.out.println("Subclass.nonstaticMethod");
        nonstaticMethodSubCalled = true;
    }
}
