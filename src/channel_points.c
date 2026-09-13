// In-game Channel Points: see channel_points.h. Windows-only (WinHTTP); on
// other platforms every entry point is a no-op that reports OFF.
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
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#include <SDL.h>

#include "channel_points.h"
#include "twitch.h"
#include "rando/rando_json.h"

// ------------------------------------------------------------ mapping ----

typedef struct CpVerb { const char *verb; const char *arg; const char *label; } CpVerb;
static const CpVerb kVerbs[] = {
  { "", "", "NONE" },
  { "heal", "", "HEAL" }, { "hurt", "", "HURT" }, { "refill", "", "REFILL" },
  { "mp", "", "MAGIC" }, { "drain", "", "DRAIN" }, { "rupees", "", "RUPEES" },
  { "tax", "", "TAX" }, { "bombs", "", "BOMBS" }, { "arrows", "", "ARROWS" },
  { "speed", "", "SPEED" }, { "slow", "", "SLOW" }, { "ice", "", "ICE" },
  { "flip", "", "FLIP" }, { "confuse", "", "CONFUSE" }, { "party", "", "PARTY" },
  { "freeze", "", "FREEZE" }, { "curse", "", "CURSE" }, { "root", "", "ROOT" },
  { "steal", "", "STEAL" }, { "deny", "", "DENY" }, { "denyboots", "", "DENY BOOTS" },
  { "illusion", "", "BUNNY" }, { "arise", "", "CUCCO" }, { "fairy", "", "FAIRY" },
  { "smite", "", "SMITE" }, { "swarm", "", "SWARM" }, { "spawn", "keese 3", "SPAWN" },
};
#define kVerbCount ((int)(sizeof(kVerbs) / sizeof(kVerbs[0])))

#define CP_MAX_REWARDS 32
typedef struct CpReward {
  char id[80];
  char title[64];
  int verb;              // index into kVerbs, 0 = NONE
} CpReward;

static SDL_mutex *g_cp_mutex;
static CpReward g_cp_rewards[CP_MAX_REWARDS];
static int g_cp_n;
static volatile int g_cp_status = kCpOff;
static SDL_Thread *g_cp_thread;
static volatile bool g_cp_running;
static volatile bool g_cp_token_changed;

static void Upper(const char *in, char *out, size_t cap) {
  size_t n = 0;
  for (; in[n] && n + 1 < cap; n++)
    out[n] = (char)toupper((unsigned char)in[n]);
  out[n] = 0;
}

// title -> verb guess ("!speed" / "speed boost" / "ARISE Chicken" ...); only
// used when a reward has no saved mapping. The screen overrides it.
static int GuessVerb(const char *title) {
  char t[128];
  size_t n = 0;
  for (; title[n] && n + 1 < sizeof(t); n++)
    t[n] = (char)tolower((unsigned char)title[n]);
  t[n] = 0;
  static const struct { const char *word; const char *verb; } kAlias[] = {
    { "chicken", "arise" }, { "cucco", "arise" }, { "bunny", "illusion" },
    { "magic", "mp" }, { "invert", "flip" }, { "mirror", "flip" },
  };
  for (int i = 1; i < kVerbCount; i++)
    if (strstr(t, kVerbs[i].verb))
      return i;
  for (size_t i = 0; i < sizeof(kAlias) / sizeof(kAlias[0]); i++)
    if (strstr(t, kAlias[i].word))
      for (int v = 1; v < kVerbCount; v++)
        if (!strcmp(kVerbs[v].verb, kAlias[i].verb))
          return v;
  return 0;
}

static void MapSave(void) {  // called with the mutex held
  FILE *f = fopen("cp_rewards.ini.tmp", "wb");
  if (!f) return;
  fputs("# written by the game (pause -> MODS -> REWARDS); reward id|verb|title\n", f);
  for (int i = 0; i < g_cp_n; i++)
    fprintf(f, "%s|%s|%s\n", g_cp_rewards[i].id, kVerbs[g_cp_rewards[i].verb].verb,
            g_cp_rewards[i].title);
  fclose(f);
#ifdef _WIN32
  if (!MoveFileExA("cp_rewards.ini.tmp", "cp_rewards.ini", MOVEFILE_REPLACE_EXISTING))
    remove("cp_rewards.ini.tmp");
#else
  rename("cp_rewards.ini.tmp", "cp_rewards.ini");
#endif
}

