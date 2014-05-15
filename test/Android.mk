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

include art/build/Android.common.mk

########################################################################

# subdirectories which are used as inputs for gtests
TEST_DEX_DIRECTORIES := \
	AbstractMethod \
	AllFields \
	ExceptionHandle \
	GetMethodSignature \
	Interfaces \
	Main \
	MyClass \
	MyClassNatives \
	Nested \
	NonStaticLeafMethods \
	ProtoCompare \
	ProtoCompare2 \
	StaticLeafMethods \
	Statics \
	StaticsFromCode \
	Transaction \
	XandY

# subdirectories of which are used with test-art-target-oat
# Declare the simplest tests (Main, HelloWorld) first, the rest are alphabetical
TEST_OAT_DIRECTORIES := \
	Main \
	HelloWorld \
	InterfaceTest \
	JniTest \
	NativeAllocations \
	ParallelGC \
	ReferenceMap \
	StackWalk \
	ThreadStress \
	UnsafeTest

# TODO: Enable when the StackWalk2 tests are passing
#	StackWalk2 \

ART_TEST_TARGET_DEX_FILES :=
ART_TEST_TARGET_DEX_FILES$(ART_PHONY_TEST_TARGET_SUFFIX) :=
ART_TEST_TARGET_DEX_FILES$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) :=
ART_TEST_HOST_DEX_FILES :=

# $(1): module prefix
# $(2): input test directory
# $(3): target output module path (default module path is used on host)
define build-art-test-dex
  ifeq ($(ART_BUILD_TARGET),true)
    include $(CLEAR_VARS)
    LOCAL_MODULE := $(1)-$(2)
    LOCAL_MODULE_TAGS := tests
    LOCAL_SRC_FILES := $(call all-java-files-under, $(2))
    LOCAL_JAVA_LIBRARIES := $(TARGET_CORE_JARS)
    LOCAL_NO_STANDARD_LIBRARIES := true
    LOCAL_MODULE_PATH := $(3)
    LOCAL_DEX_PREOPT_IMAGE := $(TARGET_CORE_IMG_OUT)
    LOCAL_DEX_PREOPT := false
    LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common.mk
    LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/Android.mk
    include $(BUILD_JAVA_LIBRARY)

    ART_TEST_TARGET_DEX_FILES += $$(LOCAL_INSTALLED_MODULE)
    ART_TEST_TARGET_DEX_FILES$(ART_PHONY_TEST_TARGET_SUFFIX) += $$(LOCAL_INSTALLED_MODULE)

    ifdef TARGET_2ND_ARCH
	    ART_TEST_TARGET_DEX_FILES$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) += $(4)/$(1)-$(2).jar

      # TODO: make this a simple copy
$(4)/$(1)-$(2).jar: $(3)/$(1)-$(2).jar $(4)
	cp $$< $(4)/
    endif
  endif

  ifeq ($(ART_BUILD_HOST),true)
    include $(CLEAR_VARS)
    LOCAL_MODULE := $(1)-$(2)
    LOCAL_SRC_FILES := $(call all-java-files-under, $(2))
    LOCAL_JAVA_LIBRARIES := $(HOST_CORE_JARS)
    LOCAL_NO_STANDARD_LIBRARIES := true
    LOCAL_DEX_PREOPT_IMAGE := $(HOST_CORE_IMG_OUT)
    LOCAL_DEX_PREOPT := false
    LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common.mk
    LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/Android.mk
    include $(BUILD_HOST_DALVIK_JAVA_LIBRARY)
    ART_TEST_HOST_DEX_FILES += $$(LOCAL_INSTALLED_MODULE)
  endif
endef
$(foreach dir,$(TEST_DEX_DIRECTORIES), $(eval $(call build-art-test-dex,art-test-dex,$(dir),$(ART_NATIVETEST_OUT),$(2ND_ART_NATIVETEST_OUT))))
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call build-art-test-dex,oat-test-dex,$(dir),$(ART_TEST_OUT),$(2ND_ART_TEST_OUT))))

# Used outside the art project to get a list of the current tests
ART_TEST_DEX_MAKE_TARGETS := $(addprefix art-test-dex-, $(TEST_DEX_DIRECTORIES))
ART_TEST_OAT_MAKE_TARGETS := $(addprefix oat-test-dex-, $(TEST_OAT_DIRECTORIES))

