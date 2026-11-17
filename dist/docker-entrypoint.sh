#!/bin/bash
# Start pondd in the container (docs/docker.md). On the first start, write /data/pond.yaml from these variables; after that
# the file is yours to edit and they are not read again:
#   POND_ADDRESS      how people reach this Pond, host[:port], as invite links say it (needed for invites that work)
#   POND_PORT         the port pondd listens on inside the container, HTTPS and voice's UDP (default 8443)
#   POND_DB_HOST      MariaDB (default "db", compose.yaml's), POND_DB_PORT (3306), POND_DB_USER (pond),
#   POND_DB_PASSWORD  POND_DB_NAME (pond)
# Then, if the Pond has no key yet: with POND_NAME set, `pondctl init "$POND_NAME"` once MariaDB answers; without it, wait
# until someone runs `docker exec -it <container> pondctl init "My Pond"`. Arguments go to pondd.
set -eu
cd /data
config=/data/pond.yaml

quote() { printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g')"; }

if [ ! -e "$config" ]; then
    {
        echo "# Pond configuration (written by the container on its first start; docs/design.md says what each setting does)"
        echo
        echo "# Every interface inside the container; publish the port (TCP and UDP) with docker run -p or compose.yaml."
        echo "listen: 0.0.0.0"
        echo "port: ${POND_PORT:-8443}"
        echo
        echo "# What invite links say: how people reach this Pond, host[:port] (the port is left off when it is 443)."
        if [ -n "${POND_ADDRESS:-}" ]; then
            echo "address: $(quote "$POND_ADDRESS")"
        else
            echo "# address: pond.example.org:8443"
        fi
        echo
        echo "# The Pond's key, its TLS certificate (self-signed unless you put your own here) and uploads."
        echo "data_dir: /data/data"
        echo
        echo "database:"
        echo "  host: $(quote "${POND_DB_HOST:-db}")"
        echo "  port: ${POND_DB_PORT:-3306}"
        echo "  user: $(quote "${POND_DB_USER:-pond}")"
        echo "  password: $(quote "${POND_DB_PASSWORD:-}")"
        echo "  name: $(quote "${POND_DB_NAME:-pond}")"
    } > "$config"
    chmod 600 "$config"
fi
if ! grep -q '^address:' "$config"; then
    echo "docker-entrypoint: $config has no address, so invite links won't work; set POND_ADDRESS or edit it" >&2
fi

if [ ! -e /data/data/pond.key ]; then
    if [ -n "${POND_NAME:-}" ]; then
        # init makes the key first and never again, so only once the database answers
        host=$(sed -n '/^database:/,$ s/^  host: *"\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$config" | head -1)
        port=$(sed -n '/^database:/,$ s/^  port: *\([0-9]*\)$/\1/p' "$config" | head -1)
        for _ in $(seq 60); do
            (exec 3<>"/dev/tcp/$host/${port:-3306}") 2>/dev/null && break
            sleep 1
        done
        sleep 2   # the port opens a moment before MariaDB takes logins
        pondctl init "$POND_NAME"
    else
        echo "docker-entrypoint: this Pond has no key yet. To make it: docker exec -it <this container> pondctl init \"My Pond\"" >&2
        until [ -e /data/data/pond.key ]; do sleep 2; done
        sleep 5   # let init finish with the database
    fi
fi

exec pondd -c "$config" "$@"
