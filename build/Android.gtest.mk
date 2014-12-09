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

# Subdirectories in art/test which contain dex files used as inputs for gtests.
GTEST_DEX_DIRECTORIES := \
  AbstractMethod \
  AllFields \
  ExceptionHandle \
  GetMethodSignature \
  Interfaces \
  Main \
  MyClass \
  MyClassNatives \
  Nested \
  NonStaticLeafMethods \
  ProtoCompare \
  ProtoCompare2 \
  StaticLeafMethods \
  Statics \
  StaticsFromCode \
  Transaction \
  XandY

# Create build rules for each dex file recording the dependency.
$(foreach dir,$(GTEST_DEX_DIRECTORIES), $(eval $(call build-art-test-dex,art-gtest,$(dir), \
  $(ART_TARGET_NATIVETEST_OUT),art/build/Android.gtest.mk,ART_TEST_TARGET_GTEST_$(dir)_DEX, \
  ART_TEST_HOST_GTEST_$(dir)_DEX)))

# Dex file dependencies for each gtest.
ART_GTEST_class_linker_test_DEX_DEPS := Interfaces MyClass Nested Statics StaticsFromCode
ART_GTEST_compiler_driver_test_DEX_DEPS := AbstractMethod
ART_GTEST_dex_file_test_DEX_DEPS := GetMethodSignature
ART_GTEST_exception_test_DEX_DEPS := ExceptionHandle
ART_GTEST_jni_compiler_test_DEX_DEPS := MyClassNatives
ART_GTEST_jni_internal_test_DEX_DEPS := AllFields StaticLeafMethods
ART_GTEST_object_test_DEX_DEPS := ProtoCompare ProtoCompare2 StaticsFromCode XandY
ART_GTEST_proxy_test_DEX_DEPS := Interfaces
ART_GTEST_reflection_test_DEX_DEPS := Main NonStaticLeafMethods StaticLeafMethods
ART_GTEST_stub_test_DEX_DEPS := AllFields
ART_GTEST_transaction_test_DEX_DEPS := Transaction

# The elf writer test has dependencies on core.oat.
ART_GTEST_elf_writer_test_HOST_DEPS := $(HOST_CORE_OAT_OUT) $(2ND_HOST_CORE_OAT_OUT)
ART_GTEST_elf_writer_test_TARGET_DEPS := $(TARGET_CORE_OAT_OUT) $(2ND_TARGET_CORE_OAT_OUT)

# The path for which all the source files are relative, not actually the current directory.
LOCAL_PATH := art

RUNTIME_GTEST_COMMON_SRC_FILES := \
  runtime/arch/arch_test.cc \
  runtime/arch/memcmp16_test.cc \
  runtime/arch/stub_test.cc \
  runtime/barrier_test.cc \
  runtime/base/bit_field_test.cc \
  runtime/base/bit_vector_test.cc \
  runtime/base/hash_set_test.cc \
  runtime/base/hex_dump_test.cc \
  runtime/base/histogram_test.cc \
  runtime/base/mutex_test.cc \
  runtime/base/scoped_flock_test.cc \
  runtime/base/stringprintf_test.cc \
  runtime/base/timing_logger_test.cc \
  runtime/base/unix_file/fd_file_test.cc \
  runtime/base/unix_file/mapped_file_test.cc \
  runtime/base/unix_file/null_file_test.cc \
  runtime/base/unix_file/random_access_file_utils_test.cc \
  runtime/base/unix_file/string_file_test.cc \
  runtime/class_linker_test.cc \
  runtime/dex_file_test.cc \
  runtime/dex_file_verifier_test.cc \
  runtime/dex_instruction_visitor_test.cc \
  runtime/dex_method_iterator_test.cc \
  runtime/entrypoints/math_entrypoints_test.cc \
  runtime/entrypoints/quick/quick_trampoline_entrypoints_test.cc \
  runtime/entrypoints_order_test.cc \
  runtime/exception_test.cc \
  runtime/gc/accounting/card_table_test.cc \
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
  runtime/monitor_pool_test.cc \
  runtime/monitor_test.cc \
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
  compiler/dex/global_value_numbering_test.cc \
  compiler/dex/local_value_numbering_test.cc \
  compiler/dex/mir_graph_test.cc \
  compiler/dex/mir_optimization_test.cc \
  compiler/driver/compiler_driver_test.cc \
  compiler/elf_writer_test.cc \
  compiler/image_test.cc \
  compiler/jni/jni_compiler_test.cc \
  compiler/oat_test.cc \
  compiler/optimizing/codegen_test.cc \
  compiler/optimizing/dominator_test.cc \
  compiler/optimizing/find_loops_test.cc \
  compiler/optimizing/graph_test.cc \
  compiler/optimizing/linearize_test.cc \
  compiler/optimizing/liveness_test.cc \
  compiler/optimizing/live_interval_test.cc \
  compiler/optimizing/live_ranges_test.cc \
  compiler/optimizing/parallel_move_test.cc \
  compiler/optimizing/pretty_printer_test.cc \
  compiler/optimizing/register_allocator_test.cc \
  compiler/optimizing/ssa_test.cc \
  compiler/optimizing/stack_map_test.cc \
  compiler/output_stream_test.cc \
  compiler/utils/arena_allocator_test.cc \
  compiler/utils/dedupe_set_test.cc \
  compiler/utils/swap_space_test.cc \
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
  compiler/utils//assembler_thumb_test.cc \
  compiler/utils/x86/assembler_x86_test.cc \
  compiler/utils/x86_64/assembler_x86_64_test.cc

