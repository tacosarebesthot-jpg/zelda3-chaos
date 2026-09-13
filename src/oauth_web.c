// In-game Twitch OAuth (implicit grant) via a LOOPBACK-ONLY HTTP listener.
// Concept port of ZALiA's obj_zalia_web (Other_68.gml) + twitch_callback.html
// ("zelda 2 shit/ZALiA_github_src"). Why a server at all: Twitch's implicit
// flow returns the token in the URL FRAGMENT (#access_token=...), which the
// browser never sends to a server - a local page has to read location.hash and
// hand it over. That page is served by the game itself, so no extra files, no
// Python, no second window.
//
// LOOPBACK ONLY (ZALiA rule, kept verbatim in spirit): the listener binds the
// loopback address and the accept path re-checks the peer address, refusing
// anything that is not 127.0.0.1 / ::1 (including the IPv4-mapped ::ffff:
// form Windows hands over on dual-stack sockets). A companion login is not
// worth handing the LAN a remote control.
//
// TOKEN DISCIPLINE: the token appears exactly twice on this machine - in the
// browser's fragment (cleared by history.replaceState) and in
// twitch_config.txt (written atomically, temp + MoveFileEx replace, every
// other byte preserved). It is NEVER logged, echoed, or served back; the
// request line that carries it is parsed in place and never printed.
//
// OWNER DIRECTIVE (2026-09-10): no automatic browser open. RETURN on the
// settings-menu "CONNECT: COPY LINK" row (or GET /twitch/login) prints the
// authorize URL to the console and copies it to the clipboard; the owner
// clicks it at his leisure. The F10 settings screen itself lives in
// settings_menu.c - this file only registers its Twitch rows.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
// winsock2.h must precede anything that pulls in windows.h (SDL does)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef int ow_socklen_t;
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define closesocket close
typedef int ow_socklen_t;
#endif

#include <SDL.h>

#include "oauth_web.h"
#include "settings_menu.h"
#include "twitch.h"

// ------------------------------------------------------------ constants --

#define OW_PORT 8795
// LOCKED by the coordinator 2026-09-10: must match the redirect URL the owner
// registers in the Twitch dev app. Overridable via twitch_config.txt's
// `oauth_redirect=` (OAuthWeb_SetRedirectUri), but the default stays this.
static const char kRedirectDefault[] = "http://localhost:8795/twitch/callback";
static const char kAuthorizeBase[] = "https://id.twitch.tv/oauth2/authorize";

// Token charset: Twitch implicit-grant access tokens are URL-safe base64-ish
// ([A-Za-z0-9_-]). Validating BEFORE the file write is also the guard against
// a rogue local process hitting /twitch/save: a rejected charset can never
// inject a newline, so the live config stays line-shaped.
#define OW_TOKEN_MIN 10
#define OW_TOKEN_MAX 120

// ---------------------------------------------------------------- state --

static SDL_mutex *g_ow_mutex;            // guards everything below
static SDL_Thread *g_ow_thread;
static volatile bool g_ow_running;

static char g_ow_client_id[64];
static char g_ow_redirect[192];

static volatile bool g_ow_listening;     // listener socket is up
static volatile int g_ow_save_state;     // kOAuthSave*
static uint32 g_ow_save_tick;            // when OK/FAILED was set (fade to idle)
static char g_ow_saved_token[128];       // parked for the main-thread poll
static bool g_ow_token_parked;

#ifdef _WIN32
static bool g_ow_wsa_up;
#endif

void OAuthWeb_SetClientId(const char *client_id) {
  if (client_id)
    snprintf(g_ow_client_id, sizeof(g_ow_client_id), "%s", client_id);
}

void OAuthWeb_SetRedirectUri(const char *redirect_uri) {
  if (redirect_uri && redirect_uri[0])
    snprintf(g_ow_redirect, sizeof(g_ow_redirect), "%s", redirect_uri);
}

int OAuthWeb_SaveState(void) {
  int st = g_ow_save_state;
  // OK / FAILED are transient flashes; PENDING sticks until it resolves.
  if ((st == kOAuthSaveOk || st == kOAuthSaveFailed) &&
      SDL_GetTicks() - g_ow_save_tick > 8000) {
    g_ow_save_state = kOAuthSaveNone;
    st = kOAuthSaveNone;
  }
  return st;
}

