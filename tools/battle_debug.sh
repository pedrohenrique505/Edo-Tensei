#!/usr/bin/env bash
# CLI do Battle Debugger para Naruto Shippuden: Ultimate Ninja 5 (edo-tensei).
# Projetado para uso direto por humanos e agentes de IA (logs estruturados, JSONL e auto-stop).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_OUT="$ROOT/.cache/battle-debug"

usage() {
    cat <<'EOF'
Uso: tools/battle_debug.sh <comando> [opcoes]

Comandos:
  replay [opcoes]   Reproduz a captura da Luta Livre com o Battle Debugger ativo
  run [opcoes]      Executa o jogo interativamente com o Battle Debugger ativo (F2 abre o painel)
  summary [arquivo] Exibe o resumo de um report.txt (padrao: .cache/battle-debug/report.txt)
  jsonl [opcoes]    Filtra eventos de um events.jsonl (padrao: .cache/battle-debug/events.jsonl)

Opcoes (replay / run):
  --out-dir DIR        Diretorio de saida (padrao: .cache/battle-debug)
  --timeout SEC        Timeout em segundos no modo replay (padrao: 75)
  --stop-after N       Encerra automaticamente N frames apos a primeira anomalia (padrao: 0 = nao encerrar)
  --max-frames N       Encerra automaticamente apos N frames de batalha (padrao: 0 = sem limite)
  --pad FILE           Arquivo de captura de controle para replay (padrao: .cache/luta-livre/pad.txt)
  --no-math            Desativa o comparador diferencial de matematica (Guest vs Host)
  --break              Pausa a luta automaticamente na primeira anomalia (modo interativo)
  --trace-camera       Registra trace por frame da camera no ring buffer/JSONL
  --trace-fighters     Registra trace por frame de P1/P2 e distancia no ring buffer/JSONL
  --overlay-3d         Ativa o overlay 3D->2D (P1, P2, alvo da camera) ao abrir
  --quiet              Nao ecoa logs do battle-dbg no stderr (grava apenas nos arquivos)

Opcoes (jsonl):
  --cat CATEGORIA      Filtra por categoria (CAMERA, MATH, MATRIX, COLLISION, GENERAL)
  --limit N            Limita o numero de linhas exibidas (padrao: 50)
  [arquivo]            Caminho para events.jsonl
EOF
}

cmd="${1:-help}"
shift || true

case "$cmd" in
    replay|run)
        out_dir="$DEFAULT_OUT"
        timeout_sec=75
        stop_after=0
        max_frames=0
        pad_file="$ROOT/.cache/luta-livre/pad.txt"
        math_check=1
        break_on_anom=0
        trace_cam=0
        trace_fighters=0
        overlay_3d=0
        log_stderr=1

        while [[ $# -gt 0 ]]; do
            case "$1" in
                --out-dir) out_dir="$2"; shift 2 ;;
                --timeout) timeout_sec="$2"; shift 2 ;;
                --stop-after) stop_after="$2"; shift 2 ;;
                --max-frames) max_frames="$2"; shift 2 ;;
                --pad) pad_file="$2"; shift 2 ;;
                --no-math) math_check=0; shift ;;
                --break) break_on_anom=1; shift ;;
                --trace-camera) trace_cam=1; shift ;;
                --trace-fighters) trace_fighters=1; shift ;;
                --overlay-3d) overlay_3d=1; shift ;;
                --quiet) log_stderr=0; shift ;;
                -h|--help) usage; exit 0 ;;
                *) echo "Opcao desconhecida: $1" >&2; usage >&2; exit 2 ;;
            esac
        done

        mkdir -p -- "$out_dir"
        report_path="$out_dir/report.txt"
        jsonl_path="$out_dir/events.jsonl"
        log_path="$out_dir/run.log"
        rm -f -- "$report_path" "$jsonl_path"

        export EDO_BATTLE_DEBUG=1
        export EDO_BATTLE_DUMP="$report_path"
        export EDO_BATTLE_JSONL="$jsonl_path"
        export EDO_BATTLE_MATH_CHECK="$math_check"
        export EDO_BATTLE_BREAK="$break_on_anom"
        export EDO_BATTLE_TRACE_CAMERA="$trace_cam"
        export EDO_BATTLE_TRACE_FIGHTERS="$trace_fighters"
        export EDO_BATTLE_OVERLAY_3D="$overlay_3d"
        export EDO_BATTLE_STDERR="$log_stderr"
        if [[ "$stop_after" -gt 0 ]]; then
            export EDO_BATTLE_STOP_AFTER="$stop_after"
        else
            unset EDO_BATTLE_STOP_AFTER || true
        fi
        if [[ "$max_frames" -gt 0 ]]; then
            export EDO_BATTLE_MAX_FRAMES="$max_frames"
        else
            unset EDO_BATTLE_MAX_FRAMES || true
        fi

        if [[ "$cmd" == "replay" ]]; then
            echo "[battle-debug] replay -> out=$out_dir timeout=${timeout_sec}s stop_after=${stop_after} max_frames=${max_frames}"
            set +e
            timeout -s INT "${timeout_sec}s" "$ROOT/tools/luta_livre.sh" reproduzir "$pad_file" > "$log_path" 2>&1
            rc=$?
            set -e
            echo "[battle-debug] encerrado (rc=$rc). Logs em: $log_path"
            if [[ -s "$report_path" ]]; then
                echo ""
                head -n 45 "$report_path"
                echo ""
                echo "[battle-debug] relatorio completo: $report_path"
                echo "[battle-debug] eventos JSONL:      $jsonl_path"
            else
                echo "[battle-debug] AVISO: nenhum report.txt gerado (verifique $log_path)" >&2
            fi
        else
            echo "[battle-debug] executando interativamente (F2 abre o painel, relatorio em $report_path)"
            exec "$ROOT/tools/run_edo.sh"
        fi
        ;;

    summary)
        report_path="${1:-$DEFAULT_OUT/report.txt}"
        if [[ ! -s "$report_path" ]]; then
            echo "Relatorio nao encontrado ou vazio: $report_path" >&2
            exit 1
        fi
        cat "$report_path"
        ;;

    jsonl)
        cat_filter=""
        limit=50
        jsonl_path="$DEFAULT_OUT/events.jsonl"
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --cat) cat_filter="$2"; shift 2 ;;
                --limit) limit="$2"; shift 2 ;;
                -h|--help) usage; exit 0 ;;
                *) jsonl_path="$1"; shift ;;
            esac
        done
        if [[ ! -s "$jsonl_path" ]]; then
            echo "Arquivo JSONL nao encontrado ou vazio: $jsonl_path" >&2
            exit 1
        fi
        if [[ -n "$cat_filter" ]]; then
            grep -F "\"cat\":\"${cat_filter}\"" "$jsonl_path" | head -n "$limit" || true
        else
            head -n "$limit" "$jsonl_path"
        fi
        ;;

    -h|--help|help)
        usage
        ;;

    *)
        echo "Comando desconhecido: $cmd" >&2
        usage >&2
        exit 2
        ;;
esac
