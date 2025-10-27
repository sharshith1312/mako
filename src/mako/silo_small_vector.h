#ifndef MAKO_SILO_SMALL_VECTOR_H
#define MAKO_SILO_SMALL_VECTOR_H

#include <algorithm>
#include <vector>
#include <type_traits>

#include "macros.h"
#include "masstree/compiler.hh"

namespace mako {
namespace silo {

// Default small buffer size for silo_small_vector (from macros.h SMALL_SIZE_VEC)
constexpr size_t DEFAULT_SMALL_VECTOR_SIZE = 128;

/**
 * @brief Small vector optimization container
 * 
 * A vector-like container that stores small numbers of elements inline
 * to avoid heap allocation. When the number of elements exceeds SmallSize,
 * it automatically switches to using std::vector for storage.
 * 
 * References are not guaranteed to be stable across mutation.
 * 
 * @tparam T Element type
 * @tparam SmallSize Number of elements to store inline before switching to heap
 */
template <typename T, size_t SmallSize = DEFAULT_SMALL_VECTOR_SIZE>
class silo_small_vector {
  typedef std::vector<T> large_vector_type;

  static const bool is_trivially_destructible =
    mass::is_trivially_destructible<T>::value;

  // std::is_trivially_copyable not supported in g++-4.7
  static const bool is_trivially_copyable = std::is_scalar<T>::value;

public:

  typedef T value_type;
  typedef T & reference;
  typedef const T & const_reference;
  typedef size_t size_type;

  silo_small_vector() : small_size_(0), large_elems(0) {}
  ~silo_small_vector()
  {
    clearDestructive();
  }

  silo_small_vector(const silo_small_vector &that)
    : small_size_(0), large_elems(0)
  {
    assignFrom(that);
  }

  // not efficient, don't use in performance critical parts
  silo_small_vector(std::initializer_list<T> l)
    : small_size_(0), large_elems(nullptr)
  {
    if (l.size() > SmallSize) {
      large_elems = new large_vector_type(l);
    } else {
      for (auto &p : l)
        push_back(p);
    }
  }

  silo_small_vector &
  operator=(const silo_small_vector &that)
  {
    assignFrom(that);
    return *this;
  }

  inline size_t
  size() const
  {
    if (unlikely(large_elems))
      return large_elems->size();
    return small_size_;
  }

  inline bool
  empty() const
  {
    return size() == 0;
  }

  inline reference
  front()
  {
    if (unlikely(large_elems))
      return large_elems->front();
    INVARIANT(small_size_ > 0);
    INVARIANT(small_size_ <= SmallSize);
    return *ptr();
  }

  inline const_reference
  front() const
  {
    return const_cast<silo_small_vector *>(this)->front();
  }

  inline reference
  back()
  {
    if (unlikely(large_elems))
      return large_elems->back();
    INVARIANT(small_size_ > 0);
    INVARIANT(small_size_ <= SmallSize);
    return ptr()[small_size_ - 1];
  }

  inline const_reference
  back() const
  {
    return const_cast<silo_small_vector *>(this)->back();
  }

  inline void
  pop_back()
  {
    if (unlikely(large_elems)) {
      large_elems->pop_back();
      return;
    }
    INVARIANT(small_size_ > 0);
    if (!is_trivially_destructible)
      ptr()[small_size_ - 1].~T();
    small_size_--;
  }

  inline void
  push_back(const T &obj)
  {
    emplace_back(obj);
  }

  inline void
  push_back(T &&obj)
  {
    emplace_back(std::move(obj));
  }

  // C++11 goodness- a strange syntax this is

  template <class... Args>
  inline void
  emplace_back(Args &&... args)
  {
    if (unlikely(large_elems)) {
      INVARIANT(!small_size_);
      large_elems->emplace_back(std::forward<Args>(args)...);
      return;
    }
    if (unlikely(small_size_ == SmallSize)) {
      large_elems = new large_vector_type(ptr(), ptr() + small_size_);
      large_elems->emplace_back(std::forward<Args>(args)...);
      small_size_ = 0;
      return;
    }
    INVARIANT(small_size_ < SmallSize);
    new (&(ptr()[small_size_++])) T(std::forward<Args>(args)...);
  }

  inline reference
  operator[](int i)
  {
    if (unlikely(large_elems))
      return large_elems->operator[](i);
    return ptr()[i];
  }

  inline const_reference
  operator[](int i) const
  {
    return const_cast<silo_small_vector *>(this)->operator[](i);
  }

  void
  clear()
  {
    if (unlikely(large_elems)) {
      INVARIANT(!small_size_);
      large_elems->clear();
      return;
    }
    if (!is_trivially_destructible)
      for (size_t i = 0; i < small_size_; i++)
        ptr()[i].~T();
    small_size_ = 0;
  }

