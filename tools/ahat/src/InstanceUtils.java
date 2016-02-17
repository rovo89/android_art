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
import com.android.tools.perflib.heap.Heap;
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
  private static boolean isInstanceOfClass(Instance inst, String className) {
    ClassObj cls = (inst == null) ? null : inst.getClassObj();
    return (cls != null && className.equals(cls.getClassName()));
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


  /**
   * Read the string value from an hprof Instance.
   * Returns null if the object can't be interpreted as a string.
   */
  public static String asString(Instance inst) {
    return asString(inst, -1);
  }

  /**
   * Read the string value from an hprof Instance.
   * Returns null if the object can't be interpreted as a string.
   * The returned string is truncated to maxChars characters.
   * If maxChars is negative, the returned string is not truncated.
   */
  public static String asString(Instance inst, int maxChars) {
    // The inst object could either be a java.lang.String or a char[]. If it
    // is a char[], use that directly as the value, otherwise use the value
    // field of the string object. The field accesses for count and offset
    // later on will work okay regardless of what type the inst object is.
    Object value = inst;
    if (isInstanceOfClass(inst, "java.lang.String")) {
      value = getField(inst, "value");
    }

    if (!(value instanceof ArrayInstance)) {
      return null;
    }

    ArrayInstance chars = (ArrayInstance) value;
    if (chars.getArrayType() != Type.CHAR) {
      return null;
    }

    // TODO: When perflib provides a better way to get the length of the
    // array, we should use that here.
    int numChars = chars.getValues().length;
    int count = getIntField(inst, "count", numChars);
    if (count == 0) {
      return "";
    }
    if (0 <= maxChars && maxChars < count) {
      count = maxChars;
    }

    int offset = getIntField(inst, "offset", 0);
    int end = offset + count - 1;
    if (offset >= 0 && offset < numChars && end >= 0 && end < numChars) {
      return new String(chars.asCharArray(offset, count));
    }
    return null;
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

    Integer width = getIntField(inst, "mWidth", null);
    if (width == null) {
      return null;
    }

    Integer height = getIntField(inst, "mHeight", null);
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
  public static Object getField(Instance inst, String fieldName) {
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
   * Returns <code>def</code> if the field value is not an int or could not be
   * read.
   */
  private static Integer getIntField(Instance inst, String fieldName, Integer def) {
    Object value = getField(inst, fieldName);
    if (!(value instanceof Integer)) {
      return def;
    }
    return (Integer)value;
  }

  /**
   * Read a long field of an instance.
   * The field is assumed to be a long type.
   * Returns <code>def</code> if the field value is not an long or could not
   * be read.
   */
  private static Long getLongField(Instance inst, String fieldName, Long def) {
    Object value = getField(inst, fieldName);
    if (!(value instanceof Long)) {
      return def;
    }
    return (Long)value;
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

  private static boolean isJavaLangRefReference(Instance inst) {
    ClassObj cls = (inst == null) ? null : inst.getClassObj();
    while (cls != null) {
      if ("java.lang.ref.Reference".equals(cls.getClassName())) {
        return true;
      }
      cls = cls.getSuperClassObj();
    }
    return false;
  }

  public static Instance getReferent(Instance inst) {
    if (isJavaLangRefReference(inst)) {
      return getRefField(inst, "referent");
    }
    return null;
  }

  /**
   * Assuming inst represents a DexCache object, return the dex location for
   * that dex cache. Returns null if the given instance doesn't represent a
   * DexCache object or the location could not be found.
   * If maxChars is non-negative, the returned location is truncated to
   * maxChars in length.
   */
  public static String getDexCacheLocation(Instance inst, int maxChars) {
    if (isInstanceOfClass(inst, "java.lang.DexCache")) {
      Instance location = getRefField(inst, "location");
      if (location != null) {
        return asString(location, maxChars);
      }
    }
    return null;
  }

  public static class NativeAllocation {
    public long size;
    public Heap heap;
    public long pointer;
    public Instance referent;

    public NativeAllocation(long size, Heap heap, long pointer, Instance referent) {
      this.size = size;
      this.heap = heap;
      this.pointer = pointer;
      this.referent = referent;
    }
  }

  /**
   * Assuming inst represents a NativeAllocation, return information about the
   * native allocation. Returns null if the given instance doesn't represent a
   * native allocation.
   */
  public static NativeAllocation getNativeAllocation(Instance inst) {
    if (!isInstanceOfClass(inst, "libcore.util.NativeAllocationRegistry$CleanerThunk")) {
      return null;
    }

    Long pointer = InstanceUtils.getLongField(inst, "nativePtr", null);
    if (pointer == null) {
      return null;
    }

    // Search for the registry field of inst.
    // Note: We know inst as an instance of ClassInstance because we already
    // read the nativePtr field from it.
    Instance registry = null;
    for (ClassInstance.FieldValue field : ((ClassInstance)inst).getValues()) {
      Object fieldValue = field.getValue();
      if (fieldValue instanceof Instance) {
        Instance fieldInst = (Instance)fieldValue;
        if (isInstanceOfClass(fieldInst, "libcore.util.NativeAllocationRegistry")) {
          registry = fieldInst;
          break;
        }
      }
    }

    if (registry == null) {
      return null;
    }

    Long size = InstanceUtils.getLongField(registry, "size", null);
    if (size == null) {
      return null;
    }

    Instance referent = null;
    for (Instance ref : inst.getHardReferences()) {
      if (isInstanceOfClass(ref, "sun.misc.Cleaner")) {
        referent = InstanceUtils.getReferent(ref);
        if (referent != null) {
          break;
        }
      }
    }

    if (referent == null) {
      return null;
    }
    return new NativeAllocation(size, inst.getHeap(), pointer, referent);
  }
}
