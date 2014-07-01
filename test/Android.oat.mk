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
LOCAL_PID := $(shell echo $$PPID)

include art/build/Android.common_test.mk

########################################################################

# Subdirectories in art/test which contain dex files used as inputs for oat tests. Declare the
# simplest tests (Main, HelloWorld) first, the rest are alphabetical.
TEST_OAT_DIRECTORIES := \
  Main \
  HelloWorld \
  InterfaceTest \
  JniTest \
  SignalTest \
  NativeAllocations \
  ParallelGC \
  ReferenceMap \
  StackWalk \
  ThreadStress \
  UnsafeTest

# TODO: Enable when the StackWalk2 tests are passing
#  StackWalk2 \

# Create build rules for each dex file recording the dependency.
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call build-art-test-dex,art-oat-test,$(dir), \
  $(ART_TARGET_TEST_OUT),$(LOCAL_PATH)/Android.oat.mk,ART_OAT_TEST_$(dir)_DEX)))

########################################################################

include $(LOCAL_PATH)/Android.libarttest.mk

ART_TEST_TARGET_OAT_DEFAULT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_DEFAULT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_DEFAULT_RULES :=
ART_TEST_TARGET_OAT_OPTIMIZING$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_OPTIMIZING$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_OPTIMIZING_RULES :=
ART_TEST_TARGET_OAT_INTERPRETER$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_INTERPRETER$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_INTERPRETER_RULES :=
ART_TEST_TARGET_OAT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_RULES :=

# We need dex2oat and dalvikvm on the target as well as the core image.
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_EXECUTABLES) $(TARGET_CORE_IMG_OUT) $(2ND_TARGET_CORE_IMG_OUT) $(ART_TARGET_TEST_OUT)/$(TARGET_ARCH)/libarttest.so
ifdef TARGET_2ND_ARCH
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_TEST_OUT)/$(TARGET_2ND_ARCH)/libarttest.so
endif

# Define rule to run an individual oat test on the host. Output from the test is written to the
# host in /tmp/android-data in a directory named after test's rule name (its target) and the parent
# process' PID (ie the PID of make). On failure the output is dumped to the console. To test for
# success on the target device a file is created following a successful test and this is pulled
# onto the host. If the pull fails then the file wasn't created because the test failed.
# $(1): directory - the name of the test we're building such as HelloWorld.
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
# $(3): the target (rule name), e.g. test-art-target-oat-default-HelloWorld64
# $(4): -Xint or undefined - do we want to run with the interpreter or default.
define define-test-art-oat-rule-target
  # Add the test dependencies to test-art-target-sync, which will be a prerequisite for the test
  # to ensure files are pushed to the device.
  TEST_ART_TARGET_SYNC_DEPS += $$(ART_OAT_TEST_$(1)_DEX)

.PHONY: $(3)
$(3): test-art-target-sync
	$(hide) mkdir -p $(ART_HOST_TEST_DIR)/android-data-$$@
	$(hide) echo Running: $$@
	$(hide) adb shell touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$(LOCAL_PID)
	$(hide) adb shell rm $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$(LOCAL_PID)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  adb shell "/system/bin/dalvikvm$($(2)ART_PHONY_TEST_TARGET_SUFFIX) \
	    $(DALVIKVM_FLAGS) $(4) -XXlib:libartd.so -Ximage:$(ART_TARGET_TEST_DIR)/core.art \
	    -classpath $(ART_TARGET_TEST_DIR)/art-oat-test-$(1).jar \
	    -Djava.library.path=$(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH) $(1) \
	      && touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$(LOCAL_PID)" \
	        > $(ART_HOST_TEST_DIR)/android-data-$$@/output.txt 2>&1 && \
	  (adb pull $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$(LOCAL_PID) $(ART_HOST_TEST_DIR)/android-data-$$@ \
	    && $$(call ART_TEST_PASSED,$$@)) \
	    || (([ ! -f $(ART_HOST_TEST_DIR)/android-data-$$@/output.txt ] || \
	         cat $(ART_HOST_TEST_DIR)/android-data-$$@/output.txt) && $$(call ART_TEST_FAILED,$$@))
	$$(hide) (echo $(MAKECMDGOALS) | grep -q $$@ && \
	  echo "run-test run as top-level target, removing test directory $(ART_HOST_TEST_DIR)" && \
	  rm -r $(ART_HOST_TEST_DIR)) || true

