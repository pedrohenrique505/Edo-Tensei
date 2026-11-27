#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class PS2Runtime;

namespace edo_debug
{

enum class Category : uint8_t
{
    Camera = 0,
    Math,
    Matrix,
    Collision,
    General,
};

enum class Severity : uint8_t
{
    Info = 0,
    Warning,
    Error,
    Critical,
};

const char *categoryName(Category cat);
const char *severityName(Severity sev);

struct CameraSnapshot
{
    uint64_t battleFrame = 0;
    uint32_t camObjAddr = 0;
    uint32_t lookAtCallerRa = 0;
    float pos[4]{};
    float tgt[4]{};
    float extra90[4]{};
    float shake[4]{};
    float distSq = 0.0f;
    bool valid = true;
    std::string reason;
};

struct FighterSnapshot
{
    uint32_t addr = 0;
    float pos[4]{};
    float prevPos[4]{};
    float deltaDist = 0.0f;
    float scaleE4 = 0.0f;
    float scaleE8 = 0.0f;
    float scale2F0 = 0.0f;
    int16_t altPosFlag = 0;
    bool present = false;
    bool valid = true;
    std::string reason;
};

struct BattleActorsSnapshot
{
    uint64_t battleFrame = 0;
    uint32_t camCtxAddr = 0;
    FighterSnapshot fighters[2];
    float distP1P2 = 0.0f;
    bool hasViewProj = false;
    float viewProj[16]{};
};

struct AnomalyEvent
{
    uint64_t seq = 0;
    uint64_t battleFrame = 0;
    Category category = Category::General;
    Severity severity = Severity::Error;
    uint32_t funcAddr = 0;
    uint32_t callerRa = 0;
    uint32_t occurrences = 1;
    std::string funcName;
    std::string detail;
};

struct TraceEvent
{
    uint64_t seq = 0;
    uint64_t battleFrame = 0;
    Category category = Category::General;
    uint32_t funcAddr = 0;
    uint32_t callerRa = 0;
    std::string summary;
};

struct SnapshotForUi
{
    bool enabled = false;
    bool btlActive = false;
    bool mathCheckEnabled = false;
    bool breakOnAnomaly = false;
    bool paused = false;
    uint64_t battleFrame = 0;
    uint64_t totalAnomalies = 0;
    uint64_t mathChecks = 0;
    uint64_t mathMismatches = 0;
    CameraSnapshot currentCam;
    CameraSnapshot lastGoodCam;
    bool hasLastGoodCam = false;
    BattleActorsSnapshot actors;
    bool overlay3dEnabled = false;
    std::optional<AnomalyEvent> firstAnomaly;
    std::vector<AnomalyEvent> anomalies;
    std::vector<TraceEvent> traces;
};

inline uint32_t floatBits(float v)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline uint64_t doubleBits(double v)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline bool isBadFloat(float v, float absLimit = 1e10f)
{
    const uint32_t bits = floatBits(v);
    if ((bits & 0x7FFFFFFFu) == 0x7FFFFFFFu)
        return true; // PS2 ±FLT_MAX sentinel (0x7FFFFFFF / 0xFFFFFFFF)
    return std::isnan(v) || std::isinf(v) || std::fabs(v) > absLimit;
}

inline bool isBadDouble(double v, double absLimit = 1e15)
{
    return std::isnan(v) || std::isinf(v) || std::fabs(v) > absLimit;
}

inline bool isBadVec3(const float v[3], float absLimit = 1e10f)
{
    return isBadFloat(v[0], absLimit) || isBadFloat(v[1], absLimit) || isBadFloat(v[2], absLimit);
}

inline bool isBadMat4(const float m[16], float absLimit = 1e10f)
{
    for (int i = 0; i < 16; ++i)
    {
        if (isBadFloat(m[i], absLimit))
            return true;
    }
    return false;
}

inline bool isZeroMat4(const float m[16])
{
    for (int i = 0; i < 16; ++i)
    {
        if (std::fabs(m[i]) > 1e-12f)
            return false;
    }
    return true;
}

class BattleDebugger
{
public:
    static BattleDebugger &instance();

    void initFromEnv();
    void installCoreHooks(PS2Runtime &runtime);
    void onBtlOverlayActivated(PS2Runtime &runtime);
    void onBtlOverlayDeactivated(PS2Runtime &runtime);

    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }
    void setEnabled(bool v) { m_enabled.store(v, std::memory_order_relaxed); }

