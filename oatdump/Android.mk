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

LOCAL_PATH := $(call my-dir)

include art/build/Android.executable.mk

OATDUMP_SRC_FILES := \
	oatdump.cc

# Build variants {target,host} x {debug,ndebug}
$(eval $(call build-art-multi-executable,oatdump,$(OATDUMP_SRC_FILES),libart-compiler libart-disassembler,libcutils,,art/compiler art/disassembler))

########################################################################
# oatdump targets

ART_DUMP_OAT_PATH ?= $(OUT_DIR)

OATDUMP := $(HOST_OUT_EXECUTABLES)/oatdump$(HOST_EXECUTABLE_SUFFIX)
OATDUMPD := $(HOST_OUT_EXECUTABLES)/oatdumpd$(HOST_EXECUTABLE_SUFFIX)
# TODO: for now, override with debug version for better error reporting
OATDUMP := $(OATDUMPD)

.PHONY: dump-oat
dump-oat: dump-oat-core dump-oat-boot

.PHONY: dump-oat-core
dump-oat-core: dump-oat-core-host dump-oat-core-target

.PHONY: dump-oat-core-host
ifeq ($(ART_BUILD_HOST),true)
dump-oat-core-host: $(HOST_CORE_IMG_OUTS) $(OATDUMP)
	$(OATDUMP) --image=$(HOST_CORE_IMG_LOCATION) --output=$(ART_DUMP_OAT_PATH)/core.host.oatdump.txt
	@echo Output in $(ART_DUMP_OAT_PATH)/core.host.oatdump.txt
endif

.PHONY: dump-oat-core-target
ifeq ($(ART_BUILD_TARGET),true)
dump-oat-core-target: $(TARGET_CORE_IMAGE_default_no-pic_32) $(OATDUMP)
	$(OATDUMP) --image=$(TARGET_CORE_IMG_LOCATION) \
	  --output=$(ART_DUMP_OAT_PATH)/core.target.oatdump.txt --instruction-set=$(TARGET_ARCH)
	@echo Output in $(ART_DUMP_OAT_PATH)/core.target.oatdump.txt
endif

.PHONY: dump-oat-boot-$(TARGET_ARCH)
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
dump-oat-boot-$(TARGET_ARCH): $(DEFAULT_DEX_PREOPT_BUILT_IMAGE_FILENAME) $(OATDUMP)
	$(OATDUMP) --image=$(DEFAULT_DEX_PREOPT_BUILT_IMAGE_LOCATION) \
	  --output=$(ART_DUMP_OAT_PATH)/boot.$(TARGET_ARCH).oatdump.txt --instruction-set=$(TARGET_ARCH)
	@echo Output in $(ART_DUMP_OAT_PATH)/boot.$(TARGET_ARCH).oatdump.txt
endif

ifdef TARGET_2ND_ARCH
dump-oat-boot-$(TARGET_2ND_ARCH): $(2ND_DEFAULT_DEX_PREOPT_BUILT_IMAGE_FILENAME) $(OATDUMP)
	$(OATDUMP) --image=$(2ND_DEFAULT_DEX_PREOPT_BUILT_IMAGE_LOCATION) \
	  --output=$(ART_DUMP_OAT_PATH)/boot.$(TARGET_2ND_ARCH).oatdump.txt --instruction-set=$(TARGET_2ND_ARCH)
	@echo Output in $(ART_DUMP_OAT_PATH)/boot.$(TARGET_2ND_ARCH).oatdump.txt
endif

.PHONY: dump-oat-boot
dump-oat-boot: dump-oat-boot-$(TARGET_ARCH)
ifdef TARGET_2ND_ARCH
dump-oat-boot: dump-oat-boot-$(TARGET_2ND_ARCH)
endif

.PHONY: dump-oat-Calculator
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
dump-oat-Calculator: $(TARGET_OUT_APPS)/Calculator.odex $(DEFAULT_DEX_PREOPT_BUILT_IMAGE) $(OATDUMP)
	$(OATDUMP) --oat-file=$< --output=$(ART_DUMP_OAT_PATH)/Calculator.oatdump.txt
	@echo Output in $(ART_DUMP_OAT_PATH)/Calculator.oatdump.txt
endif
