#!/bin/bash
set -e

# Run the Cloudflare Tunnel in the background (requires CF_TOKEN).
if [ -n "$CF_TOKEN" ]; then
    echo "Starting cloudflared tunnel..."
    cloudflared tunnel run --token "$CF_TOKEN" &
else
    echo "CF_TOKEN is not set; skipping cloudflared tunnel."
fi

# ttyd listens on the port Render provides (PORT), defaulting to 7681.
PORT="${PORT:-7681}"

# Add basic auth only when both credentials are provided.
AUTH_ARGS=()
if [ -n "$TTYD_USER" ] && [ -n "$TTYD_PASS" ]; then
    AUTH_ARGS=(-c "${TTYD_USER}:${TTYD_PASS}")
fi

exec ttyd -p "$PORT" -W "${AUTH_ARGS[@]}" bash
