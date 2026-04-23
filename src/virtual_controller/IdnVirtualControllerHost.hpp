#pragma once

#include "virtual_controller/VirtualControllerHost.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace libera_link::virtual_controller {

void ensureBuiltInIdnVirtualControllerHostLinked();

class IdnVirtualControllerHost final : public VirtualControllerHost {
public:
    explicit IdnVirtualControllerHost(std::uint32_t sliceDurationUs);
    ~IdnVirtualControllerHost() override;

    IdnVirtualControllerHost(const IdnVirtualControllerHost&) = delete;
    IdnVirtualControllerHost& operator=(const IdnVirtualControllerHost&) = delete;

    std::string_view name() const override;
    std::string_view displayName() const override;
    bool start(const VirtualControllerHostContext& context, std::string& error) override;
    void stop() override;
    bool running() const override;
    std::vector<VirtualControllerEndpoint> endpoints() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<VirtualControllerEndpoint> endpoints_;
    std::uint32_t sliceDurationUs_ = 0;
};

} // namespace libera_link::virtual_controller
