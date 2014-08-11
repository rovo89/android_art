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

include art/build/Android.common_build.mk

LIBART_COMMON_SRC_FILES := \
  atomic.cc.arm \
  barrier.cc \
  base/allocator.cc \
  base/bit_vector.cc \
  base/hex_dump.cc \
  base/logging.cc \
  base/mutex.cc \
  base/scoped_flock.cc \
  base/stringpiece.cc \
  base/stringprintf.cc \
  base/timing_logger.cc \
  base/unix_file/fd_file.cc \
  base/unix_file/mapped_file.cc \
  base/unix_file/null_file.cc \
  base/unix_file/random_access_file_utils.cc \
  base/unix_file/string_file.cc \
  check_jni.cc \
  class_linker.cc \
  common_throws.cc \
  debugger.cc \
  dex_file.cc \
  dex_file_verifier.cc \
  dex_instruction.cc \
  elf_file.cc \
  field_helper.cc \
  gc/allocator/dlmalloc.cc \
  gc/allocator/rosalloc.cc \
  gc/accounting/card_table.cc \
  gc/accounting/heap_bitmap.cc \
  gc/accounting/mod_union_table.cc \
  gc/accounting/remembered_set.cc \
  gc/accounting/space_bitmap.cc \
  gc/collector/concurrent_copying.cc \
  gc/collector/garbage_collector.cc \
  gc/collector/immune_region.cc \
  gc/collector/mark_compact.cc \
  gc/collector/mark_sweep.cc \
  gc/collector/partial_mark_sweep.cc \
  gc/collector/semi_space.cc \
  gc/collector/sticky_mark_sweep.cc \
  gc/gc_cause.cc \
  gc/heap.cc \
  gc/reference_processor.cc \
  gc/reference_queue.cc \
  gc/space/bump_pointer_space.cc \
  gc/space/dlmalloc_space.cc \
  gc/space/image_space.cc \
  gc/space/large_object_space.cc \
  gc/space/malloc_space.cc \
  gc/space/rosalloc_space.cc \
  gc/space/space.cc \
  gc/space/zygote_space.cc \
  hprof/hprof.cc \
  image.cc \
  indirect_reference_table.cc \
  instruction_set.cc \
  instrumentation.cc \
  intern_table.cc \
  interpreter/interpreter.cc \
  interpreter/interpreter_common.cc \
  interpreter/interpreter_switch_impl.cc \
  jdwp/jdwp_event.cc \
  jdwp/jdwp_expand_buf.cc \
  jdwp/jdwp_handler.cc \
  jdwp/jdwp_main.cc \
  jdwp/jdwp_request.cc \
  jdwp/jdwp_socket.cc \
  jdwp/object_registry.cc \
  jni_internal.cc \
  jobject_comparator.cc \
  mem_map.cc \
  memory_region.cc \
  method_helper.cc \
  mirror/art_field.cc \
  mirror/art_method.cc \
  mirror/array.cc \
  mirror/class.cc \
  mirror/dex_cache.cc \
  mirror/object.cc \
  mirror/reference.cc \
  mirror/stack_trace_element.cc \
  mirror/string.cc \
  mirror/throwable.cc \
  monitor.cc \
  native_bridge_art_interface.cc \
  native/dalvik_system_DexFile.cc \
  native/dalvik_system_VMDebug.cc \
  native/dalvik_system_VMRuntime.cc \
  native/dalvik_system_VMStack.cc \
  native/dalvik_system_ZygoteHooks.cc \
  native/java_lang_Class.cc \
  native/java_lang_DexCache.cc \
  native/java_lang_Object.cc \
  native/java_lang_Runtime.cc \
  native/java_lang_String.cc \
  native/java_lang_System.cc \
  native/java_lang_Thread.cc \
  native/java_lang_Throwable.cc \
  native/java_lang_VMClassLoader.cc \
  native/java_lang_ref_FinalizerReference.cc \
  native/java_lang_ref_Reference.cc \
  native/java_lang_reflect_Array.cc \
  native/java_lang_reflect_Constructor.cc \
  native/java_lang_reflect_Field.cc \
  native/java_lang_reflect_Method.cc \
  native/java_lang_reflect_Proxy.cc \
  native/java_util_concurrent_atomic_AtomicLong.cc \
  native/org_apache_harmony_dalvik_ddmc_DdmServer.cc \
  native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.cc \
  native/sun_misc_Unsafe.cc \
  oat.cc \
  oat_file.cc \
  object_lock.cc \
  offsets.cc \
  os_linux.cc \
  parsed_options.cc \
  primitive.cc \
  quick_exception_handler.cc \
  quick/inline_method_analyser.cc \
  reference_table.cc \
  reflection.cc \
  runtime.cc \
  signal_catcher.cc \
  stack.cc \
  thread.cc \
  thread_list.cc \
  thread_pool.cc \
  throw_location.cc \
  trace.cc \
  transaction.cc \
  profiler.cc \
  fault_handler.cc \
  utf.cc \
  utils.cc \
  verifier/dex_gc_map.cc \
  verifier/instruction_flags.cc \
  verifier/method_verifier.cc \
  verifier/reg_type.cc \
  verifier/reg_type_cache.cc \
  verifier/register_line.cc \
  well_known_classes.cc \
  zip_archive.cc