endef  # define-test-art-oat-rule-target

# Define rules to run oat tests on the target.
# $(1): directory - the name of the test we're building such as HelloWorld.
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
define define-test-art-oat-rules-target
  # Define a phony rule to run a target oat test using the default compiler.
  default_test_rule := test-art-target-oat-default-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX)
  $(call define-test-art-oat-rule-target,$(1),$(2),$$(default_test_rule),)

  ART_TEST_TARGET_OAT_DEFAULT$$($(2)ART_PHONY_TEST_TARGET_SUFFIX)_RULES += $$(default_test_rule)
  ART_TEST_TARGET_OAT_DEFAULT_RULES += $$(default_test_rule)
  ART_TEST_TARGET_OAT_DEFAULT_$(1)_RULES += $$(default_test_rule)

  optimizing_test_rule := test-art-target-oat-optimizing-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX)
  $(call define-test-art-oat-rule-target,$(1),$(2),$$(optimizing_test_rule), \
    -Xcompiler-option --compiler-backend=Optimizing)

  ART_TEST_TARGET_OAT_OPTIMIZING$$($(2)ART_PHONY_TEST_TARGET_SUFFIX)_RULES += $$(optimizing_test_rule)
  ART_TEST_TARGET_OAT_OPTIMIZING_RULES += $$(optimizing_test_rule)
  ART_TEST_TARGET_OAT_OPTIMIZING_$(1)_RULES += $$(optimizing_test_rule)

  # Define a phony rule to run a target oat test using the interpeter.
  interpreter_test_rule := test-art-target-oat-interpreter-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX)
  $(call define-test-art-oat-rule-target,$(1),$(2),$$(interpreter_test_rule),-Xint)

  ART_TEST_TARGET_OAT_INTERPRETER$$($(2)ART_PHONY_TEST_TARGET_SUFFIX)_RULES += $$(interpreter_test_rule)
  ART_TEST_TARGET_OAT_INTERPRETER_RULES += $$(interpreter_test_rule)
  ART_TEST_TARGET_OAT_INTERPRETER_$(1)_RULES += $$(interpreter_test_rule)

  # Define a phony rule to run both the default and interpreter variants.
  all_test_rule :=  test-art-target-oat-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX)
.PHONY: $$(all_test_rule)
$$(all_test_rule): $$(default_test_rule) $$(optimizing_test_rule) $$(interpreter_test_rule)
	$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

  ART_TEST_TARGET_OAT$$($(2)ART_PHONY_TEST_TARGET_SUFFIX)_RULES += $$(all_test_rule)
  ART_TEST_TARGET_OAT_RULES += $$(all_test_rule)
  ART_TEST_TARGET_OAT_$(1)_RULES += $$(all_test_rule)

  # Clear locally defined variables.
  interpreter_test_rule :=
  default_test_rule :=
  optimizing_test_rule :=
  all_test_rule :=
endef  # define-test-art-oat-rules-target

ART_TEST_HOST_OAT_DEFAULT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_DEFAULT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_DEFAULT_RULES :=
ART_TEST_HOST_OAT_OPTIMIZING$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_OPTIMIZING$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_OPTIMIZING_RULES :=
ART_TEST_HOST_OAT_INTERPRETER$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_INTERPRETER$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_INTERPRETER_RULES :=
ART_TEST_HOST_OAT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_RULES :=

# All tests require the host executables, libarttest and the core images.
ART_TEST_HOST_OAT_DEPENDENCIES := \
  $(ART_HOST_EXECUTABLES) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libarttest$(ART_HOST_SHLIB_EXTENSION) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION) \
  $(HOST_CORE_IMG_OUT)

ifneq ($(HOST_PREFER_32_BIT),true)
ART_TEST_HOST_OAT_DEPENDENCIES += \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libarttest$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_HOST_CORE_IMG_OUT)
endif

