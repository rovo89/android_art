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

# Java method trace dump tool

LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := cc
LOCAL_SRC_FILES := tracedump.cc
LOCAL_CFLAGS += -O0 -g -Wall
LOCAL_MODULE_HOST_OS := darwin linux windows
LOCAL_MODULE := dmtracedump
include $(BUILD_HOST_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := cc
LOCAL_SRC_FILES := createtesttrace.cc
LOCAL_CFLAGS += -O0 -g -Wall
LOCAL_MODULE := create_test_dmtrace
include $(BUILD_HOST_EXECUTABLE)
