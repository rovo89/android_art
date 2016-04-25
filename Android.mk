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

art_path := $(LOCAL_PATH)

########################################################################
# clean-oat rules
#

include $(art_path)/build/Android.common_path.mk
include $(art_path)/build/Android.oat.mk

# Following the example of build's dont_bother for clean targets.
art_dont_bother := false
ifneq (,$(filter clean-oat%,$(MAKECMDGOALS)))
  art_dont_bother := true
endif

# Don't bother with tests unless there is a test-art*, build-art*, or related target.
art_test_bother := false
ifneq (,$(filter tests test-art% valgrind-test-art% build-art% checkbuild,$(MAKECMDGOALS)))
  art_test_bother := true
endif

.PHONY: clean-oat
clean-oat: clean-oat-host clean-oat-target

.PHONY: clean-oat-host
clean-oat-host:
	find $(OUT_DIR) -name "*.oat" -o -name "*.odex" -o -name "*.art" | xargs rm -f
ifneq ($(TMPDIR),)
	rm -rf $(TMPDIR)/$(USER)/test-*/dalvik-cache/*
	rm -rf $(TMPDIR)/android-data/dalvik-cache/*
else
	rm -rf /tmp/$(USER)/test-*/dalvik-cache/*
	rm -rf /tmp/android-data/dalvik-cache/*
endif

.PHONY: clean-oat-target
clean-oat-target:
	adb root
	adb wait-for-device remount
	adb shell rm -rf $(ART_TARGET_NATIVETEST_DIR)
	adb shell rm -rf $(ART_TARGET_TEST_DIR)
	adb shell rm -rf $(ART_TARGET_DALVIK_CACHE_DIR)/*/*
	adb shell rm -rf $(DEXPREOPT_BOOT_JAR_DIR)/$(DEX2OAT_TARGET_ARCH)
	adb shell rm -rf system/app/$(DEX2OAT_TARGET_ARCH)
ifdef TARGET_2ND_ARCH
	adb shell rm -rf $(DEXPREOPT_BOOT_JAR_DIR)/$($(TARGET_2ND_ARCH_VAR_PREFIX)DEX2OAT_TARGET_ARCH)
	adb shell rm -rf system/app/$($(TARGET_2ND_ARCH_VAR_PREFIX)DEX2OAT_TARGET_ARCH)
