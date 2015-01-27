#!/usr/bin/env python2
#
# Copyright (C) 2014 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


# Checker is a testing tool which compiles a given test file and compares the
# state of the control-flow graph before and after each optimization pass
# against a set of assertions specified alongside the tests.
#
# Tests are written in Java, turned into DEX and compiled with the Optimizing
# compiler. "Check lines" are assertions formatted as comments of the Java file.
# They begin with prefix 'CHECK' followed by a pattern that the engine attempts
# to match in the compiler-generated output.
#
# Assertions are tested in groups which correspond to the individual compiler
# passes. Each group of check lines therefore must start with a 'CHECK-START'
# header which specifies the output group it should be tested against. The group
# name must exactly match one of the groups recognized in the output (they can
# be listed with the '--list-groups' command-line flag).
#
# Matching of check lines is carried out in the order of appearance in the
# source file. There are three types of check lines:
#  - CHECK:     Must match an output line which appears in the output group
#               later than lines matched against any preceeding checks. Output
#               lines must therefore match the check lines in the same order.
#               These are referred to as "in-order" checks in the code.
#  - CHECK-DAG: Must match an output line which appears in the output group
#               later than lines matched against any preceeding in-order checks.
#               In other words, the order of output lines does not matter
#               between consecutive DAG checks.
#  - CHECK-NOT: Must not match any output line which appears in the output group
#               later than lines matched against any preceeding checks and
#               earlier than lines matched against any subsequent checks.
#               Surrounding non-negative checks (or boundaries of the group)
#               therefore create a scope within which the assertion is verified.
#
# Check-line patterns are treated as plain text rather than regular expressions
# but are whitespace agnostic.
#
# Actual regex patterns can be inserted enclosed in '{{' and '}}' brackets. If
# curly brackets need to be used inside the body of the regex, they need to be
# enclosed in round brackets. For example, the pattern '{{foo{2}}}' will parse
# the invalid regex 'foo{2', but '{{(fo{2})}}' will match 'foo'.
#
# Regex patterns can be named and referenced later. A new variable is defined
# with '[[name:regex]]' and can be referenced with '[[name]]'. Variables are
# only valid within the scope of the defining group. Within a group they cannot
# be redefined or used undefined.
#
# Example:
#   The following assertions can be placed in a Java source file:
#
#   // CHECK-START: int MyClass.MyMethod() constant_folding (after)
#   // CHECK:         [[ID:i[0-9]+]] IntConstant {{11|22}}
#   // CHECK:                        Return [ [[ID]] ]
#
#   The engine will attempt to match the check lines against the output of the
#   group named on the first line. Together they verify that the CFG after
#   constant folding returns an integer constant with value either 11 or 22.
#

from __future__ import print_function
import argparse
import os
import re
import shutil
import sys
import tempfile

class Logger(object):

  class Level(object):
    NoOutput, Error, Info = range(3)

  class Color(object):
    Default, Blue, Gray, Purple, Red = range(5)

    @staticmethod
    def terminalCode(color, out=sys.stdout):
      if not out.isatty():
        return ''
      elif color == Logger.Color.Blue:
        return '\033[94m'
      elif color == Logger.Color.Gray:
        return '\033[37m'
      elif color == Logger.Color.Purple:
        return '\033[95m'
      elif color == Logger.Color.Red:
        return '\033[91m'
      else:
        return '\033[0m'

  Verbosity = Level.Info

  @staticmethod
  def log(text, level=Level.Info, color=Color.Default, newLine=True, out=sys.stdout):
    if level <= Logger.Verbosity:
      text = Logger.Color.terminalCode(color, out) + text + \
             Logger.Color.terminalCode(Logger.Color.Default, out)
      if newLine:
        print(text, file=out)
      else:
        print(text, end="", file=out)
      out.flush()

  @staticmethod
  def fail(msg, file=None, line=-1):
    location = ""
    if file:
      location += file + ":"
    if line > 0:
      location += str(line) + ":"
    if location:
      location += " "

    Logger.log(location, Logger.Level.Error, color=Logger.Color.Gray, newLine=False, out=sys.stderr)
    Logger.log("error: ", Logger.Level.Error, color=Logger.Color.Red, newLine=False, out=sys.stderr)
    Logger.log(msg, Logger.Level.Error, out=sys.stderr)
    sys.exit(msg)

  @staticmethod
  def startTest(name):
    Logger.log("TEST ", color=Logger.Color.Purple, newLine=False)
    Logger.log(name + "... ", newLine=False)

  @staticmethod
  def testPassed():
    Logger.log("PASS", color=Logger.Color.Blue)

  @staticmethod
  def testFailed(msg, file=None, line=-1):
    Logger.log("FAIL", color=Logger.Color.Red)
    Logger.fail(msg, file, line)

