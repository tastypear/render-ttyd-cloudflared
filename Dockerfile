FROM debian:bookworm-slim AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY probe.c shim.c dyntest.c ./
RUN gcc -O2 -o probe probe.c \
    && gcc -O2 -shared -fPIC -o shim.so shim.c -ldl \
    && gcc -O2 -o dyntest dyntest.c

FROM debian:bookworm-slim
RUN mkdir -p /app
COPY --from=build /build/probe  /app/probe
COPY --from=build /build/shim.so /app/shim.so
COPY --from=build /build/dyntest /app/dyntest

EXPOSE 7681
CMD ["/app/probe"]
