#include "battle_debugger.h"

#include "ps2_recompiled_functions.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef EDO_HAS_OVL_BTL
extern void ovl_btl_FUN_006e95f0_0x6e95f0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
extern void ovl_btl_FUN_006e99f0_0x6e99f0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
#endif

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

uint64_t envU64(const char *name, uint64_t defaultVal = 0)
{
    const char *v = std::getenv(name);
    if (!v || !*v)
        return defaultVal;
    return std::strtoull(v, nullptr, 10);
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

std::string jsonEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                out += esc;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

bool doubleDiverges(double guest, double host, double absTol = 1e-4, double relTol = 1e-3)
{
    if (std::isnan(guest) != std::isnan(host))
        return true;
    if (std::isinf(guest) != std::isinf(host))
        return true;
    if (std::isnan(guest) && std::isnan(host))
        return false;
    if (std::isinf(guest) && std::isinf(host))
        return std::signbit(guest) != std::signbit(host);
    const double diff = std::fabs(guest - host);
    const double scale = std::max(1.0, std::max(std::fabs(guest), std::fabs(host)));
    return diff > absTol && diff > relTol * scale;
}

bool isBtlAddress(uint32_t addr)
{
    return addr >= 0x006C6D00u && addr < 0x00A00000u;
}

// ---------------------------------------------------------------------------
// Core & Math Hooks (Main ELF)
// ---------------------------------------------------------------------------

void hookCameraLookAt_0019f990(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &dbg = BattleDebugger::instance();
    if (dbg.isEnabled() && dbg.isBtlActive() && ctx->pc == 0x0019F990u)
    {
        const uint32_t a0 = getRegU32(ctx, 4);
        const uint32_t a1 = getRegU32(ctx, 5);
        const uint32_t a2 = getRegU32(ctx, 6);
        const uint32_t ra = getRegU32(ctx, 31);
        float pos[4]{}, tgt[4]{};
        if (const uint8_t *p = getConstMemPtr(rdram, a1))
            std::memcpy(pos, p, 16);
        if (const uint8_t *t = getConstMemPtr(rdram, a2))
            std::memcpy(tgt, t, 16);
        const float dx = tgt[0] - pos[0];
        const float dy = tgt[1] - pos[1];
        const float dz = tgt[2] - pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;

        CameraSnapshot snap;
        snap.camObjAddr = a0;
        snap.lookAtCallerRa = ra;
        std::memcpy(snap.pos, pos, sizeof(pos));
        std::memcpy(snap.tgt, tgt, sizeof(tgt));
        snap.distSq = d2;

        const bool badPos = isBadVec3(pos);
        const bool badTgt = isBadVec3(tgt);
        const bool badDist = isBadFloat(d2) || d2 < 1e-6f;
        if (badPos || badTgt || badDist)
        {
            snap.valid = false;
            snap.reason = badPos ? "bad_pos" : (badTgt ? "bad_tgt" : "degenerate_dist");
            dbg.recordAnomaly(
                Category::Camera, Severity::Critical, 0x0019F990u, ra, "FUN_0019f990(CameraLookAt)",
                "a0=0x%08X pos=(%.4g[0x%08X],%.4g,%.4g) tgt=(%.4g[0x%08X],%.4g,%.4g) d2=%.4g",
                a0, pos[0], floatBits(pos[0]), pos[1], pos[2],
                tgt[0], floatBits(tgt[0]), tgt[1], tgt[2], d2);
        }
        dbg.updateCameraSnapshot(snap);
    }
    FUN_0019f990_0x19f990(rdram, ctx, runtime);
}

void hookRotMatrixZ_00153170(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &dbg = BattleDebugger::instance();
    const bool check = dbg.isEnabled() && dbg.isBtlActive() && ctx->pc == 0x00153170u;
    const uint32_t a0 = check ? getRegU32(ctx, 4) : 0;
    const uint32_t ra = check ? getRegU32(ctx, 31) : 0;
    const float theta = check ? ctx->f[12] : 0.0f;

    FUN_00153170_0x153170(rdram, ctx, runtime);

    if (check && isBtlAddress(ra))
    {
        float m[16]{};
        if (const uint8_t *p = getConstMemPtr(rdram, a0))
            std::memcpy(m, p, 64);
        if (isBadFloat(theta, 1e6f) || isBadMat4(m))
        {
            dbg.recordAnomaly(
                Category::Matrix, Severity::Error, 0x00153170u, ra, "sceVu0RotMatrixZ",
                "theta=%.6g[0x%08X] m0=(%.4g,%.4g,%.4g,%.4g) m1=(%.4g,%.4g,%.4g,%.4g)",
                theta, floatBits(theta), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
        }
    }
}

void hookApplyMatrix_00152da8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &dbg = BattleDebugger::instance();
    const bool check = dbg.isEnabled() && dbg.isBtlActive() && ctx->pc == 0x00152DA8u;
    const uint32_t a0 = check ? getRegU32(ctx, 4) : 0;
    const uint32_t a1 = check ? getRegU32(ctx, 5) : 0;
    const uint32_t a2 = check ? getRegU32(ctx, 6) : 0;
    const uint32_t ra = check ? getRegU32(ctx, 31) : 0;

    FUN_00152da8_0x152da8(rdram, ctx, runtime);

    if (check && isBtlAddress(ra))
    {
        float out[4]{}, in[4]{}, m[16]{};
        if (const uint8_t *p = getConstMemPtr(rdram, a0))
            std::memcpy(out, p, 16);
        if (isBadVec3(out))
        {
            if (const uint8_t *p = getConstMemPtr(rdram, a2))
                std::memcpy(in, p, 16);
            if (const uint8_t *p = getConstMemPtr(rdram, a1))
                std::memcpy(m, p, 64);
            dbg.recordAnomaly(
                Category::Matrix, Severity::Error, 0x00152DA8u, ra, "sceVu0ApplyMatrix",
                "in=(%.4g,%.4g,%.4g,%.4g) out=(%.4g[0x%08X],%.4g,%.4g) matBad=%d",
                in[0], in[1], in[2], in[3],
                out[0], floatBits(out[0]), out[1], out[2],
                isBadMat4(m) ? 1 : 0);
        }
    }
}

void hookAtan2_00170208(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &dbg = BattleDebugger::instance();
    const bool check = dbg.isEnabled() && dbg.isBtlActive() && dbg.isMathCheckEnabled() &&
                       ctx->pc == 0x00170208u;
    const uint64_t rawY = check ? GPR_U64(ctx, 4) : 0;
    const uint64_t rawX = check ? GPR_U64(ctx, 5) : 0;
    const uint32_t ra = check ? getRegU32(ctx, 31) : 0;

    FUN_00170208_0x170208(rdram, ctx, runtime);

    if (check)
    {
        const uint64_t rawRes = GPR_U64(ctx, 2);
        double y = 0.0, x = 0.0, guest = 0.0;
        std::memcpy(&y, &rawY, 8);
        std::memcpy(&x, &rawX, 8);
        std::memcpy(&guest, &rawRes, 8);
        const double host = std::atan2(y, x);
        const bool mismatch = doubleDiverges(guest, host);
        dbg.countMathCheck(mismatch);
        if (mismatch)
        {
            dbg.recordAnomaly(
                Category::Math, Severity::Critical, 0x00170208u, ra, "atan2(double)",
                "y=%.6g[0x%016llX] x=%.6g[0x%016llX] -> guest=%.6g[0x%016llX] host=%.6g[0x%016llX]",
                y, static_cast<unsigned long long>(rawY),
                x, static_cast<unsigned long long>(rawX),
                guest, static_cast<unsigned long long>(rawRes),
                host, static_cast<unsigned long long>(doubleBits(host)));
        }
    }
}

void hookAtan_0016ee78(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &dbg = BattleDebugger::instance();
    const bool check = dbg.isEnabled() && dbg.isBtlActive() && dbg.isMathCheckEnabled() &&
                       ctx->pc == 0x0016EE78u;
    const uint64_t rawX = check ? GPR_U64(ctx, 4) : 0;
    const uint32_t ra = check ? getRegU32(ctx, 31) : 0;

    FUN_0016ee78_0x16ee78(rdram, ctx, runtime);

    if (check)
    {
        const uint64_t rawRes = GPR_U64(ctx, 2);
        double x = 0.0, guest = 0.0;
        std::memcpy(&x, &rawX, 8);
        std::memcpy(&guest, &rawRes, 8);
        const double host = std::atan(x);
        const bool mismatch = doubleDiverges(guest, host);
        dbg.countMathCheck(mismatch);
        if (mismatch)
        {
            dbg.recordAnomaly(
                Category::Math, Severity::Critical, 0x0016EE78u, ra, "atan(double)",
                "x=%.6g[0x%016llX] -> guest=%.6g[0x%016llX] host=%.6g[0x%016llX]",
                x, static_cast<unsigned long long>(rawX),
                guest, static_cast<unsigned long long>(rawRes),
                host, static_cast<unsigned long long>(doubleBits(host)));
        }
    }
}

void hookDivdf3_00172888(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &dbg = BattleDebugger::instance();
    const bool check = dbg.isEnabled() && dbg.isBtlActive() && dbg.isMathCheckEnabled() &&
                       ctx->pc == 0x00172888u;
    const uint64_t rawA = check ? GPR_U64(ctx, 4) : 0;
    const uint64_t rawB = check ? GPR_U64(ctx, 5) : 0;
    const uint32_t ra = check ? getRegU32(ctx, 31) : 0;

    FUN_00172888_0x172888(rdram, ctx, runtime);

    if (check)
    {
        const uint64_t rawRes = GPR_U64(ctx, 2);
        double a = 0.0, b = 0.0, guest = 0.0;
        std::memcpy(&a, &rawA, 8);
        std::memcpy(&b, &rawB, 8);
        std::memcpy(&guest, &rawRes, 8);
        const double host = a / b;
        const bool mismatch = doubleDiverges(guest, host);
        dbg.countMathCheck(mismatch);
        if (mismatch)
        {
            dbg.recordAnomaly(
                Category::Math, Severity::Critical, 0x00172888u, ra, "__divdf3",
                "a=%.6g[0x%016llX] b=%.6g[0x%016llX] -> guest=%.6g[0x%016llX] host=%.6g[0x%016llX]",
                a, static_cast<unsigned long long>(rawA),
                b, static_cast<unsigned long long>(rawB),
                guest, static_cast<unsigned long long>(rawRes),
                host, static_cast<unsigned long long>(doubleBits(host)));
        }
    }
}

void hookMuldf3_00172600(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &dbg = BattleDebugger::instance();
    const bool check = dbg.isEnabled() && dbg.isBtlActive() && dbg.isMathCheckEnabled() &&
                       ctx->pc == 0x00172600u;
    const uint64_t rawA = check ? GPR_U64(ctx, 4) : 0;
    const uint64_t rawB = check ? GPR_U64(ctx, 5) : 0;
    const uint32_t ra = check ? getRegU32(ctx, 31) : 0;

    FUN_00172600_0x172600(rdram, ctx, runtime);

    if (check)
    {
        const uint64_t rawRes = GPR_U64(ctx, 2);
        double a = 0.0, b = 0.0, guest = 0.0;
        std::memcpy(&a, &rawA, 8);
        std::memcpy(&b, &rawB, 8);
        std::memcpy(&guest, &rawRes, 8);
        const double host = a * b;
        const bool mismatch = doubleDiverges(guest, host);
        dbg.countMathCheck(mismatch);
        if (mismatch)
        {
            dbg.recordAnomaly(
                Category::Math, Severity::Critical, 0x00172600u, ra, "__muldf3",
                "a=%.6g[0x%016llX] b=%.6g[0x%016llX] -> guest=%.6g[0x%016llX] host=%.6g[0x%016llX]",
                a, static_cast<unsigned long long>(rawA),
                b, static_cast<unsigned long long>(rawB),
                guest, static_cast<unsigned long long>(rawRes),
                host, static_cast<unsigned long long>(doubleBits(host)));
        }
    }
}

// ---------------------------------------------------------------------------
// BTL Overlay Camera Hooks
// ---------------------------------------------------------------------------
#ifdef EDO_HAS_OVL_BTL
void hookBtlCameraUpdate_006e95f0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &dbg = BattleDebugger::instance();
    const bool isEntry = (ctx->pc == 0x006E95F0u);
    if (isEntry)
    {
        dbg.onBattleFrame(runtime);
    }

    const bool check = isEntry && dbg.isEnabled();
    const uint32_t s2 = check ? getRegU32(ctx, 4) : 0;
    const uint32_t ra = check ? getRegU32(ctx, 31) : 0;
    float beforePos[3]{}, beforeTgt[3]{};
    if (check)
    {
        if (const uint8_t *p = getConstMemPtr(rdram, s2))
        {
            std::memcpy(beforePos, p + 0x30, 12);
            std::memcpy(beforeTgt, p + 0x80, 12);
        }
    }

    ovl_btl_FUN_006e95f0_0x6e95f0(rdram, ctx, runtime);

    if (check)
    {
        if (const uint8_t *p = getConstMemPtr(rdram, s2))
        {
            CameraSnapshot snap;
            snap.camObjAddr = s2;
            snap.lookAtCallerRa = ra;
            std::memcpy(snap.pos, p + 0x30, 16);
            std::memcpy(snap.tgt, p + 0x80, 16);
            std::memcpy(snap.extra90, p + 0x90, 16);
            std::memcpy(snap.shake, p + 0x100, 16);
            const float dx = snap.tgt[0] - snap.pos[0];
            const float dy = snap.tgt[1] - snap.pos[1];
            const float dz = snap.tgt[2] - snap.pos[2];
            snap.distSq = dx * dx + dy * dy + dz * dz;

            const bool wasBad = isBadVec3(beforePos) || isBadVec3(beforeTgt);
            const bool nowBad = isBadVec3(snap.pos) || isBadVec3(snap.tgt) || isBadFloat(snap.distSq);
            snap.valid = !nowBad;
            if (nowBad)
                snap.reason = "corrupt_after_update";

            if (!wasBad && nowBad)
            {
                dbg.recordAnomaly(
                    Category::Camera, Severity::Critical, 0x006E95F0u, ra, "ovl_btl_cam_update",
                    "TRANSITION s2=0x%08X pos:(%.3f,%.3f,%.3f)->(%.4g[0x%08X],%.3f,%.3f) "
                    "tgt:(%.3f,%.3f,%.3f)->(%.4g[0x%08X],%.3f,%.3f)",
                    s2, beforePos[0], beforePos[1], beforePos[2],
                    snap.pos[0], floatBits(snap.pos[0]), snap.pos[1], snap.pos[2],
                    beforeTgt[0], beforeTgt[1], beforeTgt[2],
                    snap.tgt[0], floatBits(snap.tgt[0]), snap.tgt[1], snap.tgt[2]);
            }
            else if (dbg.isTraceCamera())
            {
                dbg.recordTrace(
                    Category::Camera, 0x006E95F0u, ra,
                    "pos=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f) sh=(%.2f,%.2f,%.2f)",
                    snap.pos[0], snap.pos[1], snap.pos[2],
                    snap.tgt[0], snap.tgt[1], snap.tgt[2],
                    snap.shake[0], snap.shake[1], snap.shake[2]);
            }
            dbg.updateCameraSnapshot(snap);
        }
    }
}