ART_TEST_CFLAGS :=
ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  ART_TEST_CFLAGS += -DART_USE_PORTABLE_COMPILER=1
endif

include $(CLEAR_VARS)
LOCAL_MODULE := libart-gtest
LOCAL_MODULE_TAGS := optional
LOCAL_CPP_EXTENSION := cc
LOCAL_CFLAGS := $(ART_TARGET_CFLAGS)
LOCAL_SRC_FILES := runtime/common_runtime_test.cc compiler/common_compiler_test.cc
LOCAL_C_INCLUDES := $(ART_C_INCLUDES) art/runtime art/compiler
LOCAL_SHARED_LIBRARIES := libcutils libartd libartd-compiler libdl
LOCAL_STATIC_LIBRARIES += libgtest_libc++
LOCAL_CLANG := $(ART_TARGET_CLANG)
LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.gtest.mk
include external/libcxx/libcxx.mk
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libart-gtest
LOCAL_MODULE_TAGS := optional
LOCAL_CPP_EXTENSION := cc
LOCAL_CFLAGS := $(ART_HOST_CFLAGS)
LOCAL_SRC_FILES := runtime/common_runtime_test.cc compiler/common_compiler_test.cc
LOCAL_C_INCLUDES := $(ART_C_INCLUDES) art/runtime art/compiler
LOCAL_SHARED_LIBRARIES := libartd libartd-compiler
LOCAL_STATIC_LIBRARIES := libcutils
ifneq ($(WITHOUT_HOST_CLANG),true)
  # GCC host compiled tests fail with this linked, presumably due to destructors that run.
  LOCAL_STATIC_LIBRARIES += libgtest_libc++_host
endif
LOCAL_LDLIBS += -ldl -lpthread
LOCAL_MULTILIB := both
LOCAL_CLANG := $(ART_HOST_CLANG)
LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.gtest.mk
include external/libcxx/libcxx.mk
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

# Define a make rule for a target device gtest.
# $(1): gtest name - the name of the test we're building such as leb128_test.
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
define define-art-gtest-rule-target
  gtest_rule := test-art-target-gtest-$(1)$$($(2)ART_PHONY_TEST_TARGET_SUFFIX)

  # Add the test dependencies to test-art-target-sync, which will be a prerequisite for the test
  # to ensure files are pushed to the device.
  TEST_ART_TARGET_SYNC_DEPS += \
    $$(ART_GTEST_$(1)_TARGET_DEPS) \
    $(foreach file,$(ART_GTEST_$(1)_DEX_DEPS),$(ART_TEST_TARGET_GTEST_$(file)_DEX)) \
    $$(ART_TARGET_NATIVETEST_OUT)/$$(TARGET_$(2)ARCH)/$(1) \
    $$($(2)TARGET_OUT_SHARED_LIBRARIES)/libjavacore.so

