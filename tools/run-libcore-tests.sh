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

# Jar containing all the tests.
test_jar=out/target/common/obj/JAVA_LIBRARIES/core-tests_intermediates/javalib.jar

if [ ! -f $test_jar ]; then
  echo "Before running, you must build core-tests and vogar: make core-tests vogar vogar.jar"
  exit 1
fi

# Packages that currently work correctly with the expectation files.
working_packages=("libcore.icu"
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
                  "org.apache.harmony.security"
                  "org.apache.harmony.testframework"
                  "org.apache.harmony.tests.java.io"
                  "org.apache.harmony.tests.java.lang"
                  "org.apache.harmony.tests.java.math"
                  "org.apache.harmony.tests.java.util"
                  "org.apache.harmony.tests.java.text"
                  "org.apache.harmony.tests.javax.security"
                  "tests.java.lang.String")

# Run the tests using vogar.
echo "Running tests for the following test packages:"
echo ${working_packages[@]} | tr " " "\n"
vogar $@ --expectations art/tools/libcore_failures.txt --classpath $test_jar ${working_packages[@]}
