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
define build-aexec
  include $(CLEAR_VARS)
  ifeq ($(1),target)
    include external/stlport/libstlport.mk
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  ifeq ($(2),ndebug)
    LOCAL_MODULE := aexec
  else
    LOCAL_MODULE := aexecd
  endif
  LOCAL_MODULE_TAGS := optional
  LOCAL_SRC_FILES := $(AEXEC_SRC_FILES)
  LOCAL_CFLAGS := $(ART_CFLAGS)
  ifeq ($(2),debug)
    LOCAL_CFLAGS += -UNDEBUG
  endif
  LOCAL_SHARED_LIBRARIES := libnativehelper
  ifeq ($(2),ndebug)
    LOCAL_SHARED_LIBRARIES += libart
  else
    LOCAL_SHARED_LIBRARIES += libartd
  endif
  ifeq ($(1),target)
    LOCAL_SHARED_LIBRARIES += libstlport
  endif
  ifeq ($(1),target)
    include $(BUILD_EXECUTABLE)
  else
    include $(BUILD_HOST_EXECUTABLE)
  endif
endef

$(eval $(call build-aexec,target,ndebug))
$(eval $(call build-aexec,target,debug))
ifeq ($(WITH_HOST_DALVIK),true)
  $(eval $(call build-aexec,host,ndebug))
  $(eval $(call build-aexec,host,debug))
endif

