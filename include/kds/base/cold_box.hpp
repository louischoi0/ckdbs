#pragma once

#include <memory>

namespace kds {

// A value held off its owner's footprint: one pointer wide, empty until
// `emplace`, copied by value - a copy of the box is a copy of what it holds.
//
// For a field almost every instance of a hot struct leaves empty, where the
// struct's own size is a measured cost: EB's close measured 48 bytes more of
// `exec::Step` alone at about 2 % of a point read's server CPU
// (`bench/v3.5.0/results-eb-close-v3.4.17-231-ga610d007a.md`).
template <typename T>
class ColdBox {
public:
    ColdBox() = default;
    ColdBox(const ColdBox& other) : value_(other.value_ ? std::make_unique<T>(*other.value_) : nullptr) {}
    ColdBox& operator=(const ColdBox& other) {
        if (this != &other) value_ = other.value_ ? std::make_unique<T>(*other.value_) : nullptr;
        return *this;
    }
    ColdBox(ColdBox&&) noexcept = default;
    ColdBox& operator=(ColdBox&&) noexcept = default;
    ~ColdBox() = default;

    bool has_value() const noexcept { return value_ != nullptr; }
    T& emplace() {
        value_ = std::make_unique<T>();
        return *value_;
    }
    T* operator->() noexcept { return value_.get(); }
    const T* operator->() const noexcept { return value_.get(); }
    T& operator*() noexcept { return *value_; }
    const T& operator*() const noexcept { return *value_; }

private:
    std::unique_ptr<T> value_;
};

}  // namespace kds
