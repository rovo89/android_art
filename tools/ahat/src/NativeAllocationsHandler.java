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

import java.io.IOException;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

class NativeAllocationsHandler implements AhatHandler {
  private static final String ALLOCATIONS_ID = "allocations";

  private AhatSnapshot mSnapshot;

  public NativeAllocationsHandler(AhatSnapshot snapshot) {
    mSnapshot = snapshot;
  }

  @Override
  public void handle(Doc doc, Query query) throws IOException {
    List<InstanceUtils.NativeAllocation> allocs = mSnapshot.getNativeAllocations();

    doc.title("Registered Native Allocations");

    doc.section("Overview");
    long totalSize = 0;
    for (InstanceUtils.NativeAllocation alloc : allocs) {
      totalSize += alloc.size;
    }
    doc.descriptions();
    doc.description(DocString.text("Number of Registered Native Allocations"),
        DocString.format("%,14d", allocs.size()));
    doc.description(DocString.text("Total Size of Registered Native Allocations"),
        DocString.format("%,14d", totalSize));
    doc.end();

    doc.section("List of Allocations");
    if (allocs.isEmpty()) {
      doc.println(DocString.text("(none)"));
    } else {
      doc.table(
          new Column("Size", Column.Align.RIGHT),
          new Column("Heap"),
          new Column("Native Pointer"),
          new Column("Referent"));
      Comparator<InstanceUtils.NativeAllocation> compare
        = new Sort.WithPriority<InstanceUtils.NativeAllocation>(
            new Sort.NativeAllocationByHeapName(),
            new Sort.NativeAllocationBySize());
      Collections.sort(allocs, compare);
      SubsetSelector<InstanceUtils.NativeAllocation> selector
        = new SubsetSelector(query, ALLOCATIONS_ID, allocs);
      for (InstanceUtils.NativeAllocation alloc : selector.selected()) {
        doc.row(
            DocString.format("%,14d", alloc.size),
            DocString.text(alloc.heap.getName()),
            DocString.format("0x%x", alloc.pointer),
            Value.render(mSnapshot, alloc.referent));
      }

      // Print a summary of the remaining entries if there are any.
      List<InstanceUtils.NativeAllocation> remaining = selector.remaining();
      if (!remaining.isEmpty()) {
        long total = 0;
        for (InstanceUtils.NativeAllocation alloc : remaining) {
          total += alloc.size;
        }

        doc.row(
            DocString.format("%,14d", total),
            DocString.text("..."),
            DocString.text("..."),
            DocString.text("..."));
      }

      doc.end();
      selector.render(doc);
    }
  }
}

