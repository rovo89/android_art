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

ifndef ART_ANDROID_COMMON_TEST_MK
ART_ANDROID_COMMON_TEST_MK = true

include art/build/Android.common_path.mk

# We need to set a define for the nativetest dir so that common_runtime_test will know the right
# path. (The problem is being a 32b test on 64b device, which is still located in nativetest64).
ART_TARGET_CFLAGS += -DART_TARGET_NATIVETEST_DIR=${ART_TARGET_NATIVETEST_DIR}

# List of known broken tests that we won't attempt to execute. The test name must be the full
# rule name such as test-art-host-oat-optimizing-HelloWorld64.
ART_TEST_KNOWN_BROKEN :=

# List of run-tests to skip running in any configuration. This needs to be the full name of the
# run-test such as '457-regs'.
ART_TEST_RUN_TEST_SKIP ?=

# Failing valgrind tests.
# Note: *all* 64b tests involving the runtime do not work currently. b/15170219.

# List of known failing tests that when executed won't cause test execution to not finish.
# The test name must be the full rule name such as test-art-host-oat-optimizing-HelloWorld64.
ART_TEST_KNOWN_FAILING :=

# Keep going after encountering a test failure?
ART_TEST_KEEP_GOING ?= true

# Do you want all tests, even those that are time consuming?
ART_TEST_FULL ?= false

# Do you want run-test to be quieter? run-tests will only show output if they fail.
ART_TEST_QUIET ?= true

# Do you want interpreter tests run?
ART_TEST_INTERPRETER ?= $(ART_TEST_FULL)
ART_TEST_INTERPRETER_ACCESS_CHECKS ?= $(ART_TEST_FULL)

# Do you want JIT tests run?
ART_TEST_JIT ?= $(ART_TEST_FULL)

# Do you want optimizing compiler tests run?
ART_TEST_OPTIMIZING ?= true

# Do we want to test a PIC-compiled core image?
ART_TEST_PIC_IMAGE ?= $(ART_TEST_FULL)

# Do we want to test PIC-compiled tests ("apps")?
ART_TEST_PIC_TEST ?= $(ART_TEST_FULL)

# Do you want tracing tests run?
ART_TEST_TRACE ?= $(ART_TEST_FULL)

# Do you want tracing tests (streaming mode) run?
ART_TEST_TRACE_STREAM ?= $(ART_TEST_FULL)

# Do you want tests with GC verification enabled run?
ART_TEST_GC_VERIFY ?= $(ART_TEST_FULL)

# Do you want tests with the GC stress mode enabled run?
ART_TEST_GC_STRESS ?= $(ART_TEST_FULL)

# Do you want tests with the JNI forcecopy mode enabled run?
ART_TEST_JNI_FORCECOPY ?= $(ART_TEST_FULL)

# Do you want run-tests with relocation disabled run?
ART_TEST_RUN_TEST_NO_RELOCATE ?= $(ART_TEST_FULL)

# Do you want run-tests with prebuilding?
ART_TEST_RUN_TEST_PREBUILD ?= true

# Do you want run-tests with no prebuilding enabled run?
ART_TEST_RUN_TEST_NO_PREBUILD ?= $(ART_TEST_FULL)

# Do you want run-tests without a pregenerated core.art?
ART_TEST_RUN_TEST_NO_IMAGE ?= $(ART_TEST_FULL)

# Do you want run-tests with relocation enabled but patchoat failing?
ART_TEST_RUN_TEST_RELOCATE_NO_PATCHOAT ?= $(ART_TEST_FULL)

# Do you want run-tests without a dex2oat?
ART_TEST_RUN_TEST_NO_DEX2OAT ?= $(ART_TEST_FULL)

# Do you want run-tests with libartd.so?
ART_TEST_RUN_TEST_DEBUG ?= true

# Do you want run-tests with libart.so?
ART_TEST_RUN_TEST_NDEBUG ?= $(ART_TEST_FULL)

# Do you want run-tests with the host/target's second arch?
ART_TEST_RUN_TEST_2ND_ARCH ?= true