bool OAuthWeb_Listening(void) { return g_ow_listening; }

bool OAuthWeb_TakeSavedToken(char *out, int cap) {
  bool have = false;
  if (g_ow_mutex)
    SDL_LockMutex(g_ow_mutex);
  if (g_ow_token_parked) {
    snprintf(out, cap, "%s", g_ow_saved_token);
    memset(g_ow_saved_token, 0, sizeof(g_ow_saved_token));  // one reader, but
    g_ow_token_parked = false;                              // burn it anyway
    have = true;
  }
  if (g_ow_mutex)
    SDL_UnlockMutex(g_ow_mutex);
  return have;
}

// ------------------------------------------------------ url + clipboard --

// Build the implicit-grant URL. Never contains the token - safe to print,
// copy, and serve. Returns false only when no client_id is configured.
static bool OAuthBuildUrl(char *out, int cap) {
  if (!g_ow_client_id[0])
    return false;
  snprintf(out, cap,
           "%s?response_type=token&client_id=%s&redirect_uri=%s"
           "&scope=chat:read%%20chat:edit%%20channel:read:redemptions",
           kAuthorizeBase, g_ow_client_id,
           g_ow_redirect[0] ? g_ow_redirect : kRedirectDefault);
  return true;
}

#ifdef _WIN32
// OWNER DIRECTIVE: instead of ShellExecuteW, hand the URL over via the Win32
// clipboard (CF_UNICODETEXT - the modern superset of CF_TEXT; every Windows
// text field pastes it) and print it to the console. The URL is pure ASCII.
static bool CopyTextToClipboard(const char *text) {
  if (!OpenClipboard(NULL))
    return false;
  bool ok = false;
  if (EmptyClipboard()) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (n > 0) {
      HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t));
      if (g) {
        wchar_t *p = (wchar_t *)GlobalLock(g);
        if (p) {
          MultiByteToWideChar(CP_UTF8, 0, text, -1, p, n);
          GlobalUnlock(g);
          if (SetClipboardData(CF_UNICODETEXT, g))
            ok = true;                 // ownership passed to the clipboard
          else
            GlobalFree(g);
        } else {
          GlobalFree(g);
        }
      }
    }
  }
  CloseClipboard();
  return ok;
}
#endif

bool OAuthWeb_CopyConnectLink(void) {
  char url[512];
  if (!OAuthBuildUrl(url, sizeof(url))) {
    printf("[oauth] no client_id in twitch_config.txt - cannot build the "
           "connect link (add client_id=<app id>, see tools/OAUTH_RUNBOOK.md)\n");
    fflush(stdout);
    return false;
  }
  // The URL carries the (public) client_id and the locked redirect - never
  // the token - so printing it is safe.
  printf("[oauth] open this link to connect: %s\n", url);
#ifdef _WIN32
  bool copied = CopyTextToClipboard(url);
  printf("[oauth] link %s\n", copied
         ? "copied to clipboard - paste it into your browser and click "
           "Authorize"
         : "copy FAILED - copy it from the line above by hand");
#else
  bool copied = false;
  printf("[oauth] clipboard copy is Windows-only - copy the link by hand\n");
#endif
  if (g_ow_mutex) {
    SDL_LockMutex(g_ow_mutex);
    g_ow_save_state = kOAuthSavePending; // waiting for the callback round-trip
    SDL_UnlockMutex(g_ow_mutex);
  }
  fflush(stdout);
  return copied;
}

// ------------------------------------------------------- atomic config ----

