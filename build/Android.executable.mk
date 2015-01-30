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

include art/build/Android.common_build.mk

ART_HOST_EXECUTABLES ?=
ART_TARGET_EXECUTABLES ?=

ART_EXECUTABLES_CFLAGS :=

# $(1): executable ("d" will be appended for debug version)
# $(2): source
# $(3): extra shared libraries
# $(4): extra include directories
# $(5): target or host
# $(6): ndebug or debug
# $(7): value for LOCAL_MULTILIB (empty means default)
define build-art-executable
  ifneq ($(5),target)
    ifneq ($(5),host)
      $$(error expected target or host for argument 5, received $(5))
    endif
  endif
  ifneq ($(6),ndebug)
    ifneq ($(6),debug)
      $$(error expected ndebug or debug for argument 6, received $(6))
    endif
  endif

  art_executable := $(1)
  art_source := $(2)
  art_shared_libraries := $(3)
  art_c_includes := $(4)
  art_target_or_host := $(5)
  art_ndebug_or_debug := $(6)
  art_multilib := $(7)
  art_out_binary_name :=

  include $(CLEAR_VARS)
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  LOCAL_MODULE_TAGS := optional
  LOCAL_SRC_FILES := $$(art_source)
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES) art/runtime art/cmdline $$(art_c_includes)
  LOCAL_SHARED_LIBRARIES += $$(art_shared_libraries)
  LOCAL_WHOLE_STATIC_LIBRARIES += libsigchain

  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := $$(art_executable)
  else #debug
    LOCAL_MODULE := $$(art_executable)d
  endif

  LOCAL_CFLAGS := $(ART_EXECUTABLES_CFLAGS)
  # Mac OS linker doesn't understand --export-dynamic.
  ifneq ($$(HOST_OS)-$$(art_target_or_host),darwin-host)
    LOCAL_LDFLAGS := -Wl,--export-dynamic
  endif

  ifeq ($$(art_target_or_host),target)
  	$(call set-target-local-clang-vars)
  	$(call set-target-local-cflags-vars,$(6))
    LOCAL_SHARED_LIBRARIES += libdl
  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_LDLIBS := $(ART_HOST_LDLIBS)
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS)
    ifeq ($$(art_ndebug_or_debug),debug)
      LOCAL_CFLAGS += $(ART_HOST_DEBUG_CFLAGS)
    else
      LOCAL_CFLAGS += $(ART_HOST_NON_DEBUG_CFLAGS)
    endif
    LOCAL_LDLIBS += -lpthread -ldl
  endif

  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_SHARED_LIBRARIES += libart
  else # debug
    LOCAL_SHARED_LIBRARIES += libartd
  endif

  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.common_utils.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.executable.mk

  ifeq ($$(art_target_or_host),target)
    LOCAL_MODULE_TARGET_ARCH := $(ART_SUPPORTED_ARCH)
  endif

  LOCAL_MULTILIB := $$(art_multilib)
  art_out_binary_name := $$(LOCAL_MODULE)

  # If multilib=both (potentially building both 32-bit and 64-bit), need to provide stem.
  ifeq ($$(art_multilib),both)
    # Set up a 32-bit/64-bit stem if we are building both binaries.
    # In this case, the 32-bit binary has an additional 32-bit suffix.
    LOCAL_MODULE_STEM_32 := $$(LOCAL_MODULE)32
    LOCAL_MODULE_STEM_64 := $$(LOCAL_MODULE)

    # Remember the binary names so we can add them to the global art executables list later.
    art_out_binary_name := $$(LOCAL_MODULE_STEM_32) $$(LOCAL_MODULE_STEM_64)

    # For single-architecture targets, remove any binary name suffixes.
    ifeq ($$(art_target_or_host),target)
      ifeq (,$(TARGET_2ND_ARCH))
        LOCAL_MODULE_STEM_32 := $$(LOCAL_MODULE)
        art_out_binary_name := $$(LOCAL_MODULE)
      endif
    endif

    # For single-architecture hosts, remove any binary name suffixes.
    ifeq ($$(art_target_or_host),host)
      ifeq (,$(HOST_2ND_ARCH))
        LOCAL_MODULE_STEM_32 := $$(LOCAL_MODULE)
        art_out_binary_name := $$(LOCAL_MODULE)
      endif
    endif
  endif

  LOCAL_NATIVE_COVERAGE := $(ART_COVERAGE)

  ifeq ($$(art_target_or_host),target)
    include $(BUILD_EXECUTABLE)
    ART_TARGET_EXECUTABLES := $(ART_TARGET_EXECUTABLES) $$(foreach name,$$(art_out_binary_name),$(TARGET_OUT_EXECUTABLES)/$$(name))
  else # host
    LOCAL_IS_HOST_MODULE := true
    include $(BUILD_HOST_EXECUTABLE)
    ART_HOST_EXECUTABLES := $(ART_HOST_EXECUTABLES) $$(foreach name,$$(art_out_binary_name),$(HOST_OUT_EXECUTABLES)/$$(name))
  endif

  # Clear out local variables now that we're done with them.
  art_executable :=
  art_source :=
  art_shared_libraries :=
  art_c_includes :=
  art_target_or_host :=
  art_ndebug_or_debug :=
  art_multilib :=
  art_out_binary_name :=

endef

#
# Build many art executables from multiple variations (debug/ndebug, host/target, 32/64bit).
# By default only either 32-bit or 64-bit is built (but not both -- see multilib arg).
# All other variations are gated by ANDROID_BUILD_(TARGET|HOST)_[N]DEBUG.
# The result must be eval-uated.
#
# $(1): executable name
# $(2): source files
# $(3): library dependencies (common); debug prefix is added on as necessary automatically.
# $(4): library dependencies (target only)
# $(5): library dependencies (host only)
# $(6): extra include directories
# $(7): multilib (default: empty), valid values: {,32,64,both})
define build-art-multi-executable
  $(foreach debug_flavor,ndebug debug,
    $(foreach target_flavor,host target,
      art-multi-binary-name := $(1)
      art-multi-source-files := $(2)
      art-multi-lib-dependencies := $(3)
      art-multi-lib-dependencies-target := $(4)
      art-multi-lib-dependencies-host := $(5)
      art-multi-include-extra := $(6)
      art-multi-multilib := $(7)

      # Add either -host or -target specific lib dependencies to the lib dependencies.
      art-multi-lib-dependencies += $$(art-multi-lib-dependencies-$(target_flavor))

      # Replace libart- prefix with libartd- for debug flavor.
      ifeq ($(debug_flavor),debug)
        art-multi-lib-dependencies := $$(subst libart-,libartd-,$$(art-multi-lib-dependencies))
      endif

      # Build the env guard var name, e.g. ART_BUILD_HOST_NDEBUG.
      art-multi-env-guard := $$(call art-string-to-uppercase,ART_BUILD_$(target_flavor)_$(debug_flavor))

      # Build the art executable only if the corresponding env guard was set.
      ifeq ($$($$(art-multi-env-guard)),true)
        $$(eval $$(call build-art-executable,$$(art-multi-binary-name),$$(art-multi-source-files),$$(art-multi-lib-dependencies),$$(art-multi-include-extra),$(target_flavor),$(debug_flavor),$$(art-multi-multilib)))
      endif

      # Clear locals now they've served their purpose.
      art-multi-binary-name :=
      art-multi-source-files :=
      art-multi-lib-dependencies :=
      art-multi-lib-dependencies-target :=
      art-multi-lib-dependencies-host :=
      art-multi-include-extra :=
      art-multi-multilib :=
      art-multi-env-guard :=
    )
  )
endef