static void MapLoad(void) {
  FILE *f = fopen("cp_rewards.ini", "r");
  if (!f) return;
  char line[256];
  SDL_LockMutex(g_cp_mutex);
  while (fgets(line, sizeof(line), f) && g_cp_n < CP_MAX_REWARDS) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
    char *p1 = strchr(line, '|'); if (!p1) continue;
    *p1++ = 0;
    char *p2 = strchr(p1, '|'); if (!p2) continue;
    *p2++ = 0;
    CpReward *r = &g_cp_rewards[g_cp_n];
    snprintf(r->id, sizeof(r->id), "%s", line);
    snprintf(r->title, sizeof(r->title), "%s", p2);
    r->verb = 0;
    for (int v = 0; v < kVerbCount; v++)
      if (!strcmp(kVerbs[v].verb, p1)) r->verb = v;
    g_cp_n++;
  }
  SDL_UnlockMutex(g_cp_mutex);
  fclose(f);
}

// add or refresh a reward (worker thread); returns its index
static int RewardUpsert(const char *id, const char *title) {
  SDL_LockMutex(g_cp_mutex);
  int idx = -1;
  for (int i = 0; i < g_cp_n; i++)
    if (!strcmp(g_cp_rewards[i].id, id)) { idx = i; break; }
  if (idx < 0 && g_cp_n < CP_MAX_REWARDS) {
    idx = g_cp_n++;
    CpReward *r = &g_cp_rewards[idx];
    snprintf(r->id, sizeof(r->id), "%s", id);
    r->title[0] = 0;
    r->verb = GuessVerb(title);
    snprintf(r->title, sizeof(r->title), "%s", title);
    MapSave();
  } else if (idx >= 0 && title[0] && strcmp(g_cp_rewards[idx].title, title) != 0) {
    snprintf(g_cp_rewards[idx].title, sizeof(g_cp_rewards[idx].title), "%s", title);
    MapSave();
  }
  SDL_UnlockMutex(g_cp_mutex);
  return idx;
}

// ------------------------------------------------------------- public ----

int CP_Status(void) { return g_cp_status; }

const char *CP_StatusText(void) {
  switch (g_cp_status) {
  case kCpConnecting:  return "CONNECTING";
  case kCpOnline:      return "ONLINE";
  case kCpRetrying:    return "RETRYING";
  case kCpNeedConnect: return "RECONNECT";
  default:             return "OFF";
  }
}

int CP_RewardCount(void) {
  SDL_LockMutex(g_cp_mutex);
  int n = g_cp_n;
  SDL_UnlockMutex(g_cp_mutex);
  return n;
}

const char *CP_RewardTitle(int i) {
  static char buf[32];
  buf[0] = 0;
  SDL_LockMutex(g_cp_mutex);
  if (i >= 0 && i < g_cp_n) Upper(g_cp_rewards[i].title, buf, 25);
  SDL_UnlockMutex(g_cp_mutex);
  // the HUD font only has caps, digits and space: blank anything else
  for (char *p = buf; *p; p++)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) *p = ' ';
  return buf;
}

const char *CP_RewardVerb(int i) {
  SDL_LockMutex(g_cp_mutex);
  const char *s = (i >= 0 && i < g_cp_n) ? kVerbs[g_cp_rewards[i].verb].label : "NONE";
  SDL_UnlockMutex(g_cp_mutex);
  return s;
}

void CP_RewardCycle(int i, int delta) {
  SDL_LockMutex(g_cp_mutex);
  if (i >= 0 && i < g_cp_n) {
    int v = g_cp_rewards[i].verb + (delta >= 0 ? 1 : kVerbCount - 1);
    g_cp_rewards[i].verb = v % kVerbCount;
    MapSave();
    printf("[cp] reward '%s' -> %s\n", g_cp_rewards[i].title,
           kVerbs[g_cp_rewards[i].verb].verb[0] ? kVerbs[g_cp_rewards[i].verb].verb : "none");
  }
  SDL_UnlockMutex(g_cp_mutex);
}

#ifdef _WIN32
// --------------------------------------------------------------- http ----

typedef struct CpUrl { wchar_t host[128]; wchar_t path[256]; INTERNET_PORT port; bool secure; } CpUrl;

