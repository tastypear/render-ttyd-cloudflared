FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    util-linux kmod procps python3 ca-certificates libcap2-bin \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY collect.sh /app/collect.sh
COPY server.py /app/server.py
RUN chmod +x /app/collect.sh

# Run collection at startup, then serve results
CMD ["/bin/bash", "-c", "/app/collect.sh && python3 /app/server.py"]
