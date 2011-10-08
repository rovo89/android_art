// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_loader.h"

#include "class_linker.h"
#include "runtime.h"

namespace art {

bool ClassLoader::use_compile_time_class_path = false;
ClassLoader::Table ClassLoader::compile_time_class_paths_;

const std::vector<const DexFile*>& ClassLoader::GetCompileTimeClassPath(const ClassLoader* class_loader) {
  Runtime* runtime = Runtime::Current();
  if (class_loader == NULL) {
    return runtime->GetClassLinker()->GetBootClassPath();
  }
  CHECK(ClassLoader::UseCompileTimeClassPath());
  Table::const_iterator it = compile_time_class_paths_.find(class_loader);
  CHECK(it != compile_time_class_paths_.end());
  return it->second;
}

void ClassLoader::SetCompileTimeClassPath(const ClassLoader* class_loader,
                                          std::vector<const DexFile*>& class_path) {
  CHECK(!Runtime::Current()->IsStarted());
  use_compile_time_class_path = true;
  compile_time_class_paths_[class_loader] = class_path;
}

// TODO: get global references for these
Class* PathClassLoader::dalvik_system_PathClassLoader_ = NULL;

const PathClassLoader* PathClassLoader::AllocCompileTime(std::vector<const DexFile*>& dex_files) {
  CHECK(!Runtime::Current()->IsStarted());
  DCHECK(dalvik_system_PathClassLoader_ != NULL);
  PathClassLoader* p = down_cast<PathClassLoader*>(dalvik_system_PathClassLoader_->AllocObject());
  SetCompileTimeClassPath(p, dex_files);
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
