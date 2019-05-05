#pragma once

#include <cxxabi.h>
#include <cstring>

#if defined(__linux__) && defined(__GLIBCXX__)

#define USERVER_EHGLOBALS_INTERPOSE

namespace engine::impl {

// This structure is a __cxa_eh_globals storage for coroutines.
// It is required to allow task switching during unwind.
struct EhGlobals {
  void* data[4];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init, hicpp-member-init)
  EhGlobals() noexcept { ::memset(data, 0, sizeof(data)); }
};

// NOLINTNEXTLINE(hicpp-use-noexcept,modernize-use-noexcept)
abi::__cxa_eh_globals* GetGlobals() throw();

}  // namespace engine::impl

#elif defined(__APPLE__) && defined(_LIBCPP_VERSION)

// MAC_COMPAT
// "only INSERTED libraries can interpose", you say
// let's juggle some razor blades then
#define USERVER_EHGLOBALS_SWAP

namespace __cxxabiv1 {
struct __cxa_exception;
}  // namespace __cxxabiv1

namespace engine::impl {

struct EhGlobals {
  __cxxabiv1::__cxa_exception* caught_exceptions{nullptr};
  unsigned int uncaught_exceptions{0};
};

}  // namespace engine::impl

#else
#error "We don't support exceptions in your environment"
#endif

namespace engine::impl {
void ExchangeEhGlobals(EhGlobals&) noexcept;
}  // namespace engine::impl