class CommonEqualityMixin:
  """Mixin for class equality as equality of the fields."""
  def __eq__(self, other):
    return (isinstance(other, self.__class__)
           and self.__dict__ == other.__dict__)

  def __ne__(self, other):
    return not self.__eq__(other)

  def __repr__(self):
    return "<%s: %s>" % (type(self).__name__, str(self.__dict__))


class CheckElement(CommonEqualityMixin):
  """Single element of the check line."""

  class Variant(object):
    """Supported language constructs."""
    Text, Pattern, VarRef, VarDef, Separator = range(5)

  rStartOptional = r"("
  rEndOptional = r")?"

  rName = r"([a-zA-Z][a-zA-Z0-9]*)"
  rRegex = r"(.+?)"
  rPatternStartSym = r"(\{\{)"
  rPatternEndSym = r"(\}\})"
  rVariableStartSym = r"(\[\[)"
  rVariableEndSym = r"(\]\])"
  rVariableSeparator = r"(:)"

  regexPattern = rPatternStartSym + rRegex + rPatternEndSym
  regexVariable = rVariableStartSym + \
                    rName + \
                    (rStartOptional + rVariableSeparator + rRegex + rEndOptional) + \
                  rVariableEndSym

  def __init__(self, variant, name, pattern):
    self.variant = variant
    self.name = name
    self.pattern = pattern

  @staticmethod
  def newSeparator():
    return CheckElement(CheckElement.Variant.Separator, None, None)

  @staticmethod
  def parseText(text):
    return CheckElement(CheckElement.Variant.Text, None, re.escape(text))

  @staticmethod
  def parsePattern(patternElem):
    return CheckElement(CheckElement.Variant.Pattern, None, patternElem[2:-2])

  @staticmethod
  def parseVariable(varElem):
    colonPos = varElem.find(":")
    if colonPos == -1:
      # Variable reference
      name = varElem[2:-2]
      return CheckElement(CheckElement.Variant.VarRef, name, None)
    else:
      # Variable definition
      name = varElem[2:colonPos]
      body = varElem[colonPos+1:-2]
      return CheckElement(CheckElement.Variant.VarDef, name, body)

