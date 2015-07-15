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
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Class for rendering a list of instances dominated by a single instance in a
 * pretty way.
 */
class DominatedList {
  private static final int kIncrAmount = 100;
  private static final int kDefaultShown = 100;

  /**
   * Render a table to the given HtmlWriter showing a pretty list of
   * instances.
   *
   * Rather than show all of the instances (which may be very many), we use
   * the query parameter "dominated" to specify a limited number of
   * instances to show. The 'uri' parameter should be the current page URI, so
   * that we can add links to "show more" and "show less" objects that go to
   * the same page with only the number of objects adjusted.
   */
  public static void render(final AhatSnapshot snapshot, Doc doc,
      Collection<Instance> instances, Query query) {
    List<Instance> insts = new ArrayList<Instance>(instances);
    Collections.sort(insts, Sort.defaultInstanceCompare(snapshot));

    int numInstancesToShow = getNumInstancesToShow(query, insts.size());
    List<Instance> shown = new ArrayList<Instance>(insts.subList(0, numInstancesToShow));
    List<Instance> hidden = insts.subList(numInstancesToShow, insts.size());

    // Add 'null' as a marker for "all the rest of the objects".
    if (!hidden.isEmpty()) {
      shown.add(null);
    }
    HeapTable.render(doc, new TableConfig(snapshot, hidden), snapshot, shown);

    if (insts.size() > kDefaultShown) {
      printMenu(doc, query, numInstancesToShow, insts.size());
    }
  }

  private static class TableConfig implements HeapTable.TableConfig<Instance> {
    AhatSnapshot mSnapshot;

    // Map from heap name to the total size of the instances not shown in the
    // table.
    Map<Heap, Long> mHiddenSizes;

    public TableConfig(AhatSnapshot snapshot, List<Instance> hidden) {
      mSnapshot = snapshot;
      mHiddenSizes = new HashMap<Heap, Long>();
      for (Heap heap : snapshot.getHeaps()) {
        mHiddenSizes.put(heap, 0L);
      }

      if (!hidden.isEmpty()) {
        for (Instance inst : hidden) {
          for (Heap heap : snapshot.getHeaps()) {
            int index = snapshot.getHeapIndex(heap);
            long size = inst.getRetainedSize(index);
            mHiddenSizes.put(heap, mHiddenSizes.get(heap) + size);
          }
        }
      }
    }

    @Override
    public String getHeapsDescription() {
      return "Bytes Retained by Heap";
    }

    @Override
    public long getSize(Instance element, Heap heap) {
      if (element == null) {
        return mHiddenSizes.get(heap);
      }
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
          if (element == null) {
            return DocString.text("...");
          } else {
            return Value.render(element);
          }
        }
      };
      return Collections.singletonList(value);
    }
  }

  // Figure out how many objects to show based on the query parameter.
  // The resulting value is guaranteed to be at least zero, and no greater
  // than the number of total objects.
  private static int getNumInstancesToShow(Query query, int totalNumInstances) {
    String value = query.get("dominated", null);
    try {
      int count = Math.min(totalNumInstances, Integer.parseInt(value));
      return Math.max(0, count);
    } catch (NumberFormatException e) {
      // We can't parse the value as a number. Ignore it.
    }
    return Math.min(kDefaultShown, totalNumInstances);
  }

  // Print a menu line after the table to control how many objects are shown.
  // It has the form:
  //  (showing X of Y objects - show none - show less - show more - show all)
  private static void printMenu(Doc doc, Query query, int shown, int all) {
    DocString menu = new DocString();
    menu.append("(%d of %d objects shown - ", shown, all);
    if (shown > 0) {
      int less = Math.max(0, shown - kIncrAmount);
      menu.appendLink(query.with("dominated", 0), DocString.text("show none"));
      menu.append(" - ");
      menu.appendLink(query.with("dominated", less), DocString.text("show less"));
      menu.append(" - ");
    } else {
      menu.append("show none - show less - ");
    }
    if (shown < all) {
      int more = Math.min(shown + kIncrAmount, all);
      menu.appendLink(query.with("dominated", more), DocString.text("show more"));
      menu.append(" - ");
      menu.appendLink(query.with("dominated", all), DocString.text("show all"));
      menu.append(")");
    } else {
      menu.append("show more - show all)");
    }
    doc.println(menu);
  }
}

