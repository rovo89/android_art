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
	debug/elf_debug_writer.cc \
	dex/dex_to_dex_compiler.cc \
	dex/verified_method.cc \
	dex/verification_results.cc \
	dex/quick_compiler_callbacks.cc \
	dex/quick/dex_file_method_inliner.cc \
	dex/quick/dex_file_to_method_inliner_map.cc \
	driver/compiled_method_storage.cc \
	driver/compiler_driver.cc \
	driver/compiler_options.cc \
	driver/dex_compilation_unit.cc \
	linker/buffered_output_stream.cc \
	linker/file_output_stream.cc \
	linker/multi_oat_relative_patcher.cc \
	linker/output_stream.cc \
	linker/vector_output_stream.cc \
	linker/relative_patcher.cc \
	jit/jit_compiler.cc \
	jni/quick/calling_convention.cc \
	jni/quick/jni_compiler.cc \
	optimizing/block_builder.cc \
	optimizing/bounds_check_elimination.cc \
	optimizing/builder.cc \
	optimizing/code_generator.cc \
	optimizing/code_generator_utils.cc \
	optimizing/constant_folding.cc \
	optimizing/dead_code_elimination.cc \
	optimizing/dex_cache_array_fixups_arm.cc \
	optimizing/graph_checker.cc \
	optimizing/graph_visualizer.cc \
	optimizing/gvn.cc \
	optimizing/induction_var_analysis.cc \
	optimizing/induction_var_range.cc \
	optimizing/inliner.cc \
	optimizing/instruction_builder.cc \
	optimizing/instruction_simplifier.cc \
	optimizing/intrinsics.cc \
	optimizing/licm.cc \
	optimizing/load_store_elimination.cc \
	optimizing/locations.cc \
	optimizing/nodes.cc \
	optimizing/nodes_arm64.cc \
	optimizing/optimization.cc \
	optimizing/optimizing_compiler.cc \
	optimizing/parallel_move_resolver.cc \
	optimizing/prepare_for_register_allocation.cc \
	optimizing/reference_type_propagation.cc \
	optimizing/register_allocator.cc \
	optimizing/select_generator.cc \
	optimizing/sharpening.cc \
	optimizing/side_effects_analysis.cc \
	optimizing/ssa_builder.cc \
	optimizing/ssa_liveness_analysis.cc \
	optimizing/ssa_phi_elimination.cc \
	optimizing/stack_map_stream.cc \
	trampolines/trampoline_compiler.cc \
	utils/assembler.cc \
	utils/swap_space.cc \
	compiler.cc \
	elf_writer.cc \
	elf_writer_quick.cc \
	image_writer.cc \
	oat_writer.cc

LIBART_COMPILER_SRC_FILES_arm := \
	jni/quick/arm/calling_convention_arm.cc \
	linker/arm/relative_patcher_arm_base.cc \
	linker/arm/relative_patcher_thumb2.cc \
	optimizing/code_generator_arm.cc \
	optimizing/intrinsics_arm.cc \
	utils/arm/assembler_arm.cc \
	utils/arm/assembler_arm32.cc \
	utils/arm/assembler_thumb2.cc \
	utils/arm/managed_register_arm.cc \

# TODO We should really separate out those files that are actually needed for both variants of an
# architecture into its own category. Currently we just include all of the 32bit variant in the
# 64bit variant. It also might be good to allow one to compile only the 64bit variant without the
# 32bit one.
LIBART_COMPILER_SRC_FILES_arm64 := \
    $(LIBART_COMPILER_SRC_FILES_arm) \
	jni/quick/arm64/calling_convention_arm64.cc \
	linker/arm64/relative_patcher_arm64.cc \
	optimizing/code_generator_arm64.cc \
	optimizing/instruction_simplifier_arm.cc \
	optimizing/instruction_simplifier_arm64.cc \
	optimizing/instruction_simplifier_shared.cc \
	optimizing/intrinsics_arm64.cc \
	utils/arm64/assembler_arm64.cc \
	utils/arm64/managed_register_arm64.cc \

LIBART_COMPILER_SRC_FILES_mips := \
	jni/quick/mips/calling_convention_mips.cc \
	optimizing/code_generator_mips.cc \
	optimizing/intrinsics_mips.cc \
	utils/mips/assembler_mips.cc \
	utils/mips/managed_register_mips.cc \

LIBART_COMPILER_SRC_FILES_mips64 := \
    $(LIBART_COMPILER_SRC_FILES_mips) \
	jni/quick/mips64/calling_convention_mips64.cc \
	optimizing/code_generator_mips64.cc \
	optimizing/intrinsics_mips64.cc \
	utils/mips64/assembler_mips64.cc \
	utils/mips64/managed_register_mips64.cc \


LIBART_COMPILER_SRC_FILES_x86 := \
	jni/quick/x86/calling_convention_x86.cc \
	linker/x86/relative_patcher_x86.cc \
	linker/x86/relative_patcher_x86_base.cc \
	optimizing/code_generator_x86.cc \
	optimizing/intrinsics_x86.cc \
	optimizing/pc_relative_fixups_x86.cc \
	utils/x86/assembler_x86.cc \
	utils/x86/managed_register_x86.cc \

LIBART_COMPILER_SRC_FILES_x86_64 := \
    $(LIBART_COMPILER_SRC_FILES_x86) \
	jni/quick/x86_64/calling_convention_x86_64.cc \
	linker/x86_64/relative_patcher_x86_64.cc \
	optimizing/intrinsics_x86_64.cc \
	optimizing/code_generator_x86_64.cc \
	utils/x86_64/assembler_x86_64.cc \
	utils/x86_64/managed_register_x86_64.cc \


