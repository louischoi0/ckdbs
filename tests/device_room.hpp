#pragma once

// Leaves a device exactly one page of room past every id its store has
// claimed, so a cell can make a mutation's *second* creation the one the
// device has to grow for (`btree_test.cpp`, `index_tree_test.cpp`). Every
// create until one needs growth - that refusal gives its claim back - then
// one page more. Wants a device whose extent is one page.

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

namespace kds::testing_device {

inline void LeaveRoomForOnePage(storage::MemoryPageDevice& device,
                                storage::DevicePageStore& store) {
    ASSERT_EQ(device.extent_pages(), 1u);
    for (int i = 0;; ++i) {
        ASSERT_LT(i, 64) << "no create needed the device to grow";
        device.FailNextGrow(Status::IoError("probe"));
        auto made = store.CreateNew();
        device.ClearInjections();
        if (!made.ok()) break;
    }
    ASSERT_TRUE(device.EnsureCapacity(device.page_capacity() + 1).ok());
}

}  // namespace kds::testing_device
