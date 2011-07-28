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
    $(ART_TARGET_TEST_EXECUTABLES) \
    $(HOST_OUT_EXECUTABLES)/aexec \
    $(ART_HOST_TEST_EXECUTABLES) \
#

# "mm test-art" to build and run all tests on host and device
.PHONY: test-art
test-art: test-art-host test-art-target

# "mm test-art-host" to build and run all host tests
.PHONY: test-art-host
test-art-host: $(ART_HOST_TEST_EXECUTABLES)
	$(foreach file,$(ART_HOST_TEST_EXECUTABLES),$(file) &&) true

# "mm test-art-device" to build and run all target tests
.PHONY: test-art-target
test-art-target: $(ART_TARGET_TEST_EXECUTABLES)
	adb remount
	adb sync
	adb shell touch /sdcard/test-art-target
	adb shell rm /sdcard/test-art-target
	adb shell sh -c "$(foreach file,$(ART_TARGET_TEST_EXECUTABLES), /system/bin/$(notdir $(file)) &&) touch /sdcard/test-art-target"
	adb pull /sdcard/test-art-target /tmp/
	rm /tmp/test-art-target


# "mm cpplint-art" to style check art source files
.PHONY: cpplint-art
cpplint-art:
	$(LOCAL_PATH)/tools/cpplint.py $(LOCAL_PATH)/src/*.h $(LOCAL_PATH)/src/*.cc
