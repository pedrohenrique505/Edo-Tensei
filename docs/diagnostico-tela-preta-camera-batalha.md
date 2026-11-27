# Diagnóstico: Tela Preta 3D durante a Batalha (Explosão da Câmera)

## Resumo Executivo

Durante uma partida no modo Luta Livre (`tools/luta_livre.sh reproduzir`), a renderização 3D (cenário e personagens) funciona corretamente no início da luta (inclusive após os fixes de VU1 `8287465` e `0971d37`), mas **fica completamente preta a partir do frame ~85–112 da batalha**, enquanto o HUD 2D, áudio e lógica de combate continuam normalmente.

A causa raiz já foi isolada até o **ponto exato**:

1. A função de câmera de batalha `ovl_btl_FUN_006e99f0` (aplicação de *camera shake* chamada por `ovl_btl_FUN_006e98f0` ← `ovl_btl_FUN_006e95f0`) chama `atan2` em software (`FUN_00170208` → `FUN_0016b270`) para calcular o ângulo `theta` da câmera (`atan2(dx, -dy)`).
2. Sempre que `y != 0` (caminho geral de `atan2`), `FUN_0016b270` retorna **`0xffffffffbfc8d24a`** (`-nan` em IEEE-754 `double`, com os 32 bits superiores `0xffffffff`), em vez do valor correto (`-2.67463`).
3. Em seguida, `__truncdfsf2` (`FUN_00172cc0`) converte esse `double` corrompido em **`theta = -inf` (`0xff800000`)**.
4. `sceVu0RotMatrixZ` (`FUN_00153170`) recebe `theta = -inf` e produz uma matriz de rotação com `m0 = (-inf, 0, 0, 0)` e `m1 = (0, -inf, 0, 0)`.
5. Enquanto não há *camera shake* (`sh = (0, 0, 0, 0)`), o erro fica mascarado. **No primeiro frame em que um golpe dispara *camera shake*** (`sh = (0, 0, 20, 0)` em `ovl_btl_FUN_006e9be0`, por volta do frame 85), `sceVu0ApplyMatrix` (`FUN_00152da8`) multiplica `m2 = (-inf, 0, 0, 0)` por `(0, 0, 20, 0)` e gera `out = (-nan, 0, 0, 0)`.
6. `ovl_btl_FUN_006e99f0` soma `out` à posição (`s0+0x30`) e duas vezes ao alvo (`s0+0x80`) da câmera:
   - `pos.x` vira `-3.403e+38` (`0xff7fffff`, `-FLT_MAX`)
   - `tgt.x` vira `-nan` (`0xffffffff`, `-PS2_FLOAT_MAX_BITS`)
   - `y` e `z` permanecem normais.
7. `FUN_0019f990` → `sceVu0CameraMatrix` (`0x001533e0`) recebe esses valores corrompidos e colapsa a matriz 4x4 enviada à VU1 (`vuData[0..64]`) para:
   `(0,0,0,0), (0,0,0,0), (0,0,0,0), (0,0,8.389e+06,0)` — projetando toda a geometria 3D em `(0, 0)` (tela 3D preta).

---

## Cadeia Completa de Chamadas e Evidências

### 1. Cadeia de atualização da câmera no overlay BTL

