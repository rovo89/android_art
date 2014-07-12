# Copyright (C) 2014 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

all_tests = [
    ({'@INT@': 'int', '@SUFFIX@':''},
     [('CheckDiv', 'idiv_by_pow2_', [2**i for i in range(31)]),
      ('CheckDiv', 'idiv_by_small_', [i for i in range(3, 16) if i not in (4, 8)]),
      ('CheckRem', 'irem_by_pow2_', [2**i for i in range(31)])]),
    ({'@INT@': 'long', '@SUFFIX@': 'l'},
     [('CheckDiv', 'ldiv_by_pow2_', [2**i for i in range(63)]),
      ('CheckDiv', 'ldiv_by_small_', [i for i in range(3, 16) if i not in (4, 8)]),
      ('CheckRem', 'lrem_by_pow2_', [2**i for i in range(63)])])
]

def subst_vars(variables, text):
    '''Substitute variables in text.'''
    for key, value in variables.iteritems():
        text = text.replace(str(key), str(value))
    return text

# Generate all the function bodies (in decls) and all the function calls (in calls).
decls, calls = '', {}
for default_vars, tests in all_tests:
    local_vars = default_vars.copy()
    int_type = local_vars['@INT@']
    for checker, name, values in tests:
        local_vars['@CHECKER@'] = checker
        for i, value in enumerate(values):
            local_vars['@NAME@'] = name + str(i)
            local_vars['@VALUE@'] = value
            local_vars['@OP@'] = '/' if 'div' in name else '%'

            # Function body.
            decls += subst_vars(local_vars, '''
    public static @INT@ @NAME@(@INT@ x) {return x @OP@ @VALUE@@SUFFIX@;}''')

            # Function call and test.
            calls[int_type] = calls.get(int_type, '') + subst_vars(local_vars, '''
        @INT@@CHECKER@("@NAME@", @NAME@(x), x, @VALUE@@SUFFIX@);''')

# Generate the checkers.
checkers = ''
local_vars = {}
for int_type in ('int', 'long'):
    local_vars['@INT@'] = int_type
    for op, op_name in (('/', 'Div'), ('%', 'Rem')):
        local_vars['@OP@'] = op
        local_vars['@OP_NAME@'] = op_name
        checkers += subst_vars(local_vars, '''
    public static void @INT@Check@OP_NAME@(String desc, @INT@ result, @INT@ dividend, @INT@ divisor) {
        @INT@ correct_result = dividend @OP@ divisor;
        if (result != correct_result) {
            reportError(desc + "(" + dividend + ") == " + result +
                        " should be " + correct_result);
        }
    }''')


code = \
'''/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

public class Main {
    public static int num_errors = 0;

    public static void reportError(String message) {
        if (num_errors == 10) {
            System.out.println("Omitting other error messages...");
        } else if (num_errors < 10) {
            System.out.println(message);
        }
        num_errors += 1;
    }
%s
%s

    public static void intCheckAll(int x) {%s
    }

    public static void longCheckAll(long x) {%s
    }

    public static void main(String[] args) {
      int i;
      long l;

      System.out.println("Begin");

      System.out.println("Int: checking some equally spaced dividends...");
      for (i = -1000; i < 1000; i += 300) {
          intCheckAll(i);
          intCheckAll(-i);
      }

      System.out.println("Int: checking small dividends...");
      for (i = 1; i < 100; i += 1) {
          intCheckAll(i);
          intCheckAll(-i);
      }

      System.out.println("Int: checking big dividends...");
      for (i = 0; i < 100; i += 1) {
          intCheckAll(Integer.MAX_VALUE - i);
          intCheckAll(Integer.MIN_VALUE + i);
      }

      System.out.println("Long: checking some equally spaced dividends...");
      for (l = 0l; l < 1000000000000l; l += 300000000000l) {
          longCheckAll(l);
          longCheckAll(-l);
      }

      System.out.println("Long: checking small dividends...");
      for (l = 1l; l < 100l; l += 1l) {
          longCheckAll(l);
          longCheckAll(-l);
      }

      System.out.println("Long: checking big dividends...");
      for (l = 0l; l < 100l; l += 1l) {
          longCheckAll(Long.MAX_VALUE - l);
          longCheckAll(Long.MIN_VALUE + l);
      }

      System.out.println("End");
    }
}
''' % (checkers, decls, calls['int'], calls['long'])

with open('src/Main.java', 'w') as f:
    f.write(code)
