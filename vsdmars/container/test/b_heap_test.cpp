#include <gtest/gtest.h>
#include <vsdmars/container/b_heap.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <random>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using vsdmars::BHeap;

struct BigPayload {
  int id = 0;
  std::string label;
  std::array<char, 128> pad{};
};

struct alignas(128) OverAligned {
  int x = 0;
};

template <typename Compare, std::size_t BlockShift>
void fuzz_key_only(int seed, int ops) {
  std::mt19937 rng(static_cast<std::uint32_t>(seed));
  std::uniform_int_distribution<int> val_dist(-5000, 5000);
  std::uniform_int_distribution<int> op_dist(0, 99);

  BHeap<int, void, Compare, BlockShift> bh;
  std::priority_queue<int, std::vector<int>, Compare> pq;

  bh.reserve(static_cast<std::size_t>(ops));
  for (int step = 0; step < ops; ++step) {
    int r = op_dist(rng);
    if (r < 50 || pq.empty()) {
      int v = val_dist(rng);
      if (r % 2 == 0) {
        bh.push(v);
      } else {
        bh.emplace(v);
      }
      pq.push(v);
    } else if (r < 80) {
      ASSERT_EQ(bh.top(), pq.top());
      bh.pop();
      pq.pop();
    } else {
      int nv = val_dist(rng);
      ASSERT_EQ(bh.top(), pq.top());
      bh.reschedule_top(nv);
      pq.pop();
      pq.push(nv);
    }
    ASSERT_EQ(bh.size(), pq.size());
    ASSERT_EQ(bh.empty(), pq.empty());
    if (!pq.empty()) {
      ASSERT_EQ(bh.top(), pq.top());
    }
  }

  while (!pq.empty()) {
    ASSERT_EQ(bh.top(), pq.top());
    bh.pop();
    pq.pop();
  }
  EXPECT_TRUE(bh.empty());
}

template <typename Compare, std::size_t BlockShift>
void fuzz_soa_kv(int seed, int ops) {
  std::mt19937 rng(static_cast<std::uint32_t>(seed));
  std::uniform_int_distribution<int> prio_dist(-5000, 5000);
  std::uniform_int_distribution<int> op_dist(0, 99);

  struct Key {
    int prio = 0;
    int seq = 0;
  };
  struct KeyComp {
    Compare c{};
    bool operator()(const Key& a, const Key& b) const {
      if (a.prio != b.prio) {
        return c(a.prio, b.prio);
      }
      return c(a.seq, b.seq);
    }
  };
  struct PqEntry {
    Key k;
    int payload_id = 0;
  };
  struct PqComp {
    KeyComp kc{};
    bool operator()(const PqEntry& a, const PqEntry& b) const {
      return kc(a.k, b.k);
    }
  };

  BHeap<Key, BigPayload, KeyComp, BlockShift> bh;
  std::priority_queue<PqEntry, std::vector<PqEntry>, PqComp> pq;

  int next_seq = 1;
  for (int step = 0; step < ops; ++step) {
    int r = op_dist(rng);
    if (r < 45 || pq.empty()) {
      Key k{.prio = prio_dist(rng), .seq = next_seq++};
      BigPayload p{.id = k.seq, .label = "item_" + std::to_string(k.seq)};
      if (r % 2 == 0) {
        bh.push(k, std::move(p));
      } else {
        bh.emplace(
            k, BigPayload{.id = k.seq, .label = "item_" + std::to_string(k.seq)});
      }
      pq.push({.k = k, .payload_id = k.seq});
    } else if (r < 70) {
      ASSERT_EQ(bh.top_prio().seq, pq.top().k.seq);
      ASSERT_EQ(bh.top_value().id, pq.top().payload_id);
      bh.pop();
      pq.pop();
    } else if (r < 85) {
      int saved_id = pq.top().payload_id;
      ASSERT_EQ(bh.top_value().id, saved_id);
      Key nk{.prio = prio_dist(rng), .seq = next_seq++};
      bh.reschedule_top(nk);
      pq.pop();
      pq.push({.k = nk, .payload_id = saved_id});
    } else {
      Key nk{.prio = prio_dist(rng), .seq = next_seq++};
      bh.replace_top(
          nk, BigPayload{.id = nk.seq, .label = "rep_" + std::to_string(nk.seq)});
      pq.pop();
      pq.push({.k = nk, .payload_id = nk.seq});
    }
    ASSERT_EQ(bh.size(), pq.size());
    if (!pq.empty()) {
      auto [tp, tv] = bh.top();
      ASSERT_EQ(tp.prio, pq.top().k.prio);
      ASSERT_EQ(tp.seq, pq.top().k.seq);
      ASSERT_EQ(tv.id, pq.top().payload_id);
    }
  }

  while (!pq.empty()) {
    ASSERT_EQ(bh.top_prio().seq, pq.top().k.seq);
    ASSERT_EQ(bh.top_value().id, pq.top().payload_id);
    bh.pop();
    pq.pop();
  }
  EXPECT_TRUE(bh.empty());
}

