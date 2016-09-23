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

# The path for which all the dex files are relative, not actually the current directory.
LOCAL_PATH := art/test

include art/build/Android.common_test.mk
include art/build/Android.common_path.mk
include art/build/Android.common_build.mk

# Subdirectories in art/test which contain dex files used as inputs for gtests.
GTEST_DEX_DIRECTORIES := \
  AbstractMethod \
  AllFields \
  ExceptionHandle \
  GetMethodSignature \
  ImageLayoutA \
  ImageLayoutB \
  Instrumentation \
  Interfaces \
  Lookup \
  Main \
  MultiDex \
  MultiDexModifiedSecondary \
  MyClass \
  MyClassNatives \
  Nested \
  NonStaticLeafMethods \
  Packages \
  ProtoCompare \
  ProtoCompare2 \
  ProfileTestMultiDex \
  StaticLeafMethods \
  Statics \
  StaticsFromCode \
  Transaction \
  XandY

# Create build rules for each dex file recording the dependency.
$(foreach dir,$(GTEST_DEX_DIRECTORIES), $(eval $(call build-art-test-dex,art-gtest,$(dir), \
  $(ART_TARGET_NATIVETEST_OUT),art/build/Android.gtest.mk,ART_TEST_TARGET_GTEST_$(dir)_DEX, \
  ART_TEST_HOST_GTEST_$(dir)_DEX)))

# Create rules for MainStripped, a copy of Main with the classes.dex stripped
# for the oat file assistant tests.
ART_TEST_HOST_GTEST_MainStripped_DEX := $(basename $(ART_TEST_HOST_GTEST_Main_DEX))Stripped$(suffix $(ART_TEST_HOST_GTEST_Main_DEX))
ART_TEST_TARGET_GTEST_MainStripped_DEX := $(basename $(ART_TEST_TARGET_GTEST_Main_DEX))Stripped$(suffix $(ART_TEST_TARGET_GTEST_Main_DEX))

$(ART_TEST_HOST_GTEST_MainStripped_DEX): $(ART_TEST_HOST_GTEST_Main_DEX)
	cp $< $@
	$(call dexpreopt-remove-classes.dex,$@)

$(ART_TEST_TARGET_GTEST_MainStripped_DEX): $(ART_TEST_TARGET_GTEST_Main_DEX)
	cp $< $@
	$(call dexpreopt-remove-classes.dex,$@)

# Dex file dependencies for each gtest.
ART_GTEST_dex2oat_environment_tests_DEX_DEPS := Main MainStripped MultiDex MultiDexModifiedSecondary Nested

ART_GTEST_class_linker_test_DEX_DEPS := Interfaces MultiDex MyClass Nested Statics StaticsFromCode
ART_GTEST_compiler_driver_test_DEX_DEPS := AbstractMethod StaticLeafMethods ProfileTestMultiDex
ART_GTEST_dex_cache_test_DEX_DEPS := Main Packages
ART_GTEST_dex_file_test_DEX_DEPS := GetMethodSignature Main Nested
ART_GTEST_dex2oat_test_DEX_DEPS := $(ART_GTEST_dex2oat_environment_tests_DEX_DEPS)
ART_GTEST_exception_test_DEX_DEPS := ExceptionHandle
ART_GTEST_image_test_DEX_DEPS := ImageLayoutA ImageLayoutB
ART_GTEST_instrumentation_test_DEX_DEPS := Instrumentation
ART_GTEST_jni_compiler_test_DEX_DEPS := MyClassNatives
ART_GTEST_jni_internal_test_DEX_DEPS := AllFields StaticLeafMethods
ART_GTEST_oat_file_assistant_test_DEX_DEPS := $(ART_GTEST_dex2oat_environment_tests_DEX_DEPS)
ART_GTEST_oat_file_test_DEX_DEPS := Main MultiDex
ART_GTEST_oat_test_DEX_DEPS := Main
ART_GTEST_object_test_DEX_DEPS := ProtoCompare ProtoCompare2 StaticsFromCode XandY
ART_GTEST_proxy_test_DEX_DEPS := Interfaces
ART_GTEST_reflection_test_DEX_DEPS := Main NonStaticLeafMethods StaticLeafMethods
ART_GTEST_profile_assistant_test_DEX_DEPS := ProfileTestMultiDex
ART_GTEST_profile_compilation_info_test_DEX_DEPS := ProfileTestMultiDex
ART_GTEST_stub_test_DEX_DEPS := AllFields
ART_GTEST_transaction_test_DEX_DEPS := Transaction
ART_GTEST_type_lookup_table_test_DEX_DEPS := Lookup

# The elf writer test has dependencies on core.oat.
ART_GTEST_elf_writer_test_HOST_DEPS := $(HOST_CORE_IMAGE_default_no-pic_64) $(HOST_CORE_IMAGE_default_no-pic_32)
ART_GTEST_elf_writer_test_TARGET_DEPS := $(TARGET_CORE_IMAGE_default_no-pic_64) $(TARGET_CORE_IMAGE_default_no-pic_32)

ART_GTEST_dex2oat_environment_tests_HOST_DEPS := \
  $(HOST_CORE_IMAGE_default_no-pic_64) \
  $(HOST_CORE_IMAGE_default_no-pic_32)