static bool ParseUrl(const char *url, CpUrl *u) {
  const char *p = url;
  u->secure = true; u->port = 443;
  if (!strncmp(p, "https://", 8)) { p += 8; }
  else if (!strncmp(p, "wss://", 6)) { p += 6; }
  else if (!strncmp(p, "http://", 7)) { p += 7; u->secure = false; u->port = 80; }
  else if (!strncmp(p, "ws://", 5)) { p += 5; u->secure = false; u->port = 80; }
  else return false;
  char host[128]; size_t n = 0;
  while (*p && *p != '/' && *p != ':' && n + 1 < sizeof(host)) host[n++] = *p++;
  host[n] = 0;
  if (*p == ':') { u->port = (INTERNET_PORT)atoi(p + 1); while (*p && *p != '/') p++; }
  const char *path = *p ? p : "/";
  MultiByteToWideChar(CP_UTF8, 0, host, -1, u->host, 128);
  MultiByteToWideChar(CP_UTF8, 0, path, -1, u->path, 256);
  return host[0] != 0;
}

static HINTERNET g_cp_session;

// One Helix call. Returns the HTTP status (0 = transport failure); body into out.
static int HttpCall(const char *method, const CpUrl *base, const char *path,
                    const char *token, const char *client_id, const char *body,
                    char *out, size_t cap) {
  out[0] = 0;
  HINTERNET c = WinHttpConnect(g_cp_session, base->host, base->port, 0);
  if (!c) return 0;
  wchar_t wpath[512], wmethod[8];
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);
  MultiByteToWideChar(CP_UTF8, 0, method, -1, wmethod, 8);
  HINTERNET r = WinHttpOpenRequest(c, wmethod, wpath, NULL, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   base->secure ? WINHTTP_FLAG_SECURE : 0);
  int status = 0;
  if (r) {
    char hdr[512];
    snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s\r\nClient-Id: %s\r\nContent-Type: application/json\r\n",
             token, client_id);
    wchar_t whdr[512];
    MultiByteToWideChar(CP_UTF8, 0, hdr, -1, whdr, 512);
    DWORD blen = body ? (DWORD)strlen(body) : 0;
    if (WinHttpSendRequest(r, whdr, (DWORD)-1, body ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
                           blen, blen, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
      DWORD st = 0, sz = sizeof(st);
      WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
      status = (int)st;
      size_t w = 0;
      for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
        if (w + avail >= cap) avail = (DWORD)(cap - 1 - w);
        if (avail == 0) break;
        if (!WinHttpReadData(r, out + w, avail, &got) || got == 0) break;
        w += got;
      }
      out[w] = 0;
    }
    WinHttpCloseHandle(r);
  }
  WinHttpCloseHandle(c);
  return status;
}

// ------------------------------------------------------------- worker ----

static char g_cp_broadcaster[32];
static char g_cp_login[32];

static bool Bootstrap(const CpUrl *api, const char *token, const char *cid) {
  static char body[65536];
  int st = HttpCall("GET", api, "/helix/users", token, cid, NULL, body, sizeof(body));
  if (st == 401) { g_cp_status = kCpNeedConnect; printf("[cp] token rejected by Twitch - redo CONNECT\n"); return false; }
  if (st != 200) { printf("[cp] /helix/users failed (http %d)\n", st); return false; }
  JsonValue *j = Json_Parse(body, NULL, 0);
  const JsonValue *d = j ? Json_Get(j, "data") : NULL;
  const JsonValue *u = (d && d->type == JSON_ARRAY) ? d->child : NULL;
  if (!u) { Json_Free(j); printf("[cp] /helix/users: no user\n"); return false; }
  snprintf(g_cp_broadcaster, sizeof(g_cp_broadcaster), "%s", Json_AsString(Json_Get(u, "id")) ? Json_AsString(Json_Get(u, "id")) : "");
  snprintf(g_cp_login, sizeof(g_cp_login), "%s", Json_AsString(Json_Get(u, "login")) ? Json_AsString(Json_Get(u, "login")) : "");
  Json_Free(j);
  if (!g_cp_broadcaster[0]) return false;

  char path[256];
  snprintf(path, sizeof(path), "/helix/channel_points/custom_rewards?broadcaster_id=%s", g_cp_broadcaster);
  st = HttpCall("GET", api, path, token, cid, NULL, body, sizeof(body));
  if (st == 200 && (j = Json_Parse(body, NULL, 0)) != NULL) {
    int n = 0;
    for (const JsonValue *r = (Json_Get(j, "data") ? Json_Get(j, "data")->child : NULL); r; r = r->next) {
      const char *id = Json_AsString(Json_Get(r, "id")), *title = Json_AsString(Json_Get(r, "title"));
      if (id && title) { RewardUpsert(id, title); n++; }
    }
    Json_Free(j);
    printf("[cp] %d channel point reward(s) listed\n", n);
  } else {
    printf("[cp] reward list unavailable (http %d) - rewards appear as they are redeemed\n", st);
  }
  return true;
}

