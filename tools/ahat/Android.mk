#
# Copyright (C) 2015 The Android Open Source Project
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

include art/build/Android.common_test.mk

# --- ahat.jar ----------------
include $(CLEAR_VARS)
LOCAL_SRC_FILES := $(call all-java-files-under, src)
LOCAL_JAR_MANIFEST := src/manifest.txt
LOCAL_JAVA_RESOURCE_FILES := \
  $(LOCAL_PATH)/src/help.html \
  $(LOCAL_PATH)/src/style.css

LOCAL_STATIC_JAVA_LIBRARIES := perflib-prebuilt guavalib trove-prebuilt
LOCAL_IS_HOST_MODULE := true
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE := ahat
include $(BUILD_HOST_JAVA_LIBRARY)

# --- ahat script ----------------
include $(CLEAR_VARS)
LOCAL_IS_HOST_MODULE := true
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE := ahat
LOCAL_SRC_FILES := ahat
include $(BUILD_PREBUILT)

# --- ahat-tests.jar --------------
include $(CLEAR_VARS)
LOCAL_SRC_FILES := $(call all-java-files-under, test)
LOCAL_JAR_MANIFEST := test/manifest.txt
LOCAL_STATIC_JAVA_LIBRARIES := ahat junit
LOCAL_IS_HOST_MODULE := true
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE := ahat-tests
include $(BUILD_HOST_JAVA_LIBRARY)
AHAT_TEST_JAR := $(LOCAL_BUILT_MODULE)

# --- ahat-test-dump.jar --------------
include $(CLEAR_VARS)
LOCAL_MODULE := ahat-test-dump
LOCAL_MODULE_TAGS := tests
LOCAL_SRC_FILES := $(call all-java-files-under, test-dump)
include $(BUILD_HOST_DALVIK_JAVA_LIBRARY)

# Determine the location of the test-dump.jar and test-dump.hprof files.
# These use variables set implicitly by the include of
# BUILD_HOST_DALVIK_JAVA_LIBRARY above.
AHAT_TEST_DUMP_JAR := $(LOCAL_BUILT_MODULE)
AHAT_TEST_DUMP_HPROF := $(intermediates.COMMON)/test-dump.hprof

# Run ahat-test-dump.jar to generate test-dump.hprof
AHAT_TEST_DUMP_DEPENDENCIES := \
	$(ART_HOST_EXECUTABLES) \
	$(HOST_OUT_EXECUTABLES)/art \
	$(HOST_CORE_IMG_OUT_BASE)-optimizing-pic$(CORE_IMG_SUFFIX)

$(AHAT_TEST_DUMP_HPROF): PRIVATE_AHAT_TEST_ART := $(HOST_OUT_EXECUTABLES)/art
$(AHAT_TEST_DUMP_HPROF): PRIVATE_AHAT_TEST_DUMP_JAR := $(AHAT_TEST_DUMP_JAR)
$(AHAT_TEST_DUMP_HPROF): PRIVATE_AHAT_TEST_DUMP_DEPENDENCIES := $(AHAT_TEST_DUMP_DEPENDENCIES)
$(AHAT_TEST_DUMP_HPROF): $(AHAT_TEST_DUMP_JAR) $(AHAT_TEST_DUMP_DEPENDENCIES)
	$(PRIVATE_AHAT_TEST_ART) -cp $(PRIVATE_AHAT_TEST_DUMP_JAR) Main $@

.PHONY: ahat-test
ahat-test: PRIVATE_AHAT_TEST_DUMP_HPROF := $(AHAT_TEST_DUMP_HPROF)
ahat-test: PRIVATE_AHAT_TEST_JAR := $(AHAT_TEST_JAR)
ahat-test: $(AHAT_TEST_JAR) $(AHAT_TEST_DUMP_HPROF)
	java -Dahat.test.dump.hprof=$(PRIVATE_AHAT_TEST_DUMP_HPROF) -jar $(PRIVATE_AHAT_TEST_JAR)

# Clean up local variables.
AHAT_TEST_DUMP_DEPENDENCIES :=
AHAT_TEST_DUMP_HPROF :=
AHAT_TEST_DUMP_JAR :=
AHAT_TEST_JAR :=