# Define rule to run an individual oat test on the host. Output from the test is written to the
# host in /tmp/android-data in a directory named after test's rule name (its target) and the parent
# process' PID (ie the PID of make). On failure the output is dumped to the console.
# $(1): directory - the name of the test we're building such as HelloWorld.
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
# $(3): the target (rule name), e.g. test-art-host-oat-default-HelloWorld64
# $(4): argument to dex2oat
# $(5): argument to runtime, e.g. -Xint or undefined
define define-test-art-oat-rule-host
  # Remove the leading / from /tmp for the test directory.
  dex_file := $$(subst /tmp,tmp,$(ART_HOST_TEST_DIR))/android-data-$(3)/oat-test-dex-$(1).jar
  oat_file := $(ART_HOST_TEST_DIR)/android-data-$(3)/dalvik-cache/$$($(2)HOST_ARCH)/$$(subst /,@,$$(dex_file))@classes.dex
$(3): PRIVATE_DEX_FILE := /$$(dex_file)
$(3): PRIVATE_OAT_FILE := $$(oat_file)
.PHONY: $(3)
$(3): $$(ART_OAT_TEST_$(1)_DEX) $(ART_TEST_HOST_OAT_DEPENDENCIES)
	$(hide) mkdir -p $(ART_HOST_TEST_DIR)/android-data-$$@/dalvik-cache/$$($(2)HOST_ARCH)
	$(hide) cp $$(realpath $$<) $(ART_HOST_TEST_DIR)/android-data-$$@/oat-test-dex-$(1).jar
	$(hide) $(DEX2OATD) $(DEX2OAT_FLAGS) --runtime-arg -Xms16m --runtime-arg -Xmx16m $(4) \
	  --boot-image=$$(HOST_CORE_IMG_LOCATION) \
	  --dex-file=$$(PRIVATE_DEX_FILE) --oat-file=$$(PRIVATE_OAT_FILE) \
	  --instruction-set=$($(2)ART_HOST_ARCH) --host --android-root=$(HOST_OUT) \
	  || $$(call ART_TEST_FAILED,$$@)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	ANDROID_DATA=$(ART_HOST_TEST_DIR)/android-data-$$@/ \
	ANDROID_ROOT=$(HOST_OUT) \
	ANDROID_LOG_TAGS='*:d' \
	LD_LIBRARY_PATH=$$($(2)ART_HOST_OUT_SHARED_LIBRARIES) \
	$(HOST_OUT_EXECUTABLES)/dalvikvm$$($(2)ART_PHONY_TEST_HOST_SUFFIX) $(DALVIKVM_FLAGS) $(5) \
	    -XXlib:libartd$(HOST_SHLIB_SUFFIX) -Ximage:$$(HOST_CORE_IMG_LOCATION) \
	    -classpath $(ART_HOST_TEST_DIR)/android-data-$$@/oat-test-dex-$(1).jar \
	    -Djava.library.path=$$($(2)ART_HOST_OUT_SHARED_LIBRARIES) $(1) \
	      > $(ART_HOST_TEST_DIR)/android-data-$$@/output.txt 2>&1 \
	  && $$(call ART_TEST_PASSED,$$@) \
	  || (([ ! -f $(ART_HOST_TEST_DIR)/android-data-$$@/output.txt ] || \
	       cat $(ART_HOST_TEST_DIR)/android-data-$$@/output.txt) && $$(call ART_TEST_FAILED,$$@))
	$$(hide) (echo $(MAKECMDGOALS) | grep -q $$@ && \
	  echo "run-test run as top-level target, removing test directory $(ART_HOST_TEST_DIR)" && \
	  rm -r $(ART_HOST_TEST_DIR)) || true
endef  # define-test-art-oat-rule-host

