# Copyright (C) 2015 The Android Open Source Project
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

# TODO(ajcbik): Art-i-fy this makefile

LOCAL_PATH:= $(call my-dir)

dexlist_src_files := dexlist.cc
dexlist_c_includes := art/runtime
dexlist_libraries := libart

##
## Build the device command line tool dexlist.
##

ifneq ($(SDK_ONLY),true)  # SDK_only doesn't need device version
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := cc
LOCAL_SRC_FILES := $(dexlist_src_files)
LOCAL_C_INCLUDES := $(dexlist_c_includes)
LOCAL_CFLAGS += -Wall
LOCAL_SHARED_LIBRARIES += $(dexlist_libraries)
LOCAL_MODULE := dexlist
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT_OPTIONAL_EXECUTABLES)
include $(BUILD_EXECUTABLE)
endif # !SDK_ONLY

##
## Build the host command line tool dexlist.
##

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := cc
LOCAL_SRC_FILES := $(dexlist_src_files)
LOCAL_C_INCLUDES := $(dexlist_c_includes)
LOCAL_CFLAGS += -Wall
LOCAL_SHARED_LIBRARIES += $(dexlist_libraries)
LOCAL_MODULE := dexlist
LOCAL_MULTILIB := $(ART_MULTILIB_OVERRIDE_host)
include $(BUILD_HOST_EXECUTABLE)
