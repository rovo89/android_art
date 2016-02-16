AHAT - Android Heap Analysis Tool

Usage:
  java -jar ahat.jar [-p port] FILE
    Launch an http server for viewing the given Android heap-dump FILE.

  Options:
    -p <port>
       Serve pages on the given port. Defaults to 7100.

TODO:
 * Add more tips to the help page.
   - Recommend how to start looking at a heap dump.
   - Say how to enable allocation sites.
   - Where to submit feedback, questions, and bug reports.
 * Dim 'image' and 'zygote' heap sizes slightly? Why do we even show these?
 * Let user re-sort sites objects info by clicking column headers.
 * Let user re-sort "Objects" list.
 * Show site context and heap and class filter in "Objects" view?
 * Have a menu at the top of an object view with links to the sections?
 * Include ahat version and hprof file in the menu at the top of the page?
 * Heaped Table
   - Make sortable by clicking on headers.
 * For HeapTable with single heap shown, the heap name isn't centered?
 * Consistently document functions.
 * Show version number with --version.
 * Show somewhere where to send bugs.
 * Include a link to /objects in the overview and menu?
 * Turn on LOCAL_JAVACFLAGS := -Xlint:unchecked -Werror
 * Use hex for object ids in URLs?

 * [low priority] by site allocations won't line up if the stack has been
   truncated. Is there any way to manually line them up in that case?

 * [low priority] Have a switch to choose whether unreachable objects are
   ignored or not?  Is there any interest in what's unreachable, or is it only
   reachable objects that people care about?

 * [low priority] Have a way to diff two heap dumps by site.
   This should be pretty easy to do, actually. The interface is the real
   question. Maybe: augment each byte count field on every page with the diff
   if a baseline has been provided, and allow the user to sort by the diff.

Things to Test:
 * That we can open a hprof without an 'app' heap and show a tabulation of
   objects normally sorted by 'app' heap by default.
 * Visit /objects without parameters and verify it doesn't throw an exception.
 * Visit /objects with an invalid site, verify it doesn't throw an exception.
 * That we can view the list of all objects in a reasonably short amount of
   time.
 * That we don't show the 'extra' column in the DominatedList if we are
   showing all the instances.
 * That InstanceUtils.asString properly takes into account "offset" and
   "count" fields, if they are present.
 * InstanceUtils.getDexCacheLocation

Reported Issues:
 * Request to be able to sort tables by size.

Perflib Requests:
 * Class objects should have java.lang.Class as their class object, not null.
 * ArrayInstance should have asString() to get the string, without requiring a
   length function.
 * Document that getHeapIndex returns -1 for no such heap.
 * Look up totalRetainedSize for a heap by Heap object, not by a separate heap
   index.
 * What's the difference between getId and getUniqueId?
 * I see objects with duplicate references.
 * A way to get overall retained size by heap.
 * A method Instance.isReachable()

Things to move to perflib:
 * Extracting the string from a String Instance.
 * Extracting bitmap data from bitmap instances.
 * Adding up allocations by stack frame.
 * Computing, for each instance, the other instances it dominates.
 * Instance.isRoot and Instance.getRootTypes.

Release History:
 0.4 Pending
   Annotate char[] objects with their string values.
   Show registered native allocations for heap dumps that support it.

 0.3 Dec 15, 2015
   Fix page loading performance by showing a limited number of entries by default.
   Fix mismatch between overview and "roots" totals.
   Annotate root objects and show their types.
   Annotate references with their referents.

 0.2 Oct 20, 2015
   Take into account 'count' and 'offset' when displaying strings.

 0.1ss Aug 04, 2015
   Enable stack allocations code (using custom modified perflib).
   Sort objects in 'objects/' with default sort.

 0.1-stacks Aug 03, 2015
   Enable stack allocations code (using custom modified perflib).

 0.1 July 30, 2015
   Initial Release

