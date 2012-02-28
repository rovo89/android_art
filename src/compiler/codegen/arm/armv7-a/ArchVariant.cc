/*
 * Copyright (C) 2011 The Android Open Source Project
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
 * Determine the initial instruction set to be used for this trace.
 * Later components may decide to change this.
 */
OatInstructionSetType oatInstructionSet(void)
{
    return DALVIK_OAT_THUMB2;
}

/* Architecture-specific initializations and checks go here */
bool oatArchVariantInit(void)
{
    return true;
}

int oatTargetOptHint(int key)
{
    int res = 0;
    switch (key) {
        case kMaxHoistDistance:
            res = 7;
            break;
        default:
            LOG(FATAL) << "Unknown target optimization hint key: " << key;
    }
    return res;
}

void oatGenMemBarrier(CompilationUnit* cUnit, int barrierKind)
{
#if ANDROID_SMP != 0
    LIR* dmb = newLIR1(cUnit, kThumb2Dmb, barrierKind);
    dmb->defMask = ENCODE_ALL;
#endif
}

}  // namespace art
