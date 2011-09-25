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

ART_CPP_EXTENSION := .cc

ART_C_INCLUDES := \
	external/gtest/include \
	external/icu4c/common \
	external/icu4c/i18n \
	external/valgrind/dynamic_annotations \
	external/zlib \
	art/src \
	dalvik/libdex

art_cflags := \
	-O2 \
	-ggdb3 \
	-Wall \
	-Werror \
	-Wextra \
	-Wno-unused-parameter \
	-Wstrict-aliasing=2 \
	-fno-align-jumps \
	-fstrict-aliasing

ART_HOST_CFLAGS := $(art_cflags) -DANDROID_SMP=1

ART_TARGET_CFLAGS := $(art_cflags)
ifeq ($(TARGET_CPU_SMP),true)
  ART_TARGET_CFLAGS += -DANDROID_SMP=1
else
  ART_TARGET_CFLAGS += -DANDROID_SMP=0
endif

art_debug_cflags := -UNDEBUG
# TODO: move -fkeep-inline-functions to art_debug_cflags when target gcc > 4.4
ART_HOST_DEBUG_CFLAGS := $(art_debug_cflags) -fkeep-inline-functions
ART_TARGET_DEBUG_CFLAGS := $(art_debug_cflags)

DEX2OAT_SRC_FILES := \
	src/dex2oat.cc

OATDUMP_SRC_FILES := \
	src/oatdump.cc

OATEXEC_SRC_FILES := \
	src/oatexec.cc

LIBART_COMMON_SRC_FILES := \
	src/assembler.cc \
	src/assembler_arm.cc \
	src/assembler_x86.cc \
	src/atomic.cc.arm \
	src/calling_convention.cc \
	src/calling_convention_arm.cc \
	src/calling_convention_x86.cc \
	src/context.cc \
	src/context_arm.cc.arm \
	src/context_x86.cc \
	src/check_jni.cc \
	src/class_linker.cc \
	src/class_loader.cc \
	src/compiler.cc \
	src/compiler/Dataflow.cc \
	src/compiler/Frontend.cc \
	src/compiler/IntermediateRep.cc \
	src/compiler/Ralloc.cc \
	src/compiler/SSATransformation.cc \
	src/compiler/Utility.cc \
	src/compiler/codegen/RallocUtil.cc \
	src/compiler/codegen/arm/ArchUtility.cc \
	src/compiler/codegen/arm/ArmRallocUtil.cc \
	src/compiler/codegen/arm/Assemble.cc \
	src/compiler/codegen/arm/LocalOptimizations.cc \
	src/compiler/codegen/arm/armv7-a/Codegen.cc \
	src/dalvik_system_DexFile.cc \
	src/dalvik_system_VMDebug.cc \
	src/dalvik_system_VMRuntime.cc \
	src/dalvik_system_VMStack.cc \
	src/dalvik_system_Zygote.cc \
	src/dex_cache.cc \
	src/dex_file.cc \
	src/dex_instruction.cc \
	src/dex_verifier.cc \
	src/file.cc \
	src/file_linux.cc \
	src/heap.cc \
	src/image.cc \
	src/image_writer.cc \
	src/indirect_reference_table.cc \
	src/intern_table.cc \
	src/java_lang_Class.cc \
	src/java_lang_Object.cc \
	src/java_lang_Runtime.cc \
	src/java_lang_String.cc \
	src/java_lang_System.cc \
	src/java_lang_Thread.cc \
	src/java_lang_Throwable.cc \
	src/java_lang_VMClassLoader.cc \
	src/java_lang_reflect_Array.cc \
	src/java_lang_reflect_Constructor.cc \
	src/java_lang_reflect_Field.cc \
	src/java_lang_reflect_Method.cc \
	src/java_util_concurrent_atomic_AtomicLong.cc \
	src/jni_compiler.cc \
	src/jni_internal.cc \
	src/jni_internal_arm.cc \
	src/jni_internal_x86.cc \
	src/logging.cc \
	src/mark_stack.cc \
	src/mark_sweep.cc \
	src/managed_register_arm.cc \
	src/managed_register_x86.cc \
	src/mem_map.cc \
	src/memory_region.cc \
	src/monitor.cc \
	src/mspace.c \
	src/mutex.cc \
	src/object.cc \
	src/object_bitmap.cc \
	src/offsets.cc \
	src/org_apache_harmony_dalvik_ddmc_DdmServer.cc \
	src/os_linux.cc \
	src/reference_table.cc \
	src/reflection.cc \
	src/runtime.cc \
	src/signal_catcher.cc \
	src/space.cc \
	src/stringpiece.cc \
	src/stringprintf.cc \
	src/stub_arm.cc \
	src/stub_x86.cc \
	src/sun_misc_Unsafe.cc \
	src/thread.cc \
	src/thread_list.cc \
	src/utf.cc \
	src/utils.cc \
	src/zip_archive.cc \
	src/runtime_support.S

LIBART_TARGET_SRC_FILES := \
	$(LIBART_COMMON_SRC_FILES) \
	src/logging_android.cc \
	src/runtime_android.cc \
	src/thread_android.cc \
	src/thread_arm.cc

LIBART_HOST_SRC_FILES := \
	$(LIBART_COMMON_SRC_FILES) \
	src/logging_linux.cc \
	src/runtime_linux.cc \
	src/thread_linux.cc \
	src/thread_x86.cc

LIBARTTEST_COMMON_SRC_FILES := \
	src/base64.cc \
	src/jni_tests.cc \
	src/stack_walk.cc

TEST_COMMON_SRC_FILES := \
	src/class_linker_test.cc \
	src/dex_cache_test.cc \
	src/dex_file_test.cc \
	src/dex_instruction_visitor_test.cc \
	src/dex_verifier_test.cc \
	src/exception_test.cc \
	src/file_test.cc \
	src/heap_test.cc \
	src/image_test.cc \
	src/indirect_reference_table_test.cc \
	src/intern_table_test.cc \
	src/jni_internal_test.cc \
	src/jni_compiler_test.cc \
	src/managed_register_arm_test.cc \
	src/managed_register_x86_test.cc \
	src/object_test.cc \
	src/reference_table_test.cc \
	src/runtime_test.cc \
	src/space_test.cc \
	src/utils_test.cc \
	src/zip_archive_test.cc \
	src/compiler_test.cc

TEST_TARGET_SRC_FILES := \
	$(TEST_COMMON_SRC_FILES)

TEST_HOST_SRC_FILES := \
	$(TEST_COMMON_SRC_FILES) \
	src/assembler_x86_test.cc

# subdirectories of test/
TEST_DEX_DIRECTORIES := \
	AbstractMethod \
	AllFields \
	CreateMethodDescriptor \
	ExceptionTest \
	Fibonacci \
	HelloWorld \
	IntMath \
	Interfaces \
	Main \
	MemUsage \
	MyClass \
	MyClassNatives \
	Nested \
	ProtoCompare \
	ProtoCompare2 \
	StackWalk \
	StackWalk2 \
	StaticLeafMethods \
	Statics \
	SystemMethods \
	Invoke \
	XandY

ART_BUILD_TARGET := false
ART_BUILD_HOST := false
ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  ART_BUILD_TARGET := true
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  ART_BUILD_TARGET := true
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  ART_BUILD_HOST := true
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  ART_BUILD_HOST := true
endif
