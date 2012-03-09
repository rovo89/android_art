/*
 * Copyright (C) 2012 The Android Open Source Project
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

namespace art {

/*
 * This file is included by Codegen-x86.c, and implements architecture
 * variant-specific code.
 */

/*
 * Determine the initial instruction set to be used for this trace.
 * Later components may decide to change this.
 */
InstructionSet oatInstructionSet()
{
    return kX86;
}

/* Architecture-specific initializations and checks go here */
bool oatArchVariantInit(void)
{
    return true;
}

int dvmCompilerTargetOptHint(int key)
{
    int res;
    switch (key) {
        case kMaxHoistDistance:
            res = 2;
            break;
        default:
            LOG(FATAL) << "Unknown target optimization hint key: " << key;
    }
    return res;
}

void oatGenMemBarrier(CompilationUnit *cUnit, int barrierKind)
{
#if ANDROID_SMP != 0
    UNIMPLEMENTED(WARNING) << "oatGenMemBarrier";
#if 0
    newLIR1(cUnit, kX86Sync, barrierKind);
#endif
#endif
}

}  // namespace art
