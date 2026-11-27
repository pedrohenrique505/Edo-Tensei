# Battle Debugger (`edo-tensei`)

Ferramenta de diagnóstico em tempo real e via CLI para investigar bugs de **câmera**, **colisão/física**, **matrizes** e **matemática recompilada** durante lutas (*overlay* `PRG/BTL.BIN`) em *Naruto Shippuden: Ultimate Ninja 5* (`SLES_556.05`).

Foi projetada com foco **CLI-first** para facilitar o uso por agentes de IA e scripts (saída estruturada em `report.txt`, stream `events.jsonl`, dump de structs em `snapshot.txt`, auto-stop e `diff` antes/depois de correções), além de oferecer um painel visual **ImGui (`F2`)** com overlay 3D→2D para inspeção manual.

---

## Onde está no código

| Arquivo | Papel |
| --- | --- |
| `runtime/debug/battle_debugger.h` | Estruturas de snapshot (`CameraSnapshot`, `FighterSnapshot`, `AnomalyEvent`, `TraceEvent`) e interface `edo_debug::BattleDebugger` |
| `runtime/debug/battle_debugger.cpp` | Núcleo de watchdogs, guarda de preempção (`ctx->pc == ra`), mini-callstack, comparador diferencial + modo A/B `--host-math`, *ring buffer*, escritor JSONL, dumper de structs/screenshots e painel ImGui (`F2`) + overlay 3D→2D |
| `runtime/parallel_gs/edo_parallel_gs.{h,cpp}` | `edo_parallel_gs::saveLatestFramePPM` usado por `--screenshot-on-anomaly` e pelo botão `Screenshot` do painel `F2` |
| `runtime/overrides/edo_overrides.cpp` | Inicialização (`initFromEnv`, `installCoreHooks`) e ciclo de vida do overlay `BTL` (`onBtlOverlayActivated` / `onBtlOverlayDeactivated`) |
| `tools/battle_debug.sh` | Interface CLI unificada (`replay`, `run`, `summary`, `top`, `snapshot`, `jsonl`, `diff`, `check`) |
| `patches/ps2recomp/0071-ps2xRuntime-debug-panel-extra-ui-hook.patch` | Hook mínimo via símbolos fracos em `ps2_debug_panel.cpp` para compartilhar o frame `rlImGuiBegin/End` com a janela `F2` |

---

## Fluxo rápido via CLI (recomendado para agentes)

```bash
# 1. Capturar uma luta com trace de P1/P2, screenshots e auto-stop (25 frames reais após a 1ª anomalia)
tools/battle_debug.sh replay --trace-fighters --screenshot-on-anomaly --stop-after 25 --timeout 85

# 2. Triagem imediata ranqueada por contagem de ocorrências
tools/battle_debug.sh top

# 3. Inspecionar structs (Camera, P1, P2) ou apenas campos marcados como BAD
tools/battle_debug.sh snapshot --bad-only

# 4. Filtrar eventos cronológicos em JSONL por categoria
tools/battle_debug.sh jsonl --cat CAMERA --limit 30
tools/battle_debug.sh jsonl --cat COLLISION --limit 30
tools/battle_debug.sh jsonl --cat MATH --limit 30

# 5. Isolamento A/B (--host-math) e validação antes vs. depois
tools/battle_debug.sh replay --host-math --stop-after 25 --timeout 85 --out-dir .cache/battle-debug-host
tools/battle_debug.sh diff .cache/battle-debug/report.txt .cache/battle-debug-host/report.txt

# 6. Gate de regressão automatizado (retorna exit 0 PASS ou exit 1 FAIL)
tools/battle_debug.sh check --baseline .cache/battle-debug/report.txt --stop-after 25 --timeout 85
```

---

## Referência de Comandos (`tools/battle_debug.sh`)

### `replay [opções]`
Executa a captura automatizada de Luta Livre (`tools/luta_livre.sh reproduzir`) com o Battle Debugger armado, grava os artefatos no diretório de saída (padrão `.cache/battle-debug/`) e exibe o cabeçalho do `report.txt` ao encerrar.

