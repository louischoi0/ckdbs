#include "kds/server/expeditor.hpp"

#include <utility>

#include "kds/storage/file_page_device.hpp"

namespace kds::server {

Expeditor::Expeditor(Config config, std::unique_ptr<storage::PageDevice> device,
                     std::unique_ptr<storage::DevicePageStore> store) noexcept
    : config_(std::move(config)), device_(std::move(device)), store_(std::move(store)) {}

StatusOr<std::unique_ptr<Expeditor>> Expeditor::Open(Config config,
                                                     std::uint64_t now_unix_seconds) {
    auto device = storage::FilePageDevice::Open(config.data_file);
    if (!device.ok()) return device.status();

    auto store = storage::DevicePageStore::Open(*device.value(), kFirstUserPageId);
    if (!store.ok()) return store.status();

    // Built here rather than in the initializer list because the members
    // below take references into it, which only become stable once the
    // Expeditor itself is on the heap and pinned.
    auto expeditor = std::unique_ptr<Expeditor>(new Expeditor(
        std::move(config), std::move(device.value()), std::move(store.value())));

    auto database = bootstrap::BootstrapDatabase(*expeditor->store_, now_unix_seconds);
    if (!database.ok()) return database.status();
    expeditor->database_.emplace(std::move(database.value()));

    expeditor->dispatcher_.emplace(expeditor->database_->superblock,
                                   expeditor->database_->catalog, *expeditor->store_);

    if (Status s = expeditor->Sync(); !s.ok()) return s;
    return expeditor;
}

Status Expeditor::Serve() {
    auto listener = TcpServer::Listen(config_.port);
    if (!listener.ok()) return listener.status();

    listener.value().Serve(*dispatcher_);
    return Sync();
}

}  // namespace kds::server
