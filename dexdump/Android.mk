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

# TODO(ajcbik): rename dexdump2 into dexdump when Dalvik version is removed

LOCAL_PATH:= $(call my-dir)

dexdump_src_files := dexdump_main.cc dexdump.cc
dexdump_c_includes := art/runtime
dexdump_libraries := libart

##
## Build the device command line tool dexdump.
##

ifneq ($(SDK_ONLY),true)  # SDK_only doesn't need device version
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := cc
LOCAL_SRC_FILES := $(dexdump_src_files)
LOCAL_C_INCLUDES := $(dexdump_c_includes)
LOCAL_CFLAGS += -Wall
LOCAL_SHARED_LIBRARIES += $(dexdump_libraries)
LOCAL_MODULE := dexdump2
include $(BUILD_EXECUTABLE)
endif # !SDK_ONLY

##
## Build the host command line tool dexdump.
##

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := cc
LOCAL_SRC_FILES := $(dexdump_src_files)
LOCAL_C_INCLUDES := $(dexdump_c_includes)
LOCAL_CFLAGS += -Wall
LOCAL_SHARED_LIBRARIES += $(dexdump_libraries)
LOCAL_MODULE := dexdump2
LOCAL_MULTILIB := $(ART_MULTILIB_OVERRIDE_host)
include $(BUILD_HOST_EXECUTABLE)
