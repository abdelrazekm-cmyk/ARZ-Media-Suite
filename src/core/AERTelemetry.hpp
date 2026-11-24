#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aer {

struct StreamGpuTelemetry {
    uint64_t sessionId = 0;
    std::string filePathUtf8;
    std::string videoCodec;
    int width = 0;
    int height = 0;
    int bitDepth = 8;
    bool usingHardware = false;
    std::string selectedBackend;
    std::string state;        // "gpu_active", "cpu_active", "fell_back_mid_stream", "quieted"
    std::string reasonDetail; // e.g., "ok", "gpu_session_limit_reached", "hw_profile_unsupported"
};

class AERTelemetry {
public:
    static AERTelemetry& instance();

    void updateSession(const StreamGpuTelemetry& entry, uint32_t activeGpuStreams);
    void removeSession(uint64_t sessionId, uint32_t activeGpuStreams);
    std::string statusFilePath() const;

private:
    AERTelemetry() = default;
    void flushJsonLocked(uint32_t activeGpuStreams);

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, StreamGpuTelemetry> sessions_;
};

} // namespace aer