class CheckLine(CommonEqualityMixin):
  """Representation of a single assertion in the check file formed of one or
     more regex elements. Matching against an output line is successful only
     if all regex elements can be matched in the given order."""

  class Variant(object):
    """Supported types of assertions."""
    InOrder, DAG, Not = range(3)

  def __init__(self, content, variant=Variant.InOrder, fileName=None, lineNo=-1):
    self.fileName = fileName
    self.lineNo = lineNo
    self.content = content.strip()

    self.variant = variant
    self.lineParts = self.__parse(self.content)
    if not self.lineParts:
      Logger.fail("Empty check line", self.fileName, self.lineNo)

    if self.variant == CheckLine.Variant.Not:
      for elem in self.lineParts:
        if elem.variant == CheckElement.Variant.VarDef:
          Logger.fail("CHECK-NOT lines cannot define variables", self.fileName, self.lineNo)

  def __eq__(self, other):
    return (isinstance(other, self.__class__) and
            self.variant == other.variant and
            self.lineParts == other.lineParts)

  # Returns True if the given Match object was at the beginning of the line.
  def __isMatchAtStart(self, match):
    return (match is not None) and (match.start() == 0)

  # Takes in a list of Match objects and returns the minimal start point among
  # them. If there aren't any successful matches it returns the length of
  # the searched string.
  def __firstMatch(self, matches, string):
    starts = map(lambda m: len(string) if m is None else m.start(), matches)
    return min(starts)

  # This method parses the content of a check line stripped of the initial
  # comment symbol and the CHECK keyword.
  def __parse(self, line):
    lineParts = []
    # Loop as long as there is something to parse.
    while line:
      # Search for the nearest occurrence of the special markers.
      matchWhitespace = re.search(r"\s+", line)
      matchPattern = re.search(CheckElement.regexPattern, line)
      matchVariable = re.search(CheckElement.regexVariable, line)

      # If one of the above was identified at the current position, extract them
      # from the line, parse them and add to the list of line parts.
      if self.__isMatchAtStart(matchWhitespace):
        # A whitespace in the check line creates a new separator of line parts.
        # This allows for ignored output between the previous and next parts.
        line = line[matchWhitespace.end():]
        lineParts.append(CheckElement.newSeparator())
      elif self.__isMatchAtStart(matchPattern):
        pattern = line[0:matchPattern.end()]
        line = line[matchPattern.end():]
        lineParts.append(CheckElement.parsePattern(pattern))
      elif self.__isMatchAtStart(matchVariable):
        var = line[0:matchVariable.end()]
        line = line[matchVariable.end():]
        lineParts.append(CheckElement.parseVariable(var))
      else:
        # If we're not currently looking at a special marker, this is a plain
        # text match all the way until the first special marker (or the end
        # of the line).
        firstMatch = self.__firstMatch([ matchWhitespace, matchPattern, matchVariable ], line)
        text = line[0:firstMatch]
        line = line[firstMatch:]
        lineParts.append(CheckElement.parseText(text))
    return lineParts

  # Returns the regex pattern to be matched in the output line. Variable
  # references are substituted with their current values provided in the
  # 'varState' argument.
  # An exception is raised if a referenced variable is undefined.
  def __generatePattern(self, linePart, varState):
    if linePart.variant == CheckElement.Variant.VarRef:
      try:
        return re.escape(varState[linePart.name])
      except KeyError:
        Logger.testFailed("Use of undefined variable \"" + linePart.name + "\"",
                          self.fileName, self.lineNo)
    else:
      return linePart.pattern

  def __isSeparated(self, outputLine, matchStart):
    return (matchStart == 0) or (outputLine[matchStart - 1:matchStart].isspace())

  # Attempts to match the check line against a line from the output file with
  # the given initial variable values. It returns the new variable state if
  # successful and None otherwise.
  def match(self, outputLine, initialVarState):
    # Do the full matching on a shadow copy of the variable state. If the
    # matching fails half-way, we will not need to revert the state.
    varState = dict(initialVarState)

    matchStart = 0
    isAfterSeparator = True

    # Now try to parse all of the parts of the check line in the right order.
    # Variable values are updated on-the-fly, meaning that a variable can
    # be referenced immediately after its definition.
    for part in self.lineParts:
      if part.variant == CheckElement.Variant.Separator:
        isAfterSeparator = True
        continue

      # Find the earliest match for this line part.
      pattern = self.__generatePattern(part, varState)
      while True:
        match = re.search(pattern, outputLine[matchStart:])
        if (match is None) or (not isAfterSeparator and not self.__isMatchAtStart(match)):
          return None
        matchEnd = matchStart + match.end()
        matchStart += match.start()

        # Check if this is a valid match if we expect a whitespace separator
        # before the matched text. Otherwise loop and look for another match.
        if not isAfterSeparator or self.__isSeparated(outputLine, matchStart):
          break
        else:
          matchStart += 1

      if part.variant == CheckElement.Variant.VarDef:
        if part.name in varState:
          Logger.testFailed("Multiple definitions of variable \"" + part.name + "\"",
                            self.fileName, self.lineNo)
        varState[part.name] = outputLine[matchStart:matchEnd]

      matchStart = matchEnd
      isAfterSeparator = False

    # All parts were successfully matched. Return the new variable state.
    return varState