ART_GTEST_dex2oat_environment_tests_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_default_no-pic_64) \
  $(TARGET_CORE_IMAGE_default_no-pic_32)

ART_GTEST_oat_file_assistant_test_HOST_DEPS := \
   $(ART_GTEST_dex2oat_environment_tests_HOST_DEPS) \
   $(HOST_OUT_EXECUTABLES)/patchoatd
ART_GTEST_oat_file_assistant_test_TARGET_DEPS := \
   $(ART_GTEST_dex2oat_environment_tests_TARGET_DEPS) \
   $(TARGET_OUT_EXECUTABLES)/patchoatd


ART_GTEST_dex2oat_test_HOST_DEPS := \
  $(ART_GTEST_dex2oat_environment_tests_HOST_DEPS)
ART_GTEST_dex2oat_test_TARGET_DEPS := \
  $(ART_GTEST_dex2oat_environment_tests_TARGET_DEPS)

# TODO: document why this is needed.
ART_GTEST_proxy_test_HOST_DEPS := $(HOST_CORE_IMAGE_default_no-pic_64) $(HOST_CORE_IMAGE_default_no-pic_32)

# The dexdump test requires an image and the dexdump utility.
# TODO: rename into dexdump when migration completes
ART_GTEST_dexdump_test_HOST_DEPS := \
  $(HOST_CORE_IMAGE_default_no-pic_64) \
  $(HOST_CORE_IMAGE_default_no-pic_32) \
  $(HOST_OUT_EXECUTABLES)/dexdump2
ART_GTEST_dexdump_test_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_default_no-pic_64) \
  $(TARGET_CORE_IMAGE_default_no-pic_32) \
  dexdump2

# The dexlist test requires an image and the dexlist utility.
ART_GTEST_dexlist_test_HOST_DEPS := \
  $(HOST_CORE_IMAGE_default_no-pic_64) \
  $(HOST_CORE_IMAGE_default_no-pic_32) \
  $(HOST_OUT_EXECUTABLES)/dexlist
ART_GTEST_dexlist_test_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_default_no-pic_64) \
  $(TARGET_CORE_IMAGE_default_no-pic_32) \
  dexlist

# The imgdiag test has dependencies on core.oat since it needs to load it during the test.
# For the host, also add the installed tool (in the base size, that should suffice). For the
# target, just the module is fine, the sync will happen late enough.
ART_GTEST_imgdiag_test_HOST_DEPS := \
  $(HOST_CORE_IMAGE_default_no-pic_64) \
  $(HOST_CORE_IMAGE_default_no-pic_32) \
  $(HOST_OUT_EXECUTABLES)/imgdiagd
ART_GTEST_imgdiag_test_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_default_no-pic_64) \
  $(TARGET_CORE_IMAGE_default_no-pic_32) \
  imgdiagd

# Oatdump test requires an image and oatfile to dump.
ART_GTEST_oatdump_test_HOST_DEPS := \
  $(HOST_CORE_IMAGE_default_no-pic_64) \
  $(HOST_CORE_IMAGE_default_no-pic_32) \
  $(HOST_OUT_EXECUTABLES)/oatdumpd
ART_GTEST_oatdump_test_TARGET_DEPS := \
  $(TARGET_CORE_IMAGE_default_no-pic_64) \
  $(TARGET_CORE_IMAGE_default_no-pic_32) \
  oatdump

# Profile assistant tests requires profman utility.
ART_GTEST_profile_assistant_test_HOST_DEPS := \
  $(HOST_OUT_EXECUTABLES)/profmand
ART_GTEST_profile_assistant_test_TARGET_DEPS := \
  profman

# The path for which all the source files are relative, not actually the current directory.
LOCAL_PATH := art

