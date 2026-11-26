#!/usr/bin/env bash
# Grava ou reproduz os estados do controle durante o caminho até a Luta Livre.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
trace="${2:-$root/.cache/luta-livre/pad.txt}"
trace="$(realpath -m -- "$trace")"

case "${1:-}" in
    gravar)
        mkdir -p -- "$(dirname "$trace")"
        if [[ -e "$trace" ]]; then
            printf 'Já existe uma captura em %s. Informe outro caminho.\n' "$trace" >&2
            exit 1
        fi
        unset DRA2_PAD_REPLAY
        export DRA2_PAD_RECORD="$trace"
        ;;
    reproduzir)
        if [[ ! -s "$trace" ]]; then
            printf 'Captura vazia ou ausente em %s.\n' "$trace" >&2
            exit 1
        fi
        unset DRA2_PAD_RECORD
        export DRA2_PAD_REPLAY="$trace"
        ;;
    *)
        printf 'Uso: %s {gravar|reproduzir} [arquivo-da-captura]\n' "$0" >&2
        exit 2
        ;;
esac

unset EDO_AUTO_PRESS
exec "$root/tools/run_edo.sh"
