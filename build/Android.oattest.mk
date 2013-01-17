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

ART_TEST_TARGET_DEX_FILES :=
ART_TEST_HOST_DEX_FILES :=

# $(1): module prefix
# $(2): input test directory
# $(3): target output module path (default module path is used on host)
define build-art-test-dex
  include $(CLEAR_VARS)
  LOCAL_MODULE := $(1)-$(2)
  LOCAL_MODULE_TAGS := tests
  LOCAL_SRC_FILES := $(call all-java-files-under, test/$(2))
  LOCAL_JAVA_LIBRARIES := $(TARGET_CORE_JARS)
  LOCAL_NO_STANDARD_LIBRARIES := true
  LOCAL_MODULE_PATH := $(3)
  LOCAL_DEX_PREOPT_IMAGE := $(TARGET_CORE_IMG_OUT)
  LOCAL_DEX_PREOPT := false
  include $(BUILD_JAVA_LIBRARY)
  ART_TEST_TARGET_DEX_FILES += $(3)/$$(LOCAL_MODULE).jar

  include $(CLEAR_VARS)
  LOCAL_MODULE := $(1)-$(2)
  LOCAL_SRC_FILES := $(call all-java-files-under, test/$(2))
  LOCAL_JAVA_LIBRARIES := $(HOST_CORE_JARS)
  LOCAL_NO_STANDARD_LIBRARIES := true
  LOCAL_DEX_PREOPT_IMAGE := $(HOST_CORE_IMG_OUT)
  LOCAL_BUILD_HOST_DEX := true
  include $(BUILD_HOST_JAVA_LIBRARY)
  ART_TEST_HOST_DEX_FILES += $$(LOCAL_MODULE_PATH)/$$(LOCAL_MODULE).jar
endef
$(foreach dir,$(TEST_DEX_DIRECTORIES), $(eval $(call build-art-test-dex,art-test-dex,$(dir),$(ART_NATIVETEST_OUT))))
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call build-art-test-dex,oat-test-dex,$(dir),$(ART_TEST_OUT))))

########################################################################

ART_TEST_TARGET_OAT_TARGETS :=
ART_TEST_HOST_OAT_TARGETS :=
ART_TEST_HOST_INTERPRETER_OAT_TARGETS :=

# $(1): directory
# $(2): arguments
define declare-test-art-oat-targets
.PHONY: test-art-target-oat-$(1)
test-art-target-oat-$(1): $(ART_TEST_OUT)/oat-test-dex-$(1).jar test-art-target-sync
	adb shell touch $(ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell rm $(ART_TEST_DIR)/test-art-target-oat-$(1)
	adb shell sh -c "oatexecd -Ximage:$(ART_TEST_DIR)/core.art -classpath $(ART_TEST_DIR)/oat-test-dex-$(1).jar -Djava.library.path=$(ART_TEST_DIR) $(1) $(2) && touch $(ART_TEST_DIR)/test-art-target-oat-$(1)"
	$(hide) (adb pull $(ART_TEST_DIR)/test-art-target-oat-$(1) /tmp/ && echo test-art-target-oat-$(1) PASSED) || (echo test-art-target-oat-$(1) FAILED && exit 1)
	$(hide) rm /tmp/test-art-target-oat-$(1)

.PHONY: test-art-host-oat-$(1)
test-art-host-oat-$(1): $(ART_TEST_OUT)/oat-test-dex-$(1).jar test-art-host-dependencies
	mkdir -p /tmp/android-data/test-art-host-oat-$(1)
	$(DEX2OAT) --runtime-arg -Xms16m --runtime-arg -Xmx16m --boot-image=$(HOST_CORE_IMG_OUT) --dex-file=$(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).jar --oat-file=$(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).jar.oat --instruction-set=$(HOST_ARCH) --host-prefix=""
	ANDROID_DATA=/tmp/android-data/test-art-host-oat-$(1) \
	  ANDROID_ROOT=$(HOST_OUT) \
	  LD_LIBRARY_PATH=$(HOST_OUT_SHARED_LIBRARIES) \
	  oatexecd -Ximage:$(shell pwd)/$(HOST_CORE_IMG_OUT) -classpath $(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).jar -Djava.library.path=$(HOST_OUT_SHARED_LIBRARIES) $(1) $(2) \
          && echo test-art-host-oat-$(1) PASSED || (echo test-art-host-oat-$(1) FAILED && exit 1)
	$(hide) rm -r /tmp/android-data/test-art-host-oat-$(1)

.PHONY: test-art-host-interpreter-oat-$(1)
test-art-host-interpreter-oat-$(1): $(ART_TEST_OUT)/oat-test-dex-$(1).jar test-art-host-dependencies
	mkdir -p /tmp/android-data/test-art-host-interpreter-oat-$(1)
	$(DEX2OAT) --runtime-arg -Xms16m --runtime-arg -Xmx16m --boot-image=$(HOST_CORE_IMG_OUT) --dex-file=$(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).jar --oat-file=$(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).jar.oat --instruction-set=$(HOST_ARCH) --host-prefix=""
	ANDROID_DATA=/tmp/android-data/test-art-host-interpreter-oat-$(1) \
	  ANDROID_ROOT=$(HOST_OUT) \
	  LD_LIBRARY_PATH=$(HOST_OUT_SHARED_LIBRARIES) \
	  oatexecd -Ximage:$(shell pwd)/$(HOST_CORE_IMG_OUT) -Xint -classpath $(HOST_OUT_JAVA_LIBRARIES)/oat-test-dex-$(1).jar -Djava.library.path=$(HOST_OUT_SHARED_LIBRARIES) $(1) $(2) \
          && echo test-art-host-interpreter-oat-$(1) PASSED || (echo test-art-host-interpreter-oat-$(1) FAILED && exit 1)
	$(hide) rm -r /tmp/android-data/test-art-host-interpreter-oat-$(1)

ART_TEST_TARGET_OAT_TARGETS += test-art-target-oat-$(1)
ART_TEST_HOST_OAT_TARGETS += test-art-host-oat-$(1)
ART_TEST_HOST_INTERPRETER_OAT_TARGETS += test-art-host-interpreter-oat-$(1)
endef
$(foreach dir,$(TEST_OAT_DIRECTORIES), $(eval $(call declare-test-art-oat-targets,$(dir))))

########################################################################
