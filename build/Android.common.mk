#
# Copyright (C) 2011 The Android Open Source Project
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

ifndef ANDROID_COMMON_MK
ANDROID_COMMON_MK = true

ART_SUPPORTED_ARCH := arm arm64 mips x86 x86_64

ifeq (,$(filter $(TARGET_ARCH),$(ART_SUPPORTED_ARCH)))
$(warning unsupported TARGET_ARCH=$(TARGET_ARCH))
endif

# These can be overridden via the environment or by editing to
# enable/disable certain build configuration.
#
# For example, to disable everything but the host debug build you use:
#
# (export ART_BUILD_TARGET_NDEBUG=false && export ART_BUILD_TARGET_DEBUG=false && export ART_BUILD_HOST_NDEBUG=false && ...)
#
# Beware that tests may use the non-debug build for performance, notable 055-enum-performance
#
ART_BUILD_TARGET_NDEBUG ?= true
ART_BUILD_TARGET_DEBUG ?= true
ART_BUILD_HOST_NDEBUG ?= $(WITH_HOST_DALVIK)
ART_BUILD_HOST_DEBUG ?= $(WITH_HOST_DALVIK)

ART_HOST_ARCH := $(HOST_ARCH)

ifeq ($(ART_BUILD_TARGET_NDEBUG),false)
$(info Disabling ART_BUILD_TARGET_NDEBUG)
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),false)
$(info Disabling ART_BUILD_TARGET_DEBUG)
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),false)
$(info Disabling ART_BUILD_HOST_NDEBUG)
endif
ifeq ($(ART_BUILD_HOST_DEBUG),false)
$(info Disabling ART_BUILD_HOST_DEBUG)
endif

#
# Used to enable smart mode
#
ART_SMALL_MODE := false
ifneq ($(wildcard art/SMALL_ART),)
$(info Enabling ART_SMALL_MODE because of existence of art/SMALL_ART)
ART_SMALL_MODE := true
endif
ifeq ($(WITH_ART_SMALL_MODE), true)
ART_SMALL_MODE := true
endif

#
# Used to enable SEA mode
#
ART_SEA_IR_MODE := false
ifneq ($(wildcard art/SEA_IR_ART),)
$(info Enabling ART_SEA_IR_MODE because of existence of art/SEA_IR_ART)
ART_SEA_IR_MODE := true
endif
ifeq ($(WITH_ART_SEA_IR_MODE), true)
ART_SEA_IR_MODE := true
endif

#
# Used to enable portable mode
#
ART_USE_PORTABLE_COMPILER := false
ifneq ($(wildcard art/USE_PORTABLE_COMPILER),)
$(info Enabling ART_USE_PORTABLE_COMPILER because of existence of art/USE_PORTABLE_COMPILER)
ART_USE_PORTABLE_COMPILER := true
endif
ifeq ($(WITH_ART_USE_PORTABLE_COMPILER),true)
$(info Enabling ART_USE_PORTABLE_COMPILER because WITH_ART_USE_PORTABLE_COMPILER=true)
ART_USE_PORTABLE_COMPILER := true
endif

#
# Used to enable optimizing compiler
#
ART_USE_OPTIMIZING_COMPILER := false
ifneq ($(wildcard art/USE_OPTIMIZING_COMPILER),)
$(info Enabling ART_USE_OPTIMIZING_COMPILER because of existence of art/USE_OPTIMIZING_COMPILER)
ART_USE_OPTIMIZING_COMPILER := true
endif
ifeq ($(WITH_ART_USE_OPTIMIZING_COMPILER), true)
ART_USE_OPTIMIZING_COMPILER := true
endif

ifeq ($(ART_USE_OPTIMIZING_COMPILER),true)
DEX2OAT_FLAGS := --compiler-backend=Optimizing
DALVIKVM_FLAGS := -Xcompiler-option --compiler-backend=Optimizing
endif

#
# Used to change the default GC. Valid values are CMS, SS, GSS. The default is CMS.
#
ART_DEFAULT_GC_TYPE ?= CMS
ART_DEFAULT_GC_TYPE_CFLAGS := -DART_DEFAULT_GC_TYPE_IS_$(ART_DEFAULT_GC_TYPE)

ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  LLVM_ROOT_PATH := external/llvm
  # Don't fail a dalvik minimal host build.
  -include $(LLVM_ROOT_PATH)/llvm.mk
