#include <iostream>
#include <engine/task/push_strategy_queue/task_queue.hpp>

#include <engine/task/task_context.hpp>
#include <string>

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

#endif

std::string SleepStateToString(const SleepState& sleepState) {
  switch (sleepState) {
    case kBusy:
      return "kBusy";
    case kSpinning:
      return "kSpinning";
    case kSleeping:
      return "kSleeping";
    default:
      return "kUnknown";
  }
}

namespace {
// It is only used in worker threads outside of any coroutine,
// so it does not need to be protected via compiler::ThreadLocal
thread_local ssize_t localWorkerIndex = -1;
}

PushStrategyTaskQueue::PushStrategyTaskQueue(const TaskProcessorConfig& config)
  : workers_count_(config.worker_threads),
    is_terminate_(false),
    current_task_(config.worker_threads, nullptr),
    sleep_state_(config.worker_threads, SleepState::kBusy),
    sleep_variable_(config.worker_threads, 0)
{
  std::cout << "constructor\n" << std::flush;
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
  std::cout << "------TERMINATE------\n" << std::flush;
  is_terminate_.store(true);
  for (size_t i = 0; i < workers_count_; ++i) {
      sleep_variable_[i].store(1);
      FutexWake(&sleep_variable_[i], 1);
  }
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
        sleep_variable_[workerIndex].fetch_add(1);
        FutexWake(&sleep_variable_[workerIndex], 1);
        // auto s = "[PUSH] " + std::to_string(localWorkerIndex) + " to worker " + std::to_string(workerIndex) + " " + SleepStateToString(state) + "\n";
        // std::cout << s << std::flush;
        return true;
      }
    }
    return false;
  };
  // try to give task first to spinning workers
  for (size_t i = start_index; i < workers_count_; ++i) {
    if (try_give_task(i, kSpinning)) {
      spinning_hops.fetch_add(1);
      return;
    }
  }
  for (size_t i = 0; i < start_index; ++i) {
    if (try_give_task(i, kSpinning)) {
      spinning_hops.fetch_add(1);
      return;
    }
  }

  for (size_t i = start_index; i < workers_count_; ++i) {
    if (try_give_task(i, kSleeping)) {
      sleep_hops.fetch_add(1);
      return;
    }
  }
  for (size_t i = 0; i < start_index; ++i) {
    if (try_give_task(i, kSleeping)) {
      sleep_hops.fetch_add(1);
      return;
    }
  }
  // this point all workers busy
  // std::cout << "[PUSH] " + std::to_string(localWorkerIndex) + " to global queue. past size=" + std::to_string(global_queue_.size_approx()) + "\n" << std::flush;
  queue_hops.fetch_add(1);
  // for (size_t i = 0; i < workers_count_; ++i) {
  //   FutexWake(&sleep_variable_[i], 1);
  // }
  assert(global_queue_.enqueue(context));
  // FutexWakeTaskContextPtr(&current_task_[workerIndex], 1);
  // ??? wake some workers
}

impl::TaskContext* PushStrategyTaskQueue::DoPopBlocking() {
  if (localWorkerIndex == -1) {
    DetermineWorker();
  }

  int spinCount = 0;
  impl::TaskContext* item = nullptr;

  auto sleepStateInfo = [](SleepState from, SleepState to) {
    return std::to_string(localWorkerIndex) + " " + SleepStateToString(from) + "-->" + SleepStateToString(to);
  };
  // std::cout << sleepStateInfo(sleep_state_[localWorkerIndex], kSpinning) + "\n" << std::flush;
  sleep_state_[localWorkerIndex].store(kSpinning);

  while (true) {
    spinCount = 10000;
    if (is_terminate_.load()) {
        return nullptr;
    }
    while (spinCount--) {
      if (item = current_task_[localWorkerIndex].load(); item != nullptr) {
        // std::cout << sleepStateInfo(sleep_state_[localWorkerIndex], kBusy) + " SPIN\n" << std::flush;
        sleep_state_[localWorkerIndex].store(kBusy);

        sleep_variable_[localWorkerIndex].fetch_sub(1);
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
            assert(global_queue_.enqueue(item));
            item = second_task;
          }
          // std::cout << sleepStateInfo(sleep_state_[localWorkerIndex], kBusy) + " GLOBAL\n" << std::flush;
          sleep_state_[localWorkerIndex].store(kBusy);
          sleep_variable_[localWorkerIndex].fetch_sub(1);
          assert(current_task_[localWorkerIndex].compare_exchange_strong(item, nullptr));
          return item;
        }
      }
    }
    // std::cout << sleepStateInfo(sleep_state_[localWorkerIndex], kSleeping) + " queue size=" + std::to_string(global_queue_.size_approx()) + "\n" << std::flush;
    sleep_state_[localWorkerIndex].store(kSleeping);
    while (sleep_variable_[localWorkerIndex].load() == 0 && !is_terminate_.load()) {
      FutexWait(&sleep_variable_[localWorkerIndex], 0);
    }
    // std::cout << sleepStateInfo(sleep_state_[localWorkerIndex], kSpinning) + " wake\n" << std::flush;
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

  PushStrategyTaskQueue::~PushStrategyTaskQueue() {
    std::cout << "spinning_hops=" << spinning_hops.load() << "\n";
    std::cout << "sleep_hops=" << sleep_hops.load() << "\n";
    std::cout << "queue_hops=" << queue_hops.load() << "\n";
    std::cout << std::flush;
  }
}  // namespace engine

USERVER_NAMESPACE_END