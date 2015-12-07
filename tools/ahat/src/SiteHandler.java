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
import java.io.IOException;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

class SiteHandler implements AhatHandler {
  private static final String ALLOCATION_SITE_ID = "frames";
  private static final String SITES_CALLED_ID = "called";
  private static final String OBJECTS_ALLOCATED_ID = "objects";

  private AhatSnapshot mSnapshot;

  public SiteHandler(AhatSnapshot snapshot) {
    mSnapshot = snapshot;
  }

  @Override
  public void handle(Doc doc, Query query) throws IOException {
    int stackId = query.getInt("stack", 0);
    int depth = query.getInt("depth", -1);
    Site site = mSnapshot.getSite(stackId, depth);

    doc.title("Site %s", site.getName());
    doc.section("Allocation Site");
    SitePrinter.printSite(mSnapshot, doc, query, ALLOCATION_SITE_ID, site);

    doc.section("Sites Called from Here");
    List<Site> children = site.getChildren();
    if (children.isEmpty()) {
      doc.println(DocString.text("(none)"));
    } else {
      Collections.sort(children, new Sort.SiteBySize("app"));

      HeapTable.TableConfig<Site> table = new HeapTable.TableConfig<Site>() {
        public String getHeapsDescription() {
          return "Reachable Bytes Allocated on Heap";
        }

        public long getSize(Site element, Heap heap) {
          return element.getSize(heap.getName());
        }

        public List<HeapTable.ValueConfig<Site>> getValueConfigs() {
          HeapTable.ValueConfig<Site> value = new HeapTable.ValueConfig<Site>() {
            public String getDescription() {
              return "Child Site";
            }

            public DocString render(Site element) {
              return DocString.link(
                  DocString.formattedUri("site?stack=%d&depth=%d",
                    element.getStackId(), element.getStackDepth()),
                  DocString.text(element.getName()));
            }
          };
          return Collections.singletonList(value);
        }
      };
      HeapTable.render(doc, query, SITES_CALLED_ID, table, mSnapshot, children);
    }

    doc.section("Objects Allocated");
    doc.table(
        new Column("Reachable Bytes Allocated", Column.Align.RIGHT),
        new Column("Instances", Column.Align.RIGHT),
        new Column("Heap"),
        new Column("Class"));
    List<Site.ObjectsInfo> infos = site.getObjectsInfos();
    Comparator<Site.ObjectsInfo> compare = new Sort.WithPriority<Site.ObjectsInfo>(
        new Sort.ObjectsInfoByHeapName(),
        new Sort.ObjectsInfoBySize(),
        new Sort.ObjectsInfoByClassName());
    Collections.sort(infos, compare);
    SubsetSelector<Site.ObjectsInfo> selector
      = new SubsetSelector(query, OBJECTS_ALLOCATED_ID, infos);
    for (Site.ObjectsInfo info : selector.selected()) {
      String className = AhatSnapshot.getClassName(info.classObj);
      doc.row(
          DocString.format("%,14d", info.numBytes),
          DocString.link(
            DocString.formattedUri("objects?stack=%d&depth=%d&heap=%s&class=%s",
                site.getStackId(), site.getStackDepth(), info.heap.getName(), className),
            DocString.format("%,14d", info.numInstances)),
          DocString.text(info.heap.getName()),
          Value.render(mSnapshot, info.classObj));
    }
    doc.end();
    selector.render(doc);
  }
}

