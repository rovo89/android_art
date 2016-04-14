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

# Jar containing all the tests.
test_jack=${OUT_DIR-out}/host/common/obj/JAVA_LIBRARIES/apache-harmony-jdwp-tests-hostdex_intermediates/classes.jack

if [ ! -f $test_jack ]; then
  echo "Before running, you must build jdwp tests and vogar:" \
       "make apache-harmony-jdwp-tests-hostdex vogar"
  exit 1
fi

if [ "x$ART_USE_READ_BARRIER" = xtrue ]; then
  # For the moment, skip JDWP tests when read barriers are enabled, as
  # they sometimes exhibit a deadlock issue with the concurrent
  # copying collector in the read barrier configuration, between the
  # HeapTaskDeamon and the JDWP thread (b/25800335).
  #
  # TODO: Re-enable the JDWP tests when this deadlock issue is fixed.
  echo "JDWP tests are temporarily disabled in the read barrier configuration because of"
  echo "a deadlock issue (b/25800335)."
  exit 0
fi

art="/data/local/tmp/system/bin/art"
art_debugee="sh /data/local/tmp/system/bin/art"
args=$@
debuggee_args="-Xcompiler-option --debuggable"
device_dir="--device-dir=/data/local/tmp"
# We use the art script on target to ensure the runner and the debuggee share the same
# image.
vm_command="--vm-command=$art"
image_compiler_option=""
debug="no"
verbose="no"
image="-Ximage:/data/art-test/core-optimizing-pic.art"
vm_args=""
# By default, we run the whole JDWP test suite.
test="org.apache.harmony.jpda.tests.share.AllTests"
host="no"

while true; do
  if [[ "$1" == "--mode=host" ]]; then
    host="yes"
    # Specify bash explicitly since the art script cannot, since it has to run on the device
    # with mksh.
    art="bash ${OUT_DIR-out}/host/linux-x86/bin/art"
    art_debugee="bash ${OUT_DIR-out}/host/linux-x86/bin/art"
    # We force generation of a new image to avoid build-time and run-time classpath differences.
    image="-Ximage:/system/non/existent/vogar.art"
    # We do not need a device directory on host.
    device_dir=""
    # Vogar knows which VM to use on host.
    vm_command=""
    shift
  elif [[ $1 == -Ximage:* ]]; then
    image="$1"
    shift
  elif [[ $1 == "--debug" ]]; then
    debug="yes"
    # Remove the --debug from the arguments.
    args=${args/$1}
    shift
  elif [[ $1 == "--verbose" ]]; then
    verbose="yes"
    # Remove the --verbose from the arguments.
    args=${args/$1}
    shift
  elif [[ $1 == "--test" ]]; then
    # Remove the --test from the arguments.
    args=${args/$1}
    shift
    test=$1
    # Remove the test from the arguments.
    args=${args/$1}
    shift
  elif [[ "$1" == "" ]]; then
    break
  else
    shift
  fi
done

if [[ "$image" != "" ]]; then
  vm_args="--vm-arg $image"
fi
vm_args="$vm_args --vm-arg -Xusejit:true"
debuggee_args="$debuggee_args -Xusejit:true"
if [[ $debug == "yes" ]]; then
  art="$art -d"
  art_debugee="$art_debugee -d"
  vm_args="$vm_args --vm-arg -XXlib:libartd.so"
fi
if [[ $verbose == "yes" ]]; then
  # Enable JDWP logs in the debuggee.
  art_debugee="$art_debugee -verbose:jdwp"
fi

# Run the tests using vogar.
vogar $vm_command \
      $vm_args \
      --verbose \
      $args \
      $device_dir \
      $image_compiler_option \
      --timeout 800 \
      --vm-arg -Djpda.settings.verbose=true \
      --vm-arg -Djpda.settings.transportAddress=127.0.0.1:55107 \
      --vm-arg -Djpda.settings.debuggeeJavaPath="$art_debugee $image $debuggee_args" \
      --classpath $test_jack \
      --toolchain jack --language JN \
      --vm-arg -Xcompiler-option --vm-arg --debuggable \
      $test

vogar_exit_status=$?

echo "Killing stalled dalvikvm processes..."
if [[ $host == "yes" ]]; then
  pkill -9 -f /bin/dalvikvm
else
  adb shell pkill -9 -f /bin/dalvikvm
fi
echo "Done."

exit $vogar_exit_status
