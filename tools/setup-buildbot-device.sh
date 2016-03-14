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

echo -e "${green}Uptime${nc}"
adb shell uptime

echo -e "${green}Battery info${nc}"
adb shell dumpsys battery

echo -e "${green}Setting adb buffer size to 32MB${nc}"
adb logcat -G 32M
adb logcat -g

echo -e "${green}Removing adb spam filter${nc}"
adb logcat -P ""
adb logcat -p

echo -e "${green}Kill stalled dalvikvm processes${nc}"
# 'ps' on M can sometimes hang.
timeout 2s adb shell "ps"
if [ $? = 124 ]; then
  echo -e "${green}Rebooting device to fix 'ps'${nc}"
  adb reboot
  adb wait-for-device root
else
  processes=$(adb shell "ps" | grep dalvikvm | awk '{print $2}')
  for i in $processes; do adb shell kill -9 $i; done
fi
