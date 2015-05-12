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

from common.logger import Logger
from common.mixins import EqualityMixin, PrintableMixin

import re

class CheckerFile(PrintableMixin):

  def __init__(self, fileName):
    self.fileName = fileName
    self.testCases = []

  def addTestCase(self, new_test_case):
    self.testCases.append(new_test_case)

  def __eq__(self, other):
    return isinstance(other, self.__class__) \
       and self.testCases == other.testCases


class TestCase(PrintableMixin):

  def __init__(self, parent, name, startLineNo):
    assert isinstance(parent, CheckerFile)

    self.parent = parent
    self.name = name
    self.assertions = []
    self.startLineNo = startLineNo

    if not self.name:
      Logger.fail("Test case does not have a name", self.parent.fileName, self.startLineNo)

    self.parent.addTestCase(self)

  @property
  def fileName(self):
    return self.parent.fileName

  def addAssertion(self, new_assertion):
    self.assertions.append(new_assertion)

  def __eq__(self, other):
    return isinstance(other, self.__class__) \
       and self.name == other.name \
       and self.assertions == other.assertions


class TestAssertion(PrintableMixin):

  class Variant(object):
    """Supported types of assertions."""
    InOrder, DAG, Not = range(3)

  def __init__(self, parent, variant, originalText, lineNo):
    assert isinstance(parent, TestCase)

    self.parent = parent
    self.variant = variant
    self.expressions = []
    self.lineNo = lineNo
    self.originalText = originalText

    self.parent.addAssertion(self)

  @property
  def fileName(self):
    return self.parent.fileName

  def addExpression(self, new_expression):
    assert isinstance(new_expression, RegexExpression)
    if self.variant == TestAssertion.Variant.Not:
      if new_expression.variant == RegexExpression.Variant.VarDef:
        Logger.fail("CHECK-NOT lines cannot define variables", self.fileName, self.lineNo)
    self.expressions.append(new_expression)

  def toRegex(self):
    """ Returns a regex pattern for this entire assertion. Only used in tests. """
    regex = ""
    for expression in self.expressions:
      if expression.variant == RegexExpression.Variant.Separator:
        regex = regex + ", "
      else:
        regex = regex + "(" + expression.pattern + ")"
    return regex

  def __eq__(self, other):
    return isinstance(other, self.__class__) \
       and self.variant == other.variant \
       and self.expressions == other.expressions


class RegexExpression(EqualityMixin, PrintableMixin):

  class Variant(object):
    """Supported language constructs."""
    Text, Pattern, VarRef, VarDef, Separator = range(5)

  class Regex(object):
    rName = r"([a-zA-Z][a-zA-Z0-9]*)"
    rRegex = r"(.+?)"
    rPatternStartSym = r"(\{\{)"
    rPatternEndSym = r"(\}\})"
    rVariableStartSym = r"(\[\[)"
    rVariableEndSym = r"(\]\])"
    rVariableSeparator = r"(:)"

    regexPattern = rPatternStartSym + rRegex + rPatternEndSym
    regexVariableReference = rVariableStartSym + rName + rVariableEndSym
    regexVariableDefinition = rVariableStartSym + rName + rVariableSeparator + rRegex + rVariableEndSym

  def __init__(self, variant, name, pattern):
    self.variant = variant
    self.name = name
    self.pattern = pattern

  def __eq__(self, other):
    return isinstance(other, self.__class__) \
       and self.variant == other.variant \
       and self.name == other.name \
       and self.pattern == other.pattern

  @staticmethod
  def createSeparator():
    return RegexExpression(RegexExpression.Variant.Separator, None, None)

  @staticmethod
  def createText(text):
    return RegexExpression(RegexExpression.Variant.Text, None, re.escape(text))

  @staticmethod
  def createPattern(pattern):
    return RegexExpression(RegexExpression.Variant.Pattern, None, pattern)

  @staticmethod
  def createVariableReference(name):
    assert re.match(RegexExpression.Regex.rName, name)
    return RegexExpression(RegexExpression.Variant.VarRef, name, None)

  @staticmethod
  def createVariableDefinition(name, pattern):
    assert re.match(RegexExpression.Regex.rName, name)
    return RegexExpression(RegexExpression.Variant.VarDef, name, pattern)
