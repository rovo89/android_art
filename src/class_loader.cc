// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_loader.h"

#include "class_linker.h"
#include "runtime.h"

namespace art {

const std::vector<const DexFile*>& ClassLoader::GetClassPath(const ClassLoader* class_loader) {
  if (class_loader == NULL) {
    return Runtime::Current()->GetClassLinker()->GetBootClassPath();
  }
  return class_loader->class_path_;
}

// TODO: get global references for these
Class* PathClassLoader::dalvik_system_PathClassLoader_ = NULL;

const PathClassLoader* PathClassLoader::Alloc(std::vector<const DexFile*> dex_files) {
  PathClassLoader* p = down_cast<PathClassLoader*>(dalvik_system_PathClassLoader_->AllocObject());
  p->SetClassPath(dex_files);
  return p;
}

void PathClassLoader::SetClass(Class* dalvik_system_PathClassLoader) {
  CHECK(dalvik_system_PathClassLoader_ == NULL);
  CHECK(dalvik_system_PathClassLoader != NULL);
  dalvik_system_PathClassLoader_ = dalvik_system_PathClassLoader;
}

void PathClassLoader::ResetClass() {
  CHECK(dalvik_system_PathClassLoader_ != NULL);
  dalvik_system_PathClassLoader_ = NULL;
}

}  // namespace art