# Rules to explicitly create 2nd-arch test directories, as we use a "cp" for them
# instead of BUILD_JAVA_LIBRARY
ifneq ($(2ND_ART_NATIVETEST_OUT),)
$(2ND_ART_NATIVETEST_OUT):
	$(hide) mkdir -p $@
endif

ifneq ($(2ND_ART_TEST_OUT),)
$(2ND_ART_TEST_OUT):
	$(hide) mkdir -p $@
endif

########################################################################

ART_TEST_TARGET_OAT_TARGETS$(ART_PHONY_TEST_TARGET_SUFFIX) :=
ART_TEST_TARGET_OAT_TARGETS$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) :=
ART_TEST_HOST_OAT_DEFAULT_TARGETS :=
ART_TEST_HOST_OAT_INTERPRETER_TARGETS :=

define declare-test-art-oat-targets-impl
.PHONY: test-art-target-oat-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX)
test-art-target-oat-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX): $($(2)ART_TEST_OUT)/oat-test-dex-$(1).jar test-art-target-sync
	adb shell touch $($(2)ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell rm $($(2)ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell sh -c "/system/bin/dalvikvm$($(2)ART_TARGET_BINARY_SUFFIX) $(DALVIKVM_FLAGS) -XXlib:libartd.so -Ximage:$($(2)ART_TEST_DIR)/core.art -classpath $($(2)ART_TEST_DIR)/oat-test-dex-$(1).jar -Djava.library.path=$($(2)ART_TEST_DIR) $(1) && touch $($(2)ART_TEST_DIR)/test-art-target-oat-$(1)"
	$(hide) (adb pull $($(2)ART_TEST_DIR)/test-art-target-oat-$(1) /tmp/ && echo test-art-target-oat-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX) PASSED) || (echo test-art-target-oat-$(1)$($(2)ART_PHONY_TEST_TARGET_SUFFIX) FAILED && exit 1)
	$(hide) rm /tmp/test-art-target-oat-$(1)
endef

# $(1): directory
# $(2): arguments
define declare-test-art-oat-targets
  ifdef TARGET_2ND_ARCH
    $(call declare-test-art-oat-targets-impl,$(1),2ND_)

    # Bind the primary to the non-suffix rule
    ifneq ($(ART_PHONY_TEST_TARGET_SUFFIX),)
test-art-target-oat-$(1): test-art-target-oat-$(1)$(ART_PHONY_TEST_TARGET_SUFFIX)
    endif
  endif
  $(call declare-test-art-oat-targets-impl,$(1),)

$(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).odex: $(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).jar $(HOST_CORE_IMG_OUT) | $(DEX2OATD)
	$(DEX2OATD) $(DEX2OAT_FLAGS) --runtime-arg -Xms16m --runtime-arg -Xmx16m --boot-image=$(HOST_CORE_IMG_OUT) --dex-file=$$(realpath $$<) --oat-file=$$(realpath $(HOST_OUT_JAVA_LIBRARIES))/oat-test-dex-$(1).odex --instruction-set=$(ART_HOST_ARCH) --host --android-root=$(HOST_OUT)

.PHONY: test-art-host-oat-default-$(1)
test-art-host-oat-default-$(1): $(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).odex test-art-host-dependencies
	mkdir -p /tmp/android-data/test-art-host-oat-default-$(1)
	ANDROID_DATA=/tmp/android-data/test-art-host-oat-default-$(1) \
	  ANDROID_ROOT=$(HOST_OUT) \
	  LD_LIBRARY_PATH=$(HOST_OUT_SHARED_LIBRARIES) \
	  $(HOST_OUT_EXECUTABLES)/dalvikvm $(DALVIKVM_FLAGS) -XXlib:libartd.so -Ximage:$$(realpath $(HOST_CORE_IMG_OUT)) -classpath $(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).jar -Djava.library.path=$(HOST_OUT_SHARED_LIBRARIES) $(1) $(2) \
          && echo test-art-host-oat-default-$(1) PASSED || (echo test-art-host-oat-default-$(1) FAILED && exit 1)
	$(hide) rm -r /tmp/android-data/test-art-host-oat-default-$(1)

.PHONY: test-art-host-oat-interpreter-$(1)
test-art-host-oat-interpreter-$(1): $(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).odex test-art-host-dependencies
	mkdir -p /tmp/android-data/test-art-host-oat-interpreter-$(1)
	ANDROID_DATA=/tmp/android-data/test-art-host-oat-interpreter-$(1) \
	  ANDROID_ROOT=$(HOST_OUT) \
	  LD_LIBRARY_PATH=$(HOST_OUT_SHARED_LIBRARIES) \
	  $(HOST_OUT_EXECUTABLES)/dalvikvm -XXlib:libartd.so -Ximage:$$(realpath $(HOST_CORE_IMG_OUT)) -Xint -classpath $(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).jar -Djava.library.path=$(HOST_OUT_SHARED_LIBRARIES) $(1) $(2) \
          && echo test-art-host-oat-interpreter-$(1) PASSED || (echo test-art-host-oat-interpreter-$(1) FAILED && exit 1)
	$(hide) rm -r /tmp/android-data/test-art-host-oat-interpreter-$(1)

.PHONY: test-art-host-oat-$(1)
test-art-host-oat-$(1): test-art-host-oat-default-$(1) test-art-host-oat-interpreter-$(1)

.PHONY: test-art-oat-$(1)
test-art-oat-$(1): test-art-host-oat-$(1) test-art-target-oat-$(1)

ART_TEST_TARGET_OAT_TARGETS$(ART_PHONY_TEST_TARGET_SUFFIX) += test-art-target-oat-$(1)$(ART_PHONY_TEST_TARGET_SUFFIX)
ifdef TARGET_2ND_ARCH
  ART_TEST_TARGET_OAT_TARGETS$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) += test-art-target-oat-$(1)$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)
