#ifndef MAKO_STO_PRIORITY_QUEUE_HH
#define MAKO_STO_PRIORITY_QUEUE_HH

#include <vector>
#include "TaggedLow.hh"
#include "Transaction.hh"
#include "versioned_value.hh"


namespace mako {
namespace sto {
namespace constants {
    constexpr int PQ_POP_KEY = -2;
    constexpr int PQ_EMPTY_KEY = -3;
    constexpr int PQ_TOP_KEY = -4;
    constexpr int PQ_INVALID_VALUE = -1;
    constexpr double LOG_BASE_2 = 2.0;
}
}
}

/**
 * @brief Transactional priority queue implementation for STO
 * 
 * A max-heap based priority queue that supports transactional operations.
 * Provides priority queue semantics with ACID properties when used within
 * STO transactions.
 * 
 * @tparam T Element type (must be comparable)
 * @tparam Opacity Whether to use opacity checking for consistency
 */
template <typename T, bool Opacity = false>
class PriorityQueue: public TObject {
    typedef TransactionTid::type Version;
    typedef versioned_value_struct<T> versioned_value;
    
    static constexpr TransItem::flags_type insert_tag = TransItem::user0_bit;
    static constexpr TransItem::flags_type delete_tag = TransItem::user0_bit<<1;
    static constexpr TransItem::flags_type dirty_tag = TransItem::user0_bit<<2;

    static constexpr Version insert_bit = TransactionTid::user_bit; // XXX get rid of this
    static constexpr Version delete_bit = TransactionTid::user_bit<<1; // XXX get rid of this
    static constexpr Version dirty_bit = TransactionTid::user_bit<<2; // XXX get rid of this

    static constexpr int pop_key = mako::sto::constants::PQ_POP_KEY;
    static constexpr int empty_key = mako::sto::constants::PQ_EMPTY_KEY;
    static constexpr int top_key = mako::sto::constants::PQ_TOP_KEY;
public:
    /**
     * @brief Construct an empty priority queue
     */
    PriorityQueue() : heap_() {
        size_ = 0;
        poplock_ = 0;
        popversion_ = 0;
        dirtytid_ = mako::sto::constants::PQ_INVALID_VALUE;
        dirtyval_ = mako::sto::constants::PQ_INVALID_VALUE;
        dirtycount_ = 0;
    }

    /**
     * @brief Add a versioned value to the priority queue (maintains max-heap property)
     * @param value Versioned value to add
     */
    void add(versioned_value* value) {
        int child_index = size_;
        if (child_index >= static_cast<int>(heap_.size())) {
            heap_.push_back(value);
        } else {
            heap_[child_index] = value;
        }
        size_++;

        // Bubble up to maintain max-heap property
        while (child_index > 0) {
            int parent_index = (child_index - 1) / 2;
            versioned_value* parent_value = heap_[parent_index];
            
            if (heap_[child_index]->read_value() > parent_value->read_value()) {
                swap(child_index, parent_index);
                child_index = parent_index;
            } else {
                return;
            }
        }
    }
    
    /**
     * @brief Remove the maximum element from the heap
     * @param expected_value Optional expected value for validation
     * @return Pointer to the removed maximum element, or nullptr if empty
     */
    versioned_value* removeMax(versioned_value* expected_value = nullptr) {
        int last_index = --size_;
        if (last_index < 0) {
            return nullptr;
        }
        if (last_index == 0) {
            versioned_value* result = heap_[0];
            return result;
        }
        
        versioned_value* result = heap_[0];

        if (expected_value != nullptr && result != expected_value) {
            unlock(&poplock_);
            Sto::abort();
            return nullptr;
        }
        swap(last_index, 0);
        
        // Bubble down to maintain max-heap property
        int parent_index = 0;
        while (2 * parent_index < size_ - 1) {
            int left_child = parent_index * 2 + 1;
            int right_child = (parent_index * 2) + 2;
            
            if (right_child >= size_) {
                if (left_child >= size_) {
                    break;
                }
                if (heap_[left_child]->read_value() > heap_[parent_index]->read_value()) {
                    swap(parent_index, left_child);
                    parent_index = left_child;
                } else {
                    break;
                }
            } else {
                int larger_child = (heap_[left_child]->read_value() > heap_[right_child]->read_value()) 
                                   ? left_child : right_child;
                
                if (heap_[larger_child]->read_value() > heap_[parent_index]->read_value()) {
                    swap(parent_index, larger_child);
                    parent_index = larger_child;
                } else {
                    break;
                }
            }
        }
        return result;
    }
    
