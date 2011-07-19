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

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

build_path := $(LOCAL_PATH)/build
include $(build_path)/Android.common.mk

include $(build_path)/Android.libart.mk
include $(build_path)/Android.test.mk
include $(build_path)/Android.aexec.mk

ifeq ($(WITH_HOST_DALVIK),true)
    include $(build_path)/Android.libart.host.mk
    include $(build_path)/Android.test.host.mk
    include $(build_path)/Android.aexec.host.mk
endif


# "m build-art" for quick minimal build
.PHONY: build-art
build-art: \
    $(TARGET_OUT_EXECUTABLES)/aexec \
    $(foreach file,$(TEST_TARGET_SRC_FILES),$(TARGET_OUT_EXECUTABLES)/$(notdir $(file:%.cc=%))) \
    $(HOST_OUT_EXECUTABLES)/aexec \
    $(foreach file,$(TEST_HOST_SRC_FILES),$(HOST_OUT_EXECUTABLES)/$(notdir $(file:%.cc=%))) \
#

# "mm test-art-host" to build and run all host tests
.PHONY: test-art-host
test-art-host: $(foreach file,$(TEST_HOST_SRC_FILES),$(HOST_OUT_EXECUTABLES)/$(notdir $(file:%.cc=%)))
	$(foreach file,$(TEST_HOST_SRC_FILES),$(HOST_OUT_EXECUTABLES)/$(notdir $(file:%.cc=%)) &&) true
