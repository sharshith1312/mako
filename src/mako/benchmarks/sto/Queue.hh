#ifndef MAKO_STO_QUEUE_HH
#define MAKO_STO_QUEUE_HH

#include <list>
#include "TaggedLow.hh"
#include "Transaction.hh"
#include "TWrapped.hh"

namespace mako {
namespace sto {
namespace constants {
    constexpr unsigned DEFAULT_QUEUE_BUFFER_SIZE = 1000000;
    constexpr int PUSH_ITEM_KEY = -1;
    constexpr int HEAD_LOCK_KEY = -2;
}
}
}

/**
 * @brief Transactional queue implementation for STO
 * 
 * A circular buffer-based queue that supports transactional operations.
 * Provides FIFO semantics with ACID properties when used within STO transactions.
 * 
 * @tparam T Element type
 * @tparam BUF_SIZE Maximum number of elements in the queue buffer
 * @tparam W Wrapper type for versioning (default: TOpaqueWrapped)
 */
template <typename T, unsigned BUF_SIZE = mako::sto::constants::DEFAULT_QUEUE_BUFFER_SIZE,
          template <typename> class W = TOpaqueWrapped>
class Queue: public TObject {
public:
    typedef typename W<T>::version_type version_type;

    /**
     * @brief Construct an empty queue
     */
    Queue() : head_(0), tail_(0), tailversion_(0), headversion_(0) {}

    static constexpr TransItem::flags_type delete_bit = TransItem::user0_bit;
    static constexpr TransItem::flags_type read_writes = TransItem::user0_bit<<1;
    static constexpr TransItem::flags_type list_bit = TransItem::user0_bit<<2;
    static constexpr TransItem::flags_type empty_bit = TransItem::user0_bit<<3;

    // NONTRANSACTIONAL OPERATIONS
    
    /**
     * @brief Non-transactional push operation
     * @param value Element to push onto the queue
     */
    void nontrans_push(T value) {
        queueSlots[tail_] = value;
        tail_ = (tail_ + 1) % BUF_SIZE;
        assert(head_ != tail_);
    }
    
    /**
     * @brief Non-transactional pop operation
     * @return Element from the front of the queue
     */
    T nontrans_pop() {
        assert(head_ != tail_);
        T value = queueSlots[head_];
        head_ = (head_ + 1) % BUF_SIZE;
        return value;
    }

    /**
     * @brief Check if queue is empty (non-transactional)
     * @return true if queue is empty, false otherwise
     */
    bool nontrans_empty() const {
        return head_ == tail_;
    }

    template <typename RandomGen>
    void nontrans_shuffle(RandomGen gen) {
        auto head = &queueSlots[head_];
        auto tail = &queueSlots[tail_];
        // don't support wrap-around shuffle
        assert(head < tail);
        std::shuffle(head, tail, gen);
    }

    void nontrans_clear() {
        while (!nontrans_empty())
            nontrans_pop();
    }

    // TRANSACTIONAL OPERATIONS
    
    /**
     * @brief Transactional push operation
     * @param value Element to push onto the queue
     */
    void transPush(const T& value) {
        auto item = Sto::item(this, mako::sto::constants::PUSH_ITEM_KEY);
        if (item.has_write()) {
            if (!is_list(item)) {
                // Convert single value to list
                auto& existing_value = item.template write_value<T>();
                std::list<T> write_list;
                if (!is_empty(item)) {
                    write_list.push_back(existing_value);
                    item.clear_flags(empty_bit);
                }
                write_list.push_back(value);
                item.clear_write();
                item.add_write(write_list);
                item.add_flags(list_bit);
            }
            else {
                // Append to existing list
                auto& write_list = item.template write_value<std::list<T>>();
                write_list.push_back(value);
            }
        }
        else {
            // First write for this transaction
            item.add_write(value);
        }
    }

    /**
     * @brief Transactional pop operation
     * @return true if an element was popped, false if queue is empty
     */
    bool transPop() {
        auto head_version = headversion_;
        fence();
        auto current_index = head_;
        auto item = Sto::item(this, current_index);

        while (true) {
            if (current_index == tail_) {
                auto tail_version = tailversion_;
                fence();
                // Check if someone has pushed onto tail
                if (current_index == tail_) {
                    auto push_item = Sto::item(this, mako::sto::constants::PUSH_ITEM_KEY);
                    if (!push_item.has_read()) {
                        push_item.observe(tail_version);
                    }
                    if (push_item.has_write()) {
                        if (is_list(push_item)) {
                            auto& write_list = push_item.template write_value<std::list<T>>();
                            // If there is an element to be pushed, consume it
                            if (!write_list.empty()) {
                                write_list.pop_front();
                                item.add_flags(read_writes);
                                return true;
                            } else {
                                return false;
                            }
                        } else if (!is_empty(push_item)) {
                            // Single pending push - mark as consumed
                            push_item.add_flags(empty_bit);
                            return true;
                        } else {
                            return false;
                        }
                    }
                    // Queue is empty and no pending pushes
                    return false;  
                } 
            }
            
            if (has_delete(item)) {
                // Skip deleted items
                current_index = (current_index + 1) % BUF_SIZE;
                item = Sto::item(this, current_index);
            } else {
                break;
            }
        }
        
        // Ensure head is not modified by commit time
        auto lock_item = Sto::item(this, mako::sto::constants::HEAD_LOCK_KEY);
        if (!lock_item.has_read()) {
            lock_item.observe(head_version);
        }
        lock_item.add_write(0);
        item.add_flags(delete_bit);
        item.add_write(0);
        return true;
    }