    bool isBtlActive() const { return m_btlActive.load(std::memory_order_relaxed); }
    void setBtlActive(bool v) { m_btlActive.store(v, std::memory_order_relaxed); }

    bool isMathCheckEnabled() const { return m_mathCheckEnabled.load(std::memory_order_relaxed); }
    void setMathCheckEnabled(bool v) { m_mathCheckEnabled.store(v, std::memory_order_relaxed); }

    bool isBreakOnAnomaly() const { return m_breakOnAnomaly.load(std::memory_order_relaxed); }
    void setBreakOnAnomaly(bool v) { m_breakOnAnomaly.store(v, std::memory_order_relaxed); }

    bool isTraceCamera() const { return m_traceCamera.load(std::memory_order_relaxed); }
    void setTraceCamera(bool v) { m_traceCamera.store(v, std::memory_order_relaxed); }

    bool isTraceFighters() const { return m_traceFighters.load(std::memory_order_relaxed); }
    void setTraceFighters(bool v) { m_traceFighters.store(v, std::memory_order_relaxed); }

    bool isOverlay3dEnabled() const { return m_overlay3dEnabled.load(std::memory_order_relaxed); }
    void setOverlay3dEnabled(bool v) { m_overlay3dEnabled.store(v, std::memory_order_relaxed); }

    bool isPaused() const { return m_paused.load(std::memory_order_relaxed); }
    void setPaused(bool v) { m_paused.store(v, std::memory_order_relaxed); }
    void requestStepFrame() { m_stepRequests.fetch_add(1, std::memory_order_relaxed); }

    uint64_t battleFrame() const { return m_battleFrame.load(std::memory_order_relaxed); }

    void onBattleFrame(PS2Runtime *runtime);
    void updateCameraSnapshot(const CameraSnapshot &snap);
    void updateFightersSnapshot(uint32_t camCtxAddr, const FighterSnapshot fighters[2]);
    void updateViewProjMatrix(const float vp[16], bool valid);
    void countMathCheck(bool mismatch);

    void recordTrace(Category cat, uint32_t funcAddr, uint32_t callerRa, const char *fmt, ...);
    void recordAnomaly(Category cat, Severity sev, uint32_t funcAddr, uint32_t callerRa,
                       const char *funcName, const char *fmt, ...);

    void clear();
    SnapshotForUi captureUiSnapshot() const;
    bool dumpReport(const std::string &path) const;
    const std::string &defaultDumpPath() const { return m_dumpPath; }

    void drawUi(PS2Runtime &runtime);
    bool isUiVisible() const { return m_uiVisible; }
    void toggleUiVisible() { m_uiVisible = !m_uiVisible; }

private:
    BattleDebugger() = default;
    ~BattleDebugger();

    void writeJsonlAnomalyLocked(const AnomalyEvent &ev, bool isFirst);
    void writeJsonlTraceLocked(const TraceEvent &ev);

    static constexpr size_t kMaxAnomalies = 128;
    static constexpr size_t kMaxTraces = 256;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_btlActive{false};
    std::atomic<bool> m_mathCheckEnabled{true};
    std::atomic<bool> m_breakOnAnomaly{false};
    std::atomic<bool> m_traceCamera{false};
    std::atomic<bool> m_traceFighters{false};
    std::atomic<bool> m_overlay3dEnabled{false};
    std::atomic<bool> m_logStderr{true};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_autoStopTriggered{false};
    std::atomic<uint32_t> m_stepRequests{0};
    std::atomic<uint64_t> m_battleFrame{0};
    std::atomic<uint64_t> m_totalAnomalies{0};
    std::atomic<uint64_t> m_mathChecks{0};
    std::atomic<uint64_t> m_mathMismatches{0};

    uint64_t m_stopAfterAnomalyFrames = 0;
    uint64_t m_maxBattleFrames = 0;
    std::string m_dumpPath;
    std::string m_jsonlPath;
    bool m_autoDumpOnExit = false;
    bool m_uiVisible = false;

    mutable std::mutex m_mutex;
    uint64_t m_seq = 0;
    CameraSnapshot m_currentCam;
    CameraSnapshot m_lastGoodCam;
    bool m_hasLastGoodCam = false;
    BattleActorsSnapshot m_actors;
    std::optional<AnomalyEvent> m_firstAnomaly;
    std::deque<AnomalyEvent> m_anomalies;
    std::unordered_map<uint64_t, size_t> m_dedupIndex;
    std::deque<TraceEvent> m_traces;
    std::FILE *m_jsonlFile = nullptr;
};

} // namespace edo_debug
