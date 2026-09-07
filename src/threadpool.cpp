#include "threadpool.h"

#include <algorithm>

namespace cmpr {

int resolveThreadCount(int requested) {
  if (requested > 0) return requested;
  const unsigned hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1 : static_cast<int>(hardware);
}

ThreadPool::ThreadPool(int workers) {
  const int count = std::max(1, workers);
  workers_.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  workAvailable_.notify_all();
  for (std::thread& worker : workers_) worker.join();
}

void ThreadPool::workerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    workAvailable_.wait(lock, [this] { return stopping_ || nextIndex_ < taskCount_; });
    if (stopping_) return;

    const std::size_t index = nextIndex_++;

    // The task runs with the lock released -- the whole point is that workers compress
    // different blocks simultaneously. Everything they touch is either read-only shared
    // state or a slot indexed by their own task number, so no locking is needed inside.
    lock.unlock();
    try {
      (*job_)(index);
    } catch (...) {
      lock.lock();
      if (!firstError_) firstError_ = std::current_exception();
      lock.unlock();
    }
    lock.lock();

    // Decremented whether the task succeeded or threw. Skipping this on the error path
    // would leave forEach() waiting forever for a task that is never coming back.
    if (--outstanding_ == 0) jobFinished_.notify_all();
  }
}

void ThreadPool::forEach(std::size_t count, const std::function<void(std::size_t)>& fn) {
  if (count == 0) return;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = &fn;
    nextIndex_ = 0;
    taskCount_ = count;
    outstanding_ = count;
    firstError_ = nullptr;
  }
  workAvailable_.notify_all();

  std::exception_ptr error;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    jobFinished_.wait(lock, [this] { return outstanding_ == 0; });
    job_ = nullptr;
    taskCount_ = 0;
    nextIndex_ = 0;
    error = firstError_;
    firstError_ = nullptr;
  }
  if (error) std::rethrow_exception(error);
}

}  // namespace cmpr
