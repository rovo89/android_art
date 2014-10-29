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

include art/build/Android.common_build.mk

# Use dex2oat debug version for better error reporting
# $(1): compiler
# $(2): 2ND_ or undefined, 2ND_ for 32-bit host builds.
# NB depending on HOST_CORE_DEX_LOCATIONS so we are sure to have the dex files in frameworks for
# run-test --no-image
define create-core-oat-host-rules
  core_compile_options :=
  core_image_name :=
  core_oat_name :=
  core_infix :=

  ifeq ($(1),optimizing)
    core_compile_options += --compiler-backend=optimizing
    core_infix := -optimizing
  endif
  ifeq ($(1),interpreter)
    core_compile_options += --compiler-filter=interpret-only
    core_infix := -interpreter
  endif
  ifeq ($(1),default)
    # Default has no infix, no compile options.
  endif
  ifneq ($(filter-out default interpreter optimizing,$(1)),)
    #Technically this test is not precise, but hopefully good enough.
    $$(error found $(1) expected default, interpreter or optimizing)
  endif
  core_image_name := $($(2)HOST_CORE_IMG_OUT_BASE)$$(core_infix)$(CORE_IMG_SUFFIX)
  core_oat_name := $($(2)HOST_CORE_OAT_OUT_BASE)$$(core_infix)$(CORE_OAT_SUFFIX)

  # Using the bitness suffix makes it easier to add as a dependency for the run-test mk.
  ifeq ($(2),)
    HOST_CORE_IMAGE_$(1)_64 := $$(core_image_name)
  else
    HOST_CORE_IMAGE_$(1)_32 := $$(core_image_name)
  endif
  HOST_CORE_IMG_OUTS += $$(core_image_name)
  HOST_CORE_OAT_OUTS += $$(core_oat_name)

$$(core_image_name): PRIVATE_CORE_COMPILE_OPTIONS := $$(core_compile_options)
$$(core_image_name): PRIVATE_CORE_IMG_NAME := $$(core_image_name)
$$(core_image_name): PRIVATE_CORE_OAT_NAME := $$(core_oat_name)
$$(core_image_name): $$(HOST_CORE_DEX_LOCATIONS) $$(DEX2OAT_DEPENDENCY)
	@echo "host dex2oat: $$@ ($$?)"
	@mkdir -p $$(dir $$@)
	$$(hide) $$(DEX2OAT) --runtime-arg -Xms$(DEX2OAT_IMAGE_XMS) --runtime-arg -Xmx$(DEX2OAT_IMAGE_XMX) \
	  --image-classes=$$(PRELOADED_CLASSES) $$(addprefix --dex-file=,$$(HOST_CORE_DEX_FILES)) \
	  $$(addprefix --dex-location=,$$(HOST_CORE_DEX_LOCATIONS)) --oat-file=$$(PRIVATE_CORE_OAT_NAME) \
	  --oat-location=$$(PRIVATE_CORE_OAT_NAME) --image=$$(PRIVATE_CORE_IMG_NAME) \
	  --base=$$(LIBART_IMG_HOST_BASE_ADDRESS) --instruction-set=$$($(2)ART_HOST_ARCH) \
	  --instruction-set-features=$$($(2)HOST_INSTRUCTION_SET_FEATURES) \
	  --host --android-root=$$(HOST_OUT) --include-patch-information \
	  $$(PRIVATE_CORE_COMPILE_OPTIONS)

$$(core_oat_name): $$(core_image_name)

  # Clean up locally used variables.
  core_compile_options :=
  core_image_name :=
  core_oat_name :=
  core_infix :=
endef  # create-core-oat-host-rules

$(eval $(call create-core-oat-host-rules,default,))
$(eval $(call create-core-oat-host-rules,optimizing,))
$(eval $(call create-core-oat-host-rules,interpreter,))
ifneq ($(HOST_PREFER_32_BIT),true)
$(eval $(call create-core-oat-host-rules,default,2ND_))
$(eval $(call create-core-oat-host-rules,optimizing,2ND_))
$(eval $(call create-core-oat-host-rules,interpreter,2ND_))
endif