Artefatos gerados:
- `report.txt` — resumo geral (`battleFrame`, `totalAnomalies`, `mathChecks`, `mathMismatches`, `preemptedSkips`, `hostMathOverrides`), `FIRST ANOMALY`, estado atual e último estado bom da câmera, estado de `P1`/`P2`, lista de anomalias únicas deduplicadas (com `stack=`) e últimos eventos do *ring buffer*.
- `events.jsonl` — stream JSON Lines (1 objeto JSON por linha, *flushed* imediatamente) contendo anomalias (primeiras 5 ocorrências de cada ponto, incluindo `"stack"`) e traces ativados.
- `snapshot.txt` — dump em hex (`u32`) + `float` das structs `CAMERA_OBJ`, `FIGHTER_P1`, `FIGHTER_P2` e faixas extras de `--watch`, sinalizando `<<< BAD` em valores corrompidos.
- `screenshots/` *(quando `--screenshot-on-anomaly` está ativo)* — quadros PPM (`first_anomaly_fN.ppm`, `visual_anomaly_fN.ppm`, `final_fN.ppm`) capturados diretamente do `paraLLEl-GS`.
- `run.log` — saída completa (`stdout` + `stderr`) do processo.

### `run [opções]`
Inicia o jogo interativamente (`tools/run_edo.sh`) com o Battle Debugger ativo. Pressione **`F2`** para abrir/fechar o painel **Battle Debugger** (independente do `Runtime Debugger` em `F1`).

### `summary [report.txt]`
Imprime o conteúdo completo do `report.txt` (padrão: `.cache/battle-debug/report.txt`).

### `top [report.txt]`
Faz o parse da seção `ANOMALIES` e imprime uma tabela ranqueada por número de ocorrências (`COUNT`, `FRAME`, `CAT/SEV`, `FUNCTION`, `CALLER_RA`, `DETAIL`) para identificar rapidamente os maiores focos de erro.

### `snapshot [--bad-only] [snapshot.txt]`
Exibe o dump de structs/memória capturado em `snapshot.txt`. Com `--bad-only`, mostra apenas cabeçalhos de seção e linhas contendo `<<< BAD`.

### `jsonl [--cat CATEGORIA] [--limit N] [events.jsonl]`
Exibe linhas do stream `events.jsonl`.
- `--cat`: filtra por `CAMERA`, `MATH`, `MATRIX`, `COLLISION` ou `GENERAL`.
- `--limit`: quantidade máxima de linhas (padrão: `50`).

### `diff <reportA> <reportB>`
Compara dois arquivos `report.txt` (por exemplo, antes e depois de aplicar um patch):
- Mostra o delta de `totalAnomalies`, `mathMismatches` e `mathChecks`.
- Lista pontos de anomalia **resolvidos** (`RESOLVED sites`, presentes em A e ausentes em B).
- Lista pontos de anomalia **novos** (`NEW sites`, ausentes em A e presentes em B).

### `check [--baseline reportA.txt] [--max-anomalies N] [opções de replay]`
Gate de regressão automatizado: executa o `replay` e retorna `exit 0` (`PASS`) ou `exit 1` (`FAIL`):
- Com `--baseline`: falha se surgir qualquer novo ponto de anomalia ou se `totalAnomalies` aumentar em relação ao baseline.
- Sem `--baseline`: falha se `totalAnomalies > N` (padrão `N=0`).

---

## Opções de `replay` / `run` e Variáveis de Ambiente

