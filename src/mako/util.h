#ifndef MAKO_UTIL_H
#define MAKO_UTIL_H

#include <iostream>
#include <sstream>
#include <string>
#include <limits>
#include <queue>
#include <utility>
#include <memory>
#include <atomic>
#include <tuple>
#include <algorithm>
#include <chrono>

#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <cxxabi.h>

#include "macros.h"
#include "silo_small_vector.h"

typedef std::chrono::high_resolution_clock Clock;

namespace util {

// Configuration constants
namespace constants {
    constexpr unsigned long RANDOM_SEED_MULTIPLIER = 0x5DEECE66DL;
    constexpr unsigned long RANDOM_SEED_ADDEND = 0xBL;
    constexpr unsigned long RANDOM_SEED_MASK = (1L << 48) - 1;
    constexpr unsigned long RANDOM_SEED_XOR = 0x5DEECE66DL;
    constexpr size_t NANOSECONDS_PER_MILLISECOND = 1000000;
    constexpr double MICROSECONDS_PER_MILLISECOND = 1000.0;
    constexpr size_t RANDOM_DOUBLE_PRECISION_BITS = 53;
    constexpr size_t READABLE_CHAR_COUNT = 6;
}

static bool enable_thread_yield = true;

/**
 * @brief Cache-line aligned and padded element wrapper
 * 
 * Ensures that the wrapped element is aligned to cache line boundaries
 * and padded to prevent false sharing in multi-threaded environments.
 * 
 * @tparam T Element type to wrap
 * @tparam Pedantic If true, performs runtime alignment assertions
 */
template <typename T, bool Pedantic = true>
class aligned_padded_elem {
public:

  template <class... Args>
  aligned_padded_elem(Args &&... args)
    : elem(std::forward<Args>(args)...)
  {
    if (Pedantic)
      ALWAYS_ASSERT(((uintptr_t)this % CACHELINE_SIZE) == 0);
  }

  T elem;
  CACHE_PADOUT;

  // syntactic sugar- can treat like a pointer
  inline T & operator*() { return elem; }
  inline const T & operator*() const { return elem; }
  inline T * operator->() { return &elem; }
  inline const T * operator->() const { return &elem; }

private:
  inline void
  __cl_asserter() const
  {
    static_assert((sizeof(*this) % CACHELINE_SIZE) == 0, "xx");
  }
} CACHE_ALIGNED;

// some pre-defs
typedef aligned_padded_elem<uint8_t>  aligned_padded_u8;
typedef aligned_padded_elem<uint16_t> aligned_padded_u16;
typedef aligned_padded_elem<uint32_t> aligned_padded_u32;
typedef aligned_padded_elem<uint64_t> aligned_padded_u64;

template <typename T>
struct host_endian_trfm {
  inline ALWAYS_INLINE T operator()(const T &t) const { return t; }
};

template <>
struct host_endian_trfm<uint16_t> {
  inline ALWAYS_INLINE uint16_t operator()(uint16_t t) const { return be16toh(t); }
};

template <>
struct host_endian_trfm<int16_t> {
  inline ALWAYS_INLINE int16_t operator()(int16_t t) const { return be16toh(t); }
};

template <>
struct host_endian_trfm<int32_t> {
  inline ALWAYS_INLINE int32_t operator()(int32_t t) const { return be32toh(t); }
};

template <>
struct host_endian_trfm<uint32_t> {
  inline ALWAYS_INLINE uint32_t operator()(uint32_t t) const { return be32toh(t); }
};

template <>
struct host_endian_trfm<int64_t> {
  inline ALWAYS_INLINE int64_t operator()(int64_t t) const { return be64toh(t); }
};

template <>
struct host_endian_trfm<uint64_t> {
  inline ALWAYS_INLINE uint64_t operator()(uint64_t t) const { return be64toh(t); }
};

template <typename T>
struct big_endian_trfm {
  inline ALWAYS_INLINE T operator()(const T &t) const { return t; }
};

template <>
struct big_endian_trfm<uint16_t> {
  inline ALWAYS_INLINE uint16_t operator()(uint16_t t) const { return htobe16(t); }
};

template <>
struct big_endian_trfm<int16_t> {
  inline ALWAYS_INLINE int16_t operator()(int16_t t) const { return htobe16(t); }
};

template <>
struct big_endian_trfm<int32_t> {
  inline ALWAYS_INLINE int32_t operator()(int32_t t) const { return htobe32(t); }
};

template <>
struct big_endian_trfm<uint32_t> {
  inline ALWAYS_INLINE uint32_t operator()(uint32_t t) const { return htobe32(t); }
};

template <>
struct big_endian_trfm<int64_t> {
  inline ALWAYS_INLINE int64_t operator()(int64_t t) const { return htobe64(t); }
};

template <>
struct big_endian_trfm<uint64_t> {
  inline ALWAYS_INLINE uint64_t operator()(uint64_t t) const { return htobe64(t); }
};

inline std::string
hexify_buf(const char *buf, size_t len)
{
  const char *const lut = "0123456789ABCDEF";
  std::string output;
  output.reserve(2 * len);
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = (unsigned char) buf[i];
    output.push_back(lut[c >> 4]);
    output.push_back(lut[c & 15]);
  }
  return output;
}


