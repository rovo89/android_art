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

from common.testing             import ToUnicode
from file_format.checker.parser import ParseCheckerStream
from file_format.checker.struct import CheckerFile, TestCase, TestAssertion, RegexExpression

import io
import unittest

CheckerException = SystemExit

class CheckerParser_PrefixTest(unittest.TestCase):

  def tryParse(self, string):
    checkerText = u"// CHECK-START: pass\n" + ToUnicode(string)
    checkFile = ParseCheckerStream("<test-file>", "CHECK", io.StringIO(checkerText))
    self.assertEqual(len(checkFile.testCases), 1)
    testCase = checkFile.testCases[0]
    return len(testCase.assertions) != 0

  def test_InvalidFormat(self):
    self.assertFalse(self.tryParse("CHECK"))
    self.assertFalse(self.tryParse(":CHECK"))
    self.assertFalse(self.tryParse("CHECK:"))
    self.assertFalse(self.tryParse("//CHECK"))
    self.assertFalse(self.tryParse("#CHECK"))

    self.assertTrue(self.tryParse("//CHECK:foo"))
    self.assertTrue(self.tryParse("#CHECK:bar"))

  def test_InvalidLabel(self):
    self.assertFalse(self.tryParse("//ACHECK:foo"))
    self.assertFalse(self.tryParse("#ACHECK:foo"))

  def test_NotFirstOnTheLine(self):
    self.assertFalse(self.tryParse("A// CHECK: foo"))
    self.assertFalse(self.tryParse("A # CHECK: foo"))
    self.assertFalse(self.tryParse("// // CHECK: foo"))
    self.assertFalse(self.tryParse("# # CHECK: foo"))

  def test_WhitespaceAgnostic(self):
    self.assertTrue(self.tryParse("  //CHECK: foo"))
    self.assertTrue(self.tryParse("//  CHECK: foo"))
    self.assertTrue(self.tryParse("    //CHECK: foo"))
    self.assertTrue(self.tryParse("//    CHECK: foo"))


class CheckerParser_RegexExpressionTest(unittest.TestCase):

  def parseAssertion(self, string, variant=""):
    checkerText = u"// CHECK-START: pass\n// CHECK" + ToUnicode(variant) + u": " + ToUnicode(string)
    checkerFile = ParseCheckerStream("<test-file>", "CHECK", io.StringIO(checkerText))
    self.assertEqual(len(checkerFile.testCases), 1)
    testCase = checkerFile.testCases[0]
    self.assertEqual(len(testCase.assertions), 1)
    return testCase.assertions[0]

  def parseExpression(self, string):
    line = self.parseAssertion(string)
    self.assertEqual(1, len(line.expressions))
    return line.expressions[0]

  def assertEqualsRegex(self, string, expected):
    self.assertEqual(expected, self.parseAssertion(string).toRegex())

  def assertEqualsText(self, string, text):
    self.assertEqual(self.parseExpression(string), RegexExpression.createText(text))

  def assertEqualsPattern(self, string, pattern):
    self.assertEqual(self.parseExpression(string), RegexExpression.createPattern(pattern))

  def assertEqualsVarRef(self, string, name):
    self.assertEqual(self.parseExpression(string), RegexExpression.createVariableReference(name))

  def assertEqualsVarDef(self, string, name, pattern):
    self.assertEqual(self.parseExpression(string),
                     RegexExpression.createVariableDefinition(name, pattern))

  def assertVariantNotEqual(self, string, variant):
    self.assertNotEqual(variant, self.parseExpression(string).variant)

  # Test that individual parts of the line are recognized

  def test_TextOnly(self):
    self.assertEqualsText("foo", "foo")
    self.assertEqualsText("  foo  ", "foo")
    self.assertEqualsRegex("f$o^o", "(f\$o\^o)")

  def test_PatternOnly(self):
    self.assertEqualsPattern("{{a?b.c}}", "a?b.c")

  def test_VarRefOnly(self):
    self.assertEqualsVarRef("[[ABC]]", "ABC")

  def test_VarDefOnly(self):
    self.assertEqualsVarDef("[[ABC:a?b.c]]", "ABC", "a?b.c")

  def test_TextWithWhitespace(self):
    self.assertEqualsRegex("foo bar", "(foo), (bar)")
    self.assertEqualsRegex("foo   bar", "(foo), (bar)")

  def test_TextWithRegex(self):
    self.assertEqualsRegex("foo{{abc}}bar", "(foo)(abc)(bar)")

  def test_TextWithVar(self):
    self.assertEqualsRegex("foo[[ABC:abc]]bar", "(foo)(abc)(bar)")

  def test_PlainWithRegexAndWhitespaces(self):
    self.assertEqualsRegex("foo {{abc}}bar", "(foo), (abc)(bar)")
    self.assertEqualsRegex("foo{{abc}} bar", "(foo)(abc), (bar)")
    self.assertEqualsRegex("foo {{abc}} bar", "(foo), (abc), (bar)")

  def test_PlainWithVarAndWhitespaces(self):
    self.assertEqualsRegex("foo [[ABC:abc]]bar", "(foo), (abc)(bar)")
    self.assertEqualsRegex("foo[[ABC:abc]] bar", "(foo)(abc), (bar)")
    self.assertEqualsRegex("foo [[ABC:abc]] bar", "(foo), (abc), (bar)")

  def test_AllKinds(self):
    self.assertEqualsRegex("foo [[ABC:abc]]{{def}}bar", "(foo), (abc)(def)(bar)")
    self.assertEqualsRegex("foo[[ABC:abc]] {{def}}bar", "(foo)(abc), (def)(bar)")
    self.assertEqualsRegex("foo [[ABC:abc]] {{def}} bar", "(foo), (abc), (def), (bar)")

  # # Test that variables and patterns are parsed correctly

  def test_ValidPattern(self):
    self.assertEqualsPattern("{{abc}}", "abc")
    self.assertEqualsPattern("{{a[b]c}}", "a[b]c")
    self.assertEqualsPattern("{{(a{bc})}}", "(a{bc})")

  def test_ValidRef(self):
    self.assertEqualsVarRef("[[ABC]]", "ABC")
    self.assertEqualsVarRef("[[A1BC2]]", "A1BC2")

  def test_ValidDef(self):
    self.assertEqualsVarDef("[[ABC:abc]]", "ABC", "abc")
    self.assertEqualsVarDef("[[ABC:ab:c]]", "ABC", "ab:c")
    self.assertEqualsVarDef("[[ABC:a[b]c]]", "ABC", "a[b]c")
    self.assertEqualsVarDef("[[ABC:(a[bc])]]", "ABC", "(a[bc])")

  def test_Empty(self):
    self.assertVariantNotEqual("{{}}", RegexExpression.Variant.Pattern)
    self.assertVariantNotEqual("[[]]", RegexExpression.Variant.VarRef)
    self.assertVariantNotEqual("[[:]]", RegexExpression.Variant.VarDef)

  def test_InvalidVarName(self):
    self.assertVariantNotEqual("[[0ABC]]", RegexExpression.Variant.VarRef)
    self.assertVariantNotEqual("[[AB=C]]", RegexExpression.Variant.VarRef)
    self.assertVariantNotEqual("[[ABC=]]", RegexExpression.Variant.VarRef)
    self.assertVariantNotEqual("[[0ABC:abc]]", RegexExpression.Variant.VarDef)
    self.assertVariantNotEqual("[[AB=C:abc]]", RegexExpression.Variant.VarDef)
    self.assertVariantNotEqual("[[ABC=:abc]]", RegexExpression.Variant.VarDef)

  def test_BodyMatchNotGreedy(self):
    self.assertEqualsRegex("{{abc}}{{def}}", "(abc)(def)")
    self.assertEqualsRegex("[[ABC:abc]][[DEF:def]]", "(abc)(def)")

  def test_NoVarDefsInNotChecks(self):
    with self.assertRaises(CheckerException):
      self.parseAssertion("[[ABC:abc]]", "-NOT")


