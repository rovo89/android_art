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

LIBARTTEST_COMMON_SRC_FILES := \
	test/JniTest/jni_test.cc \
	test/SignalTest/signaltest.cc \
	test/ReferenceMap/stack_walk_refmap_jni.cc \
	test/StackWalk/stack_walk_jni.cc \
	test/UnsafeTest/unsafe_test.cc

ART_TARGET_LIBARTTEST_$(ART_PHONY_TEST_TARGET_SUFFIX) += $(ART_TEST_OUT)/$(TARGET_ARCH)/libarttest.so
ifdef TARGET_2ND_ARCH
  ART_TARGET_LIBARTTEST_$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) += $(ART_TEST_OUT)/$(TARGET_2ND_ARCH)/libarttest.so
endif

# $(1): target or host
define build-libarttest
  ifneq ($(1),target)
    ifneq ($(1),host)
      $$(error expected target or host for argument 1, received $(1))
    endif
  endif

  art_target_or_host := $(1)

  include $(CLEAR_VARS)
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  LOCAL_MODULE := libarttest
  ifeq ($$(art_target_or_host),target)
    LOCAL_MODULE_TAGS := tests
  endif
  LOCAL_SRC_FILES := $(LIBARTTEST_COMMON_SRC_FILES)
  LOCAL_SHARED_LIBRARIES += libartd
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES) art/runtime
  LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/build/Android.common.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/build/Android.libarttest.mk
  include external/libcxx/libcxx.mk
  ifeq ($$(art_target_or_host),target)
  	$(call set-target-local-clang-vars)
  	$(call set-target-local-cflags-vars,debug)
    LOCAL_SHARED_LIBRARIES += libdl libcutils
    LOCAL_STATIC_LIBRARIES := libgtest
    LOCAL_MULTILIB := both
    LOCAL_MODULE_PATH_32 := $(ART_TEST_OUT)/$(ART_TARGET_ARCH_32)
    LOCAL_MODULE_PATH_64 := $(ART_TEST_OUT)/$(ART_TARGET_ARCH_64)
    LOCAL_MODULE_TARGET_ARCH := $(ART_SUPPORTED_ARCH)
    include $(BUILD_SHARED_LIBRARY)
  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_CFLAGS := $(ART_HOST_CFLAGS) $(ART_HOST_DEBUG_CFLAGS)
    LOCAL_STATIC_LIBRARIES := libcutils
    LOCAL_LDLIBS += -ldl -lpthread
    ifeq ($(HOST_OS),linux)
      LOCAL_LDLIBS += -lrt
    endif
    LOCAL_IS_HOST_MODULE := true
    include $(BUILD_HOST_SHARED_LIBRARY)
  endif
endef

ifeq ($(ART_BUILD_TARGET),true)
  $(eval $(call build-libarttest,target))
endif
ifeq ($(ART_BUILD_HOST),true)
  $(eval $(call build-libarttest,host))
endif