// Rewrite twitch_config.txt with the token= line replaced, every other byte
// preserved (comments, key order, line endings). ATOMIC: write
// twitch_config.txt.tmp, then MoveFileExA(REPLACE_EXISTING) so a crash or a
// concurrent read can never observe a half-written live config. Runs on the
// listener thread; the game itself only READS the file (at boot).
static bool ConfigWriteToken(const char *bare) {
  FILE *f = fopen("twitch_config.txt", "rb");
  if (!f) {
    printf("[oauth] twitch_config.txt unreadable - token NOT saved\n");
    return false;
  }
  static char old[65536];
  size_t old_len = fread(old, 1, sizeof(old) - 1, f);
  fclose(f);
  old[old_len] = 0;
  if (old_len == 0) {
    printf("[oauth] twitch_config.txt empty - token NOT saved\n");
    return false;
  }

  static char newc[65536 + 160];
  size_t w = 0;
  bool replaced = false;
  size_t i = 0;
  while (i < old_len) {
    size_t ls = i;
    while (i < old_len && old[i] != '\n')
      i++;
    size_t body_end = i;                       // exclusive of '\n'
    size_t leol = i;
    if (leol > ls && old[leol - 1] == '\r')
      leol--;                                  // keep CRLF vs LF as-is
    // key = chars before '=' trimmed
    size_t eq = ls;
    while (eq < leol && old[eq] != '=')
      eq++;
    if (eq < leol && !replaced) {
      size_t a = ls, b = eq;
      while (a < b && (old[a] == ' ' || old[a] == '\t'))
        a++;
      while (b > a && (old[b - 1] == ' ' || old[b - 1] == '\t'))
        b--;
      size_t klen = b - a;
      if (klen == 5 && _strnicmp(old + a, "token", 5) == 0) {
        // swap the VALUE only: "token=oauth:<bare>" + original line ending
        memcpy(newc + w, "token=oauth:", 12);
        w += 12;
        memcpy(newc + w, bare, strlen(bare));
        w += strlen(bare);
        memcpy(newc + w, old + leol, body_end - leol);  // \r if CRLF
        w += body_end - leol;
        if (i < old_len)
          newc[w++] = '\n';                    // the file's own terminator
        replaced = true;
        i++;                                   // past '\n'
        continue;
      }
    }
    // verbatim copy of the whole line including its ending
    memcpy(newc + w, old + ls, i - ls);
    w += i - ls;
    if (i < old_len)
      newc[w++] = '\n';
    i++;
  }
  if (!replaced) {
    // no token line yet (fresh config): append one
    if (w > 0 && newc[w - 1] != '\n')
      newc[w++] = '\n';
    w += (size_t)snprintf(newc + w, sizeof(newc) - w, "token=oauth:%s\n", bare);
  }

  FILE *t = fopen("twitch_config.txt.tmp", "wb");
  if (!t) {
    printf("[oauth] cannot write twitch_config.txt.tmp - token NOT saved\n");
    return false;
  }
  bool ok = fwrite(newc, 1, w, t) == w;
  if (fclose(t) != 0)
    ok = false;
  if (!ok) {
    remove("twitch_config.txt.tmp");
    printf("[oauth] temp write failed - token NOT saved\n");
    return false;
  }
#ifdef _WIN32
  ok = MoveFileExA("twitch_config.txt.tmp", "twitch_config.txt",
                   MOVEFILE_REPLACE_EXISTING) != 0;
  if (!ok)
    remove("twitch_config.txt.tmp");
#else
  ok = rename("twitch_config.txt.tmp", "twitch_config.txt") == 0;
#endif
  return ok;
}

