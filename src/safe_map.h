#ifndef ART_SRC_SAFE_MAP_H_
#define ART_SRC_SAFE_MAP_H_

#include <map>

#include "logging.h"

namespace art {

// Equivalent to std::map, but without operator[] and its bug-prone semantics (in particular,
// the implicit insertion of a default-constructed value on failed lookups).
template <typename K, typename V, typename Comparator = std::less<K> >
class SafeMap {
 private:
  typedef SafeMap<K, V, Comparator> Self;

 public:
  typedef typename ::std::map<K, V, Comparator>::iterator iterator;
  typedef typename ::std::map<K, V, Comparator>::const_iterator const_iterator;
  typedef typename ::std::map<K, V, Comparator>::size_type size_type;
  typedef typename ::std::map<K, V, Comparator>::value_type value_type;

  Self& operator=(const Self& rhs) { map_ = rhs.map_; return *this; }

  iterator begin() { return map_.begin(); }
  const_iterator begin() const { return map_.begin(); }
  iterator end() { return map_.end(); }
  const_iterator end() const { return map_.end(); }

  bool empty() const { return map_.empty(); }
  size_type size() const { return map_.size(); }

  void clear() { return map_.clear(); }
  void erase(iterator it) { map_.erase(it); }
  size_type erase(const K& k) { return map_.erase(k); }

  iterator find(const K& k) { return map_.find(k); }
  const_iterator find(const K& k) const { return map_.find(k); }

  size_type count(const K& k) const { return map_.count(k); }

  // Note that unlike std::map's operator[], this doesn't return a reference to the value.
  V Get(const K& k) const {
    const_iterator it = map_.find(k);
    DCHECK(it != map_.end());
    return it->second;
  }

  // Used to insert a new mapping.
  void Put(const K& k, const V& v) {
    std::pair<iterator, bool> result = map_.insert(std::make_pair(k, v));
    DCHECK(result.second); // Check we didn't accidentally overwrite an existing value.
  }

  // Used to insert a new mapping or overwrite an existing mapping. Note that if the value type
  // of this container is a pointer, any overwritten pointer will be lost and if this container
  // was the owner, you have a leak.
  void Overwrite(const K& k, const V& v) {
    map_.insert(std::make_pair(k, v));
  }

  bool Equals(const Self& rhs) const {
    return map_ == rhs.map_;
  }

 private:
  ::std::map<K, V, Comparator> map_;
};

template <typename K, typename V, typename Comparator>
bool operator==(const SafeMap<K, V, Comparator>& lhs, const SafeMap<K, V, Comparator>& rhs) {
  return lhs.Equals(rhs);
}

template <typename K, typename V, typename Comparator>
bool operator!=(const SafeMap<K, V, Comparator>& lhs, const SafeMap<K, V, Comparator>& rhs) {
  return !(lhs == rhs);
}

}  // namespace art

#endif  // ART_SRC_SAFE_MAP_H_
