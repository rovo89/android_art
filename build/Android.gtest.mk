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

LOCAL_PATH := art

RUNTIME_GTEST_COMMON_SRC_FILES := \
	runtime/arch/arch_test.cc \
	runtime/arch/stub_test.cc \
	runtime/barrier_test.cc \
	runtime/base/bit_field_test.cc \
	runtime/base/bit_vector_test.cc \
	runtime/base/hex_dump_test.cc \
	runtime/base/histogram_test.cc \
	runtime/base/mutex_test.cc \
	runtime/base/timing_logger_test.cc \
	runtime/base/unix_file/fd_file_test.cc \
	runtime/base/unix_file/mapped_file_test.cc \
	runtime/base/unix_file/null_file_test.cc \
	runtime/base/unix_file/random_access_file_utils_test.cc \
	runtime/base/unix_file/string_file_test.cc \
	runtime/class_linker_test.cc \
	runtime/dex_file_test.cc \
	runtime/dex_instruction_visitor_test.cc \
	runtime/dex_method_iterator_test.cc \
	runtime/entrypoints/math_entrypoints_test.cc \
	runtime/exception_test.cc \
	runtime/gc/accounting/space_bitmap_test.cc \
	runtime/gc/heap_test.cc \
	runtime/gc/space/dlmalloc_space_base_test.cc \
	runtime/gc/space/dlmalloc_space_static_test.cc \
	runtime/gc/space/dlmalloc_space_random_test.cc \
	runtime/gc/space/rosalloc_space_base_test.cc \
	runtime/gc/space/rosalloc_space_static_test.cc \
	runtime/gc/space/rosalloc_space_random_test.cc \
	runtime/gc/space/large_object_space_test.cc \
	runtime/gtest_test.cc \
	runtime/handle_scope_test.cc \
	runtime/indenter_test.cc \
	runtime/indirect_reference_table_test.cc \
	runtime/instruction_set_test.cc \
	runtime/intern_table_test.cc \
	runtime/leb128_test.cc \
	runtime/mem_map_test.cc \
	runtime/mirror/dex_cache_test.cc \
	runtime/mirror/object_test.cc \
	runtime/parsed_options_test.cc \
	runtime/reference_table_test.cc \
	runtime/thread_pool_test.cc \
	runtime/transaction_test.cc \
	runtime/utils_test.cc \
	runtime/verifier/method_verifier_test.cc \
	runtime/verifier/reg_type_test.cc \
	runtime/zip_archive_test.cc

COMPILER_GTEST_COMMON_SRC_FILES := \
	runtime/jni_internal_test.cc \
	runtime/proxy_test.cc \
	runtime/reflection_test.cc \
	compiler/dex/local_value_numbering_test.cc \
	compiler/dex/mir_optimization_test.cc \
	compiler/driver/compiler_driver_test.cc \
	compiler/elf_writer_test.cc \
	compiler/image_test.cc \
	compiler/jni/jni_compiler_test.cc \
	compiler/oat_test.cc \
	compiler/optimizing/codegen_test.cc \
	compiler/optimizing/dominator_test.cc \
	compiler/optimizing/find_loops_test.cc \
	compiler/optimizing/linearize_test.cc \
	compiler/optimizing/liveness_test.cc \
	compiler/optimizing/pretty_printer_test.cc \
	compiler/optimizing/ssa_test.cc \
	compiler/output_stream_test.cc \
	compiler/utils/arena_allocator_test.cc \
	compiler/utils/dedupe_set_test.cc \
	compiler/utils/arm/managed_register_arm_test.cc \
	compiler/utils/arm64/managed_register_arm64_test.cc \
	compiler/utils/x86/managed_register_x86_test.cc \

ifeq ($(ART_SEA_IR_MODE),true)
COMPILER_GTEST_COMMON_SRC_FILES += \
	compiler/utils/scoped_hashtable_test.cc \
	compiler/sea_ir/types/type_data_test.cc \
	compiler/sea_ir/types/type_inference_visitor_test.cc \
	compiler/sea_ir/ir/regions_test.cc
endif

RUNTIME_GTEST_TARGET_SRC_FILES := \
	$(RUNTIME_GTEST_COMMON_SRC_FILES)

RUNTIME_GTEST_HOST_SRC_FILES := \
	$(RUNTIME_GTEST_COMMON_SRC_FILES)

COMPILER_GTEST_TARGET_SRC_FILES := \
	$(COMPILER_GTEST_COMMON_SRC_FILES)

