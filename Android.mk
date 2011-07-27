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
include $(build_path)/Android.aexec.mk

include $(build_path)/Android.libarttest.mk
include $(build_path)/Android.test.mk

# "m build-art" for quick minimal build
.PHONY: build-art
build-art: \
    $(TARGET_OUT_EXECUTABLES)/aexec \
    $(foreach file,$(TEST_TARGET_SRC_FILES),$(TARGET_OUT_EXECUTABLES)/$(notdir $(basename $(file:%.arm=%)))) \
    $(HOST_OUT_EXECUTABLES)/aexec \
    $(foreach file,$(TEST_HOST_SRC_FILES),$(HOST_OUT_EXECUTABLES)/$(notdir $(basename $(file:%.arm=%)))) \
#

# "mm test-art" to build and run all tests on host and device
.PHONY: test-art
test-art: test-art-host test-art-target

# "mm test-art-host" to build and run all host tests
.PHONY: test-art-host
test-art-host: $(foreach file,$(TEST_HOST_SRC_FILES),$(HOST_OUT_EXECUTABLES)/$(notdir $(basename $(file:%.arm=%))))
	$(foreach file,$(TEST_HOST_SRC_FILES),$(HOST_OUT_EXECUTABLES)/$(notdir $(basename $(file:%.arm=%))) &&) true

# "mm test-art-device" to build and run all target tests
.PHONY: test-art-target
test-art-target: $(foreach file,$(TEST_TARGET_SRC_FILES),$(TARGET_OUT_EXECUTABLES)/$(notdir $(basename $(file:%.arm=%))))
	adb remount
	adb sync
	adb shell touch /sdcard/test-art-target
	adb shell rm /sdcard/test-art-target
	adb shell sh -c "$(foreach file,$(TEST_TARGET_SRC_FILES), /system/bin/$(notdir $(basename $(file:%.arm=%))) &&) touch /sdcard/test-art-target"
	adb pull /sdcard/test-art-target /tmp/
	rm /tmp/test-art-target


# "mm cpplint-art" to style check art source files
.PHONY: cpplint-art
cpplint-art:
	$(LOCAL_PATH)/tools/cpplint.py $(LOCAL_PATH)/src/*.h $(LOCAL_PATH)/src/*.cc
