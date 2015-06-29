#
# Copyright (C) 2012 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(call my-dir)

include art/build/Android.common_build.mk

LIBART_COMPILER_SRC_FILES := \
	compiled_method.cc \
	dex/global_value_numbering.cc \
	dex/gvn_dead_code_elimination.cc \
	dex/local_value_numbering.cc \
	dex/type_inference.cc \
	dex/quick/arm/assemble_arm.cc \
	dex/quick/arm/call_arm.cc \
	dex/quick/arm/fp_arm.cc \
	dex/quick/arm/int_arm.cc \
	dex/quick/arm/target_arm.cc \
	dex/quick/arm/utility_arm.cc \
	dex/quick/arm64/assemble_arm64.cc \
	dex/quick/arm64/call_arm64.cc \
	dex/quick/arm64/fp_arm64.cc \
	dex/quick/arm64/int_arm64.cc \
	dex/quick/arm64/target_arm64.cc \
	dex/quick/arm64/utility_arm64.cc \
	dex/quick/codegen_util.cc \
	dex/quick/dex_file_method_inliner.cc \
	dex/quick/dex_file_to_method_inliner_map.cc \
	dex/quick/gen_common.cc \
	dex/quick/gen_invoke.cc \
	dex/quick/gen_loadstore.cc \
	dex/quick/lazy_debug_frame_opcode_writer.cc \
	dex/quick/local_optimizations.cc \
	dex/quick/mips/assemble_mips.cc \
	dex/quick/mips/call_mips.cc \
	dex/quick/mips/fp_mips.cc \
	dex/quick/mips/int_mips.cc \
	dex/quick/mips/target_mips.cc \
	dex/quick/mips/utility_mips.cc \
	dex/quick/mir_to_lir.cc \
	dex/quick/quick_compiler.cc \
	dex/quick/ralloc_util.cc \
	dex/quick/resource_mask.cc \
	dex/quick/x86/assemble_x86.cc \
	dex/quick/x86/call_x86.cc \
	dex/quick/x86/fp_x86.cc \
	dex/quick/x86/int_x86.cc \
	dex/quick/x86/target_x86.cc \
	dex/quick/x86/utility_x86.cc \
	dex/dex_to_dex_compiler.cc \
	dex/bb_optimizations.cc \
	dex/compiler_ir.cc \
	dex/mir_analysis.cc \
	dex/mir_dataflow.cc \
	dex/mir_field_info.cc \
	dex/mir_graph.cc \
	dex/mir_method_info.cc \
	dex/mir_optimization.cc \
	dex/post_opt_passes.cc \
	dex/pass_driver_me_opts.cc \
	dex/pass_driver_me_post_opt.cc \
	dex/pass_manager.cc \
	dex/ssa_transformation.cc \
	dex/verified_method.cc \
	dex/verification_results.cc \
	dex/vreg_analysis.cc \
	dex/quick_compiler_callbacks.cc \
	driver/compiler_driver.cc \
	driver/compiler_options.cc \
	driver/dex_compilation_unit.cc \
	linker/relative_patcher.cc \
	linker/arm/relative_patcher_arm_base.cc \
	linker/arm/relative_patcher_thumb2.cc \
	linker/arm64/relative_patcher_arm64.cc \
	linker/x86/relative_patcher_x86_base.cc \
	linker/x86/relative_patcher_x86.cc \
	linker/x86_64/relative_patcher_x86_64.cc \
	jit/jit_compiler.cc \
	jni/quick/arm/calling_convention_arm.cc \
	jni/quick/arm64/calling_convention_arm64.cc \
	jni/quick/mips/calling_convention_mips.cc \
	jni/quick/mips64/calling_convention_mips64.cc \
	jni/quick/x86/calling_convention_x86.cc \
	jni/quick/x86_64/calling_convention_x86_64.cc \
	jni/quick/calling_convention.cc \
	jni/quick/jni_compiler.cc \
	optimizing/boolean_simplifier.cc \
	optimizing/builder.cc \
	optimizing/bounds_check_elimination.cc \
	optimizing/code_generator.cc \
	optimizing/code_generator_arm.cc \
	optimizing/code_generator_arm64.cc \
	optimizing/code_generator_mips64.cc \
	optimizing/code_generator_x86.cc \
	optimizing/code_generator_x86_64.cc \
	optimizing/code_generator_utils.cc \
	optimizing/constant_folding.cc \
	optimizing/dead_code_elimination.cc \
	optimizing/graph_checker.cc \
	optimizing/graph_visualizer.cc \
	optimizing/gvn.cc \
	optimizing/inliner.cc \
	optimizing/instruction_simplifier.cc \
	optimizing/intrinsics.cc \
	optimizing/intrinsics_arm.cc \
	optimizing/intrinsics_arm64.cc \
	optimizing/intrinsics_x86.cc \
	optimizing/intrinsics_x86_64.cc \
	optimizing/licm.cc \
	optimizing/locations.cc \
	optimizing/nodes.cc \
	optimizing/optimization.cc \
	optimizing/optimizing_compiler.cc \
	optimizing/parallel_move_resolver.cc \
	optimizing/prepare_for_register_allocation.cc \
	optimizing/primitive_type_propagation.cc \
	optimizing/reference_type_propagation.cc \
	optimizing/register_allocator.cc \
	optimizing/side_effects_analysis.cc \
	optimizing/ssa_builder.cc \
	optimizing/ssa_liveness_analysis.cc \
	optimizing/ssa_phi_elimination.cc \
	optimizing/stack_map_stream.cc \
	trampolines/trampoline_compiler.cc \
	utils/arena_bit_vector.cc \
	utils/arm/assembler_arm.cc \
	utils/arm/assembler_arm32.cc \
	utils/arm/assembler_thumb2.cc \
	utils/arm/managed_register_arm.cc \
	utils/arm64/assembler_arm64.cc \
	utils/arm64/managed_register_arm64.cc \
	utils/assembler.cc \
	utils/mips/assembler_mips.cc \
	utils/mips/managed_register_mips.cc \
	utils/mips64/assembler_mips64.cc \
	utils/mips64/managed_register_mips64.cc \
	utils/x86/assembler_x86.cc \
	utils/x86/managed_register_x86.cc \
	utils/x86_64/assembler_x86_64.cc \
	utils/x86_64/managed_register_x86_64.cc \
	utils/swap_space.cc \
	buffered_output_stream.cc \
	compiler.cc \
	elf_writer.cc \
	elf_writer_debug.cc \
	elf_writer_quick.cc \
	file_output_stream.cc \
	image_writer.cc \
	oat_writer.cc \
	output_stream.cc \
	vector_output_stream.cc