    /**
     * @brief Get the maximum element (internal helper)
     * @return Pointer to maximum element, or nullptr if empty
     */
    versioned_value* getMax() {
        assert(TransactionTid::is_locked_here(poplock_));
        if (size_ == 0) {
            return nullptr;
        }
        
        while (true) {
            versioned_value* max_value = heap_[0];
            auto item = Sto::item(this, max_value);
            
            if (is_inserted(max_value->version())) {
                if (has_insert(item)) {
                    // Push then pop operation
                    return max_value;
                } else {
                    // Another transaction is inserting a high-priority node
                    unlock(&poplock_);
                    Sto::abort();
                    return nullptr;
                }
            } else if (is_deleted(max_value->version())) {
                removeMax(max_value);
                if (size_ == 0) {
                    return nullptr;
                }
            } else {
                return max_value;
            }
        }
    }
    
    void push_nontrans(T v) {
        lock(&poplock_);
        versioned_value* val = versioned_value::make(v, TransactionTid::increment_value + insert_bit);
        add(val);
        unlock(&poplock_);
    }
    
    /**
     * @brief Transactional push operation
     * @param value Element to push onto the priority queue
     */
    void push(T value) {
        lock(&poplock_); // TODO: locking this is not required, but performance seems to be better with this
                        // Can also try readers-writers lock
        if (dirtytid_ != mako::sto::constants::PQ_INVALID_VALUE && 
            dirtytid_ != TThread::id() && 
            value > dirtyval_) {
            unlock(&poplock_);
            Sto::abort();
            return;
        }
        
        versioned_value* versioned_val = versioned_value::make(value, TransactionTid::increment_value + insert_bit);
        add(versioned_val);
        Sto::item(this, versioned_val).add_write(value).add_flags(insert_tag);
        unlock(&poplock_);
    }
    
    /**
     * @brief Transactional pop operation (remove and return maximum element)
     * @return Maximum element value, or PQ_INVALID_VALUE if empty
     */
    T pop() {
        // Check if we previously read the top element
        auto top_item = Sto::check_item(this, top_key);
        versioned_value* read_val = nullptr;
        if (top_item != nullptr && top_item->has_read()) {
            read_val = (*top_item).template read_value<versioned_value*>();
        }
        // Check if we previously saw the queue as empty
        auto empty_item = Sto::check_item(this, empty_key);
        bool read_empty = empty_item != nullptr && empty_item->has_read();
        
        if (size_ == 0) {
            if (read_val != nullptr) {
                Sto::abort();
            }
            else Sto::item(this, empty_key).add_read(0);
            // XXX opacity
            Sto::item(this, pop_key).add_read(TransactionTid::unlocked(popversion_));
            return mako::sto::constants::PQ_INVALID_VALUE;
        }
        
        lock(&poplock_);
        if (dirtytid_ != mako::sto::constants::PQ_INVALID_VALUE && dirtytid_ != TThread::id()) {
            // Queue is in dirty state
            unlock(&poplock_);
            Sto::abort();
            return mako::sto::constants::PQ_INVALID_VALUE;
        }
        
        versioned_value* max_val = getMax();
        // If we already read the top value, then either max_val = read_val or max_val is pushed by the current transaction
        bool should_be_inserted = false;
        if (read_empty && max_val != nullptr) {
            should_be_inserted = true;
        }
        if (read_val != nullptr && read_val->read_value() == max_val->read_value()) { // TODO: Should we compare values or versioned_values?
            top_item->remove_read();
        } else if (read_val != nullptr) {
            should_be_inserted = true;
        }
        
        auto item = Sto::item(this, max_val);
        if (should_be_inserted && !has_insert(item)) {
            unlock(&poplock_);
            Sto::abort();
            return mako::sto::constants::PQ_INVALID_VALUE;
        }
        
        if (max_val == nullptr) {
            Sto::item(this, empty_key).add_read(0);
            Sto::item(this, pop_key).add_read(TransactionTid::unlocked(popversion_));
            unlock(&poplock_);
            return mako::sto::constants::PQ_INVALID_VALUE;
        }
        
        if (dirtytid_ == mako::sto::constants::PQ_INVALID_VALUE || max_val->read_value() < dirtyval_) {
            dirtyval_ = max_val->read_value();
            fence();
        }
        dirtytid_ = TThread::id();
        
        removeMax(max_val);
        unlock(&poplock_);
        
        if (has_insert(item)) {
            item.add_flags(delete_tag);
        } else {
            item.add_write(0).add_flags(delete_tag);
            dirtycount_++;
        }
        
        Sto::item(this, pop_key).add_write(0);
        return max_val->read_value();
    }
    