endif

# Clang build support.
# Target builds use GCC by default.
ART_TARGET_CLANG := false
ART_HOST_CLANG := false
ifneq ($(WITHOUT_HOST_CLANG),true)
  # By default, host builds use clang for better warnings.
  ART_HOST_CLANG := true
endif

# enable ART_TARGET_CLANG for ARM64
ifneq (,$(filter $(TARGET_ARCH),arm64))
ART_TARGET_CLANG := true
endif

# directory used for dalvik-cache on device
ART_DALVIK_CACHE_DIR := /data/dalvik-cache

# directory used for gtests on device
ART_BASE_NATIVETEST_DIR := /data/nativetest/art
ART_BASE_NATIVETEST_OUT := $(TARGET_OUT_DATA_NATIVE_TESTS)/art

# directory used for tests on device
ART_BASE_TEST_DIR := /data/art-test
ART_BASE_TEST_OUT := $(TARGET_OUT_DATA)/art-test

# Primary vs. secondary
2ND_TARGET_ARCH := $(TARGET_2ND_ARCH)
ART_PHONY_TEST_TARGET_SUFFIX :=
2ND_ART_PHONY_TEST_TARGET_SUFFIX :=
ART_TARGET_BINARY_SUFFIX :=
2ND_ART_TARGET_BINARY_SUFFIX :=
ifdef TARGET_2ND_ARCH
  art_test_primary_suffix :=
  art_test_secondary_suffix :=
  ifneq ($(filter %64,$(TARGET_ARCH)),)
    art_test_primary_suffix := 64
    ART_PHONY_TEST_TARGET_SUFFIX := 64
    2ND_ART_PHONY_TEST_TARGET_SUFFIX := 32
    ART_TARGET_BINARY_SUFFIX := 64
  else
    # TODO: ???
    $(error Do not know what to do with this multi-target configuration!)
  endif
  # Primary with primary suffix
  ART_NATIVETEST_DIR := $(ART_BASE_NATIVETEST_DIR)$(art_test_primary_suffix)
  ART_NATIVETEST_OUT := $(ART_BASE_NATIVETEST_OUT)$(art_test_primary_suffix)
  ART_TEST_DIR := $(ART_BASE_TEST_DIR)$(art_test_primary_suffix)
  ART_TEST_OUT := $(ART_BASE_TEST_OUT)$(art_test_primary_suffix)
  # Secondary with 2ND_ prefix and secondary suffix
  2ND_ART_NATIVETEST_DIR := $(ART_BASE_NATIVETEST_DIR)$(art_test_secondary_suffix)
  2ND_ART_NATIVETEST_OUT := $(ART_BASE_NATIVETEST_OUT)$(art_test_secondary_suffix)
  2ND_ART_TEST_DIR := $(ART_BASE_TEST_DIR)$(art_test_secondary_suffix)
  2ND_ART_TEST_OUT := $(ART_BASE_TEST_OUT)$(art_test_secondary_suffix)
else
  ART_NATIVETEST_DIR := $(ART_BASE_NATIVETEST_DIR)
  ART_NATIVETEST_OUT := $(ART_BASE_NATIVETEST_OUT)
  ART_TEST_DIR := $(ART_BASE_TEST_DIR)
  ART_TEST_OUT := $(ART_BASE_TEST_OUT)
  # No secondary
endif

ART_CPP_EXTENSION := .cc

ART_HOST_SHLIB_EXTENSION := $(HOST_SHLIB_SUFFIX)
ART_HOST_SHLIB_EXTENSION ?= .so

ART_C_INCLUDES := \
	external/gtest/include \
	external/valgrind/main/include \
	external/valgrind/main \
	external/vixl/src \
	external/zlib \
	frameworks/compile/mclinker/include

art_cflags := \
	-fno-rtti \
	-std=gnu++11 \
	-ggdb3 \
	-Wall \
	-Werror \
	-Wextra \
	-Wno-sign-promo \
	-Wno-unused-parameter \
	-Wstrict-aliasing \
	-fstrict-aliasing

# these are necessary for Clang ARM64 ART builds
ifeq ($(ART_TARGET_CLANG), true)
art_cflags += \
	-Wno-implicit-exception-spec-mismatch \
	-DNVALGRIND \
	-Wno-unused-value
