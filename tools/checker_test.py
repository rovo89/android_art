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

# This is a test file which exercises all feautres supported by the domain-
# specific markup language implemented by Checker.

import checker
import io
import unittest

# The parent type of exception expected to be thrown by Checker during tests.
# It must be specific enough to not cover exceptions thrown due to actual flaws
# in Checker.
CheckerException = SystemExit


class TestCheckFile_PrefixExtraction(unittest.TestCase):
  def __tryParse(self, string):
    checkFile = checker.CheckFile(None, [])
    return checkFile._extractLine("CHECK", string)

  def test_InvalidFormat(self):
    self.assertIsNone(self.__tryParse("CHECK"))
    self.assertIsNone(self.__tryParse(":CHECK"))
    self.assertIsNone(self.__tryParse("CHECK:"))
    self.assertIsNone(self.__tryParse("//CHECK"))
    self.assertIsNone(self.__tryParse("#CHECK"))

    self.assertIsNotNone(self.__tryParse("//CHECK:foo"))
    self.assertIsNotNone(self.__tryParse("#CHECK:bar"))

  def test_InvalidLabel(self):
    self.assertIsNone(self.__tryParse("//ACHECK:foo"))
    self.assertIsNone(self.__tryParse("#ACHECK:foo"))

  def test_NotFirstOnTheLine(self):
    self.assertIsNone(self.__tryParse("A// CHECK: foo"))
    self.assertIsNone(self.__tryParse("A # CHECK: foo"))
    self.assertIsNone(self.__tryParse("// // CHECK: foo"))
    self.assertIsNone(self.__tryParse("# # CHECK: foo"))

  def test_WhitespaceAgnostic(self):
    self.assertIsNotNone(self.__tryParse("  //CHECK: foo"))
    self.assertIsNotNone(self.__tryParse("//  CHECK: foo"))
    self.assertIsNotNone(self.__tryParse("    //CHECK: foo"))
    self.assertIsNotNone(self.__tryParse("//    CHECK: foo"))


