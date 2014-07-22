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

include art/build/Android.common_test.mk

# List of all tests of the form 003-omnibus-opcodes.
TEST_ART_RUN_TESTS := $(wildcard $(LOCAL_PATH)/[0-9]*)
TEST_ART_RUN_TESTS := $(subst $(LOCAL_PATH)/,, $(TEST_ART_RUN_TESTS))

# List all the test names for host and target and compiler variants.
# $(1): test name, e.g. 003-omnibus-opcodes
# $(2): undefined, -trace, -gcverify or -gcstress
# $(3): -relocate, -norelocate, -prebuild, or undefined.
define all-run-test-names
  test-art-host-run-test$(2)-default$(3)-$(1)32 \
  test-art-host-run-test$(2)-optimizing$(3)-$(1)32 \
  test-art-host-run-test$(2)-interpreter$(3)-$(1)32 \
  test-art-host-run-test$(2)-default$(3)-$(1)64 \
  test-art-host-run-test$(2)-optimizing$(3)-$(1)64 \
  test-art-host-run-test$(2)-interpreter$(3)-$(1)64 \
  test-art-target-run-test$(2)-default$(3)-$(1)32 \
  test-art-target-run-test$(2)-optimizing$(3)-$(1)32 \
  test-art-target-run-test$(2)-interpreter$(3)-$(1)32 \
  test-art-target-run-test$(2)-default$(3)-$(1)64 \
  test-art-target-run-test$(2)-optimizing$(3)-$(1)64 \
  test-art-target-run-test$(2)-interpreter$(3)-$(1)64
endef  # all-run-test-names

# Tests that are timing sensitive and flaky on heavily loaded systems.
TEST_ART_TIMING_SENSITIVE_RUN_TESTS := \
  053-wait-some \
  055-enum-performance

 # disable timing sensitive tests on "dist" builds.
ifdef dist_goal
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),,))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-trace,))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-gcverify,))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-gcstress,))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),,-relocate))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-trace,-relocate))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-gcverify,-relocate))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-gcstress,-relocate))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),,-norelocate))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-trace,-norelocate))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-gcverify,-norelocate))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-gcstress,-norelocate))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),,-prebuild))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-trace,-prebuild))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-gcverify,-prebuild))
  ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_TIMING_SENSITIVE_RUN_TESTS), $(call all-run-test-names,$(test),-gcstress,-prebuild))
endif

# Tests that are broken in --trace mode.
TEST_ART_BROKEN_TRACE_RUN_TESTS := \
  003-omnibus-opcodes \
  004-annotations \
  012-math \
  018-stack-overflow \
  023-many-interfaces \
  027-arithmetic \
  031-class-attributes \
  037-inherit \
  044-proxy \
  046-reflect \
  051-thread \
  055-enum-performance \
  062-character-encodings \
  064-field-access \
  074-gc-thrash \
  078-polymorphic-virtual \
  080-oom-throw \
  082-inline-execute \
  083-compiler-regressions \
  093-serialization \
  097-duplicate-method \
  100-reflect2 \
  102-concurrent-gc \
  103-string-append \
  107-int-math2 \
  112-double-math \
  700-LoadArgRegs \
  701-easy-div-rem

ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_BROKEN_TRACE_RUN_TESTS), $(call all-run-test-names,$(test),-trace,-relocate))
ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_BROKEN_TRACE_RUN_TESTS), $(call all-run-test-names,$(test),-trace,-prebuild))
ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_BROKEN_TRACE_RUN_TESTS), $(call all-run-test-names,$(test),-trace,-norelocate))
ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_BROKEN_TRACE_RUN_TESTS), $(call all-run-test-names,$(test),-trace,))

# Tests that need more than 2MB of RAM or are running into other corner cases in GC stress related
# to OOMEs.
TEST_ART_BROKEN_GCSTRESS_RUN_TESTS := \
  074-gc-thrash \
  080-oom-throw \
  096-array-copy-concurrent-gc

ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_BROKEN_GCSTRESS_RUN_TESTS), $(call all-run-test-names,$(test),-gcstress,-relocate))
ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_BROKEN_GCSTRESS_RUN_TESTS), $(call all-run-test-names,$(test),-gcstress,-prebuild))
ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_BROKEN_GCSTRESS_RUN_TESTS), $(call all-run-test-names,$(test),-gcstress,-norelocate))
ART_TEST_KNOWN_BROKEN += $(foreach test, $(TEST_ART_BROKEN_GCSTRESS_RUN_TESTS), $(call all-run-test-names,$(test),-gcstress,))


