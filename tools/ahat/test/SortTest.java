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
import com.android.tools.perflib.heap.Heap;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import static org.junit.Assert.assertEquals;
import org.junit.Test;

public class SortTest {
  @Test
  public void objectsInfo() {
    Heap heapA = new Heap(0xA, "A");
    Heap heapB = new Heap(0xB, "B");
    ClassObj classA = new ClassObj(0x1A, null, "classA", 0);
    ClassObj classB = new ClassObj(0x1B, null, "classB", 0);
    ClassObj classC = new ClassObj(0x1C, null, "classC", 0);
    Site.ObjectsInfo infoA = new Site.ObjectsInfo(heapA, classA, 4, 14);
    Site.ObjectsInfo infoB = new Site.ObjectsInfo(heapB, classB, 2, 15);
    Site.ObjectsInfo infoC = new Site.ObjectsInfo(heapA, classC, 3, 13);
    Site.ObjectsInfo infoD = new Site.ObjectsInfo(heapB, classA, 5, 12);
    Site.ObjectsInfo infoE = new Site.ObjectsInfo(heapA, classB, 1, 11);
    List<Site.ObjectsInfo> list = new ArrayList<Site.ObjectsInfo>();
    list.add(infoA);
    list.add(infoB);
    list.add(infoC);
    list.add(infoD);
    list.add(infoE);

    // Sort by size.
    Collections.sort(list, new Sort.ObjectsInfoBySize());
    assertEquals(infoB, list.get(0));
    assertEquals(infoA, list.get(1));
    assertEquals(infoC, list.get(2));
    assertEquals(infoD, list.get(3));
    assertEquals(infoE, list.get(4));

    // Sort by class name.
    Collections.sort(list, new Sort.ObjectsInfoByClassName());
    assertEquals(classA, list.get(0).classObj);
    assertEquals(classA, list.get(1).classObj);
    assertEquals(classB, list.get(2).classObj);
    assertEquals(classB, list.get(3).classObj);
    assertEquals(classC, list.get(4).classObj);

    // Sort by heap name.
    Collections.sort(list, new Sort.ObjectsInfoByHeapName());
    assertEquals(heapA, list.get(0).heap);
    assertEquals(heapA, list.get(1).heap);
    assertEquals(heapA, list.get(2).heap);
    assertEquals(heapB, list.get(3).heap);
    assertEquals(heapB, list.get(4).heap);

    // Sort first by class name, then by size.
    Collections.sort(list, new Sort.WithPriority<Site.ObjectsInfo>(
          new Sort.ObjectsInfoByClassName(),
          new Sort.ObjectsInfoBySize()));
    assertEquals(infoA, list.get(0));
    assertEquals(infoD, list.get(1));
    assertEquals(infoB, list.get(2));
    assertEquals(infoE, list.get(3));
    assertEquals(infoC, list.get(4));
  }
}
