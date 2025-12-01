#ifndef _NDB_TXN_PROTO2_IMPL_H_
#define _NDB_TXN_PROTO2_IMPL_H_

#include <iostream>
#include <atomic>
#include <vector>
#include <set>
#include <lz4.h>
#include "txn.h"
#include "txn_impl.h"
#include "txn_btree.h"
#include "macros.h"
#include "circbuf.h"
#include "spinbarrier.h"
#include "record/serializer.h"

template <typename Traits> class transaction_proto2;
template <template <typename> class Transaction> class txn_epoch_sync;

class txn_logger {
  friend class transaction_proto2_static;
  template <typename T> friend class transaction_proto2;
  template <template <typename> class T> friend class txn_epoch_sync;

public:
  static const size_t g_nmax_loggers = 16;
  static const size_t g_perthread_buffers = 256;
  static const size_t g_buffer_size = (1<<20);
  static const size_t g_horizon_buffer_size = 2 * (1<<16);
  static const size_t g_max_lag_epochs = 128;
  static const bool   g_pin_loggers_to_numa_nodes = false;

  static inline bool IsPersistenceEnabled() { return g_persist; }
  static inline bool IsCompressionEnabled() { return g_use_compression; }

  // @unsafe
  static void Init(
      size_t nworkers,
      const std::vector<std::string> &logfiles,
      const std::vector<std::vector<unsigned>> &assignments_given,
      std::vector<std::vector<unsigned>> *assignments_used = nullptr,
      bool call_fsync = true,
      bool use_compression = false,
      bool fake_writes = false);

  struct logbuf_header {
    uint64_t nentries_;
    uint64_t last_tid_;
  } PACKED;

  struct pbuffer {
    uint64_t earliest_start_us_;
    bool io_scheduled_;
    unsigned curoff_;
    const unsigned core_id_;
    const unsigned buf_sz_;
    uint8_t buf_start_[0];

    pbuffer(unsigned core_id, unsigned buf_sz)
      : core_id_(core_id), buf_sz_(buf_sz)
    {
      INVARIANT(((char *)this) + sizeof(*this) == (char *) &buf_start_[0]);
      INVARIANT(buf_sz > sizeof(logbuf_header));
      reset();
    }

    pbuffer(const pbuffer &) = delete;
    pbuffer &operator=(const pbuffer &) = delete;
    pbuffer(pbuffer &&) = delete;

    inline void reset() {
      earliest_start_us_ = 0;
      io_scheduled_ = false;
      curoff_ = sizeof(logbuf_header);
      NDB_MEMSET(&buf_start_[0], 0, buf_sz_);
    }

    inline uint8_t * pointer() {
      INVARIANT(curoff_ >= sizeof(logbuf_header));
      INVARIANT(curoff_ <= buf_sz_);
      return &buf_start_[0] + curoff_;
    }

    inline uint8_t * datastart() { return &buf_start_[0] + sizeof(logbuf_header); }

    inline size_t datasize() const {
      INVARIANT(curoff_ >= sizeof(logbuf_header));
      INVARIANT(curoff_ <= buf_sz_);
      return curoff_ - sizeof(logbuf_header);
    }

    // @unsafe
    inline logbuf_header * header() { return reinterpret_cast<logbuf_header *>(&buf_start_[0]); }
    // @unsafe
    inline const logbuf_header * header() const { return reinterpret_cast<const logbuf_header *>(&buf_start_[0]); }

    inline size_t space_remaining() const {
      INVARIANT(curoff_ >= sizeof(logbuf_header));
      INVARIANT(curoff_ <= buf_sz_);
      return buf_sz_ - curoff_;
    }

    inline bool can_hold_tid(uint64_t tid) const;
  } PACKED;

  static bool AssignmentsValid(const std::vector<std::vector<unsigned>> &assignments, unsigned nfds, unsigned nworkers) {
    if (assignments.size() > nfds) return false;
    std::set<unsigned> seen;
    for (auto &assignment : assignments)
      for (auto w : assignment) {
        if (seen.count(w) || w >= nworkers) return false;
        seen.insert(w);
      }
    return seen.size() == nworkers;
  }

  typedef circbuf<pbuffer, g_perthread_buffers> pbuffer_circbuf;

  static std::tuple<uint64_t, uint64_t, double> compute_ntxns_persisted_statistics();
  static void clear_ntxns_persisted_statistics();
  static void wait_for_idle_state();
  static void wait_until_current_point_persisted();

private:
  struct epoch_array {
    std::atomic<uint64_t> epochs_[NMAXCORES];
    std::atomic<uint64_t> dummy_work_;
    CACHE_PADOUT;
  };

