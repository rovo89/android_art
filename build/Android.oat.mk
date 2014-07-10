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
# Rules to build a smaller "core" image to support core libraries
# (that is, non-Android frameworks) testing on the host and target
#
# The main rules to build the default "boot" image are in
# build/core/dex_preopt_libart.mk

include art/build/Android.common_path.mk

# Use dex2oat debug version for better error reporting
# $(1): 2ND_ or undefined, 2ND_ for 32-bit host builds.
define create-core-oat-host-rules
$$($(1)HOST_CORE_IMG_OUT): $$(HOST_CORE_DEX_FILES) $$(DEX2OATD_DEPENDENCY)
	@echo "host dex2oat: $$@ ($$?)"
	@mkdir -p $$(dir $$@)
	$$(hide) $$(DEX2OATD) --runtime-arg -Xms$(DEX2OAT_IMAGE_XMS) --runtime-arg -Xmx$(DEX2OAT_IMAGE_XMX) \
	  --image-classes=$$(PRELOADED_CLASSES) $$(addprefix --dex-file=,$$(HOST_CORE_DEX_FILES)) \
	  $$(addprefix --dex-location=,$$(HOST_CORE_DEX_LOCATIONS)) --oat-file=$$($(1)HOST_CORE_OAT_OUT) \
	  --oat-location=$$($(1)HOST_CORE_OAT) --image=$$($(1)HOST_CORE_IMG_OUT) \
	  --base=$$(LIBART_IMG_HOST_BASE_ADDRESS) --instruction-set=$$($(1)ART_HOST_ARCH) \
	  --instruction-set-features=$$($(1)HOST_INSTRUCTION_SET_FEATURES) \
	  --host --android-root=$$(HOST_OUT) --include-patch-information

# This "renaming" eases declaration in art/Android.mk
HOST_CORE_IMG_OUT$($(1)ART_PHONY_TEST_HOST_SUFFIX) := $($(1)HOST_CORE_IMG_OUT)

$$($(1)HOST_CORE_OAT_OUT): $$($(1)HOST_CORE_IMG_OUT)
endef  # create-core-oat-host-rules

$(eval $(call create-core-oat-host-rules,))
ifneq ($(HOST_PREFER_32_BIT),true)
$(eval $(call create-core-oat-host-rules,2ND_))
endif

define create-core-oat-target-rules
$$($(1)TARGET_CORE_IMG_OUT): $$($(1)TARGET_CORE_DEX_FILES) $$(DEX2OATD_DEPENDENCY)
	@echo "target dex2oat: $$@ ($$?)"
	@mkdir -p $$(dir $$@)
	$$(hide) $$(DEX2OATD) --runtime-arg -Xms$(DEX2OAT_XMS) --runtime-arg -Xmx$(DEX2OAT_XMX) \
	  --image-classes=$$(PRELOADED_CLASSES) $$(addprefix --dex-file=,$$(TARGET_CORE_DEX_FILES)) \
	  $$(addprefix --dex-location=,$$(TARGET_CORE_DEX_LOCATIONS)) --oat-file=$$($(1)TARGET_CORE_OAT_OUT) \
	  --oat-location=$$($(1)TARGET_CORE_OAT) --image=$$($(1)TARGET_CORE_IMG_OUT) \
	  --base=$$(LIBART_IMG_TARGET_BASE_ADDRESS) --instruction-set=$$($(1)TARGET_ARCH) \
	  --instruction-set-features=$$($(1)TARGET_INSTRUCTION_SET_FEATURES) \
	  --android-root=$$(PRODUCT_OUT)/system --include-patch-information

# This "renaming" eases declaration in art/Android.mk
TARGET_CORE_IMG_OUT$($(1)ART_PHONY_TEST_TARGET_SUFFIX) := $($(1)TARGET_CORE_IMG_OUT)

$$($(1)TARGET_CORE_OAT_OUT): $$($(1)TARGET_CORE_IMG_OUT)
endef  # create-core-oat-target-rules

ifdef TARGET_2ND_ARCH
$(eval $(call create-core-oat-target-rules,2ND_))
endif
$(eval $(call create-core-oat-target-rules,))


ifeq ($(ART_BUILD_HOST),true)
include $(CLEAR_VARS)
LOCAL_MODULE := core.art-host
LOCAL_MODULE_TAGS := optional
LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common.mk
LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.oat.mk
LOCAL_ADDITIONAL_DEPENDENCIES += $(HOST_CORE_IMG_OUT)
include $(BUILD_PHONY_PACKAGE)
endif # ART_BUILD_HOST

# If we aren't building the host toolchain, skip building the target core.art.
ifeq ($(ART_BUILD_TARGET),true)
include $(CLEAR_VARS)
LOCAL_MODULE := core.art
LOCAL_MODULE_TAGS := optional
LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common.mk
LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.oat.mk
LOCAL_ADDITIONAL_DEPENDENCIES += $(TARGET_CORE_IMG_OUT)
include $(BUILD_PHONY_PACKAGE)
endif # ART_BUILD_TARGET
