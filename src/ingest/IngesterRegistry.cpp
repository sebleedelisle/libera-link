#include "ingest/IngesterRegistry.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace libera_link::ingest {
namespace {

struct RegistryState {
    std::mutex mutex;
    std::vector<RegistrationInfo> orderedInfos;
    std::unordered_map<std::string, Registration> registrationsById;
};

RegistryState& registryState() {
    static RegistryState state;
    return state;
}

} // namespace

IngesterRegistrar::IngesterRegistrar(Registration registration) {
    registerIngester(std::move(registration));
}

bool registerIngester(Registration registration, std::string* error) {
    if (registration.info.id.empty()) {
        if (error) {
            *error = "Ingester registration requires a non-empty id.";
        }
        return false;
    }
    if (registration.info.displayName.empty()) {
        registration.info.displayName = registration.info.id;
    }
    if (!registration.factory) {
        if (error) {
            *error = "Ingester registration requires a factory.";
        }
        return false;
    }

    auto& state = registryState();
    std::lock_guard<std::mutex> lock(state.mutex);

    if (state.registrationsById.find(registration.info.id) != state.registrationsById.end()) {
        if (error) {
            *error = "Ingester \"" + registration.info.id + "\" is already registered.";
        }
        return false;
    }

    state.orderedInfos.push_back(registration.info);
    state.registrationsById.emplace(registration.info.id, std::move(registration));
    return true;
}

std::vector<RegistrationInfo> availableIngesters() {
    auto& state = registryState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.orderedInfos;
}

std::optional<RegistrationInfo> findIngester(std::string_view id) {
    auto& state = registryState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.registrationsById.find(std::string(id));
    if (it == state.registrationsById.end()) {
        return std::nullopt;
    }
    return it->second.info;
}

std::optional<RegistrationInfo> defaultIngester() {
    auto infos = availableIngesters();
    const auto it = std::find_if(
        infos.begin(), infos.end(), [](const RegistrationInfo& info) {
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

std::unique_ptr<Ingester> createIngester(std::string_view id,
                                         const FactoryConfig& config,
                                         std::string& error) {
    auto& state = registryState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.registrationsById.find(std::string(id));
    if (it == state.registrationsById.end()) {
        error = "Unknown ingester \"" + std::string(id) + "\".";
        return nullptr;
    }

    auto ingester = it->second.factory(config);
    if (!ingester) {
        error = "Ingester factory for \"" + std::string(id) + "\" returned null.";
    }
    return ingester;
}

} // namespace libera_link::ingest