template <typename T>
inline std::string
hexify(const T &t)
{
  std::ostringstream buf;
  buf << std::hex << t;
  return buf.str();
}

template <>
inline std::string
hexify(const std::string &input)
{
  return hexify_buf(input.data(), input.size());
}

template <typename T, unsigned int lgbase>
struct mask_ {
  static const T value = ((T(1) << lgbase) - 1);
};

// rounding
template <typename T, unsigned int lgbase>
static constexpr inline ALWAYS_INLINE T
round_up(T t)
{
  return (t + mask_<T, lgbase>::value) & ~mask_<T, lgbase>::value;
}

template <typename T, unsigned int lgbase>
static constexpr inline ALWAYS_INLINE T
round_down(T t)
{
  return (t & ~mask_<T, lgbase>::value);
}

template <typename T, typename U>
static inline ALWAYS_INLINE T
iceil(T x, U y)
{
  U mod = x % y;
  return x + (mod ? y - mod : 0);
}

template <typename T>
static inline T
slow_round_up(T x, T q)
{
  const T r = x % q;
  if (!r)
    return x;
  return x + (q - r);
}

template <typename T>
static inline T
slow_round_down(T x, T q)
{
  const T r = x % q;
  if (!r)
    return x;
  return x - r;
}

/**
 * @brief Fast pseudo-random number generator
 * 
 * Linear congruential generator based on Java's Random implementation.
 * NOT thread-safe - each thread should have its own instance.
 * 
 * Reference: http://developer.classpath.org/doc/java/util/Random-source.html
 */
class fast_random {
public:
  fast_random(unsigned long seed)
    : seed(0)
  {
    set_seed0(seed);
  }

  inline unsigned long
  next()
  {
    return ((unsigned long) next(32) << 32) + next(32);
  }

  inline uint32_t
  next_u32()
  {
    return next(32);
  }

  inline uint16_t
  next_u16()
  {
    return next(16);
  }

  /** [0.0, 1.0) */
  inline double
  next_uniform()
  {
    return (((unsigned long) next(26) << 27) + next(27)) / (double) (1L << constants::RANDOM_DOUBLE_PRECISION_BITS);
  }

  inline char
  next_char()
  {
    return next(8) % 256;
  }

  inline char
  next_readable_char()
  {
    static const char readables[] = "0123456789@ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
    return readables[next(constants::READABLE_CHAR_COUNT)];
  }

  inline std::string
  next_string(size_t len)
  {
    std::string s(len, 0);
    for (size_t i = 0; i < len; i++)
      s[i] = next_char();
    return s;
  }

  inline std::string
  next_readable_string(size_t len)
  {
    std::string s(len, 0);
    for (size_t i = 0; i < len; i++)
      s[i] = next_readable_char();
    return s;
  }

  inline unsigned long
  get_seed()
  {
    return seed;
  }

  inline void
  set_seed(unsigned long seed)
  {
    this->seed = seed;
  }

private:
  inline void
  set_seed0(unsigned long seed)
  {
    this->seed = (seed ^ constants::RANDOM_SEED_XOR) & constants::RANDOM_SEED_MASK;
  }