  inline void
  reserve(size_t n)
  {
    if (unlikely(large_elems))
      large_elems->reserve(n);
  }

  // non-standard API
  inline bool is_small_type() const { return !large_elems; }

  template <typename Compare = std::less<T>>
  inline void
  sort(Compare c = Compare())
  {
    if (unlikely(large_elems))
      std::sort(large_elems->begin(), large_elems->end(), c);
    else
      std::sort(small_begin(), small_end(), c);
  }

private:

  void
  clearDestructive()
  {
    if (unlikely(large_elems)) {
      INVARIANT(!small_size_);
      delete large_elems;
      large_elems = nullptr;
      return;
    }
    if (!is_trivially_destructible)
      for (size_t i = 0; i < small_size_; i++)
        ptr()[i].~T();
    small_size_ = 0;
  }

  template <typename ObjType>
  class small_iterator_ : public std::iterator<std::bidirectional_iterator_tag, ObjType> {
    friend class silo_small_vector;
  public:
    inline small_iterator_() : p(0) {}

    template <typename O>
    inline small_iterator_(const small_iterator_<O> &other)
      : p(other.p)
    {}

    inline ObjType &
    operator*() const
    {
      return *p;
    }

    inline ObjType *
    operator->() const
    {
      return p;
    }

    inline bool
    operator==(const small_iterator_ &o) const
    {
      return p == o.p;
    }

    inline bool
    operator!=(const small_iterator_ &o) const
    {
      return !operator==(o);
    }

    inline bool
    operator<(const small_iterator_ &o) const
    {
      return p < o.p;
    }

    inline bool
    operator>=(const small_iterator_ &o) const
    {
      return !operator<(o);
    }

    inline bool
    operator>(const small_iterator_ &o) const
    {
      return p > o.p;
    }

    inline bool
    operator<=(const small_iterator_ &o) const
    {
      return !operator>(o);
    }

    inline small_iterator_ &
    operator+=(int n)
    {
      p += n;
      return *this;
    }

    inline small_iterator_ &
    operator-=(int n)
    {
      p -= n;
      return *this;
    }

    inline small_iterator_
    operator+(int n) const
    {
      small_iterator_ cpy = *this;
      return cpy += n;
    }

    inline small_iterator_
    operator-(int n) const
    {
      small_iterator_ cpy = *this;
      return cpy -= n;
    }

    inline intptr_t
    operator-(const small_iterator_ &o) const
    {
      return p - o.p;
    }

    inline small_iterator_ &
    operator++()
    {
      ++p;
      return *this;
    }

    inline small_iterator_
    operator++(int)
    {
      small_iterator_ cur = *this;
      ++(*this);
      return cur;
    }

    inline small_iterator_ &
    operator--()
    {
      --p;
      return *this;
    }

    inline small_iterator_
    operator--(int)
    {
      small_iterator_ cur = *this;
      --(*this);
      return cur;
    }

  protected:
    inline small_iterator_(ObjType *p) : p(p) {}

  private:
    ObjType *p;
  };

  template <typename ObjType, typename SmallTypeIter, typename LargeTypeIter>
  class iterator_ : public std::iterator<std::bidirectional_iterator_tag, ObjType> {
    friend class silo_small_vector;
  public:
    inline iterator_() : large(false) {}

    template <typename O, typename S, typename L>
    inline iterator_(const iterator_<O, S, L> &other)
      : large(other.large),
        small_it(other.small_it),
        large_it(other.large_it)
    {}

    inline ObjType &
    operator*() const
    {
      if (unlikely(large))
        return *large_it;
      return *small_it;
    }

    inline ObjType *
    operator->() const
    {
      if (unlikely(large))
        return &(*large_it);
      return &(*small_it);
    }

    inline bool
    operator==(const iterator_ &o) const
    {
      if (unlikely(large))
        return large_it == o.large_it;
      return small_it == o.small_it;
    }

    inline bool
    operator!=(const iterator_ &o) const
    {
      return !operator==(o);
    }

    inline bool
    operator<(const iterator_ &o) const
    {
      if (unlikely(large))
        return large_it < o.large_it;
      return small_it < o.small_it;
    }

    inline bool
    operator>=(const iterator_ &o) const
    {
      return !operator<(o);
    }

    inline bool
    operator>(const iterator_ &o) const
    {
      if (unlikely(large))
        return large_it > o.large_it;
      return small_it > o.small_it;
    }

    inline bool
    operator<=(const iterator_ &o) const
    {
      return !operator>(o);
    }

    inline iterator_ &
    operator+=(int n)
    {
      if (unlikely(large))
        large_it += n;
      else
        small_it += n;
      return *this;
    }

