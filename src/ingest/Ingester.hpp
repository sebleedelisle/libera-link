#pragma once

#include "libera/core/LaserPoint.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace libera_link::ingest {

struct SliceSubmission {
    std::vector<libera::core::LaserPoint> points;
    std::uint32_t durationUs = 0;
    std::optional<std::uint32_t> effectivePointRate;
    bool discontinuous = false;
};

struct FrameSubmission {
    std::vector<SliceSubmission> slices;
    bool clearTransportPrefetch = false;
};

struct TargetInfo {
    std::string id;
    std::string label;
    std::string type;
    std::uint32_t maxPointRate = 0;
};

class TargetSink {
public:
    virtual ~TargetSink() = default;

    virtual const TargetInfo& targetInfo() const = 0;
    virtual void submitContinuous(SliceSubmission submission) = 0;
    virtual void replaceFrame(FrameSubmission submission) = 0;
    virtual void reset() = 0;
};

struct Target {
    std::shared_ptr<TargetSink> sink;
};

struct StartContext {
    std::vector<Target> targets;
};

struct BindingInfo {
    std::string targetId;
    std::string label;
    std::string value;
};

struct FactoryConfig {
    std::uint32_t sliceDurationUs = 0;
    std::unordered_map<std::string, std::string> options;
};

class Ingester {
public:
    virtual ~Ingester() = default;

    virtual std::string_view name() const = 0;
    virtual std::string_view displayName() const = 0;
    virtual bool start(const StartContext& context, std::string& error) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual std::vector<BindingInfo> bindings() const = 0;
};

} // namespace libera_link::ingest