COMPILER_GTEST_HOST_SRC_FILES := \
	$(COMPILER_GTEST_COMMON_SRC_FILES) \
	compiler/utils/x86/assembler_x86_test.cc \
	compiler/utils/x86_64/assembler_x86_64_test.cc

ART_HOST_GTEST_EXECUTABLES :=
ART_TARGET_GTEST_EXECUTABLES$(ART_PHONY_TEST_TARGET_SUFFIX) :=
ART_TARGET_GTEST_EXECUTABLES$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) :=
ART_HOST_GTEST_TARGETS :=
ART_HOST_VALGRIND_GTEST_TARGETS :=
ART_TARGET_GTEST_TARGETS$(ART_PHONY_TEST_TARGET_SUFFIX) :=
ART_TARGET_GTEST_TARGETS$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) :=

ART_TEST_CFLAGS :=
ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  ART_TEST_CFLAGS += -DART_USE_PORTABLE_COMPILER=1
endif

# Build a make target for a target test.
# (1) Prefix for variables
define build-art-test-make-target
.PHONY: $$(art_gtest_target)$($(1)ART_PHONY_TEST_TARGET_SUFFIX)
$$(art_gtest_target)$($(1)ART_PHONY_TEST_TARGET_SUFFIX): $($(1)ART_NATIVETEST_OUT)/$$(LOCAL_MODULE) test-art-target-sync
	adb shell touch $($(1)ART_TEST_DIR)/$$@
	adb shell rm $($(1)ART_TEST_DIR)/$$@
	adb shell chmod 755 $($(1)ART_NATIVETEST_DIR)/$$(notdir $$<)
	adb shell sh -c "$($(1)ART_NATIVETEST_DIR)/$$(notdir $$<) && touch $($(1)ART_TEST_DIR)/$$@"
	$(hide) (adb pull $($(1)ART_TEST_DIR)/$$@ /tmp/ && echo $$@ PASSED) || (echo $$@ FAILED && exit 1)
	$(hide) rm /tmp/$$@

  ART_TARGET_GTEST_TARGETS$($(1)ART_PHONY_TEST_TARGET_SUFFIX) += $$(art_gtest_target)$($(1)ART_PHONY_TEST_TARGET_SUFFIX)
endef


# $(1): target or host
# $(2): file name
# $(3): extra C includes
# $(4): extra shared libraries
define build-art-test
  ifneq ($(1),target)
    ifneq ($(1),host)
      $$(error expected target or host for argument 1, received $(1))
    endif
  endif

  art_target_or_host := $(1)
  art_gtest_filename := $(2)
  art_gtest_extra_c_includes := $(3)
  art_gtest_extra_shared_libraries := $(4)

  art_gtest_name := $$(notdir $$(basename $$(art_gtest_filename)))

  include $(CLEAR_VARS)
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  LOCAL_MODULE := $$(art_gtest_name)
  ifeq ($$(art_target_or_host),target)
    LOCAL_MODULE_TAGS := tests
  endif
  LOCAL_SRC_FILES := $$(art_gtest_filename) runtime/common_runtime_test.cc
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES) art/runtime $(3)
  LOCAL_SHARED_LIBRARIES += libartd $(4)
  # dex2oatd is needed to go with libartd
  LOCAL_REQUIRED_MODULES := dex2oatd

  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.gtest.mk

  # Mac OS linker doesn't understand --export-dynamic.
  ifneq ($(HOST_OS)-$$(art_target_or_host),darwin-host)
    # Allow jni_compiler_test to find Java_MyClassNatives_bar within itself using dlopen(NULL, ...).
    LOCAL_LDFLAGS := -Wl,--export-dynamic -Wl,-u,Java_MyClassNatives_bar -Wl,-u,Java_MyClassNatives_sbar
  endif

  LOCAL_CFLAGS := $(ART_TEST_CFLAGS)
  ifeq ($$(art_target_or_host),target)
    LOCAL_CLANG := $(ART_TARGET_CLANG)
    LOCAL_CFLAGS += $(ART_TARGET_CFLAGS) $(ART_TARGET_DEBUG_CFLAGS)
    LOCAL_CFLAGS_x86 := $(ART_TARGET_CFLAGS_x86)
    LOCAL_SHARED_LIBRARIES += libdl libicuuc libicui18n libnativehelper libz libcutils libvixl
    LOCAL_STATIC_LIBRARIES += libgtest
    LOCAL_MODULE_PATH_32 := $(ART_BASE_NATIVETEST_OUT)
    LOCAL_MODULE_PATH_64 := $(ART_BASE_NATIVETEST_OUT)64
    LOCAL_MULTILIB := both
    include art/build/Android.libcxx.mk
    include $(BUILD_EXECUTABLE)
    
    ART_TARGET_GTEST_EXECUTABLES$(ART_PHONY_TEST_TARGET_SUFFIX) += $(ART_NATIVETEST_OUT)/$$(LOCAL_MODULE)
    art_gtest_target := test-art-$$(art_target_or_host)-gtest-$$(art_gtest_name)

    ifdef TARGET_2ND_ARCH
      $(call build-art-test-make-target,2ND_)

      ART_TARGET_GTEST_EXECUTABLES$(2ND_ART_PHONY_TEST_TARGET_SUFFIX) += $(2ND_ART_NATIVETEST_OUT)/$$(LOCAL_MODULE)

      # Bind the primary to the non-suffix rule
      ifneq ($(ART_PHONY_TEST_TARGET_SUFFIX),)
