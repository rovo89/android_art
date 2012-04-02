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

#ifndef ART_SRC_CLASS_LOADER_H_
#define ART_SRC_CLASS_LOADER_H_

#include <vector>

#include "dex_file.h"
#include "object.h"

namespace art {

// C++ mirror of java.lang.ClassLoader
class MANAGED ClassLoader : public Object {
 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  Object* packages_;
  ClassLoader* parent_;
  Object* proxyCache_;

  friend struct ClassLoaderOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(ClassLoader);
};

// C++ mirror of dalvik.system.BaseDexClassLoader
class MANAGED BaseDexClassLoader : public ClassLoader {
 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  String* original_library_path_;
  String* original_path_;
  Object* path_list_;

  friend struct BaseDexClassLoaderOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(BaseDexClassLoader);
};

// C++ mirror of dalvik.system.PathClassLoader
class MANAGED PathClassLoader : public BaseDexClassLoader {
 public:
  static PathClassLoader* AllocCompileTime(std::vector<const DexFile*>& dex_files);
  static void SetClass(Class* dalvik_system_PathClassLoader);
  static void ResetClass();
 private:
  static Class* dalvik_system_PathClassLoader_;
  friend struct PathClassLoaderOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(PathClassLoader);
};

}  // namespace art

#endif  // ART_SRC_CLASS_LOADER_H_
