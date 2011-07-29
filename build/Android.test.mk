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
# $(2): file name with .cc or .cc.arm extension
define build-art-test
  include $(CLEAR_VARS)
  ifeq ($(1),target)
    include external/stlport/libstlport.mk
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  LOCAL_MODULE := $(notdir $(basename $(2:%.arm=%)))
  LOCAL_MODULE_TAGS := tests
  LOCAL_SRC_FILES := $(2)
  LOCAL_CFLAGS := $(ART_CFLAGS)
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)
  LOCAL_SHARED_LIBRARIES := libarttest libartd
  ifeq ($(1),target)
    LOCAL_SHARED_LIBRARIES += libstlport libz
    LOCAL_STATIC_LIBRARIES := libgtest libgtest_main
  else
    LOCAL_SHARED_LIBRARIES += libz-host
    LOCAL_WHOLE_STATIC_LIBRARIES := libgtest_host libgtest_main_host
  endif
  ifeq ($(1),target)
    include $(BUILD_EXECUTABLE)
  else
    include $(BUILD_HOST_EXECUTABLE)
  endif
  ifeq ($(1),target)
    ART_TARGET_TEST_EXECUTABLES += $(TARGET_OUT_EXECUTABLES)/$$(LOCAL_MODULE)
  else
    ART_HOST_TEST_EXECUTABLES += $(HOST_OUT_EXECUTABLES)/$$(LOCAL_MODULE)
    LOCAL_LDFLAGS += -lpthread
  endif
endef

$(foreach file,$(TEST_TARGET_SRC_FILES), $(eval $(call build-art-test,target,$(file))))
ifeq ($(WITH_HOST_DALVIK),true)
  $(foreach file,$(TEST_HOST_SRC_FILES), $(eval $(call build-art-test,host,$(file))))
endif
