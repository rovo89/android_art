#!/usr/bin/python
#
# Copyright 2012 Google Inc. All Rights Reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#    * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#    * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Generates default implementations of operator<< for enum types."""

import codecs
import os
import re
import string
import sys


_ENUM_START_RE = re.compile(r'\benum\b\s+(\S+)\s+\{')
_ENUM_VALUE_RE = re.compile(r'([A-Za-z0-9_]+)(.*)')
_ENUM_END_RE = re.compile(r'^\s*\};$')
_ENUMS = {}
_NAMESPACES = {}

def Confused(filename, line_number, line):
  sys.stderr.write('%s:%d: confused by:\n%s\n' % (filename, line_number, line))
  raise Exception("giving up!")
  sys.exit(1)


def ProcessFile(filename):
  lines = codecs.open(filename, 'r', 'utf8', 'replace').read().split('\n')
  in_enum = False
  line_number = 0

  namespaces = []
  enclosing_classes = []

  for raw_line in lines:
    line_number += 1

    if not in_enum:
      # Is this the start of a new enum?
      m = _ENUM_START_RE.search(raw_line)
      if m:
        # Yes, so add an empty entry to _ENUMS for this enum.
        enum_name = m.group(1)
        if len(enclosing_classes) > 0:
          enum_name = '::'.join(enclosing_classes) + '::' + enum_name
        _ENUMS[enum_name] = []
        _NAMESPACES[enum_name] = '::'.join(namespaces)
        in_enum = True
        continue

      # Is this the start or end of a namespace?
      m = re.compile(r'^namespace (\S+) \{').search(raw_line)
      if m:
        namespaces.append(m.group(1))
        continue
      m = re.compile(r'^\}\s+// namespace').search(raw_line)
      if m:
        namespaces = namespaces[0:len(namespaces) - 1]
        continue

      # Is this the start or end of an enclosing class or struct?
      m = re.compile(r'^(?:class|struct)(?: MANAGED)? (\S+).* \{').search(raw_line)
      if m:
        enclosing_classes.append(m.group(1))
        continue
      m = re.compile(r'^\};').search(raw_line)
      if m:
        enclosing_classes = enclosing_classes[0:len(enclosing_classes) - 1]
        continue

      continue

    # Is this the end of the current enum?
    m = _ENUM_END_RE.search(raw_line)
    if m:
      if not in_enum:
        Confused(filename, line_number, raw_line)
      in_enum = False
      continue

    # Strip // comments.
    line = re.sub(r'//.*', '', raw_line)

    # Strip whitespace.
    line = line.strip()

    # Skip blank lines.
    if len(line) == 0:
      continue

    # Is this another enum value?
    m = _ENUM_VALUE_RE.search(line)
    if not m:
      Confused(filename, line_number, raw_line)

    enum_value = m.group(1)

    # By default, we turn "kSomeValue" into "SomeValue".
    enum_text = enum_value
    if enum_text.startswith('k'):
      enum_text = enum_text[1:]

    # Lose literal values because we don't care; turn "= 123, // blah" into ", // blah".
    rest = m.group(2).strip()
    m_literal = re.compile(r'= (0x[0-9a-f]+|-?[0-9]+|\'.\')').search(rest)
    if m_literal:
      rest = rest[(len(m_literal.group(0))):]

    # With "kSomeValue = kOtherValue," we take the original and skip later synonyms.
    # TODO: check that the rhs is actually an existing value.
    if rest.startswith('= k'):
      continue

    # Remove any trailing comma and whitespace
    if rest.startswith(','):
      rest = rest[1:]
    rest = rest.strip()

    # Anything left should be a comment.
    if len(rest) and not rest.startswith('// '):
      Confused(filename, line_number, raw_line)

    m_comment = re.compile(r'<<(.*?)>>').search(rest)
    if m_comment:
      enum_text = m_comment.group(1)

    if len(enclosing_classes) > 0:
      enum_value = '::'.join(enclosing_classes) + '::' + enum_value

    _ENUMS[enum_name].append((enum_value, enum_text))

def main():
  header_files = []
  for header_file in sys.argv[1:]:
    header_files.append(header_file)
    ProcessFile(header_file)

  print '#include <iostream>'
  print

  for header_file in header_files:
    # Make gives us paths relative to the top of the tree, but our -I is art/.
    # We also have -I art/src/, but icu4c is higher on the include path and has a "mutex.h" too.
    header_file = header_file.replace('art/', '')
    print '#include "%s"' % header_file

  print

  for enum_name in _ENUMS:
    print '// This was automatically generated by %s --- do not edit!' % sys.argv[0]

    namespaces = _NAMESPACES[enum_name].split('::')
    for namespace in namespaces:
      print 'namespace %s {' % namespace

    print 'std::ostream& operator<<(std::ostream& os, const %s& rhs) {' % enum_name
    print '  switch (rhs) {'
    for (enum_value, enum_text) in _ENUMS[enum_name]:
      print '    case %s: os << "%s"; break;' % (enum_value, enum_text)
    print '    default: os << "%s[" << static_cast<int>(rhs) << "]"; break;' % enum_name
    print '  }'
    print '  return os;'
    print '}'

    for namespace in reversed(namespaces):
      print '}  // namespace %s' % namespace
    print

  sys.exit(0)


if __name__ == '__main__':
  main()