endif
ART_TEST_HOST_OAT_DEFAULT_TARGETS += test-art-host-oat-default-$(1)
ART_TEST_HOST_OAT_INTERPRETER_TARGETS += test-art-host-oat-interpreter-$(1)
endef
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call declare-test-art-oat-targets,$(dir))))

########################################################################

TEST_ART_RUN_TEST_MAKE_TARGETS :=
art_run_tests_dir := $(call intermediates-dir-for,PACKAGING,art-run-tests)/DATA

# Helper to create individual build targets for tests.
# Must be called with $(eval)
# $(1): the test number
define declare-make-art-run-test
dmart_target := $(art_run_tests_dir)/art-run-tests/$(1)/touch
$$(dmart_target): $(DX) $(HOST_OUT_EXECUTABLES)/jasmin
	$(hide) rm -rf $$(dir $$@) && mkdir -p $$(dir $$@)
	$(hide) DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) $(LOCAL_PATH)/run-test --build-only --output-path $$(abspath $$(dir $$@)) $(1)
	$(hide) touch $$@


TEST_ART_RUN_TEST_MAKE_TARGETS += $$(dmart_target)
dmart_target :=
endef

# Expand all tests.
TEST_ART_RUN_TESTS := $(wildcard $(LOCAL_PATH)/[0-9]*)
TEST_ART_RUN_TESTS := $(subst $(LOCAL_PATH)/,, $(TEST_ART_RUN_TESTS))
TEST_ART_TIMING_SENSITIVE_RUN_TESTS := 053-wait-some 055-enum-performance
ifdef dist_goal # disable timing sensitive tests on "dist" builds.
  $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), \
    $(info Skipping $(test)) \
    $(eval TEST_ART_RUN_TESTS := $(filter-out $(test), $(TEST_ART_RUN_TESTS))))
endif
$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call declare-make-art-run-test,$(test))))

include $(CLEAR_VARS)
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE := art-run-tests
LOCAL_ADDITIONAL_DEPENDENCIES := $(TEST_ART_RUN_TEST_MAKE_TARGETS)
# The build system use this flag to pick up files generated by declare-make-art-run-test.
LOCAL_PICKUP_FILES := $(art_run_tests_dir)

include $(BUILD_PHONY_PACKAGE)

# clear temp vars
TEST_ART_RUN_TEST_MAKE_TARGETS :=
declare-make-art-run-test :=

########################################################################
