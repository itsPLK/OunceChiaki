#!/bin/bash
# Talks to the console's FTP server (OunceShell, port 1337, anonymous). It only
# answers while no homebrew is running.
#
#   scripts/console.sh <ip> deploy        upload build/switch2/OunceChiaki.nro
#   scripts/console.sh <ip> log [file]    download logs/ounce.log (or ounce.1.log)
#
# OUNCE_CONSOLE=<ip> can stand in for the address.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)

if [ $# -ge 1 ] && [[ $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    host=$1
    shift
else
    host=${OUNCE_CONSOLE:?usage: scripts/console.sh <ip> deploy|log}
fi
ftp="ftp://$host:1337"
dir=/switch/OunceChiaki

remote_size() {
    curl -sS --connect-timeout 10 -I "$ftp$1" | tr -d '\r' | awk '/^Content-Length:/ { print $2 }'
}

case "${1:-}" in
deploy)
    nro="$root/build/switch2/OunceChiaki.nro"
    [ -f "$nro" ] || { echo "No $nro: run scripts/build.sh" >&2; exit 1; }
    local_size=$(wc -c <"$nro" | tr -d ' ')
    # An upload cut off by Wi-Fi leaves a half NRO that crashes at start-up:
    # retry until the console has every byte.
    for attempt in 1 2 3 4 5; do
        curl -sS --connect-timeout 10 --max-time 300 --ftp-create-dirs -T "$nro" "$ftp$dir/OunceChiaki.nro" || true
        size=$(remote_size "$dir/OunceChiaki.nro" || true)
        if [ "$size" = "$local_size" ]; then
            echo "Deployed $dir/OunceChiaki.nro ($size bytes)"
            exit 0
        fi
        echo "Attempt $attempt: console has ${size:-nothing}, expected $local_size bytes; retrying" >&2
    done
    exit 1
    ;;
log)
    name=${2:-ounce.log}
    out="$root/build/logs/$name"
    mkdir -p "$root/build/logs"
    curl -sS --connect-timeout 10 --max-time 120 --retry 3 -o "$out" "$ftp$dir/logs/$name"
    echo "Saved $out"
    ;;
*)
    echo "usage: scripts/console.sh <ip> deploy|log [file]" >&2
    exit 2
    ;;
esac