# Do you want failed tests to have their artifacts cleaned up?
ART_TEST_RUN_TEST_ALWAYS_CLEAN ?= true

# Do you want run-tests with the --debuggable flag
ART_TEST_RUN_TEST_DEBUGGABLE ?= $(ART_TEST_FULL)

# Do you want to test multi-part boot-image functionality?
ART_TEST_RUN_TEST_MULTI_IMAGE ?= $(ART_TEST_FULL)

# Define the command run on test failure. $(1) is the name of the test. Executed by the shell.
define ART_TEST_FAILED
  ( [ -f $(ART_HOST_TEST_DIR)/skipped/$(1) ] || \
    (mkdir -p $(ART_HOST_TEST_DIR)/failed/ && touch $(ART_HOST_TEST_DIR)/failed/$(1) && \
      echo $(ART_TEST_KNOWN_FAILING) | grep -q $(1) \
        && (echo -e "$(1) \e[91mKNOWN FAILURE\e[0m") \
        || (echo -e "$(1) \e[91mFAILED\e[0m" >&2 )))
endef

ifeq ($(ART_TEST_QUIET),true)
  ART_TEST_ANNOUNCE_PASS := ( true )
  ART_TEST_ANNOUNCE_RUN := ( true )
  ART_TEST_ANNOUNCE_SKIP_FAILURE := ( true )
  ART_TEST_ANNOUNCE_SKIP_BROKEN := ( true )
else
  # Note the use of '=' and not ':=' is intentional since these are actually functions.
  ART_TEST_ANNOUNCE_PASS = ( echo -e "$(1) \e[92mPASSED\e[0m" )
  ART_TEST_ANNOUNCE_RUN = ( echo -e "$(1) \e[95mRUNNING\e[0m")
  ART_TEST_ANNOUNCE_SKIP_FAILURE = ( echo -e "$(1) \e[93mSKIPPING DUE TO EARLIER FAILURE\e[0m" )
  ART_TEST_ANNOUNCE_SKIP_BROKEN = ( echo -e "$(1) \e[93mSKIPPING BROKEN TEST\e[0m" )
endif

# Define the command run on test success. $(1) is the name of the test. Executed by the shell.
# The command checks prints "PASSED" then checks to see if this was a top-level make target (e.g.
# "mm test-art-host-oat-HelloWorld32"), if it was then it does nothing, otherwise it creates a file
# to be printed in the passing test summary.
define ART_TEST_PASSED
  ( $(call ART_TEST_ANNOUNCE_PASS,$(1)) && \
    (echo $(MAKECMDGOALS) | grep -q $(1) || \
      (mkdir -p $(ART_HOST_TEST_DIR)/passed/ && touch $(ART_HOST_TEST_DIR)/passed/$(1))))
endef

# Define the command run on test success of multiple prerequisites. $(1) is the name of the test.
# When the test is a top-level make target then a summary of the ran tests is produced. Executed by
# the shell.
define ART_TEST_PREREQ_FINISHED
  (echo -e "$(1) \e[32mCOMPLETE\e[0m" && \
    (echo $(MAKECMDGOALS) | grep -q -v $(1) || \
      (([ -d $(ART_HOST_TEST_DIR)/passed/ ] \
        && (echo -e "\e[92mPASSING TESTS\e[0m" && ls -1 $(ART_HOST_TEST_DIR)/passed/) \
        || (echo -e "\e[91mNO TESTS PASSED\e[0m")) && \
      ([ -d $(ART_HOST_TEST_DIR)/skipped/ ] \
        && (echo -e "\e[93mSKIPPED TESTS\e[0m" && ls -1 $(ART_HOST_TEST_DIR)/skipped/) \
        || (echo -e "\e[92mNO TESTS SKIPPED\e[0m")) && \
      ([ -d $(ART_HOST_TEST_DIR)/failed/ ] \
        && (echo -e "\e[91mFAILING TESTS\e[0m" >&2 && ls -1 $(ART_HOST_TEST_DIR)/failed/ >&2) \
        || (echo -e "\e[92mNO TESTS FAILED\e[0m")) \
      && ([ ! -d $(ART_HOST_TEST_DIR)/failed/ ] && rm -r $(ART_HOST_TEST_DIR) \
          || (rm -r $(ART_HOST_TEST_DIR) && false)))))
