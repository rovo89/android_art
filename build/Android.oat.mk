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

DEX2OAT := $(HOST_OUT_EXECUTABLES)/dex2oat$(HOST_EXECUTABLE_SUFFIX)
DEX2OATD := $(HOST_OUT_EXECUTABLES)/dex2oatd$(HOST_EXECUTABLE_SUFFIX)
# TODO: for now, override with debug version for better error reporting
DEX2OAT := $(DEX2OATD)

# TODO: change DEX2OAT_DEPENDENCY to order-only prerequisite when output is stable
DEX2OAT_DEPENDENCY := $(DEX2OAT)    # when dex2oat changes, rebuild all oat files
DEX2OAT_DEPENDENCY := | $(DEX2OAT)  # only build dex2oat if needed to build oat files

OATDUMP := $(HOST_OUT_EXECUTABLES)/oatdump$(HOST_EXECUTABLE_SUFFIX)
OATDUMPD := $(HOST_OUT_EXECUTABLES)/oatdumpd$(HOST_EXECUTABLE_SUFFIX)
# TODO: for now, override with debug version for better error reporting
OATDUMP := $(OATDUMPD)

# start of image reserved address space
IMG_HOST_BASE_ADDRESS   := 0x60000000
IMG_TARGET_BASE_ADDRESS := 0x60000000

########################################################################
# A smaller libcore only oat file
HOST_CORE_JARS := core-hostdex
TARGET_CORE_JARS := core

HOST_CORE_DEX   := $(foreach jar,$(HOST_CORE_JARS),  $(HOST_OUT_JAVA_LIBRARIES)/$(jar).jar)
TARGET_CORE_DEX := $(foreach jar,$(TARGET_CORE_JARS),$(TARGET_OUT_JAVA_LIBRARIES)/$(jar).jar)

HOST_CORE_OAT := $(HOST_OUT_JAVA_LIBRARIES)/core.oat
TARGET_CORE_OAT := $(ART_TEST_OUT)/core.oat

HOST_CORE_IMG := $(HOST_OUT_JAVA_LIBRARIES)/core.art
TARGET_CORE_IMG := $(ART_TEST_OUT)/core.art

$(HOST_CORE_OAT): $(HOST_CORE_DEX) $(DEX2OAT_DEPENDENCY)
	@echo "host dex2oat: $@ ($<)"
	@mkdir -p $(dir $@)
	$(hide) $(DEX2OAT) -Xms16m -Xmx16m $(addprefix --dex-file=,$(filter-out $(DEX2OAT),$^)) --oat=$@ --image=$(HOST_CORE_IMG) --base=$(IMG_HOST_BASE_ADDRESS)

$(TARGET_CORE_OAT): $(TARGET_CORE_DEX) $(DEX2OAT_DEPENDENCY)
	@echo "target dex2oat: $@ ($<)"
	@mkdir -p $(dir $@)
	$(hide) $(DEX2OAT) -Xms32m -Xmx32m $(addprefix --dex-file=,$(filter-out $(DEX2OAT),$^)) --oat=$@ --image=$(TARGET_CORE_IMG) --base=$(IMG_TARGET_BASE_ADDRESS) --host-prefix=$(PRODUCT_OUT)

$(HOST_CORE_IMG): $(HOST_CORE_OAT)

$(TARGET_CORE_IMG): $(TARGET_CORE_OAT)

########################################################################
# The full system boot classpath
TARGET_BOOT_JARS := $(subst :, ,$(DEXPREOPT_BOOT_JARS))
TARGET_BOOT_DEX := $(foreach jar,$(TARGET_BOOT_JARS),$(TARGET_OUT_JAVA_LIBRARIES)/$(jar).jar)
TARGET_BOOT_OAT := $(ART_CACHE_OUT)/boot.oat
TARGET_BOOT_IMG := $(ART_CACHE_OUT)/boot.art

$(TARGET_BOOT_OAT): $(TARGET_BOOT_DEX) $(DEX2OAT_DEPENDENCY)
	@echo "target dex2oat: $@ ($<)"
	$(hide) $(DEX2OAT) -Xms256m -Xmx256m $(addprefix --dex-file=,$(filter-out $(DEX2OAT),$^)) --oat=$@ --image=$(TARGET_BOOT_IMG) --base=$(IMG_TARGET_BASE_ADDRESS) --host-prefix=$(PRODUCT_OUT)
