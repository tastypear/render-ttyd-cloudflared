# render-ttyd-cloudflared

One-click Render web service that runs a `ttyd` web terminal and tunnels it out with `cloudflared`.

## Deploy on Render

1. In Render: **New → Blueprint**, pick this repo. Render reads `render.yaml`.
2. Fill in the environment variables when prompted:
   - `CF_TOKEN` — Cloudflare Tunnel token.
   - `TTYD_USER` / `TTYD_PASS` — Basic-auth credentials for the terminal.
3. Deploy. The service listens on the port Render assigns (`PORT`, defaults to 7681).

## How it works

- Base image: `debian:bookworm-slim` (amd64).
- `cloudflared` runs in the background: `cloudflared tunnel run --token $CF_TOKEN`.
- `ttyd` runs in the foreground: `ttyd -p $PORT -W -c $TTYD_USER:$TTYD_PASS bash`.

## Update binaries

Versions are pinned as `ARG` in the `Dockerfile`:
- `CLOUDFLARED_VERSION` — https://github.com/cloudflare/cloudflared/releases
- `TTYD_VERSION` — https://github.com/tastypear/ttyd/releases