# Define rules to run oat tests on the host.
# $(1): directory - the name of the test we're building such as HelloWorld.
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
define define-test-art-oat-rules-host
  # Create a rule to run the host oat test with the default compiler.
  default_test_rule := test-art-host-oat-default-$(1)$$($(2)ART_PHONY_TEST_HOST_SUFFIX)
  $(call define-test-art-oat-rule-host,$(1),$(2),$$(default_test_rule),,)

  ART_TEST_HOST_OAT_DEFAULT$$($(2)ART_PHONY_TEST_HOST_SUFFIX)_RULES += $$(default_test_rule)
  ART_TEST_HOST_OAT_DEFAULT_RULES += $$(default_test_rule)
  ART_TEST_HOST_OAT_DEFAULT_$(1)_RULES += $$(default_test_rule)

  # Create a rule to run the host oat test with the optimizing compiler.
  optimizing_test_rule := test-art-host-oat-optimizing-$(1)$$($(2)ART_PHONY_TEST_HOST_SUFFIX)
  $(call define-test-art-oat-rule-host,$(1),$(2),$$(optimizing_test_rule),--compiler-backend=Optimizing,)

  ART_TEST_HOST_OAT_OPTIMIZING$$($(2)ART_PHONY_TEST_HOST_SUFFIX)_RULES += $$(optimizing_test_rule)
  ART_TEST_HOST_OAT_OPTIMIZING_RULES += $$(optimizing_test_rule)
  ART_TEST_HOST_OAT_OPTIMIZING_$(1)_RULES += $$(optimizing_test_rule)

  # Create a rule to run the host oat test with the interpreter.
  interpreter_test_rule := test-art-host-oat-interpreter-$(1)$$($(2)ART_PHONY_TEST_HOST_SUFFIX)
  $(call define-test-art-oat-rule-host,$(1),$(2),$$(interpreter_test_rule),--compiler-filter=interpret-only,-Xint)

  ART_TEST_HOST_OAT_INTERPRETER$$($(2)ART_PHONY_TEST_HOST_SUFFIX)_RULES += $$(interpreter_test_rule)
  ART_TEST_HOST_OAT_INTERPRETER_RULES += $$(interpreter_test_rule)
  ART_TEST_HOST_OAT_INTERPRETER_$(1)_RULES += $$(interpreter_test_rule)

  # Define a phony rule to run both the default and interpreter variants.
  all_test_rule :=  test-art-host-oat-$(1)$$($(2)ART_PHONY_TEST_HOST_SUFFIX)
.PHONY: $$(all_test_rule)
$$(all_test_rule): $$(default_test_rule) $$(interpreter_test_rule) $$(optimizing_test_rule)
	$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

  ART_TEST_HOST_OAT$$($(2)ART_PHONY_TEST_HOST_SUFFIX)_RULES += $$(all_test_rule)
  ART_TEST_HOST_OAT_RULES += $$(all_test_rule)
  ART_TEST_HOST_OAT_$(1)_RULES += $$(all_test_rule)

  # Clear locally defined variables.
  default_test_rule :=
  optimizing_test_rule :=
  interpreter_test_rule :=
  all_test_rule :=
endef  # define-test-art-oat-rules-host

# For a given test create all the combinations of host/target, compiler and suffix such as:
# test-art-host-oat-HelloWord or test-art-target-oat-interpreter-HelloWorld64
# $(1): test name, e.g. HelloWorld
# $(2): host or target
# $(3): HOST or TARGET
# $(4): undefined, -default, -optimizing or -interpreter
# $(5): undefined, _DEFAULT, _OPTIMIZING or _INTERPRETER
define define-test-art-oat-combination-for-test
  ifeq ($(2),host)
    ifneq ($(3),HOST)
      $$(error argument mismatch $(2) and ($3))
    endif
  else
    ifneq ($(2),target)
      $$(error found $(2) expected host or target)
    endif
    ifneq ($(3),TARGET)
      $$(error argument mismatch $(2) and ($3))
    endif
  endif

  rule_name := test-art-$(2)-oat$(4)-$(1)
  dependencies := $$(ART_TEST_$(3)_OAT$(5)_$(1)_RULES)

  ifeq ($$(dependencies),)
    ifneq ($(4),-optimizing)
      $$(error $$(rule_name) has no dependencies)
    endif
  endif

.PHONY: $$(rule_name)
$$(rule_name): $$(dependencies)
	$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

  # Clear locally defined variables.
  rule_name :=
  dependencies :=
endef  # define-test-art-oat-combination