class TestCheckLine_Parse(unittest.TestCase):
  def __getPartPattern(self, linePart):
    if linePart.variant == checker.CheckElement.Variant.Separator:
      return "\s+"
    else:
      return linePart.pattern

  def __getRegex(self, checkLine):
    return "".join(map(lambda x: "(" + self.__getPartPattern(x) + ")", checkLine.lineParts))

  def __tryParse(self, string):
    return checker.CheckLine(string)

  def __parsesTo(self, string, expected):
    self.assertEqual(expected, self.__getRegex(self.__tryParse(string)))

  def __tryParseNot(self, string):
    return checker.CheckLine(string, checker.CheckLine.Variant.Not)

  def __parsesPattern(self, string, pattern):
    line = self.__tryParse(string)
    self.assertEqual(1, len(line.lineParts))
    self.assertEqual(checker.CheckElement.Variant.Pattern, line.lineParts[0].variant)
    self.assertEqual(pattern, line.lineParts[0].pattern)

  def __parsesVarRef(self, string, name):
    line = self.__tryParse(string)
    self.assertEqual(1, len(line.lineParts))
    self.assertEqual(checker.CheckElement.Variant.VarRef, line.lineParts[0].variant)
    self.assertEqual(name, line.lineParts[0].name)

  def __parsesVarDef(self, string, name, body):
    line = self.__tryParse(string)
    self.assertEqual(1, len(line.lineParts))
    self.assertEqual(checker.CheckElement.Variant.VarDef, line.lineParts[0].variant)
    self.assertEqual(name, line.lineParts[0].name)
    self.assertEqual(body, line.lineParts[0].pattern)

  def __doesNotParse(self, string, partType):
    line = self.__tryParse(string)
    self.assertEqual(1, len(line.lineParts))
    self.assertNotEqual(partType, line.lineParts[0].variant)

  # Test that individual parts of the line are recognized

  def test_TextOnly(self):
    self.__parsesTo("foo", "(foo)")
    self.__parsesTo("  foo  ", "(foo)")
    self.__parsesTo("f$o^o", "(f\$o\^o)")

  def test_TextWithWhitespace(self):
    self.__parsesTo("foo bar", "(foo)(\s+)(bar)")
    self.__parsesTo("foo   bar", "(foo)(\s+)(bar)")

  def test_RegexOnly(self):
    self.__parsesPattern("{{a?b.c}}", "a?b.c")

  def test_VarRefOnly(self):
    self.__parsesVarRef("[[ABC]]", "ABC")

  def test_VarDefOnly(self):
    self.__parsesVarDef("[[ABC:a?b.c]]", "ABC", "a?b.c")

  def test_TextWithRegex(self):
    self.__parsesTo("foo{{abc}}bar", "(foo)(abc)(bar)")

  def test_TextWithVar(self):
    self.__parsesTo("foo[[ABC:abc]]bar", "(foo)(abc)(bar)")

  def test_PlainWithRegexAndWhitespaces(self):
    self.__parsesTo("foo {{abc}}bar", "(foo)(\s+)(abc)(bar)")
    self.__parsesTo("foo{{abc}} bar", "(foo)(abc)(\s+)(bar)")
    self.__parsesTo("foo {{abc}} bar", "(foo)(\s+)(abc)(\s+)(bar)")

  def test_PlainWithVarAndWhitespaces(self):
    self.__parsesTo("foo [[ABC:abc]]bar", "(foo)(\s+)(abc)(bar)")
    self.__parsesTo("foo[[ABC:abc]] bar", "(foo)(abc)(\s+)(bar)")
    self.__parsesTo("foo [[ABC:abc]] bar", "(foo)(\s+)(abc)(\s+)(bar)")

  def test_AllKinds(self):
    self.__parsesTo("foo [[ABC:abc]]{{def}}bar", "(foo)(\s+)(abc)(def)(bar)")
    self.__parsesTo("foo[[ABC:abc]] {{def}}bar", "(foo)(abc)(\s+)(def)(bar)")
    self.__parsesTo("foo [[ABC:abc]] {{def}} bar", "(foo)(\s+)(abc)(\s+)(def)(\s+)(bar)")

  # Test that variables and patterns are parsed correctly

  def test_ValidPattern(self):
    self.__parsesPattern("{{abc}}", "abc")
    self.__parsesPattern("{{a[b]c}}", "a[b]c")
    self.__parsesPattern("{{(a{bc})}}", "(a{bc})")

  def test_ValidRef(self):
    self.__parsesVarRef("[[ABC]]", "ABC")
    self.__parsesVarRef("[[A1BC2]]", "A1BC2")

  def test_ValidDef(self):
    self.__parsesVarDef("[[ABC:abc]]", "ABC", "abc")
    self.__parsesVarDef("[[ABC:ab:c]]", "ABC", "ab:c")
    self.__parsesVarDef("[[ABC:a[b]c]]", "ABC", "a[b]c")
    self.__parsesVarDef("[[ABC:(a[bc])]]", "ABC", "(a[bc])")

  def test_Empty(self):
    self.__doesNotParse("{{}}", checker.CheckElement.Variant.Pattern)
    self.__doesNotParse("[[]]", checker.CheckElement.Variant.VarRef)
    self.__doesNotParse("[[:]]", checker.CheckElement.Variant.VarDef)

  def test_InvalidVarName(self):
    self.__doesNotParse("[[0ABC]]", checker.CheckElement.Variant.VarRef)
    self.__doesNotParse("[[AB=C]]", checker.CheckElement.Variant.VarRef)
    self.__doesNotParse("[[ABC=]]", checker.CheckElement.Variant.VarRef)
    self.__doesNotParse("[[0ABC:abc]]", checker.CheckElement.Variant.VarDef)
    self.__doesNotParse("[[AB=C:abc]]", checker.CheckElement.Variant.VarDef)
    self.__doesNotParse("[[ABC=:abc]]", checker.CheckElement.Variant.VarDef)

  def test_BodyMatchNotGreedy(self):
    self.__parsesTo("{{abc}}{{def}}", "(abc)(def)")
    self.__parsesTo("[[ABC:abc]][[DEF:def]]", "(abc)(def)")

  def test_NoVarDefsInNotChecks(self):
    with self.assertRaises(CheckerException):
      self.__tryParseNot("[[ABC:abc]]")