LIBART_COMMON_SRC_FILES += \
  arch/context.cc \
  arch/memcmp16.cc \
  arch/arm/registers_arm.cc \
  arch/arm64/registers_arm64.cc \
  arch/x86/registers_x86.cc \
  arch/mips/registers_mips.cc \
  entrypoints/entrypoint_utils.cc \
  entrypoints/interpreter/interpreter_entrypoints.cc \
  entrypoints/jni/jni_entrypoints.cc \
  entrypoints/math_entrypoints.cc \
  entrypoints/portable/portable_alloc_entrypoints.cc \
  entrypoints/portable/portable_cast_entrypoints.cc \
  entrypoints/portable/portable_dexcache_entrypoints.cc \
  entrypoints/portable/portable_field_entrypoints.cc \
  entrypoints/portable/portable_fillarray_entrypoints.cc \
  entrypoints/portable/portable_invoke_entrypoints.cc \
  entrypoints/portable/portable_jni_entrypoints.cc \
  entrypoints/portable/portable_lock_entrypoints.cc \
  entrypoints/portable/portable_thread_entrypoints.cc \
  entrypoints/portable/portable_throw_entrypoints.cc \
  entrypoints/portable/portable_trampoline_entrypoints.cc \
  entrypoints/quick/quick_alloc_entrypoints.cc \
  entrypoints/quick/quick_cast_entrypoints.cc \
  entrypoints/quick/quick_deoptimization_entrypoints.cc \
  entrypoints/quick/quick_dexcache_entrypoints.cc \
  entrypoints/quick/quick_field_entrypoints.cc \
  entrypoints/quick/quick_fillarray_entrypoints.cc \
  entrypoints/quick/quick_instrumentation_entrypoints.cc \
  entrypoints/quick/quick_jni_entrypoints.cc \
  entrypoints/quick/quick_lock_entrypoints.cc \
  entrypoints/quick/quick_math_entrypoints.cc \
  entrypoints/quick/quick_thread_entrypoints.cc \
  entrypoints/quick/quick_throw_entrypoints.cc \
  entrypoints/quick/quick_trampoline_entrypoints.cc

# Source files that only compile with GCC.
LIBART_GCC_ONLY_SRC_FILES := \
  interpreter/interpreter_goto_table_impl.cc

LIBART_TARGET_LDFLAGS :=
LIBART_HOST_LDFLAGS :=

LIBART_TARGET_SRC_FILES := \
  $(LIBART_COMMON_SRC_FILES) \
  base/logging_android.cc \
  jdwp/jdwp_adb.cc \
  monitor_android.cc \
  runtime_android.cc \
  thread_android.cc

LIBART_TARGET_SRC_FILES_arm := \
  arch/arm/context_arm.cc.arm \
  arch/arm/entrypoints_init_arm.cc \
  arch/arm/jni_entrypoints_arm.S \
  arch/arm/memcmp16_arm.S \
  arch/arm/portable_entrypoints_arm.S \
  arch/arm/quick_entrypoints_arm.S \
  arch/arm/arm_sdiv.S \
  arch/arm/thread_arm.cc \
  arch/arm/fault_handler_arm.cc