    /**
     * @brief Transactional top operation (peek at maximum element)
     * @return Maximum element value, or PQ_INVALID_VALUE if empty
     */
    T top() {
        if (size_ == 0) {
            Sto::item(this, empty_key).add_read(0);
            return mako::sto::constants::PQ_INVALID_VALUE;
        }
        
        Sto::item(this, pop_key).add_read(TransactionTid::unlocked(popversion_));
        acquire_fence();
        if (size_ == 0) {
            Sto::item(this, empty_key).add_read(0);
            return mako::sto::constants::PQ_INVALID_VALUE;
        }
        
        lock(&poplock_);
        if (dirtytid_ != mako::sto::constants::PQ_INVALID_VALUE && dirtytid_ != TThread::id()) {
            // Queue is in dirty state
            unlock(&poplock_);
            Sto::abort();
        }
        versioned_value* max_val = getMax();
        unlock(&poplock_);
        
        if (max_val == nullptr) {
            Sto::item(this, empty_key).add_read(0);
            return mako::sto::constants::PQ_INVALID_VALUE;
        }
        
        T return_value = max_val->read_value();
        Sto::item(this, max_val).add_read(max_val->version());
        Sto::item(this, top_key).add_read(max_val);
        return return_value;
    }
    
    int unsafe_size() {
        return size_; // TODO: this is not transactional yet
    }
    
    void lock(versioned_value *e) {
        lock(&e->version());
    }
    void unlock(versioned_value *e) {
        unlock(&e->version());
    }
    
    bool lock(TransItem& item, Transaction& txn) override {
        return item.key<int>() != pop_key
            || txn.try_lock(item, popversion_);
    }
    
    bool check(TransItem& item, Transaction&) override {
        if (item.key<int>() == top_key) { return true; }
        else if (item.key<int>() == empty_key) {
            // check that no other transaction  pushed items onto the queue
            for (int i = 0; i < size_; i++) {
                versioned_value* val = heap_[i];
                if (!is_inserted(val->version())
                    || TransactionTid::is_locked_elsewhere(val->version()))
                    return false;
            }
            
            if (dirtytid_ != -1 && dirtytid_ != TThread::id()) return false;
            return true;
        }
        else if (item.key<int>() == pop_key) {
            return TransactionTid::check_version(popversion_, item.template read_value<Version>());
        } else {
            // This is top case
            auto e = item.key<versioned_value*>();
            if (dirtytid_ != -1 && dirtytid_ != TThread::id() && dirtyval_ >= e->read_value()) return false;
            else if (has_delete(item)) return true;
            // check that e is not pushed down by other transactions
            int level = 1; // level that contains the root
            bool found = false;
            for (int i = 0; i < size_; i++) {
                versioned_value* val = heap_[i];
                if (val == e || val->read_value() == e->read_value()) found = true; 
                else if (val->read_value() > e->read_value()) {
                    auto it = Sto::check_item(this, val);
                    if (it != NULL && has_insert(*it)) {
                        level = findLevel(i) + 1;
                        continue;
                    } else {
                        return false;
                    }
                }
                if (i == endOfLevel(level)) break;
            }
            if (dirtytid_ != -1 && dirtytid_ != TThread::id() && dirtyval_ >= e->read_value()) return false;
            if (!found) return false;
            else return true;
        }
    }
    
    
    void install(TransItem& item, Transaction& t) override {
        if (item.key<int>() == pop_key){
            if (Opacity) {
                TransactionTid::set_version(popversion_, t.commit_tid());
            } else {
                TransactionTid::inc_nonopaque_version(popversion_);
            }
        } else {
            auto e = item.key<versioned_value*>();
            if (has_insert(item)) {
                erase_inserted(&e->version());
            }
        }
    }
    
