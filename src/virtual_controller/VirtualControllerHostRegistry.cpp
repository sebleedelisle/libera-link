#include "virtual_controller/VirtualControllerHostRegistry.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace libera_link::virtual_controller {
namespace {

struct RegistryState {
    std::mutex mutex;
    std::vector<VirtualControllerHostInfo> orderedInfos;
    std::unordered_map<std::string, VirtualControllerHostRegistration> registrationsById;
};

RegistryState& registryState() {
    static RegistryState state;
    return state;
}

} // namespace

VirtualControllerHostRegistrar::VirtualControllerHostRegistrar(VirtualControllerHostRegistration registration) {
    registerVirtualControllerHost(std::move(registration));
}

bool registerVirtualControllerHost(VirtualControllerHostRegistration registration, std::string* error) {
    if (registration.info.id.empty()) {
        if (error) {
            *error = "Virtual controller host registration requires a non-empty id.";
        }
        return false;
    }
    if (registration.info.displayName.empty()) {
        registration.info.displayName = registration.info.id;
    }
    if (!registration.factory) {
        if (error) {
            *error = "Virtual controller host registration requires a factory.";
        }
        return false;
    }

    auto& state = registryState();
    std::lock_guard<std::mutex> lock(state.mutex);

    if (state.registrationsById.find(registration.info.id) != state.registrationsById.end()) {
        if (error) {
            *error = "Virtual controller host \"" + registration.info.id + "\" is already registered.";
        }
        return false;
    }

    state.orderedInfos.push_back(registration.info);
    state.registrationsById.emplace(registration.info.id, std::move(registration));
    return true;
}

std::vector<VirtualControllerHostInfo> availableVirtualControllerHosts() {
    auto& state = registryState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.orderedInfos;
}

std::optional<VirtualControllerHostInfo> findVirtualControllerHost(std::string_view id) {
    auto& state = registryState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.registrationsById.find(std::string(id));
    if (it == state.registrationsById.end()) {
        return std::nullopt;
    }
    return it->second.info;
}

std::optional<VirtualControllerHostInfo> defaultVirtualControllerHost() {
    auto infos = availableVirtualControllerHosts();
    const auto it = std::find_if(
        infos.begin(), infos.end(), [](const VirtualControllerHostInfo& info) {
            return info.defaultSelection;
        });
    if (it != infos.end()) {
        return *it;
    }
    if (!infos.empty()) {
        return infos.front();
    }
    return std::nullopt;
}

std::unique_ptr<VirtualControllerHost> createVirtualControllerHost(std::string_view id,
                                         const VirtualControllerHostConfig& config,
                                         std::string& error) {
    auto& state = registryState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.registrationsById.find(std::string(id));
    if (it == state.registrationsById.end()) {
        error = "Unknown virtual controller host \"" + std::string(id) + "\".";
        return nullptr;
    }

    auto virtualControllerHost = it->second.factory(config);
    if (!virtualControllerHost) {
        error = "Virtual controller host factory for \"" + std::string(id) + "\" returned null.";
    }
    return virtualControllerHost;
}

} // namespace libera_link::virtual_controller
