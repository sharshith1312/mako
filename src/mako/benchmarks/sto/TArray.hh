#ifndef MAKO_STO_TARRAY_HH
#define MAKO_STO_TARRAY_HH

#include "TWrapped.hh"
#include "TArrayProxy.hh"

/**
 * @brief Transactional array implementation for STO
 * 
 * A fixed-size array that supports transactional operations.
 * Provides array semantics with ACID properties when used within
 * STO transactions.
 * 
 * @tparam T Element type
 * @tparam N Array size (compile-time constant)
 * @tparam W Wrapper type for versioning (default: TOpaqueWrapped)
 */
template <typename T, unsigned N, template <typename> class W = TOpaqueWrapped>
class TArray : public TObject {
public:
    class iterator;
    class const_iterator;
    typedef T value_type;
    typedef typename W<T>::read_type get_type;
    typedef typename W<T>::version_type version_type;
    typedef unsigned size_type;
    typedef int difference_type;
    typedef TConstArrayProxy<TArray<T, N, W> > const_proxy_type;
    typedef TArrayProxy<TArray<T, N, W> > proxy_type;

    /**
     * @brief Get the size of the array
     * @return Array size (compile-time constant N)
     */
    size_type size() const {
        return N;
    }

    /**
     * @brief Access element at index (const version)
     * @param index Index to access
     * @return Const proxy for transactional access
     */
    const_proxy_type operator[](size_type index) const {
        assert(index < N);
        return const_proxy_type(this, index);
    }
    
    /**
     * @brief Access element at index (mutable version)
     * @param index Index to access
     * @return Proxy for transactional access
     */
    proxy_type operator[](size_type index) {
        assert(index < N);
        return proxy_type(this, index);
    }

    inline iterator begin();
    inline iterator end();
    inline const_iterator cbegin() const;
    inline const_iterator cend() const;
    inline const_iterator begin() const;
    inline const_iterator end() const;

    /**
     * @brief Transactional read of element at index
     * @param index Index to read from
     * @return Element value
     */
    get_type transGet(size_type index) const {
        assert(index < N);
        auto item = Sto::item(this, index);
        if (item.has_write())
            return item.template write_value<T>();
        else
            return data_[index].v.read(item, data_[index].vers);
    }
    
    /**
     * @brief Transactional write of element at index
     * @param index Index to write to
     * @param value Value to write
     */
    void transPut(size_type index, T value) const {
        assert(index < N);
        Sto::item(this, index).add_write(value);
    }

    /**
     * @brief Non-transactional read of element at index
     * @param index Index to read from
     * @return Element value (unsafe - no transaction protection)
     */
    get_type nontrans_get(size_type index) const {
        assert(index < N);
        return data_[index].v.access();
    }
    
    /**
     * @brief Non-transactional write of element at index (copy)
     * @param index Index to write to
     * @param value Value to write
     */
    void nontrans_put(size_type index, const T& value) {
        assert(index < N);
        data_[index].v.access() = value;
    }
    
    /**
     * @brief Non-transactional write of element at index (move)
     * @param index Index to write to
     * @param value Value to move
     */
    void nontrans_put(size_type index, T&& value) {
        assert(index < N);
        data_[index].v.access() = std::move(value);
    }

    // Transactional interface methods
    bool lock(TransItem& item, Transaction& txn) override {
        return txn.try_lock(item, data_[item.key<size_type>()].vers);
    }
    bool check(TransItem& item, Transaction&) override {
        return item.check_version(data_[item.key<size_type>()].vers);
    }
    void install(TransItem& item, Transaction& txn) override {
        size_type index = item.key<size_type>();
        data_[index].v.write(item.write_value<T>());
        txn.set_version_unlock(data_[index].vers, item);
    }
    void unlock(TransItem& item) override {
        data_[item.key<size_type>()].vers.unlock();
    }

private:
    /**
     * @brief Array element with version and wrapped value
     */
    struct elem {
        version_type vers;
        W<T> v;
    };
    elem data_[N];

    friend class iterator;
    friend class const_iterator;
};


template <typename T, unsigned N, template <typename> class W>
class TArray<T, N, W>::const_iterator : public std::iterator<std::random_access_iterator_tag, T> {
public:
    typedef TArray<T, N, W> array_type;
    typedef typename array_type::size_type size_type;
    typedef typename array_type::difference_type difference_type;

