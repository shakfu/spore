# Network security: the loopback-only constraint

A review of the constraint described in [design.md](design.md#local-only-by-construction). It covers what the constraint guarantees, where the guarantee stops, and what to change.

Method: code reading of `src/server.c` and `src/http.c` at `51cc291`. The proxy and Docker cases were not run. They follow from the cited documentation.

## Verdict

Keep the constraint. Narrow the claim in `README.md`. Keep `token` optional, and make it safe to deliver where it is needed.

## What the constraint guarantees

- `listen_tcp()` binds `127.0.0.1` or `::1`. `spore_config` has no address field, so an embedder cannot select another address.

- `peer_allowed()` rejects TCP peers outside `127.0.0.0/8` and `::1`, and Unix-socket peers with another uid.

- `check()` rejects foreign `Host` values (TCP only) and foreign `Origin` values before routing.

- `test_not_reachable_on_other_interfaces` checks that the port refuses connections on a non-loopback address.

The constraint also removes work from the project: TLS, defences against slow remote clients, and a poller for thousands of connections.

## Where the guarantee stops

### 1. It limits the listener, not who can reach it

A forwarder on the same host connects from loopback, so `peer_allowed()` passes. Remote clients then reach spore.

| Forwarder | `Host` spore receives | Host check |
|---|---|---|
| `ssh -L`, `socat` | whatever the client sends, usually `localhost:<port>` | passes |
| nginx `proxy_pass` | the upstream address, by default ([`proxy_set_header`](https://nginx.org/en/docs/http/ngx_http_proxy_module.html#proxy_set_header)) | passes |
| Caddy `reverse_proxy` | the client's original `Host`, by default ([defaults](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy#defaults)) | fails with 403 |

`token` is optional. Behind nginx with default settings, a server without `token` answers any remote client.

Other parts of spore assume local peers, and a forwarder removes that assumption:

- no TLS;

- 64 connections, with a 30 s receive deadline as the only slow-client limit;

- the unbounded response buffer listed in [gaps.md](gaps.md).

spore cannot detect a forwarder reliably. nginx adds no `X-Forwarded-For` or `Via` header unless configured to.

### 2. Other local processes are not authenticated over TCP

Without `token`, any process of any user on the host can connect to the TCP port. Only the Unix socket checks the peer uid.

### 3. Every loopback origin is trusted

`spore__origin_allowed()` accepts `http://localhost:<any port>` and `*.localhost`. A page served by any other local server can call the API, including a page injected into that server through XSS. The allowance exists so that a UI dev server on another port works without configuration.

### 4. The Host check is skipped on Unix sockets

The reason given in `design.md` is that browsers cannot connect to a Unix socket. A reverse proxy in front of the socket makes that false. The Origin check still applies. A DNS-rebinding page can then read `GET` responses, because browsers send no `Origin` on same-origin `GET`.

### 5. Docker port publishing does not reach it

`docker run -p` forwards to the container's interface address, not its loopback ([port publishing](https://docs.docker.com/engine/network/port-publishing/)). A spore server in a container is reachable only with `--network host` or through a mounted Unix socket.

### 6. The realtime module needs a GPU

`README.md` reports that TTS is real-time only on a GPU. A client on another device than the GPU host cannot connect without a forwarder. That leads to case 1.

## When a token helps

What authenticates each kind of peer today:

| Peer | TCP loopback | Unix socket |
|---|---|---|
| Hostile web page | Host and Origin checks | cannot connect |
| Other local user | nothing | kernel uid check |
| Same-uid sandboxed app (Flatpak, macOS App Sandbox) | nothing, if it has network access | blocked by the file sandbox |
| Remote client through a forwarder | nothing | nothing; the proxy is the peer |
| Another `localhost:*` page | allowed by the Origin check | cannot connect |

A token adds a check only where the table says "nothing" or "allowed". The sandbox row is inferred from how those sandboxes treat loopback. It was not tested.

On a direct Unix socket a token is redundant. Behind a reverse proxy, every remote client has the proxy's uid, so the token or the proxy must authenticate them.

On a single-user host with no forwarder, a TCP token covers two rows: sandboxed apps and other `localhost` pages. A mandatory token is therefore not justified.

### How a client obtains the token

| Client | Delivery |
|---|---|
| Embedder running spore in-process | It generates the token and already holds it |
| Parent that spawns `spored` | Environment variable or inherited fd |
| Separate process, same host | A mode `0600` token file |
| Browser | A URL with the token in the fragment, printed to the terminal |

- **`--token T` exposes the token.** Other users read a process's arguments through `ps`. That defeats the token against the "other local user" row.

- **The token file only helps TCP-only clients.** A process that can read a `0600` file can also open the `0600` socket.

- **Fragment over query.** Browsers do not send the fragment to the server, so it appears in no log and no `Referer`. The page reads it and sends `Authorization: Bearer`, or the WebSocket subprotocol spore already accepts. Jupyter prints a token URL and opens the browser ([security docs](https://jupyter-server.readthedocs.io/en/latest/operators/security.html)); opening it is optional.

- **No cookies.** Cookies are scoped by host, not port. Other servers on `localhost` would receive the token.

## Alternatives considered

**Define "local" by authentication.** Every peer proves identity: the uid on a Unix socket, the token on TCP. Rejected as a requirement for the reason above. It remains the rule for forwarders and multi-user hosts.

**Identify TCP peers by uid.** Linux reports the owning uid of a loopback connection through `sock_diag`. macOS has no equally direct call. It needs no token delivery, but it is platform-specific and does nothing for browsers or forwarders. Rejected.

**A bind-address field.** It would not replace the proxy. Browsers allow microphone capture only in a secure context, which means HTTPS for any host other than localhost ([`getUserMedia`](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/getUserMedia#privacy_and_security)). Remote realtime clients need a TLS proxy in either design.

## Recommended changes

1. **Reword the scope claim in `README.md`.** It should say that the listener cannot be bound elsewhere, and that a forwarder on the host exposes it.

2. **Document a proxy setup.** Require `token`. The proxy should pass the client's `Host` unchanged, so spore's Host check still rejects rebinding. That needs an allowed-hosts setting for the public name (item 5). Until it exists, the proxy must validate `Host` itself and then rewrite it.

3. **Keep `token` optional and caller-supplied in the library.** No API change.

   - Add `spored --token-file PATH`: generate 32 random bytes, write them mode `0600`, and print `http://localhost:<port>/#token=...`.

   - Deprecate `spored --token T`, or document that `ps` exposes it.

   - State in `README.md` which transport to use: the Unix socket for non-browser clients, TCP with a token for browsers and forwarders.

4. **Add a setting that disables the implicit loopback origins.** With it set, only `spore_config.origins` is accepted. It closes case 3 for deployments that know their UI origin.

5. **Add an allowed-hosts setting, like `origins`.** On TCP it extends the accepted `Host` values. On a Unix socket, setting it turns the Host check on, which closes case 4. Unset, the current behaviour stays.

6. **Document the Docker setup**: a Unix socket in a mounted volume, or `--network host`.

## Open questions

- Does the realtime client run on the GPU host, or on another device?

- Is a browser UI a primary client, or mostly SDK clients? For SDK clients on one host the socket is enough, and the token work reduces to the `--token` argv fix.

- Are multi-user hosts in scope? If not, case 2 needs only a sentence in the README.

- Is Docker a target?

## Context

Chrome now asks the user before a public page may request a loopback address ([Local Network Access](https://developer.chrome.com/blog/local-network-access)). That reduces the browser attacks for Chrome users. The Host and Origin checks remain necessary for other browsers.
