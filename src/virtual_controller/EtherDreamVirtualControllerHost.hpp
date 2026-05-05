#pragma once

#include "virtual_controller/VirtualControllerHost.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace libera_link::virtual_controller {

void ensureBuiltInEtherDreamVirtualControllerHostLinked();

class EtherDreamVirtualControllerHost final : public VirtualControllerHost {
public:
    explicit EtherDreamVirtualControllerHost(const VirtualControllerHostConfig& config);
    ~EtherDreamVirtualControllerHost() override;

    EtherDreamVirtualControllerHost(const EtherDreamVirtualControllerHost&) = delete;
    EtherDreamVirtualControllerHost& operator=(const EtherDreamVirtualControllerHost&) = delete;

    std::string_view name() const override;
    std::string_view displayName() const override;
    bool start(const VirtualControllerHostContext& context, std::string& error) override;
    void stop() override;
    bool running() const override;
    std::vector<VirtualControllerEndpoint> endpoints() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VirtualControllerHostConfig config_;
};

} // namespace libera_link::virtual_controller
