# Live Web Gateway deployment (live-web branch)

This branch adds a browser-facing Gateway. A person only needs a browser; they do not need to install or run `client_agent` locally. The Gateway creates one independent DCAP client session for every successful browser login.

> **Authentication warning:** DCAP v1 still uses username-only login. Anyone who can reach the site can claim an unused username or impersonate someone after that username becomes available. HTTPS protects traffic in transit, but it does not fix missing account authentication. Keep this prototype private (for example, behind a VPN or a trusted allowlist) until password-based accounts, rate limits, abuse controls, and account ownership are implemented.

## Architecture

```text
Computer A browser ── HTTPS ──┐
                              ├── Caddy / reverse proxy ── Live Web Gateway
Computer B browser ── HTTPS ──┘                                |
                                                               | one DcapClient per browser login
                                                               v
                                                        DCAP Server
                                                        TCP 54000
                                                        UDP 54001
```

The browser talks only to the Gateway over HTTP(S). The Gateway talks to the existing DCAP Server over TCP and UDP. It serves `web/index.html`, `web/app.js`, and `web/style.css`; the login page derives its API origin from the website address, so users do not enter a Gateway URL.

Each browser profile receives an independent HttpOnly session cookie and a separate `DcapClient`. Two computers can therefore sign in to the same domain as different usernames and chat with each other. Two separate users in the same browser profile are not supported simultaneously because the session cookie is shared by tabs in that profile; use separate browser profiles for that specific test.

The Gateway currently:
- supports global chat, private chat, independent browser sessions, and file offers/transfers;
- uses HTTP polling (not WebSockets);
- limits active sessions to 64 and concurrent HTTP workers to 8;
- expires browser sessions after 12 hours of inactivity;
- buffers browser uploads in memory and limits each upload to 16 MiB;
- stages uploads under `uploads/live-web/<session-token>/`;
- stores received files under `received_files/live-web/<session-token>/`.

Uploaded and received files are not automatically cleaned up yet. Configure retention and disk-space monitoring before leaving the service running.

## Build

From the repository root:

```bash
make live-web-gateway
```

Or build all application binaries:

```bash
make app
```

The Gateway executable is `live_web_gateway` (`live_web_gateway.exe` on Windows).

## Local-only smoke test

Start the DCAP Server first. It creates or loads `dcap.key`. The Gateway must read the same key file.

Windows PowerShell:

```powershell
$env:DCAP_KEY_FILE = "$PWD\\dcap.key"
$env:DCAP_COOKIE_SECURE = "0"
.\\live_web_gateway.exe 127.0.0.1 54000 8081 web
```

Linux/macOS:

```bash
export DCAP_KEY_FILE="$PWD/dcap.key"
export DCAP_COOKIE_SECURE=0
./live_web_gateway 127.0.0.1 54000 8081 web
```

Open `http://127.0.0.1:8081/`. The Gateway binds to `127.0.0.1` by default, so it is reachable only from that host. `DCAP_COOKIE_SECURE=0` is only for local HTTP tests.

## Two computers on one LAN

On the host computer, start the Server, then start the Gateway with an explicit LAN-facing bind address:

Windows PowerShell:

```powershell
$env:DCAP_KEY_FILE = "$PWD\\dcap.key"
$env:DCAP_COOKIE_SECURE = "0"
.\\live_web_gateway.exe 127.0.0.1 54000 8081 web 0.0.0.0
```

Linux/macOS:

```bash
export DCAP_KEY_FILE="$PWD/dcap.key"
export DCAP_COOKIE_SECURE=0
./live_web_gateway 127.0.0.1 54000 8081 web 0.0.0.0
```

Find the host's LAN IPv4 address (for example, `192.168.1.10`) and allow inbound TCP 8081 only on a trusted Private network. On Windows PowerShell as Administrator:

```powershell
New-NetFirewallRule -DisplayName "DCAP Live Web Gateway" -Direction Inbound -Protocol TCP -LocalPort 8081 -Action Allow -Profile Private
```

Then both computers open the same URL, using the host's LAN address:

```text
http://192.168.1.10:8081/
```

Replace the example address with the actual host address. Do not open Server TCP 54000 or UDP 54001 to other LAN users; those connections are made by the Gateway process on the host.

## Internet / VPS deployment

For access from different Internet networks, deploy the DCAP Server and Gateway on an always-on VPS or cloud host. Point a DNS name (for example, `chat.example.com`) to the public host and terminate HTTPS with Caddy or another reverse proxy.

Keep the Gateway bound to loopback so only the local reverse proxy can reach it. Example Caddy configuration:

```caddy
chat.example.com {
    reverse_proxy 127.0.0.1:8081
}
```

Linux service environment and start command:

```bash
export DCAP_KEY_FILE=/etc/dcap/dcap.key
export DCAP_COOKIE_SECURE=1
./live_web_gateway 127.0.0.1 54000 8081 web 127.0.0.1
```

Expose only HTTPS (normally TCP 443) to the Internet. Do not expose Gateway TCP 8081, Server TCP 54000, or Server UDP 54001 to arbitrary Internet clients. If the DCAP Server is on a different host, allow TCP 54000 and UDP 54001 only from the Gateway host's fixed address. Copy the shared `dcap.key` securely to the Gateway host; never commit it or place it in browser code.

For a VPS where Caddy and Gateway are on the same host, the Gateway default loopback binding is appropriate. For a separate Server host, replace `127.0.0.1` in the Gateway's first argument with the Server's private address.

## Command-line arguments

```text
live_web_gateway [server-ip] [server-tcp-port] [http-port] [web-root] [bind-ipv4]
```

Defaults: `127.0.0.1`, `54000`, `8081`, `web`, and `127.0.0.1`.

The final argument controls which IPv4 interface accepts browser connections:
- `127.0.0.1`: local-only; recommended behind a reverse proxy on the same host.
- `0.0.0.0`: all IPv4 interfaces; use only for a trusted LAN test with a firewall rule.
- A specific LAN/private IPv4 address: bind only to that interface.

## Current limitations

- There is no password, registered account ownership, account recovery, email verification, login rate limiting, or brute-force protection. Username-only login is not safe for an unrestricted public service.
- The AES-GCM key is shared by the Server and Gateway. Do not expose or commit it.
- The Gateway uses HTTP polling rather than WebSockets.
- Browser uploads are buffered in memory and limited to 16 MiB per file.
- File-transfer resume after disconnect is not supported by protocol v1.
- Uploaded and received files need an operational cleanup/retention policy.
- This branch has an automated integration test for two independent browser sessions, but it has not been built and run in the target Windows/Linux/VPS environments from this editing session. A real browser test through a TLS proxy and an Internet-hosted deployment must still be performed before treating the service as production-ready.
