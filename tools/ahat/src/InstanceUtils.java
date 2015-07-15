/*
 * Copyright (C) 2015 The Android Open Source Project
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

package com.android.ahat;

import com.android.tools.perflib.heap.ArrayInstance;
import com.android.tools.perflib.heap.ClassInstance;
import com.android.tools.perflib.heap.ClassObj;
import com.android.tools.perflib.heap.Instance;
import com.android.tools.perflib.heap.Type;
import java.awt.image.BufferedImage;

/**
 * Utilities for extracting information from hprof instances.
 */
class InstanceUtils {
  /**
   * Returns true if the given instance is an instance of a class with the
   * given name.
   */
  public static boolean isInstanceOfClass(Instance inst, String className) {
    ClassObj cls = inst.getClassObj();
    return (cls != null && className.equals(cls.getClassName()));
  }

  /**
   * Read the char[] value from an hprof Instance.
   * Returns null if the object can't be interpreted as a char[].
   */
  private static char[] asCharArray(Instance inst) {
    if (! (inst instanceof ArrayInstance)) {
      return null;
    }

    ArrayInstance array = (ArrayInstance) inst;
    if (array.getArrayType() != Type.CHAR) {
      return null;
    }
    return array.asCharArray(0, array.getValues().length);
  }

  /**
   * Read the byte[] value from an hprof Instance.
   * Returns null if the instance is not a byte array.
   */
  private static byte[] asByteArray(Instance inst) {
    if (! (inst instanceof ArrayInstance)) {
      return null;
    }

    ArrayInstance array = (ArrayInstance)inst;
    if (array.getArrayType() != Type.BYTE) {
      return null;
    }

    Object[] objs = array.getValues();
    byte[] bytes = new byte[objs.length];
    for (int i = 0; i < objs.length; i++) {
      Byte b = (Byte)objs[i];
      bytes[i] = b.byteValue();
    }
    return bytes;
  }


  // Read the string value from an hprof Instance.
  // Returns null if the object can't be interpreted as a string.
  public static String asString(Instance inst) {
    if (!isInstanceOfClass(inst, "java.lang.String")) {
      return null;
    }
    char[] value = getCharArrayField(inst, "value");
    return (value == null) ? null : new String(value);
  }

  /**
   * Read the bitmap data for the given android.graphics.Bitmap object.
   * Returns null if the object isn't for android.graphics.Bitmap or the
   * bitmap data couldn't be read.
   */
  public static BufferedImage asBitmap(Instance inst) {
    if (!isInstanceOfClass(inst, "android.graphics.Bitmap")) {
      return null;
    }

    Integer width = getIntField(inst, "mWidth");
    if (width == null) {
      return null;
    }

    Integer height = getIntField(inst, "mHeight");
    if (height == null) {
      return null;
    }

    byte[] buffer = getByteArrayField(inst, "mBuffer");
    if (buffer == null) {
      return null;
    }

    // Convert the raw data to an image
    // Convert BGRA to ABGR
    int[] abgr = new int[height * width];
    for (int i = 0; i < abgr.length; i++) {
      abgr[i] = (
          (((int)buffer[i * 4 + 3] & 0xFF) << 24) +
          (((int)buffer[i * 4 + 0] & 0xFF) << 16) +
          (((int)buffer[i * 4 + 1] & 0xFF) << 8) +
          ((int)buffer[i * 4 + 2] & 0xFF));
    }

    BufferedImage bitmap = new BufferedImage(
        width, height, BufferedImage.TYPE_4BYTE_ABGR);
    bitmap.setRGB(0, 0, width, height, abgr, 0, width);
    return bitmap;
  }

  /**
   * Read a field of an instance.
   * Returns null if the field value is null or if the field couldn't be read.
   */
  private static Object getField(Instance inst, String fieldName) {
    if (!(inst instanceof ClassInstance)) {
      return null;
    }

    ClassInstance clsinst = (ClassInstance) inst;
    Object value = null;
    int count = 0;
    for (ClassInstance.FieldValue field : clsinst.getValues()) {
      if (fieldName.equals(field.getField().getName())) {
        value = field.getValue();
        count++;
      }
    }
    return count == 1 ? value : null;
  }

  /**
   * Read a reference field of an instance.
   * Returns null if the field value is null, or if the field couldn't be read.
   */
  private static Instance getRefField(Instance inst, String fieldName) {
    Object value = getField(inst, fieldName);
    if (!(value instanceof Instance)) {
      return null;
    }
    return (Instance)value;
  }

  /**
   * Read an int field of an instance.
   * The field is assumed to be an int type.
   * Returns null if the field value is not an int or could not be read.
   */
  private static Integer getIntField(Instance inst, String fieldName) {
    Object value = getField(inst, fieldName);
    if (!(value instanceof Integer)) {
      return null;
    }
    return (Integer)value;
  }

  /**
   * Read the given field from the given instance.
   * The field is assumed to be a byte[] field.
   * Returns null if the field value is null, not a byte[] or could not be read.
   */
  private static byte[] getByteArrayField(Instance inst, String fieldName) {
    Object value = getField(inst, fieldName);
    if (!(value instanceof Instance)) {
      return null;
    }
    return asByteArray((Instance)value);
  }

  private static char[] getCharArrayField(Instance inst, String fieldName) {
    Object value = getField(inst, fieldName);
    if (!(value instanceof Instance)) {
      return null;
    }
    return asCharArray((Instance)value);
  }

  // Return the bitmap instance associated with this object, or null if there
  // is none. This works for android.graphics.Bitmap instances and their
  // underlying Byte[] instances.
  public static Instance getAssociatedBitmapInstance(Instance inst) {
    ClassObj cls = inst.getClassObj();
    if (cls == null) {
      return null;
    }

    if ("android.graphics.Bitmap".equals(cls.getClassName())) {
      return inst;
    }

    if (inst instanceof ArrayInstance) {
      ArrayInstance array = (ArrayInstance)inst;
      if (array.getArrayType() == Type.BYTE && inst.getHardReferences().size() == 1) {
        Instance ref = inst.getHardReferences().get(0);
        ClassObj clsref = ref.getClassObj();
        if (clsref != null && "android.graphics.Bitmap".equals(clsref.getClassName())) {
          return ref;
        }
      }
    }
    return null;
  }

  /**
   * Assuming inst represents a DexCache object, return the dex location for
   * that dex cache. Returns null if the given instance doesn't represent a
   * DexCache object or the location could not be found.
   */
  public static String getDexCacheLocation(Instance inst) {
    if (isInstanceOfClass(inst, "java.lang.DexCache")) {
      Instance location = getRefField(inst, "location");
      if (location != null) {
        return asString(location);
      }
    }
    return null;
  }
}