void hookBtlCameraShake_006e99f0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &dbg = BattleDebugger::instance();
    const bool check = dbg.isEnabled() && ctx->pc == 0x006E99F0u;
    const uint32_t s0 = check ? getRegU32(ctx, 4) : 0;
    const uint32_t ra = check ? getRegU32(ctx, 31) : 0;
    float b30[3]{}, b80[3]{};
    if (check)
    {
        if (const uint8_t *p = getConstMemPtr(rdram, s0))
        {
            std::memcpy(b30, p + 0x30, 12);
            std::memcpy(b80, p + 0x80, 12);
        }
    }

    ovl_btl_FUN_006e99f0_0x6e99f0(rdram, ctx, runtime);

    if (check)
    {
        if (const uint8_t *p = getConstMemPtr(rdram, s0))
        {
            float a30[3]{}, a80[3]{}, sh[4]{};
            std::memcpy(a30, p + 0x30, 12);
            std::memcpy(a80, p + 0x80, 12);
            std::memcpy(sh, p + 0x100, 16);
            const bool wasBad = isBadVec3(b30) || isBadVec3(b80);
            const bool nowBad = isBadVec3(a30) || isBadVec3(a80);
            if (!wasBad && nowBad)
            {
                dbg.recordAnomaly(
                    Category::Camera, Severity::Critical, 0x006E99F0u, ra, "ovl_btl_cam_apply_shake",
                    "TRANSITION s0=0x%08X b30=(%.3f,%.3f,%.3f)->a30=(%.4g[0x%08X],%.3f,%.3f) "
                    "b80=(%.3f,%.3f,%.3f)->a80=(%.4g[0x%08X],%.3f,%.3f) sh=(%.4g,%.4g,%.4g,%.4g)",
                    s0, b30[0], b30[1], b30[2], a30[0], floatBits(a30[0]), a30[1], a30[2],
                    b80[0], b80[1], b80[2], a80[0], floatBits(a80[0]), a80[1], a80[2],
                    sh[0], sh[1], sh[2], sh[3]);
            }
        }
    }
}
#endif
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
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_jsonlFile)
    {
        std::fclose(m_jsonlFile);
        m_jsonlFile = nullptr;
    }
}

