// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/dex_file.h"
#include "src/heap.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/object.h"
#include "src/raw_dex_file.h"

namespace art {

DexFile* DexFile::Open(const char* filename) {
  RawDexFile* raw = RawDexFile::Open(filename);
  if (raw == NULL) {
    return NULL;
  }
  DexFile* dex_file = new DexFile(raw);
  dex_file->Init();
  return dex_file;
}

DexFile::~DexFile() {
  delete raw_;
  delete[] strings_;
  delete[] classes_;
  delete[] methods_;
  delete[] fields_;
}

void DexFile::Init() {
  num_strings_ = raw_->NumStringIds();
  strings_ = new String*[num_strings_];

  num_classes_ = raw_->NumTypeIds();
  classes_ = new Class*[num_classes_];

  num_methods_ = raw_->NumMethodIds();
  methods_ = new Method*[num_methods_];

  num_fields_ = raw_->NumFieldIds();
  fields_ = new Field*[num_fields_];
}

Class* DexFile::LoadClass(const char* descriptor) {
  const RawDexFile::ClassDef* class_def = raw_->FindClassDef(descriptor);
  if (class_def == NULL) {
    return NULL;
  } else {
    return LoadClass(*class_def);
  }
}

Class* DexFile::LoadClass(const RawDexFile::ClassDef& class_def) {
  const byte* class_data = raw_->GetClassData(class_def);
  RawDexFile::ClassDataHeader header;
  raw_->DecodeClassDataHeader(&header, class_data);

  const char* descriptor = raw_->GetClassDescriptor(class_def);
  CHECK(descriptor != NULL);

  // Allocate storage for the new class object.
  size_t size = Class::Size(header.static_fields_size_);
  Class* klass = Heap::AllocClass(size);
  CHECK(klass != NULL);

  //klass->klass_ = NULL;  // TODO
  klass->descriptor_ = descriptor;
  klass->access_flags_ = class_def.access_flags_;
  klass->class_loader_ = NULL;  // TODO
  klass->dex_file_ = this;
  klass->primitive_type_ = Class::kPrimNot;
  klass->status_ = Class::kClassIdx;

  klass->super_ = reinterpret_cast<Class*>(class_def.superclass_idx_);

  // Load class interfaces.
  LoadClassInterfaces(class_def, klass);

  // Load static fields.
  if (header.static_fields_size_ != 0) {
    for (size_t i = 0; i < header.static_fields_size_; ++i) {
      // TODO
    }
  }

  // Load instance fields.
  if (header.instance_fields_size_ != 0) {
    for (size_t i = 0; i < header.instance_fields_size_; ++i) {
      // TODO
    }
  }

  // Load direct methods.

  // Load virtual methods.

  return klass;
}

void DexFile::LoadClassInterfaces(const RawDexFile::ClassDef& class_def,
                                  Class* klass) {
  const RawDexFile::TypeList* list = raw_->GetInterfacesList(class_def);
  if (list != NULL) {
    klass->interface_count_ = list->Size();
    klass->interfaces_ = new Class*[list->Size()];
    for (size_t i = 0; i < list->Size(); ++i) {
      const RawDexFile::TypeItem& type_item = list->GetTypeItem(i);
      klass->interfaces_[i] = reinterpret_cast<Class*>(type_item.type_idx_);
    }
  }
}

void DexFile::LoadSFields(Class* klass, const RawDexFile::Field* src,
                            Field* dst) {

}

void DexFile::LoadIFields(Class* klass, const RawDexFile::Field* src,
                          Field* dst) {
}

void DexFile::LoadMethod(Class* klass, const RawDexFile::Method* src,
                         Method* dst) {
}



}  // namespace art