class CheckGroup(CommonEqualityMixin):
  """Represents a named collection of check lines which are to be matched
     against an output group of the same name."""

  def __init__(self, name, lines, fileName=None, lineNo=-1):
    self.fileName = fileName
    self.lineNo = lineNo

    if not name:
      Logger.fail("Check group does not have a name", self.fileName, self.lineNo)
    if not lines:
      Logger.fail("Check group does not have a body", self.fileName, self.lineNo)

    self.name = name
    self.lines = lines

  def __eq__(self, other):
    return (isinstance(other, self.__class__) and
            self.name == other.name and
            self.lines == other.lines)

  def __headAndTail(self, list):
    return list[0], list[1:]

  # Splits a list of check lines at index 'i' such that lines[i] is the first
  # element whose variant is not equal to the given parameter.
  def __splitByVariant(self, lines, variant):
    i = 0
    while i < len(lines) and lines[i].variant == variant:
      i += 1
    return lines[:i], lines[i:]

  # Extracts the first sequence of check lines which are independent of each
  # other's match location, i.e. either consecutive DAG lines or a single
  # InOrder line. Any Not lines preceeding this sequence are also extracted.
  def __nextIndependentChecks(self, checkLines):
    notChecks, checkLines = self.__splitByVariant(checkLines, CheckLine.Variant.Not)
    if not checkLines:
      return notChecks, [], []

    head, tail = self.__headAndTail(checkLines)
    if head.variant == CheckLine.Variant.InOrder:
      return notChecks, [head], tail
    else:
      assert head.variant == CheckLine.Variant.DAG
      independentChecks, checkLines = self.__splitByVariant(checkLines, CheckLine.Variant.DAG)
      return notChecks, independentChecks, checkLines

  # If successful, returns the line number of the first output line matching the
  # check line and the updated variable state. Otherwise returns -1 and None,
  # respectively. The 'lineFilter' parameter can be used to supply a list of
  # line numbers (counting from 1) which should be skipped.
  def __findFirstMatch(self, checkLine, outputLines, startLineNo, lineFilter, varState):
    matchLineNo = startLineNo
    for outputLine in outputLines:
      if matchLineNo not in lineFilter:
        newVarState = checkLine.match(outputLine, varState)
        if newVarState is not None:
          return matchLineNo, newVarState
      matchLineNo += 1
    return -1, None

  # Matches the given positive check lines against the output in order of
  # appearance. Variable state is propagated but the scope of the search remains
  # the same for all checks. Each output line can only be matched once.
  # If all check lines are matched, the resulting variable state is returned
  # together with the remaining output. The function also returns output lines
  # which appear before either of the matched lines so they can be tested
  # against Not checks.
  def __matchIndependentChecks(self, checkLines, outputLines, startLineNo, varState):
    # If no checks are provided, skip over the entire output.
    if not checkLines:
      return outputLines, [], startLineNo + len(outputLines), varState

    # Keep track of which lines have been matched.
    matchedLines = []

    # Find first unused output line which matches each check line.
    for checkLine in checkLines:
      matchLineNo, varState = \
        self.__findFirstMatch(checkLine, outputLines, startLineNo, matchedLines, varState)
      if varState is None:
        Logger.testFailed("Could not match check line \"" + checkLine.content + "\" " +
                          "starting from output line " + str(startLineNo),
                          self.fileName, checkLine.lineNo)
      matchedLines.append(matchLineNo)

    # Return new variable state and the output lines which lie outside the
    # match locations of this independent group.
    minMatchLineNo = min(matchedLines)
    maxMatchLineNo = max(matchedLines)
    preceedingLines = outputLines[:minMatchLineNo - startLineNo]
    remainingLines = outputLines[maxMatchLineNo - startLineNo + 1:]
    return preceedingLines, remainingLines, maxMatchLineNo + 1, varState

  # Makes sure that the given check lines do not match any of the given output
  # lines. Variable state does not change.
  def __matchNotLines(self, checkLines, outputLines, startLineNo, varState):
    for checkLine in checkLines:
      assert checkLine.variant == CheckLine.Variant.Not
      matchLineNo, matchVarState = \
        self.__findFirstMatch(checkLine, outputLines, startLineNo, [], varState)
      if matchVarState is not None:
        Logger.testFailed("CHECK-NOT line \"" + checkLine.content + "\" matches output line " + \
                          str(matchLineNo), self.fileName, checkLine.lineNo)

  # Matches the check lines in this group against an output group. It is
  # responsible for running the checks in the right order and scope, and
  # for propagating the variable state between the check lines.
  def match(self, outputGroup):
    varState = {}
    checkLines = self.lines
    outputLines = outputGroup.body
    startLineNo = outputGroup.lineNo

    while checkLines:
      # Extract the next sequence of location-independent checks to be matched.
      notChecks, independentChecks, checkLines = self.__nextIndependentChecks(checkLines)

      # Match the independent checks.
      notOutput, outputLines, newStartLineNo, newVarState = \
        self.__matchIndependentChecks(independentChecks, outputLines, startLineNo, varState)

      # Run the Not checks against the output lines which lie between the last
      # two independent groups or the bounds of the output.
      self.__matchNotLines(notChecks, notOutput, startLineNo, varState)

      # Update variable state.
      startLineNo = newStartLineNo
      varState = newVarState