.PHONY: $$(gtest_rule)
$$(gtest_rule): test-art-target-sync
	$(hide) adb shell touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$$$$PPID
	$(hide) adb shell rm $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$$$$PPID
	$(hide) adb shell chmod 755 $(ART_TARGET_NATIVETEST_DIR)/$(TARGET_$(2)ARCH)/$(1)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  (adb shell "$(ART_TARGET_NATIVETEST_DIR)/$(TARGET_$(2)ARCH)/$(1) && touch $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$$$$PPID" \
	  && (adb pull $(ART_TARGET_TEST_DIR)/$(TARGET_$(2)ARCH)/$$@-$$$$PPID /tmp/ \
	      && $$(call ART_TEST_PASSED,$$@)) \
	  || $$(call ART_TEST_FAILED,$$@))
	$(hide) rm /tmp/$$@-$$$$PPID

  ART_TEST_TARGET_GTEST$($(2)ART_PHONY_TEST_TARGET_SUFFIX)_RULES += $$(gtest_rule)
  ART_TEST_TARGET_GTEST_RULES += $$(gtest_rule)
  ART_TEST_TARGET_GTEST_$(1)_RULES += $$(gtest_rule)

  # Clear locally defined variables.
  gtest_rule :=
endef  # define-art-gtest-rule-target

# Define make rules for a host gtests.
# $(1): gtest name - the name of the test we're building such as leb128_test.
# $(2): 2ND_ or undefined - used to differentiate between the primary and secondary architecture.
define define-art-gtest-rule-host
  gtest_rule := test-art-host-gtest-$(1)$$($(2)ART_PHONY_TEST_HOST_SUFFIX)
  gtest_exe := $$(HOST_OUT_EXECUTABLES)/$(1)$$($(2)ART_PHONY_TEST_HOST_SUFFIX)
  # Dependencies for all host gtests.
  gtest_deps := $$(HOST_CORE_DEX_LOCATIONS) \
    $$($(2)ART_HOST_OUT_SHARED_LIBRARIES)/libjavacore$$(ART_HOST_SHLIB_EXTENSION)


.PHONY: $$(gtest_rule)
$$(gtest_rule): $$(gtest_exe) $$(ART_GTEST_$(1)_HOST_DEPS) $(foreach file,$(ART_GTEST_$(1)_DEX_DEPS),$(ART_TEST_HOST_GTEST_$(file)_DEX)) $$(gtest_deps)
	$(hide) ($$(call ART_TEST_SKIP,$$@) && $$< && $$(call ART_TEST_PASSED,$$@)) \
	  || $$(call ART_TEST_FAILED,$$@)

  ART_TEST_HOST_GTEST$$($(2)ART_PHONY_TEST_HOST_SUFFIX)_RULES += $$(gtest_rule)
  ART_TEST_HOST_GTEST_RULES += $$(gtest_rule)
  ART_TEST_HOST_GTEST_$(1)_RULES += $$(gtest_rule)

.PHONY: valgrind-$$(gtest_rule)
valgrind-$$(gtest_rule): $$(gtest_exe) $$(ART_GTEST_$(1)_HOST_DEPS) $(foreach file,$(ART_GTEST_$(1)_DEX_DEPS),$(ART_TEST_HOST_GTEST_$(file)_DEX)) $$(gtest_deps)
	$(hide) $$(call ART_TEST_SKIP,$$@) && \
	  valgrind --leak-check=full --error-exitcode=1 $$< && $$(call ART_TEST_PASSED,$$@) \
	    || $$(call ART_TEST_FAILED,$$@)

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
  LOCAL_C_INCLUDES += $$(ART_C_INCLUDES) art/runtime $$(art_gtest_extra_c_includes)
  LOCAL_SHARED_LIBRARIES += libartd $$(art_gtest_extra_shared_libraries) libart-gtest
  LOCAL_WHOLE_STATIC_LIBRARIES += libsigchain

  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += art/build/Android.gtest.mk

  # Mac OS linker doesn't understand --export-dynamic.
  ifneq ($$(HOST_OS)-$$(art_target_or_host),darwin-host)
    # Allow jni_compiler_test to find Java_MyClassNatives_bar within itself using dlopen(NULL, ...).
    LOCAL_LDFLAGS := -Wl,--export-dynamic -Wl,-u,Java_MyClassNatives_bar -Wl,-u,Java_MyClassNatives_sbar
  endif

  LOCAL_CFLAGS := $$(ART_TEST_CFLAGS)
  include external/libcxx/libcxx.mk
  ifeq ($$(art_target_or_host),target)
    $$(eval $$(call set-target-local-clang-vars))
    $$(eval $$(call set-target-local-cflags-vars,debug))
    LOCAL_SHARED_LIBRARIES += libdl libicuuc libicui18n libnativehelper libz libcutils libvixl
    LOCAL_MODULE_PATH_32 := $$(ART_TARGET_NATIVETEST_OUT)/$$(ART_TARGET_ARCH_32)
    LOCAL_MODULE_PATH_64 := $$(ART_TARGET_NATIVETEST_OUT)/$$(ART_TARGET_ARCH_64)
    LOCAL_MULTILIB := both
    include $$(BUILD_EXECUTABLE)

    ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES :=
    ifdef TARGET_2ND_ARCH
      $$(eval $$(call define-art-gtest-rule-target,$$(art_gtest_name),2ND_))
    endif
    $$(eval $$(call define-art-gtest-rule-target,$$(art_gtest_name),))

    # A rule to run the different architecture versions of the gtest.
