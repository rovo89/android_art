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

out_dir=${OUT_DIR-out}
java_libraries_dir=${out_dir}/target/common/obj/JAVA_LIBRARIES
common_targets="vogar ${java_libraries_dir}/core-tests_intermediates/javalib.jar apache-harmony-jdwp-tests-hostdex ${java_libraries_dir}/jsr166-tests_intermediates/javalib.jar ${out_dir}/host/linux-x86/bin/jack"
mode="target"
j_arg="-j$(nproc)"
showcommands=
make_command=

while true; do
  if [[ "$1" == "--host" ]]; then
    mode="host"
    shift
  elif [[ "$1" == "--target" ]]; then
    mode="target"
    shift
  elif [[ "$1" == -j* ]]; then
    j_arg=$1
    shift
  elif [[ "$1" == "--showcommands" ]]; then
    showcommands="showcommands"
    shift
  elif [[ "$1" == "" ]]; then
    break
  fi
done

# Workaround for b/26051370.
#
# system/core/base/include/base has been renamed to
# system/core/base/include/android-base, but the master-art manifest
# is pinned to an older revision for system/core; create this symlink
# by hand to fix include paths.
test -e system/core/base/include/android-base || ln -s base system/core/base/include/android-base

if [[ $mode == "host" ]]; then
  make_command="make $j_arg $showcommands build-art-host-tests $common_targets ${out_dir}/host/linux-x86/lib/libjavacoretests.so ${out_dir}/host/linux-x86/lib64/libjavacoretests.so"
elif [[ $mode == "target" ]]; then
  make_command="make $j_arg $showcommands build-art-target-tests $common_targets libjavacrypto libjavacoretests linker toybox toolbox sh ${out_dir}/host/linux-x86/bin/adb"
fi

echo "Executing $make_command"
$make_command
