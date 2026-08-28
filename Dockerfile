FROM debian:bookworm-slim AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY memlimit.c memshim.c ./
RUN gcc -O2 -o memlimit memlimit.c \
    && gcc -O2 -shared -fPIC -o memshim.so memshim.c -ldl

FROM debian:bookworm-slim
ARG CLOUDFLARED_VERSION=2026.8.2
ARG TTYD_VERSION=1.7.8

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates wget \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /app

RUN wget -qO /usr/local/bin/cloudflared \
        "https://github.com/cloudflare/cloudflared/releases/download/${CLOUDFLARED_VERSION}/cloudflared-linux-amd64" \
    && chmod +x /usr/local/bin/cloudflared

RUN wget -qO /usr/local/bin/ttyd \
        "https://github.com/tastypear/ttyd/releases/download/${TTYD_VERSION}/ttyd.x86_64" \
    && chmod +x /usr/local/bin/ttyd

COPY --from=build /build/memlimit   /app/memlimit
COPY --from=build /build/memshim.so /app/memshim.so
COPY start-actual.sh /app/start-actual.sh
RUN chmod +x /app/start-actual.sh

EXPOSE 7681
CMD ["/app/memlimit", "--hard=450", "--no-shim", "--", "/bin/bash", "/app/start-actual.sh"]