LIBART_TARGET_SRC_FILES_arm64 := \
  arch/arm64/context_arm64.cc \
  arch/arm64/entrypoints_init_arm64.cc \
  arch/arm64/jni_entrypoints_arm64.S \
  arch/arm64/memcmp16_arm64.S \
  arch/arm64/portable_entrypoints_arm64.S \
  arch/arm64/quick_entrypoints_arm64.S \
  arch/arm64/thread_arm64.cc \
  monitor_pool.cc \
  arch/arm64/fault_handler_arm64.cc

LIBART_SRC_FILES_x86 := \
  arch/x86/context_x86.cc \
  arch/x86/entrypoints_init_x86.cc \
  arch/x86/jni_entrypoints_x86.S \
  arch/x86/memcmp16_x86.S \
  arch/x86/portable_entrypoints_x86.S \
  arch/x86/quick_entrypoints_x86.S \
  arch/x86/thread_x86.cc \
  arch/x86/fault_handler_x86.cc

LIBART_TARGET_SRC_FILES_x86 := \
  $(LIBART_SRC_FILES_x86)

# Note that the fault_handler_x86.cc is not a mistake.  This file is
# shared between the x86 and x86_64 architectures.
LIBART_SRC_FILES_x86_64 := \
  arch/x86_64/context_x86_64.cc \
  arch/x86_64/entrypoints_init_x86_64.cc \
  arch/x86_64/jni_entrypoints_x86_64.S \
  arch/x86_64/memcmp16_x86_64.S \
  arch/x86_64/portable_entrypoints_x86_64.S \
  arch/x86_64/quick_entrypoints_x86_64.S \
  arch/x86_64/thread_x86_64.cc \
  monitor_pool.cc \
  arch/x86/fault_handler_x86.cc

LIBART_TARGET_SRC_FILES_x86_64 := \
  $(LIBART_SRC_FILES_x86_64) \

LIBART_TARGET_SRC_FILES_mips := \
  arch/mips/context_mips.cc \
  arch/mips/entrypoints_init_mips.cc \
  arch/mips/jni_entrypoints_mips.S \
  arch/mips/memcmp16_mips.S \
  arch/mips/portable_entrypoints_mips.S \
  arch/mips/quick_entrypoints_mips.S \
  arch/mips/thread_mips.cc \
  arch/mips/fault_handler_mips.cc

ifeq ($(TARGET_ARCH),mips64)
$(info TODOMips64: $(LOCAL_PATH)/Android.mk Add mips64 specific runtime files)
endif # TARGET_ARCH != mips64

LIBART_HOST_SRC_FILES := \
  $(LIBART_COMMON_SRC_FILES) \
  base/logging_linux.cc \
  monitor_linux.cc \
  runtime_linux.cc \
  thread_linux.cc

LIBART_HOST_SRC_FILES_32 := \
  $(LIBART_SRC_FILES_x86)

LIBART_HOST_SRC_FILES_64 := \
  $(LIBART_SRC_FILES_x86_64)

LIBART_ENUM_OPERATOR_OUT_HEADER_FILES := \
  arch/x86_64/registers_x86_64.h \
  base/allocator.h \
  base/mutex.h \
  dex_file.h \
  dex_instruction.h \
  gc/collector/gc_type.h \
  gc/collector_type.h \
  gc/space/space.h \
  gc/heap.h \
  indirect_reference_table.h \
  instruction_set.h \
  invoke_type.h \
  jdwp/jdwp.h \
  jdwp/jdwp_constants.h \
  lock_word.h \
  mirror/class.h \
  oat.h \
  object_callbacks.h \
  quick/inline_method_analyser.h \
  thread.h \
  thread_state.h \
  verifier/method_verifier.h

LIBART_CFLAGS :=
ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  LIBART_CFLAGS += -DART_USE_PORTABLE_COMPILER=1
endif

ifeq ($(MALLOC_IMPL),dlmalloc)
  LIBART_CFLAGS += -DUSE_DLMALLOC
else
  LIBART_CFLAGS += -DUSE_JEMALLOC
endif

ifeq ($(ART_USE_HSPACE_COMPACT),true)
  LIBART_CFLAGS += -DART_USE_HSPACE_COMPACT
endif

