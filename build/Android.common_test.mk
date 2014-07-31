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

ifndef ANDROID_COMMON_TEST_MK
ANDROID_COMMON_TEST_MK = true

include art/build/Android.common_path.mk

# We need to set a define for the nativetest dir so that common_runtime_test will know the right
# path. (The problem is being a 32b test on 64b device, which is still located in nativetest64).
ART_TARGET_CFLAGS += -DART_TARGET_NATIVETEST_DIR=${ART_TARGET_NATIVETEST_DIR}

# List of known broken tests that we won't attempt to execute. The test name must be the full
# rule name such as test-art-host-oat-optimizing-HelloWorld64.
ART_TEST_KNOWN_BROKEN := \
  test-art-host-run-test-gcstress-optimizing-no-prebuild-004-SignalTest32 \
  test-art-host-run-test-gcstress-optimizing-prebuild-004-SignalTest32 \
  test-art-host-run-test-gcstress-optimizing-norelocate-004-SignalTest32 \
  test-art-host-run-test-gcstress-optimizing-relocate-004-SignalTest32 \
  test-art-host-run-test-gcverify-optimizing-no-prebuild-004-SignalTest32 \
  test-art-host-run-test-gcverify-optimizing-prebuild-004-SignalTest32 \
  test-art-host-run-test-gcverify-optimizing-norelocate-004-SignalTest32 \
  test-art-host-run-test-gcverify-optimizing-relocate-004-SignalTest32 \
  test-art-host-run-test-optimizing-no-prebuild-004-SignalTest32 \
  test-art-host-run-test-optimizing-prebuild-004-SignalTest32 \
  test-art-host-run-test-optimizing-norelocate-004-SignalTest32 \
  test-art-host-run-test-optimizing-relocate-004-SignalTest32 \
  test-art-target-run-test-gcstress-optimizing-prebuild-004-SignalTest32 \
  test-art-target-run-test-gcstress-optimizing-norelocate-004-SignalTest32 \
  test-art-target-run-test-gcstress-default-prebuild-004-SignalTest32 \
  test-art-target-run-test-gcstress-default-norelocate-004-SignalTest32 \
  test-art-target-run-test-gcstress-optimizing-relocate-004-SignalTest32 \
  test-art-target-run-test-gcstress-default-relocate-004-SignalTest32 \
  test-art-target-run-test-gcstress-optimizing-no-prebuild-004-SignalTest32 \
  test-art-target-run-test-gcstress-default-no-prebuild-004-SignalTest32

# List of known failing tests that when executed won't cause test execution to not finish.
# The test name must be the full rule name such as test-art-host-oat-optimizing-HelloWorld64.
ART_TEST_KNOWN_FAILING :=

# Keep going after encountering a test failure?
ART_TEST_KEEP_GOING ?= true

# Do you want all tests, even those that are time consuming?
ART_TEST_FULL ?= true

# Do you want optimizing compiler tests run?
ART_TEST_OPTIMIZING ?= $(ART_TEST_FULL)

# Do you want tracing tests run?
ART_TEST_TRACE ?= $(ART_TEST_FULL)

# Do you want tests with GC verification enabled run?
ART_TEST_GC_VERIFY ?= $(ART_TEST_FULL)

# Do you want tests with the GC stress mode enabled run?
ART_TEST_GC_STRESS ?= $(ART_TEST_FULL)

# Do you want run-tests with relocation enabled?
ART_TEST_RUN_TEST_RELOCATE ?= $(ART_TEST_FULL)

# Do you want run-tests with relocation disabled?
ART_TEST_RUN_TEST_NO_RELOCATE ?= $(ART_TEST_FULL)

# Do you want run-tests with prebuild disabled?
ART_TEST_RUN_TEST_NO_PREBUILD ?= $(ART_TEST_FULL)

# Do you want run-tests with prebuild enabled?
ART_TEST_RUN_TEST_PREBUILD ?= true

# Do you want failed tests to have their artifacts cleaned up?
ART_TEST_RUN_TEST_ALWAYS_CLEAN ?= true

# Define the command run on test failure. $(1) is the name of the test. Executed by the shell.
define ART_TEST_FAILED
  ( [ -f $(ART_HOST_TEST_DIR)/skipped/$(1) ] || \
    (mkdir -p $(ART_HOST_TEST_DIR)/failed/ && touch $(ART_HOST_TEST_DIR)/failed/$(1) && \
      echo $(ART_TEST_KNOWN_FAILING) | grep -q $(1) \
        && (echo -e "$(1) \e[91mKNOWN FAILURE\e[0m") \
        || (echo -e "$(1) \e[91mFAILED\e[0m" >&2 )))
endef

# Define the command run on test success. $(1) is the name of the test. Executed by the shell.
# The command checks prints "PASSED" then checks to see if this was a top-level make target (e.g.
# "mm test-art-host-oat-HelloWorld32"), if it was then it does nothing, otherwise it creates a file
# to be printed in the passing test summary.
define ART_TEST_PASSED
  ( echo -e "$(1) \e[92mPASSED\e[0m" && \
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
     && echo -e "$(1) \e[95mRUNNING\e[0m") \
   || ((mkdir -p $(ART_HOST_TEST_DIR)/skipped/ && touch $(ART_HOST_TEST_DIR)/skipped/$(1) \
     && ([ -d $(ART_HOST_TEST_DIR)/failed/ ] \
       && echo -e "$(1) \e[93mSKIPPING DUE TO EARLIER FAILURE\e[0m") \
     || echo -e "$(1) \e[93mSKIPPING BROKEN TEST\e[0m") && false))
endef

# Create a build rule to create the dex file for a test.
# $(1): module prefix, e.g. art-test-dex
# $(2): input test directory in art/test, e.g. HelloWorld
# $(3): target output module path (default module path is used on host)
# $(4): additional dependencies
# $(5): a make variable used to collate target dependencies, e.g ART_TEST_TARGET_OAT_HelloWorld_DEX
# $(6): a make variable used to collate host dependencies, e.g ART_TEST_HOST_OAT_HelloWorld_DEX
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
    include $(BUILD_HOST_DALVIK_JAVA_LIBRARY)
    $(6) := $$(LOCAL_INSTALLED_MODULE)
  endif
endef

endif # ANDROID_COMMON_TEST_MK
