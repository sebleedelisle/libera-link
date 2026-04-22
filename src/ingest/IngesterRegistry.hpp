#pragma once

#include "ingest/Ingester.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libera_link::ingest {

struct RegistrationInfo {
    std::string id;
    std::string displayName;
    std::string description;
    bool defaultSelection = false;
};

using IngesterFactory = std::function<std::unique_ptr<Ingester>(const FactoryConfig&)>;

struct Registration {
    RegistrationInfo info;
    IngesterFactory factory;
};

class IngesterRegistrar {
public:
    explicit IngesterRegistrar(Registration registration);
};

bool registerIngester(Registration registration, std::string* error = nullptr);
std::vector<RegistrationInfo> availableIngesters();
std::optional<RegistrationInfo> findIngester(std::string_view id);
std::optional<RegistrationInfo> defaultIngester();
std::unique_ptr<Ingester> createIngester(std::string_view id,
                                         const FactoryConfig& config,
                                         std::string& error);

} // namespace libera_link::ingest
