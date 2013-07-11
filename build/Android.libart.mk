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

LIBART_COMMON_SRC_FILES := \
	src/atomic.cc.arm \
	src/barrier.cc \
	src/base/logging.cc \
	src/base/mutex.cc \
	src/base/stringpiece.cc \
	src/base/stringprintf.cc \
	src/base/timing_logger.cc \
	src/base/unix_file/fd_file.cc \
	src/base/unix_file/mapped_file.cc \
	src/base/unix_file/null_file.cc \
	src/base/unix_file/random_access_file_utils.cc \
	src/base/unix_file/string_file.cc \
	src/check_jni.cc \
	src/class_linker.cc \
	src/common_throws.cc \
	src/compiled_method.cc \
	src/debugger.cc \
	src/dex_file.cc \
	src/dex_file_verifier.cc \
	src/dex_instruction.cc \
	src/disassembler.cc \
	src/disassembler_arm.cc \
	src/disassembler_mips.cc \
	src/disassembler_x86.cc \
	src/elf_file.cc \
	src/file_output_stream.cc \
	src/gc/allocator/dlmalloc.cc \
	src/gc/accounting/card_table.cc \
	src/gc/accounting/heap_bitmap.cc \
	src/gc/accounting/mod_union_table.cc \
	src/gc/accounting/space_bitmap.cc \
	src/gc/collector/garbage_collector.cc \
	src/gc/collector/mark_sweep.cc \
	src/gc/collector/partial_mark_sweep.cc \
	src/gc/collector/sticky_mark_sweep.cc \
	src/gc/heap.cc \
	src/gc/space/dlmalloc_space.cc \
	src/gc/space/image_space.cc \
	src/gc/space/large_object_space.cc \
	src/gc/space/space.cc \
	src/hprof/hprof.cc \
	src/image.cc \
	src/indirect_reference_table.cc \
	src/instrumentation.cc \
	src/intern_table.cc \
	src/interpreter/interpreter.cc \
	src/jdwp/jdwp_event.cc \
	src/jdwp/jdwp_expand_buf.cc \
	src/jdwp/jdwp_handler.cc \
	src/jdwp/jdwp_main.cc \
	src/jdwp/jdwp_request.cc \
	src/jdwp/jdwp_socket.cc \
	src/jdwp/object_registry.cc \
	src/jni_internal.cc \
	src/jobject_comparator.cc \
	src/locks.cc \
	src/mem_map.cc \
	src/memory_region.cc \
	src/mirror/abstract_method.cc \
	src/mirror/array.cc \
	src/mirror/class.cc \
	src/mirror/dex_cache.cc \
	src/mirror/field.cc \
	src/mirror/object.cc \
	src/mirror/stack_trace_element.cc \
	src/mirror/string.cc \
	src/mirror/throwable.cc \
	src/monitor.cc \
	src/native/dalvik_system_DexFile.cc \
	src/native/dalvik_system_VMDebug.cc \
	src/native/dalvik_system_VMRuntime.cc \
	src/native/dalvik_system_VMStack.cc \
	src/native/dalvik_system_Zygote.cc \
	src/native/java_lang_Class.cc \
	src/native/java_lang_Object.cc \
	src/native/java_lang_Runtime.cc \
	src/native/java_lang_String.cc \
	src/native/java_lang_System.cc \
	src/native/java_lang_Thread.cc \
	src/native/java_lang_Throwable.cc \
	src/native/java_lang_VMClassLoader.cc \
	src/native/java_lang_reflect_Array.cc \
	src/native/java_lang_reflect_Constructor.cc \
	src/native/java_lang_reflect_Field.cc \
	src/native/java_lang_reflect_Method.cc \
	src/native/java_lang_reflect_Proxy.cc \
	src/native/java_util_concurrent_atomic_AtomicLong.cc \
	src/native/org_apache_harmony_dalvik_ddmc_DdmServer.cc \
	src/native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.cc \
	src/native/sun_misc_Unsafe.cc \
	src/oat.cc \
	src/oat/utils/arm/assembler_arm.cc \
	src/oat/utils/arm/managed_register_arm.cc \
	src/oat/utils/assembler.cc \
	src/oat/utils/mips/assembler_mips.cc \
	src/oat/utils/mips/managed_register_mips.cc \
	src/oat/utils/x86/assembler_x86.cc \
	src/oat/utils/x86/managed_register_x86.cc \
	src/oat_file.cc \
	src/offsets.cc \
	src/os_linux.cc \
	src/primitive.cc \
	src/reference_table.cc \
	src/reflection.cc \
	src/runtime.cc \
	src/runtime_support.cc \
	src/runtime_support_llvm.cc \
	src/signal_catcher.cc \
	src/stack.cc \
	src/thread.cc \
	src/thread_list.cc \
	src/thread_pool.cc \
	src/throw_location.cc \
	src/trace.cc \
	src/utf.cc \
	src/utils.cc \
	src/vector_output_stream.cc \
	src/verifier/dex_gc_map.cc \
	src/verifier/instruction_flags.cc \
	src/verifier/method_verifier.cc \
	src/verifier/reg_type.cc \
	src/verifier/reg_type_cache.cc \
	src/verifier/register_line.cc \
	src/well_known_classes.cc \
	src/zip_archive.cc