define create-core-oat-target-rules
  core_compile_options :=
  core_image_name :=
  core_oat_name :=
  core_infix :=

  ifeq ($(1),optimizing)
    core_compile_options += --compiler-backend=optimizing
    core_infix := -optimizing
  endif
  ifeq ($(1),interpreter)
    core_compile_options += --compiler-filter=interpret-only
    core_infix := -interpreter
  endif
  ifeq ($(1),default)
    # Default has no infix, no compile options.
  endif
  ifneq ($(filter-out default interpreter optimizing,$(1)),)
    #Technically this test is not precise, but hopefully good enough.
    $$(error found $(1) expected default, interpreter or optimizing)
  endif
  core_image_name := $($(2)TARGET_CORE_IMG_OUT_BASE)$$(core_infix)$(CORE_IMG_SUFFIX)
  core_oat_name := $($(2)TARGET_CORE_OAT_OUT_BASE)$$(core_infix)$(CORE_OAT_SUFFIX)

  # Using the bitness suffix makes it easier to add as a dependency for the run-test mk.
  ifeq ($(2),)
    TARGET_CORE_IMAGE_$(1)_64 := $$(core_image_name)
  else
    TARGET_CORE_IMAGE_$(1)_32 := $$(core_image_name)
  endif
  TARGET_CORE_IMG_OUTS += $$(core_image_name)
  TARGET_CORE_OAT_OUTS += $$(core_oat_name)

$$(core_image_name): PRIVATE_CORE_COMPILE_OPTIONS := $$(core_compile_options)
$$(core_image_name): PRIVATE_CORE_IMG_NAME := $$(core_image_name)
$$(core_image_name): PRIVATE_CORE_OAT_NAME := $$(core_oat_name)
$$(core_image_name): $$(TARGET_CORE_DEX_FILES) $$(DEX2OAT_DEPENDENCY)
	@echo "target dex2oat: $$@ ($$?)"
	@mkdir -p $$(dir $$@)
	$$(hide) $$(DEX2OAT) --runtime-arg -Xms$(DEX2OAT_XMS) --runtime-arg -Xmx$(DEX2OAT_XMX) \
	  --image-classes=$$(PRELOADED_CLASSES) $$(addprefix --dex-file=,$$(TARGET_CORE_DEX_FILES)) \
	  $$(addprefix --dex-location=,$$(TARGET_CORE_DEX_LOCATIONS)) --oat-file=$$(PRIVATE_CORE_OAT_NAME) \
	  --oat-location=$$(PRIVATE_CORE_OAT_NAME) --image=$$(PRIVATE_CORE_IMG_NAME) \
	  --base=$$(LIBART_IMG_TARGET_BASE_ADDRESS) --instruction-set=$$($(2)TARGET_ARCH) \
	  --instruction-set-features=$$($(2)TARGET_INSTRUCTION_SET_FEATURES) \
	  --android-root=$$(PRODUCT_OUT)/system --include-patch-information \
	  $$(PRIVATE_CORE_COMPILE_OPTIONS)

$$(core_oat_name): $$(core_image_name)

  # Clean up locally used variables.
  core_compile_options :=
  core_image_name :=
  core_oat_name :=
  core_infix :=
endef  # create-core-oat-target-rules

ifdef TARGET_2ND_ARCH
$(eval $(call create-core-oat-target-rules,default,2ND_))
$(eval $(call create-core-oat-target-rules,optimizing,2ND_))
$(eval $(call create-core-oat-target-rules,interpreter,2ND_))
endif
$(eval $(call create-core-oat-target-rules,default,))
$(eval $(call create-core-oat-target-rules,optimizing,))
$(eval $(call create-core-oat-target-rules,interpreter,))
