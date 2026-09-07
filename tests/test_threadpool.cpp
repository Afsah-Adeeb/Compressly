#include <atomic>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "testing.h"
#include "threadpool.h"

using cmpr::ThreadPool;

TEST(ThreadPoolRunsEveryIndexExactlyOnce) {
  // The property the compressor depends on: every block gets compressed, once. A pool
  // that dropped or duplicated an index would produce a corrupt file rather than a slow
  // one, which is a far worse failure.
  for (int workers : {1, 2, 4, 8}) {
    constexpr std::size_t kTasks = 1000;
    std::vector<std::atomic<int>> visits(kTasks);
    for (std::atomic<int>& v : visits) v.store(0);

    ThreadPool pool(workers);
    pool.forEach(kTasks, [&](std::size_t index) { visits[index].fetch_add(1); });

    for (std::size_t i = 0; i < kTasks; ++i) CHECK_EQ(visits[i].load(), 1);
  }
}

TEST(ThreadPoolActuallyUsesItsWorkers) {
  // Confirms the tasks are spread rather than all executed by one thread -- otherwise
  // every speedup measurement downstream would be meaningless.
  ThreadPool pool(4);
  std::atomic<int> concurrent{0};
  std::atomic<int> peak{0};

  pool.forEach(64, [&](std::size_t) {
    const int now = concurrent.fetch_add(1) + 1;
    int seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
    // Enough work that overlap is near-certain without making the test slow.
    volatile double sink = 0;
    for (int i = 0; i < 200000; ++i) sink += i * 0.5;
    (void)sink;
    concurrent.fetch_sub(1);
  });

  CHECK(peak.load() > 1);
}

TEST(ThreadPoolPropagatesTheFirstException) {
  // A task that throws must still be counted as finished, or forEach would wait forever
  // for a task that is never coming back. This test would hang rather than fail if that
  // bookkeeping were wrong.
  ThreadPool pool(4);
  bool threw = false;
  try {
    pool.forEach(100, [](std::size_t index) {
      if (index == 57) throw std::runtime_error("task failed");
    });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);

  // And the pool must still be usable afterwards -- the error state cannot leak into the
  // next job.
  std::atomic<int> ran{0};
  pool.forEach(10, [&](std::size_t) { ran.fetch_add(1); });
  CHECK_EQ(ran.load(), 10);
}

TEST(ThreadPoolHandlesEmptyAndSingleJobs) {
  ThreadPool pool(4);
  pool.forEach(0, [](std::size_t) { CHECK(false); });

  std::atomic<int> ran{0};
  pool.forEach(1, [&](std::size_t index) {
    CHECK_EQ(index, std::size_t{0});
    ran.fetch_add(1);
  });
  CHECK_EQ(ran.load(), 1);
}

TEST(ThreadCountResolution) {
  CHECK_EQ(cmpr::resolveThreadCount(1), 1);
  CHECK_EQ(cmpr::resolveThreadCount(4), 4);
  CHECK(cmpr::resolveThreadCount(0) >= 1);   // 0 means "ask the hardware"
  CHECK(cmpr::resolveThreadCount(-1) >= 1);
}
