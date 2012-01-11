// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_COMPILED_METHOD_H_
#define ART_SRC_COMPILED_METHOD_H_

#include <vector>

#include "constants.h"
#include "utils.h"

namespace art {

class CompiledMethod {
 public:
  // Create a CompiledMethod from the oatCompileMethod
  CompiledMethod(InstructionSet instruction_set,
                 std::vector<short>& code,
                 const size_t frame_size_in_bytes,
                 const uint32_t core_spill_mask,
                 const uint32_t fp_spill_mask,
                 std::vector<uint32_t>& mapping_table,
                 std::vector<uint16_t>& vmap_table);

  // Create a CompiledMethod from the JniCompiler
  CompiledMethod(InstructionSet instruction_set,
                 std::vector<uint8_t>& code,
                 const size_t frame_size_in_bytes,
                 const uint32_t core_spill_mask,
                 const uint32_t fp_spill_mask);

  ~CompiledMethod();

  InstructionSet GetInstructionSet() const;
  const std::vector<uint8_t>& GetCode() const;
  size_t GetFrameSizeInBytes() const;
  uint32_t GetCoreSpillMask() const;
  uint32_t GetFpSpillMask() const;
  const std::vector<uint32_t>& GetMappingTable() const;
  const std::vector<uint16_t>& GetVmapTable() const;
  // Aligns an offset from a page aligned value to make it suitable
  // for code storage. important to ensure that PC relative value
  // computations work out as expected on ARM.
  uint32_t AlignCode(uint32_t offset) const;
  static uint32_t AlignCode(uint32_t offset, InstructionSet instruction_set);

  // returns the difference between the code address and a usable PC.
  // mainly to cope with kThumb2 where the lower bit must be set.
  size_t CodeDelta() const;

  // Returns a pointer suitable for invoking the code at the argument
  // code_pointer address.  Mainly to cope with kThumb2 where the
  // lower bit must be set to indicate Thumb mode.
  static const void* CodePointer(const void* code_pointer,
                                 InstructionSet instruction_set);

 private:
  const InstructionSet instruction_set_;
  std::vector<uint8_t> code_;
  const size_t frame_size_in_bytes_;
  const uint32_t core_spill_mask_;
  const uint32_t fp_spill_mask_;
  std::vector<uint32_t> mapping_table_;
  std::vector<uint16_t> vmap_table_;
};

class CompiledInvokeStub {
 public:
  explicit CompiledInvokeStub(std::vector<uint8_t>& code);
  ~CompiledInvokeStub();
  const std::vector<uint8_t>& GetCode() const;
 private:
  std::vector<uint8_t> code_;
};

}  // namespace art

#endif  // ART_SRC_COMPILED_METHOD_H_