endef

# Define the command executed by the shell ahead of running an art test. $(1) is the name of the
# test.
define ART_TEST_SKIP
  ((echo $(ART_TEST_KNOWN_BROKEN) | grep -q -v $(1) \
     && ([ ! -d $(ART_HOST_TEST_DIR)/failed/ ] || [ $(ART_TEST_KEEP_GOING) = true ])\
     && $(call ART_TEST_ANNOUNCE_RUN,$(1)) ) \
   || ((mkdir -p $(ART_HOST_TEST_DIR)/skipped/ && touch $(ART_HOST_TEST_DIR)/skipped/$(1) \
     && ([ -d $(ART_HOST_TEST_DIR)/failed/ ] \
       && $(call ART_TEST_ANNOUNCE_SKIP_FAILURE,$(1)) ) \
     || $(call ART_TEST_ANNOUNCE_SKIP_BROKEN,$(1)) ) && false))
endef

# Create a build rule to create the dex file for a test.
# $(1): module prefix, e.g. art-test-dex
# $(2): input test directory in art/test, e.g. HelloWorld
# $(3): target output module path (default module path is used on host)
# $(4): additional dependencies
# $(5): a make variable used to collate target dependencies, e.g ART_TEST_TARGET_OAT_HelloWorld_DEX
# $(6): a make variable used to collate host dependencies, e.g ART_TEST_HOST_OAT_HelloWorld_DEX
#
# If the input test directory contains a file called main.list and main.jpp,
# then a multi-dex file is created passing main.list as the --main-dex-list
# argument to dx and main.jpp for Jack.
define build-art-test-dex
  ifeq ($(ART_BUILD_TARGET),true)
    include $(CLEAR_VARS)
    LOCAL_MODULE := $(1)-$(2)
    LOCAL_SRC_FILES := $(call all-java-files-under, $(2))
    LOCAL_NO_STANDARD_LIBRARIES := true
    LOCAL_DEX_PREOPT := false
    LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_test.mk $(4)
    LOCAL_MODULE_TAGS := tests
    LOCAL_JAVA_LIBRARIES := $(TARGET_CORE_JARS)
    LOCAL_MODULE_PATH := $(3)
    LOCAL_DEX_PREOPT_IMAGE_LOCATION := $(TARGET_CORE_IMG_OUT)
    ifneq ($(wildcard $(LOCAL_PATH)/$(2)/main.list),)
      LOCAL_JACK_FLAGS := -D jack.dex.output.policy=minimal-multidex -D jack.preprocessor=true -D jack.preprocessor.file=$(LOCAL_PATH)/$(2)/main.jpp
    endif
    include $(BUILD_JAVA_LIBRARY)
    $(5) := $$(LOCAL_INSTALLED_MODULE)
  endif
  ifeq ($(ART_BUILD_HOST),true)
    include $(CLEAR_VARS)
    LOCAL_MODULE := $(1)-$(2)
    LOCAL_SRC_FILES := $(call all-java-files-under, $(2))
    LOCAL_NO_STANDARD_LIBRARIES := true
    LOCAL_DEX_PREOPT := false
    LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_test.mk $(4)
    LOCAL_JAVA_LIBRARIES := $(HOST_CORE_JARS)
    LOCAL_DEX_PREOPT_IMAGE := $(HOST_CORE_IMG_LOCATION)
    ifneq ($(wildcard $(LOCAL_PATH)/$(2)/main.list),)
      LOCAL_JACK_FLAGS := -D jack.dex.output.policy=minimal-multidex -D jack.preprocessor=true -D jack.preprocessor.file=$(LOCAL_PATH)/$(2)/main.jpp
    endif
    include $(BUILD_HOST_DALVIK_JAVA_LIBRARY)
    $(6) := $$(LOCAL_INSTALLED_MODULE)
  endif
endef

endif # ART_ANDROID_COMMON_TEST_MK