  struct persist_ctx {
    bool init_;
    void *lz4ctx_;
    pbuffer *horizon_;
    circbuf<pbuffer, g_perthread_buffers> all_buffers_;
    circbuf<pbuffer, g_perthread_buffers> persist_buffers_;
    persist_ctx() : init_(false), lz4ctx_(nullptr), horizon_(nullptr) {}
  };

  struct persist_stats {
    std::atomic<uint64_t> ntxns_persisted_;
    std::atomic<uint64_t> ntxns_pushed_;
    std::atomic<uint64_t> ntxns_committed_;
    std::atomic<uint64_t> latency_numer_;
    struct per_epoch_stats {
      std::atomic<uint64_t> ntxns_;
      std::atomic<uint64_t> earliest_start_us_;
      per_epoch_stats() : ntxns_(0), earliest_start_us_(0) {}
    } d_[g_max_lag_epochs];
    persist_stats() : ntxns_persisted_(0), ntxns_pushed_(0), ntxns_committed_(0), latency_numer_(0) {}
  };

  static void advance_system_sync_epoch(const std::vector<std::vector<unsigned>> &assignments);
  static void writer(unsigned id, int fd, std::vector<unsigned> assignment);
  static void persister(std::vector<std::vector<unsigned>> assignments);

  enum InitMode { INITMODE_NONE, INITMODE_REG, INITMODE_RCU, };

  // @unsafe
  static inline persist_ctx & persist_ctx_for(uint64_t core_id, InitMode imode) {
    INVARIANT(core_id < g_persist_ctxs.size());
    persist_ctx &ctx = g_persist_ctxs[core_id];
    if (unlikely(!ctx.init_ && imode != INITMODE_NONE)) {
      size_t needed = g_perthread_buffers * (sizeof(pbuffer) + g_buffer_size);
      if (IsCompressionEnabled())
        needed += size_t(LZ4_create_size()) + sizeof(pbuffer) + g_horizon_buffer_size;
      char *mem = (imode == INITMODE_REG) ? (char *) malloc(needed) : (char *) rcu::s_instance.alloc_static(needed);
      if (IsCompressionEnabled()) {
        ctx.lz4ctx_ = mem;
        mem += LZ4_create_size();
        ctx.horizon_ = new (mem) pbuffer(core_id, g_horizon_buffer_size);
        mem += sizeof(pbuffer) + g_horizon_buffer_size;
      }
      for (size_t i = 0; i < g_perthread_buffers; i++) {
        ctx.all_buffers_.enq(new (mem) pbuffer(core_id, g_buffer_size));
        mem += sizeof(pbuffer) + g_buffer_size;
      }
      ctx.init_ = true;
    }
    return g_persist_ctxs[core_id];
  }

  static bool g_persist;
  static bool g_call_fsync;
  static bool g_use_compression;
  static bool g_fake_writes;
  static size_t g_nworkers;
  static epoch_array per_thread_sync_epochs_[g_nmax_loggers] CACHE_ALIGNED;
  static util::aligned_padded_elem<std::atomic<uint64_t>> system_sync_epoch_ CACHE_ALIGNED;
  static percore<persist_ctx> g_persist_ctxs CACHE_ALIGNED;
  static percore<persist_stats> g_persist_stats CACHE_ALIGNED;

  static event_counter g_evt_log_buffer_epoch_boundary;
  static event_counter g_evt_log_buffer_out_of_space;
  static event_counter g_evt_log_buffer_bytes_before_compress;
  static event_counter g_evt_log_buffer_bytes_after_compress;
  static event_counter g_evt_logger_writev_limit_met;
  static event_counter g_evt_logger_max_lag_wait;
  static event_avg_counter g_evt_avg_log_entry_ntxns;
  static event_avg_counter g_evt_avg_log_buffer_compress_time_us;
  static event_avg_counter g_evt_avg_logger_bytes_per_writev;
  static event_avg_counter g_evt_avg_logger_bytes_per_sec;
};

static inline std::ostream & operator<<(std::ostream &o, txn_logger::logbuf_header &hdr) {
  o << "{nentries_=" << hdr.nentries_ << ", last_tid_=" << g_proto_version_str(hdr.last_tid_) << "}";
  return o;
}

class transaction_proto2_static {
public:
#ifdef CHECK_INVARIANTS
  static const uint64_t ReadOnlyEpochMultiplier = 10;
#else
  static const uint64_t ReadOnlyEpochMultiplier = 25;
  static_assert(ticker::tick_us * ReadOnlyEpochMultiplier == 1000000, "");
#endif
  static_assert(ReadOnlyEpochMultiplier >= 1, "XX");
  static const uint64_t ReadOnlyEpochUsec = ticker::tick_us * ReadOnlyEpochMultiplier;
  static inline uint64_t constexpr to_read_only_tick(uint64_t epoch_tick) { return epoch_tick / ReadOnlyEpochMultiplier; }