RUNTIME_GTEST_COMMON_SRC_FILES := \
  cmdline/cmdline_parser_test.cc \
  dexdump/dexdump_test.cc \
  dexlist/dexlist_test.cc \
  dex2oat/dex2oat_test.cc \
  imgdiag/imgdiag_test.cc \
  oatdump/oatdump_test.cc \
  profman/profile_assistant_test.cc \
  runtime/arch/arch_test.cc \
  runtime/arch/instruction_set_test.cc \
  runtime/arch/instruction_set_features_test.cc \
  runtime/arch/memcmp16_test.cc \
  runtime/arch/stub_test.cc \
  runtime/arch/arm/instruction_set_features_arm_test.cc \
  runtime/arch/arm64/instruction_set_features_arm64_test.cc \
  runtime/arch/mips/instruction_set_features_mips_test.cc \
  runtime/arch/mips64/instruction_set_features_mips64_test.cc \
  runtime/arch/x86/instruction_set_features_x86_test.cc \
  runtime/arch/x86_64/instruction_set_features_x86_64_test.cc \
  runtime/barrier_test.cc \
  runtime/base/arena_allocator_test.cc \
  runtime/base/bit_field_test.cc \
  runtime/base/bit_utils_test.cc \
  runtime/base/bit_vector_test.cc \
  runtime/base/hash_set_test.cc \
  runtime/base/hex_dump_test.cc \
  runtime/base/histogram_test.cc \
  runtime/base/mutex_test.cc \
  runtime/base/scoped_flock_test.cc \
  runtime/base/stringprintf_test.cc \
  runtime/base/time_utils_test.cc \
  runtime/base/timing_logger_test.cc \
  runtime/base/variant_map_test.cc \
  runtime/base/unix_file/fd_file_test.cc \
  runtime/class_linker_test.cc \
  runtime/compiler_filter_test.cc \
  runtime/dex_file_test.cc \
  runtime/dex_file_verifier_test.cc \
  runtime/dex_instruction_test.cc \
  runtime/dex_instruction_visitor_test.cc \
  runtime/dex_method_iterator_test.cc \
  runtime/entrypoints/math_entrypoints_test.cc \
  runtime/entrypoints/quick/quick_trampoline_entrypoints_test.cc \
  runtime/entrypoints_order_test.cc \
  runtime/gc/accounting/card_table_test.cc \
  runtime/gc/accounting/mod_union_table_test.cc \
  runtime/gc/accounting/space_bitmap_test.cc \
  runtime/gc/collector/immune_spaces_test.cc \
  runtime/gc/heap_test.cc \
  runtime/gc/reference_queue_test.cc \
  runtime/gc/space/dlmalloc_space_static_test.cc \
  runtime/gc/space/dlmalloc_space_random_test.cc \
  runtime/gc/space/large_object_space_test.cc \
  runtime/gc/space/rosalloc_space_static_test.cc \
  runtime/gc/space/rosalloc_space_random_test.cc \
  runtime/gc/space/space_create_test.cc \
  runtime/gc/task_processor_test.cc \
  runtime/gtest_test.cc \
  runtime/handle_scope_test.cc \
  runtime/indenter_test.cc \
  runtime/indirect_reference_table_test.cc \
  runtime/instrumentation_test.cc \
  runtime/intern_table_test.cc \
  runtime/interpreter/safe_math_test.cc \
  runtime/interpreter/unstarted_runtime_test.cc \
  runtime/java_vm_ext_test.cc \
  runtime/jit/profile_compilation_info_test.cc \
  runtime/lambda/closure_test.cc \
  runtime/lambda/shorty_field_type_test.cc \
  runtime/leb128_test.cc \
  runtime/mem_map_test.cc \
  runtime/memory_region_test.cc \
  runtime/mirror/dex_cache_test.cc \
  runtime/mirror/object_test.cc \
  runtime/monitor_pool_test.cc \
  runtime/monitor_test.cc \
  runtime/oat_file_test.cc \
  runtime/oat_file_assistant_test.cc \
  runtime/parsed_options_test.cc \
  runtime/prebuilt_tools_test.cc \
  runtime/reference_table_test.cc \
  runtime/thread_pool_test.cc \
  runtime/transaction_test.cc \
  runtime/type_lookup_table_test.cc \
  runtime/utf_test.cc \
  runtime/utils_test.cc \
  runtime/verifier/method_verifier_test.cc \
  runtime/verifier/reg_type_test.cc \
  runtime/zip_archive_test.cc

COMPILER_GTEST_COMMON_SRC_FILES := \
  runtime/jni_internal_test.cc \
  runtime/proxy_test.cc \
  runtime/reflection_test.cc \
  compiler/compiled_method_test.cc \
  compiler/debug/dwarf/dwarf_test.cc \
  compiler/driver/compiled_method_storage_test.cc \
  compiler/driver/compiler_driver_test.cc \
  compiler/elf_writer_test.cc \
  compiler/exception_test.cc \
  compiler/image_test.cc \
  compiler/jni/jni_compiler_test.cc \
  compiler/linker/multi_oat_relative_patcher_test.cc \
  compiler/linker/output_stream_test.cc \
  compiler/oat_test.cc \
  compiler/optimizing/bounds_check_elimination_test.cc \
  compiler/optimizing/dominator_test.cc \
  compiler/optimizing/find_loops_test.cc \
  compiler/optimizing/graph_checker_test.cc \
  compiler/optimizing/graph_test.cc \
  compiler/optimizing/gvn_test.cc \
  compiler/optimizing/induction_var_analysis_test.cc \
  compiler/optimizing/induction_var_range_test.cc \
  compiler/optimizing/licm_test.cc \
  compiler/optimizing/live_interval_test.cc \
  compiler/optimizing/nodes_test.cc \
  compiler/optimizing/parallel_move_test.cc \
  compiler/optimizing/pretty_printer_test.cc \
  compiler/optimizing/reference_type_propagation_test.cc \
  compiler/optimizing/side_effects_test.cc \
  compiler/optimizing/ssa_test.cc \
  compiler/optimizing/stack_map_test.cc \
  compiler/optimizing/suspend_check_test.cc \
  compiler/utils/dedupe_set_test.cc \
  compiler/utils/intrusive_forward_list_test.cc \
  compiler/utils/swap_space_test.cc \
  compiler/utils/test_dex_file_builder_test.cc \

COMPILER_GTEST_COMMON_SRC_FILES_all := \
  compiler/jni/jni_cfi_test.cc \
  compiler/optimizing/codegen_test.cc \
  compiler/optimizing/constant_folding_test.cc \
  compiler/optimizing/dead_code_elimination_test.cc \
  compiler/optimizing/linearize_test.cc \
  compiler/optimizing/liveness_test.cc \
  compiler/optimizing/live_ranges_test.cc \
  compiler/optimizing/optimizing_cfi_test.cc \
  compiler/optimizing/register_allocator_test.cc \

