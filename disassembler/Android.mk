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

include art/build/Android.common.mk

LIBART_DISASSEMBLER_SRC_FILES := \
	disassembler.cc \
	disassembler_arm.cc \
	disassembler_arm64.cc \
	disassembler_mips.cc \
	disassembler_x86.cc

# $(1): target or host
# $(2): ndebug or debug
define build-libart-disassembler
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
  include art/build/Android.libcxx.mk
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := libart-disassembler
  else # debug
    LOCAL_MODULE := libartd-disassembler
  endif

  LOCAL_MODULE_TAGS := optional
  LOCAL_MODULE_CLASS := SHARED_LIBRARIES

  LOCAL_SRC_FILES := $$(LIBART_DISASSEMBLER_SRC_FILES)

  ifeq ($$(art_target_or_host),target)
    LOCAL_CLANG := $(ART_TARGET_CLANG)
    LOCAL_CFLAGS += $(ART_TARGET_CFLAGS)
  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS)
  endif

  LOCAL_SHARED_LIBRARIES += liblog
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

  LOCAL_C_INCLUDES += $(ART_C_INCLUDES) art/runtime

  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/Android.mk
  ifeq ($$(art_target_or_host),target)
    LOCAL_SHARED_LIBRARIES += libcutils libvixl
    include $(BUILD_SHARED_LIBRARY)
  else # host
    LOCAL_STATIC_LIBRARIES += libcutils libvixl
    include $(BUILD_HOST_SHARED_LIBRARY)
  endif
endef

ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  $(eval $(call build-libart-disassembler,target,ndebug))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-libart-disassembler,target,debug))
endif
ifeq ($(WITH_HOST_DALVIK),true)
  # We always build dex2oat and dependencies, even if the host build is otherwise disabled, since they are used to cross compile for the target.
  ifeq ($(ART_BUILD_NDEBUG),true)
    $(eval $(call build-libart-disassembler,host,ndebug))
  endif
  ifeq ($(ART_BUILD_DEBUG),true)
    $(eval $(call build-libart-disassembler,host,debug))
  endif
endif
