// Game overrides de Naruto Shippuden: Ultimate Ninja 5 (SLES_556.05, PAL v1.00).
//
// Convenção dos handlers: ctx->pc = $ra ANTES de chamar qualquer HLE do runtime,
// como fazem os stubs gerados pelo ps2_recomp.

#include "game_overrides.h"
#ifdef EDO_HAS_PARALLEL_GS
#include "edo_parallel_gs.h"
#endif
#include "ps2_runtime.h"
#include "ps2_iop_transport.h"
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "ps2_recompiled_functions.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef EDO_HAS_CTORS
extern void edoRegisterCtorFunctions(PS2Runtime &runtime);
#endif

struct EdoOverlayEntry
{
    uint32_t address;
    PS2Runtime::RecompiledFunction fn;
};

#ifdef EDO_HAS_OVL_BTL
extern const EdoOverlayEntry *edoOverlayEntries_btl(size_t *outCount);
#endif
#ifdef EDO_HAS_OVL_ADV
extern const EdoOverlayEntry *edoOverlayEntries_adv(size_t *outCount);
#endif
#ifdef EDO_HAS_OVL_ETC
extern const EdoOverlayEntry *edoOverlayEntries_etc(size_t *outCount);
#endif

namespace
{
    constexpr uint32_t kEntry = 0x00100008u;
    constexpr uint32_t kElfCrc32 = 0x49444F1Eu;

    // 0x0015F068: write(fd, buf, len) da libkernel para stdout/stderr -> redireciona para o host.
    void ttyWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t buf = getRegU32(ctx, 5);
        const int32_t len = static_cast<int32_t>(getRegU32(ctx, 6));

