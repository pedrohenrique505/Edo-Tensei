#!/usr/bin/env bash
# CLI do Battle Debugger para Naruto Shippuden: Ultimate Ninja 5 (edo-tensei).
# Projetado para uso direto por humanos e agentes de IA (logs estruturados, JSONL,
# auto-stop, snapshots de memoria/structs, triagem e diff antes/depois de correcoes).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_OUT="$ROOT/.cache/battle-debug"

usage() {
    cat <<'EOF'
Uso: tools/battle_debug.sh <comando> [opcoes]

Comandos:
  replay [opcoes]              Reproduz a captura da Luta Livre com o Battle Debugger ativo
  run [opcoes]                 Executa o jogo interativamente com o Battle Debugger ativo (F2 abre o painel)
  summary [report.txt]         Exibe o relatorio completo (padrao: .cache/battle-debug/report.txt)
  top [report.txt]             Ranking de anomalias por ocorrencias para triagem rapida
  snapshot [--bad-only] [file] Exibe o dump de structs/memoria (Camera, P1, P2 e --watch)
  jsonl [opcoes] [file]        Filtra eventos do events.jsonl
  diff <reportA> <reportB>     Compara dois relatorios (antes vs depois de um fix)

Opcoes (replay / run):
  --out-dir DIR                Diretorio de saida (padrao: .cache/battle-debug)
  --timeout SEC                Timeout em segundos no modo replay (padrao: 75)
  --stop-after N               Encerra automaticamente N frames apos a primeira anomalia
  --max-frames N               Encerra automaticamente apos N frames de batalha
  --snapshot-frame N           Captura snapshot de memoria/structs exatamente no frame N
  --watch ADDR:SIZE[,...]      Enderecos extras de RDRAM para incluir no snapshot (ex: 0x00617EF0:0x40)
  --pad FILE                   Arquivo de captura de controle para replay
  --no-math                    Desativa o comparador diferencial de matematica (Guest vs Host)
  --break                      Pausa a luta automaticamente na primeira anomalia (modo interativo)
  --trace-camera               Registra trace por frame da camera no ring buffer/JSONL
  --trace-fighters             Registra trace por frame de P1/P2 e distancia no ring buffer/JSONL
  --overlay-3d                 Ativa o overlay 3D->2D (P1, P2, alvo da camera) ao abrir
  --quiet                      Nao ecoa logs do battle-dbg no stderr (grava apenas nos arquivos)

Opcoes (jsonl):
  --cat CATEGORIA              Filtra por categoria (CAMERA, MATH, MATRIX, COLLISION, GENERAL)
  --limit N                    Limita o numero de linhas exibidas (padrao: 50)
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
        snapshot_frame=0
        watch_mem=""
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
                --snapshot-frame) snapshot_frame="$2"; shift 2 ;;
                --watch) watch_mem="$2"; shift 2 ;;
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
        snapshot_path="$out_dir/snapshot.txt"
        log_path="$out_dir/run.log"
        rm -f -- "$report_path" "$jsonl_path" "$snapshot_path"

        export EDO_BATTLE_DEBUG=1
        export EDO_BATTLE_DUMP="$report_path"
        export EDO_BATTLE_JSONL="$jsonl_path"
        export EDO_BATTLE_SNAPSHOT="$snapshot_path"
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
        if [[ "$snapshot_frame" -gt 0 ]]; then
            export EDO_BATTLE_SNAPSHOT_FRAME="$snapshot_frame"
        else
            unset EDO_BATTLE_SNAPSHOT_FRAME || true
        fi
        if [[ -n "$watch_mem" ]]; then
            export EDO_BATTLE_WATCH_MEM="$watch_mem"
        else
            unset EDO_BATTLE_WATCH_MEM || true
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
                head -n 50 "$report_path"
                echo ""
                echo "[battle-debug] relatorio completo: $report_path"
                echo "[battle-debug] eventos JSONL:      $jsonl_path"
                if [[ -s "$snapshot_path" ]]; then
                    echo "[battle-debug] snapshot memoria:   $snapshot_path"
                fi
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

    top)
        report_path="${1:-$DEFAULT_OUT/report.txt}"
        if [[ ! -s "$report_path" ]]; then
            echo "Relatorio nao encontrado ou vazio: $report_path" >&2
            exit 1
        fi
        python3 - "$report_path" <<'PY'