class CheckerParser_FileLayoutTest(unittest.TestCase):

  # Creates an instance of CheckerFile from provided info.
  # Data format: [ ( <case-name>, [ ( <text>, <assert-variant> ), ... ] ), ... ]
  def createFile(self, caseList):
    testFile = CheckerFile("<test_file>")
    for caseEntry in caseList:
      caseName = caseEntry[0]
      testCase = TestCase(testFile, caseName, 0)
      assertionList = caseEntry[1]
      for assertionEntry in assertionList:
        content = assertionEntry[0]
        variant = assertionEntry[1]
        assertion = TestAssertion(testCase, variant, content, 0)
        assertion.addExpression(RegexExpression.createText(content))
    return testFile

  def assertParsesTo(self, checkerText, expectedData):
    expectedFile = self.createFile(expectedData)
    actualFile = ParseCheckerStream("<test_file>", "CHECK", io.StringIO(ToUnicode(checkerText)))
    return self.assertEqual(expectedFile, actualFile)

  def test_EmptyFile(self):
    self.assertParsesTo("", [])

  def test_SingleGroup(self):
    self.assertParsesTo(
      """
        // CHECK-START: Example Group
        // CHECK:  foo
        // CHECK:    bar
      """,
      [ ( "Example Group", [ ("foo", TestAssertion.Variant.InOrder),
                             ("bar", TestAssertion.Variant.InOrder) ] ) ])

  def test_MultipleGroups(self):
    self.assertParsesTo(
      """
        // CHECK-START: Example Group1
        // CHECK: foo
        // CHECK: bar
        // CHECK-START: Example Group2
        // CHECK: abc
        // CHECK: def
      """,
      [ ( "Example Group1", [ ("foo", TestAssertion.Variant.InOrder),
                              ("bar", TestAssertion.Variant.InOrder) ] ),
        ( "Example Group2", [ ("abc", TestAssertion.Variant.InOrder),
                              ("def", TestAssertion.Variant.InOrder) ] ) ])

  def test_AssertionVariants(self):
    self.assertParsesTo(
      """
        // CHECK-START: Example Group
        // CHECK:     foo
        // CHECK-NOT: bar
        // CHECK-DAG: abc
        // CHECK-DAG: def
      """,
      [ ( "Example Group", [ ("foo", TestAssertion.Variant.InOrder),
                             ("bar", TestAssertion.Variant.Not),
                             ("abc", TestAssertion.Variant.DAG),
                             ("def", TestAssertion.Variant.DAG) ] ) ])
