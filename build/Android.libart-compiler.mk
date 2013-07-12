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

LIBART_COMPILER_SRC_FILES := \
	src/compiler/dex/local_value_numbering.cc \
	src/compiler/dex/arena_allocator.cc \
	src/compiler/dex/arena_bit_vector.cc \
	src/compiler/dex/quick/arm/assemble_arm.cc \
	src/compiler/dex/quick/arm/call_arm.cc \
	src/compiler/dex/quick/arm/fp_arm.cc \
	src/compiler/dex/quick/arm/int_arm.cc \
	src/compiler/dex/quick/arm/target_arm.cc \
	src/compiler/dex/quick/arm/utility_arm.cc \
	src/compiler/dex/quick/codegen_util.cc \
	src/compiler/dex/quick/gen_common.cc \
	src/compiler/dex/quick/gen_invoke.cc \
	src/compiler/dex/quick/gen_loadstore.cc \
	src/compiler/dex/quick/local_optimizations.cc \
	src/compiler/dex/quick/mips/assemble_mips.cc \
	src/compiler/dex/quick/mips/call_mips.cc \
	src/compiler/dex/quick/mips/fp_mips.cc \
	src/compiler/dex/quick/mips/int_mips.cc \
	src/compiler/dex/quick/mips/target_mips.cc \
	src/compiler/dex/quick/mips/utility_mips.cc \
	src/compiler/dex/quick/mir_to_lir.cc \
	src/compiler/dex/quick/ralloc_util.cc \
	src/compiler/dex/quick/x86/assemble_x86.cc \
	src/compiler/dex/quick/x86/call_x86.cc \
	src/compiler/dex/quick/x86/fp_x86.cc \
	src/compiler/dex/quick/x86/int_x86.cc \
	src/compiler/dex/quick/x86/target_x86.cc \
	src/compiler/dex/quick/x86/utility_x86.cc \
	src/compiler/dex/portable/mir_to_gbc.cc \
	src/compiler/dex/dex_to_dex_compiler.cc \
	src/compiler/dex/mir_dataflow.cc \
	src/compiler/dex/mir_optimization.cc \
	src/compiler/dex/frontend.cc \
	src/compiler/dex/mir_graph.cc \
	src/compiler/dex/vreg_analysis.cc \
	src/compiler/dex/ssa_transformation.cc \
	src/compiler/driver/compiler_driver.cc \
	src/compiler/driver/dex_compilation_unit.cc \
	src/compiler/jni/portable/jni_compiler.cc \
	src/compiler/jni/quick/arm/calling_convention_arm.cc \
	src/compiler/jni/quick/mips/calling_convention_mips.cc \
	src/compiler/jni/quick/x86/calling_convention_x86.cc \
	src/compiler/jni/quick/calling_convention.cc \
	src/compiler/jni/quick/jni_compiler.cc \
	src/compiler/llvm/compiler_llvm.cc \
	src/compiler/llvm/gbc_expander.cc \
	src/compiler/llvm/generated/art_module.cc \
	src/compiler/llvm/intrinsic_helper.cc \
	src/compiler/llvm/ir_builder.cc \
	src/compiler/llvm/llvm_compilation_unit.cc \
	src/compiler/llvm/md_builder.cc \
	src/compiler/llvm/runtime_support_builder.cc \
	src/compiler/llvm/runtime_support_builder_arm.cc \
	src/compiler/llvm/runtime_support_builder_thumb2.cc \
	src/compiler/llvm/runtime_support_builder_x86.cc \
        src/compiler/stubs/portable/stubs.cc \
        src/compiler/stubs/quick/stubs.cc \
	src/compiler/elf_fixup.cc \
	src/compiler/elf_stripper.cc \
	src/compiler/elf_writer.cc \
	src/compiler/elf_writer_quick.cc \
	src/compiler/image_writer.cc \
	src/compiler/oat_writer.cc

ifeq ($(ART_SEA_IR_MODE),true)
LIBART_COMPILER_SRC_FILES += \
	src/compiler/sea_ir/frontend.cc \
	src/compiler/sea_ir/instruction_tools.cc
endif

