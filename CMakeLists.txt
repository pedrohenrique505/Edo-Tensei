cmake_minimum_required(VERSION 3.21)

# Build nativo de Naruto Shippuden: Ultimate Ninja 5 (SLES_556.05, PAL v1.00).
#
# Usa o runtime do PS2Recomp (external/PS2Recomp) como biblioteca e compila o
# código gerado (generated/) em targets separados, para iterar rápido:
#   edo_generated   funções recompiladas (unity build + PCH, -O2)
#   edo-tensei      executável: tabela de funções (-O0), overrides e main

project(EdoTenseiRecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/PS2Recomp" CACHE PATH "Clone do PS2Recomp")
set(EDO_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "Saída do ps2_recomp")
set(EDO_UNITY_BATCH 32 CACHE STRING "Funções por TU no unity build do código gerado")

if(NOT EXISTS "${PS2RECOMP_DIR}/ps2xRuntime/CMakeLists.txt")
    message(FATAL_ERROR "PS2Recomp não encontrado em ${PS2RECOMP_DIR}")
endif()
if(NOT EXISTS "${EDO_GENERATED_DIR}/register_functions.cpp")
    message(FATAL_ERROR "Código gerado não encontrado em ${EDO_GENERATED_DIR}: rode o ps2_recomp antes")
endif()

# GCC/Clang no x86-64 precisam habilitar SSE4.1/AVX2 explicitamente (PS2Recomp#261).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT MSVC)
    add_compile_options(-march=x86-64-v3)
endif()

option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" OFF)
set(PS2X_IS_ARM_TARGET OFF)
add_subdirectory("${PS2RECOMP_DIR}/ps2xIOP" ps2xIOP EXCLUDE_FROM_ALL)
add_subdirectory("${PS2RECOMP_DIR}/ps2xRuntime" ps2xRuntime EXCLUDE_FROM_ALL)

set(RT_DIR "${PS2RECOMP_DIR}/ps2xRuntime")

# ---------------------------------------------------------------------------
# Código recompilado (sem register_functions.cpp)
# ---------------------------------------------------------------------------
file(GLOB EDO_GENERATED_SOURCES "${EDO_GENERATED_DIR}/*.cpp")
list(REMOVE_ITEM EDO_GENERATED_SOURCES
    "${EDO_GENERATED_DIR}/register_functions.cpp"
    "${EDO_GENERATED_DIR}/vu1_recompiled.cpp"
)
list(SORT EDO_GENERATED_SOURCES)

add_library(edo_generated STATIC ${EDO_GENERATED_SOURCES})
target_include_directories(edo_generated PUBLIC "${EDO_GENERATED_DIR}")
target_link_libraries(edo_generated PUBLIC ps2_runtime)
set_target_properties(edo_generated PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE "${EDO_UNITY_BATCH}"
)
# Funções geradas gigantes (>4 MB cada) explodem o tempo do otimizador em -O2.
foreach(_big_fn
    FUN_00130560_0x130560.cpp
    FUN_00133ec8_0x133ec8.cpp
    FUN_00133ee0_0x133ee0.cpp
    FUN_00134080_0x134080.cpp
    FUN_00134068_0x134068.cpp
)
    if(EXISTS "${EDO_GENERATED_DIR}/${_big_fn}")
        set_source_files_properties("${EDO_GENERATED_DIR}/${_big_fn}" PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION ON
            COMPILE_OPTIONS "-O0"
        )
    endif()
endforeach()
target_precompile_headers(edo_generated PRIVATE
    "${RT_DIR}/include/ps2_runtime_macros.h"
    "${RT_DIR}/include/ps2_runtime.h"
    "${EDO_GENERATED_DIR}/ps2_recompiled_functions.h"
    "${EDO_GENERATED_DIR}/ps2_recompiled_stubs.h"
    "${RT_DIR}/include/ps2_syscalls.h"
    "${RT_DIR}/include/ps2_stubs.h"
)

# ---------------------------------------------------------------------------
# Executável
# ---------------------------------------------------------------------------
file(GLOB EDO_OVERRIDE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/overrides/*.cpp")
file(GLOB EDO_DEBUG_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/debug/*.cpp")

add_executable(edo-tensei
    "${RT_DIR}/src/main.cpp"
    "${EDO_GENERATED_DIR}/register_functions.cpp"
    ${EDO_OVERRIDE_SOURCES}
    ${EDO_DEBUG_SOURCES}
)
target_link_libraries(edo-tensei PRIVATE ps2_runtime edo_generated)
target_include_directories(edo-tensei PRIVATE
    "${RT_DIR}/src/lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/debug"
)

# Construtores estáticos C++ (.ctors em 0x005E3A00..0x005E6840), gerados separadamente
# para não precisar recompilar edo_generated inteiro.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated_ctors/register_ctors.cpp")
    file(GLOB EDO_CTOR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/generated_ctors/*.cpp")
    add_library(edo_ctors STATIC ${EDO_CTOR_SOURCES})
    target_include_directories(edo_ctors PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/generated_ctors"
        "${EDO_GENERATED_DIR}"
    )
    target_link_libraries(edo_ctors PUBLIC ps2_runtime)
    set_target_properties(edo_ctors PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 64)
    target_link_libraries(edo-tensei PRIVATE edo_ctors)
    target_compile_definitions(edo-tensei PRIVATE EDO_HAS_CTORS=1)
endif()

# Overlays dinâmicos (PRG/BTL.BIN, PRG/ADV.BIN, PRG/ETC.BIN em 0x006C6D00).
# Cada overlay é compilado em sua própria biblioteca estática com símbolos prefixados
# e tabela de registro própria (register_ovl_<nome>.cpp) para troca dinâmica em runtime.
foreach(_ovl btl adv etc)
    set(_ovl_dir "${CMAKE_CURRENT_SOURCE_DIR}/generated_ovl_${_ovl}")
    set(_ovl_reg "${_ovl_dir}/register_ovl_${_ovl}.cpp")
    if(EXISTS "${_ovl_reg}")
        file(GLOB _ovl_sources "${_ovl_dir}/*.cpp")
        list(REMOVE_ITEM _ovl_sources "${_ovl_reg}")
        list(SORT _ovl_sources)
        add_library(edo_ovl_${_ovl} STATIC ${_ovl_sources} "${_ovl_reg}")
        target_include_directories(edo_ovl_${_ovl} PRIVATE "${_ovl_dir}")
        target_link_libraries(edo_ovl_${_ovl} PUBLIC ps2_runtime)
        set_target_properties(edo_ovl_${_ovl} PROPERTIES
            UNITY_BUILD ON
            UNITY_BUILD_BATCH_SIZE "${EDO_UNITY_BATCH}"
        )
        set_source_files_properties("${_ovl_reg}" PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION ON
            COMPILE_OPTIONS "-O0"
        )
        # Funções geradas gigantes (>1.5 MB) explodem o otimizador em -O2.
        foreach(_src ${_ovl_sources})
            file(SIZE "${_src}" _src_size)
            if(_src_size GREATER 1500000)
                set_source_files_properties("${_src}" PROPERTIES
                    SKIP_UNITY_BUILD_INCLUSION ON
                    COMPILE_OPTIONS "-O0"
                )
            endif()
        endforeach()
        target_precompile_headers(edo_ovl_${_ovl} PRIVATE
            "${RT_DIR}/include/ps2_runtime_macros.h"
            "${RT_DIR}/include/ps2_runtime.h"
            "${_ovl_dir}/ps2_recompiled_functions.h"
            "${_ovl_dir}/ps2_recompiled_stubs.h"
            "${RT_DIR}/include/ps2_syscalls.h"
            "${RT_DIR}/include/ps2_stubs.h"
        )
        string(TOUPPER "${_ovl}" _ovl_up)
        target_link_libraries(edo-tensei PRIVATE edo_ovl_${_ovl})
        target_compile_definitions(edo-tensei PRIVATE "EDO_HAS_OVL_${_ovl_up}=1")
    endif()
endforeach()

# A tabela de funções é um construtor grande; -O0 evita minutos de compilação.
set_source_files_properties("${EDO_GENERATED_DIR}/register_functions.cpp"
    PROPERTIES COMPILE_OPTIONS "-O0")

# paraLLEl-GS (Vulkan) para apresentação em GPU com FPS jogável.
option(EDO_ENABLE_PARALLEL_GS "Ativar sink Vulkan paraLLEl-GS (EDO_GS_BACKEND=gpu-present)" ON)
if(EDO_ENABLE_PARALLEL_GS)
    set(EDO_PARALLEL_GS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/.cache/parallel-gs-source"
        CACHE PATH "Checkout pinado do paraLLEl-GS")
    if(NOT EXISTS "${EDO_PARALLEL_GS_SOURCE}/gs/gs_interface.hpp")
        message(FATAL_ERROR "paraLLEl-GS não encontrado em ${EDO_PARALLEL_GS_SOURCE}")
    endif()
    set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
    set(GRANITE_AUDIO OFF CACHE BOOL "" FORCE)
    add_subdirectory("${EDO_PARALLEL_GS_SOURCE}" parallel-gs EXCLUDE_FROM_ALL)
    target_sources(edo-tensei PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/parallel_gs/edo_parallel_gs.cpp")
    target_include_directories(edo-tensei PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/parallel_gs")
    target_link_libraries(edo-tensei PRIVATE parallel-gs)
    target_compile_definitions(edo-tensei PRIVATE EDO_HAS_PARALLEL_GS=1)
endif()

if(PS2X_ENABLE_DEBUG_UI)
    target_sources(edo-tensei PRIVATE "${RT_DIR}/src/lib/ps2_debug_panel.cpp")
    target_link_libraries(edo-tensei PRIVATE rlImGui imgui)
    target_compile_definitions(edo-tensei PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

if(EXISTS "${EDO_GENERATED_DIR}/vu1_recompiled.cpp")
    set_source_files_properties("${EDO_GENERATED_DIR}/vu1_recompiled.cpp"
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    target_sources(edo-tensei PRIVATE "${EDO_GENERATED_DIR}/vu1_recompiled.cpp")
endif()
