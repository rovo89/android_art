#!/bin/sh
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
#
# Symbolize oat files from the dalvik cache of a device.
#
# By default, pulls everything from the dalvik cache. A simple yes/no/quit prompt for each file can
# be requested by giving "--interactive" as a parameter.

INTERACTIVE="no"
if [ "x$1" = "x--interactive" ] ; then
  INTERACTIVE="yes"
fi

# Pull the file from the device and symbolize it.
function one() {
  echo $1 $2
  if [ "x$INTERACTIVE" = "xyes" ] ; then
    echo -n "What to do? [Y/n/q] "
    read -e input
    if [ "x$input" = "xn" ] ; then
      return
    fi
    if [ "x$input" = "xq" ] ; then
      exit 0
    fi
  fi
  adb pull /data/dalvik-cache/$1/$2 /tmp || exit 1
  mkdir -p $OUT/symbols/data/dalvik-cache/$1
  oatdump --symbolize=/tmp/$2 --output=$OUT/symbols/data/dalvik-cache/$1/$2
}

# adb shell ls seems to output in DOS format (CRLF), which messes up scripting
function adbls() {
  adb shell ls $@ | sed 's/\r$//'
}

# Check for all ISA directories on device.
function all() {
  DIRS=$(adbls /data/dalvik-cache/)
  for DIR in $DIRS ; do
    case $DIR in
      arm|arm64|mips|x86|x86_64)
        FILES=$(adbls /data/dalvik-cache/$DIR/*.oat /data/dalvik-cache/$DIR/*.dex)
        for FILE in $FILES ; do
          # Cannot use basename as the file doesn't exist.
          NAME=$(echo $FILE | sed -e 's/.*\///')
          one $DIR $NAME
        done
        ;;
    esac
  done
}

all