LIBART_COMPILER_CFLAGS :=

LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES := \
  dex/quick/arm/arm_lir.h \
  dex/quick/arm64/arm64_lir.h \
  dex/quick/mips/mips_lir.h \
  dex/quick/resource_mask.h \
  dex/compiler_enums.h \
  dex/global_value_numbering.h \
  dex/pass_me.h \
  driver/compiler_driver.h \
  driver/compiler_options.h \
  image_writer.h \
  optimizing/locations.h \
  utils/arm/constants_arm.h

# $(1): target or host
# $(2): ndebug or debug
define build-libart-compiler
  ifneq ($(1),target)
    ifneq ($(1),host)
      $$(error expected target or host for argument 1, received $(1))
    endif
  endif
  ifneq ($(2),ndebug)
    ifneq ($(2),debug)
      $$(error expected ndebug or debug for argument 2, received $(2))
    endif
  endif

  art_target_or_host := $(1)
  art_ndebug_or_debug := $(2)

  include $(CLEAR_VARS)
  ifeq ($$(art_target_or_host),host)
    LOCAL_IS_HOST_MODULE := true
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := libart-compiler
    LOCAL_SHARED_LIBRARIES += libart
    ifeq ($$(art_target_or_host),target)
      LOCAL_FDO_SUPPORT := true
    endif
  else # debug
    LOCAL_MODULE := libartd-compiler
    LOCAL_SHARED_LIBRARIES += libartd
  endif

  LOCAL_MODULE_TAGS := optional
  LOCAL_MODULE_CLASS := SHARED_LIBRARIES

  LOCAL_SRC_FILES := $$(LIBART_COMPILER_SRC_FILES)

  GENERATED_SRC_DIR := $$(call local-generated-sources-dir)
  ENUM_OPERATOR_OUT_CC_FILES := $$(patsubst %.h,%_operator_out.cc,$$(LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES))
  ENUM_OPERATOR_OUT_GEN := $$(addprefix $$(GENERATED_SRC_DIR)/,$$(ENUM_OPERATOR_OUT_CC_FILES))