  static inline ALWAYS_INLINE uint64_t CoreId(uint64_t v) { return v & CoreMask; }
  static inline ALWAYS_INLINE uint64_t NumId(uint64_t v) { return (v & NumIdMask) >> NumIdShift; }
  static inline ALWAYS_INLINE uint64_t EpochId(uint64_t v) { return (v & EpochMask) >> EpochShift; }

  static void wait_an_epoch() {
    INVARIANT(!rcu::s_instance.in_rcu_region());
    const uint64_t e = to_read_only_tick(ticker::s_instance.global_last_tick_exclusive());
    while (to_read_only_tick(ticker::s_instance.global_last_tick_exclusive()) == e) nop_pause();
    COMPILER_MEMORY_FENCE;
  }

  static uint64_t ComputeReadOnlyTid(uint64_t global_tick_ex) {
    const uint64_t a = (global_tick_ex / ReadOnlyEpochMultiplier);
    const uint64_t b = a * ReadOnlyEpochMultiplier;
    if (!b) return MakeTid(0, 0, 0);
    else return MakeTid(CoreMask, NumIdMask >> NumIdShift, b - 1);
  }

  static const uint64_t NBitsNumber = 24;
  static const size_t CoreBits = NMAXCOREBITS;
  static const size_t NMaxCores = NMAXCORES;
  static const uint64_t CoreMask = (NMaxCores - 1);
  static const uint64_t NumIdShift = CoreBits;
  static const uint64_t NumIdMask = ((((uint64_t)1) << NBitsNumber) - 1) << NumIdShift;
  static const uint64_t EpochShift = CoreBits + NBitsNumber;
  static const uint64_t EpochMask = ((uint64_t)-1) << EpochShift;

  static inline ALWAYS_INLINE uint64_t MakeTid(uint64_t core_id, uint64_t num_id, uint64_t epoch_id) {
    static_assert((CoreMask | NumIdMask | EpochMask) == ((uint64_t)-1), "xx");
    static_assert((CoreMask & NumIdMask) == 0, "xx");
    static_assert((NumIdMask & EpochMask) == 0, "xx");
    return (core_id) | (num_id << NumIdShift) | (epoch_id << EpochShift);
  }

  static inline void set_hack_status(bool hack_status) { g_hack->status_ = hack_status; }
  static inline bool get_hack_status() { return g_hack->status_; }

  static void InitGC();
  static void PurgeThreadOutstandingGCTasks();

#ifdef PROTO2_CAN_DISABLE_GC
  static inline bool IsGCEnabled() { return g_flags->g_gc_init.load(std::memory_order_acquire); }
#endif
#ifdef PROTO2_CAN_DISABLE_SNAPSHOTS
  static void DisableSnapshots() { g_flags->g_disable_snapshots.store(true, std::memory_order_release); }
  static inline bool IsSnapshotsEnabled() { return !g_flags->g_disable_snapshots.load(std::memory_order_acquire); }
#endif

protected:
  struct delete_entry {
#ifdef CHECK_INVARIANTS
    dbtuple *tuple_ahead_;
    uint64_t trigger_tid_;
#endif
    dbtuple *tuple_;
    marked_ptr<std::string> key_;
    concurrent_btree *btr_;
    delete_entry() :
#ifdef CHECK_INVARIANTS
      tuple_ahead_(nullptr), trigger_tid_(0),
#endif
      tuple_(), key_(), btr_(nullptr) {}
    delete_entry(dbtuple *tuple_ahead, uint64_t trigger_tid, dbtuple *tuple, const marked_ptr<std::string> &key, concurrent_btree *btr) :
#ifdef CHECK_INVARIANTS
      tuple_ahead_(tuple_ahead), trigger_tid_(trigger_tid),
#endif
      tuple_(tuple), key_(key), btr_(btr) {}
    inline dbtuple * tuple() { return tuple_; }
  };

  typedef basic_px_queue<delete_entry, 4096> px_queue;

  struct threadctx {
    uint64_t last_commit_tid_;
    unsigned last_reaped_epoch_;
#ifdef ENABLE_EVENT_COUNTERS
    uint64_t last_reaped_timestamp_us_;
#endif
    px_queue queue_;
    px_queue scratch_;
    std::deque<std::string *> pool_;
    threadctx() : last_commit_tid_(0), last_reaped_epoch_(0)
#ifdef ENABLE_EVENT_COUNTERS
      , last_reaped_timestamp_us_(0)
#endif
    {
      ALWAYS_ASSERT(((uintptr_t)this % CACHELINE_SIZE) == 0);
      queue_.alloc_freelist(rcu::NQueueGroups);
      scratch_.alloc_freelist(rcu::NQueueGroups);
    }
  };

