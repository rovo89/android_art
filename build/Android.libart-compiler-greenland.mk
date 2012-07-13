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
	src/greenland/inferred_reg_category_map.cc \
	src/greenland/dalvik_reg.cc \
	src/greenland/dex_lang.cc \
	src/greenland/gbc_context.cc \
	src/greenland/greenland.cc \
	src/greenland/intrinsic_helper.cc \
	src/greenland/register_allocator.cc \
	src/greenland/target_codegen_machine.cc \
	src/greenland/target_registry.cc \
	src/oat/jni/calling_convention.cc \
	src/oat/jni/jni_compiler.cc \
	src/oat/jni/arm/calling_convention_arm.cc \
	src/oat/jni/mips/calling_convention_mips.cc \
	src/oat/jni/x86/calling_convention_x86.cc

LIBART_COMPILER_GREENLAND_arm_SRC_FILES += \
  src/greenland/arm/arm_codegen_machine.cc \
	src/greenland/arm/arm_invoke_stub_compiler.cc

LIBART_COMPILER_GREENLAND_mips_SRC_FILES += \
  src/greenland/mips/mips_codegen_machine.cc \
  src/greenland/mips/mips_invoke_stub_compiler.cc

LIBART_COMPILER_GREENLAND_x86_SRC_FILES += \
  src/greenland/x86/x86_codegen_machine.cc \
  src/greenland/x86/x86_lir_emitter.cc \
  src/greenland/x86/x86_lir_info.cc \
  src/greenland/x86/x86_invoke_stub_compiler.cc

########################################################################

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
LOCAL_MODULE := target_lir_builder_generator
LOCAL_MODULE_TAGS := optional
LOCAL_IS_HOST_MODULE := true
LOCAL_SRC_FILES := src/greenland/tools/target_lir_builder_generator.cc
LOCAL_CFLAGS := $(ART_HOST_CFLAGS) $(ART_HOST_DEBUG_CFLAGS)
LOCAL_C_INCLUDES := $(ART_C_INCLUDES)
include $(BUILD_HOST_EXECUTABLE)
TARGET_LIR_BUILDER_GENERATOR := $(LOCAL_BUILT_MODULE)

########################################################################

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
    LOCAL_IS_HOST_MODULE := true
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS)
  endif

  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)

  ifeq ($$(art_target_or_host),target)
    ENUM_INCLUDE_LIR_TARGETS := arm
    LOCAL_SRC_FILES += \
      $(LIBART_COMPILER_GREENLAND_$(TARGET_ARCH)_SRC_FILES)
  else
    ENUM_INCLUDE_LIR_TARGETS := arm mips x86
    LOCAL_SRC_FILES += \
      $(LIBART_COMPILER_GREENLAND_arm_SRC_FILES) \
      $(LIBART_COMPILER_GREENLAND_mips_SRC_FILES) \
      $(LIBART_COMPILER_GREENLAND_x86_SRC_FILES)
  endif

  GENERATED_SRC_DIR := $$(call intermediates-dir-for,$$(LOCAL_MODULE_CLASS),$$(LOCAL_MODULE),$$(LOCAL_IS_HOST_MODULE),)
  ENUM_TARGEET_LIR_BUILDER_INC_FILES := $$(foreach lir_target, $$(ENUM_INCLUDE_LIR_TARGETS), $$(lir_target)_lir_builder_base.inc)
  ENUM_TARGET_LIR_BUILDER_OUT_GEN := $$(addprefix $$(GENERATED_SRC_DIR)/, $$(ENUM_TARGEET_LIR_BUILDER_INC_FILES))

$$(ENUM_TARGET_LIR_BUILDER_OUT_GEN): PRIVATE_LIR_TARGET = $$(subst _lir_builder_base.inc,,$$(notdir $$@))
$$(ENUM_TARGET_LIR_BUILDER_OUT_GEN): %.inc : $$(TARGET_LIR_BUILDER_GENERATOR)
	@echo "target Generated: $$@"
	$$(hide) $$(TARGET_LIR_BUILDER_GENERATOR) $$(PRIVATE_LIR_TARGET) > $$@

LOCAL_GENERATED_SOURCES += $$(ENUM_TARGET_LIR_BUILDER_OUT_GEN)

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
    LOCAL_SHARED_LIBRARIES += libcorkscrew # native stack trace support
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
