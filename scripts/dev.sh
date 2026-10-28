#!/usr/bin/env bash
# A Pond to try things with: a MariaDB of its own and pondd, on 127.0.0.1:8900, kept in /tmp/pond-dev between runs.
# Prints an invite link that never runs out. Ctrl-C stops both.
#   scripts/dev.sh             start (making it all the first time)
#   scripts/dev.sh --reset     throw it away and start afresh
set -euo pipefail
cd "$(dirname "$0")/.."
dir=${POND_DEV_DIR:-/tmp/pond-dev}   # short: MariaDB's socket path must fit in ~100 bytes

[ "${1:-}" = --reset ] && rm -rf "$dir"
[ -x build/pondd ] || scripts/build.sh

fresh=0
if [ ! -d "$dir/db" ]; then
  mkdir -p "$dir"
  mariadb-install-db --no-defaults --datadir="$dir/db" --auth-root-authentication-method=normal --skip-test-db > "$dir/install.log"
  fresh=1
fi
mariadbd --no-defaults --datadir="$dir/db" --socket="$dir/sock" --skip-networking --skip-grant-tables \
  --log-error="$dir/mariadb.log" --pid-file="$dir/mariadb.pid" &
db_pid=$!
trap 'kill $db_pid 2>/dev/null; wait $db_pid 2>/dev/null' EXIT
for _ in $(seq 100); do [ -S "$dir/sock" ] && break; sleep 0.1; done

cat > "$dir/pond.yaml" <<YAML
listen: 127.0.0.1
port: 8900
data_dir: $dir/data
database:
  socket: $dir/sock
  user: root
  name: pond
YAML

if [ $fresh = 1 ]; then
  mariadb --no-defaults -S "$dir/sock" -u root -e "CREATE DATABASE pond"
  build/pondctl -c "$dir/pond.yaml" init "Dev Pond"
  build/pondctl -c "$dir/pond.yaml" channel create random --topic "Anything goes"
  build/pondctl -c "$dir/pond.yaml" invite create 2>/dev/null > "$dir/invite"
fi
echo "Invite: $(cat "$dir/invite")"
echo "Admin:  build/pondctl -c $dir/pond.yaml <command>"
build/pondd -c "$dir/pond.yaml"