LIBART_COMPILER_CFLAGS :=

LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES := \
  compiled_method.h \
  dex/compiler_enums.h \
  dex/dex_to_dex_compiler.h \
  driver/compiler_driver.h \
  driver/compiler_options.h \
  image_writer.h \
  optimizing/locations.h

LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_arm := \
  utils/arm/constants_arm.h

LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_arm64 := \
  $(LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_arm)

LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_mips := \
  utils/mips/assembler_mips.h

LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_mips64 := \
  $(LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_mips) \
  utils/mips64/assembler_mips64.h

LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_x86 :=
LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_x86_64 := \
  $(LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_x86)

# $(1): target or host
# $(2): ndebug or debug
# $(3): static or shared (empty means shared, applies only for host)
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
  art_static_or_shared := $(3)

  include $(CLEAR_VARS)
  ifeq ($$(art_target_or_host),host)
    LOCAL_IS_HOST_MODULE := true
    art_codegen_targets := $(ART_HOST_CODEGEN_ARCHS)
  else
    art_codegen_targets := $(ART_TARGET_CODEGEN_ARCHS)
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := libart-compiler
    ifeq ($$(art_static_or_shared), static)
      LOCAL_STATIC_LIBRARIES += libart liblz4 liblzma
    else
      LOCAL_SHARED_LIBRARIES += libart liblz4 liblzma
    endif
    ifeq ($$(art_target_or_host),target)
      LOCAL_FDO_SUPPORT := true
    endif
  else # debug
    LOCAL_MODULE := libartd-compiler
    ifeq ($$(art_static_or_shared), static)
      LOCAL_STATIC_LIBRARIES += libartd liblz4 liblzma
    else
      LOCAL_SHARED_LIBRARIES += libartd liblz4 liblzma
    endif
  endif

  LOCAL_MODULE_TAGS := optional
  ifeq ($$(art_static_or_shared), static)
    LOCAL_MODULE_CLASS := STATIC_LIBRARIES
  else
    LOCAL_MODULE_CLASS := SHARED_LIBRARIES
  endif

  # Sort removes duplicates.
  LOCAL_SRC_FILES := $$(LIBART_COMPILER_SRC_FILES) \
    $$(sort $$(foreach arch,$$(art_codegen_targets), $$(LIBART_COMPILER_SRC_FILES_$$(arch))))

  GENERATED_SRC_DIR := $$(call local-generated-sources-dir)
  ENUM_OPERATOR_OUT_CC_FILES := $$(patsubst %.h,%_operator_out.cc,\
                                $$(LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES) \
                                $$(sort $$(foreach arch,$$(art_codegen_targets), $$(LIBART_COMPILER_ENUM_OPERATOR_OUT_HEADER_FILES_$$(arch)))))
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
    LOCAL_ASFLAGS += $(ART_HOST_ASFLAGS)
    LOCAL_LDLIBS := $(ART_HOST_LDLIBS)
    ifeq ($$(art_static_or_shared),static)
      LOCAL_LDFLAGS += -static
    endif
    ifeq ($$(art_ndebug_or_debug),debug)
      LOCAL_CFLAGS += $(ART_HOST_DEBUG_CFLAGS)
    else
      LOCAL_CFLAGS += $(ART_HOST_NON_DEBUG_CFLAGS)
    endif
  endif

  LOCAL_C_INCLUDES += $(ART_C_INCLUDES) art/runtime art/disassembler

  ifeq ($$(art_target_or_host),host)
    # For compiler driver TLS.
    LOCAL_LDLIBS += -lpthread
  endif
  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/Android.mk
  # Vixl assembly support for ARM64 targets.
  ifeq ($$(art_ndebug_or_debug),debug)
    ifeq ($$(art_static_or_shared), static)
      LOCAL_WHOLESTATIC_LIBRARIES += libvixl
    else
      LOCAL_SHARED_LIBRARIES += libvixl
    endif
  else
    ifeq ($$(art_static_or_shared), static)
      LOCAL_WHOLE_STATIC_LIBRARIES += libvixl
    else
      LOCAL_SHARED_LIBRARIES += libvixl
    endif
  endif

  LOCAL_NATIVE_COVERAGE := $(ART_COVERAGE)

  ifeq ($$(art_target_or_host),target)
    # For atrace.
    LOCAL_SHARED_LIBRARIES += libcutils
    include $(BUILD_SHARED_LIBRARY)
  else # host
    LOCAL_MULTILIB := both
    ifeq ($$(art_static_or_shared), static)
      include $(BUILD_HOST_STATIC_LIBRARY)
    else
      include $(BUILD_HOST_SHARED_LIBRARY)
    endif
  endif

  # Clear locally defined variables.
  art_target_or_host :=
  art_ndebug_or_debug :=
  art_static_or_shared :=
  art_codegen_targets :=
endef

# We always build dex2oat and dependencies, even if the host build is otherwise disabled, since they are used to cross compile for the target.
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-libart-compiler,host,ndebug))
  ifeq ($(ART_BUILD_HOST_STATIC),true)
    $(eval $(call build-libart-compiler,host,ndebug,static))
  endif
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-libart-compiler,host,debug))
  ifeq ($(ART_BUILD_HOST_STATIC),true)
    $(eval $(call build-libart-compiler,host,debug,static))
  endif
endif
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  $(eval $(call build-libart-compiler,target,ndebug))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-libart-compiler,target,debug))
endif
