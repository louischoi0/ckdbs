#pragma once

#include <memory>
#include <type_traits>
#include <utility>

// A non-owning reference to a callable: two pointers, no allocation, no copy
// of the callable. What `std::function` costs per construction - a heap
// allocation whenever the callable outgrows its small buffer, which any lambda
// capturing more than two references does - is what the insert path cannot
// pay per row (BB-S3's review: one or two allocations per `INSERT` row).
//
// **It does not extend the callable's life.** Bind it to a named callable, or
// to a temporary in the same full-expression as the call that uses it; never
// store one beyond that call. A default-constructed reference is empty, and
// calling an empty one is undefined - test it with `explicit operator bool`
// first where it may be empty.

namespace kds {

template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
public:
    FunctionRef() noexcept = default;

    template <typename F, typename = std::enable_if_t<
                              !std::is_same_v<std::remove_cvref_t<F>, FunctionRef> &&
                              std::is_invocable_r_v<R, F&, Args...>>>
    FunctionRef(F&& f) noexcept  // NOLINT(google-explicit-constructor): binds like a reference
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_([](void* object, Args... args) -> R {
              return (*static_cast<std::remove_reference_t<F>*>(object))(
                  std::forward<Args>(args)...);
          }) {}

    R operator()(Args... args) const { return call_(object_, std::forward<Args>(args)...); }

    explicit operator bool() const noexcept { return call_ != nullptr; }

private:
    void* object_ = nullptr;
    R (*call_)(void*, Args...) = nullptr;
};

}  // namespace kds