  inline unsigned long
  next(unsigned int bits)
  {
    seed = (seed * constants::RANDOM_SEED_MULTIPLIER + constants::RANDOM_SEED_ADDEND) & constants::RANDOM_SEED_MASK;
    return (unsigned long) (seed >> (48 - bits));
  }

  unsigned long seed;
};

// Add operator<< for std::pair to support format_list
template <typename T1, typename T2>
inline std::ostream& operator<<(std::ostream& os, const std::pair<T1, T2>& p) {
  return os << "(" << p.first << ", " << p.second << ")";
}

template <typename ForwardIterator>
std::string
format_list(ForwardIterator begin, ForwardIterator end)
{
  std::ostringstream ss;
  ss << "[";
  bool first = true;
  while (begin != end) {
    if (!first)
      ss << ", ";
    first = false;
    ss << *begin++;
  }
  ss << "]";
  return ss.str();
}

static void
pclock(char *msg, clockid_t cid)
{
  struct timespec ts;
  if (clock_gettime(cid, &ts) == -1)
    printf("%s [ERROR] clock_gettime", msg);
  else
    printf("%s %4jd.%03ld\n", msg, (intmax_t) ts.tv_sec, ts.tv_nsec / constants::NANOSECONDS_PER_MILLISECOND);
}

/**
 * Returns the lowest position p such that p0+p != p1+p.
 */
inline size_t
first_pos_diff(const char *p0, size_t sz0,
               const char *p1, size_t sz1)
{
  const char *p0end = p0 + sz0;
  const char *p1end = p1 + sz1;
  size_t n = 0;
  while (p0 != p0end &&
         p1 != p1end &&
         p0[n] == p1[n])
    n++;
  return n;
}

/**
 * @brief High-resolution timer for performance measurement
 * 
 * Provides microsecond and nanosecond precision timing capabilities.
 * Non-copyable and non-movable to prevent timing state corruption.
 */
class timer {
private:
  timer(const timer &) = delete;
  timer &operator=(const timer &) = delete;
  timer(timer &&) = delete;

public:
  timer()
  {
    lap();
  }

  timer(bool nano)
  {
    lap_nano();
  }

  inline uint64_t
  lap()
  {
    uint64_t t0 = start;
    uint64_t t1 = cur_usec();
    start = t1;
    return t1 - t0;
  }

  inline uint64_t lap_nano()
  {
    auto t0 = start_clock;
    auto t1 = Clock::now();
    start_clock = t1;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  }

  inline double
  lap_ms()
  {
    return lap() / constants::MICROSECONDS_PER_MILLISECOND;
  }

  static inline uint64_t
  cur_usec()
  {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return ((uint64_t)tv.tv_sec) * 1000000 + tv.tv_usec;
  }

private:

  uint64_t start;
  Clock::time_point start_clock;
};

class scoped_timer {
private:
  timer t;
  std::string region;
  bool enabled;

public:
  scoped_timer(const std::string &region, bool enabled = true)
    : region(region), enabled(enabled)
  {}

  ~scoped_timer()
  {
    if (enabled) {
      const double x = t.lap() / constants::MICROSECONDS_PER_MILLISECOND; // ms
      std::cerr << "timed region " << region << " took " << x << " ms" << std::endl;
    }
  }
};

inline std::string
next_key(const std::string &s)
{
  std::string s0(s);
  s0.resize(s.size() + 1);
  return s0;
}

template <typename T, typename Container = std::vector<T> >
struct std_reverse_pq {
  typedef std::priority_queue<T, Container, std::greater<T> > type;
};

template <typename PairType, typename FirstComp>
struct std_pair_first_cmp {
  inline bool
  operator()(const PairType &lhs, const PairType &rhs) const
  {
    FirstComp c;
    return c(lhs.first, rhs.first);
  }
};

/**
 * @brief Vector type selector with small container optimization
 * 
 * Conditionally selects between silo_small_vector (with inline storage)
 * and std::vector based on compile-time configuration.
 * 
 * @tparam T Element type
 * @tparam SmallSize Number of elements to store inline
 */
