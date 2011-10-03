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
  include $(BUILD_JAVA_LIBRARY)
  ART_TEST_DEX_FILES += $(TARGET_OUT_JAVA_LIBRARIES)/$$(LOCAL_MODULE).jar
endef
$(foreach dir,$(TEST_DEX_DIRECTORIES), $(eval $(call build-art-test-dex,$(dir))))

########################################################################

# $(1): input jar or apk filename
# $(2): boot oat
# $(3): boot dex files
define build-art-oat
# TODO: change DEX2OATD (and perhaps $(2) boot oat) to order-only prerequisite when output is stable
$(patsubst %.apk,%.oat,$(patsubst %.jar,%.oat,$(1))): $(1) $(2) $(DEX2OAT)
	@echo "target dex2oat: $$@ ($$<)"
	$(hide) $(DEX2OAT) -Xms16m -Xmx16m $(addprefix --boot-dex-file=,$(3)) --boot-oat=$(2) --boot-image=$(patsubst %.oat,%.art,$(2)) $(addprefix --dex-file=,$$<) --oat=$$@ --image=$$(patsubst %.oat,%.art,$$@) --strip-prefix=$(PRODUCT_OUT)
endef

########################################################################
ART_TEST_OAT_FILES :=

# $(1): directory
define build-art-test-oat
  $(call build-art-oat,$(TARGET_OUT_JAVA_LIBRARIES)/art-test-dex-$(1).jar,$(TARGET_CORE_OAT),$(TARGET_CORE_DEX))
  ART_TEST_OAT_FILES += $(TARGET_OUT_JAVA_LIBRARIES)/art-test-dex-$(1).oat
endef
$(foreach dir,$(TEST_DEX_DIRECTORIES), $(eval $(call build-art-test-oat,$(dir))))

########################################################################

ART_TEST_OAT_TARGETS :=

# $(1): directory
# $(2): arguments
define declare-test-test-target
.PHONY: test-art-target-oat-$(1)
test-art-target-oat-$(1): test-art-target-sync
	adb shell touch /sdcard/test-art-target-oat-$(1)
	adb shell rm /sdcard/test-art-target-oat-$(1)
	adb shell sh -c "oatexecd -Xbootclasspath:/system/framework/core.jar -Xbootoat:/system/framework/core.oat -Xbootimage:/system/framework/core.art -classpath /system/framework/art-test-dex-$(1).jar -Xoat:/system/framework/art-test-dex-$(1).oat -Ximage:/system/framework/art-test-dex-$(1).art $(1) $(2) && touch /sdcard/test-art-target-oat-$(1)"
	$(hide) (adb pull /sdcard/test-art-target-oat-$(1) /tmp/ && echo test-art-target-oat-$(1) PASSED) || (echo test-art-target-oat-$(1) FAILED && exit 1)
	$(hide) rm /tmp/test-art-target-oat-$(1)

ART_TEST_OAT_TARGETS += test-art-target-oat-$(1)
endef

$(eval $(call declare-test-test-target,HelloWorld,))
$(eval $(call declare-test-test-target,Fibonacci,10))
$(eval $(call declare-test-test-target,IntMath,))
$(eval $(call declare-test-test-target,Invoke,))
$(eval $(call declare-test-test-target,ExceptionTest,))
$(eval $(call declare-test-test-target,SystemMethods,))
# TODO: Re-enable the test when System.LoadLibrary is working.
# $(eval $(call declare-test-test-target,StackWalk,))
# $(eval $(call declare-test-test-target,StackWalk2,))

 $(eval $(call declare-test-test-target,MemUsage,))

########################################################################
