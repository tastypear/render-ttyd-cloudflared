#!/bin/bash
set -e

PORT="${PORT:-7681}"

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
