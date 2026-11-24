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
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef EDO_HAS_CTORS
extern void edoRegisterCtorFunctions(PS2Runtime &runtime);
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

        // Sobrescreve handlers genéricos com versões específicas/instrumentadas.
        runtime.replaceFunction(0x0015F068u, ttyWrite);
        runtime.replaceFunction(0x00162808u, sifBindRpcBlocking);
        runtime.replaceFunction(0x001629E8u, sifCallRpcTraced);
        runtime.replaceFunction(0x001733C0u, cdLayerSearchFile);

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
            runtime.replaceFunction(0x00113730u, [](uint8_t *r, R5900Context *c, PS2Runtime *rt) {
                std::fprintf(stderr, "[edo:boot] -> FUN_00113730 (pad thread)\n"); std::fflush(stderr);
                FUN_00113730_0x113730(r, c, rt);
            });
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Naruto Shippuden: Ultimate Ninja 5 (PAL)", "SLES_556.05", kEntry, kElfCrc32, apply);
