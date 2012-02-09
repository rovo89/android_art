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
  include $(BUILD_JAVA_LIBRARY)
  ART_TEST_DEX_FILES += $(3)/$$(LOCAL_MODULE).jar
endef
$(foreach dir,$(TEST_DEX_DIRECTORIES), $(eval $(call build-art-test-dex,art-test-dex,$(dir),$(ART_NATIVETEST_OUT))))
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call build-art-test-dex,oat-test-dex,$(dir),$(ART_TEST_OUT))))

########################################################################

# $(1): input jar or apk filename
# $(2): input jar or apk target location
# $(3): output oat filename
# $(4): boot image
define build-art-oat
$(3): $(1) $(4) $(DEX2OAT_DEPENDENCY)
	@echo "target dex2oat: $$@ ($$?)"
	@mkdir -p $$(dir $$@)
	$(hide) $(DEX2OAT) --runtime-arg -Xms64m --runtime-arg -Xmx64m --boot-image=$(4) --dex-file=$(1) --dex-location=$(2) --oat-file=$$@ --host-prefix=$(PRODUCT_OUT)
endef

########################################################################
ART_TEST_OAT_FILES :=

# $(1): directory
define build-art-test-oat
  $(call build-art-oat,$(call intermediates-dir-for,JAVA_LIBRARIES,oat-test-dex-$(dir),,COMMON)/javalib.jar,$(ART_TEST_DIR)/oat-test-dex-$(1).jar,$(ART_TEST_OUT)/oat-test-dex-$(1).jar.oat,$(TARGET_CORE_IMG_OUT))
  ART_TEST_OAT_FILES += $(ART_TEST_OUT)/oat-test-dex-$(1).jar.oat
endef
ifneq (user,$(TARGET_BUILD_VARIANT))
  $(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call build-art-test-oat,$(dir))))
endif

########################################################################

ART_TEST_OAT_TARGETS :=

# $(1): directory
# $(2): arguments
define declare-test-test-target
.PHONY: test-art-target-oat-$(1)
test-art-target-oat-$(1): $(ART_TEST_OUT)/oat-test-dex-$(1).jar test-art-target-sync
	adb shell touch $(ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell rm $(ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell sh -c "oatexecd -Ximage:$(ART_TEST_DIR)/core.art -classpath $(ART_TEST_DIR)/oat-test-dex-$(1).jar -Djava.library.path=$(ART_TEST_DIR) $(1) $(2) && touch $(ART_TEST_DIR)/test-art-target-oat-$(1)"
	$(hide) (adb pull $(ART_TEST_DIR)/test-art-target-oat-$(1) /tmp/ && echo test-art-target-oat-$(1) PASSED) || (echo test-art-target-oat-$(1) FAILED && exit 1)
	$(hide) rm /tmp/test-art-target-oat-$(1)

ART_TEST_OAT_TARGETS += test-art-target-oat-$(1)
endef
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call declare-test-test-target,$(dir))))

########################################################################