COMPILER_GTEST_COMMON_SRC_FILES_arm := \
  compiler/linker/arm/relative_patcher_thumb2_test.cc \
  compiler/utils/arm/managed_register_arm_test.cc \

COMPILER_GTEST_COMMON_SRC_FILES_arm64 := \
  compiler/linker/arm64/relative_patcher_arm64_test.cc \
  compiler/utils/arm64/managed_register_arm64_test.cc \

COMPILER_GTEST_COMMON_SRC_FILES_mips := \

COMPILER_GTEST_COMMON_SRC_FILES_mips64 := \

COMPILER_GTEST_COMMON_SRC_FILES_x86 := \
  compiler/linker/x86/relative_patcher_x86_test.cc \
  compiler/utils/x86/managed_register_x86_test.cc \

COMPILER_GTEST_COMMON_SRC_FILES_x86_64 := \
  compiler/linker/x86_64/relative_patcher_x86_64_test.cc \

RUNTIME_GTEST_TARGET_SRC_FILES := \
  $(RUNTIME_GTEST_COMMON_SRC_FILES)

RUNTIME_GTEST_HOST_SRC_FILES := \
  $(RUNTIME_GTEST_COMMON_SRC_FILES)

COMPILER_GTEST_TARGET_SRC_FILES := \
  $(COMPILER_GTEST_COMMON_SRC_FILES)

COMPILER_GTEST_TARGET_SRC_FILES_all := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_all) \

COMPILER_GTEST_TARGET_SRC_FILES_arm := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_arm) \

COMPILER_GTEST_TARGET_SRC_FILES_arm64 := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_arm64) \

COMPILER_GTEST_TARGET_SRC_FILES_mips := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_mips) \

COMPILER_GTEST_TARGET_SRC_FILES_mips64 := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_mips64) \

COMPILER_GTEST_TARGET_SRC_FILES_x86 := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_x86) \

COMPILER_GTEST_TARGET_SRC_FILES_x86_64 := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_x86_64) \

$(foreach arch,$(ART_TARGET_CODEGEN_ARCHS),$(eval COMPILER_GTEST_TARGET_SRC_FILES += $$(COMPILER_GTEST_TARGET_SRC_FILES_$(arch))))
ifeq (true,$(ART_TARGET_COMPILER_TESTS))
  COMPILER_GTEST_TARGET_SRC_FILES += $(COMPILER_GTEST_TARGET_SRC_FILES_all)
endif

COMPILER_GTEST_HOST_SRC_FILES := \
  $(COMPILER_GTEST_COMMON_SRC_FILES) \

COMPILER_GTEST_HOST_SRC_FILES_all := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_all) \

COMPILER_GTEST_HOST_SRC_FILES_arm := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_arm) \
  compiler/utils/arm/assembler_arm32_test.cc \
  compiler/utils/arm/assembler_thumb2_test.cc \
  compiler/utils/assembler_thumb_test.cc \

COMPILER_GTEST_HOST_SRC_FILES_arm64 := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_arm64) \

COMPILER_GTEST_HOST_SRC_FILES_mips := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_mips) \
  compiler/utils/mips/assembler_mips_test.cc \

COMPILER_GTEST_HOST_SRC_FILES_mips64 := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_mips64) \
  compiler/utils/mips64/assembler_mips64_test.cc \

COMPILER_GTEST_HOST_SRC_FILES_x86 := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_x86) \
  compiler/utils/x86/assembler_x86_test.cc \

COMPILER_GTEST_HOST_SRC_FILES_x86_64 := \
  $(COMPILER_GTEST_COMMON_SRC_FILES_x86_64) \
  compiler/utils/x86_64/assembler_x86_64_test.cc

$(foreach arch,$(ART_HOST_CODEGEN_ARCHS),$(eval COMPILER_GTEST_HOST_SRC_FILES += $$(COMPILER_GTEST_HOST_SRC_FILES_$(arch))))
ifeq (true,$(ART_HOST_COMPILER_TESTS))
  COMPILER_GTEST_HOST_SRC_FILES += $(COMPILER_GTEST_HOST_SRC_FILES_all)
endif

ART_TEST_CFLAGS :=

include $(CLEAR_VARS)
LOCAL_MODULE := libart-gtest
LOCAL_MODULE_TAGS := optional
LOCAL_CPP_EXTENSION := cc
LOCAL_SRC_FILES := runtime/common_runtime_test.cc compiler/common_compiler_test.cc
LOCAL_C_INCLUDES := $(ART_C_INCLUDES) art/runtime art/cmdline art/compiler
LOCAL_SHARED_LIBRARIES := libartd libartd-compiler libdl
LOCAL_STATIC_LIBRARIES += libgtest
LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.gtest.mk
$(eval $(call set-target-local-clang-vars))
$(eval $(call set-target-local-cflags-vars,debug))
LOCAL_CLANG_CFLAGS += -Wno-used-but-marked-unused -Wno-deprecated -Wno-missing-noreturn # gtest issue
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libart-gtest
LOCAL_MODULE_TAGS := optional
LOCAL_CPP_EXTENSION := cc
LOCAL_CFLAGS := $(ART_HOST_CFLAGS)
LOCAL_ASFLAGS := $(ART_HOST_ASFLAGS)
LOCAL_SRC_FILES := runtime/common_runtime_test.cc compiler/common_compiler_test.cc
LOCAL_C_INCLUDES := $(ART_C_INCLUDES) art/runtime art/cmdline art/compiler
LOCAL_SHARED_LIBRARIES := libartd libartd-compiler
LOCAL_STATIC_LIBRARIES := libgtest_host
LOCAL_LDLIBS += -ldl -lpthread
LOCAL_MULTILIB := both
LOCAL_CLANG := $(ART_HOST_CLANG)
LOCAL_CLANG_CFLAGS += -Wno-used-but-marked-unused -Wno-deprecated -Wno-missing-noreturn  # gtest issue
LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.gtest.mk
include $(BUILD_HOST_SHARED_LIBRARY)

