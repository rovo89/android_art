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
import com.android.tools.perflib.heap.Field;
import com.android.tools.perflib.heap.Heap;
import com.android.tools.perflib.heap.Instance;
import com.android.tools.perflib.heap.RootObj;
import com.android.tools.perflib.heap.RootType;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Map;

class ObjectHandler implements AhatHandler {

  private static final String ARRAY_ELEMENTS_ID = "elements";
  private static final String DOMINATOR_PATH_ID = "dompath";
  private static final String ALLOCATION_SITE_ID = "frames";
  private static final String DOMINATED_OBJECTS_ID = "dominated";
  private static final String INSTANCE_FIELDS_ID = "ifields";
  private static final String STATIC_FIELDS_ID = "sfields";
  private static final String HARD_REFS_ID = "refs";
  private static final String SOFT_REFS_ID = "srefs";

  private AhatSnapshot mSnapshot;

  public ObjectHandler(AhatSnapshot snapshot) {
    mSnapshot = snapshot;
  }

  @Override
  public void handle(Doc doc, Query query) throws IOException {
    long id = query.getLong("id", 0);
    Instance inst = mSnapshot.findInstance(id);
    if (inst == null) {
      doc.println(DocString.format("No object with id %08xl", id));
      return;
    }

    doc.title("Object %08x", inst.getUniqueId());
    doc.big(Value.render(mSnapshot, inst));

    printAllocationSite(doc, query, inst);
    printDominatorPath(doc, query, inst);

    doc.section("Object Info");
    ClassObj cls = inst.getClassObj();
    doc.descriptions();
    doc.description(DocString.text("Class"), Value.render(mSnapshot, cls));
    doc.description(DocString.text("Size"), DocString.format("%d", inst.getSize()));
    doc.description(
        DocString.text("Retained Size"),
        DocString.format("%d", inst.getTotalRetainedSize()));
    doc.description(DocString.text("Heap"), DocString.text(inst.getHeap().getName()));

    Collection<RootType> rootTypes = mSnapshot.getRootTypes(inst);
    if (rootTypes != null) {
      DocString types = new DocString();
      String comma = "";
      for (RootType type : rootTypes) {
        types.append(comma);
        types.append(type.getName());
        comma = ", ";
      }
      doc.description(DocString.text("Root Types"), types);
    }

    doc.end();

    printBitmap(doc, inst);
    if (inst instanceof ClassInstance) {
      printClassInstanceFields(doc, query, mSnapshot, (ClassInstance)inst);
    } else if (inst instanceof ArrayInstance) {
      printArrayElements(doc, query, mSnapshot, (ArrayInstance)inst);
    } else if (inst instanceof ClassObj) {
      printClassInfo(doc, query, mSnapshot, (ClassObj)inst);
    }
    printReferences(doc, query, mSnapshot, inst);
    printDominatedObjects(doc, query, inst);
  }

  private static void printClassInstanceFields(
      Doc doc, Query query, AhatSnapshot snapshot, ClassInstance inst) {
    doc.section("Fields");
    doc.table(new Column("Type"), new Column("Name"), new Column("Value"));
    SubsetSelector<ClassInstance.FieldValue> selector
      = new SubsetSelector(query, INSTANCE_FIELDS_ID, inst.getValues());
    for (ClassInstance.FieldValue field : selector.selected()) {
      doc.row(
          DocString.text(field.getField().getType().toString()),
          DocString.text(field.getField().getName()),
          Value.render(snapshot, field.getValue()));
    }
    doc.end();
    selector.render(doc);
  }

  private static void printArrayElements(
      Doc doc, Query query, AhatSnapshot snapshot, ArrayInstance array) {
    doc.section("Array Elements");
    doc.table(new Column("Index", Column.Align.RIGHT), new Column("Value"));
    List<Object> elements = Arrays.asList(array.getValues());
    SubsetSelector<Object> selector = new SubsetSelector(query, ARRAY_ELEMENTS_ID, elements);
    int i = 0;
    for (Object elem : selector.selected()) {
      doc.row(DocString.format("%d", i), Value.render(snapshot, elem));
      i++;
    }
    doc.end();
    selector.render(doc);
  }

