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
import com.android.tools.perflib.heap.Heap;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.Iterator;

/**
 * Provides Comparators and helper functions for sorting Instances, Sites, and
 * other things.
 *
 * Note: The Comparators defined here impose orderings that are inconsistent
 * with equals. They should not be used for element lookup or search. They
 * should only be used for showing elements to the user in different orders.
 */
class Sort {
  /**
   * Compare instances by their instance id.
   * This sorts instances from smaller id to larger id.
   */
  public static class InstanceById implements Comparator<Instance> {
    @Override
    public int compare(Instance a, Instance b) {
      return Long.compare(a.getId(), b.getId());
    }
  }

  /**
   * Compare instances by their total retained size.
   * Different instances with the same total retained size are considered
   * equal for the purposes of comparison.
   * This sorts instances from larger retained size to smaller retained size.
   */
  public static class InstanceByTotalRetainedSize implements Comparator<Instance> {
    @Override
    public int compare(Instance a, Instance b) {
      return Long.compare(b.getTotalRetainedSize(), a.getTotalRetainedSize());
    }
  }

  /**
   * Compare instances by their retained size for a given heap index.
   * Different instances with the same total retained size are considered
   * equal for the purposes of comparison.
   * This sorts instances from larger retained size to smaller retained size.
   */
  public static class InstanceByHeapRetainedSize implements Comparator<Instance> {
    private int mIndex;

    public InstanceByHeapRetainedSize(AhatSnapshot snapshot, Heap heap) {
      mIndex = snapshot.getHeapIndex(heap);
    }

    public InstanceByHeapRetainedSize(int heapIndex) {
      mIndex = heapIndex;
    }

    @Override
    public int compare(Instance a, Instance b) {
      return Long.compare(b.getRetainedSize(mIndex), a.getRetainedSize(mIndex));
    }
  }

  /**
   * Compare objects based on a list of comparators, giving priority to the
   * earlier comparators in the list.
   */
  public static class WithPriority<T> implements Comparator<T> {
    private List<Comparator<T>> mComparators;

    public WithPriority(Comparator<T>... comparators) {
      mComparators = Arrays.asList(comparators);
    }

    public WithPriority(List<Comparator<T>> comparators) {
      mComparators = comparators;
    }

    @Override
    public int compare(T a, T b) {
      int res = 0;
      Iterator<Comparator<T>> iter = mComparators.iterator();
      while (res == 0 && iter.hasNext()) {
        res = iter.next().compare(a, b);
      }
      return res;
    }
  }

  public static Comparator<Instance> defaultInstanceCompare(AhatSnapshot snapshot) {
    List<Comparator<Instance>> comparators = new ArrayList<Comparator<Instance>>();

    // Priority goes to the app heap, if we can find one.
    Heap appHeap = snapshot.getHeap("app");
    if (appHeap != null) {
      comparators.add(new InstanceByHeapRetainedSize(snapshot, appHeap));
    }

    // Next is by total retained size.
    comparators.add(new InstanceByTotalRetainedSize());
    return new WithPriority<Instance>(comparators);
  }

  /**
   * Compare Sites by the size of objects allocated on a given heap.
   * Different object infos with the same size on the given heap are
   * considered equal for the purposes of comparison.
   * This sorts sites from larger size to smaller size.
   */
  public static class SiteBySize implements Comparator<Site> {
    String mHeap;

    public SiteBySize(String heap) {
      mHeap = heap;
    }

    @Override
    public int compare(Site a, Site b) {
      return Long.compare(b.getSize(mHeap), a.getSize(mHeap));
    }
  }

  /**
   * Compare Site.ObjectsInfo by their size.
   * Different object infos with the same total retained size are considered
   * equal for the purposes of comparison.
   * This sorts object infos from larger retained size to smaller size.
   */
  public static class ObjectsInfoBySize implements Comparator<Site.ObjectsInfo> {
    @Override
    public int compare(Site.ObjectsInfo a, Site.ObjectsInfo b) {
      return Long.compare(b.numBytes, a.numBytes);
    }
  }

  /**
   * Compare Site.ObjectsInfo by heap name.
   * Different object infos with the same heap name are considered equal for
   * the purposes of comparison.
   */
  public static class ObjectsInfoByHeapName implements Comparator<Site.ObjectsInfo> {
    @Override
    public int compare(Site.ObjectsInfo a, Site.ObjectsInfo b) {
      return a.heap.getName().compareTo(b.heap.getName());
    }
  }

  /**
   * Compare Site.ObjectsInfo by class name.
   * Different object infos with the same class name are considered equal for
   * the purposes of comparison.
   */
  public static class ObjectsInfoByClassName implements Comparator<Site.ObjectsInfo> {
    @Override
    public int compare(Site.ObjectsInfo a, Site.ObjectsInfo b) {
      String aName = AhatSnapshot.getClassName(a.classObj);
      String bName = AhatSnapshot.getClassName(b.classObj);
      return aName.compareTo(bName);
    }
  }

  /**
   * Compare AhatSnapshot.NativeAllocation by heap name.
   * Different allocations with the same heap name are considered equal for
   * the purposes of comparison.
   */
  public static class NativeAllocationByHeapName
      implements Comparator<InstanceUtils.NativeAllocation> {
    @Override
    public int compare(InstanceUtils.NativeAllocation a, InstanceUtils.NativeAllocation b) {
      return a.heap.getName().compareTo(b.heap.getName());
    }
  }

  /**
   * Compare InstanceUtils.NativeAllocation by their size.
   * Different allocations with the same size are considered equal for the
   * purposes of comparison.
   * This sorts allocations from larger size to smaller size.
   */
  public static class NativeAllocationBySize implements Comparator<InstanceUtils.NativeAllocation> {
    @Override
    public int compare(InstanceUtils.NativeAllocation a, InstanceUtils.NativeAllocation b) {
      return Long.compare(b.size, a.size);
    }
  }
}

