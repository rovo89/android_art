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

if [ ! -d libcore ]; then
  echo "Script needs to be run at the root of the android tree"
  exit 1
fi

if [ $ANDROID_SERIAL == 03a79ae90ae5889b ] || [ $ANDROID_SERIAL == HT4CTJT03670 ] || [ $ANDROID_SERIAL == HT49CJT00070 ]; then
  echo "Not run because of localhost failures. Investigating."
  exit 0
fi

# Jar containing all the tests.
test_jar=out/host/linux-x86/framework/apache-harmony-jdwp-tests-hostdex.jar
junit_jar=out/host/linux-x86/framework/junit.jar

if [ ! -f $test_jar -o ! -f $junit_jar ]; then
  echo "Before running, you must build jdwp tests and vogar:" \
       "make junit apache-harmony-jdwp-tests-hostdex vogar vogar.jar"
  exit 1
fi

art="/data/local/tmp/system/bin/art"
# We use Quick's image on target because optimizing's image is not compiled debuggable.
image="-Ximage:/data/art-test/core.art"
args=$@
debuggee_args="-Xcompiler-option --compiler-backend=Optimizing -Xcompiler-option --debuggable"
device_dir="--device-dir=/data/local/tmp"

while true; do
  if [[ "$1" == "--mode=host" ]]; then
    art="art"
    # We force generation of a new image to avoid build-time and run-time classpath differences.
    image="-Ximage:/system/non/existent"
    device_dir=""
    shift
  elif [[ $1 == -Ximage:* ]]; then
    image="$1"
    shift
  elif [[ "$1" == "" ]]; then
    break
  else
    shift
  fi
done

# Run the tests using vogar.
vogar --vm-command=$art \
      --vm-arg $image \
      --verbose \
      $args \
      $device_dir \
      --vm-arg -Ximage-compiler-option \
      --vm-arg --debuggable \
      --timeout 600 \
      --vm-arg -Djpda.settings.verbose=true \
      --vm-arg -Djpda.settings.syncPort=34016 \
      --vm-arg -Djpda.settings.debuggeeJavaPath="$art $image $debuggee_args" \
      --classpath $test_jar \
      --classpath $junit_jar \
      --vm-arg -Xcompiler-option --vm-arg --compiler-backend=Optimizing \
      --vm-arg -Xcompiler-option --vm-arg --debuggable \
      org.apache.harmony.jpda.tests.share.AllTests