$$(ENUM_OPERATOR_OUT_GEN): art/tools/generate-operator-out.py
$$(ENUM_OPERATOR_OUT_GEN): PRIVATE_CUSTOM_TOOL = art/tools/generate-operator-out.py $(LOCAL_PATH) $$< > $$@
$$(ENUM_OPERATOR_OUT_GEN): $$(GENERATED_SRC_DIR)/%_operator_out.cc : $(LOCAL_PATH)/%.h
	$$(transform-generated-source)

  LOCAL_GENERATED_SOURCES += $$(ENUM_OPERATOR_OUT_GEN)

  LOCAL_CFLAGS := $$(LIBART_COMPILER_CFLAGS)
  ifeq ($$(art_target_or_host),target)
    $(call set-target-local-clang-vars)
    $(call set-target-local-cflags-vars,$(2))
  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS)
    LOCAL_LDLIBS := $(ART_HOST_LDLIBS)
    ifeq ($$(art_ndebug_or_debug),debug)
      LOCAL_CFLAGS += $(ART_HOST_DEBUG_CFLAGS)
    else
      LOCAL_CFLAGS += $(ART_HOST_NON_DEBUG_CFLAGS)
    endif
  endif

  LOCAL_C_INCLUDES += $(ART_C_INCLUDES) art/runtime

  ifeq ($$(art_target_or_host),host)
    # For compiler driver TLS.
    LOCAL_LDLIBS += -lpthread
  endif
  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/Android.mk
  # Vixl assembly support for ARM64 targets.
  ifeq ($$(art_ndebug_or_debug),debug)
    LOCAL_SHARED_LIBRARIES += libvixld
  else
    LOCAL_SHARED_LIBRARIES += libvixl
  endif

  LOCAL_NATIVE_COVERAGE := $(ART_COVERAGE)

  ifeq ($$(art_target_or_host),target)
    # For atrace.
    LOCAL_SHARED_LIBRARIES += libcutils
    include $(BUILD_SHARED_LIBRARY)
  else # host
    LOCAL_MULTILIB := both
    include $(BUILD_HOST_SHARED_LIBRARY)
  endif

  ifeq ($$(art_target_or_host),target)
    ifeq ($$(art_ndebug_or_debug),debug)
      $(TARGET_OUT_EXECUTABLES)/dex2oatd: $$(LOCAL_INSTALLED_MODULE)
    else
      $(TARGET_OUT_EXECUTABLES)/dex2oat: $$(LOCAL_INSTALLED_MODULE)
    endif
  else # host
    ifeq ($$(art_ndebug_or_debug),debug)
      $(HOST_OUT_EXECUTABLES)/dex2oatd: $$(LOCAL_INSTALLED_MODULE)
    else
      $(HOST_OUT_EXECUTABLES)/dex2oat: $$(LOCAL_INSTALLED_MODULE)
    endif
  endif

endef

# We always build dex2oat and dependencies, even if the host build is otherwise disabled, since they are used to cross compile for the target.
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-libart-compiler,host,ndebug))
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-libart-compiler,host,debug))
endif
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  $(eval $(call build-libart-compiler,target,ndebug))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-libart-compiler,target,debug))
endif