$$(art_gtest_target): $$(art_gtest_target)$(ART_PHONY_TEST_TARGET_SUFFIX)
      endif
    endif
    $(call build-art-test-make-target,)

  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS) $(ART_HOST_DEBUG_CFLAGS)
    LOCAL_SHARED_LIBRARIES += libicuuc-host libicui18n-host libnativehelper libz-host
    LOCAL_STATIC_LIBRARIES += libcutils libvixl
    ifneq ($(WITHOUT_HOST_CLANG),true)
        # GCC host compiled tests fail with this linked, presumably due to destructors that run.
        LOCAL_STATIC_LIBRARIES += libgtest_host
    endif
    LOCAL_LDLIBS += -lpthread -ldl
    LOCAL_IS_HOST_MODULE := true
    include art/build/Android.libcxx.mk
    include $(BUILD_HOST_EXECUTABLE)
    art_gtest_exe := $(HOST_OUT_EXECUTABLES)/$$(LOCAL_MODULE)
    ART_HOST_GTEST_EXECUTABLES += $$(art_gtest_exe)
    art_gtest_target := test-art-$$(art_target_or_host)-gtest-$$(art_gtest_name)
.PHONY: $$(art_gtest_target)
$$(art_gtest_target): $$(art_gtest_exe) test-art-host-dependencies
	$$<
	@echo $$@ PASSED

    ART_HOST_GTEST_TARGETS += $$(art_gtest_target)

.PHONY: valgrind-$$(art_gtest_target)
valgrind-$$(art_gtest_target): $$(art_gtest_exe) test-art-host-dependencies
	valgrind --leak-check=full --error-exitcode=1 $$<
	@echo $$@ PASSED

    ART_HOST_VALGRIND_GTEST_TARGETS += valgrind-$$(art_gtest_target)
  endif
endef

ifeq ($(ART_BUILD_TARGET),true)
  $(foreach file,$(RUNTIME_GTEST_TARGET_SRC_FILES), $(eval $(call build-art-test,target,$(file),,)))
  $(foreach file,$(COMPILER_GTEST_TARGET_SRC_FILES), $(eval $(call build-art-test,target,$(file),art/compiler,libartd-compiler)))
endif
ifeq ($(WITH_HOST_DALVIK),true)
  ifeq ($(ART_BUILD_HOST),true)
    $(foreach file,$(RUNTIME_GTEST_HOST_SRC_FILES), $(eval $(call build-art-test,host,$(file),,)))
    $(foreach file,$(COMPILER_GTEST_HOST_SRC_FILES), $(eval $(call build-art-test,host,$(file),art/compiler,libartd-compiler)))
  endif
endif

# Used outside the art project to get a list of the current tests
RUNTIME_TARGET_GTEST_MAKE_TARGETS :=
$(foreach file, $(RUNTIME_GTEST_TARGET_SRC_FILES), $(eval RUNTIME_TARGET_GTEST_MAKE_TARGETS += $$(notdir $$(basename $$(file)))))
COMPILER_TARGET_GTEST_MAKE_TARGETS :=
$(foreach file, $(COMPILER_GTEST_TARGET_SRC_FILES), $(eval COMPILER_TARGET_GTEST_MAKE_TARGETS += $$(notdir $$(basename $$(file)))))
