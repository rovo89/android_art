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

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;

class MyClassLoader extends ClassLoader {
  MyClassLoader() throws Exception {
    super(MyClassLoader.class.getClassLoader());

    // Some magic to get access to the pathList field of BaseDexClassLoader.
    ClassLoader loader = getClass().getClassLoader();
    Class<?> baseDexClassLoader = loader.getClass().getSuperclass();
    Field f = baseDexClassLoader.getDeclaredField("pathList");
    f.setAccessible(true);
    Object pathList = f.get(loader);

    // Some magic to get access to the dexField field of pathList.
    f = pathList.getClass().getDeclaredField("dexElements");
    f.setAccessible(true);
    dexElements = (Object[]) f.get(pathList);
    dexFileField = dexElements[0].getClass().getDeclaredField("dexFile");
    dexFileField.setAccessible(true);
  }

  Object[] dexElements;
  Field dexFileField;

  protected Class<?> loadClass(String className, boolean resolve) throws ClassNotFoundException {
    System.out.println("Request for " + className);

    // We're only going to handle LoadedByMyClassLoader.
    if (className != "LoadedByMyClassLoader") {
      return getParent().loadClass(className);
    }

    // Mimic what DexPathList.findClass is doing.
    try {
      for (Object element : dexElements) {
        Object dex = dexFileField.get(element);
        Method method = dex.getClass().getDeclaredMethod(
            "loadClassBinaryName", String.class, ClassLoader.class, List.class);

        if (dex != null) {
          Class clazz = (Class)method.invoke(dex, className, this, null);
          if (clazz != null) {
            return clazz;
          }
        }
      }
    } catch (Exception e) { /* Ignore */ }
    return null;
  }
}

class LoadedByMyClassLoader {
  /// CHECK-START: void LoadedByMyClassLoader.bar() inliner (before)
  /// CHECK:      LoadClass
  /// CHECK-NEXT: ClinitCheck
  /// CHECK-NEXT: InvokeStaticOrDirect
  /// CHECK-NEXT: LoadClass
  /// CHECK-NEXT: ClinitCheck
  /// CHECK-NEXT: StaticFieldGet
  /// CHECK-NEXT: LoadString
  /// CHECK-NEXT: NullCheck
  /// CHECK-NEXT: InvokeVirtual

  /// CHECK-START: void LoadedByMyClassLoader.bar() inliner (after)
  /// CHECK:      LoadClass
  /// CHECK-NEXT: ClinitCheck
                /* We inlined FirstSeenByMyClassLoader.$inline$bar */
  /// CHECK-NEXT: LoadClass
  /// CHECK-NEXT: ClinitCheck
  /// CHECK-NEXT: StaticFieldGet
  /// CHECK-NEXT: LoadString
  /// CHECK-NEXT: NullCheck
  /// CHECK-NEXT: InvokeVirtual

  /// CHECK-START: void LoadedByMyClassLoader.bar() register (before)
                /* Load and initialize FirstSeenByMyClassLoader */
  /// CHECK:      LoadClass gen_clinit_check:true
                /* Load and initialize System */
  /// CHECK-NEXT: LoadClass gen_clinit_check:true
  /// CHECK-NEXT: StaticFieldGet
  /// CHECK-NEXT: LoadString
  /// CHECK-NEXT: NullCheck
  /// CHECK-NEXT: InvokeVirtual
  public static void bar() {
    FirstSeenByMyClassLoader.$inline$bar();
    System.out.println("In between the two calls.");
    FirstSeenByMyClassLoader.$noinline$bar();
  }
}

public class Main {
  public static void main(String[] args) throws Exception {
    MyClassLoader o = new MyClassLoader();
    Class foo = o.loadClass("LoadedByMyClassLoader");
    Method m = foo.getDeclaredMethod("bar");
    m.invoke(null);
  }
}