void BattleDebugger::initFromEnv()
{
    const bool enabled = envFlagEnabled("EDO_BATTLE_DEBUG", false);
    m_enabled.store(enabled, std::memory_order_relaxed);
    m_breakOnAnomaly.store(envFlagEnabled("EDO_BATTLE_BREAK", false), std::memory_order_relaxed);
    m_mathCheckEnabled.store(envFlagEnabled("EDO_BATTLE_MATH_CHECK", true), std::memory_order_relaxed);
    m_traceCamera.store(envFlagEnabled("EDO_BATTLE_TRACE_CAMERA", false), std::memory_order_relaxed);
    m_logStderr.store(envFlagEnabled("EDO_BATTLE_STDERR", true), std::memory_order_relaxed);
    m_stopAfterAnomalyFrames = envU64("EDO_BATTLE_STOP_AFTER", 0);
    m_maxBattleFrames = envU64("EDO_BATTLE_MAX_FRAMES", 0);
    m_uiVisible = enabled && envFlagEnabled("EDO_BATTLE_UI", false);

    if (const char *dumpEnv = std::getenv("EDO_BATTLE_DUMP"); dumpEnv && *dumpEnv)
    {
        m_dumpPath = dumpEnv;
        m_autoDumpOnExit = true;
    }
    else
    {
        m_dumpPath = ".cache/battle-debug/report.txt";
        m_autoDumpOnExit = false;
    }

    if (const char *jsonlEnv = std::getenv("EDO_BATTLE_JSONL"); jsonlEnv && *jsonlEnv)
    {
        m_jsonlPath = jsonlEnv;
        std::error_code ec;
        const std::filesystem::path jp(m_jsonlPath);
        if (jp.has_parent_path())
            std::filesystem::create_directories(jp.parent_path(), ec);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_jsonlFile)
            std::fclose(m_jsonlFile);
        m_jsonlFile = std::fopen(m_jsonlPath.c_str(), "w");
    }

    if (enabled)
    {
        std::fprintf(stderr,
                     "[battle-dbg] ativo (mathCheck=%d break=%d stopAfter=%llu maxFrames=%llu dump=%s jsonl=%s)\n",
                     static_cast<int>(m_mathCheckEnabled.load()),
                     static_cast<int>(m_breakOnAnomaly.load()),
                     static_cast<unsigned long long>(m_stopAfterAnomalyFrames),
                     static_cast<unsigned long long>(m_maxBattleFrames),
                     m_dumpPath.c_str(),
                     m_jsonlPath.empty() ? "(off)" : m_jsonlPath.c_str());
        std::fflush(stderr);
    }
}

