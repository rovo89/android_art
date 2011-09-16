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

ART_HOST_EXECUTABLES :=
ART_TARGET_EXECUTABLES :=

# $(1): executable ("d" will be appended for debug version)
# $(2): source
# $(3): target or host
# $(4): ndebug or debug
define build-art-executable
  include $(CLEAR_VARS)
  ifeq ($(3),target)
    include external/stlport/libstlport.mk
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  ifeq ($(4),ndebug)
    LOCAL_MODULE := $(1)
  else
    LOCAL_MODULE := $(1)d
  endif
  LOCAL_MODULE_TAGS := optional
  LOCAL_SRC_FILES := $(2)
  ifeq ($(3),target)
    LOCAL_CFLAGS := $(ART_TARGET_CFLAGS)
  else
    LOCAL_CFLAGS := $(ART_HOST_CFLAGS)
  endif
  ifeq ($(4),debug)
    ifeq ($(3),target)
      LOCAL_CFLAGS += $(ART_TARGET_DEBUG_CFLAGS)
    else
      LOCAL_CFLAGS += $(ART_HOST_DEBUG_CFLAGS)
      LOCAL_STATIC_LIBRARIES := libgtest_host
    endif
  endif
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)
  LOCAL_SHARED_LIBRARIES := libnativehelper
  ifeq ($(4),ndebug)
    LOCAL_SHARED_LIBRARIES += libart
  else
    LOCAL_SHARED_LIBRARIES += libartd
  endif
  ifeq ($(3),target)
    LOCAL_SHARED_LIBRARIES += libstlport
  endif
  ifeq ($(3),target)
    include $(BUILD_EXECUTABLE)
  else
    include $(BUILD_HOST_EXECUTABLE)
  endif
  ifeq ($(1),target)
    ART_TARGET_EXECUTABLES += $(TARGET_OUT_EXECUTABLES)/$$(LOCAL_MODULE)
  else
    ART_HOST_EXECUTABLES += $(HOST_OUT_EXECUTABLES)/$$(LOCAL_MODULE)
  endif
endef

ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),target,ndebug))
  $(eval $(call build-art-executable,oatdump,$(OATDUMP_SRC_FILES),target,ndebug))
  $(eval $(call build-art-executable,oatexec,$(OATEXEC_SRC_FILES),target,ndebug))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),target,debug))
  $(eval $(call build-art-executable,oatdump,$(OATDUMP_SRC_FILES),target,debug))
  $(eval $(call build-art-executable,oatexec,$(OATEXEC_SRC_FILES),target,debug))
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),host,ndebug))
  $(eval $(call build-art-executable,oatdump,$(OATDUMP_SRC_FILES),host,ndebug))
  $(eval $(call build-art-executable,oatexec,$(OATEXEC_SRC_FILES),host,ndebug))
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),host,debug))
  $(eval $(call build-art-executable,oatdump,$(OATDUMP_SRC_FILES),host,debug))
  $(eval $(call build-art-executable,oatexec,$(OATEXEC_SRC_FILES),host,debug))
endif