static void HandleRedemption(const JsonValue *ev) {
  const JsonValue *rw = Json_Get(ev, "reward");
  const char *rid = rw ? Json_AsString(Json_Get(rw, "id")) : NULL;
  const char *title = rw ? Json_AsString(Json_Get(rw, "title")) : NULL;
  const char *who = Json_AsString(Json_Get(ev, "user_login"));
  const char *input = Json_AsString(Json_Get(ev, "user_input"));
  if (!rid) return;
  int idx = RewardUpsert(rid, title ? title : "");
  int verb = 0;
  SDL_LockMutex(g_cp_mutex);
  if (idx >= 0) verb = g_cp_rewards[idx].verb;
  SDL_UnlockMutex(g_cp_mutex);
  if (verb <= 0) {
    printf("[cp] '%s' redeemed '%s' - no verb assigned (pause -> MODS -> REWARDS)\n",
           who ? who : "?", title ? title : rid);
    return;
  }
  // a numeric user input becomes the argument for stat verbs ("hurt 4")
  char arg[64];
  snprintf(arg, sizeof(arg), "%s", kVerbs[verb].arg);
  if (input && input[0] && !kVerbs[verb].arg[0]) {
    bool num = true;
    for (const char *p = input; *p; p++) if (!isdigit((unsigned char)*p)) { num = false; break; }
    if (num && strlen(input) <= 4) snprintf(arg, sizeof(arg), "%s", input);
  }
  printf("[cp] '%s' redeemed '%s' -> %s %s\n", who ? who : "?", title ? title : rid,
         kVerbs[verb].verb, arg);
  Twitch_EnqueueExternal(kVerbs[verb].verb, arg, who ? who : "channelpoints");
}