class TestCheckLine_Match(unittest.TestCase):
  def __matchSingle(self, checkString, outputString, varState={}):
    checkLine = checker.CheckLine(checkString)
    newVarState = checkLine.match(outputString, varState)
    self.assertIsNotNone(newVarState)
    return newVarState

  def __notMatchSingle(self, checkString, outputString, varState={}):
    checkLine = checker.CheckLine(checkString)
    self.assertIsNone(checkLine.match(outputString, varState))

  def test_TextAndWhitespace(self):
    self.__matchSingle("foo", "foo")
    self.__matchSingle("foo", "  foo  ")
    self.__matchSingle("foo", "foo bar")
    self.__notMatchSingle("foo", "XfooX")
    self.__notMatchSingle("foo", "zoo")

    self.__matchSingle("foo bar", "foo   bar")
    self.__matchSingle("foo bar", "abc foo bar def")
    self.__matchSingle("foo bar", "foo foo bar bar")

    self.__matchSingle("foo bar", "foo X bar")
    self.__notMatchSingle("foo bar", "foo Xbar")

  def test_Pattern(self):
    self.__matchSingle("foo{{A|B}}bar", "fooAbar")
    self.__matchSingle("foo{{A|B}}bar", "fooBbar")
    self.__notMatchSingle("foo{{A|B}}bar", "fooCbar")

  def test_VariableReference(self):
    self.__matchSingle("foo[[X]]bar", "foobar", {"X": ""})
    self.__matchSingle("foo[[X]]bar", "fooAbar", {"X": "A"})
    self.__matchSingle("foo[[X]]bar", "fooBbar", {"X": "B"})
    self.__notMatchSingle("foo[[X]]bar", "foobar", {"X": "A"})
    self.__notMatchSingle("foo[[X]]bar", "foo bar", {"X": "A"})
    with self.assertRaises(CheckerException):
      self.__matchSingle("foo[[X]]bar", "foobar", {})

  def test_VariableDefinition(self):
    self.__matchSingle("foo[[X:A|B]]bar", "fooAbar")
    self.__matchSingle("foo[[X:A|B]]bar", "fooBbar")
    self.__notMatchSingle("foo[[X:A|B]]bar", "fooCbar")

    env = self.__matchSingle("foo[[X:A.*B]]bar", "fooABbar", {})
    self.assertEqual(env, {"X": "AB"})
    env = self.__matchSingle("foo[[X:A.*B]]bar", "fooAxxBbar", {})
    self.assertEqual(env, {"X": "AxxB"})

    self.__matchSingle("foo[[X:A|B]]bar[[X]]baz", "fooAbarAbaz")
    self.__matchSingle("foo[[X:A|B]]bar[[X]]baz", "fooBbarBbaz")
    self.__notMatchSingle("foo[[X:A|B]]bar[[X]]baz", "fooAbarBbaz")

  def test_NoVariableRedefinition(self):
    with self.assertRaises(CheckerException):
      self.__matchSingle("[[X:...]][[X]][[X:...]][[X]]", "foofoobarbar")

  def test_EnvNotChangedOnPartialMatch(self):
    env = {"Y": "foo"}
    self.__notMatchSingle("[[X:A]]bar", "Abaz", env)
    self.assertFalse("X" in env.keys())

  def test_VariableContentEscaped(self):
    self.__matchSingle("[[X:..]]foo[[X]]", ".*foo.*")
    self.__notMatchSingle("[[X:..]]foo[[X]]", ".*fooAAAA")


