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
import com.android.tools.perflib.heap.Instance;
import com.android.tools.perflib.heap.StackFrame;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

class Site {
  // The site that this site was directly called from.
  // mParent is null for the root site.
  private Site mParent;

  // A description of the Site. Currently this is used to uniquely identify a
  // site within its parent.
  private String mName;

  // To identify this site, we pick one stack trace where we have seen the
  // site. mStackId is the id for that stack trace, and mStackDepth is the
  // depth of this site in that stack trace.
  // For the root site, mStackId is 0 and mStackDepth is 0.
  private int mStackId;
  private int mStackDepth;

  // Mapping from heap name to the total size of objects allocated in this
  // site (including child sites) on the given heap.
  private Map<String, Long> mSizesByHeap;

  // Mapping from child site name to child site.
  private Map<String, Site> mChildren;

  // List of all objects allocated in this site (including child sites).
  private List<Instance> mObjects;
  private List<ObjectsInfo> mObjectsInfos;
  private Map<Heap, Map<ClassObj, ObjectsInfo>> mObjectsInfoMap;

  public static class ObjectsInfo {
    public Heap heap;
    public ClassObj classObj;
    public long numInstances;
    public long numBytes;

    public ObjectsInfo(Heap heap, ClassObj classObj, long numInstances, long numBytes) {
      this.heap = heap;
      this.classObj = classObj;
      this.numInstances = numInstances;
      this.numBytes = numBytes;
    }
  }

  /**
   * Construct a root site.
   */
  public Site(String name) {
    this(null, name, 0, 0);
  }

  public Site(Site parent, String name, int stackId, int stackDepth) {
    mParent = parent;
    mName = name;
    mStackId = stackId;
    mStackDepth = stackDepth;
    mSizesByHeap = new HashMap<String, Long>();
    mChildren = new HashMap<String, Site>();
    mObjects = new ArrayList<Instance>();
    mObjectsInfos = new ArrayList<ObjectsInfo>();
    mObjectsInfoMap = new HashMap<Heap, Map<ClassObj, ObjectsInfo>>();
  }

  /**
   * Add an instance to this site.
   * Returns the site at which the instance was allocated.
   */
  public Site add(int stackId, int stackDepth, Iterator<StackFrame> path, Instance inst) {
    mObjects.add(inst);

    String heap = inst.getHeap().getName();
    mSizesByHeap.put(heap, getSize(heap) + inst.getSize());

    Map<ClassObj, ObjectsInfo> classToObjectsInfo = mObjectsInfoMap.get(inst.getHeap());
    if (classToObjectsInfo == null) {
      classToObjectsInfo = new HashMap<ClassObj, ObjectsInfo>();
      mObjectsInfoMap.put(inst.getHeap(), classToObjectsInfo);
    }

    ObjectsInfo info = classToObjectsInfo.get(inst.getClassObj());
    if (info == null) {
      info = new ObjectsInfo(inst.getHeap(), inst.getClassObj(), 0, 0);
      mObjectsInfos.add(info);
      classToObjectsInfo.put(inst.getClassObj(), info);
    }

    info.numInstances++;
    info.numBytes += inst.getSize();

    if (path.hasNext()) {
      String next = path.next().toString();
      Site child = mChildren.get(next);
      if (child == null) {
        child = new Site(this, next, stackId, stackDepth + 1);
        mChildren.put(next, child);
      }
      return child.add(stackId, stackDepth + 1, path, inst);
    } else {
      return this;
    }
  }

  // Get the size of a site for a specific heap.
  public long getSize(String heap) {
    Long val = mSizesByHeap.get(heap);
    if (val == null) {
      return 0;
    }
    return val;
  }

  /**
   * Get the list of objects allocated under this site. Includes objects
   * allocated in children sites.
   */
  public Collection<Instance> getObjects() {
    return mObjects;
  }

  public List<ObjectsInfo> getObjectsInfos() {
    return mObjectsInfos;
  }

  // Get the combined size of the site for all heaps.
  public long getTotalSize() {
    long size = 0;
    for (Long val : mSizesByHeap.values()) {
      size += val;
    }
    return size;
  }

  /**
   * Return the site this site was called from.
   * Returns null for the root site.
   */
  public Site getParent() {
    return mParent;
  }

  public String getName() {
    return mName;
  }

  // Returns the hprof id of a stack this site appears on.
  public int getStackId() {
    return mStackId;
  }

  // Returns the stack depth of this site in the stack whose id is returned
  // by getStackId().
  public int getStackDepth() {
    return mStackDepth;
  }

  List<Site> getChildren() {
    return new ArrayList<Site>(mChildren.values());
  }

  // Get the child at the given path relative to this site.
  // Returns null if no such child found.
  Site getChild(Iterator<StackFrame> path) {
    if (path.hasNext()) {
      String next = path.next().toString();
      Site child = mChildren.get(next);
      return (child == null) ? null : child.getChild(path);
    } else {
      return this;
    }
  }
}