# $(1): target or host
# $(2): ndebug or debug
define build-libart
  ifneq ($(1),target)
    ifneq ($(1),host)
      $$(error expected target or host for argument 1, received $(1))
    endif
  endif
  ifneq ($(2),ndebug)
    ifneq ($(2),debug)
      $$(error expected ndebug or debug for argument 2, received $(2))
    endif
  endif

  art_target_or_host := $(1)
  art_ndebug_or_debug := $(2)

  include $$(CLEAR_VARS)
  LOCAL_CPP_EXTENSION := $$(ART_CPP_EXTENSION)
  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := libart
    LOCAL_FDO_SUPPORT := true
  else # debug
    LOCAL_MODULE := libartd
  endif

  LOCAL_MODULE_TAGS := optional
  LOCAL_MODULE_CLASS := SHARED_LIBRARIES

  ifeq ($$(art_target_or_host),target)
    LOCAL_SRC_FILES := $$(LIBART_TARGET_SRC_FILES)
    $$(foreach arch,$$(ART_TARGET_SUPPORTED_ARCH), \
      $$(eval LOCAL_SRC_FILES_$$(arch) := $$$$(LIBART_TARGET_SRC_FILES_$$(arch))))
  else # host
    LOCAL_SRC_FILES := $$(LIBART_HOST_SRC_FILES)
    LOCAL_SRC_FILES_32 := $$(LIBART_HOST_SRC_FILES_32)
    LOCAL_SRC_FILES_64 := $$(LIBART_HOST_SRC_FILES_64)
    LOCAL_IS_HOST_MODULE := true
  endif

  GENERATED_SRC_DIR := $$(call local-generated-sources-dir)
  ENUM_OPERATOR_OUT_CC_FILES := $$(patsubst %.h,%_operator_out.cc,$$(LIBART_ENUM_OPERATOR_OUT_HEADER_FILES))
  ENUM_OPERATOR_OUT_GEN := $$(addprefix $$(GENERATED_SRC_DIR)/,$$(ENUM_OPERATOR_OUT_CC_FILES))

$$(ENUM_OPERATOR_OUT_GEN): art/tools/generate-operator-out.py
$$(ENUM_OPERATOR_OUT_GEN): PRIVATE_CUSTOM_TOOL = art/tools/generate-operator-out.py $(LOCAL_PATH) $$< > $$@
$$(ENUM_OPERATOR_OUT_GEN): $$(GENERATED_SRC_DIR)/%_operator_out.cc : $(LOCAL_PATH)/%.h
	$$(transform-generated-source)

  LOCAL_GENERATED_SOURCES += $$(ENUM_OPERATOR_OUT_GEN)

  LOCAL_CFLAGS := $$(LIBART_CFLAGS)
  LOCAL_LDFLAGS := $$(LIBART_LDFLAGS)
  ifeq ($$(art_target_or_host),target)
    LOCAL_LDFLAGS += $$(LIBART_TARGET_LDFLAGS)
  else
    LOCAL_LDFLAGS += $$(LIBART_HOST_LDFLAGS)
  endif
  $$(foreach arch,$$(ART_TARGET_SUPPORTED_ARCH), \
    $$(eval LOCAL_LDFLAGS_$$(arch) := $$(LIBART_TARGET_LDFLAGS_$$(arch))))

  # Clang usage
  ifeq ($$(art_target_or_host),target)
    $$(eval $$(call set-target-local-clang-vars))
    $$(eval $$(call set-target-local-cflags-vars,$(2)))
    # TODO: Loop with ifeq, ART_TARGET_CLANG
    ifneq ($$(ART_TARGET_CLANG_$$(TARGET_ARCH)),true)
      LOCAL_SRC_FILES_$$(TARGET_ARCH) += $$(LIBART_GCC_ONLY_SRC_FILES)
    endif
    ifneq ($$(ART_TARGET_CLANG_$$(TARGET_2ND_ARCH)),true)
      LOCAL_SRC_FILES_$$(TARGET_2ND_ARCH) += $$(LIBART_GCC_ONLY_SRC_FILES)
    endif
  else # host
    LOCAL_CLANG := $$(ART_HOST_CLANG)
    ifeq ($$(ART_HOST_CLANG),false)
      LOCAL_SRC_FILES += $$(LIBART_GCC_ONLY_SRC_FILES)
    endif
    LOCAL_CFLAGS += $$(ART_HOST_CFLAGS)
    ifeq ($$(art_ndebug_or_debug),debug)
      LOCAL_CFLAGS += $$(ART_HOST_DEBUG_CFLAGS)
      LOCAL_LDLIBS += $$(ART_HOST_DEBUG_LDLIBS)
      LOCAL_STATIC_LIBRARIES := libgtest_host
    else
      LOCAL_CFLAGS += $$(ART_HOST_NON_DEBUG_CFLAGS)
    endif
  endif

  LOCAL_C_INCLUDES += $$(ART_C_INCLUDES)
  LOCAL_C_INCLUDES += art/sigchainlib

  LOCAL_SHARED_LIBRARIES += liblog libnativehelper libnativebridge
  include external/libcxx/libcxx.mk
  LOCAL_SHARED_LIBRARIES += libbacktrace_libc++
  ifeq ($$(art_target_or_host),target)
    LOCAL_SHARED_LIBRARIES += libcutils libdl libselinux libutils libsigchain
    LOCAL_STATIC_LIBRARIES := libziparchive libz
    ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
	  LOCAL_WHOLE_STATIC_LIBRARIES += libqc-art
    endif
  else # host
    LOCAL_STATIC_LIBRARIES += libcutils libziparchive-host libz libutils
    LOCAL_SHARED_LIBRARIES += libsigchain
    LOCAL_LDLIBS += -ldl -lpthread
    ifeq ($$(HOST_OS),linux)
      LOCAL_LDLIBS += -lrt
    endif
    LOCAL_MULTILIB := both
  endif
  ifeq ($$(ART_USE_PORTABLE_COMPILER),true)
    include $$(LLVM_GEN_INTRINSICS_MK)
    ifeq ($$(art_target_or_host),target)
      include $$(LLVM_DEVICE_BUILD_MK)
    else # host
      include $$(LLVM_HOST_BUILD_MK)
    endif
  endif
  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
