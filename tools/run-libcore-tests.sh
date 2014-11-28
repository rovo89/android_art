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

# Packages that currently report no failures.
working_packages=("java/lang"
                  "java/math"
                  "java/util")

# Create a regexp suitable for egrep.
working_packages=$(printf "|%s" "${working_packages[@]}")
working_packages=${working_packages:1}

# Get all the tests for these packages.
test_packages=$(find libcore/*/src/test -name "*.java" | \
  egrep -E $working_packages | \
  xargs grep -h '^package ' | sed 's/^package //' | sed 's/;$//' | sort | uniq | tr "\n" " ")

# Run the tests using vogar.
echo "Running tests for following test packages:"
echo $test_packages | tr " " "\n"
vogar $@ --classpath $test_jar $test_packages
