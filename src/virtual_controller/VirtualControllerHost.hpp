#pragma once

#include "libera/core/LaserPoint.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace libera_link::virtual_controller {

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

struct VirtualControllerHostContext {
    std::vector<Target> targets;
};

struct VirtualControllerEndpoint {
    std::string targetId;
    std::string label;
    std::string value;
};

struct VirtualControllerHostConfig {
    std::uint32_t sliceDurationUs = 0;
    std::unordered_map<std::string, std::string> options;
};

class VirtualControllerHost {
public:
    virtual ~VirtualControllerHost() = default;

    virtual std::string_view name() const = 0;
    virtual std::string_view displayName() const = 0;
    virtual bool start(const VirtualControllerHostContext& context, std::string& error) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual std::vector<VirtualControllerEndpoint> endpoints() const = 0;
};

} // namespace libera_link::virtual_controller
