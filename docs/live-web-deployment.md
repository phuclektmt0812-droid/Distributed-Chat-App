# Public Web Gateway (live-web branch)

This branch adds an experimental public browser gateway while keeping the existing DCAP TCP/UDP protocol and Server implementation.

## Architecture

```text
Browser
  | HTTPS (terminated by Caddy or another reverse proxy)
  v
Live Web Gateway :8081
  | one DcapClient TCP + UDP session per browser login
  v
Existing DCAP Server :54000/TCP and :54001/UDP
```

The browser no longer needs to run `client_agent`. The Gateway serves the existing files in `web/`, creates a separate DCAP client session for each browser login, and exposes the same HTTP routes used by the existing UI.

Implemented routes:
- `GET /`, `/index.html`, `/app.js`, `/style.css`
- `POST /login`, `POST /logout`
- `GET /status`, `GET /user_list`, `GET /events`
- `POST /global_message`, `POST /private_message`
- `POST /file_upload`, `POST /file_decision`, `GET /file_download?transfer_id=...`

The Gateway uses an HttpOnly, SameSite=Lax session cookie and a different `DcapClient` for each logged-in browser. It rejects cross-origin POST requests, limits active sessions to 64, and expires sessions after 12 hours of inactivity. Uploaded files are staged under `uploads/live-web/<session-token>/`. Incoming files are stored under `received_files/live-web/<session-token>/` and become downloadable from the browser after transfer completion.

## Build

From the repository root:

```bash
make live-web-gateway
```

Or build all application binaries:

```bash
make app
```

The new executable is `live_web_gateway` (or `live_web_gateway.exe` on Windows).

## Local smoke test

Start the existing DCAP Server first. Make sure its `dcap.key` is available to both the Server and Gateway.

Linux/macOS:

```bash
export DCAP_KEY_FILE="$PWD/dcap.key"
export DCAP_COOKIE_SECURE=0
./live_web_gateway 127.0.0.1 54000 8081 web
```

Windows PowerShell:

```powershell
$env:DCAP_KEY_FILE = "$PWD\dcap.key"
$env:DCAP_COOKIE_SECURE = "0"
.\live_web_gateway.exe 127.0.0.1 54000 8081 web
```

Open `http://127.0.0.1:8081/`. Setting `DCAP_COOKIE_SECURE=0` is only for local HTTP testing. Do not use that setting for a public HTTPS deployment.

Arguments:

```text
live_web_gateway <server-ip> <server-tcp-port> <http-port> <web-root>
```

Defaults are `127.0.0.1`, `54000`, `8081`, and `web`.

## Public HTTPS deployment

Run the DCAP Server and Gateway on a Linux host. Copy the same `dcap.key` to the Gateway host securely; do not commit it. Keep the Gateway listening port private and put an HTTPS reverse proxy in front of it.

Example Caddy site configuration:

```caddy
chat.example.com {
    reverse_proxy 127.0.0.1:8081
}
```

Run the Gateway with the secure-cookie default (or explicitly set it):

```bash
export DCAP_KEY_FILE=/etc/dcap/dcap.key
export DCAP_COOKIE_SECURE=1
./live_web_gateway 127.0.0.1 54000 8081 web
```

Expose HTTPS (normally TCP 443) to users. Do not expose the Gateway's port 8081 directly to the Internet. If the DCAP Server is on the same host, restrict TCP 54000 and UDP 54001 to the local host / trusted Gateway host in the host firewall. If the Server is on another host, allow those ports only from the Gateway host.

## Important limitations before inviting public users

This is an integration prototype, not a production-ready public service.

- DCAP protocol v1 authenticates by username only. There is no password, account ownership, email verification, or protection against someone claiming another person's currently-unused username. Do not use it for a public community or sensitive conversations.
- The existing shared AES key remains a shared secret across the Server and Gateway. Keep it private and do not put it in the browser.
- Add persistent user authentication, login rate limiting, abuse controls, monitoring, and automated session/upload/received-file cleanup before a wider public launch.
- The Gateway uses HTTP polling for events, not WebSockets.
- The Gateway has not yet been verified with a live VPS, TLS proxy, multi-browser deployment, or a completed end-to-end browser test. Build and smoke-test it in the target environment before sharing a public URL.
