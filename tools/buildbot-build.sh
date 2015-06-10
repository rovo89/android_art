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

if [ ! -d art ]; then
  echo "Script needs to be run at the root of the android tree"
  exit 1
fi

common_targets="vogar vogar.jar core-tests apache-harmony-jdwp-tests-hostdex out/host/linux-x86/bin/adb jsr166-tests conscrypt-tests"
android_root="/data/local/tmp/system"
linker="linker"
mode="target"
j_arg="-j$(nproc)"
make_command=

if [[ "$TARGET_PRODUCT" == "armv8" ]]; then
  linker="linker64"
fi

if [[ "$ART_TEST_ANDROID_ROOT" != "" ]]; then
  android_root="$ART_TEST_ANDROID_ROOT"
fi

while true; do
  if [[ "$1" == "--host" ]]; then
    mode="host"
    shift
  elif [[ "$1" == "--target" ]]; then
    mode="target"
    shift
  elif [[ "$1" == "--32" ]]; then
    linker="linker"
    shift
  elif [[ "$1" == "--64" ]]; then
    linker="linker64"
    shift
  elif [[ "$1" == "--android-root" ]]; then
    shift
    android_root=$1
    shift
  elif [[ "$1" == -j* ]]; then
    j_arg=$1
    shift
  elif [[ "$1" == "" ]]; then
    break
  fi
done

if [[ $mode == "host" ]]; then
  make_command="make $j_arg build-art-host-tests $common_targets"
  echo "Executing $make_command"
  $make_command
elif [[ $mode == "target" ]]; then
  # We need to provide our own linker in case the linker on the device
  # is out of date.
  env="TARGET_GLOBAL_LDFLAGS=-Wl,-dynamic-linker=$android_root/bin/$linker"
  # Use '-e' to force the override of TARGET_GLOBAL_LDFLAGS.
  # Also, we build extra tools that will be used by tests, so that
  # they are compiled with our own linker.
  make_command="make -e $j_arg build-art-target-tests $common_targets libjavacrypto linker toybox toolbox sh"
  echo "Executing env $env $make_command"
  env $env $make_command
fi

