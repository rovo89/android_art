# Copyright (C) 2015 The Android Open Source Project
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

from file_format.common         import SplitStream
from file_format.checker.struct import CheckerFile, TestCase, TestAssertion, RegexExpression

import re

def __extractLine(prefix, line):
  """ Attempts to parse a check line. The regex searches for a comment symbol
      followed by the CHECK keyword, given attribute and a colon at the very
      beginning of the line. Whitespaces are ignored.
  """
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

def __processLine(line, lineNo, prefix):
  """ This function is invoked on each line of the check file and returns a pair
      which instructs the parser how the line should be handled. If the line is
      to be included in the current check group, it is returned in the first
      value. If the line starts a new check group, the name of the group is
      returned in the second value.
  """
  # Lines beginning with 'CHECK-START' start a new test case.
  startLine = __extractLine(prefix + "-START", line)
  if startLine is not None:
    return None, startLine

  # Lines starting only with 'CHECK' are matched in order.
  plainLine = __extractLine(prefix, line)
  if plainLine is not None:
    return (plainLine, TestAssertion.Variant.InOrder, lineNo), None

  # 'CHECK-DAG' lines are no-order assertions.
  dagLine = __extractLine(prefix + "-DAG", line)
  if dagLine is not None:
    return (dagLine, TestAssertion.Variant.DAG, lineNo), None

  # 'CHECK-NOT' lines are no-order negative assertions.
  notLine = __extractLine(prefix + "-NOT", line)
  if notLine is not None:
    return (notLine, TestAssertion.Variant.Not, lineNo), None

  # Other lines are ignored.
  return None, None

def __isMatchAtStart(match):
  """ Tests if the given Match occurred at the beginning of the line. """
  return (match is not None) and (match.start() == 0)

def __firstMatch(matches, string):
  """ Takes in a list of Match objects and returns the minimal start point among
      them. If there aren't any successful matches it returns the length of
      the searched string.
  """
  starts = map(lambda m: len(string) if m is None else m.start(), matches)
  return min(starts)

def ParseCheckerAssertion(parent, line, variant, lineNo):
  """ This method parses the content of a check line stripped of the initial
      comment symbol and the CHECK keyword.
  """
  assertion = TestAssertion(parent, variant, line, lineNo)
  # Loop as long as there is something to parse.
  while line:
    # Search for the nearest occurrence of the special markers.
    matchWhitespace = re.search(r"\s+", line)
    matchPattern = re.search(RegexExpression.Regex.regexPattern, line)
    matchVariableReference = re.search(RegexExpression.Regex.regexVariableReference, line)
    matchVariableDefinition = re.search(RegexExpression.Regex.regexVariableDefinition, line)

    # If one of the above was identified at the current position, extract them
    # from the line, parse them and add to the list of line parts.
    if __isMatchAtStart(matchWhitespace):
      # A whitespace in the check line creates a new separator of line parts.
      # This allows for ignored output between the previous and next parts.
      line = line[matchWhitespace.end():]
      assertion.addExpression(RegexExpression.createSeparator())
    elif __isMatchAtStart(matchPattern):
      pattern = line[0:matchPattern.end()]
      pattern = pattern[2:-2]
      line = line[matchPattern.end():]
      assertion.addExpression(RegexExpression.createPattern(pattern))
    elif __isMatchAtStart(matchVariableReference):
      var = line[0:matchVariableReference.end()]
      line = line[matchVariableReference.end():]
      name = var[2:-2]
      assertion.addExpression(RegexExpression.createVariableReference(name))
    elif __isMatchAtStart(matchVariableDefinition):
      var = line[0:matchVariableDefinition.end()]
      line = line[matchVariableDefinition.end():]
      colonPos = var.find(":")
      name = var[2:colonPos]
      body = var[colonPos+1:-2]
      assertion.addExpression(RegexExpression.createVariableDefinition(name, body))
    else:
      # If we're not currently looking at a special marker, this is a plain
      # text match all the way until the first special marker (or the end
      # of the line).
      firstMatch = __firstMatch([ matchWhitespace,
                                  matchPattern,
                                  matchVariableReference,
                                  matchVariableDefinition ],
                                line)
      text = line[0:firstMatch]
      line = line[firstMatch:]
      assertion.addExpression(RegexExpression.createText(text))
  return assertion

def ParseCheckerStream(fileName, prefix, stream):
  checkerFile = CheckerFile(fileName)
  fnProcessLine = lambda line, lineNo: __processLine(line, lineNo, prefix)
  fnLineOutsideChunk = lambda line, lineNo: \
      Logger.fail("C1visualizer line not inside a group", fileName, lineNo)
  for caseName, caseLines, startLineNo in SplitStream(stream, fnProcessLine, fnLineOutsideChunk):
    testCase = TestCase(checkerFile, caseName, startLineNo)
    for caseLine in caseLines:
      ParseCheckerAssertion(testCase, caseLine[0], caseLine[1], caseLine[2])
  return checkerFile
