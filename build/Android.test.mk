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
include external/stlport/libstlport.mk
LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
LOCAL_MODULE := libarttest
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := $(LIBARTTEST_COMMON_SRC_FILES)
LOCAL_CFLAGS := $(ART_CFLAGS)
LOCAL_SHARED_LIBRARIES := libstlport
include $(BUILD_SHARED_LIBRARY)

$(foreach file,$(TEST_TARGET_SRC_FILES), \
  $(eval include $(CLEAR_VARS)) \
  $(eval include external/stlport/libstlport.mk) \
  $(eval LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)) \
  $(eval LOCAL_MODULE := $(notdir $(basename $(file:%.arm=%)))) \
  $(eval LOCAL_MODULE_TAGS := tests) \
  $(eval LOCAL_SRC_FILES := $(file)) \
  $(eval LOCAL_CFLAGS := $(ART_CFLAGS)) \
  $(eval LOCAL_C_INCLUDES += external/gtest/include) \
  $(eval LOCAL_STATIC_LIBRARIES := libgtest libgtest_main) \
  $(eval LOCAL_SHARED_LIBRARIES := libarttest libart libstlport) \
  $(eval include $(BUILD_EXECUTABLE)) \
)
