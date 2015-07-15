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

import com.android.tools.perflib.heap.Instance;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

class ObjectsHandler extends AhatHandler {
  public ObjectsHandler(AhatSnapshot snapshot) {
    super(snapshot);
  }

  @Override
  public void handle(Doc doc, Query query) throws IOException {
    int stackId = query.getInt("stack", 0);
    int depth = query.getInt("depth", 0);
    String className = query.get("class", null);
    String heapName = query.get("heap", null);
    Site site = mSnapshot.getSite(stackId, depth);

    List<Instance> insts = new ArrayList<Instance>();
    for (Instance inst : site.getObjects()) {
      if ((heapName == null || inst.getHeap().getName().equals(heapName))
          && (className == null
            || AhatSnapshot.getClassName(inst.getClassObj()).equals(className))) {
        insts.add(inst);
      }
    }

    Collections.sort(insts, Sort.defaultInstanceCompare(mSnapshot));

    doc.title("Objects");
    doc.table(
        new Column("Size", Column.Align.RIGHT),
        new Column("Heap"),
        new Column("Object"));
    for (Instance inst : insts) {
      doc.row(
          DocString.text("%,d", inst.getSize()),
          DocString.text(inst.getHeap().getName()),
          Value.render(inst));
    }
    doc.end();
  }
}

