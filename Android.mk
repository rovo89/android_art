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
    $(ART_HOST_TEST_EXECUTABLES)

# "mm test-art" to build and run all tests on host and device
.PHONY: test-art
test-art: test-art-host test-art-target

define run-host-tests-with
  $(foreach file,$(sort $(ART_HOST_TEST_EXECUTABLES)),$(1) $(file) &&) true
endef

ART_HOST_TEST_DEPENDENCIES   := $(ART_HOST_TEST_EXECUTABLES)   $(ANDROID_HOST_OUT)/framework/core-hostdex.jar   $(ART_TEST_DEX_FILES)
ART_TARGET_TEST_DEPENDENCIES := $(ART_TARGET_TEST_EXECUTABLES) $(ANDROID_PRODUCT_OUT)/system/framework/core.jar $(ART_TEST_DEX_FILES)

# "mm test-art-host" to build and run all host tests
.PHONY: test-art-host
test-art-host: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-tests-with,)

# "mm valgrind-art-host" to build and run all host tests under valgrind.
.PHONY: valgrind-art-host
valgrind-art-host: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-tests-with,"valgrind")

# "mm tsan-art-host" to build and run all host tests under tsan.
.PHONY: tsan-art-host
tsan-art-host: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-tests-with,"tsan")

# "mm test-art-device" to build and run all target tests
.PHONY: test-art-target
test-art-target: $(ART_TARGET_TEST_DEPENDENCIES)
	adb remount
	adb sync
	adb shell touch /sdcard/test-art-target
	adb shell rm /sdcard/test-art-target
	adb shell sh -c "$(foreach file,$(sort $(ART_TARGET_TEST_EXECUTABLES)), /system/bin/$(notdir $(file)) &&) touch /sdcard/test-art-target"
	adb pull /sdcard/test-art-target /tmp/
	rm /tmp/test-art-target

# "mm cpplint-art" to style check art source files
.PHONY: cpplint-art
cpplint-art:
	$(LOCAL_PATH)/tools/cpplint.py $(LOCAL_PATH)/src/*.h $(LOCAL_PATH)/src/*.cc