# The path where build only targets will be output, e.g.
# out/target/product/generic_x86_64/obj/PACKAGING/art-run-tests_intermediates/DATA
art_run_tests_dir := $(call intermediates-dir-for,PACKAGING,art-run-tests)/DATA

# A generated list of prerequisites that call 'run-test --build-only', the actual prerequisite is
# an empty file touched in the intermediate directory.
TEST_ART_RUN_TEST_BUILD_RULES :=

# Helper to create individual build targets for tests. Must be called with $(eval).
# $(1): the test number
define define-build-art-run-test
  dmart_target := $(art_run_tests_dir)/art-run-tests/$(1)/touch
$$(dmart_target): $(DX) $(HOST_OUT_EXECUTABLES)/jasmin
	$(hide) rm -rf $$(dir $$@) && mkdir -p $$(dir $$@)
	$(hide) DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) \
	  $(LOCAL_PATH)/run-test --build-only --output-path $$(abspath $$(dir $$@)) $(1)
	$(hide) touch $$@

  TEST_ART_RUN_TEST_BUILD_RULES += $$(dmart_target)
  dmart_target :=
endef
$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call define-build-art-run-test,$(test))))

include $(CLEAR_VARS)
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE := art-run-tests
LOCAL_ADDITIONAL_DEPENDENCIES := $(TEST_ART_RUN_TEST_BUILD_RULES)
# The build system use this flag to pick up files generated by declare-make-art-run-test.
LOCAL_PICKUP_FILES := $(art_run_tests_dir)

include $(BUILD_PHONY_PACKAGE)

# Clear temp vars.
all-run-test-names :=
art_run_tests_dir :=
define-build-art-run-test :=
TEST_ART_RUN_TEST_BUILD_RULES :=
TEST_ART_TIMING_SENSITIVE_RUN_TESTS :=
TEST_ART_BROKEN_TRACE_RUN_TESTS :=
TEST_ART_BROKEN_GCSTRESS_RUN_TESTS :=

########################################################################

ART_TEST_TARGET_RUN_TEST_ALL_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RULES :=
ART_TEST_TARGET_RUN_TEST_RELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_NORELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_NORELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_NORELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_NORELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_PREBUILD_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_PREBUILD_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_PREBUILD_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_PREBUILD_RULES :=
ART_TEST_TARGET_RUN_TEST_ALL$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_ALL_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RULES :=
ART_TEST_HOST_RUN_TEST_RELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_NORELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_NORELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_NORELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_NORELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_PREBUILD_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_PREBUILD_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_PREBUILD_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_PREBUILD_RULES :=
ART_TEST_HOST_RUN_TEST_ALL$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=

# We need dex2oat and dalvikvm on the target as well as the core image.
TEST_ART_TARGET_SYNC_DEPS += $(ART_TARGET_EXECUTABLES) $(TARGET_CORE_IMG_OUT) $(2ND_TARGET_CORE_IMG_OUT)

# All tests require the host executables and the core images.
ART_TEST_HOST_RUN_TEST_DEPENDENCIES := \
  $(ART_HOST_EXECUTABLES) \
  $(ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION) \
  $(HOST_CORE_IMG_OUT)

