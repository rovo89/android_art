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

import com.android.tools.perflib.heap.ClassObj;
import com.android.tools.perflib.heap.Instance;
import java.net.URI;

/**
 * Class to render an hprof value to a DocString.
 */
class Value {

  // For string literals, we limit the number of characters we show to
  // kMaxChars in case the string is really long.
  private static int kMaxChars = 200;

  /**
   * Create a DocString representing a summary of the given instance.
   */
  private static DocString renderInstance(AhatSnapshot snapshot, Instance inst) {
    DocString formatted = new DocString();
    if (inst == null) {
      formatted.append("(null)");
      return formatted;
    }

    // Annotate roots as roots.
    if (snapshot.isRoot(inst)) {
      formatted.append("(root) ");
    }


    // Annotate classes as classes.
    DocString link = new DocString();
    if (inst instanceof ClassObj) {
      link.append("class ");
    }

    link.append(inst.toString());

    URI objTarget = DocString.formattedUri("object?id=%d", inst.getId());
    formatted.appendLink(objTarget, link);

    // Annotate Strings with their values.
    String stringValue = InstanceUtils.asString(inst, kMaxChars);
    if (stringValue != null) {
      formatted.appendFormat(" \"%s", stringValue);
      formatted.append(kMaxChars == stringValue.length() ? "..." : "\"");
    }

    // Annotate Reference with its referent
    Instance referent = InstanceUtils.getReferent(inst);
    if (referent != null) {
      formatted.append(" for ");

      // It should not be possible for a referent to refer back to the
      // reference object, even indirectly, so there shouldn't be any issues
      // with infinite recursion here.
      formatted.append(renderInstance(snapshot, referent));
    }

    // Annotate DexCache with its location.
    String dexCacheLocation = InstanceUtils.getDexCacheLocation(inst, kMaxChars);
    if (dexCacheLocation != null) {
      formatted.appendFormat(" for %s", dexCacheLocation);
      if (kMaxChars == dexCacheLocation.length()) {
        formatted.append("...");
      }
    }


    // Annotate bitmaps with a thumbnail.
    Instance bitmap = InstanceUtils.getAssociatedBitmapInstance(inst);
    String thumbnail = "";
    if (bitmap != null) {
      URI uri = DocString.formattedUri("bitmap?id=%d", bitmap.getId());
      formatted.appendThumbnail(uri, "bitmap image");
    }
    return formatted;
  }

  /**
   * Create a DocString summarizing the given value.
   */
  public static DocString render(AhatSnapshot snapshot, Object val) {
    if (val instanceof Instance) {
      return renderInstance(snapshot, (Instance)val);
    } else {
      return DocString.format("%s", val);
    }
  }
}
