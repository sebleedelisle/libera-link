#pragma once

#include "ingest/Ingester.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace libera_link::ingest {

void ensureBuiltInIdnIngesterLinked();

class IdnIngester final : public Ingester {
public:
    explicit IdnIngester(std::uint32_t sliceDurationUs);
    ~IdnIngester() override;

    IdnIngester(const IdnIngester&) = delete;
    IdnIngester& operator=(const IdnIngester&) = delete;

    std::string_view name() const override;
    std::string_view displayName() const override;
    bool start(const StartContext& context, std::string& error) override;
    void stop() override;
    bool running() const override;
    std::vector<BindingInfo> bindings() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<BindingInfo> bindings_;
    std::uint32_t sliceDurationUs_ = 0;
};

} // namespace libera_link::ingest