    inline iterator_ &
    operator-=(int n)
    {
      if (unlikely(large))
        large_it -= n;
      else
        small_it -= n;
      return *this;
    }

    inline iterator_
    operator+(int n) const
    {
      iterator_ cpy = *this;
      return cpy += n;
    }

    inline iterator_
    operator-(int n) const
    {
      iterator_ cpy = *this;
      return cpy -= n;
    }

    inline intptr_t
    operator-(const iterator_ &o) const
    {
      if (unlikely(large))
        return large_it - o.large_it;
      else
        return small_it - o.small_it;
    }

    inline iterator_ &
    operator++()
    {
      if (unlikely(large))
        ++large_it;
      else
        ++small_it;
      return *this;
    }

    inline iterator_
    operator++(int)
    {
      iterator_ cur = *this;
      ++(*this);
      return cur;
    }

    inline iterator_ &
    operator--()
    {
      if (unlikely(large))
        --large_it;
      else
        --small_it;
      return *this;
    }

    inline iterator_
    operator--(int)
    {
      iterator_ cur = *this;
      --(*this);
      return cur;
    }

  protected:
    iterator_(SmallTypeIter small_it)
      : large(false), small_it(small_it), large_it() {}
    iterator_(LargeTypeIter large_it)
      : large(true), small_it(), large_it(large_it) {}

  private:
    bool large;
    SmallTypeIter small_it;
    LargeTypeIter large_it;
  };

  typedef small_iterator_<T> small_iterator;
  typedef small_iterator_<const T> const_small_iterator;
  typedef typename large_vector_type::iterator large_iterator;
  typedef typename large_vector_type::const_iterator const_large_iterator;

  inline small_iterator
  small_begin()
  {
    INVARIANT(!large_elems);
    return small_iterator(ptr());
  }

  inline const_small_iterator
  small_begin() const
  {
    INVARIANT(!large_elems);
    return const_small_iterator(ptr());
  }

  inline small_iterator
  small_end()
  {
    INVARIANT(!large_elems);
    return small_iterator(ptr() + small_size_);
  }

  inline const_small_iterator
  small_end() const
  {
    INVARIANT(!large_elems);
    return const_small_iterator(ptr() + small_size_);
  }

public:

  typedef iterator_<T, small_iterator, large_iterator> iterator;
  typedef iterator_<const T, const_small_iterator, const_large_iterator> const_iterator;

  typedef std::reverse_iterator<iterator> reverse_iterator;
  typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

  inline iterator
  begin()
  {
    if (unlikely(large_elems))
      return iterator(large_elems->begin());
    return iterator(small_begin());
  }

  inline const_iterator
  begin() const
  {
    if (unlikely(large_elems))
      return const_iterator(large_elems->begin());
    return const_iterator(small_begin());
  }

  inline iterator
  end()
  {
    if (unlikely(large_elems))
      return iterator(large_elems->end());
    return iterator(small_end());
  }

  inline const_iterator
  end() const
  {
    if (unlikely(large_elems))
      return const_iterator(large_elems->end());
    return const_iterator(small_end());
  }

  inline reverse_iterator
  rbegin()
  {
    return reverse_iterator(end());
  }

  inline const_reverse_iterator
  rbegin() const
  {
    return const_reverse_iterator(end());
  }

  inline reverse_iterator
  rend()
  {
    return reverse_iterator(begin());
  }

  inline const_reverse_iterator
  rend() const
  {
    return const_reverse_iterator(begin());
  }

private:
  void
  assignFrom(const silo_small_vector &that)
  {
    if (unlikely(this == &that))
      return;
    clearDestructive();
    if (unlikely(that.large_elems)) {
      large_elems = new large_vector_type(*that.large_elems);
    } else {
      INVARIANT(that.small_size_ <= SmallSize);
      if (is_trivially_copyable) {
        NDB_MEMCPY(ptr(), that.ptr(), that.small_size_ * sizeof(T));
      } else {
        for (size_t i = 0; i < that.small_size_; i++)
          new (&(ptr()[i])) T(that.ptr()[i]);
      }
      small_size_ = that.small_size_;
    }
  }

  inline ALWAYS_INLINE T *
  ptr()
  {
    return reinterpret_cast<T *>(&small_elems_buf[0]);
  }

  inline ALWAYS_INLINE const T *
  ptr() const
  {
    return reinterpret_cast<const T *>(&small_elems_buf[0]);
  }

  size_t small_size_;
  char small_elems_buf[sizeof(T) * SmallSize];
  large_vector_type *large_elems;
};

} // namespace silo
} // namespace mako

#endif // MAKO_SILO_SMALL_VECTOR_H
