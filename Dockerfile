FROM debian:bookworm-slim

ARG CLOUDFLARED_VERSION=2026.8.2
ARG TTYD_VERSION=1.7.8
ARG WASMTIME_VERSION=33.0.2
# out.wasm is produced by .github/workflows/build-wasm.yml and published to this release tag.
ARG WASM_RELEASE_TAG=wasm-build

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates wget xz-utils \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /app

# cloudflared: host-side tunnel out to Cloudflare.
RUN wget -qO /usr/local/bin/cloudflared \
        "https://github.com/cloudflare/cloudflared/releases/download/${CLOUDFLARED_VERSION}/cloudflared-linux-amd64" \
    && chmod +x /usr/local/bin/cloudflared

# ttyd: host-side web terminal. It spawns the wasm guest as the login shell.
RUN wget -qO /usr/local/bin/ttyd \
        "https://github.com/tastypear/ttyd/releases/download/${TTYD_VERSION}/ttyd.x86_64" \
    && chmod +x /usr/local/bin/ttyd

# wasmtime: runs the c2w wasm module.
RUN wget -qO /tmp/wt.tar.xz \
        "https://github.com/bytecodealliance/wasmtime/releases/download/v${WASMTIME_VERSION}/wasmtime-v${WASMTIME_VERSION}-x86_64-linux.tar.xz" \
    && tar xf /tmp/wt.tar.xz -C /tmp \
    && cp "/tmp/wasmtime-v${WASMTIME_VERSION}-x86_64-linux/wasmtime" /usr/local/bin/wasmtime \
    && chmod +x /usr/local/bin/wasmtime \
    && rm -rf /tmp/wt.tar.xz /tmp/wasmtime-v*

# out.wasm from the CI-produced release. Retries because the release may not exist
# on the very first deploy (before the build-wasm workflow finishes).
RUN set -x; for i in $(seq 1 20); do \
      wget -qO /app/out.wasm \
        "https://github.com/tastypear/render-ttyd-cloudflared/releases/download/${WASM_RELEASE_TAG}/out.wasm" \
        && test -s /app/out.wasm && break; \
      echo "wasm release not ready (attempt $i); retrying in 30s..."; sleep 30; \
    done; test -s /app/out.wasm

# Precompile out.wasm -> out.cwasm during the Render build (build instances have ample
# RAM for the ~522 MB JIT peak; the 512 MB hard limit only applies at runtime).
# --target x86_64-unknown-linux-gnu: compile for baseline x86_64 with NO CPU-specific
# features (no AVX-512, no AVX2, etc.), so the precompiled module loads on any x86_64
# host regardless of what features the build server's CPU has.
# At runtime, --allow-precompiled skips JIT entirely (peak ~375 MB, fits 512 MB limit).
RUN echo "Build CPU flags:" && grep -m1 ^flags /proc/cpuinfo \
    && wasmtime compile --target x86_64-unknown-linux-gnu -O opt-level=2 /app/out.wasm -o /app/out.cwasm \
    && rm /app/out.wasm && test -s /app/out.cwasm

COPY guest-shell.sh /app/guest-shell.sh
COPY start.sh /start.sh
RUN chmod +x /app/guest-shell.sh /start.sh

EXPOSE 7681
CMD ["/start.sh"]