ifneq ($(HOST_PREFER_32_BIT),true)
ART_TEST_HOST_RUN_TEST_DEPENDENCIES += \
  $(2ND_ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$(ART_HOST_SHLIB_EXTENSION) \
  $(2ND_HOST_CORE_IMG_OUT)
endif

# For a given test create all the combinations of host/target, compiler and suffix such as:
# test-art-host-run-test-optimizing-003-omnibus-opcodes32
# $(1): test name, e.g. 003-omnibus-opcodes
# $(2): host or target
# $(3): default, optimizing or interpreter
# $(4): 32 or 64
# $(5): run tests with tracing or GC verification enabled or not: trace, gcverify or undefined
# $(6): relocate, norelocate, prebuild
define define-test-art-run-test
  run_test_options := $(addprefix --runtime-option ,$(DALVIKVM_FLAGS))
  run_test_rule_name :=
  uc_host_or_target :=
  prereq_rule :=
  skip_test := false
  uc_reloc_type :=
  ifeq ($(2),host)
    uc_host_or_target := HOST
    run_test_options += --host
    prereq_rule := $(ART_TEST_HOST_RUN_TEST_DEPENDENCIES)
  else
    ifeq ($(2),target)
      uc_host_or_target := TARGET
      prereq_rule := test-art-target-sync
    else
      $$(error found $(2) expected host or target)
    endif
  endif
  ifeq ($(6),relocate)
    uc_reloc_type := RELOCATE
    run_test_options += --relocate
    ifneq ($(ART_TEST_RUN_TEST_RELOCATE),true)
      skip_test := true
    endif
  else
    ifeq ($(6),prebuild)
      uc_reloc_type := PREBUILD
      run_test_options += --relocate --prebuild
      ifneq ($(ART_TEST_RUN_TEST_PREBUILD),true)
        skip_test := true
      endif
    else
      ifeq ($(6),norelocate)
        uc_reloc_type := NORELOCATE
        run_test_options += --no-relocate
        ifneq ($(ART_TEST_RUN_TEST_NO_RELOCATE),true)
          skip_test := true
        endif
      else
        $$(error found $(6) expected relocate, norelocate or prebuild)
      endif
    endif
  endif
  uc_compiler :=
  ifeq ($(3),optimizing)
    uc_compiler := OPTIMIZING
    run_test_options += -Xcompiler-option --compiler-backend=Optimizing
    ifneq ($$(ART_TEST_OPTIMIZING),true)
      skip_test := true
    endif
  else
    ifeq ($(3),interpreter)
      uc_compiler := INTERPRETER
      run_test_options += --interpreter
    else
      ifeq ($(3),default)
        uc_compiler := DEFAULT
      else
        $$(error found $(3) expected optimizing, interpreter or default)
      endif
    endif
  endif
  ifeq ($(4),64)
    run_test_options += --64
  else
    ifneq ($(4),32)
      $$(error found $(4) expected 32 or 64)
    endif
  endif
  ifeq ($(5),trace)
    run_test_options += --trace
    run_test_rule_name := test-art-$(2)-run-test-trace-$(3)-$(6)-$(1)$(4)
    ifneq ($$(ART_TEST_TRACE),true)
      skip_test := true
    endif
  else
    ifeq ($(5),gcverify)
      run_test_options += --runtime-option -Xgc:preverify --runtime-option -Xgc:postverify \
        --runtime-option -Xgc:preverify_rosalloc --runtime-option -Xgc:postverify_rosalloc
      run_test_rule_name := test-art-$(2)-run-test-gcverify-$(3)-$(6)-$(1)$(4)
      ifneq ($$(ART_TEST_GC_VERIFY),true)
        skip_test := true
      endif
    else
      ifeq ($(5),gcstress)
        run_test_options += --runtime-option -Xgc:SS --runtime-option -Xms2m \
          --runtime-option -Xmx2m --runtime-option -Xgc:preverify --runtime-option -Xgc:postverify
        run_test_rule_name := test-art-$(2)-run-test-gcstress-$(3)-$(6)-$(1)$(4)
        ifneq ($$(ART_TEST_GC_STRESS),true)
          skip_test := true
        endif
      else
        ifneq (,$(5))
          $$(error found $(5) expected undefined or gcverify, gcstress or trace)
        endif
        run_test_rule_name := test-art-$(2)-run-test-$(3)-$(6)-$(1)$(4)
      endif
    endif
  endif
  ifeq ($$(skip_test),false)
    run_test_options := --output-path $(ART_HOST_TEST_DIR)/run-test-output/$$(run_test_rule_name) \
      $$(run_test_options)
$$(run_test_rule_name): PRIVATE_RUN_TEST_OPTIONS := $$(run_test_options)
.PHONY: $$(run_test_rule_name)
$$(run_test_rule_name): $(DX) $(HOST_OUT_EXECUTABLES)/jasmin $$(prereq_rule)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  DX=$(abspath $(DX)) JASMIN=$(abspath $(HOST_OUT_EXECUTABLES)/jasmin) \
	    art/test/run-test $$(PRIVATE_RUN_TEST_OPTIONS) $(1) \
	      && $$(call ART_TEST_PASSED,$$@) || $$(call ART_TEST_FAILED,$$@)
	$$(hide) (echo $(MAKECMDGOALS) | grep -q $$@ && \
	  echo "run-test run as top-level target, removing test directory $(ART_HOST_TEST_DIR)" && \
	  rm -r $(ART_HOST_TEST_DIR)) || true
  else
    .PHONY: $$(run_test_rule_name)
$$(run_test_rule_name):
  endif

  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$$(uc_compiler)$(4)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$$(uc_compiler)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$$(uc_compiler)_$(1)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$$(uc_compiler)_$$(uc_reloc_type)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$(1)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_ALL_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_$$(uc_reloc_type)_RULES += $$(run_test_rule_name)
  ART_TEST_$$(uc_host_or_target)_RUN_TEST_ALL$(4)_RULES += $$(run_test_rule_name)

  # Clear locally defined variables.
  skip_test :=
  run_test_options :=
  run_test_rule_name :=
  uc_host_or_target :=
  prereq_rule :=
  uc_reloc_type :=
  uc_compiler :=
endef  # define-test-art-run-test

# Define a phony rule whose purpose is to test its prerequisites.
# $(1): rule name, e.g. test-art-host-run-test32
# $(2): list of prerequisites
define define-test-art-run-test-group-rule
.PHONY: $(1)
$(1): $(2)
	$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

endef  # define-test-art-run-test-group-rule

# Create rules for a group of run tests.
# $(1): test name, e.g. 003-omnibus-opcodes
# $(2): host or target
# $(3): relocate, norelocate or prebuild
define define-test-art-run-test-group-type
  group_uc_host_or_target :=
  ifeq ($(2),host)
    group_uc_host_or_target := HOST
  else
    ifeq ($(2),target)
      group_uc_host_or_target := TARGET
    else
      $$(error found $(2) expected host or target)
    endif
  endif

  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_DEFAULT_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_INTERPRETER_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_OPTIMIZING_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_$(1)_RULES :=
  $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),trace,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),trace,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),trace,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcverify,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcverify,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcverify,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcstress,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcstress,$(3)))
  $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcstress,$(3)))
  do_second := false
  ifeq ($(2),host)
    ifneq ($$(HOST_PREFER_32_BIT),true)
      do_second := true
    endif
  else
    ifdef TARGET_2ND_ARCH
      do_second := true
    endif
  endif
  ifeq (true,$$(do_second))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),trace,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),trace,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),trace,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcverify,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcverify,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcverify,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),default,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcstress,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),interpreter,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcstress,$(3)))
    $$(eval $$(call define-test-art-run-test,$(1),$(2),optimizing,$$(2ND_ART_PHONY_TEST_$$(group_uc_host_or_target)_SUFFIX),gcstress,$(3)))
  endif