// Set key=value in twitch_config.txt only when the key is missing or has an
// empty value; a streamer's own user=/channel= are never overwritten. Same
// atomic temp-file replace as ConfigWriteToken. Listener thread only.
static bool ConfigSetKeyIfEmpty(const char *key, const char *value) {
  FILE *f = fopen("twitch_config.txt", "rb");
  if (!f)
    return false;
  static char old[65536], newc[65536 + 256];
  size_t old_len = fread(old, 1, sizeof(old) - 1, f);
  fclose(f);
  old[old_len] = 0;
  size_t klen = strlen(key), w = 0, i = 0;
  bool found = false;
  while (i < old_len) {
    size_t ls = i;
    while (i < old_len && old[i] != '\n')
      i++;
    size_t body_end = i, leol = i;
    if (leol > ls && old[leol - 1] == '\r')
      leol--;
    size_t eq = ls;
    while (eq < leol && old[eq] != '=')
      eq++;
    if (eq < leol && !found) {
      size_t a = ls, b = eq;
      while (a < b && (old[a] == ' ' || old[a] == '\t')) a++;
      while (b > a && (old[b - 1] == ' ' || old[b - 1] == '\t')) b--;
      if (b - a == klen && _strnicmp(old + a, key, klen) == 0) {
        found = true;
        size_t v = eq + 1;
        while (v < leol && (old[v] == ' ' || old[v] == '\t')) v++;
        if (v == leol) {                     // empty value: fill it in
          w += (size_t)snprintf(newc + w, sizeof(newc) - w, "%s=%s", key, value);
          memcpy(newc + w, old + leol, body_end - leol);
          w += body_end - leol;
          if (i < old_len) newc[w++] = '\n';
          i++;
          continue;
        }
      }
    }
    memcpy(newc + w, old + ls, i - ls);
    w += i - ls;
    if (i < old_len) newc[w++] = '\n';
    i++;
  }
  if (!found) {
    if (w > 0 && newc[w - 1] != '\n') newc[w++] = '\n';
    w += (size_t)snprintf(newc + w, sizeof(newc) - w, "%s=%s\n", key, value);
  }
  FILE *t = fopen("twitch_config.txt.tmp", "wb");
  if (!t)
    return false;
  bool ok = fwrite(newc, 1, w, t) == w;
  if (fclose(t) != 0) ok = false;
  if (!ok) { remove("twitch_config.txt.tmp"); return false; }
#ifdef _WIN32
  ok = MoveFileExA("twitch_config.txt.tmp", "twitch_config.txt",
                   MOVEFILE_REPLACE_EXISTING) != 0;
  if (!ok) remove("twitch_config.txt.tmp");
#else
  ok = rename("twitch_config.txt.tmp", "twitch_config.txt") == 0;
#endif
  return ok;
}

// -------------------------------------------------------- tiny http get --

static void PercentDecode(const char *in, char *out, int cap) {
  int o = 0;
  for (; *in && o < cap - 1; in++) {
    if (*in == '%' && isxdigit((unsigned char)in[1]) &&
        isxdigit((unsigned char)in[2])) {
      char hex[3] = { in[1], in[2], 0 };
      out[o++] = (char)strtol(hex, NULL, 16);
      in += 2;
    } else {
      out[o++] = *in;
    }
  }
  out[o] = 0;
}

// Extract one query parameter into out (percent-decoded). Returns out, or
// NULL when the parameter is absent.
static const char *QueryParam(const char *query, const char *key,
                              char *out, int cap) {
  size_t klen = strlen(key);
  const char *p = query;
  while (p && *p) {
    const char *seg = p;
    const char *amp = strchr(seg, '&');
    size_t seglen = amp ? (size_t)(amp - seg) : strlen(seg);
    if (seglen > klen && strncmp(seg, key, klen) == 0 && seg[klen] == '=') {
      char raw[256];
      size_t n = seglen - klen - 1;
      if (n >= sizeof(raw))
        n = sizeof(raw) - 1;
      memcpy(raw, seg + klen + 1, n);
      raw[n] = 0;
      PercentDecode(raw, out, cap);
      return out;
    }
    p = amp ? amp + 1 : NULL;
  }
  return NULL;
}