endif
	adb shell rm -rf data/run-test/test-*/dalvik-cache/*

ifneq ($(art_dont_bother),true)

########################################################################
# cpplint rules to style check art source files

include $(art_path)/build/Android.cpplint.mk

########################################################################
# product rules

include $(art_path)/runtime/Android.mk
include $(art_path)/runtime/simulator/Android.mk
include $(art_path)/compiler/Android.mk
include $(art_path)/dexdump/Android.mk
include $(art_path)/dexlist/Android.mk
include $(art_path)/dex2oat/Android.mk
include $(art_path)/disassembler/Android.mk
include $(art_path)/oatdump/Android.mk
include $(art_path)/imgdiag/Android.mk
include $(art_path)/patchoat/Android.mk
include $(art_path)/profman/Android.mk
include $(art_path)/dalvikvm/Android.mk
include $(art_path)/tools/Android.mk
include $(art_path)/tools/ahat/Android.mk
include $(art_path)/tools/dexfuzz/Android.mk
include $(art_path)/tools/dmtracedump/Android.mk
include $(art_path)/sigchainlib/Android.mk
include $(art_path)/libart_fake/Android.mk


# ART_HOST_DEPENDENCIES depends on Android.executable.mk above for ART_HOST_EXECUTABLES
ART_HOST_DEPENDENCIES := \
	$(ART_HOST_EXECUTABLES) \
	$(HOST_OUT_JAVA_LIBRARIES)/core-libart-hostdex.jar \
	$(HOST_OUT_JAVA_LIBRARIES)/core-oj-hostdex.jar \
	$(ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION) \
	$(ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdk$(ART_HOST_SHLIB_EXTENSION) \
	$(ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdkjvm$(ART_HOST_SHLIB_EXTENSION)
ART_TARGET_DEPENDENCIES := \
	$(ART_TARGET_EXECUTABLES) \
	$(TARGET_OUT_JAVA_LIBRARIES)/core-libart.jar \
	$(TARGET_OUT_JAVA_LIBRARIES)/core-oj.jar \
	$(TARGET_OUT_SHARED_LIBRARIES)/libjavacore.so \
	$(TARGET_OUT_SHARED_LIBRARIES)/libopenjdk.so \
	$(TARGET_OUT_SHARED_LIBRARIES)/libopenjdkjvm.so
ifdef TARGET_2ND_ARCH
ART_TARGET_DEPENDENCIES += $(2ND_TARGET_OUT_SHARED_LIBRARIES)/libjavacore.so
ART_TARGET_DEPENDENCIES += $(2ND_TARGET_OUT_SHARED_LIBRARIES)/libopenjdk.so
ART_TARGET_DEPENDENCIES += $(2ND_TARGET_OUT_SHARED_LIBRARIES)/libopenjdkjvm.so
endif
ifdef HOST_2ND_ARCH
ART_HOST_DEPENDENCIES += $(2ND_HOST_OUT_SHARED_LIBRARIES)/libjavacore.so
ART_HOST_DEPENDENCIES += $(2ND_HOST_OUT_SHARED_LIBRARIES)/libopenjdk.so
ART_HOST_DEPENDENCIES += $(2ND_HOST_OUT_SHARED_LIBRARIES)/libopenjdkjvm.so
endif

########################################################################
# test rules

ifeq ($(art_test_bother),true)

# All the dependencies that must be built ahead of sync-ing them onto the target device.
TEST_ART_TARGET_SYNC_DEPS :=

include $(art_path)/build/Android.common_test.mk
include $(art_path)/build/Android.gtest.mk
include $(art_path)/test/Android.run-test.mk
include $(art_path)/benchmark/Android.mk

TEST_ART_ADB_ROOT_AND_REMOUNT := \
    (adb root && \
     adb wait-for-device remount && \
     ((adb shell touch /system/testfile && \
       (adb shell rm /system/testfile || true)) || \
      (adb disable-verity && \
       adb reboot && \
       adb wait-for-device root && \
       adb wait-for-device remount)))

# Sync test files to the target, depends upon all things that must be pushed to the target.
.PHONY: test-art-target-sync
# Check if we need to sync. In case ART_TEST_ANDROID_ROOT is not empty,
# the code below uses 'adb push' instead of 'adb sync', which does not
# check if the files on the device have changed.
ifneq ($(ART_TEST_NO_SYNC),true)
ifeq ($(ART_TEST_ANDROID_ROOT),)
test-art-target-sync: $(TEST_ART_TARGET_SYNC_DEPS)
	$(TEST_ART_ADB_ROOT_AND_REMOUNT)
	adb sync
else
test-art-target-sync: $(TEST_ART_TARGET_SYNC_DEPS)
	$(TEST_ART_ADB_ROOT_AND_REMOUNT)
	adb wait-for-device push $(ANDROID_PRODUCT_OUT)/system $(ART_TEST_ANDROID_ROOT)
# Push the contents of the `data` dir into `/data` on the device.  If
# `/data` already exists on the device, it is not overwritten, but its
# contents are updated.
	adb push $(ANDROID_PRODUCT_OUT)/data /
endif
endif

# "mm test-art" to build and run all tests on host and device
.PHONY: test-art
test-art: test-art-host test-art-target
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-gtest
test-art-gtest: test-art-host-gtest test-art-target-gtest
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-run-test
test-art-run-test: test-art-host-run-test test-art-target-run-test
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

########################################################################
# host test rules

VIXL_TEST_DEPENDENCY :=
# We can only run the vixl tests on 64-bit hosts (vixl testing issue) when its a
# top-level build (to declare the vixl test rule).
ifneq ($(HOST_PREFER_32_BIT),true)
ifeq ($(ONE_SHOT_MAKEFILE),)
VIXL_TEST_DEPENDENCY := run-vixl-tests
endif
endif

.PHONY: test-art-host-vixl
test-art-host-vixl: $(VIXL_TEST_DEPENDENCY)

# "mm test-art-host" to build and run all host tests.
.PHONY: test-art-host
test-art-host: test-art-host-gtest test-art-host-run-test \
               test-art-host-vixl test-art-host-dexdump
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# All host tests that run solely with the default compiler.
.PHONY: test-art-host-default
test-art-host-default: test-art-host-run-test-default
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# All host tests that run solely with the optimizing compiler.
.PHONY: test-art-host-optimizing
test-art-host-optimizing: test-art-host-run-test-optimizing
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# All host tests that run solely on the interpreter.
.PHONY: test-art-host-interpreter
test-art-host-interpreter: test-art-host-run-test-interpreter
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# All host tests that run solely on the jit.
.PHONY: test-art-host-jit
test-art-host-jit: test-art-host-run-test-jit
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# Primary host architecture variants:
.PHONY: test-art-host$(ART_PHONY_TEST_HOST_SUFFIX)
test-art-host$(ART_PHONY_TEST_HOST_SUFFIX): test-art-host-gtest$(ART_PHONY_TEST_HOST_SUFFIX) \
    test-art-host-run-test$(ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-host-default$(ART_PHONY_TEST_HOST_SUFFIX)
test-art-host-default$(ART_PHONY_TEST_HOST_SUFFIX): test-art-host-run-test-default$(ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-host-optimizing$(ART_PHONY_TEST_HOST_SUFFIX)
test-art-host-optimizing$(ART_PHONY_TEST_HOST_SUFFIX): test-art-host-run-test-optimizing$(ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-host-interpreter$(ART_PHONY_TEST_HOST_SUFFIX)
test-art-host-interpreter$(ART_PHONY_TEST_HOST_SUFFIX): test-art-host-run-test-interpreter$(ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-host-jit$(ART_PHONY_TEST_HOST_SUFFIX)
test-art-host-jit$(ART_PHONY_TEST_HOST_SUFFIX): test-art-host-run-test-jit$(ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# Secondary host architecture variants:
ifneq ($(HOST_PREFER_32_BIT),true)
.PHONY: test-art-host$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
test-art-host$(2ND_ART_PHONY_TEST_HOST_SUFFIX): test-art-host-gtest$(2ND_ART_PHONY_TEST_HOST_SUFFIX) \
    test-art-host-run-test$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-host-default$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
test-art-host-default$(2ND_ART_PHONY_TEST_HOST_SUFFIX): test-art-host-run-test-default$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-host-optimizing$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
test-art-host-optimizing$(2ND_ART_PHONY_TEST_HOST_SUFFIX): test-art-host-run-test-optimizing$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-host-interpreter$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
test-art-host-interpreter$(2ND_ART_PHONY_TEST_HOST_SUFFIX): test-art-host-run-test-interpreter$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-host-jit$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
test-art-host-jit$(2ND_ART_PHONY_TEST_HOST_SUFFIX): test-art-host-run-test-jit$(2ND_ART_PHONY_TEST_HOST_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)
endif

# Dexdump/list regression test.
.PHONY: test-art-host-dexdump
test-art-host-dexdump: $(addprefix $(HOST_OUT_EXECUTABLES)/, dexdump2 dexlist)
	ANDROID_HOST_OUT=$(realpath $(HOST_OUT)) art/test/dexdump/run-all-tests

# Valgrind.
.PHONY: valgrind-test-art-host
valgrind-test-art-host: valgrind-test-art-host-gtest
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: valgrind-test-art-host32
valgrind-test-art-host32: valgrind-test-art-host-gtest32
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: valgrind-test-art-host64
valgrind-test-art-host64: valgrind-test-art-host-gtest64
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

########################################################################
# target test rules

# "mm test-art-target" to build and run all target tests.
.PHONY: test-art-target
test-art-target: test-art-target-gtest test-art-target-run-test
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# All target tests that run solely with the default compiler.
.PHONY: test-art-target-default
test-art-target-default: test-art-target-run-test-default
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# All target tests that run solely with the optimizing compiler.
.PHONY: test-art-target-optimizing
test-art-target-optimizing: test-art-target-run-test-optimizing
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# All target tests that run solely on the interpreter.
.PHONY: test-art-target-interpreter
test-art-target-interpreter: test-art-target-run-test-interpreter
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# All target tests that run solely on the jit.
.PHONY: test-art-target-jit
test-art-target-jit: test-art-target-run-test-jit
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# Primary target architecture variants:
.PHONY: test-art-target$(ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target$(ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-gtest$(ART_PHONY_TEST_TARGET_SUFFIX) \
    test-art-target-run-test$(ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-target-default$(ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-default$(ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-run-test-default$(ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-target-optimizing$(ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-optimizing$(ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-run-test-optimizing$(ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-target-interpreter$(ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-interpreter$(ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-run-test-interpreter$(ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-target-jit$(ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-jit$(ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-run-test-jit$(ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

# Secondary target architecture variants:
ifdef TARGET_2ND_ARCH
.PHONY: test-art-target$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target$(2ND_ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-gtest$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) \
    test-art-target-run-test$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-target-default$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-default$(2ND_ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-run-test-default$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-target-optimizing$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-optimizing$(2ND_ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-run-test-optimizing$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-target-interpreter$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-interpreter$(2ND_ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-run-test-interpreter$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)

.PHONY: test-art-target-jit$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-jit$(2ND_ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-run-test-jit$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
	$(hide) $(call ART_TEST_PREREQ_FINISHED,$@)
endif

endif  # art_test_bother

########################################################################
# oat-target and oat-target-sync rules

OAT_TARGET_RULES :=

# $(1): input jar or apk target location
define declare-oat-target-target
OUT_OAT_FILE := $(PRODUCT_OUT)/$(basename $(1)).odex

ifeq ($(ONE_SHOT_MAKEFILE),)
# ONE_SHOT_MAKEFILE is empty for a top level build and we don't want
# to define the oat-target-* rules there because they will conflict
# with the build/core/dex_preopt.mk defined rules.
.PHONY: oat-target-$(1)
oat-target-$(1):

else
.PHONY: oat-target-$(1)
oat-target-$(1): $$(OUT_OAT_FILE)

$$(OUT_OAT_FILE): $(PRODUCT_OUT)/$(1) $(DEFAULT_DEX_PREOPT_BUILT_IMAGE) $(DEX2OAT_DEPENDENCY)
	@mkdir -p $$(dir $$@)
	$(DEX2OAT) --runtime-arg -Xms$(DEX2OAT_XMS) --runtime-arg -Xmx$(DEX2OAT_XMX) \
		--boot-image=$(DEFAULT_DEX_PREOPT_BUILT_IMAGE) --dex-file=$(PRODUCT_OUT)/$(1) \
		--dex-location=/$(1) --oat-file=$$@ \
		--instruction-set=$(DEX2OAT_TARGET_ARCH) \
		--instruction-set-variant=$(DEX2OAT_TARGET_CPU_VARIANT) \
		--instruction-set-features=$(DEX2OAT_TARGET_INSTRUCTION_SET_FEATURES) \
		--android-root=$(PRODUCT_OUT)/system --include-patch-information \
		--runtime-arg -Xnorelocate

endif

OAT_TARGET_RULES += oat-target-$(1)
endef

$(foreach file,\
  $(filter-out\
    $(addprefix $(TARGET_OUT_JAVA_LIBRARIES)/,$(addsuffix .jar,$(LIBART_TARGET_BOOT_JARS))),\
    $(wildcard $(TARGET_OUT_APPS)/*.apk) $(wildcard $(TARGET_OUT_JAVA_LIBRARIES)/*.jar)),\
  $(eval $(call declare-oat-target-target,$(subst $(PRODUCT_OUT)/,,$(file)))))

.PHONY: oat-target
oat-target: $(ART_TARGET_DEPENDENCIES) $(DEFAULT_DEX_PREOPT_INSTALLED_IMAGE) $(OAT_TARGET_RULES)

.PHONY: oat-target-sync
oat-target-sync: oat-target
	$(TEST_ART_ADB_ROOT_AND_REMOUNT)
	adb sync

####################################################################################################
# Fake packages to ensure generation of libopenjdkd when one builds with mm/mmm/mmma.
#
# The library is required for starting a runtime in debug mode, but libartd does not depend on it
# (dependency cycle otherwise).
#
# Note: * As the package is phony to create a dependency the package name is irrelevant.
#       * We make MULTILIB explicit to "both," just to state here that we want both libraries on
#         64-bit systems, even if it is the default.

# ART on the host.
ifeq ($(ART_BUILD_HOST_DEBUG),true)
include $(CLEAR_VARS)
LOCAL_MODULE := art-libartd-libopenjdkd-host-dependency
LOCAL_MULTILIB := both
LOCAL_REQUIRED_MODULES := libopenjdkd
LOCAL_IS_HOST_MODULE := true
include $(BUILD_PHONY_PACKAGE)
endif

# ART on the target.
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
include $(CLEAR_VARS)
LOCAL_MODULE := art-libartd-libopenjdkd-target-dependency
LOCAL_MULTILIB := both
LOCAL_REQUIRED_MODULES := libopenjdkd
include $(BUILD_PHONY_PACKAGE)
endif

########################################################################
# "m build-art" for quick minimal build
.PHONY: build-art
build-art: build-art-host build-art-target

.PHONY: build-art-host
build-art-host:   $(HOST_OUT_EXECUTABLES)/art $(ART_HOST_DEPENDENCIES) $(HOST_CORE_IMG_OUTS)

.PHONY: build-art-target
build-art-target: $(TARGET_OUT_EXECUTABLES)/art $(ART_TARGET_DEPENDENCIES) $(TARGET_CORE_IMG_OUTS)

########################################################################
# Rules for building all dependencies for tests.

.PHONY: build-art-host-tests
build-art-host-tests:   build-art-host $(TEST_ART_RUN_TEST_DEPENDENCIES) $(ART_TEST_HOST_RUN_TEST_DEPENDENCIES) $(ART_TEST_HOST_GTEST_DEPENDENCIES) | $(TEST_ART_RUN_TEST_ORDERONLY_DEPENDENCIES)

.PHONY: build-art-target-tests
build-art-target-tests:   build-art-target $(TEST_ART_RUN_TEST_DEPENDENCIES) $(TEST_ART_TARGET_SYNC_DEPS) | $(TEST_ART_RUN_TEST_ORDERONLY_DEPENDENCIES)

########################################################################
# targets to switch back and forth from libdvm to libart

.PHONY: use-art
use-art:
	adb root
	adb wait-for-device shell stop
	adb shell setprop persist.sys.dalvik.vm.lib.2 libart.so
	adb shell start

.PHONY: use-artd
use-artd:
	adb root
	adb wait-for-device shell stop
	adb shell setprop persist.sys.dalvik.vm.lib.2 libartd.so
	adb shell start

.PHONY: use-dalvik
use-dalvik:
	adb root
	adb wait-for-device shell stop
	adb shell setprop persist.sys.dalvik.vm.lib.2 libdvm.so
	adb shell start

.PHONY: use-art-full
use-art-full:
	adb root
	adb wait-for-device shell stop
	adb shell rm -rf $(ART_TARGET_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-filter \"\"
	adb shell setprop dalvik.vm.image-dex2oat-filter \"\"
	adb shell setprop persist.sys.dalvik.vm.lib.2 libart.so
	adb shell setprop dalvik.vm.usejit false
	adb shell start

.PHONY: use-artd-full
use-artd-full:
	adb root
	adb wait-for-device shell stop
	adb shell rm -rf $(ART_TARGET_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-filter \"\"
	adb shell setprop dalvik.vm.image-dex2oat-filter \"\"
	adb shell setprop persist.sys.dalvik.vm.lib.2 libartd.so
	adb shell setprop dalvik.vm.usejit false
	adb shell start

.PHONY: use-art-jit
use-art-jit:
	adb root
	adb wait-for-device shell stop
	adb shell rm -rf $(ART_TARGET_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-filter "verify-at-runtime"
	adb shell setprop dalvik.vm.image-dex2oat-filter "verify-at-runtime"
	adb shell setprop persist.sys.dalvik.vm.lib.2 libart.so
	adb shell setprop dalvik.vm.usejit true
	adb shell start

.PHONY: use-art-interpret-only
use-art-interpret-only:
	adb root
	adb wait-for-device shell stop
	adb shell rm -rf $(ART_TARGET_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-filter "interpret-only"
	adb shell setprop dalvik.vm.image-dex2oat-filter "interpret-only"
	adb shell setprop persist.sys.dalvik.vm.lib.2 libart.so
	adb shell setprop dalvik.vm.usejit false
	adb shell start

.PHONY: use-artd-interpret-only
use-artd-interpret-only:
	adb root
	adb wait-for-device shell stop
	adb shell rm -rf $(ART_TARGET_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-filter "interpret-only"
	adb shell setprop dalvik.vm.image-dex2oat-filter "interpret-only"
	adb shell setprop persist.sys.dalvik.vm.lib.2 libartd.so
	adb shell setprop dalvik.vm.usejit false
	adb shell start

.PHONY: use-art-verify-none
use-art-verify-none:
	adb root
	adb wait-for-device shell stop
	adb shell rm -rf $(ART_TARGET_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-filter "verify-none"
	adb shell setprop dalvik.vm.image-dex2oat-filter "verify-none"
	adb shell setprop persist.sys.dalvik.vm.lib.2 libart.so
	adb shell setprop dalvik.vm.usejit false
	adb shell start

########################################################################

endif # !art_dont_bother

# Clear locally used variables.
art_dont_bother :=
art_test_bother :=
TEST_ART_TARGET_SYNC_DEPS :=

include $(art_path)/runtime/openjdkjvm/Android.mk

# Helper target that depends on boot image creation.
#
# Can be used, for example, to dump initialization failures:
#   m art-boot-image ART_BOOT_IMAGE_EXTRA_ARGS=--dump-init-failures=fails.txt
.PHONY: art-boot-image
art-boot-image: $(DEFAULT_DEX_PREOPT_BUILT_IMAGE_FILENAME)
