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

#include "callee_save_frame.h"
#include "object.h"

namespace art {

/*
 * Fill the array with predefined constant values, throwing exceptions if the array is null or
 * not of sufficient length.
 *
 * NOTE: When dealing with a raw dex file, the data to be copied uses
 * little-endian ordering.  Require that oat2dex do any required swapping
 * so this routine can get by with a memcpy().
 *
 * Format of the data:
 *  ushort ident = 0x0300   magic value
 *  ushort width            width of each element in the table
 *  uint   size             number of elements in the table
 *  ubyte  data[size*width] table of data values (may contain a single-byte
 *                          padding at the end)
 */
extern "C" int artHandleFillArrayDataFromCode(Array* array, const uint16_t* table,
                                              Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
  DCHECK_EQ(table[0], 0x0300);
  if (UNLIKELY(array == NULL)) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
        "null array in fill array");
    return -1;  // Error
  }
  DCHECK(array->IsArrayInstance() && !array->IsObjectArray());
  uint32_t size = (uint32_t)table[2] | (((uint32_t)table[3]) << 16);
  if (UNLIKELY(static_cast<int32_t>(size) > array->GetLength())) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
        "failed array fill. length=%d; index=%d", array->GetLength(), size);
    return -1;  // Error
  }
  uint16_t width = table[1];
  uint32_t size_in_bytes = size * width;
  memcpy((char*)array + Array::DataOffset(width).Int32Value(), (char*)&table[4], size_in_bytes);
  return 0;  // Success
}

}  // namespace art