LIBART_COMMON_SRC_FILES += \
	src/oat/runtime/context.cc \
	src/oat/runtime/support_alloc.cc \
	src/oat/runtime/support_cast.cc \
	src/oat/runtime/support_deoptimize.cc \
	src/oat/runtime/support_dexcache.cc \
	src/oat/runtime/support_field.cc \
	src/oat/runtime/support_fillarray.cc \
	src/oat/runtime/support_instrumentation.cc \
	src/oat/runtime/support_invoke.cc \
	src/oat/runtime/support_jni.cc \
	src/oat/runtime/support_locks.cc \
	src/oat/runtime/support_math.cc \
	src/oat/runtime/support_proxy.cc \
	src/oat/runtime/support_stubs.cc \
	src/oat/runtime/support_thread.cc \
	src/oat/runtime/support_throw.cc \
	src/oat/runtime/support_interpreter.cc

ifeq ($(ART_SEA_IR_MODE),true)
LIBART_COMMON_SRC_FILES += \
	src/compiler/sea_ir/sea.cc
endif

LIBART_TARGET_SRC_FILES := \
	$(LIBART_COMMON_SRC_FILES) \
	src/base/logging_android.cc \
	src/jdwp/jdwp_adb.cc \
	src/monitor_android.cc \
	src/runtime_android.cc \
	src/thread_android.cc

ifeq ($(TARGET_ARCH),arm)
LIBART_TARGET_SRC_FILES += \
	src/oat/runtime/arm/context_arm.cc.arm \
	src/oat/runtime/arm/oat_support_entrypoints_arm.cc \
	src/oat/runtime/arm/runtime_support_arm.S
else # TARGET_ARCH != arm
ifeq ($(TARGET_ARCH),x86)
LIBART_TARGET_SRC_FILES += \
	src/oat/runtime/x86/context_x86.cc \
	src/oat/runtime/x86/oat_support_entrypoints_x86.cc \
	src/oat/runtime/x86/runtime_support_x86.S
else # TARGET_ARCH != x86
ifeq ($(TARGET_ARCH),mips)
LIBART_TARGET_SRC_FILES += \
	src/oat/runtime/mips/context_mips.cc \
	src/oat/runtime/mips/oat_support_entrypoints_mips.cc \
	src/oat/runtime/mips/runtime_support_mips.S
else # TARGET_ARCH != mips
$(error unsupported TARGET_ARCH=$(TARGET_ARCH))
endif # TARGET_ARCH != mips
endif # TARGET_ARCH != x86
endif # TARGET_ARCH != arm

ifeq ($(TARGET_ARCH),arm)
LIBART_TARGET_SRC_FILES += src/thread_arm.cc
else # TARGET_ARCH != arm
ifeq ($(TARGET_ARCH),x86)
LIBART_TARGET_SRC_FILES += src/thread_x86.cc
else # TARGET_ARCH != x86
ifeq ($(TARGET_ARCH),mips)
LIBART_TARGET_SRC_FILES += src/thread_mips.cc
else # TARGET_ARCH != mips
$(error unsupported TARGET_ARCH=$(TARGET_ARCH))
endif # TARGET_ARCH != mips
endif # TARGET_ARCH != x86
endif # TARGET_ARCH != arm

LIBART_HOST_SRC_FILES := \
	$(LIBART_COMMON_SRC_FILES) \
	src/base/logging_linux.cc \
	src/monitor_linux.cc \
	src/runtime_linux.cc \
	src/thread_linux.cc

ifeq ($(HOST_ARCH),x86)
LIBART_HOST_SRC_FILES += \
	src/oat/runtime/x86/context_x86.cc \
	src/oat/runtime/x86/oat_support_entrypoints_x86.cc \
	src/oat/runtime/x86/runtime_support_x86.S
else # HOST_ARCH != x86
$(error unsupported HOST_ARCH=$(HOST_ARCH))
endif # HOST_ARCH != x86

ifeq ($(HOST_ARCH),x86)
LIBART_HOST_SRC_FILES += src/thread_x86.cc
else # HOST_ARCH != x86
$(error unsupported HOST_ARCH=$(HOST_ARCH))
endif # HOST_ARCH != x86


LIBART_ENUM_OPERATOR_OUT_HEADER_FILES := \
	src/base/mutex.h \
	src/compiler/dex/compiler_enums.h \
	src/dex_file.h \
	src/dex_instruction.h \
	src/gc/collector/gc_type.h \
	src/gc/space/space.h \
	src/gc/heap.h \
	src/indirect_reference_table.h \
	src/instruction_set.h \
	src/invoke_type.h \
	src/jdwp/jdwp.h \
	src/jdwp/jdwp_constants.h \
	src/locks.h \
	src/mirror/class.h \
	src/thread.h \
	src/thread_state.h \
	src/verifier/method_verifier.h

