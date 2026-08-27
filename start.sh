#!/bin/bash
set -e

PORT="${PORT:-7681}"

# Cloudflare Tunnel in the background (requires CF_TOKEN).
if [ -n "$CF_TOKEN" ]; then
    echo "Starting cloudflared tunnel..."
    cloudflared tunnel run --token "$CF_TOKEN" &
else
    echo "CF_TOKEN not set; skipping cloudflared tunnel."
fi

# Basic-auth for the web terminal (optional).
AUTH_ARGS=()
if [ -n "$TTYD_USER" ] && [ -n "$TTYD_PASS" ]; then
    AUTH_ARGS=(-c "${TTYD_USER}:${TTYD_PASS}")
fi

# ttyd becomes PID 1 and binds $PORT (keeps Render's health check happy).
# Each webshell connection runs guest-shell.sh, which boots a fresh c2w guest
# (debian:bookworm-slim, RAM capped at build time via VM_MEMORY_SIZE_MB).
exec ttyd -p "$PORT" -W "${AUTH_ARGS[@]}" /app/guest-shell.sh