# Variables holding collections of gtest pre-requisits used to run a number of gtests.
ART_TEST_HOST_GTEST$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_GTEST$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_GTEST_RULES :=
ART_TEST_HOST_VALGRIND_GTEST$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_VALGRIND_GTEST$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_VALGRIND_GTEST_RULES :=
ART_TEST_TARGET_GTEST$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_GTEST$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_GTEST_RULES :=
ART_TEST_HOST_GTEST_DEPENDENCIES :=

ART_GTEST_TARGET_ANDROID_ROOT := '/system'
ifneq ($(ART_TEST_ANDROID_ROOT),)
  ART_GTEST_TARGET_ANDROID_ROOT := $(ART_TEST_ANDROID_ROOT)
endif

# Define a make rule for a target device gtest.
# $(1): gtest name - the name of the test we're building such as leb128_test.
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
# $(3): LD_LIBRARY_PATH or undefined - used in case libartd.so is not in /system/lib/
define define-art-gtest-rule-target
  gtest_rule := test-art-target-gtest-$(1)$$($(2)ART_PHONY_TEST_TARGET_SUFFIX)

  # Add the test dependencies to test-art-target-sync, which will be a prerequisite for the test
  # to ensure files are pushed to the device.
  TEST_ART_TARGET_SYNC_DEPS += \
    $$(ART_GTEST_$(1)_TARGET_DEPS) \
    $(foreach file,$(ART_GTEST_$(1)_DEX_DEPS),$(ART_TEST_TARGET_GTEST_$(file)_DEX)) \
    $$(ART_TARGET_NATIVETEST_OUT)/$$(TARGET_$(2)ARCH)/$(1) \
    $$($(2)TARGET_OUT_SHARED_LIBRARIES)/libjavacore.so \
    $$($(2)TARGET_OUT_SHARED_LIBRARIES)/libopenjdkd.so \
    $$(TARGET_OUT_JAVA_LIBRARIES)/core-libart-testdex.jar \
    $$(TARGET_OUT_JAVA_LIBRARIES)/core-oj-testdex.jar

.PHONY: $$(gtest_rule)
$$(gtest_rule): test-art-target-sync
	$(hide) adb shell touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$$$$PPID
	$(hide) adb shell rm $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$$$$PPID
	$(hide) adb shell chmod 755 $(ART_TARGET_NATIVETEST_DIR)/$(TARGET_$(2)ARCH)/$(1)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  (adb shell "$(GCOV_ENV) LD_LIBRARY_PATH=$(3) ANDROID_ROOT=$(ART_GTEST_TARGET_ANDROID_ROOT) \
	    $(ART_TARGET_NATIVETEST_DIR)/$(TARGET_$(2)ARCH)/$(1) && touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$$$$PPID" \
	  && (adb pull $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$$$$PPID /tmp/ \
	      && $$(call ART_TEST_PASSED,$$@)) \
	  || $$(call ART_TEST_FAILED,$$@))
	$(hide) rm -f /tmp/$$@-$$$$PPID

  ART_TEST_TARGET_GTEST$($(2)ART_PHONY_TEST_TARGET_SUFFIX)_RULES += $$(gtest_rule)
  ART_TEST_TARGET_GTEST_RULES += $$(gtest_rule)
  ART_TEST_TARGET_GTEST_$(1)_RULES += $$(gtest_rule)

  # Clear locally defined variables.
  gtest_rule :=
endef  # define-art-gtest-rule-target

ART_VALGRIND_DEPENDENCIES := \
  $(HOST_OUT_EXECUTABLES)/valgrind \
  $(HOST_OUT)/lib64/valgrind/memcheck-amd64-linux \
  $(HOST_OUT)/lib64/valgrind/memcheck-x86-linux \
  $(HOST_OUT)/lib64/valgrind/default.supp \
  $(HOST_OUT)/lib64/valgrind/vgpreload_core-amd64-linux.so \
  $(HOST_OUT)/lib64/valgrind/vgpreload_core-x86-linux.so \
  $(HOST_OUT)/lib64/valgrind/vgpreload_memcheck-amd64-linux.so \
  $(HOST_OUT)/lib64/valgrind/vgpreload_memcheck-x86-linux.so

