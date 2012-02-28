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

########################################################################

ART_TEST_DEX_FILES :=

# $(1): module prefix
# $(2): input test directory
# $(3): output module path
define build-art-test-dex
  include $(CLEAR_VARS)
  LOCAL_MODULE := $(1)-$(2)
  LOCAL_MODULE_TAGS := tests
  LOCAL_SRC_FILES := $(call all-java-files-under, test/$(2))
  LOCAL_JAVA_LIBRARIES := core
  LOCAL_NO_STANDARD_LIBRARIES := true
  LOCAL_MODULE_PATH := $(3)
  LOCAL_DEX_PREOPT_IMAGE := $(TARGET_CORE_IMG_OUT)
  include $(BUILD_JAVA_LIBRARY)
  ART_TEST_DEX_FILES += $(3)/$$(LOCAL_MODULE).jar
endef
$(foreach dir,$(TEST_DEX_DIRECTORIES), $(eval $(call build-art-test-dex,art-test-dex,$(dir),$(ART_NATIVETEST_OUT))))
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call build-art-test-dex,oat-test-dex,$(dir),$(ART_TEST_OUT))))

########################################################################

ART_TEST_OAT_TARGETS :=

# $(1): directory
# $(2): arguments
define declare-test-art-target
.PHONY: test-art-target-oat-$(1)
test-art-target-oat-$(1): $(ART_TEST_OUT)/oat-test-dex-$(1).jar test-art-target-sync
	adb shell touch $(ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell rm $(ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell sh -c "oatexecd -Ximage:$(ART_TEST_DIR)/core.art -classpath $(ART_TEST_DIR)/oat-test-dex-$(1).jar -Djava.library.path=$(ART_TEST_DIR) $(1) $(2) && touch $(ART_TEST_DIR)/test-art-target-oat-$(1)"
	$(hide) (adb pull $(ART_TEST_DIR)/test-art-target-oat-$(1) /tmp/ && echo test-art-target-oat-$(1) PASSED) || (echo test-art-target-oat-$(1) FAILED && exit 1)
	$(hide) rm /tmp/test-art-target-oat-$(1)

ART_TEST_OAT_TARGETS += test-art-target-oat-$(1)
endef
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call declare-test-art-target,$(dir))))

########################################################################