TEST(BHeapTest, CompileTimeInvariants) {
  static_assert(vsdmars::detail::default_block_shift<std::int32_t>() == 4uz);
  static_assert(vsdmars::detail::default_block_shift<std::int64_t>() == 3uz);
  static_assert(vsdmars::detail::default_block_shift<std::uint16_t>() == 5uz);
  static_assert(sizeof(BHeap<int>) == sizeof(std::vector<int>));
  SUCCEED();
}

TEST(BHeapTest, FromRangeKeyOnlyNonSizedView) {
  auto evens = std::views::iota(1, 20) |
               std::views::filter([](int x) { return (x % 2) == 0; });
  BHeap<int> h(std::from_range, evens);
  EXPECT_EQ(h.size(), 9uz);
  EXPECT_EQ(h.top(), 18);
  h.clear();
  EXPECT_TRUE(h.empty());
  EXPECT_EQ(h.size(), 0uz);
}

TEST(BHeapTest, FromRangeSoaCopiesLvalueAndDeducingThis) {
  std::vector<std::pair<int, std::string>> src{
      {30, "thirty"}, {10, "ten"}, {20, "twenty"}};
  BHeap<int, std::string, std::greater<>> h(std::from_range, src);
  EXPECT_EQ(src[0].second, "thirty");
  EXPECT_EQ(src[1].second, "ten");
  EXPECT_EQ(h.top_prio(), 10);
  EXPECT_EQ(h.top_value(), "ten");

  h.top_value() = "TEN";
  const auto& ch = h;
  static_assert(std::is_same_v<decltype(ch.top_value()), const std::string&>);
  static_assert(std::is_same_v<decltype(h.top_value()), std::string&>);
  static_assert(
      std::is_same_v<decltype(std::move(h).top_value()), std::string&&>);
  EXPECT_EQ(ch.top_value(), "TEN");

  std::string moved_val = std::move(h).top_value();
  EXPECT_EQ(moved_val, "TEN");
}

TEST(BHeapTest, FromRangeSoaViewsZip) {
  std::vector<int> ks{42, 7, 19, 3};
  std::vector<std::string> vs{"forty-two", "seven", "nineteen", "three"};
  BHeap<int, std::string, std::greater<>> zh(std::from_range,
                                             std::views::zip(ks, vs));
  EXPECT_EQ(zh.size(), 4uz);
  EXPECT_EQ(zh.top_prio(), 3);
  EXPECT_EQ(zh.top_value(), "three");
  zh.pop();
  EXPECT_EQ(zh.top_prio(), 7);
  EXPECT_EQ(zh.top_value(), "seven");
}

TEST(BHeapTest, CacheLineAndOverAlignment) {
  BHeap<int, BigPayload, std::greater<>> h;
  h.reserve(1024uz);
  h.push(10, BigPayload{.id = 1, .label = "a"});
  auto prio_base = reinterpret_cast<std::uintptr_t>(&h.top_prio() - 1);
  auto val_base = reinterpret_cast<std::uintptr_t>(&h.top_value() - 1);
  EXPECT_EQ(prio_base % 64uz, 0uz);
  EXPECT_EQ(val_base % 64uz, 0uz);

  BHeap<int, OverAligned> oa;
  oa.push(5, OverAligned{.x = 42});
  oa.push(9, OverAligned{.x = 77});
  auto oa_base = reinterpret_cast<std::uintptr_t>(&oa.top_value() - 1);
  EXPECT_EQ(oa_base % 128uz, 0uz);
  EXPECT_EQ(oa.top_value().x, 77);
}

TEST(BHeapTest, CustomLambdaComparatorAndIterators) {
  auto cmp = [](int a, int b) { return a > b; };
  std::vector<int> v{5, 1, 9, 2, 8};
  BHeap<int, void, decltype(cmp)> lh(v.begin(), v.end(), cmp);
  EXPECT_EQ(lh.top(), 1);
  lh.pop();
  EXPECT_EQ(lh.top(), 2);
}

TEST(BHeapTest, EmptyHeapThrows) {
  BHeap<int, std::string> sh;
  EXPECT_THROW((void)sh.top(), std::out_of_range);
  EXPECT_THROW((void)sh.top_prio(), std::out_of_range);
  EXPECT_THROW((void)sh.top_value(), std::out_of_range);
  EXPECT_THROW(sh.pop(), std::out_of_range);
  EXPECT_THROW(sh.reschedule_top(1), std::out_of_range);
  EXPECT_THROW(sh.replace_top(1, "x"), std::out_of_range);
}

TEST(BHeapTest, FuzzKeyOnlyAgainstPriorityQueue) {
  fuzz_key_only<std::less<>, 2>(1, 30000);
  fuzz_key_only<std::greater<>, 4>(2, 30000);
  fuzz_key_only<std::less<>, 9>(3, 30000);
}

TEST(BHeapTest, FuzzSoaKeyValueAgainstPriorityQueue) {
  fuzz_soa_kv<std::less<>, 2>(10, 20000);
  fuzz_soa_kv<std::greater<>, 3>(11, 20000);
  fuzz_soa_kv<std::less<>, 4>(12, 20000);
  fuzz_soa_kv<std::greater<>, 9>(13, 20000);
}

}  // namespace