| Opção CLI | Variável de Ambiente | Efeito |
| --- | --- | --- |
| *(sempre ativo no script)* | `EDO_BATTLE_DEBUG=1` | Arma todos os watchdogs do debugger (sem custo significativo quando `0`). |
| `--out-dir DIR` | `EDO_BATTLE_DUMP`, `EDO_BATTLE_JSONL`, `EDO_BATTLE_SNAPSHOT` | Define o diretório onde `report.txt`, `events.jsonl`, `snapshot.txt` e `run.log` serão gravados. |
| `--timeout SEC` | *(timeout externo)* | Limite de tempo em segundos no modo `replay` (padrão: `75`). |
| `--stop-after N` | `EDO_BATTLE_STOP_AFTER=N` | Grava `report.txt` + `snapshot.txt` e solicita encerramento limpo (`requestStop`) `N` frames de batalha após a `FIRST ANOMALY`. |
| `--max-frames N` | `EDO_BATTLE_MAX_FRAMES=N` | Encerra automaticamente após `N` frames de batalha, mesmo sem anomalias. |
| `--snapshot-frame N` | `EDO_BATTLE_SNAPSHOT_FRAME=N` | Captura `snapshot.txt` exatamente no frame de batalha `N`. Se `0`, captura no primeiro frame com `P1`/`P2` após a primeira anomalia (e novamente no auto-stop). |
| `--watch SPEC` | `EDO_BATTLE_WATCH_MEM=SPEC` | Lista separada por vírgula de faixas `0xADDR:SIZE` da RDRAM para incluir no `snapshot.txt` (ex.: `0x00617EF0:0x40,0x00D68560:0x100`, máx. `0x1000` bytes por faixa). |
| `--pad FILE` | `DRA2_PAD_REPLAY` | Arquivo de captura de controle usado no `replay` (padrão: `.cache/luta-livre/pad.txt`). |
| `--no-math` | `EDO_BATTLE_MATH_CHECK=0` | Desativa o comparador diferencial de matemática (útil para isolar apenas câmera/colisão com overhead mínimo). |
| `--host-math` | `EDO_BATTLE_HOST_MATH=1` | **Modo A/B:** mantém o registro de divergências, mas substitui o retorno das 8 funções soft-float `double` divergentes pelo valor IEEE-754 do host para isolar se bugs de câmera/colisão somem quando o soft-float está íntegro. |
| `--break` | `EDO_BATTLE_BREAK=1` | Pausa automaticamente o loop da batalha ao registrar qualquer anomalia `ERROR` ou `CRIT` (use com a janela `F2` para `Resume` / `Step +1`). |
| `--trace-camera` | `EDO_BATTLE_TRACE_CAMERA=1` | Registra `pos`, `tgt` e `shake` da câmera por frame no *ring buffer* e no `events.jsonl`. |
| `--trace-fighters` | `EDO_BATTLE_TRACE_FIGHTERS=1` | Registra `P1`, `P2`, `delta` e `distP1P2` por frame no *ring buffer* e no `events.jsonl`. |
| `--overlay-3d` | `EDO_BATTLE_OVERLAY_3D=1` | Ativa o overlay 3D→2D sobre o viewport desde o início (pode ser alternado também no painel `F2`). |
| `--screenshot-on-anomaly` | `EDO_BATTLE_SCREENSHOT_DIR` | Salva quadros PPM (`first_anomaly_fN.ppm`, `visual_anomaly_fN.ppm` e `final_fN.ppm`) via `paraLLEl-GS` em `<out-dir>/screenshots/`. |
| `--quiet` | `EDO_BATTLE_STDERR=0` | Suprime o eco das anomalias no `stderr`, gravando apenas em `report.txt` e `events.jsonl`. |
| *(apenas env)* | `EDO_BATTLE_UI=1` | Abre a janela ImGui `Battle Debugger [F2]` automaticamente ao iniciar. |

---

## O que o Debugger monitora

### 1. Câmera (`CAMERA` / `MATRIX`)
- `ovl_btl_FUN_006e95f0` (`0x006E95F0`): atualização de câmera do overlay `BTL`. Detecta transição de estado válido para `NaN`/`Inf`/`±FLT_MAX` em `pos` (`+0x30`) e `tgt` (`+0x80`), além de manter `Current Camera` vs. `Last Good Camera`.
- `ovl_btl_FUN_006e99f0` (`0x006E99F0`): aplicação de *camera shake* (`+0x100`). Detecta corrupção exata introduzida pelo shake.
- `FUN_0019f990` (`0x0019F990`): `CameraLookAt` → `sceVu0CameraMatrix`. Valida `pos`, `tgt` e distância quadrada `d2` (alerta em `d2 < 1e-6`, `NaN` ou `Inf`).
- `FUN_0010e430` (`0x0010E430`): captura a matriz `ViewProj` em espaço de clip NDC (`renderCam + 0xC0`) e alerta se estiver zerada ou com `NaN`/`Inf`.
- `sceVu0RotMatrixZ` (`0x00153170`), `sceVu0ApplyMatrix` (`0x00152DA8`), `quatToMat` (`0x00181E80`) e `matToQuat` (`0x00181FC0`): validam entradas/saídas quando chamados durante a batalha.

### 2. Lutadores e Colisão (`COLLISION`)
- Lidos diretamente da câmera principal de batalha (`s2 = 0x00D68560`): ponteiros `P1` (`s2 + 0x20`) e `P2` (`s2 + 0x24`), posição ativa (`+0x30` ou `+0x2C0` conforme `int16` em `+0x9F0`) e escalas (`+0xE4`, `+0xE8`, `+0x2F0`).
- Detecta:
  - `NaN`/`Inf`/`±FLT_MAX` na posição de `P1` ou `P2` (`COLLISION/CRIT`)
  - *Out-of-bounds* (`|x|, |y| > 5000`, `z < -500` ou `z > 5000`) (`COLLISION/ERROR`)
  - Teleporte em 1 frame (`delta > 250` com `altPosFlag == 0`) (`COLLISION/WARN`)
  - Separação absurda `distP1P2 > 5000` ou `NaN` (`COLLISION/ERROR`)
- Primitivas de distância `dist2D_XY` (`0x001814A0`) e `dist3D_XYZ` (`0x00181510`): alertam se retornarem `NaN`/`Inf`.

