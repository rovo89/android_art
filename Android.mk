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
include $(build_path)/Android.gtest.mk
include $(build_path)/Android.oattest.mk

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

ART_TARGET_TEST_DEPENDENCIES += $(TARGET_OUT_EXECUTABLES)/oat_process $(TARGET_OUT_EXECUTABLES)/oat_processd

########################################################################
# host test targets

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

########################################################################
# target test targets

# "mm test-art-target" to build and run all target tests
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
test-art-target-oat: $(ART_TEST_OAT_TARGETS)

########################################################################
# oat_process test targets

# $(1): name
define build-art-framework-oat
  $(call build-art-oat,$(1),$(TARGET_BOOT_OAT),$(TARGET_BOOT_DEX))
endef

.PHONY: test-art-target-oat-process
test-art-target-oat-process: test-art-target-oat-process-am # test-art-target-oat-process-Calculator

$(eval $(call build-art-framework-oat,$(TARGET_OUT_JAVA_LIBRARIES)/am.jar))

.PHONY: test-art-target-oat-process-am
test-art-target-oat-process-am: $(TARGET_OUT_JAVA_LIBRARIES)/am.oat test-art-target-sync
	adb remount
	adb sync
	adb shell sh -c "export CLASSPATH=/system/framework/am.jar && oat_processd /system/bin/app_process -Xbootimage:/system/framework/boot.oat -Ximage:/system/framework/am.oat /system/bin com.android.commands.am.Am start http://android.com && touch /sdcard/test-art-target-process-am"
	$(hide) (adb pull /sdcard/test-art-target-process-am /tmp/ && echo test-art-target-process-am PASSED) || echo test-art-target-process-am FAILED
	$(hide) rm /tmp/test-art-target-process-am

$(eval $(call build-art-framework-oat,$(TARGET_OUT_APPS)/Calculator.apk))

.PHONY: test-art-target-oat-process-Calculator
# Note that using this instead of "adb shell am start" make sure that the /data/art-cache is up-to-date
test-art-target-oat-process-Calculator: $(TARGET_OUT_APPS)/Calculator.oat test-art-target-sync
	mkdir -p $(TARGET_OUT_DATA)/art-cache
	unzip $(TARGET_OUT_APPS)/Calculator.apk classes.dex -d $(TARGET_OUT_DATA)/art-cache
	mv $(TARGET_OUT_DATA)/art-cache/classes.dex $(TARGET_OUT_DATA)/art-cache/system@app@Calculator.apk@classes.dex.c96b4ebb # crc32 from "unzip -lv $(TARGET_OUT_APPS)/Calculator.apk"
	adb remount
	adb sync
	adb shell setprop wrap.com.android.calculator2 "oat_processd"
	adb shell stop
	adb shell start
	sleep 15 # sleep 30
	adb shell sh -c "export CLASSPATH=/system/framework/am.jar && oat_processd /system/bin/app_process -Xbootimage:/system/framework/boot.oat -Ximage:/system/framework/am.oat /system/bin com.android.commands.am.Am start -a android.intent.action.MAIN -n com.android.calculator2/.Calculator && touch /sdcard/test-art-target-process-Calculator"
	$(hide) (adb pull /sdcard/test-art-target-process-Calculator /tmp/ && echo test-art-target-process-Calculator PASSED) || echo test-art-target-process-Calculator FAILED
	$(hide) rm /tmp/test-art-target-process-Calculator

########################################################################
# oatdump targets

.PHONY: dump-core-oat
dump-core-oat: $(TARGET_CORE_OAT) $(OATDUMP)
	$(OATDUMP) $(addprefix --dex-file=,$(TARGET_CORE_DEX)) --image=$(TARGET_CORE_OAT) --strip-prefix=$(PRODUCT_OUT) --output=/tmp/core.oatdump.txt
	@echo Output in /tmp/core.oatdump.txt

.PHONY: dump-boot-oat
dump-boot-oat: $(TARGET_BOOT_OAT) $(OATDUMP)
	$(OATDUMP) $(addprefix --dex-file=,$(TARGET_BOOT_DEX)) --image=$(TARGET_BOOT_OAT) --strip-prefix=$(PRODUCT_OUT) --output=/tmp/boot.oatdump.txt
	@echo Output in /tmp/boot.oatdump.txt

########################################################################
# cpplint target

# "mm cpplint-art" to style check art source files
.PHONY: cpplint-art
cpplint-art:
	$(LOCAL_PATH)/tools/cpplint.py $(LOCAL_PATH)/src/*.h $(LOCAL_PATH)/src/*.cc

########################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
