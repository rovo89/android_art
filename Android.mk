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
art_build_path := $(art_path)/build
include $(art_build_path)/Android.common.mk

########################################################################
# clean-oat targets
#

# following the example of build's dont_bother for clean targets
ifneq (,$(filter clean-oat,$(MAKECMDGOALS)))
art_dont_bother := true
endif
ifneq (,$(filter clean-oat-host,$(MAKECMDGOALS)))
art_dont_bother := true
endif
ifneq (,$(filter clean-oat-target,$(MAKECMDGOALS)))
art_dont_bother := true
endif

.PHONY: clean-oat
clean-oat: clean-oat-host clean-oat-target

.PHONY: clean-oat-host
clean-oat-host:
	rm -f $(ART_NATIVETEST_OUT)/*.odex
	rm -f $(ART_NATIVETEST_OUT)/*.oat
	rm -f $(ART_NATIVETEST_OUT)/*.art
	rm -f $(ART_TEST_OUT)/*.odex
	rm -f $(ART_TEST_OUT)/*.oat
	rm -f $(ART_TEST_OUT)/*.art
	rm -f $(HOST_OUT_JAVA_LIBRARIES)/*.odex
	rm -f $(HOST_OUT_JAVA_LIBRARIES)/*.oat
	rm -f $(HOST_OUT_JAVA_LIBRARIES)/*.art
	rm -f $(TARGET_OUT_JAVA_LIBRARIES)/*.odex
	rm -f $(TARGET_OUT_JAVA_LIBRARIES)/*.oat
	rm -f $(TARGET_OUT_JAVA_LIBRARIES)/*.art
	rm -f $(DEXPREOPT_PRODUCT_DIR_FULL_PATH)/$(DEXPREOPT_BOOT_JAR_DIR)/*.oat
	rm -f $(DEXPREOPT_PRODUCT_DIR_FULL_PATH)/$(DEXPREOPT_BOOT_JAR_DIR)/*.art
	rm -f $(TARGET_OUT_UNSTRIPPED)/system/framework/*.odex
	rm -f $(TARGET_OUT_UNSTRIPPED)/system/framework/*.oat
	rm -f $(TARGET_OUT_APPS)/*.odex
	rm -f $(TARGET_OUT_INTERMEDIATES)/JAVA_LIBRARIES/*_intermediates/javalib.odex
	rm -f $(TARGET_OUT_INTERMEDIATES)/APPS/*_intermediates/*.odex
ifdef TARGET_2ND_ARCH
	rm -f $(2ND_TARGET_OUT_INTERMEDIATES)/JAVA_LIBRARIES/*_intermediates/javalib.odex
	rm -f $(2ND_TARGET_OUT_INTERMEDIATES)/APPS/*_intermediates/*.odex
endif
	rm -rf /tmp/$(USER)/test-*/dalvik-cache/*
	rm -rf /tmp/android-data/dalvik-cache/*

.PHONY: clean-oat-target
clean-oat-target:
	adb remount
	adb shell rm -f $(ART_NATIVETEST_DIR)/*.odex
	adb shell rm -f $(ART_NATIVETEST_DIR)/*.oat
	adb shell rm -f $(ART_NATIVETEST_DIR)/*.art
	adb shell rm -f $(ART_TEST_DIR)/*.odex
	adb shell rm -f $(ART_TEST_DIR)/*.oat
	adb shell rm -f $(ART_TEST_DIR)/*.art
ifdef TARGET_2ND_ARCH
	adb shell rm -f $(2ND_ART_NATIVETEST_DIR)/*.odex
	adb shell rm -f $(2ND_ART_NATIVETEST_DIR)/*.oat
	adb shell rm -f $(2ND_ART_NATIVETEST_DIR)/*.art
	adb shell rm -f $(2ND_ART_TEST_DIR)/*.odex
	adb shell rm -f $(2ND_ART_TEST_DIR)/*.oat
	adb shell rm -f $(2ND_ART_TEST_DIR)/*.art
endif
	adb shell rm -rf $(ART_DALVIK_CACHE_DIR)/*
	adb shell rm -f $(DEXPREOPT_BOOT_JAR_DIR)/*.oat
	adb shell rm -f $(DEXPREOPT_BOOT_JAR_DIR)/*.art
	adb shell rm -f system/app/*.odex
	adb shell rm -rf data/run-test/test-*/dalvik-cache/*

ifneq ($(art_dont_bother),true)

########################################################################
# product targets
include $(art_path)/runtime/Android.mk
include $(art_path)/compiler/Android.mk
include $(art_path)/dex2oat/Android.mk
include $(art_path)/disassembler/Android.mk
include $(art_path)/oatdump/Android.mk
include $(art_path)/dalvikvm/Android.mk
include $(art_path)/tools/Android.mk
include $(art_build_path)/Android.oat.mk



# ART_HOST_DEPENDENCIES depends on Android.executable.mk above for ART_HOST_EXECUTABLES
ART_HOST_DEPENDENCIES := $(ART_HOST_EXECUTABLES) $(HOST_OUT_JAVA_LIBRARIES)/core-libart-hostdex.jar
ART_HOST_DEPENDENCIES += $(HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION)
ART_TARGET_DEPENDENCIES := $(ART_TARGET_EXECUTABLES) $(TARGET_OUT_JAVA_LIBRARIES)/core-libart.jar $(TARGET_OUT_SHARED_LIBRARIES)/libjavacore.so

########################################################################
# test targets

include $(art_path)/test/Android.mk
include $(art_build_path)/Android.gtest.mk

$(eval $(call combine-art-multi-target-var,ART_TARGET_GTEST_TARGETS))
$(eval $(call combine-art-multi-target-var,ART_TARGET_GTEST_EXECUTABLES))

# The ART_*_TEST_DEPENDENCIES definitions:
# - depend on Android.oattest.mk above for ART_TEST_*_DEX_FILES
# - depend on Android.gtest.mk above for ART_*_GTEST_EXECUTABLES
ART_HOST_TEST_DEPENDENCIES   := $(ART_HOST_DEPENDENCIES)   $(ART_HOST_GTEST_EXECUTABLES)   $(ART_TEST_HOST_DEX_FILES)   $(HOST_CORE_IMG_OUT)

define declare-art-target-test-dependencies-var
ART_TARGET_TEST_DEPENDENCIES$(1) := $(ART_TARGET_DEPENDENCIES) $(ART_TARGET_GTEST_EXECUTABLES$(1)) $(ART_TEST_TARGET_DEX_FILES$(1)) $(TARGET_CORE_IMG_OUT$(1))
endef
$(eval $(call call-art-multi-target-var,declare-art-target-test-dependencies-var,ART_TARGET_TEST_DEPENDENCIES))

include $(art_build_path)/Android.libarttest.mk

# "mm test-art" to build and run all tests on host and device
.PHONY: test-art
test-art: test-art-host test-art-target
	@echo test-art PASSED

.PHONY: test-art-gtest
test-art-gtest: test-art-host-gtest test-art-target-gtest
	@echo test-art-gtest PASSED

.PHONY: test-art-oat
test-art-oat: test-art-host-oat test-art-target-oat
	@echo test-art-oat PASSED

.PHONY: test-art-run-test
test-art-run-test: test-art-host-run-test test-art-target-run-test
	@echo test-art-run-test PASSED

########################################################################
# host test targets

.PHONY: test-art-host-vixl
VIXL_TEST_DEPENDENCY :=
# We can only run the vixl tests on 64-bit hosts (vixl testing issue) when its a
# top-level build (to declare the vixl test rule).
ifneq ($(HOST_IS_64_BIT),)
ifeq ($(ONE_SHOT_MAKEFILE),)
VIXL_TEST_DEPENDENCY := run-vixl-tests
endif
endif

test-art-host-vixl: $(VIXL_TEST_DEPENDENCY)

# "mm test-art-host" to build and run all host tests
.PHONY: test-art-host
test-art-host: test-art-host-gtest test-art-host-oat test-art-host-run-test test-art-host-vixl
	@echo test-art-host PASSED

.PHONY: test-art-host-interpreter
test-art-host-interpreter: test-art-host-oat-interpreter test-art-host-run-test-interpreter
	@echo test-art-host-interpreter PASSED

.PHONY: test-art-host-dependencies
test-art-host-dependencies: $(ART_HOST_TEST_DEPENDENCIES) $(HOST_OUT_SHARED_LIBRARIES)/libarttest$(ART_HOST_SHLIB_EXTENSION) $(HOST_CORE_DEX_LOCATIONS)

.PHONY: test-art-host-gtest
test-art-host-gtest: $(ART_HOST_GTEST_TARGETS)
	@echo test-art-host-gtest PASSED

# "mm valgrind-test-art-host-gtest" to build and run the host gtests under valgrind.
.PHONY: valgrind-test-art-host-gtest
valgrind-test-art-host-gtest: $(ART_HOST_VALGRIND_GTEST_TARGETS)
	@echo valgrind-test-art-host-gtest PASSED

.PHONY: test-art-host-oat-default
test-art-host-oat-default: $(ART_TEST_HOST_OAT_DEFAULT_TARGETS)
	@echo test-art-host-oat-default PASSED

.PHONY: test-art-host-oat-interpreter
test-art-host-oat-interpreter: $(ART_TEST_HOST_OAT_INTERPRETER_TARGETS)
	@echo test-art-host-oat-interpreter PASSED

.PHONY: test-art-host-oat
test-art-host-oat: test-art-host-oat-default test-art-host-oat-interpreter
	@echo test-art-host-oat PASSED

define declare-test-art-host-run-test
.PHONY: test-art-host-run-test-default-$(1)
test-art-host-run-test-default-$(1): test-art-host-dependencies $(DX) $(HOST_OUT_EXECUTABLES)/jasmin
	DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) art/test/run-test $(DALVIKVM_FLAGS) --host $(1)
	@echo test-art-host-run-test-default-$(1) PASSED

TEST_ART_HOST_RUN_TEST_DEFAULT_TARGETS += test-art-host-run-test-default-$(1)

.PHONY: test-art-host-run-test-interpreter-$(1)
test-art-host-run-test-interpreter-$(1): test-art-host-dependencies $(DX) $(HOST_OUT_EXECUTABLES)/jasmin
	DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) art/test/run-test $(DALVIKVM_FLAGS) --host --interpreter $(1)
	@echo test-art-host-run-test-interpreter-$(1) PASSED

TEST_ART_HOST_RUN_TEST_INTERPRETER_TARGETS += test-art-host-run-test-interpreter-$(1)

.PHONY: test-art-host-run-test-$(1)
test-art-host-run-test-$(1): test-art-host-run-test-default-$(1) test-art-host-run-test-interpreter-$(1)

endef

$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call declare-test-art-host-run-test,$(test))))

.PHONY: test-art-host-run-test-default
test-art-host-run-test-default: $(TEST_ART_HOST_RUN_TEST_DEFAULT_TARGETS)
	@echo test-art-host-run-test-default PASSED

.PHONY: test-art-host-run-test-interpreter
test-art-host-run-test-interpreter: $(TEST_ART_HOST_RUN_TEST_INTERPRETER_TARGETS)
	@echo test-art-host-run-test-interpreter PASSED

.PHONY: test-art-host-run-test
test-art-host-run-test: test-art-host-run-test-default test-art-host-run-test-interpreter
	@echo test-art-host-run-test PASSED

########################################################################
# target test targets

# "mm test-art-target" to build and run all target tests
define declare-test-art-target
.PHONY: test-art-target$(1)
test-art-target$(1): test-art-target-gtest$(1) test-art-target-oat$(1) test-art-target-run-test$(1)
	@echo test-art-target$(1) PASSED
endef
$(eval $(call call-art-multi-target-rule,declare-test-art-target,test-art-target))


define declare-test-art-target-dependencies
.PHONY: test-art-target-dependencies$(1)
test-art-target-dependencies$(1): $(ART_TARGET_TEST_DEPENDENCIES$(1)) $(ART_TEST_OUT)/libarttest.so
endef
$(eval $(call call-art-multi-target-rule,declare-test-art-target-dependencies,test-art-target-dependencies))


.PHONY: test-art-target-sync
test-art-target-sync: test-art-target-dependencies$(ART_PHONY_TEST_TARGET_SUFFIX) test-art-target-dependencies$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
	adb remount
	adb sync
	adb shell mkdir -p $(ART_TEST_DIR)


define declare-test-art-target-gtest
.PHONY: test-art-target-gtest$(1)
test-art-target-gtest$(1): $(ART_TARGET_GTEST_TARGETS$(1))
	@echo test-art-target-gtest$(1) PASSED
endef
$(eval $(call call-art-multi-target-rule,declare-test-art-target-gtest,test-art-target-gtest))


define declare-test-art-target-oat
.PHONY: test-art-target-oat$(1)
test-art-target-oat$(1): $(ART_TEST_TARGET_OAT_TARGETS$(1))
	@echo test-art-target-oat$(1) PASSED
endef
$(eval $(call call-art-multi-target-rule,declare-test-art-target-oat,test-art-target-oat))


define declare-test-art-target-run-test-impl
$(2)run_test_$(1) :=
ifeq ($($(2)ART_PHONY_TEST_TARGET_SUFFIX),64)
 $(2)run_test_$(1) := --64
endif
.PHONY: test-art-target-run-test-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-run-test-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX): test-art-target-sync $(DX) $(HOST_OUT_EXECUTABLES)/jasmin
	DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) art/test/run-test $(DALVIKVM_FLAGS) $$($(2)run_test_$(1)) $(1)
	@echo test-art-target-run-test-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX) PASSED
endef

define declare-test-art-target-run-test

  ifdef TARGET_2ND_ARCH
    $(call declare-test-art-target-run-test-impl,$(1),2ND_)
    
    TEST_ART_TARGET_RUN_TEST_TARGETS$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) += test-art-target-run-test-$(1)$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)

    ifneq ($(ART_PHONY_TEST_TARGET_SUFFIX),)
      # Link primary to non-suffix
test-art-target-run-test-$(1): test-art-target-run-test-$(1)$(ART_PHONY_TEST_TARGET_SUFFIX)
    endif
  endif
  $(call declare-test-art-target-run-test-impl,$(1),)

  TEST_ART_TARGET_RUN_TEST_TARGETS$(ART_PHONY_TEST_TARGET_SUFFIX) += test-art-target-run-test-$(1)$(ART_PHONY_TEST_TARGET_SUFFIX)

test-art-run-test-$(1): test-art-host-run-test-$(1) test-art-target-run-test-$(1)

endef

$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call declare-test-art-target-run-test,$(test))))


define declare-test-art-target-run-test
.PHONY: test-art-target-run-test$(1)
test-art-target-run-test$(1): $(TEST_ART_TARGET_RUN_TEST_TARGETS$(1))
	@echo test-art-target-run-test$(1) PASSED
endef
$(eval $(call call-art-multi-target-rule,declare-test-art-target-run-test,test-art-target-run-test))


########################################################################
# oat-target and oat-target-sync targets

OAT_TARGET_TARGETS :=

# $(1): input jar or apk target location
define declare-oat-target-target
ifneq (,$(filter $(1),$(addprefix system/app/,$(addsuffix .apk,$(PRODUCT_DEX_PREOPT_PACKAGES_IN_DATA)))))
OUT_OAT_FILE := $(call dalvik-cache-out,$(1)/classes.dex)
else
OUT_OAT_FILE := $(PRODUCT_OUT)/$(basename $(1)).odex
endif

ifeq ($(ONE_SHOT_MAKEFILE),)
# ONE_SHOT_MAKEFILE is empty for a top level build and we don't want
# to define the oat-target-* rules there because they will conflict
# with the build/core/dex_preopt.mk defined rules.
.PHONY: oat-target-$(1)
oat-target-$(1):

else
.PHONY: oat-target-$(1)
oat-target-$(1): $$(OUT_OAT_FILE)

$$(OUT_OAT_FILE): $(PRODUCT_OUT)/$(1) $(DEFAULT_DEX_PREOPT_BUILT_IMAGE) $(DEX2OATD_DEPENDENCY)
	@mkdir -p $$(dir $$@)
	$(DEX2OATD) --runtime-arg -Xms64m --runtime-arg -Xmx64m \
		--boot-image=$(DEFAULT_DEX_PREOPT_BUILT_IMAGE) --dex-file=$(PRODUCT_OUT)/$(1) \
		--dex-location=/$(1) --oat-file=$$@ \
		--instruction-set=$(DEX2OAT_TARGET_ARCH) \
		--instruction-set-features=$(DEX2OAT_TARGET_INSTRUCTION_SET_FEATURES) \
		--android-root=$(PRODUCT_OUT)/system

endif

OAT_TARGET_TARGETS += oat-target-$(1)
endef

$(foreach file,\
  $(filter-out\
    $(addprefix $(TARGET_OUT_JAVA_LIBRARIES)/,$(addsuffix .jar,$(LIBART_TARGET_BOOT_JARS))),\
    $(wildcard $(TARGET_OUT_APPS)/*.apk) $(wildcard $(TARGET_OUT_JAVA_LIBRARIES)/*.jar)),\
  $(eval $(call declare-oat-target-target,$(subst $(PRODUCT_OUT)/,,$(file)))))

.PHONY: oat-target
oat-target: $(ART_TARGET_DEPENDENCIES) $(DEFAULT_DEX_PREOPT_INSTALLED_IMAGE) $(OAT_TARGET_TARGETS)

.PHONY: oat-target-sync
oat-target-sync: oat-target
	adb remount
	adb sync

########################################################################
# "m build-art" for quick minimal build
.PHONY: build-art
build-art: build-art-host build-art-target

.PHONY: build-art-host
build-art-host:   $(ART_HOST_EXECUTABLES)   $(ART_HOST_GTEST_EXECUTABLES)   $(HOST_CORE_IMG_OUT)   $(HOST_OUT)/lib/libjavacore.so

.PHONY: build-art-target
build-art-target: $(ART_TARGET_EXECUTABLES) $(ART_TARGET_GTEST_EXECUTABLES) $(TARGET_CORE_IMG_OUT) $(TARGET_OUT)/lib/libjavacore.so

########################################################################
# "m art-host" for just building the files needed to run the art script
.PHONY: art-host
art-host:   $(HOST_OUT_EXECUTABLES)/art $(HOST_OUT)/bin/dalvikvm $(HOST_OUT)/lib/libart.so $(HOST_OUT)/bin/dex2oat $(HOST_OUT_JAVA_LIBRARIES)/core.art $(HOST_OUT)/lib/libjavacore.so

.PHONY: art-host-debug
art-host-debug:   art-host $(HOST_OUT)/lib/libartd.so $(HOST_OUT)/bin/dex2oatd

########################################################################
# oatdump targets

ART_DUMP_OAT_PATH ?= $(OUT_DIR)

OATDUMP := $(HOST_OUT_EXECUTABLES)/oatdump$(HOST_EXECUTABLE_SUFFIX)
OATDUMPD := $(HOST_OUT_EXECUTABLES)/oatdumpd$(HOST_EXECUTABLE_SUFFIX)
# TODO: for now, override with debug version for better error reporting
OATDUMP := $(OATDUMPD)

.PHONY: dump-oat
dump-oat: dump-oat-core dump-oat-boot

.PHONY: dump-oat-core
dump-oat-core: dump-oat-core-host dump-oat-core-target

.PHONY: dump-oat-core-host
ifeq ($(ART_BUILD_HOST),true)
dump-oat-core-host: $(HOST_CORE_IMG_OUT) $(OATDUMP)
	$(OATDUMP) --image=$(HOST_CORE_IMG_OUT) --output=$(ART_DUMP_OAT_PATH)/core.host.oatdump.txt
	@echo Output in $(ART_DUMP_OAT_PATH)/core.host.oatdump.txt
endif

.PHONY: dump-oat-core-target
ifeq ($(ART_BUILD_TARGET),true)
dump-oat-core-target: $(TARGET_CORE_IMG_OUT) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_CORE_IMG_OUT) --output=$(ART_DUMP_OAT_PATH)/core.target.oatdump.txt
	@echo Output in $(ART_DUMP_OAT_PATH)/core.target.oatdump.txt
endif

.PHONY: dump-oat-boot
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
dump-oat-boot: $(DEFAULT_DEX_PREOPT_BUILT_IMAGE) $(OATDUMP)
	$(OATDUMP) --image=$(DEFAULT_DEX_PREOPT_BUILT_IMAGE) --output=$(ART_DUMP_OAT_PATH)/boot.oatdump.txt
	@echo Output in $(ART_DUMP_OAT_PATH)/boot.oatdump.txt
endif

.PHONY: dump-oat-Calculator
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
dump-oat-Calculator: $(TARGET_OUT_APPS)/Calculator.odex $(DEFAULT_DEX_PREOPT_BUILT_IMAGE) $(OATDUMP)
	$(OATDUMP) --oat-file=$< --output=$(ART_DUMP_OAT_PATH)/Calculator.oatdump.txt
	@echo Output in $(ART_DUMP_OAT_PATH)/Calculator.oatdump.txt
endif

########################################################################
# cpplint targets to style check art source files

include $(art_build_path)/Android.cpplint.mk

########################################################################
# targets to switch back and forth from libdvm to libart

.PHONY: use-art
use-art:
	adb root && sleep 3
	adb shell stop
	adb shell setprop persist.sys.dalvik.vm.lib.1 libart.so
	adb shell start

.PHONY: use-artd
use-artd:
	adb root && sleep 3
	adb shell stop
	adb shell setprop persist.sys.dalvik.vm.lib.1 libartd.so
	adb shell start

.PHONY: use-dalvik
use-dalvik:
	adb root && sleep 3
	adb shell stop
	adb shell setprop persist.sys.dalvik.vm.lib.1 libdvm.so
	adb shell start

.PHONY: use-art-full
use-art-full:
	adb root && sleep 3
	adb shell stop
	adb shell rm -rf $(ART_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-flags ""
	adb shell setprop dalvik.vm.image-dex2oat-flags ""
	adb shell setprop persist.sys.dalvik.vm.lib.1 libart.so
	adb shell start

.PHONY: use-art-smart
use-art-smart:
	adb root && sleep 3
	adb shell stop
	adb shell rm -rf $(ART_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-flags "--compiler-filter=interpret-only"
	adb shell setprop dalvik.vm.image-dex2oat-flags ""
	adb shell setprop persist.sys.dalvik.vm.lib.1 libart.so
	adb shell start

.PHONY: use-art-interpret-only
use-art-interpret-only:
	adb root && sleep 3
	adb shell stop
	adb shell rm -rf $(ART_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-flags "--compiler-filter=interpret-only"
	adb shell setprop dalvik.vm.image-dex2oat-flags "--compiler-filter=interpret-only"
	adb shell setprop persist.sys.dalvik.vm.lib.1 libart.so
	adb shell start

.PHONY: use-art-verify-none
use-art-verify-none:
	adb root && sleep 3
	adb shell stop
	adb shell rm -rf $(ART_DALVIK_CACHE_DIR)/*
	adb shell setprop dalvik.vm.dex2oat-flags "--compiler-filter=verify-none"
	adb shell setprop dalvik.vm.image-dex2oat-flags "--compiler-filter=verify-none"
	adb shell setprop persist.sys.dalvik.vm.lib.1 libart.so
	adb shell start

########################################################################

endif # !art_dont_bother