CheckVariant = checker.CheckLine.Variant

def prepareSingleCheck(line):
  if isinstance(line, str):
    return checker.CheckLine(line)
  else:
    return checker.CheckLine(line[0], line[1])

def prepareChecks(lines):
  if isinstance(lines, str):
    lines = lines.splitlines()
  return list(map(lambda line: prepareSingleCheck(line), lines))


class TestCheckGroup_Match(unittest.TestCase):
  def __matchMulti(self, checkLines, outputString):
    checkGroup = checker.CheckGroup("MyGroup", prepareChecks(checkLines))
    outputGroup = checker.OutputGroup("MyGroup", outputString.splitlines())
    return checkGroup.match(outputGroup)

  def __notMatchMulti(self, checkString, outputString):
    with self.assertRaises(CheckerException):
      self.__matchMulti(checkString, outputString)

  def test_TextAndPattern(self):
    self.__matchMulti("""foo bar
                         abc {{def}}""",
                      """foo bar
                         abc def""");
    self.__matchMulti("""foo bar
                         abc {{de.}}""",
                      """=======
                         foo bar
                         =======
                         abc de#
                         =======""");
    self.__notMatchMulti("""//XYZ: foo bar
                            //XYZ: abc {{def}}""",
                         """=======
                            foo bar
                            =======
                            abc de#
                            =======""");

  def test_Variables(self):
    self.__matchMulti("""foo[[X:.]]bar
                         abc[[X]]def""",
                      """foo bar
                         abc def""");
    self.__matchMulti("""foo[[X:([0-9]+)]]bar
                         abc[[X]]def
                         ### [[X]] ###""",
                      """foo1234bar
                         abc1234def
                         ### 1234 ###""");

  def test_Ordering(self):
    self.__matchMulti([("foo", CheckVariant.InOrder),
                       ("bar", CheckVariant.InOrder)],
                      """foo
                         bar""")
    self.__notMatchMulti([("foo", CheckVariant.InOrder),
                          ("bar", CheckVariant.InOrder)],
                         """bar
                            foo""")
    self.__matchMulti([("abc", CheckVariant.DAG),
                       ("def", CheckVariant.DAG)],
                      """abc
                         def""")
    self.__matchMulti([("abc", CheckVariant.DAG),
                       ("def", CheckVariant.DAG)],
                      """def
                         abc""")
    self.__matchMulti([("foo", CheckVariant.InOrder),
                       ("abc", CheckVariant.DAG),
                       ("def", CheckVariant.DAG),
                       ("bar", CheckVariant.InOrder)],
                      """foo
                         def
                         abc
                         bar""")
    self.__notMatchMulti([("foo", CheckVariant.InOrder),
                          ("abc", CheckVariant.DAG),
                          ("def", CheckVariant.DAG),
                          ("bar", CheckVariant.InOrder)],
                         """foo
                            abc
                            bar""")
    self.__notMatchMulti([("foo", CheckVariant.InOrder),
                          ("abc", CheckVariant.DAG),
                          ("def", CheckVariant.DAG),
                          ("bar", CheckVariant.InOrder)],
                         """foo
                            def
                            bar""")

  def test_NotAssertions(self):
    self.__matchMulti([("foo", CheckVariant.Not)],
                      """abc
                         def""")
    self.__notMatchMulti([("foo", CheckVariant.Not)],
                         """abc foo
                            def""")
    self.__notMatchMulti([("foo", CheckVariant.Not),
                          ("bar", CheckVariant.Not)],
                         """abc
                            def bar""")

  def test_LineOnlyMatchesOnce(self):
    self.__matchMulti([("foo", CheckVariant.DAG),
                       ("foo", CheckVariant.DAG)],
                       """foo
                          foo""")
    self.__notMatchMulti([("foo", CheckVariant.DAG),
                          ("foo", CheckVariant.DAG)],
                          """foo
                             bar""")

