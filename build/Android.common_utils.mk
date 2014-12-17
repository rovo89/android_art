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

ifndef ART_ANDROID_COMMON_UTILS_MK
ART_ANDROID_COMMON_UTILS_MK = true

#
# Convert a string into an uppercase string.
#
# $(1): a string which should be made uppercase
art-string-to-uppercase = $(shell echo $(1) | tr '[:lower:]' '[:upper:]')

endif # ART_ANDROID_COMMON_UTILS_MK
