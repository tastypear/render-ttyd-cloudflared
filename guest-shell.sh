#!/bin/bash
# Boot a fresh c2w guest as the login shell.
#   --dir HOST::GUEST  mounts host /root (the roomy disk) into the guest at /mnt/host.
#   Guest RAM is capped at build time (VM_MEMORY_SIZE_MB in the CI workflow),
#   so the guest's own OOM killer handles memory pressure; the host container
#   never touches Render's 512 MB limit.
# If wasmtime v48 rejects the c2w module's WASI, try adding: -S preview2=false
exec wasmtime run --dir /root::/mnt/host /app/out.wasm /bin/bash