endif

ifeq ($(ART_SMALL_MODE),true)
  art_cflags += -DART_SMALL_MODE=1
endif

ifeq ($(ART_SEA_IR_MODE),true)
  art_cflags += -DART_SEA_IR_MODE=1
endif

ifeq ($(HOST_OS),linux)
  art_non_debug_cflags := \
	-Wframe-larger-than=1728
endif

art_non_debug_cflags := \
	-O3

# FIXME: upstream LLVM has a vectorizer bug that needs to be fixed
ifeq ($(ART_TARGET_CLANG),true)
art_non_debug_cflags += \
        -fno-vectorize
endif

art_debug_cflags := \
	-O1 \
	-DDYNAMIC_ANNOTATIONS_ENABLED=1 \
	-UNDEBUG

ART_HOST_CFLAGS := $(art_cflags) -DANDROID_SMP=1 -DART_BASE_ADDRESS=$(LIBART_IMG_HOST_BASE_ADDRESS)
ART_HOST_CFLAGS += -DART_DEFAULT_INSTRUCTION_SET_FEATURES=default
ART_HOST_CFLAGS += $(ART_DEFAULT_GC_TYPE_CFLAGS)

ART_TARGET_CFLAGS := $(art_cflags) -DART_TARGET -DART_BASE_ADDRESS=$(LIBART_IMG_TARGET_BASE_ADDRESS)
ifeq ($(TARGET_CPU_SMP),true)
  ART_TARGET_CFLAGS += -DANDROID_SMP=1
else
  ifeq ($(TARGET_CPU_SMP),false)
    ART_TARGET_CFLAGS += -DANDROID_SMP=0
  else
    $(warning TARGET_CPU_SMP should be (true|false), found $(TARGET_CPU_SMP))
    # Make sure we emit barriers for the worst case.
    ART_TARGET_CFLAGS += -DANDROID_SMP=1
  endif
endif
ART_TARGET_CFLAGS += $(ART_DEFAULT_GC_TYPE_CFLAGS)

# TODO: remove when target no longer implies stlport.
ART_TARGET_CFLAGS += -DART_WITH_STLPORT=1

# DEX2OAT_TARGET_INSTRUCTION_SET_FEATURES is set in ../build/core/dex_preopt.mk based on
# the TARGET_CPU_VARIANT
ifeq ($(DEX2OAT_TARGET_INSTRUCTION_SET_FEATURES),)
$(error Required DEX2OAT_TARGET_INSTRUCTION_SET_FEATURES is not set)
endif
ART_TARGET_CFLAGS += -DART_DEFAULT_INSTRUCTION_SET_FEATURES=$(DEX2OAT_TARGET_INSTRUCTION_SET_FEATURES)

# Enable thread-safety for GCC 4.6, and clang, but not for GCC 4.7 or later where this feature was
# removed. Warn when -Wthread-safety is not used.
ifneq ($(filter 4.6 4.6.%, $(TARGET_GCC_VERSION)),)
  ART_TARGET_CFLAGS += -Wthread-safety
else
  # FIXME: add -Wthread-safety when the problem is fixed
  ifeq ($(ART_TARGET_CLANG),true)
    ART_TARGET_CFLAGS +=
  else
    # Warn if -Wthread-safety is not suport and not doing a top-level or 'mma' build.
    ifneq ($(ONE_SHOT_MAKEFILE),)
      # Enable target GCC 4.6 with: export TARGET_GCC_VERSION_EXP=4.6
      $(info Using target GCC $(TARGET_GCC_VERSION) disables thread-safety checks.)
    endif
  endif
endif
# We compile with GCC 4.6 or clang on the host, both of which support -Wthread-safety.
ART_HOST_CFLAGS += -Wthread-safety

# To use oprofile_android --callgraph, uncomment this and recompile with "mmm art -B -j16"
# ART_TARGET_CFLAGS += -fno-omit-frame-pointer -marm -mapcs

# Addition CPU specific CFLAGS.
ifeq ($(TARGET_ARCH),arm)
  ifneq ($(filter cortex-a15, $(TARGET_CPU_VARIANT)),)
    # Fake a ARM feature for LPAE support.
    ART_TARGET_CFLAGS += -D__ARM_FEATURE_LPAE=1
  endif
