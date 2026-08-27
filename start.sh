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
# opt-level=1 keeps the JIT peak at ~386 MB so this won't OOM the container.
# stdin held open 120s for Render's slow Bochs boot (c2w #566 workaround).
(
  set +e
  sleep 25  # let cloudflared finish tunnel setup before we measure/add load
  echo "[selftest] baseline RSS of all procs: $(ps -eo rss= | awk '{s+=$1} END {print s/1024" MB"}')"
  echo "[selftest] starting wasmtime boot probe (opt-level=1)..."
  s=$(date +%s)
  { echo 'echo GUEST_BOOT_OK'; echo 'grep MemTotal /proc/meminfo'; echo 'exit'; sleep 120; } \
    | timeout 200 wasmtime -O opt-level=1 --dir=/root::/mnt/host /app/out.wasm /bin/sh 2>&1 \
    | while IFS= read -r l; do echo "[selftest] $l"; done
  echo "[selftest] probe finished, elapsed=$(($(date +%s)-s))s"
) &

# ttyd becomes PID 1 and binds $PORT (keeps Render's health check happy).
# Each webshell connection runs guest-shell.sh, which boots a fresh c2w guest
# (debian:bookworm-slim, RAM capped at build time via VM_MEMORY_SIZE_MB).
exec ttyd -p "$PORT" -W "${AUTH_ARGS[@]}" /app/guest-shell.sh