// Callback page - port of ZALiA's datafiles/web/twitch_callback.html, inlined
// so the game ships no datafiles. The token stays in the fragment: this
// page's JS reads location.hash, hands it to /twitch/save via fetch, then
// wipes the address bar. Nothing token-shaped is ever rendered into the DOM.
static const char kCallbackHtml[] =
"<!doctype html>\n"
"<html><head><meta charset=\"utf-8\"><title>zelda3 - Twitch connected</title>\n"
"<style>\n"
":root { color-scheme: dark; }\n"
"body { margin:0; font:15px ui-monospace,Consolas,monospace; background:#2b2f4a; color:#e8e8f0;\n"
"       display:flex; align-items:center; justify-content:center; height:100vh; text-align:center; }\n"
".box { max-width:520px; padding:30px; }\n"
"h1 { font-size:17px; letter-spacing:.06em; margin:0 0 14px; }\n"
".ok  { color:#8ef7b0; }\n"
".bad { color:#ff9a9a; }\n"
"p { opacity:.75; line-height:1.6; }\n"
"</style></head><body>\n"
"<div class=\"box\">\n"
"  <h1 id=\"h\">FINISHING UP&#8230;</h1>\n"
"  <p id=\"m\">Handing the token to zelda3.</p>\n"
"</div>\n"
"<script>\n"
"(function () {\n"
"  const frag = new URLSearchParams(location.hash.replace(/^#/, ''));\n"
"  const qs   = new URLSearchParams(location.search);\n"
"  const h = document.getElementById('h'), m = document.getElementById('m');\n"
"  const err = frag.get('error_description') || frag.get('error')\n"
"           || qs.get('error_description')   || qs.get('error');\n"
"  if (err) {\n"
"    h.textContent = 'NOT CONNECTED';\n"
"    h.className = 'bad';\n"
"    m.textContent = err + ' - nothing was saved. Close this tab and try again.';\n"
"    return;\n"
"  }\n"
"  const token = frag.get('access_token');\n"
"  if (!token) {\n"
"    h.textContent = 'NOTHING TO SAVE';\n"
"    h.className = 'bad';\n"
"    m.textContent = 'Twitch did not return a token. Close this tab and try again.';\n"
"    return;\n"
"  }\n"
"  // Ask Twitch who this token belongs to, so the game can fill in user= and\n"
"  // channel= by itself (nothing for the streamer to type). validate supports\n"
"  // CORS; if it fails for any reason the token is still saved without it.\n"
"  const save = function (login) {\n"
"    return fetch('/twitch/save?token=' + encodeURIComponent(token)\n"
"                 + (login ? '&login=' + encodeURIComponent(login) : ''));\n"
"  };\n"
"  fetch('https://id.twitch.tv/oauth2/validate', { headers: { 'Authorization': 'OAuth ' + token } })\n"
"    .then(function (r) { return r.ok ? r.json() : null; })\n"
"    .then(function (j) { return save(j && j.login ? j.login : ''); },\n"
"          function () { return save(''); })\n"
"    .then(function (r) { return r.text(); })\n"
"    .then(function () {\n"
"      history.replaceState(null, '', '/twitch/callback');\n"
"      h.textContent = 'CONNECTED';\n"
"      h.className = 'ok';\n"
"      m.textContent = 'zelda3 has your chat token. You can close this tab and go back to the game.';\n"
"    })\n"
"    .catch(function () {\n"
"      h.textContent = 'COULD NOT REACH THE GAME';\n"
"      h.className = 'bad';\n"
"      m.textContent = 'Is zelda3 still running? Nothing was saved.';\n"
"    });\n"
"})();\n"
"</script></body></html>\n";

static void HttpRespond(SOCKET c, const char *status, const char *ctype,
                        const char *body, size_t len) {
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "HTTP/1.1 %s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %lu\r\n"
                    "Cache-Control: no-store\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    status, ctype, (unsigned long)len);
  if (hn > 0)
    send(c, head, hn, 0);
  if (len)
    send(c, body, (int)len, 0);
}

