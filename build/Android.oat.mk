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

ifeq ($(DEX2OAT_HOST_INSTRUCTION_SET_FEATURES),)
  DEX2OAT_HOST_INSTRUCTION_SET_FEATURES := default
endif
ifeq ($($(HOST_2ND_ARCH_VAR_PREFIX)DEX2OAT_HOST_INSTRUCTION_SET_FEATURES),)
  $(HOST_2ND_ARCH_VAR_PREFIX)DEX2OAT_HOST_INSTRUCTION_SET_FEATURES := default
endif

# Use dex2oat debug version for better error reporting
# $(1): compiler - default, optimizing, jit or interpreter.
# $(2): pic/no-pic
# $(3): 2ND_ or undefined, 2ND_ for 32-bit host builds.
# $(4): wrapper, e.g., valgrind.
# $(5): dex2oat suffix, e.g, valgrind requires 32 right now.
# NB depending on HOST_CORE_DEX_LOCATIONS so we are sure to have the dex files in frameworks for
# run-test --no-image
define create-core-oat-host-rules
  core_compile_options :=
  core_image_name :=
  core_oat_name :=
  core_infix :=
  core_pic_infix :=
  core_dex2oat_dependency := $(DEX2OAT_DEPENDENCY)

  # With the optimizing compiler, we want to rerun dex2oat whenever there is
  # a dex2oat change to catch regressions early.
  ifeq ($(ART_USE_OPTIMIZING_COMPILER), true)
    core_dex2oat_dependency := $(DEX2OAT)
  endif

  ifeq ($(1),default)
    core_compile_options += --compiler-backend=Quick
  endif
  ifeq ($(1),optimizing)
    core_compile_options += --compiler-backend=Optimizing
    core_dex2oat_dependency := $(DEX2OAT)
    core_infix := -optimizing
  endif
  ifeq ($(1),interpreter)
    core_compile_options += --compiler-filter=interpret-only
    core_infix := -interpreter
  endif
  ifeq ($(1),default)
    # Default has no infix, no compile options.
  endif
  ifneq ($(filter-out default interpreter jit optimizing,$(1)),)
    #Technically this test is not precise, but hopefully good enough.
    $$(error found $(1) expected default, interpreter, jit or optimizing)
  endif

  ifeq ($(2),pic)
    core_compile_options += --compile-pic
    core_pic_infix := -pic
  endif
  ifeq ($(2),no-pic)
    # No change for non-pic
  endif
  ifneq ($(filter-out pic no-pic,$(2)),)
    # Technically this test is not precise, but hopefully good enough.
    $$(error found $(2) expected pic or no-pic)
  endif

  core_image_name := $($(3)HOST_CORE_IMG_OUT_BASE)$$(core_infix)$$(core_pic_infix)$(4)$(CORE_IMG_SUFFIX)
  core_oat_name := $($(3)HOST_CORE_OAT_OUT_BASE)$$(core_infix)$$(core_pic_infix)$(4)$(CORE_OAT_SUFFIX)

  # Using the bitness suffix makes it easier to add as a dependency for the run-test mk.
  ifeq ($(3),)
    $(4)HOST_CORE_IMAGE_$(1)_$(2)_64 := $$(core_image_name)
  else
    $(4)HOST_CORE_IMAGE_$(1)_$(2)_32 := $$(core_image_name)
  endif
  $(4)HOST_CORE_IMG_OUTS += $$(core_image_name)
  $(4)HOST_CORE_OAT_OUTS += $$(core_oat_name)

  # If we have a wrapper, make the target phony.
  ifneq ($(4),)
.PHONY: $$(core_image_name)
  endif