template <typename T, size_t SmallSize = SMALL_SIZE_VEC>
struct vec {
#ifdef USE_SMALL_CONTAINER_OPT
  typedef mako::silo::silo_small_vector<T, SmallSize> type;
#else
  typedef std::vector<T> type;
#endif
};

static inline std::vector<std::string>
split(const std::string &s, char delim)
{
  std::vector<std::string> elems;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim))
    elems.emplace_back(item);
  return elems;
}

struct default_string_allocator {
  inline std::string *
  operator()()
  {
    strs.emplace_back(new std::string);
    return strs.back().get();
  }
  inline void
  return_last(std::string *px)
  {
    // XXX: check px in strs
  }
private:
  std::vector<std::shared_ptr<std::string>> strs;
};

static constexpr uint64_t
compute_fields_mask()
{
  return 0;
}

template <typename First, typename... Rest>
static constexpr uint64_t
compute_fields_mask(First f, Rest... rest)
{
  return (1UL << f) | compute_fields_mask(rest...);
}

template <uint64_t Mask>
struct Fields {
  static const uint64_t value = Mask;
};

#define FIELDS(args...) \
  ::util::Fields< ::util::compute_fields_mask(args) >()

#ifdef DISABLE_FIELD_SELECTION
#define GUARDED_FIELDS(args...) \
  ::util::Fields< ::std::numeric_limits<uint64_t>::max() >()
#else
#define GUARDED_FIELDS(args...) FIELDS(args)
#endif

template <typename T>
struct cxx_typename {
  static std::string
  value()
  {
    int st;
    char *name = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &st);
    if (unlikely(st))
      return std::string(typeid(T).name()) + "<demangle failed>";
    std::string ret(name);
    free(name);
    return ret;
  }
};

// returns a vector of [start, ..., end)
template <typename T>
static std::vector<T>
MakeRange(T start, T end)
{
  std::vector<T> ret;
  for (T i = start; i < end; i++)
    ret.push_back(i);
  return ret;
}

/**
 * @brief Utilities for timespec arithmetic
 */
struct timespec_utils {
	/**
	 * @brief Subtract two timespec values (x - y = out)
	 * @param x Minuend timespec
	 * @param y Subtrahend timespec  
	 * @param out Result of subtraction
	 */
	static void
	subtract(const struct timespec *x,
					 const struct timespec *y,
					 struct timespec *out)
	{
		constexpr long NANOSECONDS_PER_SECOND = 1000000000L;
		
		// Perform the carry for the later subtraction by updating y.
		struct timespec y_adjusted = *y;
		if (x->tv_nsec < y_adjusted.tv_nsec) {
			int carry_seconds = (y_adjusted.tv_nsec - x->tv_nsec) / NANOSECONDS_PER_SECOND + 1;
			y_adjusted.tv_nsec -= NANOSECONDS_PER_SECOND * carry_seconds;
			y_adjusted.tv_sec += carry_seconds;
		}
		if (x->tv_nsec - y_adjusted.tv_nsec > NANOSECONDS_PER_SECOND) {
			int borrow_seconds = (x->tv_nsec - y_adjusted.tv_nsec) / NANOSECONDS_PER_SECOND;
			y_adjusted.tv_nsec += NANOSECONDS_PER_SECOND * borrow_seconds;
			y_adjusted.tv_sec -= borrow_seconds;
		}

		// Compute the time remaining to wait. tv_nsec is certainly positive.
		out->tv_sec  = x->tv_sec - y_adjusted.tv_sec;
		out->tv_nsec = x->tv_nsec - y_adjusted.tv_nsec;
	}
};

template <typename T>
struct RangeAwareParser {
  inline std::vector<T>
  operator()(const std::string &s) const
  {
    std::vector<T> ret;
    if (s.find('-') == std::string::npos) {
      T t;
      std::istringstream iss(s);
      iss >> t;
      ret.emplace_back(t);
    } else {
      std::vector<std::string> toks(split(s, '-'));
      ALWAYS_ASSERT(toks.size() == 2);
      T t0, t1;
      std::istringstream iss0(toks[0]), iss1(toks[1]);
      iss0 >> t0;
      iss1 >> t1;
      for (T t = t0; t <= t1; t++)
        ret.emplace_back(t);
    }
    return ret;
  }
};

