FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl python3 unzip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Download all latest amd64 prebuilt packages in one layer
RUN set -ex \
    && AXONHUB_FILE=$(curl -sL https://github.com/looplj/axonhub/releases/latest/download/checksums.txt | grep linux_amd64 | awk '{print $2}') \
    && curl -sL -o /tmp/axonhub.zip "https://github.com/looplj/axonhub/releases/latest/download/$AXONHUB_FILE" \
    && unzip -o /tmp/axonhub.zip -d /tmp/axonhub \
    && mv /tmp/axonhub/axonhub /app/axonhub \
    && GOTTY_FILE=$(curl -sL https://github.com/tastypear/gotty/releases/latest/download/SHA256SUMS | grep linux_amd64 | awk '{print $2}') \
    && curl -sL -o /tmp/gotty.tar.gz "https://github.com/tastypear/gotty/releases/latest/download/$GOTTY_FILE" \
    && tar xzf /tmp/gotty.tar.gz -C /app \
    && curl -sL -o /tmp/handy-sshd.tar.gz https://github.com/nwtgck/handy-sshd/releases/download/v0.4.3/handy-sshd-0.4.3-linux-amd64.tar.gz \
    && mkdir -p /tmp/handy-sshd \
    && tar xzf /tmp/handy-sshd.tar.gz -C /tmp/handy-sshd \
    && mv /tmp/handy-sshd/handy-sshd /app/handy-sshd \
    && WSTUNNEL_FILE=$(curl -sL https://github.com/erebe/wstunnel/releases/latest/download/checksums.txt | grep linux_amd64 | awk '{print $2}') \
    && curl -sL -o /tmp/wstunnel.tar.gz "https://github.com/erebe/wstunnel/releases/latest/download/$WSTUNNEL_FILE" \
    && mkdir -p /tmp/wstunnel \
    && tar xzf /tmp/wstunnel.tar.gz -C /tmp/wstunnel \
    && mv /tmp/wstunnel/wstunnel /app/wstunnel \
    && curl -sL -o /app/cloudflared https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64 \
    && chmod +x /app/axonhub /app/gotty /app/handy-sshd /app/wstunnel /app/cloudflared \
    && rm -rf /tmp/axonhub /tmp/axonhub.zip /tmp/gotty.tar.gz /tmp/handy-sshd /tmp/handy-sshd.tar.gz /tmp/wstunnel /tmp/wstunnel.tar.gz

COPY start.sh /app/start.sh
COPY healthz.py /app/healthz.py
RUN chmod +x /app/start.sh

CMD ["/app/start.sh"]