.PHONY: test-art-target-gtest-$$(art_gtest_name)
test-art-target-gtest-$$(art_gtest_name): $$(ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES)
	$$(hide) $$(call ART_TEST_PREREQ_FINISHED,$$@)

    # Clear locally defined variables.
    ART_TEST_TARGET_GTEST_$$(art_gtest_name)_RULES :=
  else # host
    LOCAL_CLANG := $$(ART_HOST_CLANG)
    LOCAL_CFLAGS += $$(ART_HOST_CFLAGS) $$(ART_HOST_DEBUG_CFLAGS)
    LOCAL_SHARED_LIBRARIES += libicuuc-host libicui18n-host libnativehelper libz-host
    LOCAL_STATIC_LIBRARIES += libcutils libvixl
    LOCAL_LDLIBS += -lpthread -ldl
    LOCAL_IS_HOST_MODULE := true
    LOCAL_MULTILIB := both
    LOCAL_MODULE_STEM_32 := $$(art_gtest_name)32
    LOCAL_MODULE_STEM_64 := $$(art_gtest_name)64
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
endef  # define-art-gtest

ifeq ($(ART_BUILD_TARGET),true)
  $(foreach file,$(RUNTIME_GTEST_TARGET_SRC_FILES), $(eval $(call define-art-gtest,target,$(file),,)))
  $(foreach file,$(COMPILER_GTEST_TARGET_SRC_FILES), $(eval $(call define-art-gtest,target,$(file),art/compiler,libartd-compiler)))
endif
ifeq ($(ART_BUILD_HOST),true)
  $(foreach file,$(RUNTIME_GTEST_HOST_SRC_FILES), $(eval $(call define-art-gtest,host,$(file),,)))
  $(foreach file,$(COMPILER_GTEST_HOST_SRC_FILES), $(eval $(call define-art-gtest,host,$(file),art/compiler,libartd-compiler)))
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
  dependencies := $$(ART_TEST_$(2)_GTEST$(4)_RULES)

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
ART_GTEST_class_linker_test_DEX_DEPS :=
ART_GTEST_compiler_driver_test_DEX_DEPS :=
ART_GTEST_dex_file_test_DEX_DEPS :=
ART_GTEST_exception_test_DEX_DEPS :=
ART_GTEST_elf_writer_test_HOST_DEPS :=
ART_GTEST_elf_writer_test_TARGET_DEPS :=
ART_GTEST_jni_compiler_test_DEX_DEPS :=
ART_GTEST_jni_internal_test_DEX_DEPS :=
ART_GTEST_object_test_DEX_DEPS :=
ART_GTEST_proxy_test_DEX_DEPS :=
ART_GTEST_reflection_test_DEX_DEPS :=
ART_GTEST_stub_test_DEX_DEPS :=
ART_GTEST_transaction_test_DEX_DEPS :=
$(foreach dir,$(GTEST_DEX_DIRECTORIES), $(eval ART_TEST_TARGET_GTEST_$(dir)_DEX :=))
$(foreach dir,$(GTEST_DEX_DIRECTORIES), $(eval ART_TEST_HOST_GTEST_$(dir)_DEX :=))
GTEST_DEX_DIRECTORIES :=
LOCAL_PATH :=