template <typename T, typename Parser>
static std::vector<T>
ParseCSVString(const std::string &s, Parser p = Parser())
{
  std::vector<T> ret;
  std::vector<std::string> toks(split(s, ','));
  for (auto &s : toks) {
    auto values = p(s);
    ret.insert(ret.end(), values.begin(), values.end());
  }
  return ret;
}

template <typename T>
static inline T
non_atomic_fetch_add(std::atomic<T> &data, T arg)
{
  const T ret = data.load(std::memory_order_acquire);
  data.store(ret + arg, std::memory_order_release);
  return ret;
}

template <typename T>
static inline T
non_atomic_fetch_sub(std::atomic<T> &data, T arg)
{
  const T ret = data.load(std::memory_order_acquire);
  data.store(ret - arg, std::memory_order_release);
  return ret;
}

static inline std::string
to_lower(const std::string &s)
{
  std::string ret(s);
  std::transform(ret.begin(), ret.end(), ret.begin(), ::tolower);
  return ret;
}

} // namespace util

// pretty printer for std::pair<A, B>
template <typename A, typename B>
inline std::ostream &
operator<<(std::ostream &o, const std::pair<A, B> &p)
{
  o << "[" << p.first << ", " << p.second << "]";
  return o;
}

// pretty printer for std::vector<T, Alloc>
template <typename T, typename Alloc>
static std::ostream &
operator<<(std::ostream &o, const std::vector<T, Alloc> &v)
{
  bool first = true;
  o << "[";
  for (auto &p : v) {
    if (!first)
      o << ", ";
    first = false;
    o << p;
  }
  o << "]";
  return o;
}

// pretty printer for std::tuple<...>
namespace private_ {
  template <size_t Idx, bool Enable, class... Types>
  struct helper {
    static inline void
    apply(std::ostream &o, const std::tuple<Types...> &t)
    {
      if (Idx)
        o << ", ";
      o << std::get<Idx, Types...>(t);
      helper<Idx + 1,
             (Idx + 1) < std::tuple_size<std::tuple<Types...>>::value,
             Types...>::apply(o, t);
    }
  };

  template <size_t Idx, class... Types>
  struct helper<Idx, false, Types...> {
    static inline void
    apply(std::ostream &o, const std::tuple<Types...> &t)
    {
    }
  };
}

template <class... Types>
static inline std::ostream &
operator<<(std::ostream &o, const std::tuple<Types...> &t)
{
  o << "[";
  private_::helper<0, 0 < std::tuple_size<std::tuple<Types...>>::value, Types...>::apply(o, t);
  o << "]";
  return o;
}

/**
 * @brief Manual lifetime management wrapper
 * 
 * Provides placement new construction with explicit destructor calls.
 * Used when automatic RAII is not suitable and manual resource management
 * is required. The user is responsible for calling destroy() exactly once.
 * 
 * @tparam T Type to manage
 */
template <typename T>
class unmanaged {
public:
  template <class... Args>
  unmanaged(Args &&... args)
#ifdef CHECK_INVARIANTS
    : destroyed_(false)
#endif
  {
    new (&obj_[0]) T(std::forward<Args>(args)...);
  }

  // up to you to call this at most once
  inline void
  destroy()
  {
#ifdef CHECK_INVARIANTS
    ALWAYS_ASSERT(!destroyed_);
    destroyed_ = true;
#endif
    obj()->~T();
  }

  inline T * obj() { return (T *) &obj_[0]; }
  inline const T * obj() const { return (const T *) &obj_[0]; }

  // syntactic sugar

  inline T & operator*() { return *obj(); }
  inline const T & operator*() const { return *obj(); }
  inline T * operator->() { return obj(); }
  inline const T * operator->() const { return obj(); }

private:
  char obj_[sizeof(T)];
#ifdef CHECK_INVARIANTS
  bool destroyed_;
#endif
} PACKED;

#endif /* MAKO_UTIL_H */
