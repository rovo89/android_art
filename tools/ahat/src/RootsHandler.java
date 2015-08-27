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
import com.android.tools.perflib.heap.RootObj;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

class RootsHandler extends AhatHandler {
  public RootsHandler(AhatSnapshot snapshot) {
    super(snapshot);
  }

  @Override
  public void handle(Doc doc, Query query) throws IOException {
    doc.title("Roots");

    Set<Instance> rootset = new HashSet<Instance>();
    for (RootObj root : mSnapshot.getGCRoots()) {
      Instance inst = root.getReferredInstance();
      if (inst != null) {
        rootset.add(inst);
      }
    }

    List<Instance> roots = new ArrayList<Instance>();
    for (Instance inst : rootset) {
      roots.add(inst);
    }
    DominatedList.render(mSnapshot, doc, roots, query);
  }
}