void BattleDebugger::installCoreHooks(PS2Runtime &runtime)
{
    runtime.replaceFunction(0x0019F990u, &hookCameraLookAt_0019f990);
    runtime.replaceFunction(0x00153170u, &hookRotMatrixZ_00153170);
    runtime.replaceFunction(0x00152DA8u, &hookApplyMatrix_00152da8);
    runtime.replaceFunction(0x00170208u, &hookAtan2_00170208);
    runtime.replaceFunction(0x0016EE78u, &hookAtan_0016ee78);
    runtime.replaceFunction(0x00172888u, &hookDivdf3_00172888);
    runtime.replaceFunction(0x00172600u, &hookMuldf3_00172600);
}

void BattleDebugger::onBtlOverlayActivated(PS2Runtime &runtime)
{
    m_btlActive.store(true, std::memory_order_relaxed);
    m_battleFrame.store(0, std::memory_order_relaxed);
    m_autoStopTriggered.store(false, std::memory_order_relaxed);
#ifdef EDO_HAS_OVL_BTL
    runtime.replaceFunction(0x006E95F0u, &hookBtlCameraUpdate_006e95f0);
    runtime.replaceFunction(0x006E99F0u, &hookBtlCameraShake_006e99f0);
#else
    (void)runtime;
#endif
    if (isEnabled())
    {
        recordTrace(Category::General, 0x006C6D00u, 0, "BTL overlay activated");
    }
}