# Define target and host oat test rules for the differing multilib flavors and default vs
# interpreter runs. The format of the generated rules (for running an individual test) is:
#   test-art-(host|target)-oat-(default|interpreter)-${directory}(32|64)
# The rules are appended to various lists to enable shorter phony build rules to be built.
# $(1): directory
define define-test-art-oat-rules
  # Define target tests.
  ART_TEST_TARGET_OAT_DEFAULT_$(1)_RULES :=
  ART_TEST_TARGET_OAT_OPTIMIZING_$(1)_RULES :=
  ART_TEST_TARGET_OAT_INTERPRETER_$(1)_RULES :=
  ART_TEST_TARGET_OAT_$(1)_RULES :=
  $(call define-test-art-oat-rules-target,$(1),)
  ifdef TARGET_2ND_ARCH
    $(call define-test-art-oat-rules-target,$(1),2ND_)
  endif
  $(call define-test-art-oat-combination-for-test,$(1),target,TARGET,,))
  $(call define-test-art-oat-combination-for-test,$(1),target,TARGET,-default,_DEFAULT))
  $(call define-test-art-oat-combination-for-test,$(1),target,TARGET,-optimizing,_OPTIMIZING))
  $(call define-test-art-oat-combination-for-test,$(1),target,TARGET,-interpreter,_INTERPRETER))

  # Define host tests.
  ART_TEST_HOST_OAT_DEFAULT_$(1)_RULES :=
  ART_TEST_HOST_OAT_OPTIMIZING_$(1)_RULES :=
  ART_TEST_HOST_OAT_INTERPRETER_$(1)_RULES :=
  ART_TEST_HOST_OAT_$(1)_RULES :=
  $(call define-test-art-oat-rules-host,$(1),)
  ifneq ($(HOST_PREFER_32_BIT),true)
    $(call define-test-art-oat-rules-host,$(1),2ND_)
  endif
  $(call define-test-art-oat-combination-for-test,$(1),host,HOST,,)
  $(call define-test-art-oat-combination-for-test,$(1),host,HOST,-default,_DEFAULT)
  $(call define-test-art-oat-combination-for-test,$(1),host,HOST,-optimizing,_OPTIMIZING)
  $(call define-test-art-oat-combination-for-test,$(1),host,HOST,-interpreter,_INTERPRETER)

  # Clear locally defined variables.
  ART_TEST_TARGET_OAT_DEFAULT_$(1)_RULES :=
  ART_TEST_TARGET_OAT_OPTIMIZING_$(1)_RULES :=
  ART_TEST_TARGET_OAT_INTERPRETER_$(1)_RULES :=
  ART_TEST_TARGET_OAT_$(1)_RULES :=
  ART_TEST_HOST_OAT_DEFAULT_$(1)_RULES :=
  ART_TEST_HOST_OAT_OPTIMIZING_$(1)_RULES :=
  ART_TEST_HOST_OAT_INTERPRETER_$(1)_RULES :=
  ART_TEST_HOST_OAT_$(1)_RULES :=
endef  # define-test-art-oat-rules
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call define-test-art-oat-rules,$(dir))))

# Define all the combinations of host/target, compiler and suffix such as:
# test-art-host-oat or test-art-target-oat-interpreter64
# $(1): host or target
# $(2): HOST or TARGET
# $(3): undefined, -default, -optimizing or -interpreter
# $(4): undefined, _DEFAULT, _OPTIMIZING or _INTERPRETER
# $(5): undefined, 32 or 64
define define-test-art-oat-combination
  ifeq ($(1),host)
    ifneq ($(2),HOST)
      $$(error argument mismatch $(1) and ($2))
    endif
  else
    ifneq ($(1),target)
      $$(error found $(1) expected host or target)
    endif
    ifneq ($(2),TARGET)
      $$(error argument mismatch $(1) and ($2))
    endif
  endif

  rule_name := test-art-$(1)-oat$(3)$(5)
  dependencies := $$(ART_TEST_$(2)_OAT$(4)$(5)_RULES)

  ifeq ($$(dependencies),)
    ifneq ($(3),-optimizing)
      $$(error $$(rule_name) has no dependencies)
    endif
  endif

.PHONY: $$(rule_name)
$$(rule_name): $$(dependencies)
	$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

  # Clear locally defined variables.
  rule_name :=
  dependencies :=