// Handle one connection (listener thread). Emits only safe text; the token
// from /twitch/save is consumed in place and never copied into any log path.
static void HandleConnection(SOCKET c) {
#ifdef _WIN32
  DWORD rto = 5000;
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rto, sizeof(rto));
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&rto, sizeof(rto));
#endif

  char req[4096];
  int rl = 0;
  bool have_header = false;
  while (rl < (int)sizeof(req) - 1) {
    int n = recv(c, req + rl, (int)sizeof(req) - 1 - rl, 0);
    if (n <= 0)
      break;
    rl += n;
    req[rl] = 0;
    if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) {
      have_header = true;
      break;
    }
  }
  if (!have_header) {
    HttpRespond(c, "400 Bad Request", "text/plain", "bad request", 11);
    return;
  }

  if (strncmp(req, "GET ", 4) != 0) {
    HttpRespond(c, "405 Method Not Allowed", "text/plain", "GET only", 8);
    return;
  }
  char *target = req + 4;
  char *sp = strchr(target, ' ');
  if (!sp) {
    HttpRespond(c, "400 Bad Request", "text/plain", "bad request", 11);
    return;
  }
  *sp = 0;

  char *query = strchr(target, '?');
  if (query)
    *query++ = 0;

  if (!strcmp(target, "/twitch/login") || !strcmp(target, "/twitch/link")) {
    // Same code path as the settings-menu row: print + clipboard, NO browser
    // launch (owner directive).
    OAuthWeb_CopyConnectLink();
    {
      static const char kOk[] = "connect link printed to the game console and "
                                "copied to the clipboard";
      static const char kNo[] = "no client_id";
      const char *body = g_ow_client_id[0] ? kOk : kNo;
      HttpRespond(c, "200 OK", "text/plain", body, strlen(body));
    }
    return;
  }
  if (!strcmp(target, "/twitch/callback")) {
    HttpRespond(c, "200 OK", "text/html; charset=utf-8", kCallbackHtml,
                strlen(kCallbackHtml));
    return;
  }
  if (!strcmp(target, "/twitch/save")) {
    char tok[160], dec[160];
    if (!QueryParam(query, "token", tok, sizeof(tok)) || tok[0] == 0) {
      HttpRespond(c, "200 OK", "text/plain", "no token", 8);
      return;
    }
    // Charset gate BEFORE anything touches the config (see OW_TOKEN_* above).
    bool valid = strlen(tok) >= OW_TOKEN_MIN && strlen(tok) <= OW_TOKEN_MAX;
    for (const char *p = tok; valid && *p; p++)
      if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
        valid = false;
    if (!valid) {
      printf("[oauth] /twitch/save rejected a malformed token (len %d)\n",
             (int)strlen(tok));
      fflush(stdout);
      if (g_ow_mutex) {
        SDL_LockMutex(g_ow_mutex);
        g_ow_save_state = kOAuthSaveFailed;
        g_ow_save_tick = SDL_GetTicks();
        SDL_UnlockMutex(g_ow_mutex);
      }
      HttpRespond(c, "200 OK", "text/plain", "invalid token", 13);
      return;
    }
    PercentDecode(tok, dec, sizeof(dec));
    // Optional login= (from id.twitch.tv/oauth2/validate in the callback
    // page): fills user= and channel= when they are empty, so a fresh
    // install needs nothing typed. Twitch logins are [a-z0-9_]{1,25}.
    char login[40] = {0}, login_dec[40] = {0};
    if (QueryParam(query, "login", login, sizeof(login)) && login[0]) {
      PercentDecode(login, login_dec, sizeof(login_dec));
      bool lok = strlen(login_dec) >= 1 && strlen(login_dec) <= 25;
      for (const char *p = login_dec; lok && *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
          lok = false;
      if (!lok)
        login_dec[0] = 0;
    }
    if (ConfigWriteToken(dec)) {
      if (login_dec[0]) {
        ConfigSetKeyIfEmpty("user", login_dec);
        ConfigSetKeyIfEmpty("channel", login_dec);
        printf("[oauth] user/channel filled from the token's login\n");
      }
      if (g_ow_mutex) {
        SDL_LockMutex(g_ow_mutex);
        snprintf(g_ow_saved_token, sizeof(g_ow_saved_token), "%s", dec);
        memset(dec, 0, sizeof(dec));     // burn the stack copy; the parked one
        g_ow_token_parked = true;        // is taken + zeroed by the main thread
        g_ow_save_state = kOAuthSaveOk;
        g_ow_save_tick = SDL_GetTicks();
        SDL_UnlockMutex(g_ow_mutex);
      }
      printf("[oauth] token saved to twitch_config.txt (len %d) - handing it "
             "to the twitch module\n", (int)strlen(g_ow_saved_token));
      fflush(stdout);
      HttpRespond(c, "200 OK", "text/plain", "saved", 5);
    } else {
      if (g_ow_mutex) {
        SDL_LockMutex(g_ow_mutex);
        g_ow_save_state = kOAuthSaveFailed;
        g_ow_save_tick = SDL_GetTicks();
        SDL_UnlockMutex(g_ow_mutex);
      }
      HttpRespond(c, "200 OK", "text/plain", "save failed", 11);
    }
    return;
  }
  if (!strcmp(target, "/twitch/status") || !strcmp(target, "/twitch")) {
    // Booleans only (ZALiA rule): a token echoed to any page is a token that
    // ends up in a stream screenshot.
    char body[32];
    int n = snprintf(body, sizeof(body), "{\"has_token\":%s}",
                     Twitch_HasToken() ? "true" : "false");
    HttpRespond(c, "200 OK", "application/json", body, (size_t)n);
    return;
  }
  HttpRespond(c, "404 Not Found", "text/plain", "no such page", 12);
}