LIBART_CFLAGS :=
ifeq ($(ART_USE_PORTABLE_COMPILER),true)
  LIBART_CFLAGS += -DART_USE_PORTABLE_COMPILER=1
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

  include $(CLEAR_VARS)
  ifeq ($$(art_target_or_host),target)
    include external/stlport/libstlport.mk
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := libart
  else # debug
    LOCAL_MODULE := libartd
  endif

  LOCAL_MODULE_TAGS := optional
  LOCAL_MODULE_CLASS := SHARED_LIBRARIES

  ifeq ($$(art_target_or_host),target)
    LOCAL_SRC_FILES := $(LIBART_TARGET_SRC_FILES)
  else # host
    LOCAL_SRC_FILES := $(LIBART_HOST_SRC_FILES)
    LOCAL_IS_HOST_MODULE := true
  endif

  GENERATED_SRC_DIR := $$(call intermediates-dir-for,$$(LOCAL_MODULE_CLASS),$$(LOCAL_MODULE),$$(LOCAL_IS_HOST_MODULE),)
  ENUM_OPERATOR_OUT_CC_FILES := $$(patsubst %.h,%_operator_out.cc,$$(LIBART_ENUM_OPERATOR_OUT_HEADER_FILES))
  ENUM_OPERATOR_OUT_GEN := $$(addprefix $$(GENERATED_SRC_DIR)/,$$(ENUM_OPERATOR_OUT_CC_FILES))

$$(ENUM_OPERATOR_OUT_GEN): art/tools/generate-operator-out.py
$$(ENUM_OPERATOR_OUT_GEN): PRIVATE_CUSTOM_TOOL = art/tools/generate-operator-out.py $$< > $$@
$$(ENUM_OPERATOR_OUT_GEN): $$(GENERATED_SRC_DIR)/%_operator_out.cc : art/%.h
	$$(transform-generated-source)

  LOCAL_GENERATED_SOURCES += $$(ENUM_OPERATOR_OUT_GEN)

  LOCAL_CFLAGS := $(LIBART_CFLAGS)
  ifeq ($$(art_target_or_host),target)
    LOCAL_CLANG := $(ART_TARGET_CLANG)
    LOCAL_CFLAGS += $(ART_TARGET_CFLAGS)
  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS)
  endif
  ifeq ($$(art_ndebug_or_debug),debug)
    ifeq ($$(art_target_or_host),target)
      LOCAL_CFLAGS += $(ART_TARGET_DEBUG_CFLAGS)
    else # host
      LOCAL_CFLAGS += $(ART_HOST_DEBUG_CFLAGS)
      LOCAL_LDLIBS += $(ART_HOST_DEBUG_LDLIBS)
      LOCAL_STATIC_LIBRARIES := libgtest_host
    endif
  else
    ifeq ($$(art_target_or_host),target)
      LOCAL_CFLAGS += $(ART_TARGET_NON_DEBUG_CFLAGS)
    else # host
      LOCAL_CFLAGS += $(ART_HOST_NON_DEBUG_CFLAGS)
    endif
  endif
  LOCAL_C_INCLUDES += $(ART_C_INCLUDES)
  LOCAL_SHARED_LIBRARIES := liblog libnativehelper
  LOCAL_SHARED_LIBRARIES += libcorkscrew # native stack trace support
  ifeq ($$(art_target_or_host),target)
    LOCAL_SHARED_LIBRARIES += libcutils libstlport libz libdl libselinux
  else # host
    LOCAL_STATIC_LIBRARIES += libcutils
    LOCAL_SHARED_LIBRARIES += libz-host
    LOCAL_LDLIBS += -ldl -lpthread
    ifeq ($(HOST_OS),linux)
      LOCAL_LDLIBS += -lrt
    endif
  endif
  include $(LLVM_GEN_INTRINSICS_MK)
  LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/build/Android.common.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/build/Android.libart.mk
  ifeq ($$(art_target_or_host),target)
    include $(LLVM_DEVICE_BUILD_MK)
    include $(BUILD_SHARED_LIBRARY)
  else # host
    include $(LLVM_HOST_BUILD_MK)
    include $(BUILD_HOST_SHARED_LIBRARY)
  endif
endef

ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  $(eval $(call build-libart,target,ndebug))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-libart,target,debug))
endif

# We always build dex2oat and dependencies, even if the host build is otherwise disabled, since they are used to cross compile for the target.
ifeq ($(ART_BUILD_NDEBUG),true)
  $(eval $(call build-libart,host,ndebug))
endif
ifeq ($(ART_BUILD_DEBUG),true)
  $(eval $(call build-libart,host,debug))
endif
