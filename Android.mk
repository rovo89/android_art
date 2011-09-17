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

# These can be overridden via the environment or by editing to
# enable/disable certain build configuration.
ART_BUILD_TARGET_NDEBUG ?= true
ART_BUILD_TARGET_DEBUG ?= true
ifeq ($(HOST_OS),linux)
  ART_BUILD_HOST_NDEBUG ?= true
  ART_BUILD_HOST_DEBUG ?= true
else
  ART_BUILD_HOST_NDEBUG ?= false
  ART_BUILD_HOST_DEBUG ?= false
endif

build_path := $(LOCAL_PATH)/build
include $(build_path)/Android.common.mk

include $(build_path)/Android.libart.mk
include $(build_path)/Android.executable.mk

include $(build_path)/Android.oat.mk

include $(build_path)/Android.libarttest.mk
include $(build_path)/Android.test.mk

# "m build-art" for quick minimal build
.PHONY: build-art
build-art: \
    $(ART_TARGET_EXECUTABLES) \
    $(ART_TARGET_TEST_EXECUTABLES) \
    $(ART_HOST_EXECUTABLES) \
    $(ART_HOST_TEST_EXECUTABLES)

# "mm test-art" to build and run all tests on host and device
.PHONY: test-art
test-art: test-art-host test-art-target

define run-host-tests-with
  $(foreach file,$(sort $(ART_HOST_TEST_EXECUTABLES)),$(1) $(file) &&) true
endef

ART_HOST_TEST_DEPENDENCIES   := $(ART_HOST_TEST_EXECUTABLES)   $(ANDROID_HOST_OUT)/framework/core-hostdex.jar   $(ART_TEST_OAT_FILES)
ART_TARGET_TEST_DEPENDENCIES := $(ART_TARGET_TEST_EXECUTABLES) $(ANDROID_PRODUCT_OUT)/system/framework/core.jar $(ART_TEST_OAT_FILES)

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
test-art-target: test-art-target-gtest test-art-target-oat

.PHONY: test-art-target-sync
test-art-target-sync: $(ART_TARGET_TEST_DEPENDENCIES)
	adb remount
	adb sync

.PHONY: test-art-target-gtest
test-art-target-gtest: test-art-target-sync
	adb shell touch /sdcard/test-art-target-gtest
	adb shell rm /sdcard/test-art-target-gtest
	adb shell sh -c "$(foreach file,$(sort $(ART_TARGET_TEST_EXECUTABLES)), /system/bin/$(notdir $(file)) &&) touch /sdcard/test-art-target-gtest"
	$(hide) (adb pull /sdcard/test-art-target-gtest /tmp/ && echo test-art-target-gtest PASSED) || echo test-art-target-gtest FAILED
	$(hide) rm /tmp/test-art-target-gtest

.PHONY: test-art-target-oat
test-art-target-oat: test-art-target-oat-HelloWorld test-art-target-oat-Fibonacci

.PHONY: test-art-target-oat-HelloWorld
test-art-target-oat-HelloWorld: test-art-target-sync
	adb shell touch /sdcard/test-art-target-oat-HelloWorld
	adb shell rm /sdcard/test-art-target-oat-HelloWorld
	adb shell sh -c "oatexecd -Xbootclasspath:/system/framework/core.jar -Xbootimage:/system/framework/boot.oat -classpath /system/framework/art-test-dex-HelloWorld.jar -Ximage:/system/framework/art-test-dex-HelloWorld.oat HelloWorld && touch /sdcard/test-art-target-oat-HelloWorld"
	$(hide) (adb pull /sdcard/test-art-target-oat-HelloWorld /tmp/ && echo test-art-target-oat-HelloWorld PASSED) || (echo test-art-target-oat-HelloWorld FAILED && exit 1)
	$(hide) rm /tmp/test-art-target-oat-HelloWorld

.PHONY: test-art-target-oat-Fibonacci
test-art-target-oat-Fibonacci: test-art-target-sync
	adb shell touch /sdcard/test-art-target-oat-Fibonacci
	adb shell rm /sdcard/test-art-target-oat-Fibonacci
	adb shell sh -c "oatexecd -Xbootclasspath:/system/framework/core.jar -Xbootimage:/system/framework/boot.oat -classpath /system/framework/art-test-dex-Fibonacci.jar -Ximage:/system/framework/art-test-dex-Fibonacci.oat Fibonacci 10 && touch /sdcard/test-art-target-oat-Fibonacci"
	$(hide) (adb pull /sdcard/test-art-target-oat-Fibonacci /tmp/ && echo test-art-target-oat-Fibonacci PASSED) || (echo test-art-target-oat-Fibonacci FAILED && exit 1)
	$(hide) rm /tmp/test-art-target-oat-Fibonacci

.PHONY: dump-boot-oat
dump-boot-oat: $(TARGET_BOOT_OAT) $(OATDUMP)
	$(OATDUMP) $(addprefix --dex-file=,$(TARGET_BOOT_DEX)) --image=$(TARGET_BOOT_OAT) --strip-prefix=$(PRODUCT_OUT)

# "mm cpplint-art" to style check art source files
.PHONY: cpplint-art
cpplint-art:
	$(LOCAL_PATH)/tools/cpplint.py $(LOCAL_PATH)/src/*.h $(LOCAL_PATH)/src/*.cc
