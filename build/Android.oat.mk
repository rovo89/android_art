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

# start of oat reserved address space
OAT_HOST_BASE_ADDRESS := 0x50000000
OAT_TARGET_BASE_ADDRESS := 0x50000000

HOST_BOOT_OAT := $(HOST_OUT_JAVA_LIBRARIES)/boot.oat
TARGET_BOOT_OAT := $(TARGET_OUT_JAVA_LIBRARIES)/boot.oat

# TODO: just use libcore for now, not full bootclasspath.
# eventually need to replace with full list based on DEXPREOPT_BOOT_JARS.
HOST_BOOT_JARS := core-hostdex
TARGET_BOOT_JARS := core

HOST_BOOT_DEX   := $(foreach jar,$(HOST_BOOT_JARS),  $(HOST_OUT_JAVA_LIBRARIES)/$(jar).jar)
TARGET_BOOT_DEX := $(foreach jar,$(TARGET_BOOT_JARS),$(TARGET_OUT_JAVA_LIBRARIES)/$(jar).jar)

# TODO: change DEX2OATD to order-only prerequisite when output is stable
$(HOST_BOOT_OAT): $(HOST_BOOT_DEX) $(DEX2OAT)
	@echo "host dex2oat: $@ ($<)"
	$(hide) $(DEX2OAT) $(addprefix --dex-file=,$(filter-out $(DEX2OAT),$^)) --image=$@ --base=$(OAT_HOST_BASE_ADDRESS)

# TODO: change DEX2OATD to order-only prerequisite when output is stable
$(TARGET_BOOT_OAT): $(TARGET_BOOT_DEX) $(DEX2OAT)
	@echo "target dex2oat: $@ ($<)"
	$(hide) $(DEX2OAT) $(addprefix --dex-file=,$(filter-out $(DEX2OAT),$^)) --image=$@ --base=$(OAT_TARGET_BASE_ADDRESS) --strip-prefix=$(PRODUCT_OUT)