$$(core_image_name): PRIVATE_CORE_COMPILE_OPTIONS := $$(core_compile_options)
$$(core_image_name): PRIVATE_CORE_IMG_NAME := $$(core_image_name)
$$(core_image_name): PRIVATE_CORE_OAT_NAME := $$(core_oat_name)
$$(core_image_name): $$(HOST_CORE_DEX_LOCATIONS) $$(core_dex2oat_dependency)
	@echo "host dex2oat: $$@ ($$?)"
	@mkdir -p $$(dir $$@)
	$$(hide) $(4) $$(DEX2OAT)$(5) --runtime-arg -Xms$(DEX2OAT_IMAGE_XMS) \
	  --runtime-arg -Xmx$(DEX2OAT_IMAGE_XMX) \
	  --image-classes=$$(PRELOADED_CLASSES) $$(addprefix --dex-file=,$$(HOST_CORE_DEX_FILES)) \
	  $$(addprefix --dex-location=,$$(HOST_CORE_DEX_LOCATIONS)) --oat-file=$$(PRIVATE_CORE_OAT_NAME) \
	  --oat-location=$$(PRIVATE_CORE_OAT_NAME) --image=$$(PRIVATE_CORE_IMG_NAME) \
	  --base=$$(LIBART_IMG_HOST_BASE_ADDRESS) --instruction-set=$$($(3)ART_HOST_ARCH) \
	  --instruction-set-features=$$($(3)DEX2OAT_HOST_INSTRUCTION_SET_FEATURES) \
	  --host --android-root=$$(HOST_OUT) --include-patch-information --generate-debug-info \
	  $$(PRIVATE_CORE_COMPILE_OPTIONS)

$$(core_oat_name): $$(core_image_name)

  # Clean up locally used variables.
  core_dex2oat_dependency :=
  core_compile_options :=
  core_image_name :=
  core_oat_name :=
  core_infix :=
  core_pic_infix :=
endef  # create-core-oat-host-rules

# $(1): compiler - default, optimizing, jit or interpreter.
# $(2): wrapper.
# $(3): dex2oat suffix.
define create-core-oat-host-rule-combination
  $(call create-core-oat-host-rules,$(1),no-pic,,$(2),$(3))
  $(call create-core-oat-host-rules,$(1),pic,,$(2),$(3))

  ifneq ($(HOST_PREFER_32_BIT),true)
    $(call create-core-oat-host-rules,$(1),no-pic,2ND_,$(2),$(3))
    $(call create-core-oat-host-rules,$(1),pic,2ND_,$(2),$(3))
  endif
endef

$(eval $(call create-core-oat-host-rule-combination,default,,))
$(eval $(call create-core-oat-host-rule-combination,optimizing,,))
$(eval $(call create-core-oat-host-rule-combination,interpreter,,))

valgrindHOST_CORE_IMG_OUTS :=
valgrindHOST_CORE_OAT_OUTS :=
$(eval $(call create-core-oat-host-rule-combination,default,valgrind,32))
$(eval $(call create-core-oat-host-rule-combination,optimizing,valgrind,32))
$(eval $(call create-core-oat-host-rule-combination,interpreter,valgrind,32))

valgrind-test-art-host-dex2oat-host: $(valgrindHOST_CORE_IMG_OUTS)

define create-core-oat-target-rules
  core_compile_options :=
  core_image_name :=
  core_oat_name :=
  core_infix :=
  core_pic_infix :=
  core_dex2oat_dependency := $(DEX2OAT_DEPENDENCY)

  # With the optimizing compiler, we want to rerun dex2oat whenever there is
  # a dex2oat change to catch regressions early.
  ifeq ($(ART_USE_OPTIMIZING_COMPILER), true)
    core_dex2oat_dependency := $(DEX2OAT)
  endif

  ifeq ($(1),default)
    core_compile_options += --compiler-backend=Quick
  endif
  ifeq ($(1),optimizing)
    core_compile_options += --compiler-backend=Optimizing
    core_dex2oat_dependency := $(DEX2OAT)
    core_infix := -optimizing
  endif
  ifeq ($(1),interpreter)
    core_compile_options += --compiler-filter=interpret-only
    core_infix := -interpreter
  endif
  ifeq ($(1),default)
    # Default has no infix, no compile options.
  endif
  ifneq ($(filter-out default interpreter jit optimizing,$(1)),)
    # Technically this test is not precise, but hopefully good enough.
    $$(error found $(1) expected default, interpreter, jit or optimizing)
  endif

  ifeq ($(2),pic)
    core_compile_options += --compile-pic
    core_pic_infix := -pic
  endif
  ifeq ($(2),no-pic)
    # No change for non-pic
  endif
  ifneq ($(filter-out pic no-pic,$(2)),)
    #Technically this test is not precise, but hopefully good enough.
    $$(error found $(2) expected pic or no-pic)
  endif

  core_image_name := $($(3)TARGET_CORE_IMG_OUT_BASE)$$(core_infix)$$(core_pic_infix)$(4)$(CORE_IMG_SUFFIX)
  core_oat_name := $($(3)TARGET_CORE_OAT_OUT_BASE)$$(core_infix)$$(core_pic_infix)$(4)$(CORE_OAT_SUFFIX)

  # Using the bitness suffix makes it easier to add as a dependency for the run-test mk.
  ifeq ($(3),)
    ifdef TARGET_2ND_ARCH
      $(4)TARGET_CORE_IMAGE_$(1)_$(2)_64 := $$(core_image_name)
    else
      $(4)TARGET_CORE_IMAGE_$(1)_$(2)_32 := $$(core_image_name)
    endif
  else
    $(4)TARGET_CORE_IMAGE_$(1)_$(2)_32 := $$(core_image_name)
  endif
  $(4)TARGET_CORE_IMG_OUTS += $$(core_image_name)
  $(4)TARGET_CORE_OAT_OUTS += $$(core_oat_name)

  # If we have a wrapper, make the target phony.
  ifneq ($(4),)
