// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RAW_DEX_FILE_H_
#define ART_SRC_RAW_DEX_FILE_H_

#include "src/globals.h"
#include "src/leb128.h"
#include "src/logging.h"
#include "src/strutil.h"

#include <map>

namespace art {

class RawDexFile {
 public:
  static const byte kDexMagic[];
  static const byte kDexMagicVersion[];
  static const size_t kSha1DigestSize = 20;

  // Raw header_item.
  struct Header {
    uint8_t magic[8];
    uint32_t checksum;
    uint8_t signature[kSha1DigestSize];
    uint32_t file_size_;  // length of entire file
    uint32_t header_size_;  // offset to start of next section
    uint32_t endian_tag_;
    uint32_t link_size_;
    uint32_t link_off_;
    uint32_t map_off_;
    uint32_t string_ids_size_;
    uint32_t string_ids_off_;
    uint32_t type_ids_size_;
    uint32_t type_ids_off_;
    uint32_t proto_ids_size_;
    uint32_t proto_ids_off_;
    uint32_t field_ids_size_;
    uint32_t field_ids_off_;
    uint32_t method_ids_size_;
    uint32_t method_ids_off_;
    uint32_t class_defs_size_;
    uint32_t class_defs_off_;
    uint32_t data_size_;
    uint32_t data_off_;
  };

  // Raw string_id_item.
  struct StringId {
    uint32_t string_data_off_;  // offset in bytes from the base address
  };

  // Raw type_id_item.
  struct TypeId {
    uint32_t descriptor_idx_;  // index into string_ids
  };

  // Raw field_id_item.
  struct FieldId {
    uint32_t class_idx_;  // index into typeIds list for defining class
    uint32_t type_idx_;  // index into typeIds for field type
    uint32_t name_idx_;  // index into stringIds for field name
  };

  // Raw method_id_item.
  struct MethodId {
    uint32_t class_idx_;  // index into typeIds list for defining class
    uint32_t proto_idx_;  // index into protoIds for method prototype
    uint32_t name_idx_;  // index into stringIds for method name
  };

  // Raw proto_id_item.
  struct ProtoId {
    uint32_t shorty_idx_;  // index into string_ids for shorty descriptor
    uint32_t return_type_idx_;  // index into type_ids list for return type
    uint32_t parameters_off_;  // file offset to type_list for parameter types
  };

  // Raw class_def_item.
  struct ClassDef {
    uint32_t class_idx_;  // index into typeIds for this class
    uint32_t access_flags_;
    uint32_t superclass_idx_;  // index into typeIds for superclass
    uint32_t interfaces_off_;  // file offset to TypeList
    uint32_t source_file_idx_;  // index into stringIds for source file name
    uint32_t annotations_off_;  // file offset to annotations_directory_item
    uint32_t class_data_off_;  // file offset to class_data_item
    uint32_t static_values_off_;  // file offset to EncodedArray
  };

  // Raw type_item.
  struct TypeItem {
    uint16_t type_idx_;  // index into type_ids section
  };

  // Raw type_list.
  class TypeList {
   public:
    uint32_t Size() const {
      return size_;
    }

    const TypeItem& GetTypeItem(uint32_t idx) const {
      CHECK_LT(idx, this->size_);
      return this->list_[idx];
    }

   private:
    uint32_t size_;  // size of the list, in entries
    TypeItem list_[1];  // elements of the list
  };

  // Raw code_item.
  struct Code {
    uint16_t registers_size_;
    uint16_t ins_size_;
    uint16_t outs_size_;
    uint16_t tries_size_;
    uint32_t debug_info_off_;  // file offset to debug info stream
    uint32_t insns_size_;  // size of the insns array, in 2 byte code units
    uint16_t insns_[1];
  };

  // Partially decoded form of class_data_item.
  struct ClassDataHeader {
    uint32_t static_fields_size_;  // the number of static fields
    uint32_t instance_fields_size_;  // the number of instance fields
    uint32_t direct_methods_size_;  // the number of direct methods
    uint32_t virtual_methods_size_;  // the number of virtual methods
  };

  // Decoded form of encoded_field.
  struct Field {
    uint32_t field_idx_;  // index into the field_ids list for the identity of this field
    uint32_t access_flags_;  // access flags for the field
  };

  // Decoded form of encoded_method.
  struct Method {
    uint32_t method_idx_;
    uint32_t access_flags_;
    uint32_t code_off_;
  };

  // Opens a .dex file.
  static RawDexFile* Open(const char* filename);

  // Closes a .dex file.
  ~RawDexFile();

  // Looks up a class definition by its class descriptor.
  const ClassDef* FindClassDef(const char* descriptor);

  // Returns the number of string identifiers in the .dex file.
  size_t NumStringIds() const {
    CHECK(header_ != NULL);
    return header_->string_ids_size_;
  }

  // Returns the number of type identifiers in the .dex file.
  size_t NumTypeIds() const {
    CHECK(header_ != NULL);
    return header_->type_ids_size_;
  }

  // Returns the number of prototype identifiers in the .dex file.
  size_t NumProtoIds() const {
    CHECK(header_ != NULL);
    return header_->proto_ids_size_;
  }

  // Returns the number of field identifiers in the .dex file.
  size_t NumFieldIds() const {
    CHECK(header_ != NULL);
    return header_->field_ids_size_;
  }

