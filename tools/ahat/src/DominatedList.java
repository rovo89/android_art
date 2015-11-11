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

import com.android.tools.perflib.heap.Heap;
import com.android.tools.perflib.heap.Instance;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.List;

/**
 * Class for rendering a list of instances dominated by a single instance in a
 * pretty way.
 */
class DominatedList {
  /**
   * Render a table to the given HtmlWriter showing a pretty list of
   * instances.
   *
   * @param snapshot  the snapshot where the instances reside
   * @param doc       the document to render the dominated list to
   * @param query     the current page query
   * @param id        a unique identifier to use for the dominated list in the current page
   * @param instances the collection of instances to generate a list for
   */
  public static void render(final AhatSnapshot snapshot,
      Doc doc, Query query, String id, Collection<Instance> instances) {
    List<Instance> insts = new ArrayList<Instance>(instances);
    Collections.sort(insts, Sort.defaultInstanceCompare(snapshot));
    HeapTable.render(doc, query, id, new TableConfig(snapshot), snapshot, insts);
  }

  private static class TableConfig implements HeapTable.TableConfig<Instance> {
    AhatSnapshot mSnapshot;

    public TableConfig(AhatSnapshot snapshot) {
      mSnapshot = snapshot;
    }

    @Override
    public String getHeapsDescription() {
      return "Bytes Retained by Heap";
    }

    @Override
    public long getSize(Instance element, Heap heap) {
      int index = mSnapshot.getHeapIndex(heap);
      return element.getRetainedSize(index);
    }

    @Override
    public List<HeapTable.ValueConfig<Instance>> getValueConfigs() {
      HeapTable.ValueConfig<Instance> value = new HeapTable.ValueConfig<Instance>() {
        public String getDescription() {
          return "Object";
        }

        public DocString render(Instance element) {
          return Value.render(mSnapshot, element);
        }
      };
      return Collections.singletonList(value);
    }
  }
}