    const_iterator(const TArray<T, N, W>* a, size_type i)
        : a_(const_cast<array_type*>(a)), i_(i) {
    }

    typename array_type::const_proxy_type operator*() const {
        return array_type::const_proxy_type(a_, i_);
    }

    bool operator==(const const_iterator& x) const {
        return a_ == x.a_ && i_ == x.i_;
    }
    bool operator!=(const const_iterator& x) const {
        return !(*this == x);
    }
    bool operator<(const const_iterator& x) const {
        assert(a_ == x.a_);
        return i_ < x.i_;
    }
    bool operator<=(const const_iterator& x) const {
        assert(a_ == x.a_);
        return i_ <= x.i_;
    }
    bool operator>(const const_iterator& x) const {
        assert(a_ == x.a_);
        return i_ > x.i_;
    }
    bool operator>=(const const_iterator& x) const {
        assert(a_ == x.a_);
        return i_ >= x.i_;
    }

    const_iterator& operator+=(difference_type delta) {
        i_ += delta;
        return *this;
    }
    const_iterator& operator-=(difference_type delta) {
        i_ += delta;
        return *this;
    }
    const_iterator operator+(difference_type delta) const {
        return const_iterator(a_, i_ + delta);
    }
    const_iterator operator-(difference_type delta) const {
        return const_iterator(a_, i_ - delta);
    }
    const_iterator& operator++() {
        ++i_;
        return *this;
    }
    const_iterator operator++(int) {
        ++i_;
        return const_iterator(a_, i_ - 1);
    }
    const_iterator& operator--() {
        --i_;
        return *this;
    }
    const_iterator operator--(int) {
        --i_;
        return const_iterator(a_, i_ + 1);
    }

    difference_type operator-(const const_iterator& x) const {
        assert(a_ == x.a_);
        return i_ - x.i_;
    }

protected:
    array_type* a_;
    size_type i_;
};

template <typename T, unsigned N, template <typename> class W>
class TArray<T, N, W>::iterator : public const_iterator {
public:
    typedef TArray<T, N, W> array_type;
    typedef typename array_type::size_type size_type;
    typedef typename array_type::difference_type difference_type;

    iterator(const TArray<T, N, W>* a, size_type i)
        : const_iterator(a, i) {
    }

    typename array_type::proxy_type operator*() const {
        return array_type::proxy_type(this->a_, this->i_);
    }

    iterator& operator+=(difference_type delta) {
        this->i_ += delta;
        return *this;
    }
    iterator& operator-=(difference_type delta) {
        this->i_ += delta;
        return *this;
    }
    iterator operator+(difference_type delta) const {
        return iterator(this->a_, this->i_ + delta);
    }
    iterator operator-(difference_type delta) const {
        return iterator(this->a_, this->i_ - delta);
    }
    iterator& operator++() {
        ++this->i_;
        return *this;
    }
    iterator operator++(int) {
        ++this->i_;
        return iterator(this->a_, this->i_ - 1);
    }
    iterator& operator--() {
        --this->i_;
        return *this;
    }
    iterator operator--(int) {
        --this->i_;
        return iterator(this->a_, this->i_ + 1);
    }
};

template <typename T, unsigned N, template <typename> class W>
inline auto TArray<T, N, W>::begin() -> iterator {
    return iterator(this, 0);
}

template <typename T, unsigned N, template <typename> class W>
inline auto TArray<T, N, W>::end() -> iterator {
    return iterator(this, N);
}

template <typename T, unsigned N, template <typename> class W>
inline auto TArray<T, N, W>::cbegin() const -> const_iterator {
    return const_iterator(this, 0);
}

template <typename T, unsigned N, template <typename> class W>
inline auto TArray<T, N, W>::cend() const -> const_iterator {
    return const_iterator(this, N);
}

template <typename T, unsigned N, template <typename> class W>
inline auto TArray<T, N, W>::begin() const -> const_iterator {
    return const_iterator(this, 0);
}

template <typename T, unsigned N, template <typename> class W>
inline auto TArray<T, N, W>::end() const -> const_iterator {
    return const_iterator(this, N);
}

#endif /* MAKO_STO_TARRAY_HH */
