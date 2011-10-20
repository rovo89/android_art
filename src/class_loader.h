// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CLASS_LOADER_H_
#define ART_SRC_CLASS_LOADER_H_

#include <vector>

#include "dex_file.h"
#include "object.h"
#include "unordered_map.h"

namespace art {

// C++ mirror of java.lang.ClassLoader
class MANAGED ClassLoader : public Object {
 public:
  static const std::vector<const DexFile*>& GetCompileTimeClassPath(const ClassLoader* class_loader);
  static void SetCompileTimeClassPath(const ClassLoader* class_loader, std::vector<const DexFile*>& class_path);
  static bool UseCompileTimeClassPath() {
    return use_compile_time_class_path;
  }

 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  Object* packages_;
  ClassLoader* parent_;
  Object* proxyCache_;

  typedef std::tr1::unordered_map<const ClassLoader*, std::vector<const DexFile*>, ObjectIdentityHash> Table;
  static Table compile_time_class_paths_;

  static bool use_compile_time_class_path;

  friend struct ClassLoaderOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(ClassLoader);
};

// C++ mirror of dalvik.system.BaseDexClassLoader
// TODO: add MANAGED when class_path_ removed
class BaseDexClassLoader : public ClassLoader {
 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  String* original_path_;
  Object* path_list_;

  friend struct BaseDexClassLoaderOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(BaseDexClassLoader);
};

// C++ mirror of dalvik.system.PathClassLoader
// TODO: add MANAGED when class_path_ removed
class PathClassLoader : public BaseDexClassLoader {
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

#endif  // ART_SRC_OBJECT_H_