# Define make rules for a host gtests.
# $(1): gtest name - the name of the test we're building such as leb128_test.
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
define define-art-gtest-rule-host
  gtest_rule := test-art-host-gtest-$(1)$$($(2)ART_PHONY_TEST_HOST_SUFFIX)
  gtest_exe := $$(HOST_OUT_EXECUTABLES)/$(1)$$($(2)ART_PHONY_TEST_HOST_SUFFIX)
  # Dependencies for all host gtests.
  gtest_deps := $$(HOST_CORE_DEX_LOCATIONS) \
    $$($(2)ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$$(ART_HOST_SHLIB_EXTENSION) \
    $$($(2)ART_HOST_OUT_SHARED_LIBRARIES)/libopenjdkd$$(ART_HOST_SHLIB_EXTENSION) \
    $$(gtest_exe) \
    $$(ART_GTEST_$(1)_HOST_DEPS) \
    $(foreach file,$(ART_GTEST_$(1)_DEX_DEPS),$(ART_TEST_HOST_GTEST_$(file)_DEX))

  ART_TEST_HOST_GTEST_DEPENDENCIES += $$(gtest_deps)

.PHONY: $$(gtest_rule)
$$(gtest_rule): $$(gtest_exe) $$(gtest_deps)
	$(hide) ($$(call ART_TEST_SKIP,$$@) && $$< && $$(call ART_TEST_PASSED,$$@)) \
	  || $$(call ART_TEST_FAILED,$$@)

  ART_TEST_HOST_GTEST$$($(2)ART_PHONY_TEST_HOST_SUFFIX)_RULES += $$(gtest_rule)
  ART_TEST_HOST_GTEST_RULES += $$(gtest_rule)
  ART_TEST_HOST_GTEST_$(1)_RULES += $$(gtest_rule)


.PHONY: valgrind-$$(gtest_rule)
valgrind-$$(gtest_rule): $$(gtest_exe) $$(gtest_deps) $(ART_VALGRIND_DEPENDENCIES)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  VALGRIND_LIB=$(HOST_OUT)/lib64/valgrind \
	  $(HOST_OUT_EXECUTABLES)/valgrind --leak-check=full --error-exitcode=1 \
	    --suppressions=art/test/valgrind-suppressions.txt $$< && \
	    $$(call ART_TEST_PASSED,$$@) || $$(call ART_TEST_FAILED,$$@)

  ART_TEST_HOST_VALGRIND_GTEST$$($(2)ART_PHONY_TEST_HOST_SUFFIX)_RULES += valgrind-$$(gtest_rule)
  ART_TEST_HOST_VALGRIND_GTEST_RULES += valgrind-$$(gtest_rule)
  ART_TEST_HOST_VALGRIND_GTEST_$(1)_RULES += valgrind-$$(gtest_rule)

  # Clear locally defined variables.
  valgrind_gtest_rule :=
  gtest_rule :=
  gtest_exe :=
  gtest_deps :=
endef  # define-art-gtest-rule-host

# Define the rules to build and run host and target gtests.
# $(1): target or host
# $(2): file name
# $(3): extra C includes
# $(4): extra shared libraries
define define-art-gtest
  ifneq ($(1),target)
    ifneq ($(1),host)
      $$(error expected target or host for argument 1, received $(1))
    endif
  endif

  art_target_or_host := $(1)
  art_gtest_filename := $(2)
  art_gtest_extra_c_includes := $(3)
  art_gtest_extra_shared_libraries := $(4)

  include $$(CLEAR_VARS)
  art_gtest_name := $$(notdir $$(basename $$(art_gtest_filename)))
  LOCAL_MODULE := $$(art_gtest_name)
  ifeq ($$(art_target_or_host),target)
    LOCAL_MODULE_TAGS := tests
  endif
  LOCAL_CPP_EXTENSION := $$(ART_CPP_EXTENSION)
  LOCAL_SRC_FILES := $$(art_gtest_filename)
  LOCAL_C_INCLUDES += $$(ART_C_INCLUDES) art/runtime art/cmdline $$(art_gtest_extra_c_includes)
  LOCAL_SHARED_LIBRARIES += libartd $$(art_gtest_extra_shared_libraries) libart-gtest libartd-disassembler
  LOCAL_WHOLE_STATIC_LIBRARIES += libsigchain

  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.gtest.mk

  # Mac OS linker doesn't understand --export-dynamic.
  ifneq ($$(HOST_OS)-$$(art_target_or_host),darwin-host)
    # Allow jni_compiler_test to find Java_MyClassNatives_bar within itself using dlopen(NULL, ...).
    LOCAL_LDFLAGS := -Wl,--export-dynamic -Wl,-u,Java_MyClassNatives_bar -Wl,-u,Java_MyClassNatives_sbar
  endif

  LOCAL_CFLAGS := $$(ART_TEST_CFLAGS)
  ifeq ($$(art_target_or_host),target)
    $$(eval $$(call set-target-local-clang-vars))
    $$(eval $$(call set-target-local-cflags-vars,debug))
    LOCAL_SHARED_LIBRARIES += libdl libicuuc libicui18n libnativehelper libz libcutils libvixl
    LOCAL_MODULE_PATH_32 := $$(ART_TARGET_NATIVETEST_OUT)/$$(ART_TARGET_ARCH_32)
    LOCAL_MODULE_PATH_64 := $$(ART_TARGET_NATIVETEST_OUT)/$$(ART_TARGET_ARCH_64)
    LOCAL_MULTILIB := both
    LOCAL_CLANG_CFLAGS += -Wno-used-but-marked-unused -Wno-deprecated -Wno-missing-noreturn  # gtest issue
    include $$(BUILD_EXECUTABLE)
    library_path :=
    2nd_library_path :=
    ifneq ($$(ART_TEST_ANDROID_ROOT),)
      ifdef TARGET_2ND_ARCH
        2nd_library_path := $$(ART_TEST_ANDROID_ROOT)/lib
        library_path := $$(ART_TEST_ANDROID_ROOT)/lib64
      else
        ifneq ($(filter %64,$(TARGET_ARCH)),)
          library_path := $$(ART_TEST_ANDROID_ROOT)/lib64
        else
          library_path := $$(ART_TEST_ANDROID_ROOT)/lib
        endif
      endif
    endif

    ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES :=
    ifdef TARGET_2ND_ARCH
      $$(eval $$(call define-art-gtest-rule-target,$$(art_gtest_name),2ND_,$$(2nd_library_path)))
    endif
    $$(eval $$(call define-art-gtest-rule-target,$$(art_gtest_name),,$$(library_path)))

    # A rule to run the different architecture versions of the gtest.
.PHONY: test-art-target-gtest-$$(art_gtest_name)
test-art-target-gtest-$$(art_gtest_name): $$(ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES)
	$$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

    # Clear locally defined variables.
    ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES :=
  else # host
    LOCAL_CLANG := $$(ART_HOST_CLANG)
    LOCAL_CFLAGS += $$(ART_HOST_CFLAGS) $$(ART_HOST_DEBUG_CFLAGS)
    LOCAL_ASFLAGS += $$(ART_HOST_ASFLAGS)
    LOCAL_SHARED_LIBRARIES += libicuuc-host libicui18n-host libnativehelper libziparchive-host libz-host libvixl
    LOCAL_LDLIBS := $(ART_HOST_LDLIBS) -lpthread -ldl
    LOCAL_IS_HOST_MODULE := true
    LOCAL_MULTILIB := both
    LOCAL_MODULE_STEM_32 := $$(art_gtest_name)32
    LOCAL_MODULE_STEM_64 := $$(art_gtest_name)64
    LOCAL_CLANG_CFLAGS += -Wno-used-but-marked-unused -Wno-deprecated -Wno-missing-noreturn  # gtest issue
    include $$(BUILD_HOST_EXECUTABLE)

    ART_TEST_HOST_GTEST_$$(art_gtest_name)_RULES :=
    ART_TEST_HOST_VALGRIND_GTEST_$$(art_gtest_name)_RULES :=
    ifneq ($$(HOST_PREFER_32_BIT),true)
      $$(eval $$(call define-art-gtest-rule-host,$$(art_gtest_name),2ND_))
    endif
    $$(eval $$(call define-art-gtest-rule-host,$$(art_gtest_name),))

    # Rules to run the different architecture versions of the gtest.
.PHONY: test-art-host-gtest-$$(art_gtest_name)
test-art-host-gtest-$$(art_gtest_name): $$(ART_TEST_HOST_GTEST_$$(art_gtest_name)_RULES)
	$$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

.PHONY: valgrind-test-art-host-gtest-$$(art_gtest_name)
valgrind-test-art-host-gtest-$$(art_gtest_name): $$(ART_TEST_HOST_VALGRIND_GTEST_$$(art_gtest_name)_RULES)
	$$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

    # Clear locally defined variables.
    ART_TEST_HOST_GTEST_$$(art_gtest_name)_RULES :=
    ART_TEST_HOST_VALGRIND_GTEST_$$(art_gtest_name)_RULES :=
  endif  # host_or_target

  # Clear locally defined variables.
  art_target_or_host :=
  art_gtest_filename :=
  art_gtest_extra_c_includes :=
  art_gtest_extra_shared_libraries :=
  art_gtest_name :=
  library_path :=
  2nd_library_path :=
endef  # define-art-gtest


ifeq ($(ART_BUILD_TARGET),true)
  $(foreach file,$(RUNTIME_GTEST_TARGET_SRC_FILES), $(eval $(call define-art-gtest,target,$(file),,libbacktrace)))
  $(foreach file,$(COMPILER_GTEST_TARGET_SRC_FILES), $(eval $(call define-art-gtest,target,$(file),art/compiler,libartd-compiler libbacktrace libnativeloader)))
endif
ifeq ($(ART_BUILD_HOST),true)
  $(foreach file,$(RUNTIME_GTEST_HOST_SRC_FILES), $(eval $(call define-art-gtest,host,$(file),,libbacktrace)))
  $(foreach file,$(COMPILER_GTEST_HOST_SRC_FILES), $(eval $(call define-art-gtest,host,$(file),art/compiler,libartd-compiler libbacktrace libnativeloader)))
endif

# Used outside the art project to get a list of the current tests
RUNTIME_TARGET_GTEST_MAKE_TARGETS :=
$(foreach file, $(RUNTIME_GTEST_TARGET_SRC_FILES), $(eval RUNTIME_TARGET_GTEST_MAKE_TARGETS += $$(notdir $$(basename $$(file)))))
COMPILER_TARGET_GTEST_MAKE_TARGETS :=
$(foreach file, $(COMPILER_GTEST_TARGET_SRC_FILES), $(eval COMPILER_TARGET_GTEST_MAKE_TARGETS += $$(notdir $$(basename $$(file)))))

# Define all the combinations of host/target, valgrind and suffix such as:
# test-art-host-gtest or valgrind-test-art-host-gtest64
# $(1): host or target
# $(2): HOST or TARGET
# $(3): valgrind- or undefined
# $(4): undefined, 32 or 64
define define-test-art-gtest-combination
  ifeq ($(1),host)
    ifneq ($(2),HOST)
      $$(error argument mismatch $(1) and ($2))
    endif
  else
    ifneq ($(1),target)
      $$(error found $(1) expected host or target)
    endif
    ifneq ($(2),TARGET)
      $$(error argument mismatch $(1) and ($2))
    endif
  endif

  rule_name := $(3)test-art-$(1)-gtest$(4)
  ifeq ($(3),valgrind-)
    ifneq ($(1),host)
      $$(error valgrind tests only wired up for the host)
    endif
    dependencies := $$(ART_TEST_$(2)_VALGRIND_GTEST$(4)_RULES)
  else
    dependencies := $$(ART_TEST_$(2)_GTEST$(4)_RULES)
  endif

.PHONY: $$(rule_name)
$$(rule_name): $$(dependencies)
	$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

  # Clear locally defined variables.
  rule_name :=
  dependencies :=
endef  # define-test-art-gtest-combination

$(eval $(call define-test-art-gtest-combination,target,TARGET,,))
$(eval $(call define-test-art-gtest-combination,target,TARGET,,$(ART_PHONY_TEST_TARGET_SUFFIX)))
ifdef TARGET_2ND_ARCH
$(eval $(call define-test-art-gtest-combination,target,TARGET,,$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)))
endif
$(eval $(call define-test-art-gtest-combination,host,HOST,,))
$(eval $(call define-test-art-gtest-combination,host,HOST,valgrind-,))
$(eval $(call define-test-art-gtest-combination,host,HOST,,$(ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-gtest-combination,host,HOST,valgrind-,$(ART_PHONY_TEST_HOST_SUFFIX)))
ifneq ($(HOST_PREFER_32_BIT),true)
$(eval $(call define-test-art-gtest-combination,host,HOST,,$(2ND_ART_PHONY_TEST_HOST_SUFFIX)))
$(eval $(call define-test-art-gtest-combination,host,HOST,valgrind-,$(2ND_ART_PHONY_TEST_HOST_SUFFIX)))
endif

# Clear locally defined variables.
define-art-gtest-rule-target :=
define-art-gtest-rule-host :=
define-art-gtest :=
define-test-art-gtest-combination :=
RUNTIME_GTEST_COMMON_SRC_FILES :=
COMPILER_GTEST_COMMON_SRC_FILES :=
RUNTIME_GTEST_TARGET_SRC_FILES :=
RUNTIME_GTEST_HOST_SRC_FILES :=
COMPILER_GTEST_TARGET_SRC_FILES :=
COMPILER_GTEST_HOST_SRC_FILES :=
ART_TEST_CFLAGS :=
ART_TEST_HOST_GTEST$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_GTEST$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_GTEST_RULES :=
ART_TEST_HOST_VALGRIND_GTEST$(ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_VALGRIND_GTEST$(2ND_ART_PHONY_TEST_HOST_SUFFIX)_RULES :=
ART_TEST_HOST_VALGRIND_GTEST_RULES :=
ART_TEST_TARGET_GTEST$(ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_GTEST$(2ND_ART_PHONY_TEST_TARGET_SUFFIX)_RULES :=
ART_TEST_TARGET_GTEST_RULES :=
ART_GTEST_TARGET_ANDROID_ROOT :=
ART_GTEST_class_linker_test_DEX_DEPS :=
ART_GTEST_compiler_driver_test_DEX_DEPS :=
ART_GTEST_dex_file_test_DEX_DEPS :=
ART_GTEST_exception_test_DEX_DEPS :=
ART_GTEST_elf_writer_test_HOST_DEPS :=
ART_GTEST_elf_writer_test_TARGET_DEPS :=
ART_GTEST_jni_compiler_test_DEX_DEPS :=
ART_GTEST_jni_internal_test_DEX_DEPS :=
ART_GTEST_oat_file_assistant_test_DEX_DEPS :=
ART_GTEST_oat_file_assistant_test_HOST_DEPS :=
ART_GTEST_oat_file_assistant_test_TARGET_DEPS :=
ART_GTEST_dex2oat_test_DEX_DEPS :=
ART_GTEST_dex2oat_test_HOST_DEPS :=
ART_GTEST_dex2oat_test_TARGET_DEPS :=
ART_GTEST_object_test_DEX_DEPS :=
ART_GTEST_proxy_test_DEX_DEPS :=
ART_GTEST_reflection_test_DEX_DEPS :=
ART_GTEST_stub_test_DEX_DEPS :=
ART_GTEST_transaction_test_DEX_DEPS :=
ART_GTEST_dex2oat_environment_tests_DEX_DEPS :=
ART_VALGRIND_DEPENDENCIES :=
$(foreach dir,$(GTEST_DEX_DIRECTORIES), $(eval ART_TEST_TARGET_GTEST_$(dir)_DEX :=))
$(foreach dir,$(GTEST_DEX_DIRECTORIES), $(eval ART_TEST_HOST_GTEST_$(dir)_DEX :=))
ART_TEST_HOST_GTEST_MainStripped_DEX :=
ART_TEST_TARGET_GTEST_MainStripped_DEX :=
GTEST_DEX_DIRECTORIES :=
LOCAL_PATH :=