class TestOutputFile_Parse(unittest.TestCase):
  def __parsesTo(self, string, expected):
    if isinstance(string, str):
      string = unicode(string)
    outputStream = io.StringIO(string)
    return self.assertEqual(checker.OutputFile(outputStream).groups, expected)

  def test_NoInput(self):
    self.__parsesTo(None, [])
    self.__parsesTo("", [])

  def test_SingleGroup(self):
    self.__parsesTo("""begin_compilation
                         method "MyMethod"
                       end_compilation
                       begin_cfg
                         name "pass1"
                         foo
                         bar
                       end_cfg""",
                    [ checker.OutputGroup("MyMethod pass1", [ "foo", "bar" ]) ])

  def test_MultipleGroups(self):
    self.__parsesTo("""begin_compilation
                         name "xyz1"
                         method "MyMethod1"
                         date 1234
                       end_compilation
                       begin_cfg
                         name "pass1"
                         foo
                         bar
                       end_cfg
                       begin_cfg
                         name "pass2"
                         abc
                         def
                       end_cfg""",
                    [ checker.OutputGroup("MyMethod1 pass1", [ "foo", "bar" ]),
                      checker.OutputGroup("MyMethod1 pass2", [ "abc", "def" ]) ])

    self.__parsesTo("""begin_compilation
                         name "xyz1"
                         method "MyMethod1"
                         date 1234
                       end_compilation
                       begin_cfg
                         name "pass1"
                         foo
                         bar
                       end_cfg
                       begin_compilation
                         name "xyz2"
                         method "MyMethod2"
                         date 5678
                       end_compilation
                       begin_cfg
                         name "pass2"
                         abc
                         def
                       end_cfg""",
                    [ checker.OutputGroup("MyMethod1 pass1", [ "foo", "bar" ]),
                      checker.OutputGroup("MyMethod2 pass2", [ "abc", "def" ]) ])

class TestCheckFile_Parse(unittest.TestCase):
  def __parsesTo(self, string, expected):
    if isinstance(string, str):
      string = unicode(string)
    checkStream = io.StringIO(string)
    return self.assertEqual(checker.CheckFile("CHECK", checkStream).groups, expected)

  def test_NoInput(self):
    self.__parsesTo(None, [])
    self.__parsesTo("", [])

  def test_SingleGroup(self):
    self.__parsesTo("""// CHECK-START: Example Group
                       // CHECK:  foo
                       // CHECK:    bar""",
                    [ checker.CheckGroup("Example Group", prepareChecks([ "foo", "bar" ])) ])

  def test_MultipleGroups(self):
    self.__parsesTo("""// CHECK-START: Example Group1
                       // CHECK: foo
                       // CHECK: bar
                       // CHECK-START: Example Group2
                       // CHECK: abc
                       // CHECK: def""",
                    [ checker.CheckGroup("Example Group1", prepareChecks([ "foo", "bar" ])),
                      checker.CheckGroup("Example Group2", prepareChecks([ "abc", "def" ])) ])

  def test_CheckVariants(self):
    self.__parsesTo("""// CHECK-START: Example Group
                       // CHECK:     foo
                       // CHECK-NOT: bar
                       // CHECK-DAG: abc
                       // CHECK-DAG: def""",
                    [ checker.CheckGroup("Example Group",
                                         prepareChecks([ ("foo", CheckVariant.InOrder),
                                                         ("bar", CheckVariant.Not),
                                                         ("abc", CheckVariant.DAG),
                                                         ("def", CheckVariant.DAG) ])) ])

if __name__ == '__main__':
  checker.Logger.Verbosity = checker.Logger.Level.NoOutput
  unittest.main()