```text
ovl_btl_FUN_006e95f0(s2 = 0x00d68560)
 ├─ s2->vtable->update(s2)  [vtbl=0x005eb0d0, fn=0x006ea5a0 (ovl_btl_FUN_006ea5a0)]
 │   ├─ ovl_btl_FUN_006eb3a0(s1, s0)
 │   ├─ ovl_btl_FUN_006ebc00(s1, s0)
 │   ├─ ovl_btl_FUN_006ec300(s1, s0)   // atualiza pos (s1+0x30) — valores normais
 │   └─ ovl_btl_FUN_006ec610(s1, s0)   // atualiza tgt (s1+0x80) — valores normais
 ├─ ovl_btl_FUN_006e9730(s2)
 ├─ ovl_btl_FUN_006e98f0(s2)
 │   ├─ ovl_btl_FUN_006e9be0(s2)       // calcula shake vec (s2+0x100) e shake mat (s2+0x110) — valores normais
 │   └─ ovl_btl_FUN_006e99f0(s2)       // APLICA SHAKE -> CORROMPE pos.x e tgt.x aqui!
 │       ├─ FUN_00172108 (__extendsfdf2)  // OK: y=-268.328 [0xc070c54040000000], x=-532.24 [0xc080a1ec20000000]
 │       ├─ FUN_00170208 -> FUN_0016b270 (atan2)  // BUG: retorna 0xffffffffbfc8d24a em vez de -2.67463
 │       ├─ FUN_00172cc0 (__truncdfsf2)           // converte para -inf [0xff800000]
 │       ├─ FUN_00153170 (sceVu0RotMatrixZ)       // produz m0=(-inf,0,0,0), m1=(0,-inf,0,0)
 │       ├─ FUN_00152dd8 (sceVu0MulMatrix)        // multiplica pela matriz de shake (rotY 90°)
 │       └─ FUN_00152da8 (sceVu0ApplyMatrix)      // aplica ao shake vec (0,0,20,0) -> out=(-nan,0,0,0)
 └─ FUN_0019f990(cam, pos, tgt) -> sceVu0CameraMatrix (0x001533e0)
```

### 2. Logs exatos capturados

Do trace de `atan2` e `sceVu0RotMatrixZ` (`.cache/match-diag/cam-step5.log` / `cam-fixed.log`):

```text
[DEBUG-cam:atan2] #1 y=-268.328[0xc070c54040000000] x=-532.24[0xc080a1ec20000000] -> res=-nan[0xffffffffbfc8d24a] (host=-2.67463)
[DEBUG-cam:153170] #1 theta=-inf[0xff800000] m0=(-inf,0,0,0) m1=(0,-inf,0,0)
[DEBUG-cam:atan2] #2 y=0[0x0000000000000000] x=-0[0x8000000000000000] -> res=3.14159[0x400921fb54442d18] (host=3.14159)
[DEBUG-cam:153170] #2 theta=3.14159[0x40490fda] m0=(-1,0,0,0) m1=(0,-1,0,0)
```

No momento exato da transição para tela preta (frame `n=85`):

```text
[DEBUG-cam:6e9be0] s0=0x00d68560 bad=0 idx=-1 amp=20 ang=16384 sh=(0,0,20,0) m0=(0,0,-1,0) m2=(1,0,0,0)
[DEBUG-cam:152da8] in=(0,0,20,0) out=(-nan,0,0,0) m2=(-inf,0,0,0)
[DEBUG-cam:6e99f0] TRANSITION s0=0x00d68560 b30=(-298.838,-545.708,107.987) a30=(-3.403e+38,-545.708,107.987) b80=(-547.772,0.000,116.230) a80=(-nan,0.000,116.230) sh=(0,0,20,0)
[DEBUG-cam] TRANSITION TO ZERO #1 pc=0x0050
[DEBUG-cam]   prev: (691.6,0,0,0) (-1.907e-06,-806.8,0,0) (-2048,-2048,8,-1) (6.341e+05,1.745e+05,8.388e+06,33.07)
[DEBUG-cam]   curr: (0,0,0,0) (0,0,0,0) (0,0,0,0) (0,0,8.389e+06,0)
```

Observação importante:
- Quando `y == 0`, `FUN_0016b270` usa o caminho rápido (lê constante `+pi` de `0x5C4528`) e retorna `0x400921fb54442d18` corretamente.
- Quando `y != 0` (`y = -268.328`, `x = -532.24`), `FUN_0016b270` executa o caminho geral:
  1. `FUN_00172888` (`__divdf3`) em `0x16b4b8`
  2. `FUN_0016f4b0` (`fabs`) em `0x16b4c4`
  3. `FUN_0016ee78` (`atan`) em `0x16b4d0` (chama `__muldf3` `0x172600`, `__adddf3` `0x172540`, `__subdf3` `0x172598`, `__divdf3` `0x172888`, `0x172ae8`)
  4. `FUN_00172598` (`__subdf3`) em `0x16b580` e `0x16b594` (com `pi_lo` em `0x5C4590` e `pi` em `0x5C4598`, ambos verificados no Ghidra como corretos).

---

