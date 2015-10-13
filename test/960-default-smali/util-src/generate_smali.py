#!/usr/bin/python3
#
# Copyright (C) 2015 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Generate Smali Main file for test 960
"""

import os
import sys
from pathlib import Path

BUILD_TOP = os.getenv("ANDROID_BUILD_TOP")
if BUILD_TOP is None:
  print("ANDROID_BUILD_TOP not set. Please run build/envsetup.sh", file=sys.stderr)
  sys.exit(1)

# Allow us to import utils and mixins.
sys.path.append(str(Path(BUILD_TOP)/"art"/"test"/"utils"/"python"))

from testgen.utils import get_copyright
import testgen.mixins as mixins

from collections import namedtuple
import itertools
import functools
import xml.etree.ElementTree as ET

class MainClass(mixins.DumpMixin, mixins.Named, mixins.SmaliFileMixin):
  """
  A mainclass and main method for this test.
  """

  MAIN_CLASS_TEMPLATE = """{copyright}
.class public LMain;
.super Ljava/lang/Object;

# class Main {{

.method public constructor <init>()V
    .registers 1
    invoke-direct {{p0}}, Ljava/lang/Object;-><init>()V
    return-void
.end method

{test_groups}

{test_funcs}

{main_func}

# }}
"""

  MAIN_FUNCTION_TEMPLATE = """
