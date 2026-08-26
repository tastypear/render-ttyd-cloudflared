FROM debian:bookworm-slim

# Pinned to the latest releases at build time; override with --build-arg to bump.
ARG CLOUDFLARED_VERSION=2026.8.2
ARG TTYD_VERSION=1.7.8

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates wget \
    && rm -rf /var/lib/apt/lists/*

# cloudflared: https://github.com/cloudflare/cloudflared/releases
RUN wget -qO /usr/local/bin/cloudflared \
        "https://github.com/cloudflare/cloudflared/releases/download/${CLOUDFLARED_VERSION}/cloudflared-linux-amd64" \
    && chmod +x /usr/local/bin/cloudflared

# ttyd: https://github.com/tastypear/ttyd/releases
RUN wget -qO /usr/local/bin/ttyd \
        "https://github.com/tastypear/ttyd/releases/download/${TTYD_VERSION}/ttyd.x86_64" \
    && chmod +x /usr/local/bin/ttyd

COPY start.sh /start.sh
RUN chmod +x /start.sh

EXPOSE 7681

CMD ["/start.sh"]
