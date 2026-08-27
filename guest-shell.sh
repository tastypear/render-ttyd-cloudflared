#!/bin/bash
# Boot a fresh c2w guest as the login shell.
#   --dir=HOST::GUEST mounts host /root (the roomy disk) into the guest at /mnt/host.
#   Guest RAM is capped at build time (VM_MEMORY_SIZE_MB in the CI workflow),
#   so the guest's own OOM killer handles memory pressure; the host container
#   never touches Render's 512 MB limit.
# wasmtime v33.0.2 is c2w v0.8.4's pinned runtime (v48+ Cranelift can't compile
# the module). out.cwasm is precompiled during the Render Docker build (where RAM is
# plentiful); --allow-precompiled skips runtime JIT, cutting the peak from ~522 MB to
# ~375 MB so 256 MB guest RAM fits Render's 512 MB runtime limit.
# The guest takes ~10-12s to boot under Bochs; the prompt appears after that.
# Known c2w cosmetic issue #567: commands echo twice; run `stty -echo` if it bothers you.
exec wasmtime --allow-precompiled --dir=/root::/mnt/host /app/out.cwasm /bin/bash
