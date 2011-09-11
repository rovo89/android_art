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

# $(1): target or host
# $(2): ndebug or debug
define build-libart
  include $(CLEAR_VARS)
  ifeq ($(1),target)
    include external/stlport/libstlport.mk
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  ifeq ($(2),ndebug)
    LOCAL_MODULE := libart
  else
    LOCAL_MODULE := libartd
  endif
  LOCAL_MODULE_TAGS := optional
  ifeq ($(1),target)
    LOCAL_SRC_FILES := $(LIBART_TARGET_SRC_FILES)
  else
    LOCAL_SRC_FILES := $(LIBART_HOST_SRC_FILES)
  endif
  LOCAL_CFLAGS := $(ART_CFLAGS)
  ifeq ($(2),debug)
    LOCAL_CFLAGS += -UNDEBUG
  endif
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)
  LOCAL_SHARED_LIBRARIES := liblog libnativehelper
  ifeq ($(1),target)
    LOCAL_SHARED_LIBRARIES += libcutils libstlport libz libdl
  else
    LOCAL_SHARED_LIBRARIES += libz-host
    LOCAL_LDLIBS := -ldl -lpthread -lrt
  endif
  LOCAL_STATIC_LIBRARIES += libdex
  ifeq ($(1),target)
    include $(BUILD_SHARED_LIBRARY)
  else
    include $(BUILD_HOST_SHARED_LIBRARY)
  endif
endef

ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  $(eval $(call build-libart,target,ndebug))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-libart,target,debug))
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-libart,host,ndebug))
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-libart,host,debug))
endif