    /**
     * @brief Transactional front operation (peek at first element)
     * @param value Reference to store the front element
     * @return true if an element was found, false if queue is empty
     */
    bool transFront(T& value) {
        auto head_version = headversion_;
        fence();
        unsigned current_index = head_;
        auto item = Sto::item(this, current_index);
        
        while (true) {
            // Check if queue is empty
            if (current_index == tail_) {
                auto tail_version = tailversion_;
                fence();
                // Check if someone has pushed onto tail
                if (current_index == tail_) {
                    auto push_item = Sto::item(this, mako::sto::constants::PUSH_ITEM_KEY);
                    if (!push_item.has_read()) {
                        push_item.observe(tail_version);
                    }
                    if (push_item.has_write()) {
                        if (is_list(push_item)) {
                            auto& write_list = push_item.template write_value<std::list<T>>();
                            // If there is a pending push, return its front element
                            if (!write_list.empty()) {
                                value = write_list.front();
                                return true;
                            } else {
                                return false;
                            }
                        } else if (!is_empty(push_item)) {
                            // Single pending push
                            auto& pending_value = push_item.template write_value<T>();
                            value = pending_value;
                            return true;
                        } else {
                            return false;
                        }
                    }
                    return false;
                }
            }
            
            if (has_delete(item)) {
                // Skip deleted items
                current_index = (current_index + 1) % BUF_SIZE;
                item = Sto::item(this, current_index);
            } else {
                break;
            }
        }
        
        // Ensure head was not modified at commit time
        auto lock_item = Sto::item(this, mako::sto::constants::HEAD_LOCK_KEY);
        if (!lock_item.has_read()) {
            lock_item.observe(head_version);
        }  
        value = queueSlots[current_index];
        return true;
    }
    
private:
    /**
     * @brief Check if transaction item has delete flag
     */
    bool has_delete(const TransItem& item) {
        return item.flags() & delete_bit;
    }
    
    /**
     * @brief Check if transaction item has read-writes flag
     */
    bool is_rw(const TransItem& item) {
        return item.flags() & read_writes;
    }
 
    /**
     * @brief Check if transaction item represents a list of values
     */
    bool is_list(const TransItem& item) {
        return item.flags() & list_bit;
    }
 
    /**
     * @brief Check if transaction item is marked as empty
     */
    bool is_empty(const TransItem& item) {
        return item.flags() & empty_bit;
    }

    bool lock(TransItem& item, Transaction& txn) override {
        if (item.key<int>() == mako::sto::constants::PUSH_ITEM_KEY)
            return txn.try_lock(item, tailversion_);
        else if (item.key<int>() == mako::sto::constants::HEAD_LOCK_KEY)
            return txn.try_lock(item, headversion_);
        else
            return true;
    }

    bool check(TransItem& item, Transaction& t) override {
        (void) t;
        // Check if was a pop or front operation
        if (item.key<int>() == mako::sto::constants::HEAD_LOCK_KEY)
            return item.check_version(headversion_);
        // Check if we read from the write_list (and locked tailversion)
        else if (item.key<int>() == mako::sto::constants::PUSH_ITEM_KEY)
            return item.check_version(tailversion_);
        // Shouldn't reach this point
        assert(false);
        return false;
    }

    void install(TransItem& item, Transaction& txn) override {
        // Ignore head lock marker item
        if (item.key<int>() == mako::sto::constants::HEAD_LOCK_KEY)
            return;
            
        // Install pop operations
        if (has_delete(item)) {
            // Only increment head if item was popped from actual queue
            if (!is_rw(item))
                head_ = (head_ + 1) % BUF_SIZE;
            headversion_.set_version(txn.commit_tid());
        }
        // Install push operations
        else if (item.key<int>() == mako::sto::constants::PUSH_ITEM_KEY) {
            auto head_index = head_;
            // Write all pending elements
            if (is_list(item)) {
                auto& write_list = item.template write_value<std::list<T>>();
                while (!write_list.empty()) {
                    // Assert queue is not out of space            
                    assert(tail_ != (head_index - 1) % BUF_SIZE);
                    queueSlots[tail_] = write_list.front();
                    write_list.pop_front();
                    tail_ = (tail_ + 1) % BUF_SIZE;
                }
            }
            else if (!is_empty(item)) {
                auto& value = item.template write_value<T>();
                queueSlots[tail_] = value;
                tail_ = (tail_ + 1) % BUF_SIZE;
            }

            tailversion_.set_version(txn.commit_tid());
        }
    }
    
    void unlock(TransItem& item) override {
        if (item.key<int>() == mako::sto::constants::PUSH_ITEM_KEY)
            tailversion_.unlock();
        else if (item.key<int>() == mako::sto::constants::HEAD_LOCK_KEY)
            headversion_.unlock();
    }

    T queueSlots[BUF_SIZE];

    unsigned head_;
    unsigned tail_;
    version_type tailversion_;
    version_type headversion_;
};

#endif /* MAKO_STO_QUEUE_HH */
