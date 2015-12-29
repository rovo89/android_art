#!/bin/bash
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

if [ ! -d libcore ]; then
  echo "Script needs to be run at the root of the android tree"
  exit 1
fi

# Jar containing jsr166 tests.
jsr166_test_jar=${OUT_DIR-out}/target/common/obj/JAVA_LIBRARIES/jsr166-tests_intermediates/javalib.jar

# Jar containing all the other tests.
test_jar=${OUT_DIR-out}/target/common/obj/JAVA_LIBRARIES/core-tests_intermediates/javalib.jar


if [ ! -f $test_jar ]; then
  echo "Before running, you must build core-tests, jsr166-tests and vogar: \
        make core-tests jsr166-tests vogar vogar.jar"
  exit 1
fi

emulator="no"
if [ "$ANDROID_SERIAL" = "emulator-5554" ]; then
  emulator="yes"
fi

# Packages that currently work correctly with the expectation files.
working_packages=("dalvik.system"
                  "libcore.icu"
                  "libcore.io"
                  "libcore.java.lang"
                  "libcore.java.math"
                  "libcore.java.text"
                  "libcore.java.util"
                  "libcore.javax.crypto"
                  "libcore.javax.security"
                  "libcore.javax.sql"
                  "libcore.javax.xml"
                  "libcore.net"
                  "libcore.reflect"
                  "libcore.util"
                  "org.apache.harmony.annotation"
                  "org.apache.harmony.crypto"
                  "org.apache.harmony.luni"
                  "org.apache.harmony.nio"
                  "org.apache.harmony.regex"
                  "org.apache.harmony.testframework"
                  "org.apache.harmony.tests.java.io"
                  "org.apache.harmony.tests.java.lang"
                  "org.apache.harmony.tests.java.math"
                  "org.apache.harmony.tests.java.util"
                  "org.apache.harmony.tests.java.text"
                  "org.apache.harmony.tests.javax.security"
                  "tests.java.lang.String"
                  "jsr166")

# List of packages we could run, but don't have rights to revert
# changes in case of failures.
# "org.apache.harmony.security"

vogar_args=$@
while true; do
  if [[ "$1" == "--mode=device" ]]; then
    vogar_args="$vogar_args --device-dir=/data/local/tmp"
    vogar_args="$vogar_args --vm-command=/data/local/tmp/system/bin/art"
    vogar_args="$vogar_args --vm-arg -Ximage:/data/art-test/core-optimizing.art"
    shift
  elif [[ "$1" == "--mode=host" ]]; then
    # We explicitly give a wrong path for the image, to ensure vogar
    # will create a boot image with the default compiler. Note that
    # giving an existing image on host does not work because of
    # classpath/resources differences when compiling the boot image.
    vogar_args="$vogar_args --vm-arg -Ximage:/non/existent/vogar.art"
    shift
  elif [[ "$1" == "--debug" ]]; then
    # Remove the --debug from the arguments.
    vogar_args=${vogar_args/$1}
    vogar_args="$vogar_args --vm-arg -XXlib:libartd.so"
    shift
  elif [[ "$1" == "" ]]; then
    break
  else
    shift
  fi
done

# Increase the timeout, as vogar cannot set individual test
# timeout when being asked to run packages, and some tests go above
# the default timeout.
vogar_args="$vogar_args --timeout 480"

# Run the tests using vogar.
echo "Running tests for the following test packages:"
echo ${working_packages[@]} | tr " " "\n"
vogar $vogar_args --vm-arg -Xusejit:true --expectations art/tools/libcore_failures.txt --classpath $jsr166_test_jar --classpath $test_jar ${working_packages[@]}
