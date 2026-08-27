#!/bin/bash
# Boot a fresh c2w guest as the login shell.
#   --dir=HOST::GUEST mounts host /root (the roomy disk) into the guest at /mnt/host.
#   Guest RAM is capped at build time (VM_MEMORY_SIZE_MB in the CI workflow),
#   so the guest's own OOM killer handles memory pressure; the host container
#   never touches Render's 512 MB limit.
# wasmtime v33.0.2 is c2w v0.8.4's pinned runtime (v48+ Cranelift can't compile
# the module). Invocation matches c2w's own tests (no `run` subcommand, --dir= form).
# The guest takes ~10-12s to boot under Bochs; the prompt appears after that.
# Known c2w cosmetic issue #567: commands echo twice; run `stty -echo` if it bothers you.
exec wasmtime --dir=/root::/mnt/host /app/out.wasm /bin/bash
