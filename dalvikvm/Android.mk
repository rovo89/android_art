#
# Copyright (C) 2013 The Android Open Source Project
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

dalvikvm_cflags := -Wall -Werror -Wextra -std=gnu++11

include $(CLEAR_VARS)
LOCAL_MODULE := dalvikvm
LOCAL_MODULE_TAGS := optional
LOCAL_CPP_EXTENSION := cc
LOCAL_SRC_FILES := dalvikvm.cc
LOCAL_CFLAGS := $(dalvikvm_cflags)
LOCAL_C_INCLUDES := art/runtime
LOCAL_SHARED_LIBRARIES := libdl libnativehelper
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk
LOCAL_MULTILIB := both
LOCAL_MODULE_STEM_32 := dalvikvm32
LOCAL_MODULE_STEM_64 := dalvikvm64
include art/build/Android.libcxx.mk
include $(BUILD_EXECUTABLE)

# create symlink for the primary version target.
include  $(BUILD_SYSTEM)/executable_prefer_symlink.mk

ART_TARGET_EXECUTABLES += $(TARGET_OUT_EXECUTABLES)/$(LOCAL_MODULE)

ifeq ($(WITH_HOST_DALVIK),true)
include $(CLEAR_VARS)
LOCAL_MODULE := dalvikvm
LOCAL_MODULE_TAGS := optional
LOCAL_CLANG := true
LOCAL_CPP_EXTENSION := cc
LOCAL_SRC_FILES := dalvikvm.cc
LOCAL_CFLAGS := $(dalvikvm_cflags)
LOCAL_C_INCLUDES := art/runtime
LOCAL_SHARED_LIBRARIES := libnativehelper
LOCAL_LDFLAGS := -ldl -lpthread
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/Android.mk
LOCAL_IS_HOST_MODULE := true
include art/build/Android.libcxx.mk
include $(BUILD_HOST_EXECUTABLE)
ART_HOST_EXECUTABLES += $(HOST_OUT_EXECUTABLES)/$(LOCAL_MODULE)
endif
