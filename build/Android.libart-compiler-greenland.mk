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


LIBART_COMPILER_GREENLAND_CFLAGS := -DART_USE_GREENLAND_COMPILER=1

LIBART_COMPILER_GREENLAND_SRC_FILES += \
	src/greenland/dalvik_reg.cc \
	src/greenland/dex_lang.cc \
	src/greenland/greenland.cc \
	src/greenland/lir.cc \
	src/greenland/lir_function.cc \
	src/greenland/inferred_reg_category_map.cc \
	src/greenland/intrinsic_helper.cc \
	src/greenland/ir_builder.cc \
	src/greenland/target_codegen_machine.cc \
	src/greenland/target_lir_emitter.cc \
	src/greenland/target_registry.cc \
	src/oat/jni/calling_convention.cc \
	src/oat/jni/jni_compiler.cc \
	src/oat/jni/arm/calling_convention_arm.cc \
	src/oat/jni/x86/calling_convention_x86.cc

LIBART_COMPILER_GREENLAND_ARM_SRC_FILES += \
  src/greenland/arm/arm_codegen_machine.cc \
	src/greenland/arm/arm_invoke_stub_compiler.cc

LIBART_COMPILER_GREENLAND_MIPS_SRC_FILES += \
  src/greenland/mips/mips_codegen_machine.cc \
  src/greenland/mips/mips_invoke_stub_compiler.cc

LIBART_COMPILER_GREENLAND_X86_SRC_FILES += \
  src/greenland/x86/x86_codegen_machine.cc \
  src/greenland/x86/x86_lir_emitter.cc \
  src/greenland/x86/x86_lir_info.cc \
  src/greenland/x86/x86_invoke_stub_compiler.cc

# $(1): target or host
# $(2): ndebug or debug
define build-libart-compiler-greenland
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
    LOCAL_MODULE := libart-compiler-greenland
  else # debug
    LOCAL_MODULE := libartd-compiler-greenland
  endif

  LOCAL_MODULE_TAGS := optional
  LOCAL_MODULE_CLASS := SHARED_LIBRARIES

  LOCAL_SRC_FILES := $(LIBART_COMPILER_GREENLAND_SRC_FILES)
  LOCAL_CFLAGS := $(LIBART_COMPILER_GREENLAND_CFLAGS)
  ifeq ($$(art_target_or_host),target)
    LOCAL_CFLAGS += $(ART_TARGET_CFLAGS)
  else # host
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS)
  endif

  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)

  ifeq ($$(art_target_or_host),target)
    LOCAL_SRC_FILES += \
      $(LIBART_COMPILER_GREENLAND_ARM_SRC_FILES)
  else
    LOCAL_SRC_FILES += \
      $(LIBART_COMPILER_GREENLAND_ARM_SRC_FILES) \
      $(LIBART_COMPILER_GREENLAND_MIPS_SRC_FILES) \
      $(LIBART_COMPILER_GREENLAND_X86_SRC_FILES)
  endif

  LOCAL_STATIC_LIBRARIES += \
    libLLVMBitWriter \
    libLLVMBitReader \
    libLLVMScalarOpts \
    libLLVMInstCombine \
    libLLVMTransformUtils \
    libLLVMAnalysis \
    libLLVMTarget \
    libLLVMCore \
    libLLVMSupport
  LOCAL_SHARED_LIBRARIES := liblog libnativehelper
  ifeq ($$(art_target_or_host),target)
    LOCAL_SHARED_LIBRARIES += libcutils libstlport libz libdl
    LOCAL_SHARED_LIBRARIES += libdynamic_annotations # tsan support
#    LOCAL_SHARED_LIBRARIES += libcorkscrew # native stack trace support
  else # host
    LOCAL_STATIC_LIBRARIES += libcutils
    LOCAL_SHARED_LIBRARIES += libz-host
    LOCAL_SHARED_LIBRARIES += libdynamic_annotations-host # tsan support
    LOCAL_LDLIBS := -ldl -lpthread
    ifeq ($(HOST_OS),linux)
      LOCAL_LDLIBS += -lrt
    endif
  endif
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
  ifeq ($$(art_target_or_host),target)
    include $(LLVM_GEN_INTRINSICS_MK)
    include $(LLVM_DEVICE_BUILD_MK)
    include $(BUILD_SHARED_LIBRARY)
  else # host
    LOCAL_IS_HOST_MODULE := true
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
  $(eval $(call build-libart-compiler-greenland,target,ndebug))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-libart-compiler-greenland,target,debug))
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-libart-compiler-greenland,host,ndebug))
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-libart-compiler-greenland,host,debug))
endif
