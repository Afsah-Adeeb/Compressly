#pragma once
//
// A fixed-size thread pool with a shared work queue.
//
// Written by hand rather than reached for from a library, and deliberately not something
// cleverer. Two decisions worth defending:
//
// - NOT OpenMP. A `#pragma omp parallel for` would do this in one line, and that is the
//   problem: the pragma would be doing the work, and there would be nothing to explain.
//
// - NOT work stealing. Every block is the same size and costs roughly the same to
//   compress, so there is no skew for stealing to correct. It would add per-worker deques
//   and a steal protocol to solve a problem the workload does not have. The measured
//   speedup curve (NOTES.md) flattens for reasons stealing would not touch.
//
// The pool runs one job at a time: forEach() hands out indices 0..count-1 to the workers
// and blocks until all of them are finished. That is all the compressor needs -- blocks
// are independent, so there is no dependency graph to schedule, just a range to divide.
//
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cmpr {

// Turns a requested thread count into a real one: 0 means "ask the hardware", and the
// result is always at least 1.
int resolveThreadCount(int requested);

class ThreadPool {
 public:
  explicit ThreadPool(int workers);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Calls fn(i) for every i in [0, count), spread across the workers, and returns once
  // all of them have completed. If any call throws, the first exception is rethrown here
  // after every task has finished -- a task that throws must still be counted, or the
  // wait below would never be satisfied.
  void forEach(std::size_t count, const std::function<void(std::size_t)>& fn);

  int workerCount() const { return static_cast<int>(workers_.size()); }

 private:
  void workerLoop();

  std::vector<std::thread> workers_;

  std::mutex mutex_;
  std::condition_variable workAvailable_;
  std::condition_variable jobFinished_;

  const std::function<void(std::size_t)>* job_ = nullptr;
  std::size_t nextIndex_ = 0;   // next task to hand out
  std::size_t taskCount_ = 0;   // tasks in the current job
  std::size_t outstanding_ = 0; // handed out or not yet started, still unfinished
  std::exception_ptr firstError_;
  bool stopping_ = false;
};

}  // namespace cmpr
