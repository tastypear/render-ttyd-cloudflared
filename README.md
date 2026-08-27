# render-ttyd-cloudflared (wasm-capped verification)

One-click Render web service that runs a **c2w (container-to-wasm) guest** behind a `ttyd` web terminal and a `cloudflared` tunnel. The guest's RAM is capped at build time, so the guest's own OOM killer handles memory pressure and the host container never hits Render's 512 MB hard limit.

## Architecture

```
browser -> cloudflared tunnel -> ttyd (host, PID 1) -> wasmtime -> c2w guest (debian, RAM capped)
                                                          |
                                              --dir /root::/mnt/host  (host disk mounted into guest)
```

- `cloudflared` (host): tunnels the port out to Cloudflare.
- `ttyd` (host): web terminal; each connection spawns a fresh guest as the shell.
- `wasmtime` (host): runs `out.wasm`.
- `out.wasm` = `c2w debian:bookworm-slim` with `VM_MEMORY_SIZE_MB=256`. The emulated kernel sees only 256 MB RAM; its own OOM killer enforces the cap.
- Host `/root` is mounted into the guest at `/mnt/host` (persistence on the roomy disk).

## Build pipeline

`out.wasm` is built by `.github/workflows/build-wasm.yml` (needs Docker, so it runs in GitHub Actions, not on Render) and published to release tag `wasm-build`. The Render Dockerfile downloads it from that release.

## Deploy

1. Push this repo to GitHub. The `build-wasm` workflow starts building `out.wasm` — the first build takes ~20-40 min as it compiles Bochs from source.
2. In Render: **New -> Blueprint**, pick this repo. Set env vars:
   - `CF_TOKEN` — Cloudflare Tunnel token.
   - `TTYD_USER` / `TTYD_PASS` — basic-auth for the terminal.
3. If the first deploy fails to download `out.wasm` (release not ready yet), wait for the `build-wasm` workflow to finish, then redeploy.

## Verify the memory cap

Open the web terminal (via your Cloudflare tunnel URL). **Wait ~10-12 seconds** for the Bochs-emulated guest to boot — the prompt appears after that. You are now inside the **guest** (debian, RAM capped).

> Known c2w cosmetic issue (#567): commands may echo twice and some escape sequences leak. Run `stty -echo` inside the guest if it bothers you. This does not affect correctness.

1. Confirm you're in the guest and see the cap:
   ```
   uname -a
   grep MemTotal /proc/meminfo      # ~244 MB (VM_MEMORY_SIZE_MB=256 minus kernel reserved)
   ls /mnt/host                      # host /root contents
   ```
2. Trigger guest-internal OOM with a subshell hog (the parent shell must survive):
   ```
   ( arr=(); i=0; while :; do arr+=("$(head -c 1M /dev/zero | base64)"); i=$((i+1)); echo "alloc $i MB"; done )
   ```
   The subshell is killed by the **guest's** OOM killer; you should return to the parent prompt.
3. Confirm it was the guest kernel, not Render:
   ```
   dmesg | grep -i "out of memory"
   ```
   You should still have your shell, and the Render service should NOT have restarted.
4. Watch Render's dashboard memory metric: it should plateau well under 512 MB (around 256 MB guest + overhead). If it hits 512 and the service restarts, the cap isn't holding — lower `VM_MEMORY_SIZE_MB` in the workflow.

## Tuning

- `VM_MEMORY_SIZE_MB` (in `.github/workflows/build-wasm.yml`): the guest RAM cap. Raise it if boot RSS is comfortable; lower it if the host container OOMs. Re-run the workflow after changing.
- `WASMTIME_VERSION` / `CLOUDFLARED_VERSION` / `TTYD_VERSION`: build args in `Dockerfile`.

## Known limitations (to optimize later)

- Each webshell connection cold-boots a fresh guest (wizer snapshot restore, seconds). A persistent guest (ttyd inside the guest via c2w-net) is the later step.
- No outbound networking inside the guest yet, so `apt install` won't work until c2w-net is wired in (verification #4).
- No supervisor: if a host-side process dies, only Render's container restart covers it.
