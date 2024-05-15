#include <engine/task/push_strategy_queue/task_queue.hpp>

#include <engine/task/task_context.hpp>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

USERVER_NAMESPACE_BEGIN

namespace engine {

#ifdef __linux__
void FutexWait(std::atomic<std::uint32_t>* value, int expected_value) {
    syscall(SYS_futex, value, FUTEX_WAIT, expected_value, nullptr, nullptr, 0);
}

void FutexWake(std::atomic<std::uint32_t>* value, int count) {
    syscall(SYS_futex, value, FUTEX_WAKE, count, nullptr, nullptr, 0);
}  // namespace


void FutexWaitTaskContextPtr(std::atomic<impl::TaskContext*>* value, impl::TaskContext* expected_value) {
    syscall(SYS_futex, value, FUTEX_WAIT, expected_value, nullptr, nullptr, 0);
}

void FutexWakeTaskContextPtr(std::atomic<impl::TaskContext*>* value, int count) {
    syscall(SYS_futex, value, FUTEX_WAKE, count, nullptr, nullptr, 0);
}  // namespace
#endif


namespace {
// It is only used in worker threads outside of any coroutine,
// so it does not need to be protected via compiler::ThreadLocal
thread_local ssize_t localWorkerIndex = -1;
}

PushStrategyTaskQueue::PushStrategyTaskQueue(const TaskProcessorConfig& config)
  : workers_count_(config.worker_threads),
    is_terminate_(false),
    current_task_(config.worker_threads, nullptr),
    sleep_state_(config.worker_threads, SleepState::kBusy)
{
}

void PushStrategyTaskQueue::Push(boost::intrusive_ptr<impl::TaskContext>&& context) {
  DoPush(context.get());
  context.detach();
}

boost::intrusive_ptr<impl::TaskContext> PushStrategyTaskQueue::PopBlocking() {
  boost::intrusive_ptr<impl::TaskContext> context{DoPopBlocking(),
                                                  /* add_ref= */ false};
  if (!context) {
    DoPush(nullptr);
  }

  return context;
}

void PushStrategyTaskQueue::StopProcessing() {
  is_terminate_.store(true);
}

std::size_t PushStrategyTaskQueue::GetSizeApproximate() const noexcept {
  return global_queue_.size_approx();
}

void PushStrategyTaskQueue::DoPush(impl::TaskContext* context) {
  size_t start_index = 0;
  if (localWorkerIndex != -1) {
    start_index = localWorkerIndex;
  }

  auto try_give_task = [&](size_t workerIndex, SleepState state) {
    if (sleep_state_[workerIndex].load() == state) {
      impl::TaskContext* expected_value = nullptr;
      if (current_task_[workerIndex].compare_exchange_strong(expected_value, context)) {
        // if worker fell asleep at last moment
        FutexWakeTaskContextPtr(&current_task_[workerIndex], 1);
        return true;
      }
    }
    return false;
  };
  // try to give task first to spinning workers
  for (size_t i = start_index; i < workers_count_; ++i) {
    if (try_give_task(i, kSpinning)) {
      return;
    }
  }
  for (size_t i = 0; i < start_index; ++i) {
    if (try_give_task(i, kSpinning)) {
      return;
    }
  }

  for (size_t i = start_index; i < workers_count_; ++i) {
    if (try_give_task(i, kSleeping)) {
      return;
    }
  }
  for (size_t i = 0; i < start_index; ++i) {
    if (try_give_task(i, kSleeping)) {
      return;
    }
  }
  // this point all workers busy
  global_queue_.enqueue(context);
  // ??? wake some workers
}

impl::TaskContext* PushStrategyTaskQueue::DoPopBlocking() {
  if (localWorkerIndex == -1) {
    DetermineWorker();
  }

  int spinCount = 0;
  impl::TaskContext* item = nullptr;

  sleep_state_[localWorkerIndex].store(kSpinning);

  while (true) {
    spinCount = 10000;
    if (is_terminate_.load()) {
        return nullptr;
    }
    while (spinCount--) {
      if (current_task_[localWorkerIndex].load() != nullptr) {
        sleep_state_[localWorkerIndex].store(kBusy);
        item = current_task_[localWorkerIndex].load();
        assert(current_task_[localWorkerIndex].compare_exchange_strong(item, nullptr));
        return item;
      }
      // with some chance so that we don't have
      // too many situations where we have to put an item back in the queue.
      if (spinCount % 10 == 0) {
        if (global_queue_.try_dequeue(item)) {
          impl::TaskContext* second_task = nullptr;
          // this moment can be given task in current_task_,
          // so we should check it and process only one of two available tasks
          if (!current_task_[localWorkerIndex].compare_exchange_strong(second_task, item)) {
            global_queue_.enqueue(item);
            item = second_task;
          }
          sleep_state_[localWorkerIndex].store(kBusy);
          assert(current_task_[localWorkerIndex].compare_exchange_strong(item, nullptr));
          return item;
        }
      }
    }
    sleep_state_[localWorkerIndex].store(kSleeping);
    while (current_task_[localWorkerIndex].load() == nullptr && !is_terminate_.load()) {
      FutexWaitTaskContextPtr(&current_task_[localWorkerIndex], nullptr);
    }
    sleep_state_[localWorkerIndex].store(kSpinning);
  }
}

void PushStrategyTaskQueue::DetermineWorker() {
  std::size_t index = workers_order_.load();
  while (!workers_order_.compare_exchange_weak(index, index + 1)) {
  }
  assert(index < workers_count_);
  localWorkerIndex = index;
}

}  // namespace engine

USERVER_NAMESPACE_END
