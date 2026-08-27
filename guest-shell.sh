#!/bin/bash
# Boot a fresh c2w guest as the login shell, with networking via c2w-net.
#   --dir=HOST::GUEST mounts host /root (the roomy disk) into the guest at /mnt/host.
#   Guest RAM is capped at build time (VM_MEMORY_SIZE_MB in the CI workflow),
#   so the guest's own OOM killer handles memory pressure; the host container
#   never touches Render's 512 MB limit.
# wasmtime v33.0.2 is c2w v0.8.4's pinned runtime. out.cwasm is precompiled during
# the Render Docker build; --allow-precompiled skips runtime JIT (peak ~375 MB).
# Networking: c2w-net runs in the background as the host-side network stack
# (gvisor-tap-vsock). wasmtime listens on 127.0.0.1:1234 via -S tcplisten;
# c2w-net dials that and bridges guest traffic to the host's network.
# --net=socket tells the c2w guest to use socket-based networking (fd=3).
# The guest takes ~10-12s to boot under Bochs; the prompt appears after that.
# Known c2w cosmetic issue #567: commands echo twice; run `stty -echo` if it bothers you.

# Start c2w-net in the background; retry until wasmtime's tcplisten is ready.
(
  for i in $(seq 1 30); do
    c2w-net 127.0.0.1:1234 2>/dev/null && break
    sleep 0.5
  done
) &

exec wasmtime --allow-precompiled \
  -S preview2=n -S tcplisten=127.0.0.1:1234 \
  --env=LISTEN_FDS=1 \
  --dir=/root::/mnt/host \
  -- /app/out.cwasm --net=socket /bin/bash
