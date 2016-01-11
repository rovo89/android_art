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

from common.testing                  import ToUnicode
from file_format.c1visualizer.parser import ParseC1visualizerStream
from file_format.c1visualizer.struct import C1visualizerFile, C1visualizerPass
from file_format.checker.parser      import ParseCheckerStream, ParseCheckerAssertion
from file_format.checker.struct      import CheckerFile, TestCase, TestAssertion, RegexExpression
from match.file                      import MatchFiles
from match.line                      import MatchLines

import io
import unittest

CheckerException = SystemExit

class MatchLines_Test(unittest.TestCase):

  def createTestAssertion(self, checkerString):
    checkerFile = CheckerFile("<checker-file>")
    testCase = TestCase(checkerFile, "TestMethod TestPass", 0)
    return ParseCheckerAssertion(testCase, checkerString, TestAssertion.Variant.InOrder, 0)

  def tryMatch(self, checkerString, c1String, varState={}):
    return MatchLines(self.createTestAssertion(checkerString), ToUnicode(c1String), varState)

  def matches(self, checkerString, c1String, varState={}):
    return self.tryMatch(checkerString, c1String, varState) is not None

  def test_TextAndWhitespace(self):
    self.assertTrue(self.matches("foo", "foo"))
    self.assertTrue(self.matches("foo", "  foo  "))
    self.assertTrue(self.matches("foo", "foo bar"))
    self.assertFalse(self.matches("foo", "XfooX"))
    self.assertFalse(self.matches("foo", "zoo"))

    self.assertTrue(self.matches("foo bar", "foo   bar"))
    self.assertTrue(self.matches("foo bar", "abc foo bar def"))
    self.assertTrue(self.matches("foo bar", "foo foo bar bar"))

    self.assertTrue(self.matches("foo bar", "foo X bar"))
    self.assertFalse(self.matches("foo bar", "foo Xbar"))

  def test_Pattern(self):
    self.assertTrue(self.matches("foo{{A|B}}bar", "fooAbar"))
    self.assertTrue(self.matches("foo{{A|B}}bar", "fooBbar"))
    self.assertFalse(self.matches("foo{{A|B}}bar", "fooCbar"))

  def test_VariableReference(self):
    self.assertTrue(self.matches("foo[[X]]bar", "foobar", {"X": ""}))
    self.assertTrue(self.matches("foo[[X]]bar", "fooAbar", {"X": "A"}))
    self.assertTrue(self.matches("foo[[X]]bar", "fooBbar", {"X": "B"}))
    self.assertFalse(self.matches("foo[[X]]bar", "foobar", {"X": "A"}))
    self.assertFalse(self.matches("foo[[X]]bar", "foo bar", {"X": "A"}))
    with self.assertRaises(CheckerException):
      self.assertTrue(self.matches("foo[[X]]bar", "foobar", {}))

  def test_VariableDefinition(self):
    self.assertTrue(self.matches("foo[[X:A|B]]bar", "fooAbar"))
    self.assertTrue(self.matches("foo[[X:A|B]]bar", "fooBbar"))
    self.assertFalse(self.matches("foo[[X:A|B]]bar", "fooCbar"))

    env = self.tryMatch("foo[[X:A.*B]]bar", "fooABbar", {})
    self.assertEqual(env, {"X": "AB"})
    env = self.tryMatch("foo[[X:A.*B]]bar", "fooAxxBbar", {})
    self.assertEqual(env, {"X": "AxxB"})

    self.assertTrue(self.matches("foo[[X:A|B]]bar[[X]]baz", "fooAbarAbaz"))
    self.assertTrue(self.matches("foo[[X:A|B]]bar[[X]]baz", "fooBbarBbaz"))
    self.assertFalse(self.matches("foo[[X:A|B]]bar[[X]]baz", "fooAbarBbaz"))

  def test_NoVariableRedefinition(self):
    with self.assertRaises(CheckerException):
      self.matches("[[X:...]][[X]][[X:...]][[X]]", "foofoobarbar")

  def test_EnvNotChangedOnPartialMatch(self):
    env = {"Y": "foo"}
    self.assertFalse(self.matches("[[X:A]]bar", "Abaz", env))
    self.assertFalse("X" in env.keys())

  def test_VariableContentEscaped(self):
    self.assertTrue(self.matches("[[X:..]]foo[[X]]", ".*foo.*"))
    self.assertFalse(self.matches("[[X:..]]foo[[X]]", ".*fooAAAA"))


