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

ART_EXECUTABLES_CFLAGS :=
ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  ART_EXECUTABLES_CFLAGS += -DART_USE_PORTABLE_COMPILER=1
endif

# $(1): executable ("d" will be appended for debug version)
# $(2): source
# $(3): target or host
# $(4): ndebug or debug
define build-art-executable
  ifneq ($(3),target)
    ifneq ($(3),host)
      $$(error expected target or host for argument 3, received $(3))
    endif
  endif
  ifneq ($(4),ndebug)
    ifneq ($(4),debug)
      $$(error expected ndebug or debug for argument 4, received $(4))
    endif
  endif

  art_executable := $(1)
  art_source := $(2)
  art_target_or_host := $(3)
  art_ndebug_or_debug := $(4)

  include $(CLEAR_VARS)
  ifeq ($$(art_target_or_host),target)
    include external/stlport/libstlport.mk
  endif

  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  LOCAL_MODULE_TAGS := optional
  LOCAL_SRC_FILES := $$(art_source)
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)
  LOCAL_SHARED_LIBRARIES := libnativehelper

  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := $$(art_executable)
  else #debug
    LOCAL_MODULE := $$(art_executable)d
  endif

  LOCAL_CFLAGS := $(ART_EXECUTABLES_CFLAGS)
  ifeq ($$(art_target_or_host),target)
    LOCAL_CLANG := $(ART_TARGET_CLANG)
    LOCAL_CFLAGS += $(ART_TARGET_CFLAGS)
    ifeq ($$(art_ndebug_or_debug),debug)
      LOCAL_CFLAGS += $(ART_TARGET_DEBUG_CFLAGS)
    else
      LOCAL_CFLAGS += $(ART_TARGET_NON_DEBUG_CFLAGS)
    endif
  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS)
    ifeq ($$(art_ndebug_or_debug),debug)
      LOCAL_CFLAGS += $(ART_HOST_DEBUG_CFLAGS)
    else
      LOCAL_CFLAGS += $(ART_HOST_NON_DEBUG_CFLAGS)
    endif
  endif

  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_SHARED_LIBRARIES += libart
  else # debug
    LOCAL_SHARED_LIBRARIES += libartd
  endif

  ifeq ($$(art_target_or_host),target)
    LOCAL_SHARED_LIBRARIES += libstlport
  endif

  ifeq ($$(art_target_or_host),target)
    include $(BUILD_EXECUTABLE)
    ART_TARGET_EXECUTABLES := $(ART_TARGET_EXECUTABLES) $(TARGET_OUT_EXECUTABLES)/$$(LOCAL_MODULE)
  else # host
    include $(BUILD_HOST_EXECUTABLE)
    ART_HOST_EXECUTABLES := $(ART_HOST_EXECUTABLES) $(HOST_OUT_EXECUTABLES)/$$(LOCAL_MODULE)
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

# We always build dex2oat and dependencies, even if the host build is otherwise disabled, since they are used to cross compile for the target.
ifeq ($(ART_BUILD_NDEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),host,ndebug))
endif
ifeq ($(ART_BUILD_NDEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),host,debug))
endif

ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-art-executable,oatdump,$(OATDUMP_SRC_FILES),host,ndebug))
  $(eval $(call build-art-executable,oatexec,$(OATEXEC_SRC_FILES),host,ndebug))
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-art-executable,oatdump,$(OATDUMP_SRC_FILES),host,debug))
  $(eval $(call build-art-executable,oatexec,$(OATEXEC_SRC_FILES),host,debug))
endif
