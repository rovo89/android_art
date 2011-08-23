// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CLASS_LOADER_H_
#define ART_SRC_CLASS_LOADER_H_

#include <vector>

#include "dex_file.h"
#include "object.h"

namespace art {

// ClassLoader objects.
class ClassLoader : public Object {
 public:
  static const std::vector<const DexFile*>& GetClassPath(const ClassLoader* class_loader);

  void SetClassPath(std::vector<const DexFile*>& class_path) {
    DCHECK_EQ(0U, class_path_.size());
    class_path_ = class_path;
  }

 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  Object* packages_;
  ClassLoader* parent_;

  // TODO: remove once we can create a real PathClassLoader
  std::vector<const DexFile*> class_path_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(ClassLoader);
};

class BaseDexClassLoader : public ClassLoader {
 private:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  String* original_path_;
  Object* path_list_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(BaseDexClassLoader);
};

class PathClassLoader : public BaseDexClassLoader {
 public:
  static const PathClassLoader* Alloc(std::vector<const DexFile*> dex_files);
  static void SetClass(Class* dalvik_system_PathClassLoader);
  static void ResetClass();
 private:
  static Class* dalvik_system_PathClassLoader_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(PathClassLoader);
};

}  // namespace art

#endif  // ART_SRC_OBJECT_H_