endef  # define-test-art-run-test-group-type

# Create rules for a group of run tests.
# $(1): test name, e.g. 003-omnibus-opcodes
# $(2): host or target
define define-test-art-run-test-group
  group_uc_host_or_target :=
  ifeq ($(2),host)
    group_uc_host_or_target := HOST
  else
    ifeq ($(2),target)
      group_uc_host_or_target := TARGET
    else
      $$(error found $(2) expected host or target)
    endif
  endif
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_DEFAULT_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_INTERPRETER_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_OPTIMIZING_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_$(1)_RULES :=
  $$(eval $$(call define-test-art-run-test-group-type,$(1),$(2),norelocate))
  $$(eval $$(call define-test-art-run-test-group-type,$(1),$(2),relocate))
  $$(eval $$(call define-test-art-run-test-group-type,$(1),$(2),prebuild))
  $$(eval $$(call define-test-art-run-test-group-rule,test-art-$(2)-run-test-default-$(1), \
    $$(ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_DEFAULT_$(1)_RULES)))
  $$(eval $$(call define-test-art-run-test-group-rule,test-art-$(2)-run-test-interpreter-$(1), \
    $$(ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_INTERPRETER_$(1)_RULES)))
  $$(eval $$(call define-test-art-run-test-group-rule,test-art-$(2)-run-test-optimizing-$(1), \
    $$(ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_OPTIMIZING_$(1)_RULES)))
  $$(eval $$(call define-test-art-run-test-group-rule,test-art-$(2)-run-test-$(1), \
    $$(ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_$(1)_RULES)))

  # Clear locally defined variables.
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_DEFAULT_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_INTERPRETER_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_OPTIMIZING_$(1)_RULES :=
  ART_TEST_$$(group_uc_host_or_target)_RUN_TEST_$(1)_RULES :=
  group_uc_host_or_target :=
  do_second :=
endef  # define-test-art-run-test-group

$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call define-test-art-run-test-group,$(test),target)))
$(foreach test, $(TEST_ART_RUN_TESTS), $(eval $(call define-test-art-run-test-group,$(test),host)))