void BattleDebugger::onBtlOverlayDeactivated(PS2Runtime & /*runtime*/)
{
    m_btlActive.store(false, std::memory_order_relaxed);
    if (isEnabled() && m_autoDumpOnExit && !m_dumpPath.empty())
    {
        dumpReport(m_dumpPath);
    }
}

void BattleDebugger::onBattleFrame(PS2Runtime *runtime)
{
    const uint64_t frame = m_battleFrame.fetch_add(1, std::memory_order_relaxed) + 1;

    if (!isEnabled())
        return;

    if (!m_autoStopTriggered.load(std::memory_order_relaxed))
    {
        bool shouldStop = false;
        const char *stopReason = nullptr;
        if (m_maxBattleFrames > 0 && frame >= m_maxBattleFrames)
        {
            shouldStop = true;
            stopReason = "max_frames";
        }
        else if (m_stopAfterAnomalyFrames > 0)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_firstAnomaly.has_value() &&
                frame >= m_firstAnomaly->battleFrame + m_stopAfterAnomalyFrames)
            {
                shouldStop = true;
                stopReason = "stop_after_anomaly";
            }
        }

        if (shouldStop && !m_autoStopTriggered.exchange(true, std::memory_order_relaxed))
        {
            if (!m_dumpPath.empty())
                dumpReport(m_dumpPath);
            std::fprintf(stderr, "[battle-dbg] auto-stop (%s) no frame %llu\n",
                         stopReason, static_cast<unsigned long long>(frame));
            std::fflush(stderr);
            if (runtime)
                runtime->requestStop();
            return;
        }
    }

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

