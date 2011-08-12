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

ART_C_INCLUDES := external/gtest/include external/zlib

ART_CFLAGS := \
	-O0 \
	-ggdb3 \
	-Wall \
	-Werror \
	-Wextra \
	-Wno-unused-parameter \
	-Wstrict-aliasing=2 \
	-fno-align-jumps \
	-fstrict-aliasing

AEXEC_SRC_FILES := \
	src/main.cc

LIBART_COMMON_SRC_FILES := \
	src/assembler.cc \
	src/calling_convention.cc \
	src/class_linker.cc \
	src/dex_cache.cc \
	src/dex_file.cc \
	src/dex_instruction.cc \
	src/dex_verifier.cc \
	src/heap.cc \
	src/intern_table.cc \
	src/jni_compiler.cc \
	src/jni_internal.cc \
	src/mark_stack.cc \
	src/mark_sweep.cc \
	src/memory_region.cc \
	src/mspace.c \
	src/object.cc \
	src/object_bitmap.cc \
	src/offsets.cc \
	src/runtime.cc \
	src/space.cc \
	src/stringpiece.cc \
	src/stringprintf.cc \
	src/thread.cc \
	src/zip_archive.cc

LIBART_TARGET_SRC_FILES := \
	$(LIBART_COMMON_SRC_FILES) \
	src/assembler_arm.cc \
	src/calling_convention_arm.cc \
	src/logging_android.cc \
	src/managed_register_arm.cc \
	src/runtime_android.cc \
	src/thread_arm.cc

LIBART_HOST_SRC_FILES := \
	$(LIBART_COMMON_SRC_FILES) \
	src/assembler_x86.cc \
	src/calling_convention_x86.cc \
	src/logging_linux.cc \
	src/managed_register_x86.cc \
	src/runtime_linux.cc \
	src/thread_x86.cc

LIBARTTEST_COMMON_SRC_FILES := \
	src/base64.cc

TEST_COMMON_SRC_FILES := \
	src/class_linker_test.cc \
	src/dex_cache_test.cc \
	src/dex_file_test.cc \
	src/dex_instruction_visitor_test.cc \
	src/exception_test.cc \
	src/intern_table_test.cc \
	src/jni_compiler_test.cc.arm \
	src/jni_internal_test.cc \
	src/object_test.cc \
	src/runtime_test.cc \
	src/space_test.cc \
	src/zip_archive_test.cc

TEST_TARGET_SRC_FILES := \
	$(TEST_COMMON_SRC_FILES) \
	src/managed_register_arm_test.cc

TEST_HOST_SRC_FILES := \
	$(TEST_COMMON_SRC_FILES) \
	src/assembler_x86_test.cc \
	src/managed_register_x86_test.cc