$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-prebuild, \
  $(ART_TEST_TARGET_RUN_TEST_PREBUILD_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-norelocate, \
  $(ART_TEST_TARGET_RUN_TEST_NORELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-relocate, \
  $(ART_TEST_TARGET_RUN_TEST_RELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test, \
  $(ART_TEST_TARGET_RUN_TEST_ALL_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default, \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter, \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing, \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default-prebuild, \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT_PREBUILD_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter-prebuild, \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_PREBUILD_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing-prebuild, \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_PREBUILD_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default-norelocate, \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT_NORELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter-norelocate, \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_NORELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing-norelocate, \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_NORELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default-relocate, \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT_RELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter-relocate, \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_RELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing-relocate, \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_ALL$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-prebuild$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-norelocate$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-relocate$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default-prebuild$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter-prebuild$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing-prebuild$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default-norelocate$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter-norelocate$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing-norelocate$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default-relocate$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_DEFAULT_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter-relocate$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing-relocate$(ART_PHONY_TEST_TARGET_SUFFIX), \
  $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
ifdef TARGET_2ND_ARCH
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-prebuild$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-norelocate$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-relocate$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default-prebuild$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_DEFAULT_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter-prebuild$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing-prebuild$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default-norelocate$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_DEFAULT_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter-norelocate$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing-norelocate$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-default-relocate$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_DEFAULT_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-interpreter-relocate$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_INTERPRETER_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-target-run-test-optimizing-relocate$(2ND_ART_PHONY_TEST_TARGET_SUFFIX), \
    $(ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES)))
endif

$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-prebuild, \
  $(ART_TEST_HOST_RUN_TEST_PREBUILD_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-norelocate, \
  $(ART_TEST_HOST_RUN_TEST_NORELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-relocate, \
  $(ART_TEST_HOST_RUN_TEST_RELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test, \
  $(ART_TEST_HOST_RUN_TEST_ALL_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default, \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter, \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing, \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default-prebuild, \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT_PREBUILD_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter-prebuild, \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER_PREBUILD_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing-prebuild, \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_PREBUILD_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default-norelocate, \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT_NORELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter-norelocate, \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER_NORELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing-norelocate, \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_NORELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default-relocate, \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT_RELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter-relocate, \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER_RELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing-relocate, \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_RELOCATE_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_ALL$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-prebuild$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-norelocate$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-relocate$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default-prebuild$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter-prebuild$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing-prebuild$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default-norelocate$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter-norelocate$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing-norelocate$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default-relocate$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_DEFAULT_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter-relocate$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_INTERPRETER_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
$(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing-relocate$(ART_PHONY_TEST_HOST_SUFFIX), \
  $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
ifneq ($(HOST_PREFER_32_BIT),true)
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-prebuild$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-norelocate$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-relocate$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default-prebuild$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_DEFAULT_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter-prebuild$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_INTERPRETER_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing-prebuild$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default-norelocate$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_DEFAULT_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter-norelocate$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_INTERPRETER_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing-norelocate$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-default-relocate$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_DEFAULT_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-interpreter-relocate$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_INTERPRETER_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
  $(eval $(call define-test-art-run-test-group-rule,test-art-host-run-test-optimizing-relocate$(2ND_ART_PHONY_TEST_HOST_SUFFIX), \
    $(ART_TEST_HOST_RUN_TEST_OPTIMIZING_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES)))
endif

define-test-art-run-test :=
define-test-art-run-test-group-rule :=
define-test-art-run-test-group :=
TEST_ART_RUN_TESTS :=
ART_TEST_TARGET_RUN_TEST_ALL_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RULES :=
ART_TEST_TARGET_RUN_TEST_RELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_NORELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_NORELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_NORELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_NORELOCATE_RULES :=
ART_TEST_TARGET_RUN_TEST_PREBUILD_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_PREBUILD_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_PREBUILD_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_PREBUILD_RULES :=
ART_TEST_TARGET_RUN_TEST_ALL$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_NORELOCATE$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_PREBUILD$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_RELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_NORELOCATE$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_DEFAULT_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_INTERPRETER_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_RUN_TEST_OPTIMIZING_PREBUILD$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_ALL_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RULES :=
ART_TEST_HOST_RUN_TEST_RELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_NORELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_NORELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_NORELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_NORELOCATE_RULES :=
ART_TEST_HOST_RUN_TEST_PREBUILD_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_PREBUILD_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_PREBUILD_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_PREBUILD_RULES :=
ART_TEST_HOST_RUN_TEST_ALL$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_NORELOCATE$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_PREBUILD$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_ALL$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_RELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_NORELOCATE$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_DEFAULT_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_INTERPRETER_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_RUN_TEST_OPTIMIZING_PREBUILD$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
