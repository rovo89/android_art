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
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE := ahat
include $(BUILD_SYSTEM)/base_rules.mk
$(LOCAL_BUILT_MODULE): $(LOCAL_PATH)/ahat $(ACP)
	@echo "Copy: $(PRIVATE_MODULE) ($@)"
	$(copy-file-to-new-target)
	$(hide) chmod 755 $@

ahat: $(LOCAL_BUILT_MODULE)

# --- ahat-test.jar --------------
include $(CLEAR_VARS)
LOCAL_SRC_FILES := $(call all-java-files-under, test)
LOCAL_JAR_MANIFEST := test/manifest.txt
LOCAL_STATIC_JAVA_LIBRARIES := ahat junit
LOCAL_IS_HOST_MODULE := true
LOCAL_MODULE_TAGS := tests
LOCAL_MODULE := ahat-tests
include $(BUILD_HOST_JAVA_LIBRARY)

ahat-test: $(LOCAL_BUILT_MODULE)
	java -jar $<
