#!/bin/bash
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

green='\033[0;32m'
nc='\033[0m'

echo -e "${green}Date on device${nc}"
adb shell date

echo -e "${green}Turn off selinux${nc}"
adb shell setenforce 0
adb shell getenforce

echo -e "${green}Setting local loopback${nc}"
adb shell ifconfig lo up
adb shell ifconfig

echo -e "${green}List properties${nc}"
adb shell getprop