### 3. Comparador Diferencial de Matemática — Guest vs. Host (`MATH`)
Compara o retorno da função MIPS recompilada contra o equivalente nativo em C++ no host (apenas enquanto o overlay `BTL` está ativo):
- **Soft-float `double`:** `atan2` (`0x00170208`), `atan` (`0x0016EE78`), `__adddf3` (`0x00172540`), `__subdf3` (`0x00172598`), `__muldf3` (`0x00172600`), `__divdf3` (`0x00172888`), `__extendsfdf2` (`0x00172108`), `__truncdfsf2` (`0x00172CC0`).
- **Trigonometria `float`:** `sinf` (`0x001700A8`), `cosf` (`0x0016FD78`), `tanf` (`0x00170190`), `atanf` (`0x0016FAA8`), `atan2f` (`0x00170280`).
- **Inteiro 64 bits:** `__muldi3` (`0x001702B0`), `__divdi3` (`0x00170328`).

### 4. Painel Interativo ImGui (`F2`) e Overlay 3D→2D
- **Controles:** `Armed`, `Math Diff`, `Host Math A/B`, `Break on Anomaly`, `Trace Cam`, `Trace P1/P2`, `Overlay 3D`, `Pause` / `Resume` / `Step +1`, `Clear`, `Dump Report`, `Dump Snapshot` e `Screenshot`.
- **Abas:** `Anomalies` (com filtro por categoria), `Camera` (atual vs. última boa), `Fighters` (P1/P2 e `ViewProj`) e `Trace` (*ring buffer* reverso).
- **Overlay 3D→2D:** projeta `P1` (ciano), `P2` (laranja), eixo vertical, linha de distância `dist` e alvo da câmera `CAM TGT` (amarelo) diretamente sobre a janela do jogo usando a matriz `ViewProj`.

---

## Limitações Conhecidas

1. **Escopo restrito ao overlay `BTL.BIN`:** a maioria dos watchdogs e todo o comparador matemático só atuam enquanto o overlay de batalha (`overlayId == 1`) está carregado. Bugs nos menus ou no modo Aventura (`ADV.BIN`) não são monitorados por padrão.
2. **Contagem de `battleFrame`:** `battleFrame` incrementa **1:1** com a câmera principal de batalha (a que possui ponteiros `P1`/`P2` em `+0x20/+0x24`, ex.: `0x00D68560`), ignorando as câmeras auxiliares sem lutadores.
3. **Sensibilidade de timing no `replay`:** `.cache/luta-livre/pad.txt` reproduz inputs por tempo de relógio (*wall-clock*). Logs muito verbosos no `stderr` podem derrubar o FPS nos menus e desalinhar a navegação até a Luta Livre; se isso acontecer, use `--quiet` ou `--no-math`.
4. **Preempção/continuações:** os hooks verificam `ctx->pc == ra` após o retorno e descartam chamadas interrompidas pelo escalonador EE (contabilizadas em `preemptedSkips`), evitando falsos positivos de retorno parcial. Cada anomalia também registra o mini-callstack dos hooks ativos (`stack`).
5. **Cobertura de colisão:** os watchdogs de colisão atuais cobrem posições, deltas, limites de arena, distância `P1↔P2` e primitivas `dist2D_XY`/`dist3D_XYZ`. Volumes individuais de golpe/hurtbox (`ccBgAttackHit`, `ccWireHitModel`) e malhas triangulares de cenário ainda não estão mapeados nem desenhados no overlay 3D→2D.
6. **Limites de retenção em memória:** o *ring buffer* guarda até `256` eventos de trace e `128` pontos únicos de anomalia por sessão; no `events.jsonl`, cada ponto único `(cat, fn, ra)` grava no máximo as primeiras `5` ocorrências para evitar arquivos gigantes em bugs a 60 FPS.
7. **Dependência do overlay visual e screenshots:** o painel `F2` e o overlay 3D→2D requerem build com `PS2X_ENABLE_DEBUG_UI=ON` (padrão atual em `build/`). A captura de screenshots (`--screenshot-on-anomaly`) requer `EDO_ENABLE_PARALLEL_GS=ON` com `EDO_GS_BACKEND=gpu-present` ou `gpu-shadow`. O modo CLI (`report.txt`, `events.jsonl`, `snapshot.txt`) funciona independentemente da UI.
8. **Armadilha de rebuild:** ao estender o debugger, edite `runtime/debug/battle_debugger.{h,cpp}` (rebuild em ~8–15s). Evite criar novos `.cpp` em `runtime/debug/` sem necessidade (aciona regeração do CMake de ~3,5 min por causa de `CONFIGURE_DEPENDS`) e **nunca** edite `ps2_runtime_macros.h` ou `ps2_runtime.h` para probes (invalida o PCH e dispara rebuild de ~38 min).
