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

LOCAL_PATH := $(call my-dir)

include art/build/Android.common_build.mk

LIBARTTEST_COMMON_SRC_FILES := \
  common/runtime_state.cc \
  common/stack_inspect.cc \
  004-JniTest/jni_test.cc \
  004-SignalTest/signaltest.cc \
  004-ReferenceMap/stack_walk_refmap_jni.cc \
  004-StackWalk/stack_walk_jni.cc \
  004-UnsafeTest/unsafe_test.cc \
  044-proxy/native_proxy.cc \
  051-thread/thread_test.cc \
  117-nopatchoat/nopatchoat.cc \
  1337-gc-coverage/gc_coverage.cc \
  136-daemon-jni-shutdown/daemon_jni_shutdown.cc \
  137-cfi/cfi.cc \
  139-register-natives/regnative.cc \
  141-class-unload/jni_unload.cc \
  148-multithread-gc-annotations/gc_coverage.cc \
  454-get-vreg/get_vreg_jni.cc \
  457-regs/regs_jni.cc \
  461-get-reference-vreg/get_reference_vreg_jni.cc \
  466-get-live-vreg/get_live_vreg_jni.cc \
  497-inlining-and-class-loader/clear_dex_cache.cc \
  543-env-long-ref/env_long_ref.cc \
  566-polymorphic-inlining/polymorphic_inline.cc \
  570-checker-osr/osr.cc \
  595-profile-saving/profile-saving.cc \
  596-app-images/app_images.cc \
  597-deopt-new-string/deopt.cc

ART_TARGET_LIBARTTEST_$(ART_PHONY_TEST_TARGET_SUFFIX) += $(ART_TARGET_TEST_OUT)/$(TARGET_ARCH)/libarttest.so
ART_TARGET_LIBARTTEST_$(ART_PHONY_TEST_TARGET_SUFFIX) += $(ART_TARGET_TEST_OUT)/$(TARGET_ARCH)/libarttestd.so
ifdef TARGET_2ND_ARCH
  ART_TARGET_LIBARTTEST_$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) += $(ART_TARGET_TEST_OUT)/$(TARGET_2ND_ARCH)/libarttest.so
  ART_TARGET_LIBARTTEST_$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) += $(ART_TARGET_TEST_OUT)/$(TARGET_2ND_ARCH)/libarttestd.so
endif

# $(1): target or host
define build-libarttest
  ifneq ($(1),target)
    ifneq ($(1),host)
      $$(error expected target or host for argument 1, received $(1))
    endif
  endif
  ifneq ($(2),debug)
    ifneq ($(2),)
      $$(error d or empty for argument 2, received $(2))
    endif
    suffix := d
  else
    suffix :=
  endif

  art_target_or_host := $(1)

  include $(CLEAR_VARS)
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  LOCAL_MODULE := libarttest$$(suffix)
  ifeq ($$(art_target_or_host),target)
    LOCAL_MODULE_TAGS := tests
  endif
  LOCAL_SRC_FILES := $(LIBARTTEST_COMMON_SRC_FILES)
  LOCAL_SHARED_LIBRARIES += libart$$(suffix) libbacktrace libnativehelper
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES) art/runtime
  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/Android.libarttest.mk
  ifeq ($$(art_target_or_host),target)
    $(call set-target-local-clang-vars)
    $(call set-target-local-cflags-vars,debug)
    LOCAL_SHARED_LIBRARIES += libdl
    LOCAL_MULTILIB := both
    LOCAL_MODULE_PATH_32 := $(ART_TARGET_TEST_OUT)/$(ART_TARGET_ARCH_32)
    LOCAL_MODULE_PATH_64 := $(ART_TARGET_TEST_OUT)/$(ART_TARGET_ARCH_64)
    LOCAL_MODULE_TARGET_ARCH := $(ART_SUPPORTED_ARCH)
    include $(BUILD_SHARED_LIBRARY)
  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_CFLAGS := $(ART_HOST_CFLAGS)
    ifeq ($$(suffix),d)
      LOCAL_CFLAGS += $(ART_HOST_DEBUG_CFLAGS)
    else
      LOCAL_CFLAGS += $(ART_HOST_NON_DEBUG_CFLAGS)
    endif
    LOCAL_ASFLAGS := $(ART_HOST_ASFLAGS)
    LOCAL_LDLIBS := $(ART_HOST_LDLIBS) -ldl -lpthread
    LOCAL_IS_HOST_MODULE := true
    LOCAL_MULTILIB := both
    include $(BUILD_HOST_SHARED_LIBRARY)
  endif

  # Clear locally used variables.
  art_target_or_host :=
  suffix :=
endef

ifeq ($(ART_BUILD_TARGET),true)
  $(eval $(call build-libarttest,target,))
  $(eval $(call build-libarttest,target,debug))
endif
ifeq ($(ART_BUILD_HOST),true)
  $(eval $(call build-libarttest,host,))
  $(eval $(call build-libarttest,host,debug))
endif

# Clear locally used variables.
LOCAL_PATH :=
LIBARTTEST_COMMON_SRC_FILES :=
