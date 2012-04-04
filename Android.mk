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

########################################################################
# product targets
include $(build_path)/Android.libart.mk
include $(build_path)/Android.libart-compiler.mk
ifeq ($(ART_USE_LLVM_COMPILER),true)
include $(build_path)/Android.libart-compiler-llvm.mk
endif
include $(build_path)/Android.executable.mk
include $(build_path)/Android.oat.mk

# ART_HOST_DEPENDENCIES depends on Android.executable.mk above for ART_HOST_EXECUTABLES
ART_HOST_DEPENDENCIES := $(ART_HOST_EXECUTABLES) $(HOST_OUT_JAVA_LIBRARIES)/core-hostdex.jar
ifeq ($(HOST_OS),linux)
  ART_HOST_DEPENDENCIES += $(HOST_OUT_SHARED_LIBRARIES)/libjavacore.so
else
  ART_HOST_DEPENDENCIES += $(HOST_OUT_SHARED_LIBRARIES)/libjavacore.dylib
endif
ART_TARGET_DEPENDENCIES := $(ART_TARGET_EXECUTABLES) $(TARGET_OUT_JAVA_LIBRARIES)/core.jar $(TARGET_OUT_SHARED_LIBRARIES)/libjavacore.so

########################################################################
# test targets

include $(build_path)/Android.oattest.mk

# The ART_*_TEST_DEPENDENCIES definitions:
# - depend on Android.oattest.mk above for ART_TEST_*_DEX_FILES
# - are needed by Android.gtest.mk below
ART_HOST_TEST_DEPENDENCIES   := $(ART_HOST_DEPENDENCIES)   $(ART_HOST_TEST_EXECUTABLES)   $(ART_TEST_HOST_DEX_FILES)   $(HOST_CORE_IMG_OUT)
ART_TARGET_TEST_DEPENDENCIES := $(ART_TARGET_DEPENDENCIES) $(ART_TARGET_TEST_EXECUTABLES) $(ART_TEST_TARGET_DEX_FILES) $(TARGET_CORE_IMG_OUT)

include $(build_path)/Android.libarttest.mk
include $(build_path)/Android.gtest.mk

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
test-art-gtest: test-art-host-gtest test-art-target-gtest
	@echo test-art-gtest PASSED

.PHONY: test-art-oat
test-art-oat: test-art-target-oat # test-art-host-oat
	@echo test-art-oat PASSED

########################################################################
# host test targets

# "mm test-art-host" to build and run all host tests
.PHONY: test-art-host
test-art-host: test-art-host-gtest # test-art-host-oat # test-art-host-run-test
	@echo test-art-host PASSED

.PHONY: test-art-host-gtest
test-art-host-gtest: $(ART_HOST_TEST_TARGETS)
	@echo test-art-host-gtest PASSED

define run-host-gtests-with
  $(foreach file,$(sort $(ART_HOST_TEST_EXECUTABLES)),$(1) $(file) &&) true
endef

# "mm valgrind-test-art-host-gtest" to build and run the host gtests under valgrind.
.PHONY: valgrind-test-art-host-gtest
valgrind-test-art-host-gtest: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-gtests-with,valgrind --leak-check=full)
	@echo valgrind-test-art-host-gtest PASSED

# "mm tsan-test-art-host-gtest" to build and run the host gtests under tsan.
.PHONY: tsan-test-art-host-gtest
tsan-test-art-host-gtest: $(ART_HOST_TEST_DEPENDENCIES)
	$(call run-host-gtests-with,"tsan")
	@echo tsan-test-art-host-gtest PASSED

.PHONY: test-art-host-oat
test-art-host-oat: $(ART_TEST_HOST_OAT_TARGETS)
	@echo test-art-host-oat PASSED

define declare-test-art-host-run-test
.PHONY: test-art-host-run-test-$(1)
test-art-host-run-test-$(1): $(ART_HOST_TEST_DEPENDENCIES) $(HOST_OUT_SHARED_LIBRARIES)/libarttest.so
	art/test/run-test --host $(1)
	@echo test-art-host-run-test-$(1) PASSED

TEST_ART_HOST_RUN_TEST_TARGETS += test-art-host-run-test-$(1)
endef

$(foreach test, $(wildcard art/test/0*), $(eval $(call declare-test-art-host-run-test,$(notdir $(test)))))

.PHONY: test-art-host-run-test
test-art-host-run-test: $(TEST_ART_HOST_RUN_TEST_TARGETS)
	@echo test-art-host-run-test PASSED

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
test-art-target-oat: $(ART_TEST_TARGET_OAT_TARGETS)
	@echo test-art-target-oat PASSED

