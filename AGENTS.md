# AGENTS.md — `edo-tensei`

Recompilação estática nativa de *Naruto Shippuden: Ultimate Ninja 5* (PAL `SLES_556.05`) sobre `PS2Recomp` + `paraLLEl-GS`.

## Build e Execução

- Compilar `edo-tensei` (`ninja` não está no `$PATH` padrão):
  ```bash
  /home/zathura/Projects/GhidraMCP/.venv/bin/ninja -C build edo-tensei
  ```
- **Regras de rebuild rápido:**
  - Edições em `runtime/overrides/*.cpp`, `runtime/debug/battle_debugger.{h,cpp}`, `runtime/parallel_gs/*.cpp` ou `ps2_debug_panel.cpp` recompilam em **~8–15s**.
  - Evite adicionar novos `.cpp` em `runtime/` sem necessidade (`CONFIGURE_DEPENDS` regera o CMake em ~3,5 min).
  - Evite alterar `external/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h` ou `ps2_runtime.h` para diagnósticos: ambos estão no PCH e disparam rebuild de 4496 TUs (~38 min).
  - Alterações definitivas em `external/PS2Recomp/` devem ser salvas em `patches/ps2recomp/`.

## Battle Debugger (CLI-first)

Para diagnosticar bugs de **câmera**, **colisão/física**, **matrizes**, **lutadores (`P1`/`P2`)**, **soft-float/matemática recompilada**, inspecionar **structs em RDRAM** ou validar correções antes/depois em lutas (`BTL.BIN`), use `tools/battle_debug.sh` e consulte [`docs/battle-debugger.md`](docs/battle-debugger.md) para a referência completa de comandos, flags, watchdogs e limitações.

```bash
# Captura automatizada da Luta Livre com trace de lutadores e auto-stop
tools/battle_debug.sh replay --trace-fighters --stop-after 95 --timeout 85

# Triagem ranqueada, inspeção de structs e filtro JSONL
tools/battle_debug.sh top
tools/battle_debug.sh snapshot --bad-only
tools/battle_debug.sh jsonl --cat MATH --limit 30

# Comparar antes vs. depois de um fix
tools/battle_debug.sh diff <reportA> <reportB>
```

No modo interativo (`tools/battle_debug.sh run --overlay-3d`), **`F2`** abre o painel ImGui do Battle Debugger (`F1` abre o Runtime Debugger).

## Diagnósticos Existentes

- [`docs/diagnostico-luta-livre.md`](docs/diagnostico-luta-livre.md): reprodução da Luta Livre (`tools/luta_livre.sh`), patches IOP/SPU2/MMI (`PHMADH`) e reserva de `sbrk`.
- [`docs/diagnostico-tela-preta-camera-batalha.md`](docs/diagnostico-tela-preta-camera-batalha.md): cadeia de explosão da câmera 3D na batalha (`ovl_btl_FUN_006e99f0` / `atan2` / soft-float `double`) e patches `0069`/`0070`.
