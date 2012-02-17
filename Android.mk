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
ART_BUILD_HOST_NDEBUG ?= true
ART_BUILD_HOST_DEBUG ?= true

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
	@echo test-art PASSED

.PHONY: test-art-gtest
test-art-gtest: test-art-host test-art-target-gtest
	@echo test-art-gtest PASSED

define run-host-tests-with
  $(foreach file,$(sort $(ART_HOST_TEST_EXECUTABLES)),$(1) $(file) &&) true
endef

ART_HOST_DEPENDENCIES := $(ART_HOST_EXECUTABLES) $(HOST_OUT_JAVA_LIBRARIES)/core-hostdex.jar
ifeq ($(HOST_OS),linux)
  ART_HOST_DEPENDENCIES += $(HOST_OUT_SHARED_LIBRARIES)/libjavacore.so
else
  ART_HOST_DEPENDENCIES += $(HOST_OUT_SHARED_LIBRARIES)/libjavacore.dylib
endif

ART_TARGET_DEPENDENCIES := $(ART_TARGET_EXECUTABLES) $(TARGET_OUT_JAVA_LIBRARIES)/core.jar $(TARGET_OUT_SHARED_LIBRARIES)/libjavacore.so

ART_HOST_TEST_DEPENDENCIES   := $(ART_HOST_DEPENDENCIES)   $(ART_HOST_TEST_EXECUTABLES)   $(ART_TEST_DEX_FILES) $(ART_TEST_OAT_FILES)
ART_TARGET_TEST_DEPENDENCIES := $(ART_TARGET_DEPENDENCIES) $(ART_TARGET_TEST_EXECUTABLES) $(ART_TEST_DEX_FILES) $(ART_TEST_OAT_FILES)

########################################################################
# host test targets

# "mm test-art-host" to build and run all host tests
.PHONY: test-art-host
test-art-host: $(ART_HOST_TEST_DEPENDENCIES) $(ART_HOST_TEST_TARGETS)
	@echo test-art-host PASSED

# "mm valgrind-art-host" to build and run all host tests under valgrind.
.PHONY: valgrind-art-host
valgrind-art-host: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-tests-with,valgrind --leak-check=full)
	@echo valgrind-art-host PASSED

# "mm tsan-art-host" to build and run all host tests under tsan.
.PHONY: tsan-art-host
tsan-art-host: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-tests-with,"tsan")
	@echo tsan-art-host PASSED

########################################################################
# target test targets

# "mm test-art-target" to build and run all target tests
.PHONY: test-art-target
test-art-target: test-art-target-gtest test-art-target-oat test-art-target-run-test
	@echo test-art-target PASSED

.PHONY: test-art-target-sync
test-art-target-sync: $(ART_TARGET_TEST_DEPENDENCIES) $(ART_TEST_OUT)/libarttest.so
	adb remount
	adb sync
	adb shell mkdir -p $(ART_TEST_DIR)

.PHONY: test-art-target-gtest
test-art-target-gtest: $(ART_TARGET_TEST_TARGETS)

.PHONY: test-art-target-oat
test-art-target-oat: $(ART_TEST_OAT_TARGETS)
	@echo test-art-target-oat PASSED

.PHONY: test-art-target-run-test
test-art-target-run-test: test-art-target-run-test-002
	@echo test-art-target-run-test PASSED

.PHONY: test-art-target-run-test-002
test-art-target-run-test-002: test-art-target-sync
	art/test/run-test 002
	@echo test-art-target-run-test-002 PASSED

########################################################################
# oat test targets

# $(1): jar or apk name
define art-cache-oat
  $(call art-cache-out,$(1).oat)
endef

ART_CACHE_OATS :=
# $(1): name
define build-art-cache-oat
  $(call build-art-oat,$(PRODUCT_OUT)/$(1),/$(1),$(call art-cache-oat,$(1)),$(TARGET_BOOT_IMG_OUT))
  ART_CACHE_OATS += $(call art-cache-oat,$(1))