LIBART_COMPILER_CFLAGS :=
ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  LIBART_COMPILER_SRC_FILES += src/compiler/elf_writer_mclinker.cc
  LIBART_COMPILER_CFLAGS += -DART_USE_PORTABLE_COMPILER=1
endif

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
  ifeq ($$(art_target_or_host),target)
    include external/stlport/libstlport.mk
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := libart-compiler
  else # debug
    LOCAL_MODULE := libartd-compiler
  endif

  LOCAL_MODULE_TAGS := optional
  LOCAL_MODULE_CLASS := SHARED_LIBRARIES

  LOCAL_SRC_FILES := $$(LIBART_COMPILER_SRC_FILES)

  LOCAL_CFLAGS := $$(LIBART_COMPILER_CFLAGS)
  ifeq ($$(art_target_or_host),target)
    LOCAL_CLANG := $(ART_TARGET_CLANG)
    LOCAL_CFLAGS += $(ART_TARGET_CFLAGS)
  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS)
  endif

  # TODO: clean up the compilers and remove this.
  LOCAL_CFLAGS += -Wno-unused-parameter

  LOCAL_SHARED_LIBRARIES := liblog
  ifeq ($$(art_ndebug_or_debug),debug)
    ifeq ($$(art_target_or_host),target)
      LOCAL_CFLAGS += $(ART_TARGET_DEBUG_CFLAGS)
    else # host
      LOCAL_CFLAGS += $(ART_HOST_DEBUG_CFLAGS)
    endif
    LOCAL_SHARED_LIBRARIES += libartd
  else
    ifeq ($$(art_target_or_host),target)
      LOCAL_CFLAGS += $(ART_TARGET_NON_DEBUG_CFLAGS)
    else # host
      LOCAL_CFLAGS += $(ART_HOST_NON_DEBUG_CFLAGS)
    endif
    LOCAL_SHARED_LIBRARIES += libart
  endif
  LOCAL_SHARED_LIBRARIES += libbcc libLLVM

  ifeq ($(ART_USE_PORTABLE_COMPILER),true)
    LOCAL_CFLAGS += -DART_USE_PORTABLE_COMPILER=1
  endif

  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)

  ifeq ($$(art_target_or_host),target)
    LOCAL_SHARED_LIBRARIES += libstlport
  else # host
    LOCAL_LDLIBS := -ldl -lpthread
  endif
  LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/build/Android.common.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/build/Android.libart-compiler.mk
  ifeq ($$(art_target_or_host),target)
    LOCAL_SHARED_LIBRARIES += libcutils
    include $(LLVM_GEN_INTRINSICS_MK)
    include $(LLVM_DEVICE_BUILD_MK)
    include $(BUILD_SHARED_LIBRARY)
  else # host
    LOCAL_IS_HOST_MODULE := true
    LOCAL_STATIC_LIBRARIES += libcutils
    include $(LLVM_GEN_INTRINSICS_MK)
    include $(LLVM_HOST_BUILD_MK)
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

ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  $(eval $(call build-libart-compiler,target,ndebug))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-libart-compiler,target,debug))
endif
# We always build dex2oat and dependencies, even if the host build is otherwise disabled, since they are used to cross compile for the target.
ifeq ($(ART_BUILD_NDEBUG),true)
  $(eval $(call build-libart-compiler,host,ndebug))
endif
ifeq ($(ART_BUILD_DEBUG),true)
  $(eval $(call build-libart-compiler,host,debug))
endif

# Rule to build /system/lib/libcompiler_rt.a
# Usually static libraries are not installed on the device.
ifeq ($(ART_USE_PORTABLE_COMPILER),true)
ifeq ($(ART_BUILD_TARGET),true)
# TODO: Move to external/compiler_rt
$(eval $(call copy-one-file, $(call intermediates-dir-for,STATIC_LIBRARIES,libcompiler_rt,,)/libcompiler_rt.a, $(TARGET_OUT_SHARED_LIBRARIES)/libcompiler_rt.a))

$(DEX2OAT): $(TARGET_OUT_SHARED_LIBRARIES)/libcompiler_rt.a

endif
endif
