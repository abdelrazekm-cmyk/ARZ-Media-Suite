#include "core/AERTelemetry.hpp"
#include "cache/FrameCache.hpp"
#include "core/AERConfig.hpp"
#include "media/GpuDeviceManager.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aer {

namespace {

std::string jsonEscape(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "?";
                else out << static_cast<char>(c);
                break;
        }
    }
    return out.str();
}

uint32_t currentPid() {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return 0;
#endif
}

} // namespace

AERTelemetry& AERTelemetry::instance() {
    static AERTelemetry s_inst;
    return s_inst;
}

std::string AERTelemetry::statusFilePath() const {
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData) return {};
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(localAppData) / "ARZ" / "Status";
    std::filesystem::create_directories(dir, ec);
    return (dir / ("gpu-status-" + std::to_string(currentPid()) + ".json")).string();
}

void AERTelemetry::updateSession(const StreamGpuTelemetry& entry, uint32_t activeGpuStreams) {
    if (!AERConfig::instance().enableJsonTelemetry) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[entry.sessionId] = entry;
    flushJsonLocked(activeGpuStreams);
}

void AERTelemetry::removeSession(uint64_t sessionId, uint32_t activeGpuStreams) {
    if (!AERConfig::instance().enableJsonTelemetry) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
    flushJsonLocked(activeGpuStreams);
}

void AERTelemetry::flushJsonLocked(uint32_t activeGpuStreams) {
    const std::string finalPath = statusFilePath();
    if (finalPath.empty()) return;
    const std::string tmpPath = finalPath + ".tmp";

    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();

    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;

    const auto& cfg = AERConfig::instance();
    const auto& gpuMgr = GpuDeviceManager::instance();
    const size_t aerVramBytes = LRUFrameCache::instance().totalVramBytes();
    const VramStatus vst = gpuMgr.queryVramStatus(aerVramBytes);
    const AdaptiveHardwareBudgets adaptive =
        cfg.computeAdaptiveBudgets(vst.budgetBytes, vst.currentUsageBytes, aerVramBytes);

    out << "{\n"
        << "  \"plugin\": \"ARZ\",\n"
        << "  \"version\": \"1.3.0\",\n"
        << "  \"governor_profile\": \"" << jsonEscape(adaptive.tierName) << "\",\n"
        << "  \"auto_l1_vram_cap_mb\": " << (adaptive.l1VramCacheBytes / (1024ULL * 1024ULL)) << ",\n"
        << "  \"auto_l2_ram_cap_mb\": " << (adaptive.l2RamCacheBytes / (1024ULL * 1024ULL)) << ",\n"
        << "  \"auto_lookahead_frames\": " << adaptive.lookaheadFrames << ",\n"
        << "  \"auto_aux_cpu_threads\": " << adaptive.maxAuxCpuThreads << ",\n"
        << "  \"process_id\": " << currentPid() << ",\n"
        << "  \"updated_at_ms\": " << nowMs << ",\n"
        << "  \"gpu_adapter\": \"" << jsonEscape(gpuMgr.adapterName()) << "\",\n"
        << "  \"active_gpu_streams\": " << activeGpuStreams << ",\n"
        << "  \"vram_budget_mb\": " << (vst.budgetBytes / (1024ULL * 1024ULL)) << ",\n"
        << "  \"vram_used_mb\": " << (vst.currentUsageBytes / (1024ULL * 1024ULL)) << ",\n"
        << "  \"vram_ceiling_pct\": " << cfg.maxPercentVramBudget << ",\n"
        << "  \"aer_vram_cache_mb\": " << (aerVramBytes / (1024ULL * 1024ULL)) << ",\n"
        << "  \"aer_vram_cache_frames\": " << LRUFrameCache::instance().totalVramEntries() << ",\n"
        << "  \"aer_ram_overflow_mb\": " << (LRUFrameCache::instance().totalRamBytes() / (1024ULL * 1024ULL)) << ",\n"
        << "  \"aer_ram_overflow_frames\": " << LRUFrameCache::instance().totalRamEntries() << ",\n"
        << "  \"sessions\": [\n";

    size_t idx = 0;
    for (const auto& [id, s] : sessions_) {
        out << "    {\n"
            << "      \"session_id\": " << s.sessionId << ",\n"
            << "      \"file\": \"" << jsonEscape(s.filePathUtf8) << "\",\n"
            << "      \"codec\": \"" << jsonEscape(s.videoCodec) << "\",\n"
            << "      \"resolution\": \"" << s.width << "x" << s.height << "\",\n"
            << "      \"bit_depth\": " << s.bitDepth << ",\n"
            << "      \"using_hardware\": " << (s.usingHardware ? "true" : "false") << ",\n"
            << "      \"selected_backend\": \"" << jsonEscape(s.selectedBackend) << "\",\n"
            << "      \"state\": \"" << jsonEscape(s.state) << "\",\n"
            << "      \"reason\": \"" << jsonEscape(s.reasonDetail) << "\"\n"
            << "    }" << (++idx < sessions_.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    out.close();

    std::error_code ec;
    std::filesystem::rename(tmpPath, finalPath, ec);
    if (ec) {
        std::filesystem::copy_file(tmpPath, finalPath, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmpPath, ec);
    }
}

} // namespace aer
