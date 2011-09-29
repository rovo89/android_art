// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_STL_UTIL_H_
#define ART_SRC_STL_UTIL_H_

namespace art {

// STLDeleteContainerPointers()
//  For a range within a container of pointers, calls delete
//  (non-array version) on these pointers.
// NOTE: for these three functions, we could just implement a DeleteObject
// functor and then call for_each() on the range and functor, but this
// requires us to pull in all of algorithm.h, which seems expensive.
// For hash_[multi]set, it is important that this deletes behind the iterator
// because the hash_set may call the hash function on the iterator when it is
// advanced, which could result in the hash function trying to deference a
// stale pointer.
template <class ForwardIterator>
void STLDeleteContainerPointers(ForwardIterator begin,
                                ForwardIterator end) {
  while (begin != end) {
    ForwardIterator temp = begin;
    ++begin;
    delete *temp;
  }
}

// STLDeleteElements() deletes all the elements in an STL container and clears
// the container.  This function is suitable for use with a vector, set,
// hash_set, or any other STL container which defines sensible begin(), end(),
// and clear() methods.
//
// If container is NULL, this function is a no-op.
//
// As an alternative to calling STLDeleteElements() directly, consider
// ElementDeleter (defined below), which ensures that your container's elements
// are deleted when the ElementDeleter goes out of scope.
template <class T>
void STLDeleteElements(T *container) {
  if (!container) return;
  STLDeleteContainerPointers(container->begin(), container->end());
  container->clear();
}

// Given an STL container consisting of (key, value) pairs, STLDeleteValues
// deletes all the "value" components and clears the container.  Does nothing
// in the case it's given a NULL pointer.
template <class T>
void STLDeleteValues(T *v) {
  if (!v) return;
  for (typename T::iterator i = v->begin(); i != v->end(); ++i) {
    delete i->second;
  }
  v->clear();
}

}  // namespace art

#endif  // ART_SRC_STL_UTIL_H_
