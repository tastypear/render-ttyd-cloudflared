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

# One-shot boot diagnostic (background, logs to stdout -> Render logs).
# Runs wasmtime with stdin held open (c2w #566 workaround) so the guest can boot,
# then reports boot result + MemTotal + elapsed. Remove after debugging.
(
  set +e
  echo "[selftest] starting wasmtime boot probe..."
  s=$(date +%s)
  { echo 'echo GUEST_BOOT_OK'; echo 'grep MemTotal /proc/meminfo'; echo 'exit'; sleep 90; } \
    | timeout 200 wasmtime --dir=/root::/mnt/host /app/out.wasm /bin/sh 2>&1 \
    | while IFS= read -r l; do echo "[selftest] $l"; done
  echo "[selftest] probe finished, elapsed=$(($(date +%s)-s))s"
) &

# ttyd becomes PID 1 and binds $PORT (keeps Render's health check happy).
# Each webshell connection runs guest-shell.sh, which boots a fresh c2w guest
# (debian:bookworm-slim, RAM capped at build time via VM_MEMORY_SIZE_MB).
exec ttyd -p "$PORT" -W "${AUTH_ARGS[@]}" /app/guest-shell.sh
