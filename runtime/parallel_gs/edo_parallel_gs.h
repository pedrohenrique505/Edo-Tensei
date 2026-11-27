#pragma once
// Sink paraLLEl-GS (Vulkan): espelha ou apresenta o fluxo GIF pós-arbitragem.
// Ativado com EDO_GS_BACKEND=gpu-shadow ou EDO_GS_BACKEND=gpu-present
// em build com -DEDO_ENABLE_PARALLEL_GS=ON.
#include "runtime/gs/gs_frontend.h"
#include <cstdint>
#include <vector>

class PS2Runtime;

namespace edo_parallel_gs
{
bool installIfRequested(PS2Runtime &runtime);
bool saveLatestFramePPM(const char *path);
} // namespace edo_parallel_gs