    void unlock(TransItem& item) override {
        if (item.key<int>() == pop_key)
            unlock(&popversion_);
    }

    void cleanup(TransItem& item, bool committed) override {
        if (committed && dirtytid_ == TThread::id()) {
            dirtytid_ = mako::sto::constants::PQ_INVALID_VALUE;
        }
        if (!committed) {
            if (has_insert(item) && has_delete(item)) {
                // Insert and delete cancel out - do nothing
                return;
            }
            if (has_insert(item)) {
                auto element = item.key<versioned_value*>();
                mark_deleted(&element->version());
                fence();
                erase_inserted(&element->version());
            } else if (has_delete(item)) {
                auto element = item.key<versioned_value*>();
                auto value = element->read_value();
                versioned_value* restored_val = versioned_value::make(value, TransactionTid::increment_value);
                lock(&poplock_);
                add(restored_val);
                unlock(&poplock_);
                fence();
                dirtycount_--;
                if (dirtycount_ == 0) {
                    assert(dirtytid_ == TThread::id());
                    dirtytid_ = mako::sto::constants::PQ_INVALID_VALUE;
                }
            }
        }
    }
    
    // Used for debugging
    void print() {
        for (int i =0; i < size_; i++) {
            std::cout << heap_[i]->read_value() << "[" << (!is_inserted(heap_[i]->version()) && !is_deleted(heap_[i]->version())) << "] ";
        }
        std::cout << std::endl;
    }
    
    
private:
    static void lock(Version *v) {
        TransactionTid::lock(*v);
    }
    
    static void unlock(Version *v) {
        TransactionTid::unlock(*v);
    }
    
    static bool has_insert(const TransItem& item) {
        return item.flags() & insert_tag;
    }
    static bool has_delete(const TransItem& item) {
        return item.flags() & delete_tag;
    }
    
    static bool has_dirty(const TransItem& item) {
        return item.flags() & dirty_tag;
    }
    
    static bool is_inserted(Version v) {
        return v & insert_bit;
    }
    
    static void erase_inserted(Version* v) {
        *v = *v & (~insert_bit);
    }
    
    static void mark_inserted(Version* v) {
        *v = *v | insert_bit;
    }
    
    static bool is_dirty(Version v) {
        return v & dirty_bit;
    }
    
    static void erase_dirty(Version* v) {
        assert(is_dirty(*v));
        *v = *v & (~dirty_bit);
    }
    
    static void mark_dirty(Version* v) {
        assert(!is_dirty(*v));
        *v = *v | dirty_bit;
    }
            
    static bool is_deleted(Version v) {
        return v & delete_bit;
    }
            
    static void erase_deleted(Version* v) {
        *v = *v & (~delete_bit);
    }
            
    static void mark_deleted(Version* v) {
        *v = *v | delete_bit;
    }
    
    /**
     * @brief Find the level of a heap element at given index
     * @param index Index in the heap array
     * @return Level number (1-based)
     */
    static int findLevel(int index) {
        return static_cast<int>(ceil(log(static_cast<double>(index + 2)) / log(mako::sto::constants::LOG_BASE_2)));
    }
    
    /**
     * @brief Get the last index of a given level in the heap
     * @param level Level number (1-based)
     * @return Last index of the level
     */
    static int endOfLevel(int level) {
        assert(level >= 1);
        return (1 << level) - 2;
    }


    void swap(int i, int j) {
        versioned_value* tmp = heap_[i];
        heap_[i] = heap_[j];
        heap_[j] = tmp;
    }
    
    std::vector<versioned_value *> heap_;
    Version poplock_;
    Version popversion_;
    int size_;
    int dirtyval_; // min value popped by a transaction that dirtied the queue
    int dirtytid_; // thread id of the transaction that dirtied the queue
    int dirtycount_; // number of pops by the transaction that dirtied the queue
    
    
};

#endif /* MAKO_STO_PRIORITY_QUEUE_HH */