void BattleDebugger::writeJsonlAnomalyLocked(const AnomalyEvent &ev, bool isFirst)
{
    if (!m_jsonlFile)
        return;
    std::fprintf(m_jsonlFile,
                 "{\"type\":\"anomaly\",\"seq\":%llu,\"frame\":%llu,\"cat\":\"%s\",\"sev\":\"%s\","
                 "\"fn\":\"0x%08X\",\"fn_name\":\"%s\",\"ra\":\"0x%08X\",\"count\":%u,"
                 "\"first\":%s,\"detail\":\"%s\"}\n",
                 static_cast<unsigned long long>(ev.seq),
                 static_cast<unsigned long long>(ev.battleFrame),
                 categoryName(ev.category),
                 severityName(ev.severity),
                 ev.funcAddr,
                 jsonEscape(ev.funcName).c_str(),
                 ev.callerRa,
                 ev.occurrences,
                 isFirst ? "true" : "false",
                 jsonEscape(ev.detail).c_str());
    std::fflush(m_jsonlFile);
}

void BattleDebugger::writeJsonlTraceLocked(const TraceEvent &ev)
{
    if (!m_jsonlFile)
        return;
    std::fprintf(m_jsonlFile,
                 "{\"type\":\"trace\",\"seq\":%llu,\"frame\":%llu,\"cat\":\"%s\","
                 "\"fn\":\"0x%08X\",\"ra\":\"0x%08X\",\"summary\":\"%s\"}\n",
                 static_cast<unsigned long long>(ev.seq),
                 static_cast<unsigned long long>(ev.battleFrame),
                 categoryName(ev.category),
                 ev.funcAddr,
                 ev.callerRa,
                 jsonEscape(ev.summary).c_str());
    std::fflush(m_jsonlFile);
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

    writeJsonlTraceLocked(ev);

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
            if (occurrences <= 5)
            {
                AnomalyEvent copy = existing;
                copy.battleFrame = frame;
                copy.detail = detail;
                writeJsonlAnomalyLocked(copy, false);
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

            writeJsonlAnomalyLocked(ev, isFirst);

            if (m_anomalies.size() < kMaxAnomalies)
            {
                m_dedupIndex[key] = m_anomalies.size();
                m_anomalies.push_back(std::move(ev));
            }
            shouldPrint = true;
        }

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

void BattleDebugger::drawUi(PS2Runtime & /*runtime*/)
{
    // Implemented in Commit 3 (ImGui panel)
}

} // namespace edo_debug