define declare-test-art-target-run-test
.PHONY: test-art-target-run-test-$(1)
test-art-target-run-test-$(1): test-art-target-sync
	art/test/run-test $(1)
	@echo test-art-target-run-test-$(1) PASSED

TEST_ART_TARGET_RUN_TEST_TARGETS += test-art-target-run-test-$(1)
endef

$(foreach test, $(wildcard art/test/0*), $(eval $(call declare-test-art-target-run-test,$(notdir $(test)))))

.PHONY: test-art-target-run-test
test-art-target-run-test: $(TEST_ART_TARGET_RUN_TEST_TARGETS)
	@echo test-art-target-run-test PASSED

########################################################################
# oat-target and oat-target-sync targets

OAT_TARGET_TARGETS :=

# $(1): input jar or apk target location
define declare-oat-target-target
.PHONY: oat-target-$(1)
oat-target-$(1): $(PRODUCT_OUT)/$(1) $(TARGET_BOOT_IMG_OUT) $(DEX2OAT_DEPENDENCY)
	$(DEX2OAT) --runtime-arg -Xms64m --runtime-arg -Xmx64m --boot-image=$(TARGET_BOOT_IMG_OUT) --dex-file=$(PRODUCT_OUT)/$(1) --dex-location=/$(1) --oat-file=$(call art-cache-out,$(1).oat) --host-prefix=$(PRODUCT_OUT)

OAT_TARGET_TARGETS += oat-target-$(1)
endef

$(foreach file,\
  $(filter-out\
    $(TARGET_BOOT_DEX),\
    $(wildcard $(TARGET_OUT_APPS)/*.apk) $(wildcard $(TARGET_OUT_JAVA_LIBRARIES)/*.jar)),\
  $(eval $(call declare-oat-target-target,$(subst $(PRODUCT_OUT)/,,$(file)))))

.PHONY: oat-target
oat-target: $(ART_TARGET_DEPENDENCIES) $(TARGET_BOOT_OAT_OUT) $(OAT_TARGET_TARGETS)

.PHONY: oat-target-sync
oat-target-sync: oat-target
	adb remount
	adb sync

########################################################################
# oatdump targets

.PHONY: dump-oat
dump-oat: dump-oat-core dump-oat-boot dump-oat-Calculator

.PHONY: dump-oat-core
dump-oat-core: dump-oat-core-host dump-oat-core-target

.PHONY: dump-oat-core-host
dump-oat-core-host: $(HOST_CORE_IMG_OUT) $(OATDUMP)
	$(OATDUMP) --image=$(HOST_CORE_IMG_OUT) --output=/tmp/core.host.oatdump.txt --host-prefix=""
	@echo Output in /tmp/core.host.oatdump.txt

.PHONY: dump-oat-core-target
dump-oat-core-target: $(TARGET_CORE_IMG_OUT) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_CORE_IMG_OUT) --output=/tmp/core.target.oatdump.txt
	@echo Output in /tmp/core.target.oatdump.txt

.PHONY: dump-oat-boot
dump-oat-boot: $(TARGET_BOOT_IMG_OUT) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_BOOT_IMG_OUT) --output=/tmp/boot.oatdump.txt
	@echo Output in /tmp/boot.oatdump.txt

.PHONY: dump-oat-Calculator
dump-oat-Calculator: $(call art-cache-out,system/app/Calculator.apk.oat) $(TARGET_BOOT_IMG_OUT) $(OATDUMP)
	$(OATDUMP) --oat-file=$< --output=/tmp/Calculator.oatdump.txt
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
	adb remount
	adb shell rm $(ART_NATIVETEST_DIR)/*.oat
	adb shell rm $(ART_NATIVETEST_DIR)/*.art
	adb shell rm $(ART_TEST_DIR)/*.oat
	adb shell rm $(ART_TEST_DIR)/*.art
	adb shell rm $(ART_CACHE_DIR)/*.oat
	adb shell rm $(ART_CACHE_DIR)/*.art
	adb shell rm $(DEXPREOPT_BOOT_JAR_DIR)/*.oat
	adb shell rm $(DEXPREOPT_BOOT_JAR_DIR)/*.art
	adb shell rm system/app/*.oat
	adb shell rm data/run-test/*.oat

########################################################################
# cpplint target

# "mm cpplint-art" to style check art source files
.PHONY: cpplint-art
cpplint-art:
	./art/tools/cpplint.py \
	    --filter=-whitespace/comments,-whitespace/line_length,-build/include,-build/header_guard,-readability/function,-readability/streams,-readability/todo,-runtime/references \
	    $(ANDROID_BUILD_TOP)/art/src/*.h $(ANDROID_BUILD_TOP)/art/src/*.cc

########################################################################

include $(call all-makefiles-under,$(LOCAL_PATH))
