#include <userver/ugrpc/server/middlewares/base.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server {

MiddlewareBase::MiddlewareBase() = default;

MiddlewareBase::~MiddlewareBase() = default;

MiddlewareCallContext::MiddlewareCallContext(
    const Middlewares& middlewares, CallAnyBase& call,
    utils::function_ref<void()> user_call,
    const dynamic_config::Snapshot& config,
    ::google::protobuf::Message* request)
    : middleware_(middlewares.begin()),
      middleware_end_(middlewares.end()),
      user_call_(std::move(user_call)),
      call_(call),
      config_(config),
      request_(request) {}

void MiddlewareCallContext::Next() {
  if (middleware_ == middleware_end_) {
    ClearMiddlewaresResources();
    user_call_();
  } else {
    // It is important for non-stream calls
    if (request_) {
      (*middleware_)->CallRequestHook(*this, *request_);
    }
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto middleware = middleware_++;

    (*middleware)->Handle(*this);
  }
}

void MiddlewareCallContext::ClearMiddlewaresResources() {
  UASSERT(config_);
  config_.reset();
}

CallAnyBase& MiddlewareCallContext::GetCall() const { return call_; }

std::string_view MiddlewareCallContext::GetServiceName() const {
  return call_.GetServiceName();
}

std::string_view MiddlewareCallContext::GetMethodName() const {
  return call_.GetMethodName();
}

const dynamic_config::Snapshot& MiddlewareCallContext::GetInitialDynamicConfig()
    const {
  UASSERT(config_);
  return config_.value();
}

void MiddlewareBase::CallRequestHook(const MiddlewareCallContext&,
                                     google::protobuf::Message&){};

void MiddlewareBase::CallResponseHook(const MiddlewareCallContext&,
                                      google::protobuf::Message&){};

}  // namespace ugrpc::server

USERVER_NAMESPACE_END
