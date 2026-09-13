# Channel Points (`tools/channel_points/`)

Turns **real Twitch Channel Point redemptions** on the broadcaster's channel
into the game's existing drop-file command path (`twitch_drop/cp_*.txt` ->
`src/twitch.c` `TwitchPollDrop()`). No game code, no `src/` edits, no harness
changes.

```
Twitch EventSub websocket (wss://eventsub.wss.twitch.tv/ws)
        |  channel.channel_points_custom_reward_redemption.add
        v  channel.channel_points_automatic_reward_redemption.add
tools/channel_points/cp_listen.py
        |  reward name -> cp_map.ini -> verb|arg|who|dur
        v
twitch_drop/cp_<ts>_<reward>.txt   -->   game (drop=1 in twitch_config.txt)
```

---

# OWNER RUNBOOK — the 2-minute unlock

### LANE FIRST-RUN (the normal deployment path)

This flow is **account-agnostic and repeatable by anyone on any machine** —
nothing in the repo, the scripts, or the configs names the streamer. When
LANE starts using it on his own machine:

1. The streamer boots the game (or doesn't even need to yet) and runs
   `python tools\channel_points\cp_auth.py` (after pasting the two app
   credentials — Step 1 below).
2. He pastes the printed link into a browser and signs into Twitch
   **AS THE BROADCASTER**, clicks **Authorize**.
3. Done. His token is his own: cp_auth records the signing account in
   `[auth] login`, and the channel follows the token automatically. He never
   edits anyone else's credentials and no one else's token ever touches his
   setup.

**Token lifetime is ~4 HOURS** (Twitch user tokens; `expires_at` in
cp_config.ini shows when it dies). The ritual: **re-run `cp_auth.py` before
each stream** — one command, one browser approve — or renew with the stored
`refresh_token` (`grant_type=refresh_token`) if a future script automates it.
`cp_probe.py` exit code tells you in one line whether the current token is
still alive.

**Prerequisite (one-time per dev app):** the dev.twitch.tv app being used
(https://dev.twitch.tv/console) must have OAuth Redirect URL exactly:

```
http://localhost:3000/
```

...**trailing slash included**. Twitch compares that string byte-for-byte
with what cp_auth.py sends; `localhost:3000` without the slash, `127.0.0.1`
instead of `localhost`, or `https` all fail with "redirect mismatch".

### Step 1 — paste the two credentials into `cp_config.ini`

Open `tools/channel_points/cp_config.ini` in Notepad and fill in (from the
"test 36" Manage page):

```
[eventsub]
client_id = <the Client ID shown on the manage page>

[auth]
client_secret = <click "New Secret", paste the value>
```

(Both are already filled in on this machine. Alternative without editing the
file: `python tools\channel_points\cp_auth.py --client-id <ID> --client-secret <SECRET>`
— the values get written into cp_config.ini for you.)

### Step 2 — run the auth script

```
python tools\channel_points\cp_auth.py
```

It prints the authorization link **and copies it to your clipboard** (no
browser opens by itself). Paste the link into a browser, sign in as **the
account that should own the redemptions** (the streamer's account), click
**Authorize**. The script is waiting the whole time: Twitch redirects to
`http://localhost:3000/?code=...`, the loopback-only catcher (accepts
127.0.0.1/::1 only) grabs the code, exchanges it for a user access token,
validates it, and writes `token` / `expires_at` / `refresh_token` / `login`
(the signing account — a first-class config fact) into `cp_config.ini`
**atomically**. The token and secret are never printed or logged — they live
only in that file.

### Step 3 — done; verify anytime

```
python tools\channel_points\cp_probe.py
```

Exit code 0 + `OK: Channel Points token is live...` = good. Then start the
bridge with `python tools\channel_points\cp_listen.py` (game running,
`drop=1` already set in `twitch_config.txt`) and redeem a reward for real.
Remember: ~4-hour token lifetime — re-run Step 2 before each stream (or
renew via `refresh_token`). The channel is resolved args → `[auth] channel`
→ `twitch_config.txt` → **the token's own account** (empty channel is fine);
a channel/token mismatch prints a loud warning because EventSub will 403.

### Troubleshooting

| Symptom | Meaning / fix |
|---|---|
| `MISSING CREDENTIALS` from cp_auth | `client_id`/`client_secret` empty in cp_config.ini — do Step 1. |
| `redirect mismatch` on the Twitch page | The app's redirect isn't the exact string above — fix it in the "test 36" Manage page. |
| `cannot listen on port 3000` | Something else owns port 3000 — close it, re-run. |
| cp_probe: `token rejected (HTTP 401)` | Token expired/revoked — re-run Step 2. |
| cp_listen: `HTTP 403` | Token lacks `channel:read:redemptions` — re-run Step 2 (scope is fixed in the script). |

---

# Files

| File | Purpose |
|---|---|
| `cp_auth.py` | **The one-command unlock** (runbook Step 2). Stdlib-only. Loopback-only catcher on port 3000, code->token exchange, token validation, then atomic config write of `token`/`expires_at`/`refresh_token`/`login` (whoever signed in — account-agnostic). `--client-id/--client-secret` args write themselves into the ini; `--config` to point at another file. Never prints token/secret. |
| `cp_probe.py` | Health check. Validates the stored token, prints login/scopes/expiry (safe), flags a stale `[auth] login`, exit 0/1. Empty token -> `no token — run cp_auth.py`. |
| `cp_config.ini` | Credentials. **Holds secrets once filled — never commit/paste/screenshot.** `cp_auth.py` writes `[auth] token/expires_at/refresh_token/login`; `cp_listen.py` reads `[eventsub] client_id` + `[auth] token/login/channel`. Key lookup in cp_auth/cp_probe scans all sections, so exact placement is flexible. |
| `cp_listen.py` | The bridge: EventSub websocket (hand-rolled RFC6455, stdlib-only) -> `cp_map.ini` -> drop files. Channel resolution: `--channel` arg -> `[auth] channel` -> `twitch_config.txt` -> **token's own account** (`[auth] login`, or derived from token validation; `--mock`/`--check` stay offline). Warns loudly on channel/token mismatch. `--check` sanity pass, `--mock` + `test_server_mock.py` for the fully offline pipeline test. |
| `cp_map.ini` | Reward name -> verb table — **lane's own catalog** (intended content, not a hardcode); unknown rewards logged+ignored. |
| `cp_map.ini.example` | Empty-but-valid starting point for a fresh deployment (grammar + sample rows commented). |
| `cp_config.ini.example` | Fully annotated template incl. the optional listener keys (`types`, `wss_url`, `drop.dir`, ...). |
| `test_server_mock.py` | Offline fake EventSub + Helix server; proves the whole listen path with zero credentials. |

(`auth_catch.py` was removed — superseded by `cp_auth.py`, which additionally
copies the link to the clipboard, refuses non-loopback peers, validates the
token, and writes a config layout `cp_listen.py` can parse.)

# Scope & token facts (verified against dev.twitch.tv 2026-09-09/10)

- One scope needed: **`channel:read:redemptions`** (there is **no**
  `points:read` scope; `channel:manage:redemptions` is not needed — we act on
  redemptions, we don't approve/refund them). It authorizes BOTH subscription
  types below.
- Token lifetime: **~4 hours** for user access tokens (`expires_in` ≈ 14400 s
  — verified live 2026-09-10). `cp_config.ini [auth] expires_at` records the
  death time; the before-each-stream ritual is re-running cp_auth.py.
  `refresh_token` is stored for renewal (`POST id.twitch.tv/oauth2/token`,
  `grant_type=refresh_token` + `client_id` + `client_secret` +
  `refresh_token`) — the supported path today is simply re-running cp_auth.
- `[auth] login` records WHO authorized (written by cp_auth.py from the
  validate response). cp_listen treats an empty channel as "follow the
  token" and warns when the configured channel and the token's account
  disagree (EventSub would 403).
- Validation endpoint: `GET https://id.twitch.tv/oauth2/validate` with
  `Authorization: Bearer <token>` — returns login/scopes/expires_in; those
  are the only fields the scripts print.
- Broadcaster-authorized websocket subscriptions cost 0 points; limits that
  exist (300 enabled subs/socket, 3 concurrent sockets, max_total_cost 10)
  are far above our 2 subscriptions.

# Subscription types

| Type | Version | Scope | Condition | Payload of interest |
|---|---|---|---|---|
| `channel.channel_points_custom_reward_redemption.add` | 1 | `channel:read:redemptions` | `broadcaster_user_id` | `reward.title`, `reward.cost`, `user_input`, `status` |
| `channel.channel_points_automatic_reward_redemption.add` | 1 | same | `broadcaster_user_id` | `reward.type` (e.g. `send_highlighted_message`), `message.text` |

The streamer's catalog (Arise Chicken!!, Flashbang, ...) is **custom** rewards, so
the custom type is the one that matters. Automatic rewards have no human
name (only a type enum) and are disabled by default in `cp_map.ini`.

# Mapping reference (`cp_map.ini`)

| Reward | Verb |
|---|---|
| Arise Chicken!! (ATHF) | `arise` |
| Flashbang the Stream | `flip` |
| Duck Vanishes / Ducks Running In | `spawn raven 2` (cap 6) |
| So Disappointing | `tax 50` (`{input}` variant documented in-file) |
| Look At Us | `party` (confuse + flip) |
| Commence the Jigglin' | `party` (`dance` is not a game verb — intentionally not mapped) |

Grammar: `verb [arg words...] [@frames]`, `{input}` = redeemer's message,
`-` disables a mapping. Matching is case/whitespace-insensitive. The game
re-validates every verb and its own gates, so a bad mapping can't crash
anything — it just prints `RESULT verb=... DROPPED`.

# Safety / notes

- cp_auth.py touches nothing but its config file; cp_listen.py writes only
  `twitch_drop/cp_*.txt` (atomic `.tmp`+rename, so the game never reads a
  half-written file). No game or harness edits.
- The catcher binds `127.0.0.1:3000` AND verifies the peer address is
  127.0.0.1/::1 per request (403 otherwise) — loopback-only twice over.
- Drop files bypass the game's IRC cooldown by design (`src/twitch.c`);
  natural throttle = the reward's point cost.
- Chat keeps working exactly as before — this is an additional input path.
- cp_listen protocol trivia (10-second subscribe window, session-scoped
  subs, `session_reconnect` follow, server-ping answering, keepalive
  timeout) is handled inside cp_listen.py and commented there.