class OutputGroup(CommonEqualityMixin):
  """Represents a named part of the test output against which a check group of
     the same name is to be matched."""

  def __init__(self, name, body, fileName=None, lineNo=-1):
    if not name:
      Logger.fail("Output group does not have a name", fileName, lineNo)
    if not body:
      Logger.fail("Output group does not have a body", fileName, lineNo)

    self.name = name
    self.body = body
    self.lineNo = lineNo

  def __eq__(self, other):
    return (isinstance(other, self.__class__) and
            self.name == other.name and
            self.body == other.body)


class FileSplitMixin(object):
  """Mixin for representing text files which need to be split into smaller
     chunks before being parsed."""

  def _parseStream(self, stream):
    lineNo = 0
    allGroups = []
    currentGroup = None

    for line in stream:
      lineNo += 1
      line = line.strip()
      if not line:
        continue

      # Let the child class process the line and return information about it.
      # The _processLine method can modify the content of the line (or delete it
      # entirely) and specify whether it starts a new group.
      processedLine, newGroupName = self._processLine(line, lineNo)
      if newGroupName is not None:
        currentGroup = (newGroupName, [], lineNo)
        allGroups.append(currentGroup)
      if processedLine is not None:
        if currentGroup is not None:
          currentGroup[1].append(processedLine)
        else:
          self._exceptionLineOutsideGroup(line, lineNo)

    # Finally, take the generated line groups and let the child class process
    # each one before storing the final outcome.
    return list(map(lambda group: self._processGroup(group[0], group[1], group[2]), allGroups))