## Bugs Reais Encontrados no PS2Recomp Durante a Investigação

Mesmo sem terem eliminado o `0xffffffffbfc8d24a` sozinhos, dois bugs reais de semântica PS2 foram identificados e têm patches prontos (ainda não commitados):

1. **`patches/ps2recomp/0069-ps2xRuntime-ps2-float-compare-semantics.patch`** (já aplicado em `external/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h` e compilado nos 4496 TUs):
   - Substitui `FPU_C_EQ_S`/`FPU_C_OLT_S`/`FPU_C_OLE_S` por `ps2_feq`/`ps2_flt`/`ps2_fle` para que floats PS2 com expoente 255 (`±PS2_FLOAT_MAX_BITS = 0x7FFFFFFF / 0xFFFFFFFF`) sejam comparados como finitos em vez de `NaN` IEEE-754.
2. **`patches/ps2recomp/0070-ps2xRecomp-64bit-sign-branch-conditions.patch`** (aplicado em `external/PS2Recomp/ps2xRecomp/src/lib/control_flow_emitter.cpp` e nos 207 arquivos `generated/FUN_0016*.cpp` e `generated/FUN_0017*.cpp`):
   - `BLTZ`/`BLTZL`/`BGEZ`/`BGEZL`/`BLEZ`/`BLEZL`/`BGTZ`/`BGTZL` estavam emitindo `GPR_S32(ctx, rs) < 0` (testando bit 31) em vez de `GPR_S64(ctx, rs) < 0` (testando bit 63, como manda a ISA MIPS III / R5900).
   - Isso afetava diretamente `_fpadd_parts` (`FUN_001722e0` em `0x172464`: `bltzl $v1` sobre uma diferença de mantissas de 64 bits).

---

## O Que Falta Investigar (Próximo Passo Exato)

O valor retornado por `FUN_0016b270` (`0xffffffffbfc8d24a`) tem padrão clássico de **valor 32 bits negativo sign-extended para 64 bits** (`0xffffffff` nos 32 bits superiores).

### Passo 1: Isolar qual sub-função dentro de `FUN_0016b270` produz `0xffffffff...`
Na primeira chamada de `debugCam_170208` (ou em um teste unitário/hook rápido), logar o retorno `$v0` de cada passo dentro de `FUN_0016b270` para `(y=0xc070c54040000000, x=0xc080a1ec20000000)`:
- `0x172888` (`__divdf3`): esperado `~0.5041485` (`0x3fe021fc...`)
- `0x16f4b0` (`fabs`): esperado `~0.5041485`
- `0x16ee78` (`atan`): esperado `~0.466963` (`0x3fdddf8...`)
  - Dentro de `FUN_0016ee78`, verificar especialmente **`0x1702b0`** (chamado por `FUN_00172600` `__muldf3` em `0x17274c`, possivelmente `__muldi3` / `dmultu`!), **`0x172ae8`**, e qualquer instrução `SET_GPR_S32` / `addu` / `subu` / `sll` / `srl` / `multu` usada em variáveis de 64 bits!
- `0x172598` (`__subdf3`): esperado `-2.67463` (`0xc00565a8...`)

**Suspeito nº 1 imediato**: `FUN_001702b0` (chamado por `FUN_00172600` `__muldf3`!) ou alguma instrução 64-bit (`dmultu`, `mflo`/`mfhi`, `dsllv`/`dsrlv`, etc.) em `PS2Recomp` que esteja truncando/sign-extending para 32 bits!

### Passo 2: Corrigir a causa raiz no `PS2Recomp` (e/ou override temporário)
- Corrigir o codegen em `external/PS2Recomp/ps2xRecomp/` e gerar o patch `0071-...`.
- Recompilar apenas os arquivos afetados (`generated/FUN_0016*.cpp`, `generated/FUN_0017*.cpp`, etc.) para validar em ~15s.

