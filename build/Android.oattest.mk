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

# $(1): directory
define build-art-test-dex
  include $(CLEAR_VARS)
  LOCAL_MODULE := art-test-dex-$(1)
  LOCAL_MODULE_TAGS := optional
  LOCAL_SRC_FILES := $(call all-java-files-under, test/$(1))
  LOCAL_JAVA_LIBRARIES := core
  LOCAL_NO_STANDARD_LIBRARIES := true
  LOCAL_MODULE_PATH := $(ART_TEST_OUT)
  include $(BUILD_JAVA_LIBRARY)
  ART_TEST_DEX_FILES += $(TARGET_OUT_JAVA_LIBRARIES)/$$(LOCAL_MODULE).jar
endef
$(foreach dir,$(TEST_DEX_DIRECTORIES), $(eval $(call build-art-test-dex,$(dir))))

########################################################################

# $(1): input jar or apk filename
# $(2): output oat filename
# $(3): boot image
define build-art-oat
$(2): $(1) $(3) $(DEX2OAT_DEPENDENCY)
	@echo "target dex2oat: $$@ ($$?)"
	@mkdir -p $$(dir $$@)
	$(hide) $(DEX2OAT) --runtime-arg -Xms64m --runtime-arg -Xmx64m --boot-image=$(3) $(addprefix --dex-file=,$$<) --oat-file=$$@ --host-prefix=$(PRODUCT_OUT)
endef

########################################################################
ART_TEST_OAT_FILES :=

# $(1): directory
define build-art-test-oat
  $(call build-art-oat,$(ART_TEST_OUT)/art-test-dex-$(1).jar,$(ART_TEST_OUT)/art-test-dex-$(1).jar.oat,$(TARGET_CORE_IMG))
  ART_TEST_OAT_FILES += $(ART_TEST_OUT)/art-test-dex-$(1).jar.oat
endef
$(foreach dir,$(TEST_DEX_DIRECTORIES), $(eval $(call build-art-test-oat,$(dir))))

########################################################################

ART_TEST_OAT_TARGETS :=

# $(1): directory
# $(2): arguments
define declare-test-test-target
.PHONY: test-art-target-oat-$(1)
test-art-target-oat-$(1): $(ART_TEST_OUT)/art-test-dex-$(1).jar test-art-target-sync
	adb shell touch $(ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell rm $(ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell sh -c "oatexecd -Ximage:$(ART_TEST_DIR)/core.art -classpath $(ART_TEST_DIR)/art-test-dex-$(1).jar -Djava.library.path=$(ART_TEST_DIR) $(1) $(2) && touch $(ART_TEST_DIR)/test-art-target-oat-$(1)"
	$(hide) (adb pull $(ART_TEST_DIR)/test-art-target-oat-$(1) /tmp/ && echo test-art-target-oat-$(1) PASSED) || (echo test-art-target-oat-$(1) FAILED && exit 1)
	$(hide) rm /tmp/test-art-target-oat-$(1)

ART_TEST_OAT_TARGETS += test-art-target-oat-$(1)
endef

# Declare the simplest test first
$(eval $(call declare-test-test-target,HelloWorld,))
$(eval $(call declare-test-test-target,Fibonacci,10))

# The rest are alphabetical
$(eval $(call declare-test-test-target,ExceptionTest,))
$(eval $(call declare-test-test-target,GrowthLimit,))
$(eval $(call declare-test-test-target,IntMath,))
$(eval $(call declare-test-test-target,Invoke,))
$(eval $(call declare-test-test-target,MemUsage,))
$(eval $(call declare-test-test-target,ParallelGC,))
$(eval $(call declare-test-test-target,ReferenceMap,))
$(eval $(call declare-test-test-target,ReflectionTest,))
$(eval $(call declare-test-test-target,StackWalk,))
$(eval $(call declare-test-test-target,ThreadStress,))

# TODO: Enable when the StackWalk2 tests are passing
# $(eval $(call declare-test-test-target,StackWalk2,))

########################################################################