#  LOCAL_ADDITIONAL_DEPENDENCIES += $$(LOCAL_PATH)/Android.mk

  ifeq ($$(art_target_or_host),target)
    LOCAL_MODULE_TARGET_ARCH := $$(ART_TARGET_SUPPORTED_ARCH)
  endif

  ifeq ($$(art_target_or_host),target)
    ifneq ($$(art_ndebug_or_debug),debug)
      # Leave the symbols in the shared library so that stack unwinders can
      # produce meaningful name resolution.
      LOCAL_STRIP_MODULE := keep_symbols
    endif
    include $$(BUILD_SHARED_LIBRARY)
  else # host
    include $$(BUILD_HOST_SHARED_LIBRARY)
  endif

  # Clear locally defined variables.
  GENERATED_SRC_DIR :=
  ENUM_OPERATOR_OUT_CC_FILES :=
  ENUM_OPERATOR_OUT_GEN :=
  art_target_or_host :=
  art_ndebug_or_debug :=
endef

# We always build dex2oat and dependencies, even if the host build is otherwise disabled, since
# they are used to cross compile for the target.
ifeq ($(ART_BUILD_NDEBUG),true)
  $(eval $(call build-libart,host,ndebug))
endif
ifeq ($(ART_BUILD_DEBUG),true)
  $(eval $(call build-libart,host,debug))
endif

ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
#  $(error $(call build-libart,target,ndebug))
  $(eval $(call build-libart,target,ndebug))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-libart,target,debug))
endif

# Clear locally defined variables.
LOCAL_PATH :=
LIBART_COMMON_SRC_FILES :=
LIBART_GCC_ONLY_SRC_FILES :=
LIBART_TARGET_LDFLAGS :=
LIBART_HOST_LDFLAGS :=
LIBART_TARGET_SRC_FILES :=
LIBART_TARGET_SRC_FILES_arm :=
LIBART_TARGET_SRC_FILES_arm64 :=
LIBART_TARGET_SRC_FILES_x86 :=
LIBART_TARGET_SRC_FILES_x86_64 :=
LIBART_TARGET_SRC_FILES_mips :=
LIBART_HOST_SRC_FILES :=
LIBART_HOST_SRC_FILES_32 :=
LIBART_HOST_SRC_FILES_64 :=
LIBART_ENUM_OPERATOR_OUT_HEADER_FILES :=
LIBART_CFLAGS :=
build-libart :=
