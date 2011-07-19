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
LOCAL_MODULE := libarttest
LOCAL_MODULE_TAGS := tests
LOCAL_SRC_FILES := $(LIBARTTEST_COMMON_SRC_FILES)
LOCAL_CFLAGS := $(ART_CFLAGS)
LOCAL_LDLIBS := -lrt
include $(BUILD_HOST_SHARED_LIBRARY)

$(foreach file,$(TEST_HOST_SRC_FILES), \
  $(eval include $(CLEAR_VARS)) \
  $(eval LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)) \
  $(eval LOCAL_MODULE := $(notdir $(file:%.cc=%))) \
  $(eval LOCAL_MODULE_TAGS := tests) \
  $(eval LOCAL_SRC_FILES := $(file)) \
  $(eval LOCAL_CFLAGS := $(ART_CFLAGS)) \
  $(eval LOCAL_C_INCLUDES += external/gtest/include) \
  $(eval LOCAL_WHOLE_STATIC_LIBRARIES := libgtest_host libgtest_main_host) \
  $(eval LOCAL_SHARED_LIBRARIES := libarttest libart) \
  $(eval include $(BUILD_HOST_EXECUTABLE)) \
)
