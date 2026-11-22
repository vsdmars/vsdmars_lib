#pragma once

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace vsdmars {

namespace detail {

// Assumed L1 CPU cache line size in bytes (64B on x86-64 and most ARM64).
inline constexpr std::size_t kCacheLineSize = 64uz;

// Computes the largest power-of-two mini-heap shift such that 2^shift priority
// elements fit inside a single cache line (clamped to a minimum shift of 2,
// i.e., 4 elements per mini-heap).
// Examples: 4B int -> shift 4 (16 elems/64B), 8B int64 -> shift 3 (8
// elems/64B).
template <typename Prio> constexpr std::size_t default_block_shift() noexcept {
  constexpr std::size_t per_line = kCacheLineSize / sizeof(Prio);
  constexpr std::size_t shift =
      per_line > 1uz ? std::bit_width(per_line) - 1uz : 0uz;
  return shift < 2uz ? 2uz : shift;
}

// Minimal over-aligned allocator so backing arrays start on an exact cache-line
// boundary and mini-heap blocks never straddle two cache lines.
template <typename U, std::size_t Alignment> struct AlignedAllocator {
  using value_type = U;

  static constexpr std::size_t kAlign = std::max(alignof(U), Alignment);
  static_assert(std::has_single_bit(kAlign),
                "Alignment must be a power of two.");

  constexpr AlignedAllocator() noexcept = default;

  template <typename V>
  constexpr AlignedAllocator(const AlignedAllocator<V, Alignment> &) noexcept {}

  template <typename V> struct rebind { // NOLINT(readability-identifier-naming)
    using other = AlignedAllocator<V, Alignment>;
  };

  [[nodiscard]] U *allocate(std::size_t n) {
    if (n > (static_cast<std::size_t>(-1) / sizeof(U))) {
      throw std::bad_alloc();
    }
    return static_cast<U *>(
        ::operator new(n * sizeof(U), std::align_val_t{kAlign}));
  }

  void deallocate(U *p, std::size_t) noexcept {
    ::operator delete(p, std::align_val_t{kAlign});
  }

  template <typename V>
  constexpr bool
  operator==(const AlignedAllocator<V, Alignment> &) const noexcept {
    return true;
  }
};

struct EmptyStorage {};

template <typename V> struct ValueStorage {
  using type = std::vector<V, AlignedAllocator<V, kCacheLineSize>>;
};

template <> struct ValueStorage<void> {
  using type = EmptyStorage;
};

// Issues a non-faulting read prefetch hint for the cache line containing `ptr`.
inline void prefetch_read(const void *ptr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(ptr, 0, 3);
#else
  (void)ptr;
#endif
}

} // namespace detail

/**
 * C++23 Cache-Friendly B-Heap (combining Poul-Henning Kamp's B-heap layout with
 * Björn Fahller's cache-optimization techniques from "What do you mean by
 * 'Cache Friendly'?" / rollbear/prio_queue).
 *
 * Key cache optimizations:
 *   1. Mini-heap blocking (B-heap): packs subtrees of P = 2^BlockShift elements
 *      into contiguous blocks sized by default to fit a single 64B cache line
 *      (override BlockShift = 9 for 4 KiB VM-page blocking).
 *   2. Cache-line alignment: backing storage is 64B-aligned so mini-heaps never
 *      straddle cache lines.
 *   3. Structure of Arrays (SoA): when `Value != void`, priorities (`prios_`)
 *      and payloads (`values_`) live in separate parallel arrays. Comparisons
 *      touch only tightly packed `prios_`, keeping cold payloads out of L1.
 *   4. In-place top rescheduling: `reschedule_top` / `replace_top` update the
 *      root and sift down without touching the cold tail of the heap.
 *   5. Bitmask navigation & prefetching: all parent/child jumps use shifts and
 *      bitmasks (no division/modulo), and cross-block descents prefetch the
 *      next priority block while moving data.
 *
 * Block layout (1-based root at index 1; index 0 is an unused sentinel slot):
 *   - Block 0 (root block, offsets 0..P-1):
 *       offset 0          : unused sentinel
 *       offset 1          : heap root (children at offsets 2 and 3)
 *       offsets 2..P/2-1  : intra-block binary heap (children 2*o, 2*o+1)
 *       offsets P/2..P-1  : P/2 leaves, each owning one child block
 *   - Block b >= 1 (non-root block, offsets 0..P-1):
 *       offsets 0, 1      : two children of a leaf in the parent block; each
 *                           has a single intra-block child (0->2, 1->3)
 *       offsets 2..P/2-1  : intra-block binary heap (children 2*o, 2*o+1)
 *       offsets P/2..P-1  : P/2 leaves, each owning one child block
 *
 * Ordering follows std::priority_queue:
 *   - Compare = std::less<Prio>    -> max-heap (default)
 *   - Compare = std::greater<Prio> -> min-heap
 *
 * Usage:
 *   - Key-only max-heap : vsdmars::BHeap<int> h;
 *   - Key-only min-heap : vsdmars::BHeap<int, void, std::greater<int>> h;
 *   - SoA key+value     : vsdmars::BHeap<int, Payload, std::greater<int>> h;
 *   - From C++23 range  : vsdmars::BHeap<int> h(std::from_range, vec);
 *
 * Not thread-safe. Requires Prio (and Value, if non-void) to be
 * default_initializable (for sentinel slot 0) and movable.
 */
template <typename Prio, typename Value = void,
          typename Compare = std::less<Prio>,
          std::size_t BlockShift = detail::default_block_shift<Prio>()>
  requires std::default_initializable<Prio> && std::movable<Prio> &&
           std::strict_weak_order<Compare, Prio, Prio> &&
           (std::is_void_v<Value> ||
            (std::default_initializable<Value> && std::movable<Value>))
class BHeap {
  static_assert(
      BlockShift >= 2uz,
      "BlockShift must be at least 2 (minimum 4 elements per block).");

  static constexpr bool kHasValue = !std::is_void_v<Value>;
  static constexpr std::size_t kBlockShift = BlockShift;
  static constexpr std::size_t kBlockSize = 1uz << kBlockShift; // P = 2^k
  static constexpr std::size_t kBlockMask = kBlockSize - 1uz;   // low k bits
  static constexpr std::size_t kHalfShift = kBlockShift - 1uz;  // k - 1
  static constexpr std::size_t kHalfSize = 1uz << kHalfShift;   // P / 2
  static constexpr std::size_t kHalfMask = kHalfSize - 1uz;     // low k-1 bits

  using PrioVec = std::vector<
      Prio, detail::AlignedAllocator<Prio, detail::kCacheLineSize>>;
  using ValueVec = detail::ValueStorage<Value>::type;
  using StoredValue =
      std::conditional_t<kHasValue, Value, detail::EmptyStorage>;

  struct ChildInfo {
    std::size_t left; // Index of the first (left or sole) child.
    bool has_right;   // True if a right sibling exists at (left | 1uz).
  };

public:
  using priority_type = Prio;
  using value_type = Value;
  using size_type = std::size_t;
  using key_compare = Compare;

  // Constructs an empty B-heap with the given comparator.
  // Allocates index 0 as an unused sentinel so the root sits at index 1.
  explicit BHeap(Compare comp = Compare())
      : prios_(1uz), comp_(std::move(comp)) {
    if constexpr (kHasValue) {
      values_.emplace_back();
    }
  }

  // [Key-only] Constructs a B-heap from iterator range [first, last) in O(n)
  // time by bulk-inserting priorities and running bottom-up heapify.
  template <std::input_iterator InputIt, std::sentinel_for<InputIt> Sent>
    requires(!kHasValue) &&
                std::convertible_to<std::iter_reference_t<InputIt>, Prio>
  BHeap(InputIt first, Sent last, Compare comp = Compare())
      : prios_(1uz), comp_(std::move(comp)) {
    if constexpr (std::sized_sentinel_for<Sent, InputIt>) {
      prios_.reserve(static_cast<size_type>(last - first) + 1uz);
    }
    for (; first != last; ++first) {
      prios_.emplace_back(*first);
    }
    heapify();
  }

  // [Key-only] Constructs a B-heap from any C++23 input range in O(n) time.
  template <std::ranges::input_range R>
    requires(!kHasValue) &&
                std::convertible_to<std::ranges::range_reference_t<R>, Prio>
  BHeap(std::from_range_t, R &&rng, Compare comp = Compare())
      : prios_(1uz), comp_(std::move(comp)) {
    if constexpr (std::ranges::sized_range<R>) {
      prios_.reserve(std::ranges::size(rng) + 1uz);
    }
    for (auto &&elem : rng) {
      prios_.emplace_back(std::forward<decltype(elem)>(elem));
    }
    heapify();
  }

  // [SoA Key-Value] Constructs a B-heap from any C++23 input range of 2-element
  // tuple-like items (e.g., std::pair, std::tuple, std::views::zip) in O(n)
  // time, splitting priorities and values into separate cache-line-aligned
  // arrays.
  template <std::ranges::input_range R>
    requires kHasValue
  BHeap(std::from_range_t, R &&rng, Compare comp = Compare())
      : prios_(1uz), comp_(std::move(comp)) {
    if constexpr (std::ranges::sized_range<R>) {
      const size_type cap = std::ranges::size(rng) + 1uz;
      prios_.reserve(cap);
      values_.reserve(cap);
    }
    values_.emplace_back();
    for (auto &&kv : rng) {
      using KV = decltype(kv);
      prios_.emplace_back(std::forward_like<KV>(std::get<0>(kv)));
      values_.emplace_back(std::forward_like<KV>(std::get<1>(kv)));
    }
    heapify();
  }

  // [Key-only] Constructs a B-heap from an initializer list of priorities in
  // O(n).
  BHeap(std::initializer_list<Prio> init, Compare comp = Compare())
    requires(!kHasValue)
      : BHeap(std::from_range, init, std::move(comp)) {}

  // [SoA Key-Value] Constructs a B-heap from an initializer list of
  // {priority, value} pairs in O(n) time.
  BHeap(std::initializer_list<std::pair<Prio, StoredValue>> init,
        Compare comp = Compare())
    requires kHasValue
      : BHeap(std::from_range, init, std::move(comp)) {}

  // Returns true if the heap contains no elements (only sentinel slot 0
  // exists).
  [[nodiscard]] bool empty() const noexcept { return prios_.size() <= 1uz; }

  // Returns the number of elements currently stored in the heap.
  [[nodiscard]] size_type size() const noexcept { return prios_.size() - 1uz; }

  // Pre-allocates cache-line-aligned backing capacity for at least `cap`
  // elements (+1 for sentinel slot 0) across priority (and value) arrays.
  void reserve(size_type cap) {
    prios_.reserve(cap + 1uz);
    if constexpr (kHasValue) {
      values_.reserve(cap + 1uz);
    }
  }

  // Removes all elements while keeping sentinel slot 0 intact.
  // Uses pop_back() rather than resize(1) so it is provably non-allocating
  // and noexcept.
  void clear() noexcept {
    while (prios_.size() > 1uz) {
      prios_.pop_back();
    }
    if constexpr (kHasValue) {
      while (values_.size() > 1uz) {
        values_.pop_back();
      }
    }
  }

  // Returns a const reference to the root priority (at index 1).
  // Throws std::out_of_range if the heap is empty.
  [[nodiscard]] const Prio &top_prio() const {
    if (empty()) {
      throw std::out_of_range("BHeap::top_prio() called on empty heap");
    }
    return prios_[1uz];
  }

  // [Key-only] Returns a const reference to the highest-priority element.
  // Throws std::out_of_range if the heap is empty.
  [[nodiscard]] const Prio &top() const
    requires(!kHasValue)
  {
    return top_prio();
  }

  // [SoA Key-Value] Returns a pair of const references {prio, value} for the
  // root element at index 1. Throws std::out_of_range if the heap is empty.
  [[nodiscard]] std::pair<const Prio &, const StoredValue &> top() const
    requires kHasValue
  {
    if (empty()) {
      throw std::out_of_range("BHeap::top() called on empty heap");
    }
    return {prios_[1uz], values_[1uz]};
  }

  // [SoA Key-Value] Returns a reference to the root element's payload,
  // preserving const/value category via C++23 deducing `this` so callers can
  // read (const) or mutate (non-const) payload fields without breaking heap
  // order. Throws std::out_of_range if the heap is empty.
  [[nodiscard]] auto &&top_value(this auto &&self)
    requires kHasValue
  {
    if (self.empty()) {
      throw std::out_of_range("BHeap::top_value() called on empty heap");
    }
    return std::forward_like<decltype(self)>(self.values_[1uz]);
  }

  // [Key-only] Inserts a copy of `prio` at the tail and sifts up in O(log n).
  void push(const Prio &prio)
    requires(!kHasValue)
  {
    prios_.push_back(prio);
    sift_up(size());
  }

  // [Key-only] Moves `prio` into the tail and sifts up in O(log n).
  void push(Prio &&prio)
    requires(!kHasValue)
  {
    prios_.push_back(std::move(prio));
    sift_up(size());
  }

  // [Key-only] Constructs a priority in-place at the tail and sifts up in O(log
  // n).
  template <typename... Args>
    requires(!kHasValue) && std::constructible_from<Prio, Args...>
  void emplace(Args &&...args) {
    prios_.emplace_back(std::forward<Args>(args)...);
    sift_up(size());
  }

  // [SoA Key-Value] Appends `(prio, value)` into the parallel priority/value
  // arrays and sifts up to restore heap order in O(log n) time.
  template <typename P, typename U>
    requires kHasValue && std::constructible_from<Prio, P> &&
             std::constructible_from<Value, U>
  void push(P &&prio, U &&value) {
    prios_.emplace_back(std::forward<P>(prio));
    values_.emplace_back(std::forward<U>(value));
    sift_up(size());
  }

  // [SoA Key-Value] Appends `prio` and constructs the payload in-place from
  // `value_args...`, then sifts up in O(log n) time.
  template <typename P, typename... Args>
    requires kHasValue && std::constructible_from<Prio, P> &&
             std::constructible_from<Value, Args...>
  void emplace(P &&prio, Args &&...value_args) {
    prios_.emplace_back(std::forward<P>(prio));
    values_.emplace_back(std::forward<Args>(value_args)...);
    sift_up(size());
  }

  // Removes the highest-priority element (root index 1) in O(log n) time.
  // Moves the last element into root slot 1, shrinks storage by 1, and sifts
  // down to restore heap order. Throws std::out_of_range if empty.
  void pop() {
    if (empty()) {
      throw std::out_of_range("BHeap::pop() called on empty heap");
    }
    if (size() == 1uz) {
      prios_.pop_back();
      if constexpr (kHasValue) {
        values_.pop_back();
      }
      return;
    }
    prios_[1uz] = std::move(prios_.back());
    prios_.pop_back();
    if constexpr (kHasValue) {
      values_[1uz] = std::move(values_.back());
      values_.pop_back();
    }
    sift_down(1uz);
  }

  // Updates the root element's priority in-place and sifts it down in O(log n).
  // Much more cache-friendly than pop() + push() (e.g., timer rescheduling)
  // because it never touches the cold tail of the heap and stays in L1 when
  // the rescheduled item remains near the top. Throws if empty.
  void reschedule_top(Prio new_prio) {
    if (empty()) {
      throw std::out_of_range("BHeap::reschedule_top() called on empty heap");
    }
    prios_[1uz] = std::move(new_prio);
    sift_down(1uz);
  }

  // [SoA Key-Value] Fused pop() + push(): replaces the root's priority and
  // payload in-place and sifts down in a single pass without touching the tail.
  // Throws std::out_of_range if empty.
  template <typename P, typename U>
    requires kHasValue && std::assignable_from<Prio &, P> &&
             std::assignable_from<Value &, U>
  void replace_top(P &&new_prio, U &&new_value) {
    if (empty()) {
      throw std::out_of_range("BHeap::replace_top() called on empty heap");
    }
    prios_[1uz] = std::forward<P>(new_prio);
    values_[1uz] = std::forward<U>(new_value);
    sift_down(1uz);
  }

private:
  // Computes the parent index of node `i` (precondition: i > 1) using bitmasks:
  //   1. Root block (blk == 0): standard 1-based heap parent (off >> 1).
  //   2. Top of non-root block (off in {0, 1}): climbs up to the owning leaf
  //      in the parent block: parent_blk = (blk - 1) >> kHalfShift,
  //      parent_off = kHalfSize | ((blk - 1) & kHalfMask).
  //   3. Second level of non-root block (off in {2, 3}): single-child link
  //      directly below offsets 0 and 1, computed via (i ^ 2uz).
  //   4. All deeper nodes (off >= 4): intra-block binary heap parent (off >>
  //   1).
  static constexpr size_type parent_index(size_type i) noexcept {
    const size_type blk = i >> kBlockShift;
    const size_type off = i & kBlockMask;

    if (blk == 0uz) {
      return off >> 1uz;
    }
    if ((off & ~1uz) == 0uz) {
      const size_type bm1 = blk - 1uz;
      return ((bm1 >> kHalfShift) << kBlockShift) | kHalfSize |
             (bm1 & kHalfMask);
    }
    if ((off & ~3uz) == 0uz) {
      return i ^ 2uz;
    }
    return (i & ~kBlockMask) | (off >> 1uz);
  }

  // Computes the first child index of node `i` and whether a right sibling
  // exists at (left | 1uz):
  //   1. Block leaf ((off & kHalfSize) != 0, i.e., off in [P/2, P-1]):
  //      children sit at offsets 0 and 1 of child_blk = (blk << kHalfShift)
  //      + (off & kHalfMask) + 1.
  //   2. Top of non-root block (blk != 0 && off in {0, 1}):
  //      single intra-block child at (i | 2uz) (offset 2 or 3).
  //   3. Intra-block internal node (including root slot 1 in block 0):
  //      left child at (base | (off << 1)) and right child at (left | 1uz).
  static constexpr ChildInfo child_index(size_type i) noexcept {
    const size_type blk = i >> kBlockShift;
    const size_type off = i & kBlockMask;

    if ((off & kHalfSize) != 0uz) {
      const size_type child_blk = (blk << kHalfShift) + (off & kHalfMask) + 1uz;
      return {child_blk << kBlockShift, true};
    }
    if (blk != 0uz && (off & ~1uz) == 0uz) {
      return {i | 2uz, false};
    }
    return {(i & ~kBlockMask) | (off << 1uz), true};
  }

  // Picks the higher-priority child in [ch.left, ch.left | 1uz] within [1, n].
  // Only touches `prios_` (never loads cold payloads for losing siblings) and
  // uses a ternary so the compiler can emit a branchless cmov.
  [[nodiscard]] size_type select_best_child(ChildInfo ch, size_type n) const {
    const size_type right = ch.left | 1uz;
    if (ch.has_right && right <= n) {
      return comp_(prios_[ch.left], prios_[right]) ? right : ch.left;
    }
    return ch.left;
  }

  // Restores heap order upwards from index `i` using hole-shifting (1 move per
  // level). Only compares `prios_`; moves `values_` along the winning path.
  void sift_up(size_type i) {
    Prio prio = std::move(prios_[i]);
    if constexpr (kHasValue) {
      auto val = std::move(values_[i]);
      while (i > 1uz) {
        const size_type p = parent_index(i);
        if (!comp_(prios_[p], prio)) {
          break;
        }
        prios_[i] = std::move(prios_[p]);
        values_[i] = std::move(values_[p]);
        i = p;
      }
      prios_[i] = std::move(prio);
      values_[i] = std::move(val);
    } else {
      while (i > 1uz) {
        const size_type p = parent_index(i);
        if (!comp_(prios_[p], prio)) {
          break;
        }
        prios_[i] = std::move(prios_[p]);
        i = p;
      }
      prios_[i] = std::move(prio);
    }
  }

  // Restores heap order downwards from index `i` using hole-shifting.
  // Comparisons read only hot `prios_`. When stepping onto a block leaf
  // ((best & kHalfSize) != 0), prefetches the next priority block before
  // moving data to overlap cache-line fetch latency with the move.
  void sift_down(size_type i) {
    const size_type n = size();
    ChildInfo ch = child_index(i);
    if (ch.left > n) {
      return;
    }

    Prio prio = std::move(prios_[i]);
    if constexpr (kHasValue) {
      auto val = std::move(values_[i]);
      while (ch.left <= n) {
        const size_type best = select_best_child(ch, n);
        if (!comp_(prio, prios_[best])) {
          break;
        }
        const ChildInfo next_ch = child_index(best);
        if (next_ch.left <= n && ((best & kHalfSize) != 0uz)) {
          detail::prefetch_read(&prios_[next_ch.left]);
        }
        prios_[i] = std::move(prios_[best]);
        values_[i] = std::move(values_[best]);
        i = best;
        ch = next_ch;
      }
      prios_[i] = std::move(prio);
      values_[i] = std::move(val);
    } else {
      while (ch.left <= n) {
        const size_type best = select_best_child(ch, n);
        if (!comp_(prio, prios_[best])) {
          break;
        }
        const ChildInfo next_ch = child_index(best);
        if (next_ch.left <= n && ((best & kHalfSize) != 0uz)) {
          detail::prefetch_read(&prios_[next_ch.left]);
        }
        prios_[i] = std::move(prios_[best]);
        i = best;
        ch = next_ch;
      }
      prios_[i] = std::move(prio);
    }
  }

  // Establishes the heap invariant across all elements in O(n) time.
  // Because parent_index(j) < j holds for every j >= 2, iterating downwards
  // from size() to 1 guarantees every node's children are already valid
  // sub-heaps before sifting it down.
  void heapify() {
    for (size_type i = size(); i >= 1uz; --i) {
      sift_down(i);
    }
  }

  PrioVec prios_;
  [[no_unique_address]] ValueVec values_{};
  [[no_unique_address]] Compare comp_{};
};

} // namespace vsdmars