// LOOPBACK REFUSAL: accept-time peer check, the Winsock twin of ZALiA's
// network_type_connect guard. The listen socket is also bound to loopback
// only, so this is belt and suspenders - but the bind is what a browser on
// the LAN hits first, and this check is what a spoofed/dual-stack surprise
// hits second.
static bool PeerIsLoopback(SOCKET c) {
  struct sockaddr_storage ss;
  ow_socklen_t sl = (ow_socklen_t)sizeof(ss);
  if (getpeername(c, (struct sockaddr *)&ss, &sl) != 0)
    return false;
  if (ss.ss_family == AF_INET) {
    struct sockaddr_in *a = (struct sockaddr_in *)&ss;
    return ntohl(a->sin_addr.s_addr) == 0x7f000001;   // 127.0.0.1
  }
  if (ss.ss_family == AF_INET6) {
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
#ifdef _WIN32
    if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr))
      return ntohl(*(const uint32_t *)&a6->sin6_addr.s6_bytes[12]) == 0x7f000001;
#else
    if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr))
      return ntohl(*(const uint32_t *)&a6->sin6_addr.s6_addr[12]) == 0x7f000001;
#endif
    return IN6_IS_ADDR_LOOPBACK(&a6->sin6_addr) != 0; // ::1
  }
  return false;
}

static int OAuthThreadFunc(void *unused) {
  (void)unused;
  // Two loopback listeners, IPv4 and IPv6: browsers resolve "localhost" to
  // either family and do not always fall back to the other.  The old code
  // bound ::1 only (the V6ONLY=0 trick only helps when bound to ::), so a
  // browser that tried 127.0.0.1 got "unable to connect" on the callback.
  SOCKET socks[2] = { INVALID_SOCKET, INVALID_SOCKET };
  int nsocks = 0;
  {
    SOCKET s4 = socket(AF_INET, SOCK_STREAM, 0);
    if (s4 != INVALID_SOCKET) {
      struct sockaddr_in a4;
      memset(&a4, 0, sizeof(a4));
      a4.sin_family = AF_INET;
      a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      a4.sin_port = htons(OW_PORT);
      if (bind(s4, (struct sockaddr *)&a4, sizeof(a4)) == 0 && listen(s4, 4) == 0)
        socks[nsocks++] = s4;
      else
        closesocket(s4);
    }
  }
  {
    SOCKET s6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s6 != INVALID_SOCKET) {
      int yes = 1;
      setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&yes, sizeof(yes));
      struct sockaddr_in6 a6;
      memset(&a6, 0, sizeof(a6));
      a6.sin6_family = AF_INET6;
      a6.sin6_addr = in6addr_loopback;
      a6.sin6_port = htons(OW_PORT);
      if (bind(s6, (struct sockaddr *)&a6, sizeof(a6)) == 0 && listen(s6, 4) == 0)
        socks[nsocks++] = s6;
      else
        closesocket(s6);
    }
  }
  if (nsocks == 0) {
    printf("[oauth] port %d busy - in-game login disabled (the IRC client "
           "keeps using the existing token)\n", OW_PORT);
    fflush(stdout);
    return 0;
  }
  for (int i = 0; i < nsocks; i++) {
#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(socks[i], FIONBIO, &nonblocking);
#else
    fcntl(socks[i], F_SETFL, O_NONBLOCK);
#endif
  }
  g_ow_listening = true;
  printf("[oauth] listening on http://localhost:%d (loopback only, %s)\n", OW_PORT,
         nsocks == 2 ? "IPv4+IPv6" : "one family");
  fflush(stdout);

  while (g_ow_running) {
    bool any = false;
    for (int i = 0; i < nsocks; i++) {
      struct sockaddr_storage peer;
      ow_socklen_t pl = (ow_socklen_t)sizeof(peer);
      SOCKET c = accept(socks[i], (struct sockaddr *)&peer, &pl);
      if (c == INVALID_SOCKET)
        continue;
      any = true;
      if (!PeerIsLoopback(c)) {
        char ip[64] = "?";
        if (peer.ss_family == AF_INET)
          snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
                   ((const uint8_t *)&((struct sockaddr_in *)&peer)->sin_addr)[0],
                   ((const uint8_t *)&((struct sockaddr_in *)&peer)->sin_addr)[1],
                   ((const uint8_t *)&((struct sockaddr_in *)&peer)->sin_addr)[2],
                   ((const uint8_t *)&((struct sockaddr_in *)&peer)->sin_addr)[3]);
        else if (peer.ss_family == AF_INET6)
          snprintf(ip, sizeof(ip), "ipv6");
        printf("[oauth] refused non-loopback peer %s\n", ip);
        fflush(stdout);
        closesocket(c);
        continue;
      }
      HandleConnection(c);
      closesocket(c);
    }
    if (!any)
      SDL_Delay(50);                     // non-blocking: idle poll
  }

  for (int i = 0; i < nsocks; i++)
    closesocket(socks[i]);
  g_ow_listening = false;
  return 0;
}

