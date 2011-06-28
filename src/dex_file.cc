// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/dex_file.h"
#include "src/heap.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/object.h"
#include "src/raw_dex_file.h"

namespace art {

DexFile* DexFile::OpenFile(const char* filename) {
  RawDexFile* raw = RawDexFile::OpenFile(filename);
  return Open(raw);
}

DexFile* DexFile::OpenBase64(const char* base64) {
  RawDexFile* raw = RawDexFile::OpenBase64(base64);
  return Open(raw);
}

DexFile* DexFile::Open(RawDexFile* raw) {
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
  RawDexFile::ClassDataHeader header = raw_->ReadClassDataHeader(&class_data);

  const char* descriptor = raw_->GetClassDescriptor(class_def);
  CHECK(descriptor != NULL);

  // Allocate storage for the new class object.
  size_t size = Class::Size(header.static_fields_size_);
  Class* klass = Heap::AllocClass(size);
  CHECK(klass != NULL);  // TODO: throw an OOME

  klass->klass_ = NULL;  // TODO
  klass->descriptor_ = descriptor;
  klass->access_flags_ = class_def.access_flags_;
  klass->class_loader_ = NULL;  // TODO
  klass->dex_file_ = this;
  klass->primitive_type_ = Class::kPrimNot;
  klass->status_ = Class::kClassIdx;

  klass->super_ = reinterpret_cast<Class*>(class_def.superclass_idx_);

  klass->num_sfields_ = header.static_fields_size_;
  klass->num_ifields_ = header.instance_fields_size_;
  klass->num_dmethods_ = header.direct_methods_size_;
  klass->num_vmethods_ = header.virtual_methods_size_;

  klass->source_file_ = raw_->dexGetSourceFile(class_def);

  // Load class interfaces.
  LoadInterfaces(class_def, klass);

  // Load static fields.
  if (klass->num_sfields_ != 0) {
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->num_sfields_; ++i) {
      RawDexFile::Field raw_field;
      raw_->dexReadClassDataField(&class_data, &raw_field, &last_idx);
      LoadField(klass, raw_field, &klass->sfields_[i]);
    }
  }

  // Load instance fields.
  if (klass->num_ifields_ != 0) {
    // TODO: append instance fields to class object
    klass->ifields_ = new IField[klass->num_ifields_];
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->num_ifields_; ++i) {
      RawDexFile::Field raw_field;
      raw_->dexReadClassDataField(&class_data, &raw_field, &last_idx);
      LoadField(klass, raw_field, &klass->ifields_[i]);
    }
  }

  // Load direct methods.
  if (klass->num_dmethods_ != 0) {
    // TODO: append direct methods to class object
    klass->dmethods_ = new Method[klass->num_dmethods_];
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->num_dmethods_; ++i) {
      RawDexFile::Method raw_method;
      raw_->dexReadClassDataMethod(&class_data, &raw_method, &last_idx);
      LoadMethod(klass, raw_method, &klass->dmethods_[i]);
      // TODO: register maps
    }
  }

  // Load virtual methods.
  if (klass->num_vmethods_ != 0) {
    // TODO: append virtual methods to class object
    klass->vmethods_ = new Method[klass->num_vmethods_];
    memset(klass->vmethods_, 0xff, sizeof(Method));
    uint32_t last_idx = 0;
    for (size_t i = 0; i < klass->num_vmethods_; ++i) {
      RawDexFile::Method raw_method;
      raw_->dexReadClassDataMethod(&class_data, &raw_method, &last_idx);
      LoadMethod(klass, raw_method, &klass->vmethods_[i]);
      // TODO: register maps
    }
  }

  return klass;
}

void DexFile::LoadInterfaces(const RawDexFile::ClassDef& class_def,
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

void DexFile::LoadField(Class* klass, const RawDexFile::Field& src,
                        Field* dst) {
  const RawDexFile::FieldId& field_id = raw_->GetFieldId(src.field_idx_);
  dst->klass_ = klass;
  dst->name_ = raw_->dexStringById(field_id.name_idx_);
  dst->signature_ = raw_->dexStringByTypeIdx(field_id.type_idx_);
  dst->access_flags_ = src.access_flags_;
}

void DexFile::LoadMethod(Class* klass, const RawDexFile::Method& src,
                         Method* dst) {
  const RawDexFile::MethodId& method_id = raw_->GetMethodId(src.method_idx_);
  dst->klass_ = klass;
  dst->name_ = raw_->dexStringById(method_id.name_idx_);
  dst->dex_file_ = this;
  dst->proto_idx_ = method_id.proto_idx_;
  dst->shorty_ = raw_->GetShorty(method_id.proto_idx_);
  dst->access_flags_ = src.access_flags_;

  // TODO: check for finalize method

  const RawDexFile::Code* code_item = raw_->GetCode(src);
  if (code_item != NULL) {
    dst->num_registers_ = code_item->registers_size_;
    dst->num_ins_ = code_item->ins_size_;
    dst->num_outs_ = code_item->outs_size_;
    dst->insns_ = code_item->insns_;
  } else {
    uint16_t num_args = dst->NumArgRegisters();
    if (!dst->IsStatic()) {
      ++num_args;
    }
    dst->num_registers_ = dst->num_ins_ + num_args;
    // TODO: native methods
  }
}

}  // namespace art
