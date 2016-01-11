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

from common.logger              import Logger
from file_format.checker.struct import TestAssertion, RegexExpression

import re

def __isMatchAtStart(match):
  """ Tests if the given Match occurred at the beginning of the line. """
  return (match is not None) and (match.start() == 0)

def __generatePattern(checkLine, linePart, varState):
  """ Returns the regex pattern to be matched in the output line. Variable
      references are substituted with their current values provided in the
      'varState' argument.

  An exception is raised if a referenced variable is undefined.
  """
  if linePart.variant == RegexExpression.Variant.VarRef:
    try:
      return re.escape(varState[linePart.name])
    except KeyError:
      Logger.testFailed("Use of undefined variable \"" + linePart.name + "\"",
                        checkLine.fileName, checkLine.lineNo)
  else:
    return linePart.pattern

def __isSeparated(outputLine, matchStart):
  return (matchStart == 0) or (outputLine[matchStart - 1:matchStart].isspace())

def MatchLines(checkLine, outputLine, initialVarState):
  """ Attempts to match the check line against a line from the output file with
      the given initial variable values. It returns the new variable state if
      successful and None otherwise.
  """
  # Do the full matching on a shadow copy of the variable state. If the
  # matching fails half-way, we will not need to revert the state.
  varState = dict(initialVarState)

  matchStart = 0
  isAfterSeparator = True

  # Now try to parse all of the parts of the check line in the right order.
  # Variable values are updated on-the-fly, meaning that a variable can
  # be referenced immediately after its definition.
  for part in checkLine.expressions:
    if part.variant == RegexExpression.Variant.Separator:
      isAfterSeparator = True
      continue

    # Find the earliest match for this line part.
    pattern = __generatePattern(checkLine, part, varState)
    while True:
      match = re.search(pattern, outputLine[matchStart:])
      if (match is None) or (not isAfterSeparator and not __isMatchAtStart(match)):
        return None
      matchEnd = matchStart + match.end()
      matchStart += match.start()

      # Check if this is a valid match if we expect a whitespace separator
      # before the matched text. Otherwise loop and look for another match.
      if not isAfterSeparator or __isSeparated(outputLine, matchStart):
        break
      else:
        matchStart += 1

    if part.variant == RegexExpression.Variant.VarDef:
      if part.name in varState:
        Logger.testFailed("Multiple definitions of variable \"" + part.name + "\"",
                          checkLine.fileName, checkLine.lineNo)
      varState[part.name] = outputLine[matchStart:matchEnd]

    matchStart = matchEnd
    isAfterSeparator = False

  # All parts were successfully matched. Return the new variable state.
  return varState