        const uint8_t *host = (fd == 1 || fd == 2) ? getConstMemPtr(rdram, buf) : nullptr;
        if (!host || len < 0)
        {
            setReturnS32(ctx, -1);
        }
        else
        {
            std::fwrite(host, 1, static_cast<size_t>(len), fd == 2 ? stderr : stdout);
            std::fflush(fd == 2 ? stderr : stdout);
            setReturnS32(ctx, len);
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    struct IsoEntry
    {
        uint32_t lsn = 0;
        uint32_t size = 0;
        std::string name;
    };

    const std::unordered_map<std::string, IsoEntry> &isoIndex()
    {
        static const std::unordered_map<std::string, IsoEntry> index = [] {
            std::unordered_map<std::string, IsoEntry> map;
            const char *isoPath = std::getenv("EDO_ISO");
            if (!isoPath || !*isoPath)
            {
                return map;
            }
            std::ifstream f(isoPath, std::ios::binary);
            if (!f)
            {
                return map;
            }
            auto readU32LE = [](const uint8_t *p) -> uint32_t {
                return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            };
            uint8_t pvd[2048]{};
            f.seekg(16LL * 2048LL);
            f.read(reinterpret_cast<char *>(pvd), sizeof(pvd));
            if (f.gcount() != sizeof(pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0)
            {
                return map;
            }
            const uint32_t rootLba = readU32LE(pvd + 156 + 2);
            const uint32_t rootSize = readU32LE(pvd + 156 + 10);

            auto walk = [&](auto &&self, uint32_t lba, uint32_t size, const std::string &prefix) -> void {
                std::vector<uint8_t> buf(size);
                f.seekg(static_cast<std::streamoff>(lba) * 2048LL);
                f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(size));
                size_t i = 0;
                while (i < buf.size())
                {
                    const uint8_t rl = buf[i];
                    if (rl == 0)
                    {
                        i = ((i / 2048u) + 1u) * 2048u;
                        continue;
                    }
                    if (i + 33u > buf.size())
                    {
                        break;
                    }
                    const uint32_t elba = readU32LE(buf.data() + i + 2);
                    const uint32_t esz = readU32LE(buf.data() + i + 10);
                    const uint8_t flags = buf[i + 25];
                    const uint8_t nlen = buf[i + 32];
                    if (i + 33u + nlen <= buf.size() && nlen > 0)
                    {
                        std::string name(reinterpret_cast<const char *>(buf.data() + i + 33), nlen);
                        if (!(nlen == 1 && (name[0] == '\0' || name[0] == '\1')))
                        {
                            for (char &ch : name)
                            {
                                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                            }
                            const std::string full = prefix + "/" + name;
                            if (flags & 2u)
                            {
                                self(self, elba, esz, full);
                            }
                            else
                            {
                                IsoEntry e{elba, esz, name};
                                map[full] = e;
                                const auto semi = full.find(';');
                                if (semi != std::string::npos)
                                {
                                    map[full.substr(0, semi)] = e;
                                }
                            }
                        }
                    }
                    i += rl;
                }
            };
            walk(walk, rootLba, rootSize, "");
            std::fprintf(stderr, "[edo] ISO9660 indexado: %zu entradas\n", map.size());
            return map;
        }();
        return index;
    }

    // 0x001733C0: sceCdLayerSearchFile(fp, name, layer).
    // Usa os LSNs reais da ISO9660 quando EDO_ISO está definido, para que o EE e o
    // CRI_ADXI.IRX no IOP leiam exatamente os mesmos setores (em especial DATA.CVM).
    void cdLayerSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fpAddr = getRegU32(ctx, 4);
        const uint32_t pathAddr = getRegU32(ctx, 5);
        const auto &idx = isoIndex();
        if (!idx.empty())
        {
            const uint8_t *p = getConstMemPtr(rdram, pathAddr);
            uint8_t *dst = getMemPtr(rdram, fpAddr);
            if (p && dst)
            {
                std::string raw(reinterpret_cast<const char *>(p), strnlen(reinterpret_cast<const char *>(p), 256));
                std::string norm;
                norm.reserve(raw.size() + 1);
                for (char ch : raw)
                {
                    if (ch == '\\')
                        ch = '/';
                    norm.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
                }
                if (norm.rfind("CDROM0:", 0) == 0)
                    norm.erase(0, 7);
                if (norm.empty() || norm[0] != '/')
                    norm.insert(norm.begin(), '/');
                auto it = idx.find(norm);
                if (it == idx.end())
                {
                    const auto semi = norm.find(';');
                    if (semi != std::string::npos)
                        it = idx.find(norm.substr(0, semi));
                }
                if (it != idx.end())
                {
                    std::memset(dst, 0, 36);
                    std::memcpy(dst + 0, &it->second.lsn, 4);
                    std::memcpy(dst + 4, &it->second.size, 4);
                    const size_t ncopy = std::min<size_t>(it->second.name.size(), 15);
                    std::memcpy(dst + 8, it->second.name.data(), ncopy);
                    std::fprintf(stderr, "[edo:cd] search \"%s\" -> lsn=0x%X size=0x%X\n",
                                 raw.c_str(), it->second.lsn, it->second.size);
                    setReturnS32(ctx, 1);
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
        }
        ctx->pc = getRegU32(ctx, 31);
        ps2_stubs::sceCdSearchFile(rdram, ctx, runtime);
    }

    // 0x00162808: sceSifBindRpc(client, sid, mode) com espera ativa pelas threads do IOP.
    void sifBindRpcBlocking(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sid = getRegU32(ctx, 5);
        const uint32_t mode = getRegU32(ctx, 6);

        if (runtime && (mode & 1u) == 0u)
        {
            constexpr uint64_t kSliceCycles = 294912000ull / 1000ull; // 1 ms de EE
            constexpr int kMaxSlices = 2000;
            int slices = 0;
            while (!PS2IopTransport::canBindRpc(runtime, sid) && slices < kMaxSlices)
            {
                runtime->eeScheduler().accountCycles(static_cast<uint32_t>(kSliceCycles));
                ++slices;
            }
            if (slices > 0)
            {
                std::fprintf(stderr, "[edo] sceSifBindRpc sid=0x%X: esperou %d ms pelo IOP%s\n",
                             sid, slices, slices >= kMaxSlices ? " (TIMEOUT)" : "");
            }
        }

        ctx->pc = getRegU32(ctx, 31);
        ps2_stubs::sceSifBindRpc(rdram, ctx, runtime);
    }

    // 0x001E1390: player de FMV .PSS (sceMpeg + FFmpeg via PS2Recomp).
    // Se EDO_SKIP_FMV=1 estiver definido, pula o video imediatamente retornando 1.
    void fmvPlayerHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool skipFmv = [] {
            const char *v = std::getenv("EDO_SKIP_FMV");
            return v && *v && *v != '0';
        }();
        const uint32_t movieId = getRegU32(ctx, 4);
        const uint32_t flags = getRegU32(ctx, 5);
        if (skipFmv)
        {
            std::fprintf(stderr, "[edo:fmv] pulando video .PSS id=%u (EDO_SKIP_FMV=1)\n", movieId);
            std::fflush(stderr);
            setReturnS32(ctx, 1);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        std::fprintf(stderr, "[edo:fmv] -> reproduzindo video .PSS id=%u flags=0x%X\n", movieId, flags);
        std::fflush(stderr);
        FUN_001e1390_0x1e1390(rdram, ctx, runtime);
        std::fprintf(stderr, "[edo:fmv] <- video .PSS id=%u finalizado (ret=%d)\n",
                     movieId, static_cast<int32_t>(getRegU32(ctx, 2)));
        std::fflush(stderr);
    }

    // 0x0019FEC0: atualizacao hierarquica da matriz mundo de nos da cena (world = parent.world * local).
    // O codigo gerado pelo PS2Recomp tem bugs em vsqi/vlqi/vlqd (vsqi $vf0, ($vi1++) troca fs/it,
    // corrompe $vi0 e escreve em RDRAM 0x00000010 em vez de VU0 Data RAM), zerando todas as matrizes
    // a partir do 1o no filho. Reimplementamos fielmente em C++ mantendo exatamente a mesma semantica.
    void updateNodeWorldMatrix_0019fec0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        uint32_t node = getRegU32(ctx, 4);
        if (node == 0u)
        {
            ctx->pc = getRegU32(ctx, 31);
            return;
        }

        uint32_t chain[256];
        size_t count = 0;
        size_t topDirty = 0;
        uint32_t cur = node;
        while (cur != 0u && count < 255u)
        {
            const uint8_t *p = getConstMemPtr(rdram, cur);
            if (!p)
                break;
            if (static_cast<int8_t>(p[0x8D]) != 0)
                topDirty = count;
            chain[count++] = cur;
            uint32_t parent = 0;
            std::memcpy(&parent, p + 0x80, 4);
            cur = parent;
        }
        if (count == 0)
        {
            ctx->pc = getRegU32(ctx, 31);
            return;
        }

        auto mulMat4 = [](float out[16], const float a[16], const float b[16]) {
            for (int c = 0; c < 4; ++c)
            {
                const float bx = b[c * 4 + 0];
                const float by = b[c * 4 + 1];
                const float bz = b[c * 4 + 2];
                const float bw = b[c * 4 + 3];
                for (int r = 0; r < 4; ++r)
                {
                    out[c * 4 + r] = a[0 * 4 + r] * bx + a[1 * 4 + r] * by +
                                     a[2 * 4 + r] * bz + a[3 * 4 + r] * bw;
                }
            }
        };

        float world[16]{};
        size_t idx = topDirty;
        if (idx + 1u < count)
        {
            const uint8_t *parentPtr = getConstMemPtr(rdram, chain[idx + 1u]);
            if (parentPtr)
                std::memcpy(world, parentPtr, 64);
            for (size_t step = idx + 1u; step-- > 0;)
            {
                uint8_t *nodePtr = getMemPtr(rdram, chain[step]);
                if (!nodePtr)
                    break;
                float local[16];
                float nextWorld[16];
                std::memcpy(local, nodePtr + 0x40, 64);
                mulMat4(nextWorld, world, local);
                std::memcpy(world, nextWorld, 64);
                std::memcpy(nodePtr, world, 64);
                nodePtr[0x8D] = 0;
            }
        }
        else
        {
            uint8_t *rootPtr = getMemPtr(rdram, chain[idx]);
            if (rootPtr)
            {
                std::memcpy(world, rootPtr + 0x40, 64);
                std::memcpy(rootPtr, world, 64);
                rootPtr[0x8D] = 0;
            }
            for (size_t step = idx; step-- > 0;)
            {
                uint8_t *nodePtr = getMemPtr(rdram, chain[step]);
                if (!nodePtr)
                    break;
                float local[16];
                float nextWorld[16];
                std::memcpy(local, nodePtr + 0x40, 64);
                mulMat4(nextWorld, world, local);
                std::memcpy(world, nextWorld, 64);
                std::memcpy(nodePtr, world, 64);
                nodePtr[0x8D] = 0;
            }
        }

        ctx->pc = getRegU32(ctx, 31);
    }

    // 0x003D5700: helper na fronteira do CreateGapFunctions (chamado por BTL.BIN e ADV.BIN).
    // Se int16(a0) < int16(a1), retorna float(int32(a0)) / float(int32(a1)); senao retorna 1.0f.
    void ratioClamp1f_003d5700(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        const int32_t raw0 = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t raw1 = static_cast<int32_t>(getRegU32(ctx, 5));
        const int16_t s0 = static_cast<int16_t>(raw0 & 0xFFFF);
        const int16_t s1 = static_cast<int16_t>(raw1 & 0xFFFF);
        ctx->f[0] = (s0 < s1) ? (static_cast<float>(raw0) / static_cast<float>(raw1)) : 1.0f;
        ctx->pc = getRegU32(ctx, 31);
    }

    // 0x00100270: ativacao de overlay MWo3 chamada por FUN_001c3150(slot, filename).
    // Troca os entrypoints do overlay ativo na g_sparseFunctionTable ANTES de
    // FUN_00100270 zerar o .bss e executar os construtores estaticos (.ctors) via FUN_0011a0b0.
    void activateOverlay(PS2Runtime *runtime, uint32_t overlayId, const char *name, uint32_t base, uint32_t size)
    {
        static std::mutex mu;
        static uint32_t activeId = 0;
        static const EdoOverlayEntry *activeEntries = nullptr;
        static size_t activeCount = 0;

        std::lock_guard<std::mutex> lock(mu);
        if (activeId == overlayId && activeEntries != nullptr)
        {
            return;
        }

        const EdoOverlayEntry *nextEntries = nullptr;
        size_t nextCount = 0;
        switch (overlayId)
        {
#ifdef EDO_HAS_OVL_BTL
        case 1u:
            nextEntries = ::edoOverlayEntries_btl(&nextCount);
            break;
#endif
#ifdef EDO_HAS_OVL_ADV
        case 2u:
            nextEntries = ::edoOverlayEntries_adv(&nextCount);
            break;
#endif
#ifdef EDO_HAS_OVL_ETC
        case 3u:
            nextEntries = ::edoOverlayEntries_etc(&nextCount);
            break;
#endif
        default:
            break;
        }

        if (activeEntries != nullptr)
        {
            for (size_t i = 0; i < activeCount; ++i)
            {
                runtime->replaceFunction(activeEntries[i].address, nullptr);
            }
            activeEntries = nullptr;
            activeCount = 0;
            activeId = 0;
        }

        if (nextEntries != nullptr && nextCount > 0)
        {
            for (size_t i = 0; i < nextCount; ++i)
            {
                runtime->registerFunction(nextEntries[i].address, nextEntries[i].fn);
            }
            activeEntries = nextEntries;
            activeCount = nextCount;
            activeId = overlayId;
            std::fprintf(stderr, "[edo:ovl] ativo id=%u (%s) base=0x%08X size=0x%X (%zu entrypoints)\n",
                         overlayId, name ? name : "?", base, size, nextCount);
            std::fflush(stderr);
        }
        else
        {
            std::fprintf(stderr, "[edo:ovl] AVISO: overlay id=%u (%s) nao recompilado (base=0x%08X size=0x%X)\n",
                         overlayId, name ? name : "?", base, size);
            std::fflush(stderr);
        }
    }

    void overlayActivateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x00100270u && runtime != nullptr)
        {
            const uint32_t base = getRegU32(ctx, 4);
            const uint32_t size = getRegU32(ctx, 5);
            const uint8_t *hdr = getConstMemPtr(rdram, base);
            if (hdr != nullptr)
            {
                uint32_t magic = 0;
                uint32_t overlayId = 0;
                std::memcpy(&magic, hdr + 0, 4);
                std::memcpy(&overlayId, hdr + 4, 4);
                if (magic == 0x336F574Du) // "MWo3"
                {
                    char nameBuf[33]{};
                    std::memcpy(nameBuf, hdr + 0x20, 32);
                    activateOverlay(runtime, overlayId, nameBuf, base, size);
                }
            }
        }
        FUN_00100270_0x100270(rdram, ctx, runtime);
    }

    // 0x001629E8: sceSifCallRpc com trace opcional (EDO_TRACE_RPC=1).
    void sifCallRpcTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool enabled = [] {
            const char *v = std::getenv("EDO_TRACE_RPC");
            return v && *v && *v != '0';
        }();
        static uint64_t entries = 0;
        ++entries;

        const uint32_t client = getRegU32(ctx, 4);
        const uint32_t fno = getRegU32(ctx, 5);
        const uint32_t mode = getRegU32(ctx, 6);
        const uint32_t ra = getRegU32(ctx, 31);
        const auto t0 = std::chrono::steady_clock::now();

        if (enabled && (entries <= 60 || entries % 2000 == 0))
        {
            std::fprintf(stderr, "[edo:rpc] -> #%llu client=0x%X fno=0x%X mode=%u ra=0x%X\n",
                         static_cast<unsigned long long>(entries), client, fno, mode, ra);
        }

        ctx->pc = ra;
        ps2_syscalls::sceSifCallRpc(rdram, ctx, runtime);

        if (enabled && (entries <= 60 || entries % 2000 == 0))
        {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            std::fprintf(stderr, "[edo:rpc] <- #%llu client=0x%X fno=0x%X ret=%d %lldus\n",
                         static_cast<unsigned long long>(entries), client, fno,
                         static_cast<int32_t>(getRegU32(ctx, 2)), static_cast<long long>(us));
        }
    }

    struct SdkBinding
    {
        uint32_t address;
        const char *handler;
    };

    constexpr SdkBinding kSdkBindings[] = {
#include "edo_sdk_bindings.inc"
    };

    void configureDiscImage()
    {
        const char *iso = std::getenv("EDO_ISO");
        if (!iso || !*iso)
        {
            return;
        }
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = iso;
        PS2Runtime::setIoPaths(paths);
        std::fprintf(stderr, "[edo] cdImage = %s\n", iso);
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandler(std::string_view name)
    {
        const std::string_view syscall = ps2_runtime_calls::resolveSyscallName(name);
        if (!syscall.empty())
        {
#define EDO_RESOLVE_SYSCALL(n) \
    if (syscall == std::string_view{#n}) return &ps2_syscalls::n;
            PS2_SYSCALL_LIST(EDO_RESOLVE_SYSCALL)
#undef EDO_RESOLVE_SYSCALL
        }
        const std::string_view stub = ps2_runtime_calls::resolveStubName(name);
        if (!stub.empty())
        {
#define EDO_RESOLVE_STUB(n) \
    if (stub == std::string_view{#n}) return &ps2_stubs::n;
            PS2_STUB_LIST(EDO_RESOLVE_STUB)
#undef EDO_RESOLVE_STUB
        }
        return std::nullopt;
    }

    std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> &sdkHandlers()
    {
        static std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> handlers;
        return handlers;
    }

    void sdkTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entry = ctx->pc;
        const auto it = sdkHandlers().find(entry);
        ctx->pc = getRegU32(ctx, 31);
        if (it == sdkHandlers().end())
        {
            std::fprintf(stderr, "[edo] trampolim sem handler para 0x%08X\n", entry);
            setReturnS32(ctx, -1);
            return;
        }
        it->second(rdram, ctx, runtime);
    }

    void apply(PS2Runtime &runtime)
    {
#ifdef EDO_HAS_CTORS
        ::edoRegisterCtorFunctions(runtime);
#endif
#ifdef EDO_HAS_PARALLEL_GS
        edo_parallel_gs::installIfRequested(runtime);
#endif
        configureDiscImage();

        size_t bound = 0;
        for (const SdkBinding &b : kSdkBindings)
        {
            const auto handler = resolveHandler(b.handler);
            if (!handler)
            {
                std::fprintf(stderr, "[edo] handler '%s' não existe no runtime (0x%08X)\n", b.handler, b.address);
                continue;
            }
            sdkHandlers()[b.address] = *handler;
            if (runtime.replaceFunction(b.address, sdkTrampoline))
            {
                ++bound;
            }
        }
        std::fprintf(stderr, "[edo] %zu/%zu SDK bindings aplicados\n", bound, std::size(kSdkBindings));

        // Bindings de libmpeg que o ps2_analyzer nao identificou automaticamente.
        sdkHandlers()[0x001595E0u] = &ps2_stubs::sceMpegDelete;
        runtime.replaceFunction(0x001595E0u, sdkTrampoline);
        sdkHandlers()[0x00159748u] = &ps2_stubs::sceMpegIsEnd;
        runtime.replaceFunction(0x00159748u, sdkTrampoline);
        sdkHandlers()[0x00159758u] = &ps2_stubs::sceMpegIsRefBuffEmpty;
        runtime.replaceFunction(0x00159758u, sdkTrampoline);

        // Sobrescreve handlers genéricos com versões específicas/instrumentadas.
        runtime.replaceFunction(0x00100270u, overlayActivateHook);
        runtime.replaceFunction(0x0019FEC0u, updateNodeWorldMatrix_0019fec0);
        runtime.replaceFunction(0x003D5700u, ratioClamp1f_003d5700);
        runtime.replaceFunction(0x0015F068u, ttyWrite);
        runtime.replaceFunction(0x00162808u, sifBindRpcBlocking);
        runtime.replaceFunction(0x001629E8u, sifCallRpcTraced);
        runtime.replaceFunction(0x001733C0u, cdLayerSearchFile);
        runtime.replaceFunction(0x001E1390u, fmvPlayerHook);

        if (const char *tf = std::getenv("EDO_TRACE_FMV"); tf && *tf && *tf != '0')
        {
            runtime.replaceFunction(0x0015DC20u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:fmv] sceMpegAddStrCallback mp=0x%X type=%u ch=%u fn=0x%X data=0x%X\n",
                             getRegU32(c, 4), getRegU32(c, 5), getRegU32(c, 6), getRegU32(c, 7), getRegU32(c, 8));
                std::fflush(stderr);
                c->pc = getRegU32(c, 31);
                ps2_stubs::sceMpegAddStrCallback(r, c, rt);
            });
            runtime.replaceFunction(0x0015D820u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                const int id = ++n;
                const uint32_t mp = getRegU32(c, 4);
                const uint32_t data = getRegU32(c, 5);
                const uint32_t avail = getRegU32(c, 6);
                const uint32_t ring = getRegU32(c, 7);
                const uint32_t rsz = getRegU32(c, 8);
                c->pc = getRegU32(c, 31);
                ps2_stubs::sceMpegDemuxPssRing(r, c, rt);
                const int32_t ret = static_cast<int32_t>(getRegU32(c, 2));
                if (id <= 12 || (id % 200 == 0))
                {
                    std::fprintf(stderr, "[edo:fmv] sceMpegDemuxPssRing #%d mp=0x%X data=0x%X avail=%u ring=0x%X rsz=0x%X -> %d\n",
                                 id, mp, data, avail, ring, rsz, ret);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x00104800u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                const int id = ++n;
                if (id <= 5)
                {
                    std::fprintf(stderr, "[edo:fmv] -> FUN_00104800 (videoCb) #%d\n", id);
                    std::fflush(stderr);
                }
                FUN_00104800_0x104800(r, c, rt);
                if (id <= 5)
                {
                    std::fprintf(stderr, "[edo:fmv] <- FUN_00104800 (videoCb) #%d ret=%d\n", id, static_cast<int32_t>(getRegU32(c, 2)));
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x00159628u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                const int id = ++n;
                const uint32_t mp = getRegU32(c, 4);
                const uint32_t img = getRegU32(c, 5);
                if (id <= 8 || id % 30 == 0)
                {
                    std::fprintf(stderr, "[edo:fmv] -> sceMpegGetPicture #%d mp=0x%X img=0x%X\n", id, mp, img);
                    std::fflush(stderr);
                }
                c->pc = getRegU32(c, 31);
                ps2_stubs::sceMpegGetPicture(r, c, rt);
                if (id <= 8 || id % 30 == 0)
                {
                    const uint32_t *m = reinterpret_cast<const uint32_t *>(getConstMemPtr(r, mp));
                    std::fprintf(stderr, "[edo:fmv] <- sceMpegGetPicture #%d ret=%d %ux%u frame=%u\n",
                                 id, static_cast<int32_t>(getRegU32(c, 2)),
                                 m ? m[0] : 0u, m ? m[1] : 0u, m ? m[2] : 0u);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x001044A0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:fmv] FUN_001044a0 (startDisplay) a0=%u\n", getRegU32(c, 4));
                std::fflush(stderr);
                FUN_001044a0_0x1044a0(r, c, rt);
            });
            runtime.replaceFunction(0x00104990u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 5)
                {
                    std::fprintf(stderr, "[edo:fmv] FUN_00104990 (buildGif) #%d w=%u h=%u\n", n, getRegU32(c, 7), getRegU32(c, 8));
                    std::fflush(stderr);
                }
                FUN_00104990_0x104990(r, c, rt);
            });
            runtime.replaceFunction(0x00105120u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 8 || n % 60 == 0)
                {
                    std::fprintf(stderr, "[edo:fmv] FUN_00105120 (dispDone) #%d\n", n);
                    std::fflush(stderr);
                }
                FUN_00105120_0x105120(r, c, rt);
            });
            runtime.replaceFunction(0x001011F0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:fmv] FUN_001011f0 (audioStart) aoBuf=0x%X\n", getRegU32(c, 4));
                std::fflush(stderr);
                FUN_001011f0_0x1011f0(r, c, rt);
            });
            runtime.replaceFunction(0x001046A0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                const int id = ++n;
                const uint32_t cbData = getRegU32(c, 5);
                const uint32_t user = getRegU32(c, 6);
                const uint32_t dataAddr = cbData ? *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, cbData + 8u)) : 0u;
                const uint32_t dataLen = cbData ? *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, cbData + 12u)) : 0u;
                FUN_001046a0_0x1046a0(r, c, rt);
                if (id <= 10 || id % 100 == 0)
                {
                    std::fprintf(stderr, "[edo:fmv] FUN_001046a0 (pcmCb) #%d data=0x%X len=%u user=0x%X ret=%d\n",
                                 id, dataAddr, dataLen, user, static_cast<int32_t>(getRegU32(c, 2)));
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x00100C20u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                const int id = ++n;
                FUN_00100c20_0x100c20(r, c, rt);
                const int32_t ret = static_cast<int32_t>(getRegU32(c, 2));
                if (id <= 5 || ret > 0)
                {
                    static int sentLogs = 0;
                    if (id <= 5 || ++sentLogs <= 10)
                    {
                        std::fprintf(stderr, "[edo:fmv] FUN_00100c20 (aoSend) #%d ret=%d\n", id, ret);
                        std::fflush(stderr);
                    }
                }
            });
            runtime.replaceFunction(0x001057B0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                FUN_001057b0_0x1057b0(r, c, rt);
                if (++n <= 5 || n % 15 == 0)
                {
                    const uint32_t ctrl = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617B3Cu));
                    const uint32_t vo = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617B28u));
                    const uint32_t ao = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617B18u));
                    const uint32_t mp = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617B14u));
                    const uint8_t ctrlState = ctrl ? *getConstMemPtr(r, ctrl + 0x3Au) : 0xFF;
                    const int32_t voCnt = vo ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, vo + 0x1Cu)) : -1;
                    const int32_t voMax = vo ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, vo + 0x20u)) : -1;
                    const int32_t aoState = ao ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, ao)) : -1;
                    const int32_t aoSent = ao ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, ao + 0x54u)) : -1;
                    const int32_t aoIopSz = ao ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, ao + 0x48u)) : -1;
                    const int32_t mpFrames = mp ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, mp + 0x08u)) : -1;
                    const uint32_t dispOn = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617B40u));
                    std::fprintf(stderr, "[edo:fmv] poll #%d ret=%u ctrlSt=%u dispOn=%u vo=%d/%d ao=(st=%d sent=%d/%d) mpFrames=%d\n",
                                 n, getRegU32(c, 2), ctrlState, dispOn, voCnt, voMax, aoState, aoSent, aoIopSz, mpFrames);
                    std::fflush(stderr);
                }
            });
        }

        if (const char *tb = std::getenv("EDO_TRACE_BOOT"); tb && *tb && *tb != '0')
        {
            static auto wrapBoot = [](const char *name, PS2Runtime::RecompiledFunction orig) {
                return [name, orig](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) {
                    std::fprintf(stderr, "[edo:boot] -> %s (a0=0x%X a1=0x%X ra=0x%X)\n",
                                 name, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 31));
                    std::fflush(stderr);
                    orig(rdram, ctx, rt);
                    std::fprintf(stderr, "[edo:boot] <- %s (v0=0x%X)\n", name, getRegU32(ctx, 2));
                    std::fflush(stderr);
                };
            };
            (void)wrapBoot;
            runtime.replaceFunction(0x001C1B70u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001c1b70 (IOP init)\n"); std::fflush(stderr);
                FUN_001c1b70_0x1c1b70(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_001c1b70\n"); std::fflush(stderr);
            });
            runtime.replaceFunction(0x00106100u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_00106100 (pc=0x%X ra=0x%X)\n", c->pc, getRegU32(c, 31)); std::fflush(stderr);
                FUN_00106100_0x106100(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_00106100 (pc=0x%X ra=0x%X)\n", c->pc, getRegU32(c, 31)); std::fflush(stderr);
            });
            runtime.replaceFunction(0x001DFA00u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001dfa00 (sndbase.irx)\n"); std::fflush(stderr);
                FUN_001dfa00_0x1dfa00(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_001dfa00\n"); std::fflush(stderr);
            });
            runtime.replaceFunction(0x001DFA40u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001dfa40 (sound init)\n"); std::fflush(stderr);
                FUN_001dfa40_0x1dfa40(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_001dfa40\n"); std::fflush(stderr);
            });
            runtime.replaceFunction(0x001C5EA0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001c5ea0\n"); std::fflush(stderr);
                FUN_001c5ea0_0x1c5ea0(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_001c5ea0\n"); std::fflush(stderr);
            });
            runtime.replaceFunction(0x001C1C40u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001c1c40 (cri_adxi.irx)\n"); std::fflush(stderr);
                FUN_001c1c40_0x1c1c40(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_001c1c40\n"); std::fflush(stderr);
            });
            runtime.replaceFunction(0x001E6B20u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001e6b20 (main game thread)\n"); std::fflush(stderr);
                FUN_001e6b20_0x1e6b20(r, c, rt);
            });
            runtime.replaceFunction(0x001C2310u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001c2310 (CVM table)\n"); std::fflush(stderr);
                FUN_001c2310_0x1c2310(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_001c2310 pc=0x%X\n", c->pc); std::fflush(stderr);
            });
            runtime.replaceFunction(0x001E7B10u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001e7b10\n"); std::fflush(stderr);
                FUN_001e7b10_0x1e7b10(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_001e7b10 pc=0x%X\n", c->pc); std::fflush(stderr);
            });
            runtime.replaceFunction(0x001ECF50u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001ecf50 (boot screen/lang)\n"); std::fflush(stderr);
                FUN_001ecf50_0x1ecf50(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_001ecf50 pc=0x%X\n", c->pc); std::fflush(stderr);
            });
            runtime.replaceFunction(0x00108E50u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                ++n;
                if (n <= 3 || n == 60 || n == 120) {
                    const uint32_t d_ac = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8ACu));
                    const uint32_t d_98 = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE898u));
                    const uint32_t d_f0 = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8F0u));
                    const uint32_t d_f4 = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8F4u));
                    const uint32_t d_f8 = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8F8u));
                    const uint32_t d_fc = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8FCu));
                    auto st = [&](int tid) {
                        const auto *t = rt->eeScheduler().thread(tid);
                        return t ? static_cast<int>(t->status) : -1;
                    };
                    const auto *t1 = rt->eeScheduler().thread(1);
                    std::fprintf(stderr, "[edo:vsync] #%d ac=%u 98=%u f0=%u(st=%d) f4=%u(st=%d) f8=%u(st=%d pc=0x%X wait=%d wcnt=%u) fc=%u(st=%d)\n",
                                 n, d_ac, d_98, d_f0, st(d_f0), d_f4, st(d_f4), d_f8, st(d_f8),
                                 t1 ? t1->activeContext().pc : 0u,
                                 t1 ? static_cast<int>(t1->wait.reason) : -1,
                                 t1 ? t1->wakeupCount : 0u,
                                 d_fc, st(d_fc));
                    std::fflush(stderr);
                }
                FUN_00108e50_0x108e50(r, c, rt);
            });
            runtime.replaceFunction(0x001C3150u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                const uint32_t a0 = getRegU32(c, 4);
                const uint32_t a1 = getRegU32(c, 5);
                const uint8_t *s = a1 ? getConstMemPtr(r, a1) : nullptr;
                std::fprintf(stderr, "[edo:ovl] FUN_001c3150 a0=%u path=\"%s\" ra=0x%X\n",
                             a0, s ? reinterpret_cast<const char *>(s) : "(null)", getRegU32(c, 31));
                std::fflush(stderr);
                FUN_001c3150_0x1c3150(r, c, rt);
            });
            runtime.replaceFunction(0x00113680u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                FUN_00113680_0x113680(r, c, rt);
                static const bool autoPress = [] {
                    const char *v = std::getenv("EDO_AUTO_PRESS");
                    return v && *v && *v != '0';
                }();
                static int n = 0;
                ++n;
                if (autoPress && n >= 90 && (n % 60) == 0) {
                    const uint32_t sysPtr = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617EF0u - 0x3F4u));
                    if (sysPtr) {
                        uint32_t *padEdge = reinterpret_cast<uint32_t *>(getMemPtr(r, sysPtr + 0x84u));
                        if (padEdge) {
                            *padEdge |= 0x840u; // Cross (0x40) | Start (0x800)
                        }
                    }
                }
            });
            runtime.replaceFunction(0x001D5760u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 5 || n % 120 == 0) {
                    std::fprintf(stderr, "[edo:sched] FUN_001d5760 #%d tid=%d\n", n, rt->eeScheduler().currentThreadId()); std::fflush(stderr);
                }
                FUN_001d5760_0x1d5760(r, c, rt);
            });
            runtime.replaceFunction(0x00108510u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 5) {
                    const uint8_t *p = getConstMemPtr(r, getRegU32(c, 4));
                    std::fprintf(stderr, "[edo:waitv] -> FUN_00108510 #%d p0=%u p1=%u p2=%u p2ad=%u\n",
                                 n, p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, p ? p[0x2ad] : 0);
                    std::fflush(stderr);
                }
                FUN_00108510_0x108510(r, c, rt);
                if (n <= 5) {
                    std::fprintf(stderr, "[edo:waitv] <- FUN_00108510 #%d pc=0x%X\n", n, c->pc);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x001D5200u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 10) {
                    std::fprintf(stderr, "[edo:sleep] FUN_001d5200 #%d task=0x%X frames=%d ra=0x%X\n",
                                 n, getRegU32(c, 4), static_cast<int32_t>(getRegU32(c, 5)), getRegU32(c, 31));
                    std::fflush(stderr);
                }
                FUN_001d5200_0x1d5200(r, c, rt);
            });
            runtime.replaceFunction(0x00108600u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 5) {
                    const uint32_t ac = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8ACu));
                    std::fprintf(stderr, "[edo:frame] -> FUN_00108600 #%d ac=%u\n", n, ac); std::fflush(stderr);
                }
                FUN_00108600_0x108600(r, c, rt);
                if (n <= 5) {
                    const uint32_t ac = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8ACu));
                    std::fprintf(stderr, "[edo:frame] <- FUN_00108600 #%d pc=0x%X ac=%u\n", n, c->pc, ac); std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x001E9250u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001e9250\n"); std::fflush(stderr);
                FUN_001e9250_0x1e9250(r, c, rt);
                std::fprintf(stderr, "[edo:boot] <- FUN_001e9250 pc=0x%X\n", c->pc); std::fflush(stderr);
            });
            runtime.replaceFunction(0x0012FBE0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 10) {
                    std::fprintf(stderr, "[edo:cri] -> FUN_0012fbe0 (sleep main) #%d tid=%d\n", n, rt->eeScheduler().currentThreadId());
                    std::fflush(stderr);
                }
                FUN_0012fbe0_0x12fbe0(r, c, rt);
                if (n <= 10) {
                    const uint32_t ac = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8ACu));
                    std::fprintf(stderr, "[edo:cri] <- FUN_0012fbe0 #%d ac=%u pc=0x%X\n", n, ac, c->pc);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x0012FB60u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                const int id = ++n;
                if (id <= 6) {
                    const uint32_t ac = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8ACu));
                    const auto *t1 = rt->eeScheduler().thread(1);
                    const auto *t3 = rt->eeScheduler().thread(3);
                    std::fprintf(stderr, "[edo:cri] -> FUN_0012fb60 #%d tid=%d ac=%u p1=%d p3=%d\n",
                                 id, rt->eeScheduler().currentThreadId(), ac,
                                 t1 ? t1->currentPriority : -1, t3 ? t3->currentPriority : -1);
                    std::fflush(stderr);
                }
                FUN_0012fb60_0x12fb60(r, c, rt);
                if (id <= 6) {
                    const uint32_t ac = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8ACu));
                    std::fprintf(stderr, "[edo:cri] <- FUN_0012fb60 #%d ac=%u pc=0x%X\n", id, ac, c->pc);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x0012EC98u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 10 && rt->eeScheduler().currentThreadId() == 1) {
                    const uint32_t ac = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x3EE8ACu));
                    std::fprintf(stderr, "[edo:cri] -> FUN_0012ec98 (SleepThread) tid=1 ac=%u ra=0x%X\n", ac, getRegU32(c, 31));
                    std::fflush(stderr);
                }
                FUN_0012ec98_0x12ec98(r, c, rt);
            });
            runtime.replaceFunction(0x001E68E0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_001e68e0 (boot wait DONE!)\n"); std::fflush(stderr);
                FUN_001e68e0_0x1e68e0(r, c, rt);
            });
            runtime.replaceFunction(0x001E4310u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 5 || n % 60 == 0) {
                    std::fprintf(stderr, "[edo:state] FUN_001e4310 (state 2 intro/logo) #%d\n", n); std::fflush(stderr);
                }
                FUN_001e4310_0x1e4310(r, c, rt);
            });
            runtime.replaceFunction(0x001E4480u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                static int lastS0 = -999, lastS6 = -999;
                FUN_001e4480_0x1e4480(r, c, rt);
                const uint32_t obj = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617CC4u));
                const int32_t s0 = obj ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, obj)) : -1;
                const int32_t s6 = obj ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, obj + 0x18u)) : -1;
                if (++n <= 5 || n % 60 == 0 || s0 != lastS0 || s6 != lastS6) {
                    lastS0 = s0; lastS6 = s6;
                    const uint32_t o7 = obj ? *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, obj + 0x1Cu)) : 0;
                    const uint32_t ob = obj ? *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, obj + 0x2Cu)) : 0;
                    const uint32_t oc = obj ? *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, obj + 0x30u)) : 0;
                    std::fprintf(stderr, "[edo:state] FUN_001e4480 #%d s0=%d s6=%d o7=0x%X ob=0x%X oc=0x%X\n",
                                 n, s0, s6, o7, ob, oc);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x001EF8E0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int lastS8 = -999, lastSc = -999;
                FUN_001ef8e0_0x1ef8e0(r, c, rt);
                const uint32_t obj = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617EF0u - 0x1F0u));
                const int32_t s8 = obj ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, obj + 8u)) : -1;
                const int32_t sc = obj ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, obj + 12u)) : -1;
                if (s8 != lastS8 || sc != lastSc) {
                    lastS8 = s8; lastSc = sc;
                    std::fprintf(stderr, "[edo:state] FUN_001ef8e0 s8=%d sc=%d\n", s8, sc);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x001EFB70u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int lastSt = -999;
                FUN_001efb70_0x1efb70(r, c, rt);
                const uint32_t obj = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617EF0u - 0x1E4u));
                const int32_t st = obj ? *reinterpret_cast<const int32_t *>(getConstMemPtr(r, obj)) : -1;
                if (st != lastSt) {
                    lastSt = st;
                    std::fprintf(stderr, "[edo:state] FUN_001efb70 st=%d\n", st);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x00184D20u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:fade] FUN_00184d20 frames=%u col=0x%08X ra=0x%X\n",
                             getRegU32(c, 5), getRegU32(c, 6), getRegU32(c, 31));
                std::fflush(stderr);
                FUN_00184d20_0x184d20(r, c, rt);
            });
            runtime.replaceFunction(0x00184E60u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:fade] FUN_00184e60 frames=%u c0=0x%08X c1=0x%08X ra=0x%X\n",
                             getRegU32(c, 5), getRegU32(c, 6), getRegU32(c, 7), getRegU32(c, 31));
                std::fflush(stderr);
                FUN_00184e60_0x184e60(r, c, rt);
            });
            runtime.replaceFunction(0x001D5540u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 3 || n % 60 == 0) {
                    const uint32_t gp = 0x617EF0u;
                    const uint32_t pSound = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, gp - 0x294u));
                    const uint32_t pFdec = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, gp - 0x214u));
                    std::fprintf(stderr, "[edo:wait] FUN_001d5540 #%d ra=0x%X iGp_fd6c=0x%X psGp_fdec=0x%X\n",
                                 n, getRegU32(c, 31), pSound, pFdec);
                    std::fflush(stderr);
                }
                FUN_001d5540_0x1d5540(r, c, rt);
            });
            runtime.replaceFunction(0x001DFCC0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int last = -999;
                FUN_001dfcc0_0x1dfcc0(r, c, rt);
                int v = static_cast<int32_t>(getRegU32(c, 2));
                if (v != last) { last = v; std::fprintf(stderr, "[edo:wait] FUN_001dfcc0(sound) -> %d\n", v); std::fflush(stderr); }
            });
            runtime.replaceFunction(0x001C2130u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int last = -999;
                FUN_001c2130_0x1c2130(r, c, rt);
                int v = static_cast<int32_t>(getRegU32(c, 2));
                if (v != last) { last = v; std::fprintf(stderr, "[edo:wait] FUN_001c2130(cvm) -> %d\n", v); std::fflush(stderr); }
            });
            runtime.replaceFunction(0x001E6500u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int last = -999;
                static int16_t lastState = -999;
                const uint32_t obj = getRegU32(c, 4);
                FUN_001e6500_0x1e6500(r, c, rt);
                int v = static_cast<int32_t>(getRegU32(c, 2));
                int16_t st = obj ? *reinterpret_cast<const int16_t *>(getConstMemPtr(r, obj)) : -1;
                if (v != last || st != lastState) {
                    last = v; lastState = st;
                    const uint32_t gp4 = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617EF4u));
                    const uint32_t langObj = *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, 0x617EF0u - 0x210u));
                    const uint32_t spr1 = langObj ? *reinterpret_cast<const uint32_t *>(getConstMemPtr(r, langObj + 4u)) : 0u;
                    std::fprintf(stderr, "[edo:wait] FUN_001e6500(splash) state=%d ret=%d gp4=0x%X langObj=0x%X spr1=0x%X\n",
                                 st, v, gp4, langObj, spr1);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x001D1180u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 12) {
                    const uint32_t obj = getRegU32(c, 4);
                    const int32_t *p = reinterpret_cast<const int32_t *>(getConstMemPtr(r, obj));
                    std::fprintf(stderr, "[edo:spr] FUN_001d1180 #%d obj=0x%X p0=%d flags=0x%X cnt=%d buf=0x%X max=%d\n",
                                 n, obj, p ? p[0] : -1, p ? p[1] : 0, p ? p[8] : -1, p ? p[11] : 0, p ? p[9] : -1);
                    std::fflush(stderr);
                }
                FUN_001d1180_0x1d1180(r, c, rt);
            });
            runtime.replaceFunction(0x0010F1B0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                const bool log = (++n <= 3);
                const uint32_t a0 = getRegU32(c, 4);
                const uint32_t a1 = getRegU32(c, 5);
                const uint32_t a2 = getRegU32(c, 6);
                FUN_0010f1b0_0x10f1b0(r, c, rt);
                if (log) {
                    const float *in = reinterpret_cast<const float *>(getConstMemPtr(r, a2));
                    const float *m = reinterpret_cast<const float *>(getConstMemPtr(r, a0 + 0x1c0u));
                    const int32_t *out = reinterpret_cast<const int32_t *>(getConstMemPtr(r, a1));
                    std::fprintf(stderr, "[edo:proj] #%d a0=0x%X in=(%.2f,%.2f,%.2f,%.2f) out=(0x%X,0x%X,0x%X,0x%X)\n",
                                 n, a0, in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);
                    std::fprintf(stderr, "[edo:proj]   m0=(%.2f,%.2f,%.2f,%.2f) m1=(%.2f,%.2f,%.2f,%.2f) m3=(%.2f,%.2f,%.2f,%.2f)\n",
                                 m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[12], m[13], m[14], m[15]);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x00113730u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_00113730 (pad thread)\n"); std::fflush(stderr);
                FUN_00113730_0x113730(r, c, rt);
            });
            runtime.replaceFunction(0x0013E830u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 20) {
                    const uint8_t *s = getConstMemPtr(r, getRegU32(c, 4));
                    std::fprintf(stderr, "[edo:criErr] #%d msg=\"%s\" ra=0x%X\n",
                                 n, s ? reinterpret_cast<const char *>(s) : "(null)", getRegU32(c, 31));
                    std::fflush(stderr);
                }
                FUN_0013e830_0x13e830(r, c, rt);
            });
            runtime.replaceFunction(0x0011F9B0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                if (++n <= 20) {
                    const uint8_t *s1 = getConstMemPtr(r, getRegU32(c, 4));
                    const uint8_t *s2 = getConstMemPtr(r, getRegU32(c, 6));
                    std::fprintf(stderr, "[edo:rofsErr] #%d file=\"%s\" line=%u msg=\"%s\" code=%d ra=0x%X\n",
                                 n, s1 ? reinterpret_cast<const char *>(s1) : "(null)",
                                 getRegU32(c, 5), s2 ? reinterpret_cast<const char *>(s2) : "(null)",
                                 static_cast<int32_t>(getRegU32(c, 8)), getRegU32(c, 31));
                    std::fflush(stderr);
                }
                FUN_0011f9b0_0x11f9b0(r, c, rt);
            });
            runtime.replaceFunction(0x001402E0u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                const bool log = (++n <= 5);
                if (log) {
                    const uint8_t *s0 = getConstMemPtr(r, getRegU32(c, 4));
                    const uint8_t *s1 = getConstMemPtr(r, getRegU32(c, 5));
                    std::fprintf(stderr, "[edo:1402e0] -> #%d a0=\"%s\" a1=\"%s\" a2=0x%X a3=0x%X\n",
                                 n, s0 ? reinterpret_cast<const char *>(s0) : "",
                                 s1 ? reinterpret_cast<const char *>(s1) : "",
                                 getRegU32(c, 6), getRegU32(c, 7));
                    std::fflush(stderr);
                }
                FUN_001402e0_0x1402e0(r, c, rt);
                if (log) {
                    std::fprintf(stderr, "[edo:1402e0] <- #%d v0=%d pc=0x%X\n",
                                 n, static_cast<int32_t>(getRegU32(c, 2)), c->pc);
                    std::fflush(stderr);
                }
            });
            runtime.replaceFunction(0x00122E38u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                static int n = 0;
                const bool log = (++n <= 5);
                if (log) {
                    std::fprintf(stderr, "[edo:122e38] -> #%d a0=0x%X a1=%u a2=0x%X a3=0x%X\n",
                                 n, getRegU32(c, 4), getRegU32(c, 5), getRegU32(c, 6), getRegU32(c, 7));
                    std::fflush(stderr);
                }
                FUN_00122e38_0x122e38(r, c, rt);
                if (log) {
                    std::fprintf(stderr, "[edo:122e38] <- #%d v0=%d pc=0x%X\n",
                                 n, static_cast<int32_t>(getRegU32(c, 2)), c->pc);
                    std::fflush(stderr);
                }
            });
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Naruto Shippuden: Ultimate Ninja 5 (PAL)", "SLES_556.05", kEntry, kElfCrc32, apply);
