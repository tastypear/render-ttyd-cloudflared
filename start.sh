#!/bin/bash

export THIS_SERVICE=${THIS_SERVICE:-https://render-ttyd-cloudflared.onrender.com}
export USER_PASS=${USER_PASS:-root:render}
PORT=${PORT:-10000}

echo "=== Render Multi-Service Startup ==="
echo "THIS_SERVICE=$THIS_SERVICE"
echo "USER_PASS=$USER_PASS"
echo "PORT=$PORT"
echo "CF_TOKEN set: $([ -n "$CF_TOKEN" ] && echo yes || echo no)"

# 1. healthz server (Render health check endpoint)
python3 /app/healthz.py &
echo "[started] healthz on :$PORT"

# 2. axonhub (default port 8090)
/app/axonhub &
echo "[started] axonhub on :8090 (pid $!)"

# 3. gotty (default port 8080)
COLORTERM=truecolor /app/gotty -c "$USER_PASS" -w bash &
echo "[started] gotty on :8080 (pid $!)"

# 4. handy-sshd (default port 2222)
COLORTERM=truecolor /app/handy-sshd -u "$USER_PASS" &
echo "[started] handy-sshd on :2222 (pid $!)"

# 5. wstunnel - executable only, not started
chmod +x /app/wstunnel
echo "[ready] wstunnel at /app/wstunnel (not started)"

# 6. cloudflared tunnel
if [ -n "$CF_TOKEN" ]; then
    /app/cloudflared tunnel run --token "$CF_TOKEN" &
    echo "[started] cloudflared (pid $!)"
else
    echo "[skip] cloudflared - CF_TOKEN not set"
fi

# 7. keep-alive loop: ping /healthz every 60s
(
    sleep 15
    while true; do
        curl -s --max-time 10 "$THIS_SERVICE/healthz" > /dev/null 2>&1 || true
        sleep 60
    done
) &
echo "[started] keep-alive: $THIS_SERVICE/healthz every 60s"

echo "=== All services started ==="
wait
