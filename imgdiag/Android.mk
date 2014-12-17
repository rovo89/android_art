#
# Copyright (C) 2014 The Android Open Source Project
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

IMGDIAG_SRC_FILES := \
	imgdiag.cc

# Note that this tool needs to be built for both 32-bit and 64-bit since it requires
# that the image it's analyzing be the same ISA as the runtime ISA.

# Build variants {target,host} x {debug,ndebug} x {32,64}
$(eval $(call build-art-multi-executable,imgdiag,$(IMGDIAG_SRC_FILES),libart-compiler libbacktrace,libcutils,libziparchive-host,art/compiler,both))