// One websocket session. Returns true when Twitch asked for a reconnect
// (new url in *next), false on any failure (caller backs off).
static bool WsSession(const CpUrl *api, const CpUrl *ws, const char *token,
                      const char *cid, char *next, size_t nextcap) {
  next[0] = 0;
  HINTERNET c = WinHttpConnect(g_cp_session, ws->host, ws->port, 0);
  if (!c) return false;
  HINTERNET r = WinHttpOpenRequest(c, L"GET", ws->path, NULL, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   ws->secure ? WINHTTP_FLAG_SECURE : 0);
  if (!r) { WinHttpCloseHandle(c); return false; }
  WinHttpSetOption(r, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
  bool ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
            WinHttpReceiveResponse(r, NULL);
  HINTERNET wsh = ok ? WinHttpWebSocketCompleteUpgrade(r, 0) : NULL;
  WinHttpCloseHandle(r);
  if (!wsh) { WinHttpCloseHandle(c); printf("[cp] websocket upgrade failed (%lu)\n", GetLastError()); return false; }

  static char msg[65536];
  bool subscribed = false, reconnect = false;
  while (g_cp_running && !g_cp_token_changed) {
    size_t w = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
    bool fail = false;
    do {
      DWORD got = 0;
      DWORD rc = WinHttpWebSocketReceive(wsh, msg + w, (DWORD)(sizeof(msg) - 1 - w), &got, &type);
      if (rc != NO_ERROR) { fail = true; break; }
      w += got;
      if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { fail = true; break; }
    } while (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
             type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE);
    if (fail) break;
    msg[w] = 0;
    JsonValue *j = Json_Parse(msg, NULL, 0);
    if (!j) continue;
    const JsonValue *meta = Json_Get(j, "metadata");
    const char *mtype = meta ? Json_AsString(Json_Get(meta, "message_type")) : NULL;
    const JsonValue *payload = Json_Get(j, "payload");
    if (mtype && !strcmp(mtype, "session_welcome") && payload) {
      const char *sid = Json_AsString(Json_Get(Json_Get(payload, "session"), "id"));
      char body[512], out[4096];
      snprintf(body, sizeof(body),
               "{\"type\":\"channel.channel_points_custom_reward_redemption.add\",\"version\":\"1\","
               "\"condition\":{\"broadcaster_user_id\":\"%s\"},"
               "\"transport\":{\"method\":\"websocket\",\"session_id\":\"%s\"}}",
               g_cp_broadcaster, sid ? sid : "");
      int st = HttpCall("POST", api, "/helix/eventsub/subscriptions", token, cid, body, out, sizeof(out));
      if (st == 202 || st == 200) {
        subscribed = true;
        g_cp_status = kCpOnline;
        printf("[cp] channel points online (%s)\n", g_cp_login);
      } else if (st == 401 || st == 403) {
        g_cp_status = kCpNeedConnect;
        printf("[cp] subscription refused (http %d) - the token lacks channel:read:redemptions; redo CONNECT\n", st);
        Json_Free(j);
        break;
      } else {
        printf("[cp] subscription failed (http %d): %.200s\n", st, out);
        Json_Free(j);
        break;
      }
    } else if (mtype && !strcmp(mtype, "session_reconnect") && payload) {
      const char *url = Json_AsString(Json_Get(Json_Get(payload, "session"), "reconnect_url"));
      if (url) { snprintf(next, nextcap, "%s", url); reconnect = true; }
      Json_Free(j);
      break;
    } else if (mtype && !strcmp(mtype, "revocation")) {
      printf("[cp] subscription revoked by Twitch\n");
      g_cp_status = kCpNeedConnect;
      Json_Free(j);
      break;
    } else if (mtype && !strcmp(mtype, "notification") && payload) {
      const JsonValue *ev = Json_Get(payload, "event");
      if (ev) HandleRedemption(ev);
    }
    Json_Free(j);
  }
  WinHttpWebSocketClose(wsh, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
  WinHttpCloseHandle(wsh);
  WinHttpCloseHandle(c);
  (void)subscribed;
  return reconnect;
}

static int CpThread(void *ud) {
  (void)ud;
  int backoff = 5;
  while (g_cp_running) {
    char token[160];
    g_cp_token_changed = false;
    if (!Twitch_CopyToken(token, sizeof(token))) {
      g_cp_status = kCpOff;
      for (int i = 0; i < 20 && g_cp_running && !g_cp_token_changed; i++) SDL_Delay(100);
      continue;
    }
    const char *cid = Twitch_ClientId();
    CpUrl api, ws;
    if (!ParseUrl(Twitch_CpApiUrl(), &api) || !ParseUrl(Twitch_CpWsUrl(), &ws)) {
      printf("[cp] bad cp_api/cp_ws url\n"); g_cp_status = kCpOff; SDL_Delay(5000); continue;
    }
    g_cp_status = kCpConnecting;
    if (!Bootstrap(&api, token, cid)) {
      if (g_cp_status != kCpNeedConnect) g_cp_status = kCpRetrying;
      for (int i = 0; i < backoff * 10 && g_cp_running && !g_cp_token_changed; i++) SDL_Delay(100);
      if (backoff < 60) backoff *= 2;
      continue;
    }
    char next[512];
    for (;;) {
      bool again = WsSession(&api, &ws, token, cid, next, sizeof(next));
      if (!g_cp_running || g_cp_token_changed) break;
      if (again && next[0] && ParseUrl(next, &ws)) { printf("[cp] following reconnect url\n"); continue; }
      break;
    }
    if (!g_cp_running || g_cp_token_changed) continue;
    if (g_cp_status == kCpOnline) { g_cp_status = kCpRetrying; backoff = 5; }
    printf("[cp] disconnected, retrying in %ds\n", backoff);
    for (int i = 0; i < backoff * 10 && g_cp_running && !g_cp_token_changed; i++) SDL_Delay(100);
    if (backoff < 60) backoff *= 2;
  }
  return 0;
}
#endif  // _WIN32

void CP_Init(void) {
  g_cp_mutex = SDL_CreateMutex();
  MapLoad();
#ifdef _WIN32
  g_cp_session = WinHttpOpen(L"zelda3-stream/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!g_cp_session) { printf("[cp] WinHttpOpen failed - channel points off\n"); return; }
  // Twitch keepalives arrive every ~10 s; anything past 60 s idle is a dead socket
  WinHttpSetTimeouts(g_cp_session, 15000, 15000, 15000, 60000);
  g_cp_running = true;
  g_cp_thread = SDL_CreateThread(CpThread, "channel_points", NULL);
  printf("[cp] channel points worker started\n");
#else
  printf("[cp] channel points: Windows only\n");
#endif
}

void CP_TokenChanged(void) { g_cp_token_changed = true; }

void CP_Shutdown(void) {
  g_cp_running = false;
#ifdef _WIN32
  if (g_cp_thread) { SDL_WaitThread(g_cp_thread, NULL); g_cp_thread = NULL; }
  if (g_cp_session) { WinHttpCloseHandle(g_cp_session); g_cp_session = NULL; }
#endif
}
