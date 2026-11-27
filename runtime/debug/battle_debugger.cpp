#include "battle_debugger.h"

#include "ps2_runtime.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace edo_debug
{

const char *categoryName(Category cat)
{
    switch (cat)
    {
    case Category::Camera:
        return "CAMERA";
    case Category::Math:
        return "MATH";
    case Category::Matrix:
        return "MATRIX";
    case Category::Collision:
        return "COLLISION";
    case Category::General:
        return "GENERAL";
    }
    return "?";
}

const char *severityName(Severity sev)
{
    switch (sev)
    {
    case Severity::Info:
        return "INFO";
    case Severity::Warning:
        return "WARN";
    case Severity::Error:
        return "ERROR";
    case Severity::Critical:
        return "CRIT";
    }
    return "?";
}

namespace
{
bool envFlagEnabled(const char *name, bool defaultVal = false)
{
    const char *v = std::getenv(name);
    if (!v || !*v)
        return defaultVal;
    return v[0] != '0';
}

uint64_t makeDedupKey(Category cat, uint32_t funcAddr, uint32_t callerRa)
{
    return (static_cast<uint64_t>(cat) << 56) ^
           (static_cast<uint64_t>(funcAddr) << 24) ^
           static_cast<uint64_t>(callerRa);
}

std::string formatVarArgs(const char *fmt, va_list args)
{
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    return std::string(buf);
}
} // namespace

BattleDebugger &BattleDebugger::instance()
{
    static BattleDebugger inst;
    return inst;
}

BattleDebugger::~BattleDebugger()
{
    if (m_autoDumpOnExit && !m_dumpPath.empty())
    {
        dumpReport(m_dumpPath);
    }
}

void BattleDebugger::initFromEnv()
{
    const bool enabled = envFlagEnabled("EDO_BATTLE_DEBUG", false);
    m_enabled.store(enabled, std::memory_order_relaxed);
    m_breakOnAnomaly.store(envFlagEnabled("EDO_BATTLE_BREAK", false), std::memory_order_relaxed);
    m_mathCheckEnabled.store(envFlagEnabled("EDO_BATTLE_MATH_CHECK", true), std::memory_order_relaxed);
    m_logStderr.store(envFlagEnabled("EDO_BATTLE_STDERR", true), std::memory_order_relaxed);

    if (const char *dumpEnv = std::getenv("EDO_BATTLE_DUMP"); dumpEnv && *dumpEnv)
    {
        m_dumpPath = dumpEnv;
        m_autoDumpOnExit = true;
    }
    else
    {
        m_dumpPath = ".cache/battle-debug-report.txt";
        m_autoDumpOnExit = false;
    }

    if (enabled)
    {
        std::fprintf(stderr,
                     "[battle-dbg] ativo (mathCheck=%d breakOnAnomaly=%d dump=%s)\n",
                     static_cast<int>(m_mathCheckEnabled.load()),
                     static_cast<int>(m_breakOnAnomaly.load()),
                     m_dumpPath.c_str());
        std::fflush(stderr);
    }
}

void BattleDebugger::onBattleFrame(PS2Runtime *runtime)
{
    m_battleFrame.fetch_add(1, std::memory_order_relaxed);

    if (!isEnabled())
        return;

    while (m_paused.load(std::memory_order_relaxed))
    {
        if (runtime && runtime->isStopRequested())
            break;

        uint32_t steps = m_stepRequests.load(std::memory_order_relaxed);
        if (steps > 0)
        {
            if (m_stepRequests.compare_exchange_weak(steps, steps - 1, std::memory_order_relaxed))
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

void BattleDebugger::updateCameraSnapshot(const CameraSnapshot &snap)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentCam = snap;
    m_currentCam.battleFrame = m_battleFrame.load(std::memory_order_relaxed);
    if (snap.valid)
    {
        m_lastGoodCam = m_currentCam;
        m_hasLastGoodCam = true;
    }
}

void BattleDebugger::countMathCheck(bool mismatch)
{
    m_mathChecks.fetch_add(1, std::memory_order_relaxed);
    if (mismatch)
        m_mathMismatches.fetch_add(1, std::memory_order_relaxed);
}

void BattleDebugger::recordTrace(Category cat, uint32_t funcAddr, uint32_t callerRa, const char *fmt, ...)
{
    if (!isEnabled())
        return;

    va_list args;
    va_start(args, fmt);
    std::string msg = formatVarArgs(fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(m_mutex);
    TraceEvent ev;
    ev.seq = ++m_seq;
    ev.battleFrame = m_battleFrame.load(std::memory_order_relaxed);
    ev.category = cat;
    ev.funcAddr = funcAddr;
    ev.callerRa = callerRa;
    ev.summary = std::move(msg);

    if (m_traces.size() >= kMaxTraces)
        m_traces.pop_front();
    m_traces.push_back(std::move(ev));
}

void BattleDebugger::recordAnomaly(Category cat, Severity sev, uint32_t funcAddr, uint32_t callerRa,
                                   const char *funcName, const char *fmt, ...)
{
    if (!isEnabled())
        return;

    va_list args;
    va_start(args, fmt);
    std::string detail = formatVarArgs(fmt, args);
    va_end(args);

    const uint64_t frame = m_battleFrame.load(std::memory_order_relaxed);
    m_totalAnomalies.fetch_add(1, std::memory_order_relaxed);

    bool shouldPrint = false;
    bool isFirst = false;
    uint32_t occurrences = 1;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t key = makeDedupKey(cat, funcAddr, callerRa);
        auto it = m_dedupIndex.find(key);
        if (it != m_dedupIndex.end() && it->second < m_anomalies.size())
        {
            AnomalyEvent &existing = m_anomalies[it->second];
            existing.occurrences++;
            occurrences = existing.occurrences;
            if (occurrences <= 3 || (occurrences % 500) == 0)
            {
                shouldPrint = true;
            }
        }
        else
        {
            AnomalyEvent ev;
            ev.seq = ++m_seq;
            ev.battleFrame = frame;
            ev.category = cat;
            ev.severity = sev;
            ev.funcAddr = funcAddr;
            ev.callerRa = callerRa;
            ev.occurrences = 1;
            ev.funcName = funcName ? funcName : "";
            ev.detail = detail;

            if (!m_firstAnomaly.has_value())
            {
                m_firstAnomaly = ev;
                isFirst = true;
            }

            if (m_anomalies.size() < kMaxAnomalies)
            {
                m_dedupIndex[key] = m_anomalies.size();
                m_anomalies.push_back(std::move(ev));
            }
            shouldPrint = true;
        }

        // Also mirror first few occurrences into the chronological trace buffer.
        if (occurrences <= 2)
        {
            TraceEvent tr;
            tr.seq = ++m_seq;
            tr.battleFrame = frame;
            tr.category = cat;
            tr.funcAddr = funcAddr;
            tr.callerRa = callerRa;
            tr.summary = std::string("[") + severityName(sev) + "] " + (funcName ? funcName : "") + ": " + detail;
            if (m_traces.size() >= kMaxTraces)
                m_traces.pop_front();
            m_traces.push_back(std::move(tr));
        }
    }

    if (shouldPrint && m_logStderr.load(std::memory_order_relaxed))
    {
        std::fprintf(stderr,
                     "[battle-dbg:%s:%s] f=%llu fn=0x%08X(%s) ra=0x%08X #%u%s %s\n",
                     categoryName(cat),
                     severityName(sev),
                     static_cast<unsigned long long>(frame),
                     funcAddr,
                     funcName ? funcName : "?",
                     callerRa,
                     occurrences,
                     isFirst ? " [FIRST]" : "",
                     detail.c_str());
        std::fflush(stderr);
    }

    if (isFirst && m_autoDumpOnExit && !m_dumpPath.empty())
    {
        dumpReport(m_dumpPath);
    }

    if (m_breakOnAnomaly.load(std::memory_order_relaxed) && sev >= Severity::Error)
    {
        m_paused.store(true, std::memory_order_relaxed);
    }
}

void BattleDebugger::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_firstAnomaly.reset();
    m_anomalies.clear();
    m_dedupIndex.clear();
    m_traces.clear();
    m_totalAnomalies.store(0, std::memory_order_relaxed);
    m_mathChecks.store(0, std::memory_order_relaxed);
    m_mathMismatches.store(0, std::memory_order_relaxed);
}

SnapshotForUi BattleDebugger::captureUiSnapshot() const
{
    SnapshotForUi out;
    out.enabled = m_enabled.load(std::memory_order_relaxed);
    out.btlActive = m_btlActive.load(std::memory_order_relaxed);
    out.mathCheckEnabled = m_mathCheckEnabled.load(std::memory_order_relaxed);
    out.breakOnAnomaly = m_breakOnAnomaly.load(std::memory_order_relaxed);
    out.paused = m_paused.load(std::memory_order_relaxed);
    out.battleFrame = m_battleFrame.load(std::memory_order_relaxed);
    out.totalAnomalies = m_totalAnomalies.load(std::memory_order_relaxed);
    out.mathChecks = m_mathChecks.load(std::memory_order_relaxed);
    out.mathMismatches = m_mathMismatches.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(m_mutex);
    out.currentCam = m_currentCam;
    out.lastGoodCam = m_lastGoodCam;
    out.hasLastGoodCam = m_hasLastGoodCam;
    out.firstAnomaly = m_firstAnomaly;
    out.anomalies.assign(m_anomalies.begin(), m_anomalies.end());
    out.traces.assign(m_traces.begin(), m_traces.end());
    return out;
}

bool BattleDebugger::dumpReport(const std::string &path) const
{
    if (path.empty())
        return false;

    const SnapshotForUi snap = captureUiSnapshot();
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path(), ec);

    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f)
        return false;

    f << "=== Edo-Tensei Battle Debugger Report ===\n";
    f << "battleFrame=" << snap.battleFrame
      << " btlActive=" << (snap.btlActive ? 1 : 0)
      << " totalAnomalies=" << snap.totalAnomalies
      << " mathChecks=" << snap.mathChecks
      << " mathMismatches=" << snap.mathMismatches << "\n\n";

    if (snap.firstAnomaly.has_value())
    {
        const auto &a = *snap.firstAnomaly;
        char hdr[256];
        std::snprintf(hdr, sizeof(hdr),
                      "FIRST ANOMALY: [%s/%s] f=%llu fn=0x%08X(%s) ra=0x%08X\n",
                      categoryName(a.category), severityName(a.severity),
                      static_cast<unsigned long long>(a.battleFrame),
                      a.funcAddr, a.funcName.c_str(), a.callerRa);
        f << hdr << "  " << a.detail << "\n\n";
    }
    else
    {
        f << "FIRST ANOMALY: none\n\n";
    }

    auto writeCam = [&](const char *label, const CameraSnapshot &c) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "%s (f=%llu obj=0x%08X valid=%d%s%s):\n"
                      "  pos=(%.4g [0x%08X], %.4g [0x%08X], %.4g [0x%08X])\n"
                      "  tgt=(%.4g [0x%08X], %.4g [0x%08X], %.4g [0x%08X])\n"
                      "  shake=(%.4g, %.4g, %.4g, %.4g) distSq=%.4g\n",
                      label, static_cast<unsigned long long>(c.battleFrame), c.camObjAddr,
                      c.valid ? 1 : 0, c.reason.empty() ? "" : " reason=", c.reason.c_str(),
                      c.pos[0], floatBits(c.pos[0]), c.pos[1], floatBits(c.pos[1]), c.pos[2], floatBits(c.pos[2]),
                      c.tgt[0], floatBits(c.tgt[0]), c.tgt[1], floatBits(c.tgt[1]), c.tgt[2], floatBits(c.tgt[2]),
                      c.shake[0], c.shake[1], c.shake[2], c.shake[3], c.distSq);
        f << buf;
    };

    writeCam("CURRENT CAMERA", snap.currentCam);
    if (snap.hasLastGoodCam)
        writeCam("LAST GOOD CAMERA", snap.lastGoodCam);
    f << "\n";

    f << "--- ANOMALIES (" << snap.anomalies.size() << " unique) ---\n";
    for (const auto &a : snap.anomalies)
    {
        char line[512];
        std::snprintf(line, sizeof(line),
                      "[#%llu f=%llu %s/%s x%u] fn=0x%08X(%s) ra=0x%08X | %s\n",
                      static_cast<unsigned long long>(a.seq),
                      static_cast<unsigned long long>(a.battleFrame),
                      categoryName(a.category), severityName(a.severity),
                      a.occurrences, a.funcAddr, a.funcName.c_str(), a.callerRa,
                      a.detail.c_str());
        f << line;
    }

    f << "\n--- RECENT TRACE (" << snap.traces.size() << ") ---\n";
    for (const auto &t : snap.traces)
    {
        char line[512];
        std::snprintf(line, sizeof(line),
                      "[#%llu f=%llu %s] fn=0x%08X ra=0x%08X | %s\n",
                      static_cast<unsigned long long>(t.seq),
                      static_cast<unsigned long long>(t.battleFrame),
                      categoryName(t.category), t.funcAddr, t.callerRa,
                      t.summary.c_str());
        f << line;
    }

    return true;
}

} // namespace edo_debug