  private static void printClassInfo(
      Doc doc, Query query, AhatSnapshot snapshot, ClassObj clsobj) {
    doc.section("Class Info");
    doc.descriptions();
    doc.description(DocString.text("Super Class"),
        Value.render(snapshot, clsobj.getSuperClassObj()));
    doc.description(DocString.text("Class Loader"),
        Value.render(snapshot, clsobj.getClassLoader()));
    doc.end();

    doc.section("Static Fields");
    doc.table(new Column("Type"), new Column("Name"), new Column("Value"));
    List<Map.Entry<Field, Object>> fields
      = new ArrayList<Map.Entry<Field, Object>>(clsobj.getStaticFieldValues().entrySet());
    SubsetSelector<Map.Entry<Field, Object>> selector
      = new SubsetSelector(query, STATIC_FIELDS_ID, fields);
    for (Map.Entry<Field, Object> field : selector.selected()) {
      doc.row(
          DocString.text(field.getKey().getType().toString()),
          DocString.text(field.getKey().getName()),
          Value.render(snapshot, field.getValue()));
    }
    doc.end();
    selector.render(doc);
  }

  private static void printReferences(
      Doc doc, Query query, AhatSnapshot snapshot, Instance inst) {
    doc.section("Objects with References to this Object");
    if (inst.getHardReferences().isEmpty()) {
      doc.println(DocString.text("(none)"));
    } else {
      doc.table(new Column("Object"));
      List<Instance> references = inst.getHardReferences();
      SubsetSelector<Instance> selector = new SubsetSelector(query, HARD_REFS_ID, references);
      for (Instance ref : selector.selected()) {
        doc.row(Value.render(snapshot, ref));
      }
      doc.end();
      selector.render(doc);
    }

    if (inst.getSoftReferences() != null) {
      doc.section("Objects with Soft References to this Object");
      doc.table(new Column("Object"));
      List<Instance> references = inst.getSoftReferences();
      SubsetSelector<Instance> selector = new SubsetSelector(query, SOFT_REFS_ID, references);
      for (Instance ref : selector.selected()) {
        doc.row(Value.render(snapshot, ref));
      }
      doc.end();
      selector.render(doc);
    }
  }

  private void printAllocationSite(Doc doc, Query query, Instance inst) {
    doc.section("Allocation Site");
    Site site = mSnapshot.getSiteForInstance(inst);
    SitePrinter.printSite(mSnapshot, doc, query, ALLOCATION_SITE_ID, site);
  }

  // Draw the bitmap corresponding to this instance if there is one.
  private static void printBitmap(Doc doc, Instance inst) {
    Instance bitmap = InstanceUtils.getAssociatedBitmapInstance(inst);
    if (bitmap != null) {
      doc.section("Bitmap Image");
      doc.println(DocString.image(
            DocString.formattedUri("bitmap?id=%d", bitmap.getId()), "bitmap image"));
    }
  }

  private void printDominatorPath(Doc doc, Query query, Instance inst) {
    doc.section("Dominator Path from Root");
    List<Instance> path = new ArrayList<Instance>();
    for (Instance parent = inst;
        parent != null && !(parent instanceof RootObj);
        parent = parent.getImmediateDominator()) {
      path.add(parent);
    }

    // Add 'null' as a marker for the root.
    path.add(null);
    Collections.reverse(path);

    HeapTable.TableConfig<Instance> table = new HeapTable.TableConfig<Instance>() {
      public String getHeapsDescription() {
        return "Bytes Retained by Heap";
      }

      public long getSize(Instance element, Heap heap) {
        if (element == null) {
          return mSnapshot.getHeapSize(heap);
        }
        int index = mSnapshot.getHeapIndex(heap);
        return element.getRetainedSize(index);
      }

      public List<HeapTable.ValueConfig<Instance>> getValueConfigs() {
        HeapTable.ValueConfig<Instance> value = new HeapTable.ValueConfig<Instance>() {
          public String getDescription() {
            return "Object";
          }

          public DocString render(Instance element) {
            if (element == null) {
              return DocString.link(DocString.uri("rooted"), DocString.text("ROOT"));
            } else {
              return DocString.text("→ ").append(Value.render(mSnapshot, element));
            }
          }
        };
        return Collections.singletonList(value);
      }
    };
    HeapTable.render(doc, query, DOMINATOR_PATH_ID, table, mSnapshot, path);
  }

  public void printDominatedObjects(Doc doc, Query query, Instance inst) {
    doc.section("Immediately Dominated Objects");
    List<Instance> instances = mSnapshot.getDominated(inst);
    if (instances != null) {
      DominatedList.render(mSnapshot, doc, query, DOMINATED_OBJECTS_ID, instances);
    } else {
      doc.println(DocString.text("(none)"));
    }
  }
}

