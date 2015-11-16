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

from common.logger                    import Logger
from file_format.c1visualizer.struct  import C1visualizerFile, C1visualizerPass
from file_format.checker.struct       import CheckerFile, TestCase, TestAssertion
from match.line                       import MatchLines

def __headAndTail(list):
  return list[0], list[1:]

def __splitByVariant(lines, variant):
  """ Splits a list of check lines at index 'i' such that lines[i] is the first
      element whose variant is not equal to the given parameter.
  """
  i = 0
  while i < len(lines) and lines[i].variant == variant:
    i += 1
  return lines[:i], lines[i:]

def __nextIndependentChecks(checkLines):
  """ Extracts the first sequence of check lines which are independent of each
      other's match location, i.e. either consecutive DAG lines or a single
      InOrder line. Any Not lines preceeding this sequence are also extracted.
  """
  notChecks, checkLines = __splitByVariant(checkLines, TestAssertion.Variant.Not)
  if not checkLines:
    return notChecks, [], []

  head, tail = __headAndTail(checkLines)
  if head.variant == TestAssertion.Variant.InOrder:
    return notChecks, [head], tail
  else:
    assert head.variant == TestAssertion.Variant.DAG
    independentChecks, checkLines = __splitByVariant(checkLines, TestAssertion.Variant.DAG)
    return notChecks, independentChecks, checkLines

def __findFirstMatch(checkLine, outputLines, startLineNo, lineFilter, varState):
  """ If successful, returns the line number of the first output line matching
      the check line and the updated variable state. Otherwise returns -1 and
      None, respectively. The 'lineFilter' parameter can be used to supply a
      list of line numbers (counting from 1) which should be skipped.
  """
  matchLineNo = startLineNo
  for outputLine in outputLines:
    if matchLineNo not in lineFilter:
      newVarState = MatchLines(checkLine, outputLine, varState)
      if newVarState is not None:
        return matchLineNo, newVarState
    matchLineNo += 1
  return -1, None

def __matchIndependentChecks(checkLines, outputLines, startLineNo, varState):
  """ Matches the given positive check lines against the output in order of
      appearance. Variable state is propagated but the scope of the search
      remains the same for all checks. Each output line can only be matched
      once. If all check lines are matched, the resulting variable state is
      returned together with the remaining output. The function also returns
      output lines which appear before either of the matched lines so they can
      be tested against Not checks.
  """
  # If no checks are provided, skip over the entire output.
  if not checkLines:
    return outputLines, [], startLineNo + len(outputLines), varState

  # Keep track of which lines have been matched.
  matchedLines = []

  # Find first unused output line which matches each check line.
  for checkLine in checkLines:
    matchLineNo, varState = \
      __findFirstMatch(checkLine, outputLines, startLineNo, matchedLines, varState)
    if varState is None:
      Logger.testFailed("Could not match check line \"" + checkLine.originalText + "\" " +
                        "starting from output line " + str(startLineNo),
                        checkLine.fileName, checkLine.lineNo)
    matchedLines.append(matchLineNo)

  # Return new variable state and the output lines which lie outside the
  # match locations of this independent group.
  minMatchLineNo = min(matchedLines)
  maxMatchLineNo = max(matchedLines)
  preceedingLines = outputLines[:minMatchLineNo - startLineNo]
  remainingLines = outputLines[maxMatchLineNo - startLineNo + 1:]
  return preceedingLines, remainingLines, maxMatchLineNo + 1, varState

def __matchNotLines(checkLines, outputLines, startLineNo, varState):
  """ Makes sure that the given check lines do not match any of the given output
      lines. Variable state does not change.
  """
  for checkLine in checkLines:
    assert checkLine.variant == TestAssertion.Variant.Not
    matchLineNo, matchVarState = \
      __findFirstMatch(checkLine, outputLines, startLineNo, [], varState)
    if matchVarState is not None:
      Logger.testFailed("CHECK-NOT line \"" + checkLine.originalText + "\" matches output line " + \
                        str(matchLineNo), checkLine.fileName, checkLine.lineNo)

def __matchGroups(checkGroup, outputGroup):
  """ Matches the check lines in this group against an output group. It is
      responsible for running the checks in the right order and scope, and
      for propagating the variable state between the check lines.
  """
  varState = {}
  checkLines = checkGroup.assertions
  outputLines = outputGroup.body
  startLineNo = outputGroup.startLineNo

  while checkLines:
    # Extract the next sequence of location-independent checks to be matched.
    notChecks, independentChecks, checkLines = __nextIndependentChecks(checkLines)

    # Match the independent checks.
    notOutput, outputLines, newStartLineNo, newVarState = \
      __matchIndependentChecks(independentChecks, outputLines, startLineNo, varState)

    # Run the Not checks against the output lines which lie between the last
    # two independent groups or the bounds of the output.
    __matchNotLines(notChecks, notOutput, startLineNo, varState)

    # Update variable state.
    startLineNo = newStartLineNo
    varState = newVarState

def MatchFiles(checkerFile, c1File):
  for testCase in checkerFile.testCases:
    # TODO: Currently does not handle multiple occurrences of the same group
    # name, e.g. when a pass is run multiple times. It will always try to
    # match a check group against the first output group of the same name.
    c1Pass = c1File.findPass(testCase.name)
    if c1Pass is None:
      Logger.fail("Test case \"" + testCase.name + "\" not found in the C1visualizer output",
                  testCase.fileName, testCase.lineNo)
    Logger.startTest(testCase.name)
    __matchGroups(testCase, c1Pass)
    Logger.testPassed()
