#!/bin/bash
set -e

PORT="${PORT:-7681}"

# Self-test: multiple apt installs to verify per-pid reaping + apt cache fix
(
    echo "[selftest] starting multi-install test..."
    apt-get update -qq 2>&1
    apt-get install -y -qq procps 2>&1
    apt-get install -y -qq unzip 2>&1
    apt-get install -y -qq strace 2>&1
    apt-get install -y -qq tree 2>&1
    echo "[selftest] APT_MULTI_INSTALL_EXIT=$?"
) >&2 || echo "[selftest] failed (non-fatal)" >&2

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
