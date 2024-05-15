

#pragma once

#include <moodycamel/concurrentqueue.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include <engine/task/task_processor_config.hpp>

#include <userver/utils/fixed_array.hpp>

USERVER_NAMESPACE_BEGIN

namespace engine {

namespace impl {
class TaskContext;
}  // namespace impl

enum SleepState : std::uint32_t {
    kBusy,
    kSpinning,
    kSleeping
};

class PushStrategyTaskQueue {

 public:
  explicit PushStrategyTaskQueue(const TaskProcessorConfig& config);

  void Push(boost::intrusive_ptr<impl::TaskContext>&& context);

  boost::intrusive_ptr<impl::TaskContext> PopBlocking();

  void StopProcessing();

  std::size_t GetSizeApproximate() const noexcept;

 private:
  void DoPush(impl::TaskContext* context);
  impl::TaskContext* DoPopBlocking();
  void DetermineWorker();

  const std::size_t workers_count_;
  std::atomic<int> is_terminate_;
  utils::FixedArray<std::atomic<impl::TaskContext*>> current_task_;
  utils::FixedArray<std::atomic<SleepState>> sleep_state_; 
  moodycamel::ConcurrentQueue<impl::TaskContext*> global_queue_;
  std::atomic<std::size_t> workers_order_{0};
};

}  // namespace engine

USERVER_NAMESPACE_END