class CheckFile(FileSplitMixin):
  """Collection of check groups extracted from the input test file."""

  def __init__(self, prefix, checkStream, fileName=None):
    self.fileName = fileName
    self.prefix = prefix
    self.groups = self._parseStream(checkStream)

  # Attempts to parse a check line. The regex searches for a comment symbol
  # followed by the CHECK keyword, given attribute and a colon at the very
  # beginning of the line. Whitespaces are ignored.
  def _extractLine(self, prefix, line):
    rIgnoreWhitespace = r"\s*"
    rCommentSymbols = [r"//", r"#"]
    regexPrefix = rIgnoreWhitespace + \
                  r"(" + r"|".join(rCommentSymbols) + r")" + \
                  rIgnoreWhitespace + \
                  prefix + r":"

    # The 'match' function succeeds only if the pattern is matched at the
    # beginning of the line.
    match = re.match(regexPrefix, line)
    if match is not None:
      return line[match.end():].strip()
    else:
      return None

  # This function is invoked on each line of the check file and returns a pair
  # which instructs the parser how the line should be handled. If the line is to
  # be included in the current check group, it is returned in the first value.
  # If the line starts a new check group, the name of the group is returned in
  # the second value.
  def _processLine(self, line, lineNo):
    # Lines beginning with 'CHECK-START' start a new check group.
    startLine = self._extractLine(self.prefix + "-START", line)
    if startLine is not None:
      return None, startLine

    # Lines starting only with 'CHECK' are matched in order.
    plainLine = self._extractLine(self.prefix, line)
    if plainLine is not None:
      return (plainLine, CheckLine.Variant.InOrder, lineNo), None

    # 'CHECK-DAG' lines are no-order assertions.
    dagLine = self._extractLine(self.prefix + "-DAG", line)
    if dagLine is not None:
      return (dagLine, CheckLine.Variant.DAG, lineNo), None

    # 'CHECK-NOT' lines are no-order negative assertions.
    notLine = self._extractLine(self.prefix + "-NOT", line)
    if notLine is not None:
      return (notLine, CheckLine.Variant.Not, lineNo), None

    # Other lines are ignored.
    return None, None

  def _exceptionLineOutsideGroup(self, line, lineNo):
    Logger.fail("Check line not inside a group", self.fileName, lineNo)

  # Constructs a check group from the parser-collected check lines.
  def _processGroup(self, name, lines, lineNo):
    checkLines = list(map(lambda line: CheckLine(line[0], line[1], self.fileName, line[2]), lines))
    return CheckGroup(name, checkLines, self.fileName, lineNo)

  def match(self, outputFile):
    for checkGroup in self.groups:
      # TODO: Currently does not handle multiple occurrences of the same group
      # name, e.g. when a pass is run multiple times. It will always try to
      # match a check group against the first output group of the same name.
      outputGroup = outputFile.findGroup(checkGroup.name)
      if outputGroup is None:
        Logger.fail("Group \"" + checkGroup.name + "\" not found in the output",
                    self.fileName, checkGroup.lineNo)
      Logger.startTest(checkGroup.name)
      checkGroup.match(outputGroup)
      Logger.testPassed()


class OutputFile(FileSplitMixin):
  """Representation of the output generated by the test and split into groups
     within which the checks are performed.

     C1visualizer format is parsed with a state machine which differentiates
     between the 'compilation' and 'cfg' blocks. The former marks the beginning
     of a method. It is parsed for the method's name but otherwise ignored. Each
     subsequent CFG block represents one stage of the compilation pipeline and
     is parsed into an output group named "<method name> <pass name>".
     """

  class ParsingState:
    OutsideBlock, InsideCompilationBlock, StartingCfgBlock, InsideCfgBlock = range(4)

  def __init__(self, outputStream, fileName=None):
    self.fileName = fileName

    # Initialize the state machine
    self.lastMethodName = None
    self.state = OutputFile.ParsingState.OutsideBlock
    self.groups = self._parseStream(outputStream)

  # This function is invoked on each line of the output file and returns a pair
  # which instructs the parser how the line should be handled. If the line is to
  # be included in the current group, it is returned in the first value. If the
  # line starts a new output group, the name of the group is returned in the
  # second value.
  def _processLine(self, line, lineNo):
    if self.state == OutputFile.ParsingState.StartingCfgBlock:
      # Previous line started a new 'cfg' block which means that this one must
      # contain the name of the pass (this is enforced by C1visualizer).
      if re.match("name\s+\"[^\"]+\"", line):
        # Extract the pass name, prepend it with the name of the method and
        # return as the beginning of a new group.
        self.state = OutputFile.ParsingState.InsideCfgBlock
        return (None, self.lastMethodName + " " + line.split("\"")[1])
      else:
        Logger.fail("Expected output group name", self.fileName, lineNo)

    elif self.state == OutputFile.ParsingState.InsideCfgBlock:
      if line == "end_cfg":
        self.state = OutputFile.ParsingState.OutsideBlock
        return (None, None)
      else:
        return (line, None)

    elif self.state == OutputFile.ParsingState.InsideCompilationBlock:
      # Search for the method's name. Format: method "<name>"
      if re.match("method\s+\"[^\"]*\"", line):
        methodName = line.split("\"")[1].strip()
        if not methodName:
          Logger.fail("Empty method name in output", self.fileName, lineNo)
        self.lastMethodName = methodName
      elif line == "end_compilation":
        self.state = OutputFile.ParsingState.OutsideBlock
      return (None, None)

    else:
      assert self.state == OutputFile.ParsingState.OutsideBlock
      if line == "begin_cfg":
        # The line starts a new group but we'll wait until the next line from
        # which we can extract the name of the pass.
        if self.lastMethodName is None:
          Logger.fail("Expected method header", self.fileName, lineNo)
        self.state = OutputFile.ParsingState.StartingCfgBlock
        return (None, None)
      elif line == "begin_compilation":
        self.state = OutputFile.ParsingState.InsideCompilationBlock
        return (None, None)
      else:
        Logger.fail("Output line not inside a group", self.fileName, lineNo)

  # Constructs an output group from the parser-collected output lines.
  def _processGroup(self, name, lines, lineNo):
    return OutputGroup(name, lines, self.fileName, lineNo + 1)

  def findGroup(self, name):
    for group in self.groups:
      if group.name == name:
        return group
    return None


