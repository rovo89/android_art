# Copyright (C) 2012 The Android Open Source Project
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

TEST_ART_RUN_TEST_MAKE_TARGETS :=

# Helper to create individual build targets for tests.
# Must be called with $(eval)
# $(1): the test number
define declare-make-art-run-test
dmart_target := $(TARGET_OUT_DATA)/art-run-tests/$(1)/touch
$$(dmart_target):
	$(hide) rm -rf $$(dir $$@) && mkdir -p $$(dir $$@)
	$(hide) $(LOCAL_PATH)/run-test --build-only --output-path $$(abspath $$(dir $$@)) $(1)
	$(hide) touch $$@

TEST_ART_RUN_TEST_MAKE_TARGETS += $$(dmart_target)
dmart_target :=
endef

# Expand all tests.
$(foreach test, $(wildcard art/test/[0-9]*), $(eval $(call declare-make-art-run-test,$(notdir $(test)))))

include $(CLEAR_VARS)
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE := art-run-tests
LOCAL_ADDITIONAL_DEPENDENCIES := $(TEST_ART_RUN_TEST_MAKE_TARGETS)
include $(BUILD_PHONY_PACKAGE)

# clear temp vars
TEST_ART_RUN_TEST_MAKE_TARGETS :=
declare-make-art-run-test :=