.PHONY: $$(core_image_name)
  endif
$$(core_image_name): PRIVATE_CORE_COMPILE_OPTIONS := $$(core_compile_options)
$$(core_image_name): PRIVATE_CORE_IMG_NAME := $$(core_image_name)
$$(core_image_name): PRIVATE_CORE_OAT_NAME := $$(core_oat_name)
$$(core_image_name): $$(TARGET_CORE_DEX_FILES) $$(core_dex2oat_dependency)
	@echo "target dex2oat: $$@ ($$?)"
	@mkdir -p $$(dir $$@)
	$$(hide) $(4) $$(DEX2OAT)$(5) --runtime-arg -Xms$(DEX2OAT_IMAGE_XMS) \
	  --runtime-arg -Xmx$(DEX2OAT_IMAGE_XMX) \
	  --image-classes=$$(PRELOADED_CLASSES) $$(addprefix --dex-file=,$$(TARGET_CORE_DEX_FILES)) \
	  $$(addprefix --dex-location=,$$(TARGET_CORE_DEX_LOCATIONS)) --oat-file=$$(PRIVATE_CORE_OAT_NAME) \
	  --oat-location=$$(PRIVATE_CORE_OAT_NAME) --image=$$(PRIVATE_CORE_IMG_NAME) \
	  --base=$$(LIBART_IMG_TARGET_BASE_ADDRESS) --instruction-set=$$($(3)TARGET_ARCH) \
	  --instruction-set-variant=$$($(3)DEX2OAT_TARGET_CPU_VARIANT) \
	  --instruction-set-features=$$($(3)DEX2OAT_TARGET_INSTRUCTION_SET_FEATURES) \
	  --android-root=$$(PRODUCT_OUT)/system --include-patch-information --generate-debug-info \
	  $$(PRIVATE_CORE_COMPILE_OPTIONS) || (rm $$(PRIVATE_CORE_OAT_NAME); exit 1)

$$(core_oat_name): $$(core_image_name)

  # Clean up locally used variables.
  core_dex2oat_dependency :=
  core_compile_options :=
  core_image_name :=
  core_oat_name :=
  core_infix :=
  core_pic_infix :=
endef  # create-core-oat-target-rules

# $(1): compiler - default, optimizing, jit or interpreter.
# $(2): wrapper.
# $(3): dex2oat suffix.
define create-core-oat-target-rule-combination
  $(call create-core-oat-target-rules,$(1),no-pic,,$(2),$(3))
  $(call create-core-oat-target-rules,$(1),pic,,$(2),$(3))

  ifdef TARGET_2ND_ARCH
    $(call create-core-oat-target-rules,$(1),no-pic,2ND_,$(2),$(3))
    $(call create-core-oat-target-rules,$(1),pic,2ND_,$(2),$(3))
  endif
endef

$(eval $(call create-core-oat-target-rule-combination,default,,))
$(eval $(call create-core-oat-target-rule-combination,optimizing,,))
$(eval $(call create-core-oat-target-rule-combination,interpreter,,))

valgrindTARGET_CORE_IMG_OUTS :=
valgrindTARGET_CORE_OAT_OUTS :=
$(eval $(call create-core-oat-target-rule-combination,default,valgrind,32))
$(eval $(call create-core-oat-target-rule-combination,optimizing,valgrind,32))
$(eval $(call create-core-oat-target-rule-combination,interpreter,valgrind,32))

valgrind-test-art-host-dex2oat-target: $(valgrindTARGET_CORE_IMG_OUTS)

valgrind-test-art-host-dex2oat: valgrind-test-art-host-dex2oat-host valgrind-test-art-host-dex2oat-target