  // Returns the number of method identifiers in the .dex file.
  size_t NumMethodIds() const {
    CHECK(header_ != NULL);
    return header_->method_ids_size_;
  }

  // Returns the number of class definitions in the .dex file.
  size_t NumClassDefs() const {
    CHECK(header_ != NULL);
    return header_->class_defs_size_;
  }

  // Returns a pointer to the memory mapped class data.
  const byte* GetClassData(const ClassDef& class_def) const {
    if (class_def.class_data_off_ == 0) {
      LG << "class_def.class_data_off_ == 0";
      return NULL;
    }
    return base_ + class_def.class_data_off_;
  }

  // Decodes the header section from the raw class data bytes.
  void DecodeClassDataHeader(ClassDataHeader* header, const byte* class_data) {
    CHECK(header != NULL);
    memset(header, 0, sizeof(ClassDataHeader));
    if (header != NULL) {
      header->static_fields_size_ = DecodeUnsignedLeb128(&class_data);
      header->instance_fields_size_ = DecodeUnsignedLeb128(&class_data);
      header->direct_methods_size_ = DecodeUnsignedLeb128(&class_data);
      header->virtual_methods_size_ = DecodeUnsignedLeb128(&class_data);
    }
  }

  // Returns the class descriptor string of a class definition.
  const char* GetClassDescriptor(const ClassDef& class_def) const {
    return dexStringByTypeIdx(class_def.class_idx_);
  }

  // Returns the StringId at the specified index.
  const StringId& GetStringId(uint32_t idx) const {
    CHECK_LT(idx, NumStringIds());
    return string_ids_[idx];
  }

  // Returns the TypeId at the specified index.
  const TypeId& GetTypeId(uint32_t idx) const {
    CHECK_LT(idx, NumTypeIds());
    return type_ids_[idx];
  }

  // Returns the FieldId at the specified index.
  const FieldId& GetFieldId(uint32_t idx) const {
    CHECK_LT(idx, NumFieldIds());
    return field_ids_[idx];
  }

  // Returns the MethodId at the specified index.
  const MethodId& GetMethodId(uint32_t idx) const {
    CHECK_LT(idx, NumMethodIds());
    return method_ids_[idx];
  }

  // Returns the ProtoId at the specified index.
  const ProtoId& GetProtoId(uint32_t idx) const {
    CHECK_LT(idx, NumProtoIds());
    return proto_ids_[idx];
  }

  // Returns the ClassDef at the specified index.
  const ClassDef& GetClassDef(uint32_t idx) const {
    CHECK_LT(idx, NumClassDefs());
    return class_defs_[idx];
  }

  const TypeList* GetInterfacesList(const ClassDef& class_def) {
    if (class_def.interfaces_off_ == 0) {
        return NULL;
    } else {
      const byte* addr = base_ + class_def.interfaces_off_;
      return reinterpret_cast<const TypeList*>(addr);
    }
  }

  // From libdex...

  // Returns a pointer to the UTF-8 string data referred to by the
  // given string_id.
  const char* dexGetStringData(const StringId& string_id) const {
    const byte* ptr = base_ + string_id.string_data_off_;
    // Skip the uleb128 length.
    while (*(ptr++) > 0x7f) /* empty */ ;
    return (const char*) ptr;
  }

  // return the UTF-8 encoded string with the specified string_id index
  const char* dexStringById(uint32_t idx) const {
    const StringId& string_id = GetStringId(idx);
    return dexGetStringData(string_id);
  }

  // Get the descriptor string associated with a given type index.
  const char* dexStringByTypeIdx(uint32_t idx) const {
    const TypeId& type_id = GetTypeId(idx);
    return dexStringById(type_id.descriptor_idx_);
  }

 private:
  RawDexFile(const byte* addr, size_t length)
      : base_(addr),
        length_(length),
        header_(0),
        string_ids_(0),
        type_ids_(0),
        field_ids_(0),
        method_ids_(0),
        proto_ids_(0),
        class_defs_(0) {}

  // Top-level initializer that calls other Init methods.
  bool Init();

  // Caches pointers into to the various file sections.
  void InitMembers();

  // Builds the index of descriptors to class definitions.
  void InitIndex();

  // Returns true if the byte string equals the magic value.
  bool CheckMagic(const byte* magic);

  // Returns true if the header magic is of the expected value.
  bool IsMagicValid();

  // The index of descriptors to class definitions.
  typedef std::map<const char*, const RawDexFile::ClassDef*, CStringLt> Index;
  Index index_;

  // The base address of the memory mapping.
  const byte* base_;

  // The length of the memory mapping in bytes.
  const size_t length_;

  // Points to the header section.
  const Header* header_;

  // Points to the base of the string identifier list.
  const StringId* string_ids_;

  // Points to the base of the type identifier list.
  const TypeId* type_ids_;

  // Points to the base of the field identifier list.
  const FieldId* field_ids_;

  // Points to the base of the method identifier list.
  const MethodId* method_ids_;

  // Points to the base of the prototype identifier list.
  const ProtoId* proto_ids_;

  // Points to the base of the class definition list.
  const ClassDef* class_defs_;
};

}  // namespace art

#endif  // ART_SRC_RAW_DEX_FILE_H_
