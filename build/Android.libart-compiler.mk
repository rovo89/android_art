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
	src/compiler/dex/bb_opt.cc \
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
	src/compiler/dex/compiler_utility.cc \
	src/compiler/dex/dataflow.cc \
	src/compiler/dex/frontend.cc \
	src/compiler/dex/ralloc.cc \
	src/compiler/dex/ssa_transformation.cc \
	src/compiler/dex/write_elf.cc \
	src/elf_writer.cc \
	src/greenland/intrinsic_helper.cc \
	src/greenland/ir_builder.cc \
	src/oat/jni/arm/calling_convention_arm.cc \
	src/oat/jni/arm/jni_internal_arm.cc \
	src/oat/jni/calling_convention.cc \
	src/oat/jni/jni_compiler.cc \
	src/oat/jni/mips/calling_convention_mips.cc \
	src/oat/jni/mips/jni_internal_mips.cc \
	src/oat/jni/x86/calling_convention_x86.cc \
	src/oat/jni/x86/jni_internal_x86.cc

LIBART_COMPILER_CFLAGS :=
ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  LIBART_COMPILER_CFLAGS += -DART_USE_PORTABLE_COMPILER=1
endif

ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  LIBART_COMPILER_SRC_FILES += \
	src/compiler_llvm/compiler_llvm.cc \
	src/compiler_llvm/gbc_expander.cc \
	src/compiler_llvm/generated/art_module.cc \
	src/compiler_llvm/ir_builder.cc \
	src/compiler_llvm/jni_compiler.cc \
	src/compiler_llvm/llvm_compilation_unit.cc \
	src/compiler_llvm/md_builder.cc \
	src/compiler_llvm/runtime_support_builder.cc \
	src/compiler_llvm/runtime_support_builder_arm.cc \
	src/compiler_llvm/runtime_support_builder_thumb2.cc \
	src/compiler_llvm/runtime_support_builder_x86.cc \
	src/compiler_llvm/runtime_support_llvm.cc \
	src/compiler_llvm/stub_compiler.cc
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
    LOCAL_CFLAGS += $(ART_TARGET_CFLAGS)
  else # host
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
  LOCAL_SHARED_LIBRARIES += libbcc

  ifeq ($(ART_USE_PORTABLE_COMPILER),true)
    LOCAL_CFLAGS += -DART_USE_PORTABLE_COMPILER=1
  endif

  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)

  ifeq ($$(art_target_or_host),target)
    LOCAL_SHARED_LIBRARIES += libstlport
  else # host
    LOCAL_LDLIBS := -ldl -lpthread
  endif
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
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-libart-compiler,host,ndebug))
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-libart-compiler,host,debug))
endif
