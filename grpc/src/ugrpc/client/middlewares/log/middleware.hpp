#pragma once

#include <cstddef>

#include <userver/ugrpc/client/middlewares/base.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::client::middlewares::log {

struct Settings {
  /// Max gRPC message size, the rest will be truncated
  std::size_t max_msg_size{512};

  /// gRPC message logging level
  logging::Level log_level{logging::Level::kDebug};
};

/// @brief middleware for RPC handler logging settings
class Middleware final : public MiddlewareBase {
 public:
  explicit Middleware(const Settings& settings);

  void Handle(MiddlewareCallContext& context) const override;

 private:
  Settings settings_;
};

/// @cond
class MiddlewareFactory final : public MiddlewareFactoryBase {
 public:
  explicit MiddlewareFactory(const Settings& settings);

  std::shared_ptr<const MiddlewareBase> GetMiddleware(
      std::string_view client_name) const override;

 private:
  Settings settings_;
};
/// @endcond

}  // namespace ugrpc::client::middlewares::log

USERVER_NAMESPACE_END
