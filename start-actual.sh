#!/bin/bash
set -e

PORT="${PORT:-7681}"

# Self-test: verify apt install works under memlimit (munmap fix)
(
    echo "[selftest] starting apt install test..."
    apt-get update -qq 2>&1
    apt-get install -y -qq procps 2>&1
    echo "[selftest] APT_INSTALL_EXIT=$?"
) >&2 || echo "[selftest] apt install failed (non-fatal)" >&2

if [ -n "$CF_TOKEN" ]; then
    echo "Starting cloudflared tunnel..."
    cloudflared tunnel run --token "$CF_TOKEN" &
else
    echo "CF_TOKEN not set; skipping cloudflared tunnel."
fi

AUTH_ARGS=()
if [ -n "$TTYD_USER" ] && [ -n "$TTYD_PASS" ]; then
    AUTH_ARGS=(-c "${TTYD_USER}:${TTYD_PASS}")
fi

exec ttyd -p "$PORT" -m 1 -W "${AUTH_ARGS[@]}" /bin/bash