class MatchFiles_Test(unittest.TestCase):

  def matches(self, checkerString, c1String):
    checkerString = \
      """
        // CHECK-START: MyMethod MyPass
      """ + checkerString
    c1String = \
      """
        begin_compilation
          name "MyMethod"
          method "MyMethod"
          date 1234
        end_compilation
        begin_cfg
          name "MyPass"
      """ + c1String + \
      """
        end_cfg
      """
    checkerFile = ParseCheckerStream("<test-file>", "CHECK", io.StringIO(ToUnicode(checkerString)))
    c1File = ParseC1visualizerStream("<c1-file>", io.StringIO(ToUnicode(c1String)))
    try:
      MatchFiles(checkerFile, c1File)
      return True
    except CheckerException:
      return False

  def test_Text(self):
    self.assertTrue(self.matches( "// CHECK: foo bar", "foo bar"))
    self.assertFalse(self.matches("// CHECK: foo bar", "abc def"))

  def test_Pattern(self):
    self.assertTrue(self.matches( "// CHECK: abc {{de.}}", "abc de#"))
    self.assertFalse(self.matches("// CHECK: abc {{de.}}", "abc d#f"))

  def test_Variables(self):
    self.assertTrue(self.matches(
    """
      // CHECK: foo[[X:.]]bar
      // CHECK: abc[[X]]def
    """,
    """
      foo bar
      abc def
    """))
    self.assertTrue(self.matches(
    """
      // CHECK: foo[[X:([0-9]+)]]bar
      // CHECK: abc[[X]]def
      // CHECK: ### [[X]] ###
    """,
    """
      foo1234bar
      abc1234def
      ### 1234 ###
    """))
    self.assertFalse(self.matches(
    """
      // CHECK: foo[[X:([0-9]+)]]bar
      // CHECK: abc[[X]]def
    """,
    """
      foo1234bar
      abc1235def
    """))

  def test_InOrderAssertions(self):
    self.assertTrue(self.matches(
    """
      // CHECK: foo
      // CHECK: bar
    """,
    """
      foo
      bar
    """))
    self.assertFalse(self.matches(
    """
      // CHECK: foo
      // CHECK: bar
    """,
    """
      bar
      foo
    """))

  def test_DagAssertions(self):
    self.assertTrue(self.matches(
    """
      // CHECK-DAG: foo
      // CHECK-DAG: bar
    """,
    """
      foo
      bar
    """))
    self.assertTrue(self.matches(
    """
      // CHECK-DAG: foo
      // CHECK-DAG: bar
    """,
    """
      bar
      foo
    """))

  def test_DagAssertionsScope(self):
    self.assertTrue(self.matches(
    """
      // CHECK:     foo
      // CHECK-DAG: abc
      // CHECK-DAG: def
      // CHECK:     bar
    """,
    """
      foo
      def
      abc
      bar
    """))
    self.assertFalse(self.matches(
    """
      // CHECK:     foo
      // CHECK-DAG: abc
      // CHECK-DAG: def
      // CHECK:     bar
    """,
    """
      foo
      abc
      bar
      def
    """))
    self.assertFalse(self.matches(
    """
      // CHECK:     foo
      // CHECK-DAG: abc
      // CHECK-DAG: def
      // CHECK:     bar
    """,
    """
      foo
      def
      bar
      abc
    """))

  def test_NotAssertions(self):
    self.assertTrue(self.matches(
    """
      // CHECK-NOT: foo
    """,
    """
      abc
      def
    """))
    self.assertFalse(self.matches(
    """
      // CHECK-NOT: foo
    """,
    """
      abc foo
      def
    """))
    self.assertFalse(self.matches(
    """
      // CHECK-NOT: foo
      // CHECK-NOT: bar
    """,
    """
      abc
      def bar
    """))

  def test_NotAssertionsScope(self):
    self.assertTrue(self.matches(
    """
      // CHECK:     abc
      // CHECK-NOT: foo
      // CHECK:     def
    """,
    """
      abc
      def
    """))
    self.assertTrue(self.matches(
    """
      // CHECK:     abc
      // CHECK-NOT: foo
      // CHECK:     def
    """,
    """
      abc
      def
      foo
    """))
    self.assertFalse(self.matches(
    """
      // CHECK:     abc
      // CHECK-NOT: foo
      // CHECK:     def
    """,
    """
      abc
      foo
      def
    """))

  def test_LineOnlyMatchesOnce(self):
    self.assertTrue(self.matches(
    """
      // CHECK-DAG: foo
      // CHECK-DAG: foo
    """,
    """
      foo
      abc
      foo
    """))
    self.assertFalse(self.matches(
    """
      // CHECK-DAG: foo
      // CHECK-DAG: foo
    """,
    """
      foo
      abc
      bar
    """))