endef  # define-test-art-oat-combination

$(eval $(call define-test-art-oat-combination,target,TARGET,,,))
$(eval $(call define-test-art-oat-combination,target,TARGET,-default,_DEFAULT,))
$(eval $(call define-test-art-oat-combination,target,TARGET,-optimizing,_OPTIMIZING,))
$(eval $(call define-test-art-oat-combination,target,TARGET,-interpreter,_INTERPRETER,))
$(eval $(call define-test-art-oat-combination,target,TARGET,,,$(ART_PHONY_TEST_TARGET_SUFFIX)))
$(eval $(call define-test-art-oat-combination,target,TARGET,-default,_DEFAULT,$(ART_PHONY_TEST_TARGET_SUFFIX)))
$(eval $(call define-test-art-oat-combination,target,TARGET,-optimizing,_OPTIMIZING,$(ART_PHONY_TEST_TARGET_SUFFIX)))
$(eval $(call define-test-art-oat-combination,target,TARGET,-interpreter,_INTERPRETER,$(ART_PHONY_TEST_TARGET_SUFFIX)))
ifdef TARGET_2ND_ARCH
$(eval $(call define-test-art-oat-combination,target,TARGET,,,$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)))
$(eval $(call define-test-art-oat-combination,target,TARGET,-default,_DEFAULT,$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)))
$(eval $(call define-test-art-oat-combination,target,TARGET,-optimizing,_OPTIMIZING,$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)))
$(eval $(call define-test-art-oat-combination,target,TARGET,-interpreter,_INTERPRETER,$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)))
endif

$(eval $(call define-test-art-oat-combination,host,HOST,,,))
$(eval $(call define-test-art-oat-combination,host,HOST,-default,_DEFAULT,))
$(eval $(call define-test-art-oat-combination,host,HOST,-optimizing,_OPTIMIZING,))
$(eval $(call define-test-art-oat-combination,host,HOST,-interpreter,_INTERPRETER,))
$(eval $(call define-test-art-oat-combination,host,HOST,,,$(ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-oat-combination,host,HOST,-default,_DEFAULT,$(ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-oat-combination,host,HOST,-optimizing,_OPTIMIZING,$(ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-oat-combination,host,HOST,-interpreter,_INTERPRETER,$(ART_PHONY_TEST_HOST_SUFFIX)))
ifneq ($(HOST_PREFER_32_BIT),true)
$(eval $(call define-test-art-oat-combination,host,HOST,,,$(2ND_ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-oat-combination,host,HOST,-default,_DEFAULT,$(2ND_ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-oat-combination,host,HOST,-optimizing,_OPTIMIZING,$(2ND_ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-oat-combination,host,HOST,-interpreter,_INTERPRETER,$(2ND_ART_PHONY_TEST_HOST_SUFFIX)))
endif

# Clear locally defined variables.
define-test-art-oat-rule-target :=
define-test-art-oat-rules-target :=
define-test-art-oat-rule-host :=
define-test-art-oat-rules-host :=
define-test-art-oat-combination-for-test :=
define-test-art-oat-combination :=
ART_TEST_TARGET_OAT_DEFAULT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_DEFAULT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_DEFAULT_RULES :=
ART_TEST_TARGET_OAT_OPTIMIZING$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_OPTIMIZING$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_OPTIMIZING_RULES :=
ART_TEST_TARGET_OAT_INTERPRETER$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_INTERPRETER$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_INTERPRETER_RULES :=
ART_TEST_TARGET_OAT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_OAT_RULES :=
ART_TEST_HOST_OAT_DEFAULT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_DEFAULT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_DEFAULT_RULES :=
ART_TEST_HOST_OAT_OPTIMIZING$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_OPTIMIZING$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_OPTIMIZING_RULES :=
ART_TEST_HOST_OAT_INTERPRETER$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_INTERPRETER$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_INTERPRETER_RULES :=
ART_TEST_HOST_OAT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_OAT_RULES :=
ART_TEST_HOST_OAT_DEPENDENCIES :=
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval ART_OAT_TEST_$(dir)_DEX :=))
TEST_OAT_DIRECTORIES :=
LOCAL_PID :=
LOCAL_PATH :=
