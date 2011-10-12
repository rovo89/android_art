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

ART_HOST_TEST_EXECUTABLES :=
ART_TARGET_TEST_EXECUTABLES :=

# $(1): target or host
# $(2): file name
define build-art-test
  ifneq ($(1),target)
    ifneq ($(1),host)
      $$(error expected target or host for argument 1, received $(1))
    endif
  endif

  art_target_or_host := $(1)
  art_gtest_filename := $(2)

  include $(CLEAR_VARS)
  ifeq ($$(art_target_or_host),target)
    include external/stlport/libstlport.mk
  endif

  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  LOCAL_MODULE := $(notdir $(basename $$(art_gtest_filename)))
  LOCAL_MODULE_TAGS := tests
  LOCAL_SRC_FILES := $$(art_gtest_filename)
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)
  LOCAL_SHARED_LIBRARIES := libarttest libartd

  ifeq ($$(art_target_or_host),target)
    LOCAL_CFLAGS := $(ART_TARGET_CFLAGS) $(ART_TARGET_DEBUG_CFLAGS)
    LOCAL_SHARED_LIBRARIES += libdl libicuuc libicui18n libnativehelper libstlport libz
    LOCAL_STATIC_LIBRARIES := libgtest libgtest_main
    include $(BUILD_EXECUTABLE)
    ART_TARGET_TEST_EXECUTABLES += $(TARGET_OUT_EXECUTABLES)/$$(LOCAL_MODULE)
  else # host
    LOCAL_CFLAGS := $(ART_HOST_CFLAGS) $(ART_HOST_DEBUG_CFLAGS)
    LOCAL_SHARED_LIBRARIES += libicuuc-host libicui18n-host libnativehelper libz-host
    LOCAL_WHOLE_STATIC_LIBRARIES := libgtest_main_host
    include $(BUILD_HOST_EXECUTABLE)
    ART_HOST_TEST_EXECUTABLES += $(HOST_OUT_EXECUTABLES)/$$(LOCAL_MODULE)
  endif

endef

ifeq ($(ART_BUILD_TARGET),true)
  $(foreach file,$(TEST_TARGET_SRC_FILES), $(eval $(call build-art-test,target,$(file))))
endif
ifeq ($(ART_BUILD_HOST),true)
  $(foreach file,$(TEST_HOST_SRC_FILES), $(eval $(call build-art-test,host,$(file))))
endif