// ------------------------------------------------ settings-menu row glue --
// Defined at the bottom of the file; forward-declared for OAuthWeb_Start.

static uint32 g_ow_link_copied_tick;
const char *TwRowChatValue(void);
void TwRowChatAdjust(int delta);
const char *TwRowLinkValue(void);
void TwRowLinkAdjust(int delta);

void OAuthWeb_Start(void) {
  if (g_ow_thread)
    return;
  // The mutex guards the token handoff and the status flags. It was never
  // created, so every `if (g_ow_mutex)` block was skipped: the token landed
  // on disk but the RUNNING game never received it (chat/channel points only
  // came up after a restart) and the MODS row never showed PENDING/saved.
  if (!g_ow_mutex)
    g_ow_mutex = SDL_CreateMutex();
  if (!g_ow_redirect[0])
    snprintf(g_ow_redirect, sizeof(g_ow_redirect), "%s", kRedirectDefault);

  // Settings-menu rows (registered once; settings_menu.h owns the screen).
  static bool rows_registered;
  if (!rows_registered) {
    static const SettingsRow kRowChat = { "TWITCH CHAT", TwRowChatValue,
                                          TwRowChatAdjust };
    static const SettingsRow kRowLink = { "CONNECT: COPY LINK",
                                          TwRowLinkValue, TwRowLinkAdjust };
    SettingsMenu_RegisterRow(&kRowChat);
    SettingsMenu_RegisterRow(&kRowLink);
    rows_registered = true;
  }
  g_ow_running = true;
#ifdef _WIN32
  if (!g_ow_wsa_up) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
      g_ow_wsa_up = true;
  }
  if (!g_ow_wsa_up) {
    printf("[oauth] WSAStartup failed - in-game login disabled\n");
    return;
  }
#endif
  g_ow_thread = SDL_CreateThread(OAuthThreadFunc, "oauth_web", NULL);
}

void OAuthWeb_Stop(void) {
  if (g_ow_thread) {
    g_ow_running = false;
    SDL_WaitThread(g_ow_thread, NULL);   // exits within ~50 ms (non-blocking)
    g_ow_thread = NULL;
  }
#ifdef _WIN32
  if (g_ow_wsa_up) {
    WSACleanup();
    g_ow_wsa_up = false;
  }
#endif
}

// ------------------------------------------------ settings-menu row glue --

const char *TwRowChatValue(void) {
  int st = OAuthWeb_SaveState();
  if (st == kOAuthSavePending)
    return "TOKEN PENDING";
  if (st == kOAuthSaveOk)
    return "TOKEN SAVED";
  if (st == kOAuthSaveFailed)
    return "SAVE FAILED";
  return Twitch_IsConnected() ? "CONNECTED" : "NOT CONNECTED";
}

void TwRowChatAdjust(int delta) {
  (void)delta;                           // informational row: nothing to adjust
}

const char *TwRowLinkValue(void) {
  if (!g_ow_client_id[0])
    return "NO CLIENT_ID";
  if (SDL_GetTicks() - g_ow_link_copied_tick < 3000)
    return "LINK COPIED!";
  return "PRESS RETURN";
}

void TwRowLinkAdjust(int delta) {
  if (delta != 0)
    return;                              // action row: left/right do nothing
  if (OAuthWeb_CopyConnectLink())
    g_ow_link_copied_tick = SDL_GetTicks();
}
