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

# DEX2OAT defined in build/core/config.mk
DEX2OATD := $(HOST_OUT_EXECUTABLES)/dex2oatd$(HOST_EXECUTABLE_SUFFIX)
# TODO: for now, override with debug version for better error reporting
DEX2OAT := $(DEX2OATD)

# TODO: change DEX2OAT_DEPENDENCY to order-only prerequisite when output is stable
# DEX2OAT_DEPENDENCY := | $(DEX2OAT)  # only build dex2oat if needed to build oat files
DEX2OAT_DEPENDENCY := $(DEX2OAT)    # when dex2oat changes, rebuild all oat files

OATDUMP := $(HOST_OUT_EXECUTABLES)/oatdump$(HOST_EXECUTABLE_SUFFIX)
OATDUMPD := $(HOST_OUT_EXECUTABLES)/oatdumpd$(HOST_EXECUTABLE_SUFFIX)
# TODO: for now, override with debug version for better error reporting
OATDUMP := $(OATDUMPD)

# start of image reserved address space
IMG_HOST_BASE_ADDRESS   := 0x60000000
IMG_TARGET_BASE_ADDRESS := 0x60000000

PRELOADED_CLASSES := frameworks/base/preloaded-classes

########################################################################
# A smaller libcore only oat file
TARGET_CORE_JARS := core core-junit bouncycastle
HOST_CORE_JARS := $(addsuffix -hostdex,$(TARGET_CORE_JARS))

HOST_CORE_DEX_LOCATIONS   := $(foreach jar,$(HOST_CORE_JARS),  $(HOST_OUT_JAVA_LIBRARIES)/$(jar).jar)
TARGET_CORE_DEX_LOCATIONS := $(foreach jar,$(TARGET_CORE_JARS),/$(DEXPREOPT_BOOT_JAR_DIR)/$(jar).jar)

HOST_CORE_DEX_FILES   := $(foreach jar,$(HOST_CORE_JARS),  $(call intermediates-dir-for,JAVA_LIBRARIES,$(jar),t,COMMON)/javalib.jar)
TARGET_CORE_DEX_FILES := $(foreach jar,$(TARGET_CORE_JARS),$(call intermediates-dir-for,JAVA_LIBRARIES,$(jar), ,COMMON)/javalib.jar)

HOST_CORE_OAT := $(HOST_OUT_JAVA_LIBRARIES)/core.oat
TARGET_CORE_OAT := $(ART_TEST_DIR)/core.oat

HOST_CORE_OAT_OUT := $(HOST_OUT_JAVA_LIBRARIES)/core.oat
TARGET_CORE_OAT_OUT := $(ART_TEST_OUT)/core.oat

HOST_CORE_IMG_OUT := $(HOST_OUT_JAVA_LIBRARIES)/core.art
TARGET_CORE_IMG_OUT := $(ART_TEST_OUT)/core.art

$(HOST_CORE_IMG_OUT): $(HOST_CORE_DEX_FILES) $(DEX2OAT_DEPENDENCY)
	@echo "host dex2oat: $@ ($?)"
	@mkdir -p $(dir $@)
	$(hide) $(DEX2OAT) --runtime-arg -Xms16m --runtime-arg -Xmx16m --image-classes=$(PRELOADED_CLASSES) $(addprefix --dex-file=,$(HOST_CORE_DEX_FILES)) $(addprefix --dex-location=,$(HOST_CORE_DEX_LOCATIONS)) --oat-file=$(HOST_CORE_OAT_OUT) --oat-location=$(HOST_CORE_OAT) --image=$(HOST_CORE_IMG_OUT) --base=$(IMG_HOST_BASE_ADDRESS)

$(TARGET_CORE_IMG_OUT): $(TARGET_CORE_DEX_FILES) $(DEX2OAT_DEPENDENCY)
	@echo "target dex2oat: $@ ($?)"
	@mkdir -p $(dir $@)
	$(hide) $(DEX2OAT) --runtime-arg -Xms16m --runtime-arg -Xmx16m --image-classes=$(PRELOADED_CLASSES) $(addprefix --dex-file=,$(TARGET_CORE_DEX_FILES)) $(addprefix --dex-location=,$(TARGET_CORE_DEX_LOCATIONS)) --oat-file=$(TARGET_CORE_OAT_OUT) --oat-location=$(TARGET_CORE_OAT) --image=$(TARGET_CORE_IMG_OUT) --base=$(IMG_TARGET_BASE_ADDRESS) --host-prefix=$(PRODUCT_OUT)

$(HOST_CORE_OAT_OUT): $(HOST_CORE_IMG_OUT)

$(TARGET_CORE_OAT_OUT): $(TARGET_CORE_IMG_OUT)

########################################################################
# The full system boot classpath
TARGET_BOOT_JARS := $(subst :, ,$(DEXPREOPT_BOOT_JARS))
TARGET_BOOT_DEX_LOCATIONS := $(foreach jar,$(TARGET_BOOT_JARS),/$(DEXPREOPT_BOOT_JAR_DIR)/$(jar).jar)
TARGET_BOOT_DEX_FILES := $(foreach jar,$(TARGET_BOOT_JARS),$(call intermediates-dir-for,JAVA_LIBRARIES,$(jar),,COMMON)/javalib.jar)

TARGET_BOOT_IMG_OUT := $(DEFAULT_DEX_PREOPT_IMAGE)
TARGET_BOOT_OAT_OUT := $(patsubst %.art,%.oat,$(TARGET_BOOT_IMG_OUT))
TARGET_BOOT_OAT := $(subst $(PRODUCT_OUT),,$(TARGET_BOOT_OAT_OUT))

$(TARGET_BOOT_IMG_OUT): $(TARGET_BOOT_DEX_FILES) $(DEX2OAT_DEPENDENCY)
	@echo "target dex2oat: $@ ($?)"
	@mkdir -p $(dir $@)
	$(hide) $(DEX2OAT) --runtime-arg -Xms256m --runtime-arg -Xmx256m --image-classes=$(PRELOADED_CLASSES) $(addprefix --dex-file=,$(TARGET_BOOT_DEX_FILES)) $(addprefix --dex-location=,$(TARGET_BOOT_DEX_LOCATIONS)) --oat-file=$(TARGET_BOOT_OAT_OUT) --oat-location=$(TARGET_BOOT_OAT) --image=$(TARGET_BOOT_IMG_OUT) --base=$(IMG_TARGET_BASE_ADDRESS) --host-prefix=$(PRODUCT_OUT)

$(TARGET_BOOT_OAT_OUT): $(TARGET_BOOT_IMG_OUT)

include $(CLEAR_VARS)
LOCAL_MODULE := boot.art
LOCAL_MODULE_TAGS := optional
LOCAL_ADDITIONAL_DEPENDENCIES := $(TARGET_BOOT_IMG_OUT)
include $(BUILD_PHONY_PACKAGE)
