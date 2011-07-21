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

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
LOCAL_MODULE := libart
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := $(LIBART_HOST_SRC_FILES)
LOCAL_CFLAGS := $(ART_CFLAGS)
LOCAL_C_INCLUDES += src external/gtest/include
LOCAL_SHARED_LIBRARIES := liblog libz-host
LOCAL_LDLIBS := -ldl -lpthread -lrt
include $(BUILD_HOST_SHARED_LIBRARY)
