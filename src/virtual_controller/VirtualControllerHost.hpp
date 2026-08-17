#pragma once

#include "libera/core/LaserPoint.hpp"

#include <chrono>
#include <cstddef>
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
    std::optional<std::uint32_t> effectivePointRate;
    std::optional<std::chrono::steady_clock::time_point> targetBeginTime;
    bool clearTransportPrefetch = false;
};

struct TargetStatus {
    std::size_t queuedPoints = 0;
    std::size_t maxQueuedPoints = 0;
    std::size_t controllerPrefetchedPoints = 0;
    std::size_t controllerTransportBufferedPoints = 0;
    std::size_t controllerBufferedPoints = 0;
    std::size_t totalBufferedPoints = 0;
    std::size_t targetBufferedPoints = 0;
    std::uint32_t outputPointRate = 0;
    std::uint32_t observedInputPointRate = 0;
    std::uint32_t latencyMs = 0;
    std::uint64_t receivedPoints = 0;
    std::uint64_t droppedPoints = 0;
    std::uint64_t underrunEvents = 0;
    std::uint64_t underrunPoints = 0;
    bool buffering = false;
};

struct SubmissionResult {
    bool accepted = false;
    std::size_t submittedPoints = 0;
    std::size_t acceptedPoints = 0;
    std::size_t droppedPoints = 0;
    TargetStatus status;
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
    virtual SubmissionResult submitContinuous(SliceSubmission submission) = 0;
    virtual SubmissionResult replaceFrame(FrameSubmission submission) = 0;
    virtual SubmissionResult submitFrame(FrameSubmission submission) = 0;
    virtual void setScannerSync(std::int64_t offsetNs, bool enabled) = 0;
    virtual TargetStatus status() const = 0;
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
    std::string kind;
    std::string protocol;
    std::string transport;
    std::string address;
    std::uint16_t port = 0;
    std::uint32_t channels = 0;
    std::unordered_map<std::string, std::string> attributes;
};

enum class VirtualControllerHostOptionType {
    String,
    Integer,
    Boolean,
    Decimal,
    Choice
};

struct VirtualControllerHostOption {
    std::string key;
    std::string displayName;
    std::string description;
    VirtualControllerHostOptionType type = VirtualControllerHostOptionType::String;
    std::string defaultValue;
    std::vector<std::string> choices;
    bool required = false;
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
