#
# Copyright (C) 2016 The Android Open Source Project
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

include art/build/Android.executable.mk

PROFMAN_SRC_FILES := \
	profman.cc \
	profile_assistant.cc

# TODO: Remove this when the framework (installd) supports pushing the
# right instruction-set parameter for the primary architecture.
ifneq ($(filter ro.zygote=zygote64,$(PRODUCT_DEFAULT_PROPERTY_OVERRIDES)),)
  profman_arch := 64
else
  profman_arch := 32
endif

ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  $(eval $(call build-art-executable,profman,$(PROFMAN_SRC_FILES),libcutils,art/profman,target,ndebug,$(profman_arch)))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-art-executable,profman,$(PROFMAN_SRC_FILES),libcutils,art/profman,target,debug,$(profman_arch)))
endif

ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-art-executable,profman,$(PROFMAN_SRC_FILES),libcutils,art/profman,host,ndebug))
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-art-executable,profman,$(PROFMAN_SRC_FILES),libcutils,art/profman,host,debug))
endif