  // @unsafe
  static void clean_up_to_including(threadctx &ctx, uint64_t ro_tick_geq);

  static inline txn_logger::pbuffer * wait_for_head(txn_logger::pbuffer_circbuf &pull_buf) {
    txn_logger::pbuffer *px;
    while (unlikely(!(px = pull_buf.peek()))) {
      nop_pause();
      ++g_evt_worker_thread_wait_log_buffer;
    }
    INVARIANT(!px->io_scheduled_);
    return px;
  }

  static inline size_t push_horizon_to_buffer(txn_logger::pbuffer *horizon, void *lz4ctx, txn_logger::pbuffer_circbuf &pull_buf, txn_logger::pbuffer_circbuf &push_buf) {
    INVARIANT(txn_logger::IsCompressionEnabled());
    if (unlikely(!horizon->header()->nentries_)) return 0;
    INVARIANT(horizon->datasize());
    size_t ntxns_pushed_to_logger = 0;
    txn_logger::pbuffer *px = wait_for_head(pull_buf);
    const uint64_t compressed_space_needed = sizeof(uint32_t) + LZ4_compressBound(horizon->datasize());
    bool buffer_cond = false;
    if (px->space_remaining() < compressed_space_needed || (buffer_cond = !px->can_hold_tid(horizon->header()->last_tid_))) {
      INVARIANT(px->header()->nentries_);
      ntxns_pushed_to_logger = px->header()->nentries_;
      txn_logger::pbuffer *px1 = pull_buf.deq();
      INVARIANT(px == px1);
      push_buf.enq(px1);
      px = wait_for_head(pull_buf);
      if (buffer_cond) ++txn_logger::g_evt_log_buffer_epoch_boundary;
      else ++txn_logger::g_evt_log_buffer_out_of_space;
    }
    INVARIANT(px->space_remaining() >= compressed_space_needed);
    if (!px->header()->nentries_) px->earliest_start_us_ = horizon->earliest_start_us_;
    px->header()->nentries_ += horizon->header()->nentries_;
    px->header()->last_tid_  = horizon->header()->last_tid_;
#ifdef ENABLE_EVENT_COUNTERS
    util::timer tt;
#endif
    const int ret = LZ4_compress_heap_limitedOutput(lz4ctx, (const char *) horizon->datastart(), (char *) px->pointer() + sizeof(uint32_t), horizon->datasize(), px->space_remaining() - sizeof(uint32_t));
#ifdef ENABLE_EVENT_COUNTERS
    txn_logger::g_evt_avg_log_buffer_compress_time_us.offer(tt.lap());
    txn_logger::g_evt_log_buffer_bytes_before_compress.inc(horizon->datasize());
    txn_logger::g_evt_log_buffer_bytes_after_compress.inc(ret);
#endif
    INVARIANT(ret > 0);
    serializer<uint32_t, false> s_uint32_t;
    s_uint32_t.write(px->pointer(), ret);
    px->curoff_ += sizeof(uint32_t) + uint32_t(ret);
    horizon->reset();
    return ntxns_pushed_to_logger;
  }

  struct hackstruct {
    std::atomic<bool> status_;
    std::atomic<uint64_t> global_tid_;
    constexpr hackstruct() : status_(false), global_tid_(0) {}
  };
  static util::aligned_padded_elem<hackstruct> g_hack CACHE_ALIGNED;

  struct flags {
    std::atomic<bool> g_gc_init;
    std::atomic<bool> g_disable_snapshots;
    constexpr flags() : g_gc_init(false), g_disable_snapshots(false) {}
  };
  static util::aligned_padded_elem<flags> g_flags;
  static percore_lazy<threadctx> g_threadctxs;
  static event_counter g_evt_worker_thread_wait_log_buffer;
  static event_counter g_evt_dbtuple_no_space_for_delkey;
  static event_counter g_evt_proto_gc_delete_requeue;
  static event_avg_counter g_evt_avg_log_entry_size;
  static event_avg_counter g_evt_avg_proto_gc_queue_len;
};

bool txn_logger::pbuffer::can_hold_tid(uint64_t tid) const {
  return !header()->nentries_ || (transaction_proto2_static::EpochId(header()->last_tid_) == transaction_proto2_static::EpochId(tid));
}

#endif /* _NDB_TXN_PROTO2_IMPL_H_ */