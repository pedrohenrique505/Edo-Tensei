# Diagnóstico da Luta Livre

A captura percorre a seleção de personagens e arena até a tentativa de iniciar a luta em *Naruto Shippuden: Ultimate Ninja 5*, versão PAL `SLES_556.05`. O build original fechava ao chegar a 100% no carregamento. Com os patches abaixo, o replay passa desse ponto, mas a tela fica preta e a luta ainda não inicia. O áudio da seleção continua saturando.

## Reprodução

Execute os comandos da raiz do repositório. O launcher usa `build/edo-tensei`, os arquivos extraídos em `game/` e a ISO PAL. O caminho padrão da ISO está em `tools/run_edo.sh`. Defina `EDO_ISO=/caminho/para/jogo.iso` se ela estiver em outro local. O backend padrão é `gpu-present`.

Para repetir os comandos da sessão original e guardar o log:

```bash
mkdir -p .cache/luta-livre
timeout 90s tools/luta_livre.sh reproduzir > .cache/luta-livre/replay-novo.log 2>&1
```

O script usa `.cache/luta-livre/pad.txt`. O último botão foi gravado por volta de 44 s desde o início da captura do controle. O replay usa tempo decorrido, então diferenças de desempenho podem deslocar comandos. Confirme visualmente que a execução chegou à seleção de arena e à tela de carregamento. O código de saída do processo também teve valor zero no replay que fechou com erro, portanto confira o log e a tela para determinar o resultado.

Para gravar outro percurso:

```bash
tools/luta_livre.sh gravar .cache/luta-livre/pad-novo.txt
tools/luta_livre.sh reproduzir .cache/luta-livre/pad-novo.txt
```

Na primeira execução, avance até **Luta Livre**, confirme os personagens e a arena e tente iniciar a partida. Feche a janela após observar o resultado. O script recusa sobrescrever uma captura existente.

## Resultados verificados

| Etapa | Evidência |
| --- | --- |
| Encerramento aos 100% | `jogo.log` e `replay.log` registram `EE invocation stack space exhausted`. O GDB confirmou a falha em `EeScheduler::invocationStackTop`, chamado pelo escalonador ao preparar uma invocação pendente. |
| Depois de reutilizar as pilhas | O processo passou do encerramento original e chegou à tela preta. O módulo `SNDBASE.IRX` repetia erros na leitura de arquivo após os imports IOMAN sem implementação. |
| Depois das correções de IOMAN e SPU2 | Os erros de leitura, fechamento e `sceHSyn_VoisTrans Error` desapareceram no replay com os três patches. O rastreio posterior mostrou que o banco observado retornou os 389.568 bytes esperados e que o último RPC de áudio registrado, `0xB2`, retornou com sucesso. A tela continuou preta. |
| Tela preta | Em `numberwait.log`, o contexto base da thread 8 permanecia em `0x0017BD10` (`FUN_0017bcc0` / `lshift` da `newlib`), com retorno para `0x0017E76C` (`FUN_0017d990` / `_strtod_r`). A causa raiz foi identificada em `FUN_00118ca0`: o alocador principal do jogo pede `0x016CD6F0` bytes via `malloc` (`FUN_0017a610`), assumindo `EndOfHeap() == 0x01FF8000` (o que no PS2 real deixa `0x2010` bytes livres no `sbrk`). Como o PS2Recomp limita o heap em `0x01F00000` para reservar pilhas de callbacks assíncronos, o primeiro `malloc` falhava e o fallback guloso (`while (!p) size -= 0x100`) consumia 100% do `sbrk` até `0x01F00000`, deixando menos de 256 bytes para a `newlib`. Ao carregar o overlay `BTL`, `_strtod_r` -> `Balloc` (`FUN_0017b528`) -> `_calloc_r` (`FUN_00177fb8`) -> `_malloc_r` (`FUN_0017a908`) -> `sbrk` (`FUN_0015f1a0`) recebia `ENOMEM` (`-1`), fazendo `Balloc` retornar `NULL`. `d2b` (`FUN_0017c278`) escrevia `len=1` no endereço `0x00000010` sem checar `NULL` e passava ponteiro nulo a `lshift`, causando loop infinito com `maxwds == 0`. |
| Correção da tela preta | Adicionado override em `runtime/overrides/edo_overrides.cpp` para `0x0017A610` (`malloc`) quando chamado por `FUN_00118ca0` (`ra == 0x00118D3C` ou `0x00118D58`), reservando 64 KiB (`0x10000`) de margem abaixo de `guestHeapLimit()` no `sbrk` (`0x0040F774`). Validado via `.cache/luta-livre/verificar.sh`: o carregamento passa de 100% e a luta (Kakashi vs Naruto) inicia e renderiza normalmente (GIF avança de `728.202` para `>1.042.408` pacotes; `replay-fixed-60.png` confirma a partida em andamento). |
| Áudio da seleção | A instrumentação por segundo no `IopLibsd::mixFrames` separou o clipping entre AutoDMA (`CRI_ADXI.IRX`) e vozes VAG (`SNDBASE.IRX`). Durante a seleção de personagens (`t=35..41s`), o fluxo AutoDMA chegava ao IOP já saturado (até `7.407` amostras em `±32767` em `t=38s` e `19.749` amostras clipadas na saída entre `t=35..40s`), enquanto a BGM ADX (`t=0..34s` e `t=42..73s`) apresentava `0` clips. A engenharia reversa via Ghidra/REA em `SLES_556.05` mostrou que as vozes dos personagens usam o codec **CRI AHX** (`AHX/PS2EE Ver.1.55`, `FUN_0013e158` -> `FUN_0013a808` -> filtro de síntese sub-banda `FUN_0013baa0`), que emprega a instrução MMI **`PHMADH`**. Em `PS2Recomp` (`ps2xRecomp/src/lib/mmi_translation_helpers.cpp`), `CodeGenerator::translatePHMADH` (e `translatePHMSBH`) estava incorreto: multiplicava `rs*rs` e `rt*rt` em 16 bits truncados (`_mm_mullo_epi16`), acumulava escalarmente em `hi/lo` e escrevia apenas 64 bits (`SET_GPR_U64`) em vez dos 4 produtos internos de 32 bits em 128 bits (`_mm_madd_epi16`) + `lo/hi/lo1/hi1`. |
| Correção do áudio da seleção | Corrigidas `translatePHMADH` (usando `_mm_madd_epi16` + escrita em `rd`, `lo`, `hi`, `lo1`, `hi1`) e `translatePHMSBH` em `patches/ps2recomp/0066-ps2xRecomp-fix-phmadh-and-phmsbh-translation.patch`, e atualizados os 8 arquivos recompilados do filtro AHX em `generated/` (84 ocorrências de `phmadh`). Na reexecução do replay (`after-phmadh.s16le`), os clips no AutoDMA caíram de `8.013` para `6` (`-99,93%`, com `t=38s` caindo de `7.407` para `0`) e o clipping total na saída durante `t=35..40s` caiu de `19.749` para `697` (`-96,5%`, `0,027%..0,243%` das amostras). |