endef


########################################################################
# oat-target-sync

$(foreach file,\
  $(filter-out\
    $(TARGET_BOOT_DEX),\
    $(wildcard $(TARGET_OUT_APPS)/*.apk) $(wildcard $(TARGET_OUT_JAVA_LIBRARIES)/*.jar)),\
  $(eval $(call build-art-cache-oat,$(subst $(PRODUCT_OUT)/,,$(file)))))

.PHONY: oat-target-sync
oat-target-sync: $(ART_TARGET_DEPENDENCIES) $(TARGET_BOOT_OAT_OUT) $(ART_CACHE_OATS)
	adb remount
	adb sync

########################################################################
# oatdump targets

.PHONY: dump-oat
dump-oat: dump-oat-core dump-oat-boot dump-oat-Calculator

.PHONY: dump-oat-core
dump-oat-core: $(TARGET_CORE_OAT_OUT) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_CORE_IMG_OUT) --host-prefix=$(PRODUCT_OUT) --output=/tmp/core.oatdump.txt
	@echo Output in /tmp/core.oatdump.txt

.PHONY: dump-oat-boot
dump-oat-boot: $(TARGET_BOOT_OAT_OUT) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_BOOT_IMG_OUT) --host-prefix=$(PRODUCT_OUT) --output=/tmp/boot.oatdump.txt
	@echo Output in /tmp/boot.oatdump.txt

.PHONY: dump-oat-Calculator
dump-oat-Calculator: $(call art-cache-oat,system/app/Calculator.apk) $(TARGET_BOOT_OAT_OUT) $(OATDUMP)
	$(OATDUMP) --oat-file=$< --boot-image=$(TARGET_BOOT_IMG) --host-prefix=$(PRODUCT_OUT) --output=/tmp/Calculator.oatdump.txt
	@echo Output in /tmp/Calculator.oatdump.txt


########################################################################
# clean-oat target
#

.PHONY: clean-oat
clean-oat:
	rm -f $(ART_NATIVETEST_OUT)/*.oat
	rm -f $(ART_NATIVETEST_OUT)/*.art
	rm -f $(ART_TEST_OUT)/*.oat
	rm -f $(ART_TEST_OUT)/*.art
	rm -f $(ART_CACHE_OUT)/*.oat
	rm -f $(ART_CACHE_OUT)/*.art
	rm -f $(HOST_OUT_JAVA_LIBRARIES)/*.oat
	rm -f $(HOST_OUT_JAVA_LIBRARIES)/*.art
	rm -f $(TARGET_OUT_JAVA_LIBRARIES)/*.oat
	rm -f $(TARGET_OUT_JAVA_LIBRARIES)/*.art
	rm -f $(TARGET_OUT_APPS)/*.oat
	adb shell rm $(ART_NATIVETEST_DIR)/*.oat
	adb shell rm $(ART_NATIVETEST_DIR)/*.art
	adb shell rm $(ART_TEST_DIR)/*.oat
	adb shell rm $(ART_TEST_DIR)/*.art
	adb shell rm $(ART_CACHE_DIR)/*.oat
	adb shell rm $(ART_CACHE_DIR)/*.art
	adb shell rm $(DEXPREOPT_BOOT_JAR_DIR)/*.oat
	adb shell rm $(DEXPREOPT_BOOT_JAR_DIR)/*.art
	adb shell rm system/app/*.oat

########################################################################
# cpplint target

# "mm cpplint-art" to style check art source files
.PHONY: cpplint-art
cpplint-art:
	./art/tools/cpplint.py \
	    --filter=-whitespace/comments,-whitespace/line_length,-build/include,-build/header_guard,-readability/streams,-readability/todo,-runtime/references \
	    $(ANDROID_BUILD_TOP)/art/src/*.h $(ANDROID_BUILD_TOP)/art/src/*.cc

########################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