endif

ART_HOST_NON_DEBUG_CFLAGS := $(art_non_debug_cflags)
ART_TARGET_NON_DEBUG_CFLAGS := $(art_non_debug_cflags)

# TODO: move -fkeep-inline-functions to art_debug_cflags when target gcc > 4.4 (and -lsupc++)
ART_HOST_DEBUG_CFLAGS := $(art_debug_cflags) -fkeep-inline-functions
ART_HOST_DEBUG_LDLIBS := -lsupc++

ifneq ($(HOST_OS),linux)
  # Some Mac OS pthread header files are broken with -fkeep-inline-functions.
  ART_HOST_DEBUG_CFLAGS := $(filter-out -fkeep-inline-functions,$(ART_HOST_DEBUG_CFLAGS))
  # Mac OS doesn't have libsupc++.
  ART_HOST_DEBUG_LDLIBS := $(filter-out -lsupc++,$(ART_HOST_DEBUG_LDLIBS))
endif

ART_TARGET_DEBUG_CFLAGS := $(art_debug_cflags)

ART_BUILD_TARGET := false
ART_BUILD_HOST := false
ART_BUILD_NDEBUG := false
ART_BUILD_DEBUG := false
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  ART_BUILD_TARGET := true
  ART_BUILD_NDEBUG := true
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  ART_BUILD_TARGET := true
  ART_BUILD_DEBUG := true
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  ART_BUILD_HOST := true
  ART_BUILD_NDEBUG := true
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  ART_BUILD_HOST := true
  ART_BUILD_DEBUG := true
endif

# Helper function to call a function twice with a target suffix
# $(1): The generator function for the rules
#         Has one argument, the suffix
define call-art-multi-target
  $(call $(1),$(ART_PHONY_TEST_TARGET_SUFFIX))

  ifdef TARGET_2ND_ARCH
    $(call $(1),$(2ND_ART_PHONY_TEST_TARGET_SUFFIX))
  endif
endef

# Helper function to combine two variables with suffixes together.
# $(1): The base name.
define combine-art-multi-target-var
  ifdef TARGET_2ND_ARCH
    ifneq ($(ART_PHONY_TEST_TARGET_SUFFIX),)
      ifneq ($(2ND_ART_PHONY_TEST_TARGET_SUFFIX),)
$(1) := $($(1)$(ART_PHONY_TEST_TARGET_SUFFIX)) $($(1)$(2ND_ART_PHONY_TEST_TARGET_SUFFIX))
      endif
    endif
  endif
endef


# Helper function to define a variable twice with a target suffix. Assume the name generated is
# derived from $(2) so we can create a combined var.
# $(1): The generator function for the rules
#         Has one argument, the suffix
define call-art-multi-target-var
  $(call $(1),$(ART_PHONY_TEST_TARGET_SUFFIX))

  ifdef TARGET_2ND_ARCH
    $(call $(1),$(2ND_ART_PHONY_TEST_TARGET_SUFFIX))

    # Link both together, if it makes sense
    ifneq ($(ART_PHONY_TEST_TARGET_SUFFIX),)
      ifneq ($(2ND_ART_PHONY_TEST_TARGET_SUFFIX),)
$(2) := $(2)$(ART_PHONY_TEST_TARGET_SUFFIX) $(2)$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
      endif
    endif

  endif
endef

# Helper function to call a function twice with a target suffix. Assume it generates make rules
# with the given name, and link them.
# $(1): The generator function for the rules
#         Has one argument, the suffix
# $(2): The base rule name, necessary for the link
#       We assume we can link the names together easily...
define call-art-multi-target-rule
  $(call $(1),$(ART_PHONY_TEST_TARGET_SUFFIX))

  ifdef TARGET_2ND_ARCH
    $(call $(1),$(2ND_ART_PHONY_TEST_TARGET_SUFFIX))

    # Link both together, if it makes sense
    ifneq ($(ART_PHONY_TEST_TARGET_SUFFIX),)
      ifneq ($(2ND_ART_PHONY_TEST_TARGET_SUFFIX),)
.PHONY: $(2)
$(2): $(2)$(ART_PHONY_TEST_TARGET_SUFFIX) $(2)$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
      endif
    endif
  endif
endef


endif # ANDROID_COMMON_MK