Em algumas execuções instrumentadas, o carregamento parou mais cedo na leitura de dados do overlay `BTL`. Nesses casos, o contexto base da thread 8 alternava entre `0x001BB2E8` e `0x001BB340`, enquanto as threads de carga e decodificação estavam prontas. Use `hostwait.log` para esse caso e `numberwait.log` para a tela preta posterior.

## Estado do código e evidências locais

- `patches/ps2recomp/0063-ps2xRuntime-reuse-idle-ee-invocation-stacks.patch` reaproveita pilhas de callbacks sem invocações ativas.
- `patches/ps2recomp/0064-ps2xIOP-support-read-only-ioman-files.patch` implementa abertura, leitura, busca e fechamento de arquivos somente leitura no IOP. O fechamento bem-sucedido retorna o descritor, conforme a implementação original usada pelo jogo.
- `patches/ps2recomp/0065-ps2xIOP-align-spu2-voice-dma-size.patch` contabiliza a transferência de voz em blocos de 64 bytes.
- `patches/ps2recomp/0066-ps2xRecomp-fix-phmadh-and-phmsbh-translation.patch` corrige a tradução das instruções MMI `PHMADH` e `PHMSBH` no recompilador (usadas pelo filtro de síntese sub-banda do codec CRI AHX).
- `.cache/luta-livre/` guarda a captura, os logs, as imagens, os áudios PCM e o diff das sondas temporárias em `diagnostico-temporario.patch`. O log `debug-replay-stack.log` contém o backtrace do encerramento original.
- As sondas foram retiradas do código-fonte e o build limpo foi validado com `.cache/luta-livre/verificar.sh`. Para reproduzir um build limpo, aplique os patches ao clone de PS2Recomp com `tools/apply_upstream_patches.sh` e recompile o alvo `edo-tensei` na árvore `build/`. A configuração local usa `ld.bfd`.

- Tanto a tela preta ao iniciar a luta (reserva de 64 KiB no `sbrk` em `runtime/overrides/edo_overrides.cpp`) quanto o estouro de áudio na seleção de personagens (tradução de `PHMADH`/`PHMSBH` em `PS2Recomp`) foram resolvidos e validados no replay completo até a batalha.
