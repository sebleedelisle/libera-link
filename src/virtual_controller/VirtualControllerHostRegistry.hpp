#pragma once

#include "virtual_controller/VirtualControllerHost.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libera_link::virtual_controller {

struct VirtualControllerHostInfo {
    std::string id;
    std::string displayName;
    std::string description;
    std::vector<VirtualControllerHostOption> options;
    bool defaultSelection = false;
};

using VirtualControllerHostFactory = std::function<std::unique_ptr<VirtualControllerHost>(const VirtualControllerHostConfig&)>;

struct VirtualControllerHostRegistration {
    VirtualControllerHostInfo info;
    VirtualControllerHostFactory factory;
};

class VirtualControllerHostRegistrar {
public:
    explicit VirtualControllerHostRegistrar(VirtualControllerHostRegistration registration);
};

bool registerVirtualControllerHost(VirtualControllerHostRegistration registration, std::string* error = nullptr);
std::vector<VirtualControllerHostInfo> availableVirtualControllerHosts();
std::optional<VirtualControllerHostInfo> findVirtualControllerHost(std::string_view id);
std::optional<VirtualControllerHostInfo> defaultVirtualControllerHost();
std::unique_ptr<VirtualControllerHost> createVirtualControllerHost(std::string_view id,
                                         const VirtualControllerHostConfig& config,
                                         std::string& error);

} // namespace libera_link::virtual_controller