#   public static void main(String[] args) {{
.method public static main([Ljava/lang/String;)V
    .locals 2
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;

    {test_group_invoke}

    return-void
.end method
#   }}
"""

  TEST_GROUP_INVOKE_TEMPLATE = """
#     {test_name}();
    invoke-static {{}}, {test_name}()V
"""

  def __init__(self):
    """
    Initialize this MainClass
    """
    self.tests = set()
    self.global_funcs = set()

  def add_instance(self, it):
    """
    Add an instance test for the given class
    """
    self.tests.add(it)

  def add_func(self, f):
    """
    Add a function to the class
    """
    self.global_funcs.add(f)

  def get_name(self):
    """
    Get the name of this class
    """
    return "Main"

  def __str__(self):
    """
    Print this class
    """
    all_tests = sorted(self.tests)
    test_invoke = ""
    test_groups = ""
    for t in all_tests:
      test_groups += str(t)
    for t in sorted(all_tests):
      test_invoke += self.TEST_GROUP_INVOKE_TEMPLATE.format(test_name=t.get_name())
    main_func = self.MAIN_FUNCTION_TEMPLATE.format(test_group_invoke=test_invoke)

    funcs = ""
    for f in self.global_funcs:
      funcs += str(f)
    return self.MAIN_CLASS_TEMPLATE.format(copyright = get_copyright('smali'),
                                           test_groups=test_groups,
                                           main_func=main_func, test_funcs=funcs)


class InstanceTest(mixins.Named, mixins.NameComparableMixin):
  """
  A method that runs tests for a particular concrete type, It calls the test
  cases for running it in all possible ways.
  """

  INSTANCE_TEST_TEMPLATE = """
#   public static void {test_name}() {{
#     System.out.println("Testing for type {ty}");
#     String s = "{ty}";
#     {ty} v = new {ty}();
.method public static {test_name}()V
    .locals 3
    sget-object v2, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v0, "Testing for type {ty}"
    invoke-virtual {{v2,v0}}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    const-string v0, "{ty}"
    new-instance v1, L{ty};
    invoke-direct {{v1}}, L{ty};-><init>()V

    {invokes}

    const-string v0, "End testing for type {ty}"
    invoke-virtual {{v2,v0}}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    return-void
.end method
#     System.out.println("End testing for type {ty}");
#   }}
"""

  TEST_INVOKE_TEMPLATE = """
#     {fname}(s, v);
    invoke-static {{v0, v1}}, {fname}(Ljava/lang/String;L{farg};)V
"""

  def __init__(self, main, ty):
    """
    Initialize this test group for the given type
    """
    self.ty = ty
    self.main = main
    self.funcs = set()
    self.main.add_instance(self)

  def get_name(self):
    """
    Get the name of this test group
    """
    return "TEST_NAME_"+self.ty

  def add_func(self, f):
    """
    Add a test function to this test group
    """
    self.main.add_func(f)
    self.funcs.add(f)

  def __str__(self):
    """
    Returns the smali code for this function
    """
    func_invokes = ""
    for f in sorted(self.funcs, key=lambda a: (a.func, a.farg)):
      func_invokes += self.TEST_INVOKE_TEMPLATE.format(fname=f.get_name(),
                                                       farg=f.farg)

    return self.INSTANCE_TEST_TEMPLATE.format(test_name=self.get_name(), ty=self.ty,
                                              invokes=func_invokes)

class Func(mixins.Named, mixins.NameComparableMixin):
  """
  A single test case that attempts to invoke a function on receiver of a given type.
  """

  TEST_FUNCTION_TEMPLATE = """
#   public static void {fname}(String s, {farg} v) {{
#     try {{
#       System.out.printf("%s-{invoke_type:<9} {farg:>9}.{callfunc}()='%s'\\n", s, v.{callfunc}());
#       return;
#     }} catch (Error e) {{
#       System.out.printf("%s-{invoke_type} on {farg}: {callfunc}() threw exception!\\n", s);
#       e.printStackTrace(System.out);
#     }}
#   }}
.method public static {fname}(Ljava/lang/String;L{farg};)V
    .locals 7
    :call_{fname}_try_start
      const/4 v0, 2
      new-array v1,v0, [Ljava/lang/Object;
      const/4 v0, 0
      aput-object p0,v1,v0

      sget-object v2, Ljava/lang/System;->out:Ljava/io/PrintStream;
      const-string v3, "%s-{invoke_type:<9} {farg:>9}.{callfunc}()='%s'\\n"

      invoke-{invoke_type} {{p1}}, L{farg};->{callfunc}()Ljava/lang/String;
      move-result-object v4
      const/4 v0, 1
      aput-object v4, v1, v0

      invoke-virtual {{v2,v3,v1}}, Ljava/io/PrintStream;->printf(Ljava/lang/String;[Ljava/lang/Object;)Ljava/io/PrintStream;
      return-void
    :call_{fname}_try_end
    .catch Ljava/lang/Error; {{:call_{fname}_try_start .. :call_{fname}_try_end}} :error_{fname}_start
    :error_{fname}_start
      move-exception v3
      const/4 v0, 1
      new-array v1,v0, [Ljava/lang/Object;
      const/4 v0, 0
      aput-object p0, v1, v0
      sget-object v2, Ljava/lang/System;->out:Ljava/io/PrintStream;
      const-string v4, "%s-{invoke_type} on {farg}: {callfunc}() threw exception!\\n"
      invoke-virtual {{v2,v4,v1}}, Ljava/io/PrintStream;->printf(Ljava/lang/String;[Ljava/lang/Object;)Ljava/io/PrintStream;
      invoke-virtual {{v3,v2}}, Ljava/lang/Error;->printStackTrace(Ljava/io/PrintStream;)V
      return-void
.end method
"""

  def __init__(self, func, farg, invoke):
    """
    Initialize this test function for the given invoke type and argument
    """
    self.func = func
    self.farg = farg
    self.invoke = invoke

  def get_name(self):
    """
    Get the name of this test
    """
    return "Test_Func_{}_{}_{}".format(self.func, self.farg, self.invoke)

  def __str__(self):
    """
    Get the smali code for this test function
    """
    return self.TEST_FUNCTION_TEMPLATE.format(fname=self.get_name(),
                                              farg=self.farg,
                                              invoke_type=self.invoke,
                                              callfunc=self.func)

def flatten_classes(classes, c):
  """
  Iterate over all the classes 'c' can be used as
  """
  while c:
    yield c
    c = classes.get(c.super_class)

def flatten_class_methods(classes, c):
  """
  Iterate over all the methods 'c' can call
  """
  for c1 in flatten_classes(classes, c):
    yield from c1.methods

def flatten_interfaces(dat, c):
  """
  Iterate over all the interfaces 'c' transitively implements
  """
  def get_ifaces(cl):
    for i2 in cl.implements:
      yield dat.interfaces[i2]
      yield from get_ifaces(dat.interfaces[i2])

  for cl in flatten_classes(dat.classes, c):
    yield from get_ifaces(cl)

def flatten_interface_methods(dat, i):
  """
  Iterate over all the interface methods 'c' can call
  """
  yield from i.methods
  for i2 in flatten_interfaces(dat, i):
    yield from i2.methods

def make_main_class(dat):
  """
  Creates a Main.smali file that runs all the tests
  """
  m = MainClass()
  for c in dat.classes.values():
    i = InstanceTest(m, c.name)
    for clazz in flatten_classes(dat.classes, c):
      for meth in flatten_class_methods(dat.classes, clazz):
        i.add_func(Func(meth, clazz.name, 'virtual'))
      for iface in flatten_interfaces(dat, clazz):
        for meth in flatten_interface_methods(dat, iface):
          i.add_func(Func(meth, clazz.name, 'virtual'))
          i.add_func(Func(meth, iface.name, 'interface'))
  return m

class TestData(namedtuple("TestData", ['classes', 'interfaces'])):
  """
  A class representing the classes.xml document.
  """
  pass

class Clazz(namedtuple("Clazz", ["name", "methods", "super_class", "implements"])):
  """
  A class representing a class element in the classes.xml document.
  """
  pass

class IFace(namedtuple("IFace", ["name", "methods", "super_class", "implements"])):
  """
  A class representing an interface element in the classes.xml document.
  """
  pass

def parse_xml(xml):
  """
  Parse the xml description of this test.
  """
  classes = dict()
  ifaces  = dict()
  root = ET.fromstring(xml)
  for iface in root.find("interfaces"):
    name = iface.attrib['name']
    implements = [a.text for a in iface.find("implements")]
    methods = [a.text for a in iface.find("methods")]
    ifaces[name] = IFace(name = name,
                         super_class = iface.attrib['super'],
                         methods = methods,
                         implements = implements)
  for clazz in root.find('classes'):
    name = clazz.attrib['name']
    implements = [a.text for a in clazz.find("implements")]
    methods = [a.text for a in clazz.find("methods")]
    classes[name] = Clazz(name = name,
                          super_class = clazz.attrib['super'],
                          methods = methods,
                          implements = implements)
  return TestData(classes, ifaces)

def main(argv):
  smali_dir = Path(argv[1])
  if not smali_dir.exists() or not smali_dir.is_dir():
    print("{} is not a valid smali dir".format(smali_dir), file=sys.stderr)
    sys.exit(1)
  class_data = parse_xml((smali_dir / "classes.xml").open().read())
  make_main_class(class_data).dump(smali_dir)

if __name__ == '__main__':
  main(sys.argv)