import re, sys
path = sys.argv[1]
rows = []
in_anom = False
for line in open(path, encoding="utf-8", errors="replace"):
    if line.startswith("--- ANOMALIES"):
        in_anom = True
        continue
    if line.startswith("--- RECENT TRACE"):
        break
    if not in_anom:
        continue
    m = re.match(r"^\[#(\d+)\s+f=(\d+)\s+([A-Z]+)/([A-Z]+)\s+x(\d+)\]\s+fn=(0x[0-9A-Fa-f]+)\(([^)]*)\)\s+ra=(0x[0-9A-Fa-f]+)\s*\|\s*(.*)$", line.strip())
    if m:
        rows.append((int(m.group(5)), int(m.group(2)), m.group(3), m.group(4), m.group(6), m.group(7), m.group(8), m.group(9)))
rows.sort(key=lambda r: (-r[0], r[1]))
print(f"{'COUNT':>7}  {'FRAME':>5}  {'CAT/SEV':<13}  {'FUNCTION':<28}  {'CALLER_RA':<10}  DETAIL")
print("-" * 115)
for cnt, fr, cat, sev, fn, name, ra, detail in rows:
    fn_col = f"{fn}({name})"
    print(f"{cnt:>7}  {fr:>5}  {cat+'/'+sev:<13}  {fn_col:<28}  {ra:<10}  {detail[:80]}")
PY
        ;;

    snapshot)
        bad_only=0
        snap_path="$DEFAULT_OUT/snapshot.txt"
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --bad-only) bad_only=1; shift ;;
                -h|--help) usage; exit 0 ;;
                *) snap_path="$1"; shift ;;
            esac
        done
        if [[ ! -s "$snap_path" ]]; then
            echo "Snapshot nao encontrado ou vazio: $snap_path" >&2
            exit 1
        fi
        if [[ "$bad_only" -eq 1 ]]; then
            grep -E "^===|^---|<<< BAD" "$snap_path" || true
        else
            cat "$snap_path"
        fi
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

    diff)
        if [[ $# -lt 2 ]]; then
            echo "Uso: $0 diff <reportA> <reportB>" >&2
            exit 2
        fi
        python3 - "$1" "$2" <<'PY'
import re, sys

def parse(path):
    meta = {}
    anoms = {}
    in_anom = False
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("battleFrame="):
            for kv in line.strip().split():
                k, _, v = kv.partition("=")
                meta[k] = v
        elif line.startswith("--- ANOMALIES"):
            in_anom = True
        elif line.startswith("--- RECENT TRACE"):
            in_anom = False
        elif in_anom:
            m = re.match(r"^\[#\d+\s+f=(\d+)\s+([A-Z]+)/([A-Z]+)\s+x(\d+)\]\s+fn=(0x[0-9A-Fa-f]+)\(([^)]*)\)\s+ra=(0x[0-9A-Fa-f]+)\s*\|\s*(.*)$", line.strip())
            if m:
                key = (m.group(2), m.group(5), m.group(6), m.group(7))
                anoms[key] = (int(m.group(4)), int(m.group(1)), m.group(8))
    return meta, anoms

mA, aA = parse(sys.argv[1])
mB, aB = parse(sys.argv[2])
print(f"A: {sys.argv[1]} -> {mA}")
print(f"B: {sys.argv[2]} -> {mB}")
for k in ("totalAnomalies", "mathMismatches", "mathChecks"):
    if k in mA and k in mB:
        va, vb = int(mA[k]), int(mB[k])
        print(f"  delta {k}: {va} -> {vb} ({vb - va:+d})")

resolved = sorted(set(aA) - set(aB))
added = sorted(set(aB) - set(aA))
print(f"\nRESOLVED sites ({len(resolved)}):")
for k in resolved:
    print(f"  - [{k[0]}] {k[1]}({k[2]}) ra={k[3]} (was x{aA[k][0]})")
print(f"\nNEW sites ({len(added)}):")
for k in added:
    print(f"  + [{k[0]}] {k[1]}({k[2]}) ra={k[3]} (now x{aB[k][0]} @ f={aB[k][1]})")
PY
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
