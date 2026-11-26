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
| Tela preta | Em `numberwait.log`, o contexto base da thread 8 permanece em `0x0017BD10`, com retorno para `0x0017E76C`. A rotina desloca a capacidade de um inteiro multiprecisão até alcançar o tamanho pedido. A sonda registrou ponteiro do objeto igual a zero, capacidade zero e tamanho pedido igual a três. Deslocar zero mantém o laço em execução. O chamador é a rotina de conversão decimal em `0x0017D990`. A origem do ponteiro nulo ainda precisa ser confirmada. |
| Áudio da seleção | As capturas PCM do fluxo do jogo registraram milhares de amostras nos mesmos dois valores extremos, inclusive após o ajuste de DMA. Isso confirma saturação na saída. O estágio em que ela começa ainda precisa ser identificado. |

Em algumas execuções instrumentadas, o carregamento parou mais cedo na leitura de dados do overlay `BTL`. Nesses casos, o contexto base da thread 8 alternava entre `0x001BB2E8` e `0x001BB340`, enquanto as threads de carga e decodificação estavam prontas. Use `hostwait.log` para esse caso e `numberwait.log` para a tela preta posterior.

## Estado do código e evidências locais

- `patches/ps2recomp/0063-ps2xRuntime-reuse-idle-ee-invocation-stacks.patch` reaproveita pilhas de callbacks sem invocações ativas.
- `patches/ps2recomp/0064-ps2xIOP-support-read-only-ioman-files.patch` implementa abertura, leitura, busca e fechamento de arquivos somente leitura no IOP. O fechamento bem-sucedido retorna o descritor, conforme a implementação original usada pelo jogo.
- `patches/ps2recomp/0065-ps2xIOP-align-spu2-voice-dma-size.patch` contabiliza a transferência de voz em blocos de 64 bytes.
- `.cache/luta-livre/` guarda a captura, os logs, as imagens, os áudios PCM e o diff das sondas temporárias em `diagnostico-temporario.patch`. O log `debug-replay-stack.log` contém o backtrace do encerramento original.
- As sondas foram retiradas do código-fonte. O executável local ainda é o último build instrumentado. Para um build limpo, aplique os patches ao clone de PS2Recomp quando necessário com `tools/apply_upstream_patches.sh`, depois recompile o alvo `edo-tensei` na árvore `build/`. A configuração local usa `ld.bfd`.

Para retomar o diagnóstico da tela preta, siga o objeto multiprecisão usado na chamada de `0x0017BCC0` e confira onde ele recebe o ponteiro zero. A alocação usada pela conversão decimal é uma hipótese a testar.