### Passo 3: Limpeza e Verificação Final
1. **Remover toda instrumentação temporária (`[DEBUG-prof]` e `[DEBUG-cam]`)** atualmente no working tree:
   - `runtime/overrides/edo_overrides.cpp`: hooks `debugCameraLookAt_0019f990`, `debugCamStep`, `debugCam_6eb3a0`, `debugCam_6ebc00`, `debugCam_6ec300`, `debugCam_6ec610`, `debugCam_6e9be0`, `debugCam_6e99f0`, `debugCam_153170`, `debugCam_152da8`, `debugCam_170208`.
   - `runtime/parallel_gs/edo_parallel_gs.cpp`: `[DEBUG-prof:gs]`, `m_profEnabled`, `m_profGifNs`.
   - `external/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp`: `[DEBUG-prof:vu1]`, `[DEBUG-prof:vudata]`, `[DEBUG-cam]`, `EDO_PROF_MATCH`, `EDO_DUMP_VU1_DIR`, `#include <unordered_set>`.
2. Recompilar com `/home/zathura/Projects/GhidraMCP/.venv/bin/ninja -C build edo-tensei`.
3. Rodar `EDO_DUMP_FRAME=.cache/match-diag/final.ppm EDO_DUMP_FRAME_NUM=3400 timeout -s INT 72s tools/luta_livre.sh reproduzir`, inspecionar o PNG resultante e validar FPS + cena 3D intacta.

---

## Comandos Úteis e Armadilhas do Ambiente

- **Compilar `edo-tensei`** (`cmake`/`ninja` não estão no `$PATH` padrão):
  ```bash
  /home/zathura/Projects/GhidraMCP/.venv/bin/ninja -C build edo-tensei
  ```
  - Editar `runtime/overrides/edo_overrides.cpp` ou `ps2_vu1_core.cpp`: rebuild leva **~8–13s**.
  - Editar `generated/FUN_0016*.cpp` / `FUN_0017*.cpp`: rebuild leva **~15s**.
  - **CUIDADO**: editar `external/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h` invalida o PCH e dispara um rebuild completo de 4496 TUs (~38 min). Evitar mexer nesse header sem necessidade.
- **Reproduzir a batalha e capturar frame headless** (independe de foco da janela no Hyprland):
  ```bash
  EDO_PROF_MATCH=1 EDO_DUMP_FRAME=.cache/match-diag/check.ppm EDO_DUMP_FRAME_NUM=3400 \
    timeout -s INT 72s tools/luta_livre.sh reproduzir > .cache/match-diag/run.log 2>&1
  ```
  - **CUIDADO**: `.cache/luta-livre/pad.txt` reproduz inputs por tempo de relógio (*wall-clock*). Tracing pesado por frame (`EDO_TRACE_SCANOUT=1`, `PS2X_PERF`) derruba o FPS nos menus e faz o replay errar o menu da Luta Livre. `EDO_PROF_MATCH=1` tem overhead desprezível e entra na luta de forma confiável.
- **Ghidra MCP**: o binário `SLES_556.05` (MIPS-R5900) já está carregado e pronto para consultas (`decompile_function`, `read_memory`, etc.).

---

## Estado Atual do Repositório

### Commits já feitos nesta sessão
- `2e1dfe3` — `fix(overrides): registrar entrypoint ausente 0x00726550 do overlay BTL` (resolveu freeze imediato no `timer=99`)
- `8287465` — `fix(vu1): respeitar delaysNextBranchRead no loop rapido VU1 (0067)` (resolveu cabeças/braços/pernas pretos dos personagens)
- `0971d37` — `perf(vu1): suportar opcodes de batalha, janela need_mac, FTZ/DAZ e dispatch por prefixHash (0068)` (subiu FPS de 6–15 FPS para 26–60 FPS)

### Alterações não commitadas no working tree
- `patches/ps2recomp/0069-ps2xRuntime-ps2-float-compare-semantics.patch` (aplicado e compilado em `external/PS2Recomp`)
- `patches/ps2recomp/0070-ps2xRecomp-64bit-sign-branch-conditions.patch` (aplicado em `external/PS2Recomp` e em `generated/FUN_0016*.cpp` / `FUN_0017*.cpp`)
- `runtime/overrides/edo_overrides.cpp` — apenas hooks temporários `DEBUG-cam`
- `runtime/parallel_gs/edo_parallel_gs.cpp` — apenas probes temporários `DEBUG-prof:gs`
- `external/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp` — patches `0067`/`0068` + probes temporários `DEBUG-prof`/`DEBUG-cam`