def ParseArguments():
  parser = argparse.ArgumentParser()
  parser.add_argument("tested_file",
                      help="text file the checks should be verified against")
  parser.add_argument("source_path", nargs="?",
                      help="path to file/folder with checking annotations")
  parser.add_argument("--check-prefix", dest="check_prefix", default="CHECK", metavar="PREFIX",
                      help="prefix of checks in the test files (default: CHECK)")
  parser.add_argument("--list-groups", dest="list_groups", action="store_true",
                      help="print a list of all groups found in the tested file")
  parser.add_argument("--dump-group", dest="dump_group", metavar="GROUP",
                      help="print the contents of an output group")
  parser.add_argument("-q", "--quiet", action="store_true",
                      help="print only errors")
  return parser.parse_args()


def ListGroups(outputFilename):
  outputFile = OutputFile(open(outputFilename, "r"))
  for group in outputFile.groups:
    Logger.log(group.name)


def DumpGroup(outputFilename, groupName):
  outputFile = OutputFile(open(outputFilename, "r"))
  group = outputFile.findGroup(groupName)
  if group:
    lineNo = group.lineNo
    maxLineNo = lineNo + len(group.body)
    lenLineNo = len(str(maxLineNo)) + 2
    for line in group.body:
      Logger.log((str(lineNo) + ":").ljust(lenLineNo) + line)
      lineNo += 1
  else:
    Logger.fail("Group \"" + groupName + "\" not found in the output")


# Returns a list of files to scan for check annotations in the given path. Path
# to a file is returned as a single-element list, directories are recursively
# traversed and all '.java' files returned.
def FindCheckFiles(path):
  if not path:
    Logger.fail("No source path provided")
  elif os.path.isfile(path):
    return [ path ]
  elif os.path.isdir(path):
    foundFiles = []
    for root, dirs, files in os.walk(path):
      for file in files:
        if os.path.splitext(file)[1] == ".java":
          foundFiles.append(os.path.join(root, file))
    return foundFiles
  else:
    Logger.fail("Source path \"" + path + "\" not found")


def RunChecks(checkPrefix, checkPath, outputFilename):
  outputBaseName = os.path.basename(outputFilename)
  outputFile = OutputFile(open(outputFilename, "r"), outputBaseName)

  for checkFilename in FindCheckFiles(checkPath):
    checkBaseName = os.path.basename(checkFilename)
    checkFile = CheckFile(checkPrefix, open(checkFilename, "r"), checkBaseName)
    checkFile.match(outputFile)


if __name__ == "__main__":
  args = ParseArguments()

  if args.quiet:
    Logger.Verbosity = Logger.Level.Error

  if args.list_groups:
    ListGroups(args.tested_file)
  elif args.dump_group:
    DumpGroup(args.tested_file, args.dump_group)
  else:
    RunChecks(args.check_prefix, args.source_path, args.tested_file)
