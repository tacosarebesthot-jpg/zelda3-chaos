// Twitch chat integration for zelda3.
// Concept port of the ZALiA GML design (see "zelda 2 shit/ZALiA_github_src"):
//   twitch_config.txt   - token/user/channel/cooldown/effect_secs/vs_mode
//                         + per-viewer limits (GAP_CHECK #6): per_user_limits,
//                         per_user_cooldown_secs, per_user_max_effects,
//                         global_max_effects - see the limits block below
//   background thread   - raw TCP to irc.chat.twitch.tv:6667, PASS/NICK/JOIN,
//                         PING keepalive, PRIVMSG -> command queue
//   Twitch_Tick         - cooldown gate + verb dispatcher + per-frame effect
//                         tick over a fixed array of {frames, reapply, restore}
// All effects are runtime-only and reversible; nothing touches the save.

// System headers first: variables.h below defines R10/R12/R14 macros that
// collide with winnt.h's CONTEXT fields, so it must come after windows.h.
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
#include <direct.h>
#include <sys/stat.h>
#pragma comment(lib, "ws2_32.lib")
#define TW_INVALID_SOCKET INVALID_SOCKET
#define tw_closesocket closesocket
typedef int tw_socklen_t;
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define tw_closesocket close
#define tw_socklen_t socklen_t
#endif

#include <SDL.h>

#ifdef _MSC_VER
#define tw_stricmp _stricmp
#else
#include <strings.h>
#define tw_stricmp strcasecmp
#endif

#include "twitch.h"
#include "controls.h"
#include "randomizer.h"
#include "dungeon.h"
#include "channel_points.h"
#include "oauth_web.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "sprite.h"
#include "dialogue_override.h"
#ifdef _WIN32
#define strcasecmp _stricmp
#endif
#include "player.h"
#include "tracker.h"
#include "quickchat.h"
#include "music_player.h"   // rocket league quick chat box (top-left overlay)

// ---------------------------------------------------------------- config --

typedef struct TwitchConfig {
  char token[128];     // bare token, "oauth:" re-added on send
  char user[64];
  char channel[64];
  int cooldown_frames; // global anti-spam cooldown, frames
  int effect_secs;     // default timed-effect length, seconds
  bool vs_mode;        // deny-list for helpful verbs
  bool autostart;      // connect on boot
  bool drop_enabled;   // poll twitch_drop/ for "verb|arg|who|dur" files
  bool debug;          // machine-readable RESULT lines on stdout
  bool test_mode;      // test=1: safe verbs work outside gameplay (harness)
  bool ice_legacy;     // ice_legacy=1: old flag-only ice (A/B kill switch)
  bool boss_enabled;   // CHAT BOSS master kill switch (boss_enabled=0 = off)
  int boss_min_rate;   // auto-start: accepted cmds/min over trailing 5 min
  int boss_every_min;  // minimum minutes between fights (cooldown)
  int boss_secs;       // fight window, seconds
  bool per_user_limits; // master switch for the per-viewer limits below
  int per_user_cd_secs; // per-viewer per-verb cooldown, seconds (0 = off)
  int per_user_max_fx;  // max active effect slots per viewer (0 = off)
  int global_max_fx;    // max active effect slots, all viewers (0 = off)
  char client_id[64];  // public Twitch app id - drives the OAuth authorize URL
  char oauth_redirect[192]; // override for the locked http://localhost:8795/...
  // Harness-only: point the IRC client at a local fake Twitch server so the
  // chat path (per-viewer limits, parsing, reconnects) can be tested with
  // scripted viewers and no live channel.  Empty/0 = the real Twitch IRC.
  char irc_host[128];
  int irc_port;
  char cp_api[160];   // harness: http://127.0.0.1:8931 (default https://api.twitch.tv)
  char cp_ws[160];    // harness: ws://127.0.0.1:8932   (default wss://eventsub.wss.twitch.tv/ws)
} TwitchConfig;

static TwitchConfig g_twc = { "", "", "", 600, 5, false, false, true, false,
                              false, false, true, 6, 25, 120, true, 30, 2, 8,
                              "", "" };

// Public Twitch app id (dev console); not a secret. Baked in so a fresh
// install works with nothing typed: first boot writes twitch_config.txt with
// it, the in-game CONNECT flow supplies the token AND the login.
#define TW_DEFAULT_CLIENT_ID "8r59nr8wl8ri1aj63n83tbqcj1vfdp"

static const char kDefaultTwitchConfig[] =
  "# zelda3 stream build - Twitch settings. Written on first boot.\n"
  "# Connect from inside the game: pause, hold SELECT + tap L (MODS page),\n"
  "# CONNECT -> the login link is copied; paste it in a browser, Authorize.\n"
  "# user= and channel= are filled in for you from the login.\n"
  "token=\n"
  "user=\n"
  "channel=\n"
  "client_id=" TW_DEFAULT_CLIENT_ID "\n"
  "enabled=1\n"
  "cooldown=600\n"
  "effect_secs=5\n"
  "vs_mode=0\n"
  "drop=1\n"
  "debug=0\n"
  "test=0\n";

static void TwitchLoadConfig(void) {
  FILE *f = fopen("twitch_config.txt", "r");
  if (!f) {
    FILE *w = fopen("twitch_config.txt", "wb");
    if (w) {
      fwrite(kDefaultTwitchConfig, 1, sizeof(kDefaultTwitchConfig) - 1, w);
      fclose(w);
      printf("[twitch] wrote a default twitch_config.txt (first run)\n");
      f = fopen("twitch_config.txt", "r");
    }
  }
  if (!f) {
    printf("[twitch] no twitch_config.txt - integration idle\n");
    return;
  }
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#' || *s == '/' || *s == ';' || *s == '\n' || *s == '\r' || *s == 0)
      continue;
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *key = s, *val = eq + 1;
    while (*val == ' ' || *val == '	') val++;
    char *e = key + strlen(key);
    while (e > key && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    e = val + strlen(val);
    while (e > val && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    if (!tw_stricmp(key, "token")) {
      if (!strncmp(val, "oauth:", 6)) val += 6;
      snprintf(g_twc.token, sizeof(g_twc.token), "%s", val);
    } else if (!tw_stricmp(key, "user") || !tw_stricmp(key, "nick")) {
      snprintf(g_twc.user, sizeof(g_twc.user), "%s", val);
      for (char *p = g_twc.user; *p; p++) *p = (char)tolower((unsigned char)*p);
    } else if (!tw_stricmp(key, "channel")) {
      if (*val == '#') val++;
      snprintf(g_twc.channel, sizeof(g_twc.channel), "%s", val);
      for (char *p = g_twc.channel; *p; p++) *p = (char)tolower((unsigned char)*p);
    } else if (!tw_stricmp(key, "cooldown")) {
      int v = atoi(val);
      g_twc.cooldown_frames = v > 0 ? v : 600;
    } else if (!tw_stricmp(key, "effect_secs")) {
      g_twc.effect_secs = atoi(val);
      if (g_twc.effect_secs < 1) g_twc.effect_secs = 5;
      if (g_twc.effect_secs > 3600) g_twc.effect_secs = 3600;
    } else if (!tw_stricmp(key, "vs_mode")) {
      g_twc.vs_mode = atoi(val) != 0;
    } else if (!tw_stricmp(key, "enabled")) {
      g_twc.autostart = atoi(val) != 0;
    } else if (!tw_stricmp(key, "drop")) {
      g_twc.drop_enabled = atoi(val) != 0;
    } else if (!tw_stricmp(key, "debug")) {
      g_twc.debug = atoi(val) != 0;
    } else if (!tw_stricmp(key, "test")) {
      g_twc.test_mode = atoi(val) != 0;
    } else if (!tw_stricmp(key, "ice_legacy")) {
      g_twc.ice_legacy = atoi(val) != 0;
    } else if (!tw_stricmp(key, "boss_enabled")) {
      g_twc.boss_enabled = atoi(val) != 0;
    } else if (!tw_stricmp(key, "boss_min_rate")) {
      g_twc.boss_min_rate = atoi(val);
      if (g_twc.boss_min_rate < 1) g_twc.boss_min_rate = 1;
      if (g_twc.boss_min_rate > 60) g_twc.boss_min_rate = 60;
    } else if (!tw_stricmp(key, "boss_every_min")) {
      g_twc.boss_every_min = atoi(val);
      if (g_twc.boss_every_min < 1) g_twc.boss_every_min = 1;
      if (g_twc.boss_every_min > 240) g_twc.boss_every_min = 240;
    } else if (!tw_stricmp(key, "boss_secs")) {
      g_twc.boss_secs = atoi(val);
      if (g_twc.boss_secs < 30) g_twc.boss_secs = 30;
      if (g_twc.boss_secs > 600) g_twc.boss_secs = 600;
    } else if (!tw_stricmp(key, "per_user_limits")) {
      // GAP_CHECK #6: master kill switch for the per-viewer limits
      // (per_user_cooldown_secs / per_user_max_effects / global_max_effects)
      g_twc.per_user_limits = atoi(val) != 0;
    } else if (!tw_stricmp(key, "per_user_cooldown_secs")) {
      g_twc.per_user_cd_secs = atoi(val);
      if (g_twc.per_user_cd_secs < 0) g_twc.per_user_cd_secs = 0;
      if (g_twc.per_user_cd_secs > 3600) g_twc.per_user_cd_secs = 3600;
    } else if (!tw_stricmp(key, "irc_host")) {
      snprintf(g_twc.irc_host, sizeof(g_twc.irc_host), "%s", val);
    } else if (!tw_stricmp(key, "cp_api")) {
      snprintf(g_twc.cp_api, sizeof(g_twc.cp_api), "%s", val);
    } else if (!tw_stricmp(key, "cp_ws")) {
      snprintf(g_twc.cp_ws, sizeof(g_twc.cp_ws), "%s", val);
    } else if (!tw_stricmp(key, "irc_port")) {
      g_twc.irc_port = atoi(val);
      if (g_twc.irc_port < 0 || g_twc.irc_port > 65535) g_twc.irc_port = 0;
    } else if (!tw_stricmp(key, "per_user_max_effects")) {
      g_twc.per_user_max_fx = atoi(val);
      if (g_twc.per_user_max_fx < 0) g_twc.per_user_max_fx = 0;
      if (g_twc.per_user_max_fx > 12) g_twc.per_user_max_fx = 12;  // kFxCount
    } else if (!tw_stricmp(key, "global_max_effects")) {
      g_twc.global_max_fx = atoi(val);
      if (g_twc.global_max_fx < 0) g_twc.global_max_fx = 0;
      if (g_twc.global_max_fx > 12) g_twc.global_max_fx = 12;      // kFxCount
    } else if (!tw_stricmp(key, "client_id")) {
      // public Twitch app id (not a secret): feeds the OAuth authorize URL
      snprintf(g_twc.client_id, sizeof(g_twc.client_id), "%s", val);
    } else if (!tw_stricmp(key, "oauth_redirect")) {
      // default stays LOCKED to http://localhost:8795/twitch/callback
      snprintf(g_twc.oauth_redirect, sizeof(g_twc.oauth_redirect), "%s", val);
    }
  }
  fclose(f);
  if (!g_twc.client_id[0])                 // older config without the key
    snprintf(g_twc.client_id, sizeof(g_twc.client_id), "%s", TW_DEFAULT_CLIENT_ID);
  printf("[twitch] config: user=%s channel=%s cooldown=%df effect=%ds vs=%d "
         "boss=%d(auto>=%d/min every %dmin %ds) peruser=%d(%ds cap %d/%d)\n",
         g_twc.user[0] ? g_twc.user : "-", g_twc.channel[0] ? g_twc.channel : "-",
         g_twc.cooldown_frames, g_twc.effect_secs, g_twc.vs_mode,
         g_twc.boss_enabled, g_twc.boss_min_rate, g_twc.boss_every_min,
         g_twc.boss_secs, g_twc.per_user_limits, g_twc.per_user_cd_secs,
         g_twc.per_user_max_fx, g_twc.global_max_fx);
}

// ------------------------------------------------------------------ queue --

#define TW_QUEUE_SIZE 32
typedef struct TwCommand {
  char verb[24];
  char arg[64];
  char who[64];
  bool external;   // channel points etc: no global cooldown, no per-viewer limits
} TwCommand;
static bool g_tw_external_source;  // set around TwitchApply for external commands
static void TwitchNoteRecent(const char *who);  // recent-chatter ring (dialogue {chatter})
static void TwitchSeenPush(const char *who);    // net thread -> main: every chat sender

static SDL_mutex *g_tw_mutex;
static SDL_Thread *g_tw_thread;
static volatile bool g_tw_running;       // module should keep the thread alive
static volatile bool g_tw_connected;
static TwCommand g_tw_queue[TW_QUEUE_SIZE];
static int g_tw_qhead, g_tw_qtail;       // ring buffer, guarded by g_tw_mutex
static int g_tw_frame_ctr;               // main-thread frame counter
static int g_tw_last_cmd_frame = -1000000;
static int g_tw_last_room = -1;          // room/screen key for confuse-in-screens
static int g_tw_countdown_frames = 0;    // debug "countdown" verb: big on-screen
                                         // 3-2-1 for human-gated test sessions

static void TwitchQueuePush(const TwCommand *cmd) {
  SDL_LockMutex(g_tw_mutex);
  int next = (g_tw_qhead + 1) % TW_QUEUE_SIZE;
  if (next != g_tw_qtail) {              // full -> drop
    g_tw_queue[g_tw_qhead] = *cmd;
    g_tw_qhead = next;
  }
  SDL_UnlockMutex(g_tw_mutex);
}

// Internal-event lane: one-way handoff for feed lines that originate on the
// IRC thread. Tracker_LogActivity (plain static ring + stdio .tmp/rename) is
// only safe on the main thread, so the net thread NEVER calls it directly -
// it parks the text here and Twitch_Tick flushes it on the next frame.
#define TW_EVENT_CAP 8
static char g_tw_ev_who[TW_EVENT_CAP][64];
static char g_tw_ev_text[TW_EVENT_CAP][128];
static int g_tw_ev_head, g_tw_ev_tail;   // ring buffer, guarded by g_tw_mutex

static void TwitchQueueActivity(const char *who, const char *text) {
  SDL_LockMutex(g_tw_mutex);
  int next = (g_tw_ev_head + 1) % TW_EVENT_CAP;
  if (next != g_tw_ev_tail) {            // full -> drop (feed line, not a command)
    snprintf(g_tw_ev_who[g_tw_ev_head], sizeof(g_tw_ev_who[0]), "%s", who);
    snprintf(g_tw_ev_text[g_tw_ev_head], sizeof(g_tw_ev_text[0]), "%s", text);
    g_tw_ev_head = next;
  }
  SDL_UnlockMutex(g_tw_mutex);
}

// ----------------------------------------------------------------- socket --

static void TwitchHandleIrcLine(const char *line);  // defined in irc parsing below

static SOCKET g_tw_sock = INVALID_SOCKET;
// Liveness/reconnect state. All of it is owned by the IRC thread (this file's
// TwitchThreadFunc / TwitchSocketRun / TwitchHandleIrcLine); the main thread
// never sends on the socket - it only snapshots + shutdown()s the handle under
// g_tw_mutex in Twitch_Shutdown, so every mutation of g_tw_sock and every
// (re)connect decision checks g_tw_shutdown under that same mutex.
static volatile bool g_tw_shutdown;      // Twitch_Shutdown: stop (re)connecting
static int g_tw_backoff_s = 5;           // reconnect delay: 5s doubling to cap
#define TW_BACKOFF_MAX_S 300
// Set by Twitch_UpdateToken (main thread, OAuth flow) so the net thread's
// backoff sleep cuts short and redials immediately with the new token.
static volatile bool g_tw_reconnect_now;
// Client keepalive: with no traffic for a minute we send "PING :tmi.twitch.tv"
// (Twitch answers PONG; ANY traffic also resets the idle clock). Combined with
// the recv timeout below, a dead tunnel (laptop sleep, NAT idle drop) becomes
// a clean reconnect instead of this thread blocking in recv() forever.
#define TW_PING_EVERY_MS 60000
// recv() wakes every 30 s so the idle check at the top of the read loop
// actually runs on a quiet channel (it used to be reached only after
// traffic, which reset the idle clock, so the client PING was dead code and
// a quiet channel reconnected every 120 s). A wake without traffic is idle,
// not a disconnect; only TW_DEAD_AFTER_MS of total silence is.
#define TW_RCV_TIMEOUT_MS 30000
#define TW_DEAD_AFTER_MS 120000

#ifdef _WIN32
static bool g_tw_wsa_up;                 // WSAStartup once per process, not once
static bool TwitchEnsureWsa(void) {      // per connect attempt (refcount leak)
  if (g_tw_wsa_up)
    return true;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    return false;
  g_tw_wsa_up = true;                    // balanced by WSACleanup in Twitch_Shutdown
  return true;
}
#endif

static int TwitchSend(const char *msg) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "%s\r\n", msg);
#ifdef _WIN32
  return send(g_tw_sock, buf, n, 0) == n ? 0 : -1;
#else
  return send(g_tw_sock, buf, (size_t)n, 0) == n ? 0 : -1;
#endif
}

static int TwitchSocketConnect(void) {
  if (g_tw_shutdown)
    return -1;                           // shutting down: never open a new socket
#ifdef _WIN32
  if (!TwitchEnsureWsa())
    return -1;
#endif
  struct addrinfo hints = { 0 }, *res = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  const char *host = g_twc.irc_host[0] ? g_twc.irc_host : "irc.chat.twitch.tv";
  char port[8];
  snprintf(port, sizeof(port), "%d", g_twc.irc_port > 0 ? g_twc.irc_port : 6667);
  if (g_twc.irc_host[0]) {
    static bool said;
    if (!said) { printf("[twitch] IRC host override: %s:%s (harness)\n", host, port); said = true; }
  }
  if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
    return -1;
  SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == INVALID_SOCKET) {
    freeaddrinfo(res);
    return -1;
  }
  if (connect(s, res->ai_addr, (tw_socklen_t)res->ai_addrlen) != 0) {
    tw_closesocket(s);
    freeaddrinfo(res);
    return -1;
  }
  // Dead-connection detector: a healthy connection never goes 120 s without
  // traffic (Twitch PINGs us every few minutes and we PING after 60 s of
  // silence), so a recv() that times out means the tunnel is gone. Best
  // effort - on failure the socket stays blocking and the keepalive's send
  // check catches the dead peer instead.
#ifdef _WIN32
  {
    DWORD to = TW_RCV_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
  }
#else
  {
    struct timeval tv = { TW_RCV_TIMEOUT_MS / 1000, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
#endif
  freeaddrinfo(res);
  // Publish the socket under the lock so Twitch_Shutdown can never snapshot
  // a torn or just-replaced handle, and never sees a socket published after
  // it asked us to stop (g_tw_shutdown). SDL_LockMutex(NULL) is a safe no-op,
  // so this also works if shutdown already cleared the mutex pointer.
  bool published = false;
  if (g_tw_mutex)
    SDL_LockMutex(g_tw_mutex);
  if (!g_tw_shutdown) {
    g_tw_sock = s;
    published = true;
  }
  if (g_tw_mutex)
    SDL_UnlockMutex(g_tw_mutex);
  if (!published) {
    tw_closesocket(s);
    return -1;
  }
  return 0;
}

// Login + receive loop. Runs on the network thread only. All socket sends
// happen on this same thread (here + the PONG/PING replies in
// TwitchHandleIrcLine), so TwitchSend never races another sender; the main
// thread only shutdown()s a snapshot of the handle (Twitch_Shutdown).
static int TwitchSocketRun(void) {
  char login[256];
  // Read the token under the mutex: Twitch_UpdateToken (main thread, OAuth
  // flow) may swap it while this reconnect is dialing.
  char tokbuf[128];
  if (g_tw_mutex)
    SDL_LockMutex(g_tw_mutex);
  snprintf(tokbuf, sizeof(tokbuf), "%s", g_twc.token);
  if (g_tw_mutex)
    SDL_UnlockMutex(g_tw_mutex);
  snprintf(login, sizeof(login), "PASS oauth:%s", tokbuf);
  if (TwitchSend(login) || (snprintf(login, sizeof(login), "NICK %s", g_twc.user), TwitchSend(login)) ||
      (snprintf(login, sizeof(login), "JOIN #%s", g_twc.channel), TwitchSend(login))) {
    printf("[twitch] login send failed\n");
    return -1;
  }
  printf("[twitch] login sent, joining #%s\n", g_twc.channel);

  char acc[2048];
  int acc_len = 0;
  char buf[1024];
  uint32 last_traffic = SDL_GetTicks();  // unsigned ms diffs are wrap-safe
  uint32 last_ping = last_traffic;
  while (g_tw_running) {
    // Keepalive: after a full minute of total silence, ask Twitch for a
    // PONG. If the tunnel is already dead the send fails here; if it is
    // half-dead and nothing answers, the SO_RCVTIMEO recv below gives up
    // after 120 s - either way we fall into the normal reconnect path
    // instead of this thread blocking in recv() forever (the old behavior).
    uint32 now = SDL_GetTicks();
    if (now - last_traffic >= TW_PING_EVERY_MS && now - last_ping >= TW_PING_EVERY_MS) {
      if (TwitchSend("PING :tmi.twitch.tv") != 0)
        return -1;                        // dead socket: reconnect
      last_ping = now;
    }
#ifdef _WIN32
    int n = recv(g_tw_sock, buf, (int)sizeof(buf), 0);
#else
    int n = (int)recv(g_tw_sock, buf, sizeof(buf), 0);
#endif
    if (n <= 0) {
      // A timed-out recv on a quiet channel is not a disconnect: loop back
      // so the keepalive above can PING once idle >= TW_PING_EVERY_MS. Give
      // up only after TW_DEAD_AFTER_MS with no traffic at all (a PING that
      // never gets its PONG), or on any real socket error / orderly close.
#ifdef _WIN32
      bool timed_out = (n < 0) && (WSAGetLastError() == WSAETIMEDOUT);
#else
      bool timed_out = (n < 0) && (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
      if (!timed_out || !g_tw_running)
        return -1;                        // disconnected: caller retries
      if (SDL_GetTicks() - last_traffic >= TW_DEAD_AFTER_MS)
        return -1;                        // silent too long: tunnel is dead
      continue;
    }
    last_traffic = SDL_GetTicks();
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n') {
        acc[acc_len] = 0;
        TwitchHandleIrcLine(acc); // note: defined below via forward decl
        acc_len = 0;
      } else if (c != '\r' && acc_len < (int)sizeof(acc) - 1) {
        acc[acc_len++] = c;
      }
    }
  }
  return 0;
}

// Runs on the network thread: connect, serve, retry with exponential backoff.
// The backoff doubles from 5 s up to a 5 min cap so a permanently broken
// setup (dead token, no network) doesn't pair "login sent/disconnected" spam
// with unbounded stdout growth every 5 seconds; TwitchHandleIrcLine resets it
// to 5 s the moment a real login (001) completes.
static int TwitchThreadFunc(void *unused) {
  (void)unused;
  while (g_tw_running) {
    if (TwitchSocketConnect() != 0) {
      printf("[twitch] connect failed, retrying in %ds\n", g_tw_backoff_s);
      for (int i = 0; i < g_tw_backoff_s * 10 && g_tw_running; i++) {
        if (g_tw_reconnect_now)
          break;                       // fresh token: dial again right now
        SDL_Delay(100);
      }
      g_tw_reconnect_now = false;
      g_tw_backoff_s *= 2;
      if (g_tw_backoff_s > TW_BACKOFF_MAX_S)
        g_tw_backoff_s = TW_BACKOFF_MAX_S;
      continue;
    }
    g_tw_connected = true;
    TwitchSocketRun();
    g_tw_connected = false;
    // Close + invalidate under the mutex: Twitch_Shutdown snapshots and
    // shutdown()s g_tw_sock under the same lock, so it can never grab a
    // half-retired or freshly-replaced handle here.
    if (g_tw_mutex)
      SDL_LockMutex(g_tw_mutex);
    if (g_tw_sock != INVALID_SOCKET) {
      tw_closesocket(g_tw_sock);
      g_tw_sock = INVALID_SOCKET;
    }
    if (g_tw_mutex)
      SDL_UnlockMutex(g_tw_mutex);
    printf("[twitch] disconnected, retrying in %ds\n", g_tw_backoff_s);
    for (int i = 0; i < g_tw_backoff_s * 10 && g_tw_running; i++) {
      if (g_tw_reconnect_now)
        break;                         // fresh token: dial again right now
      SDL_Delay(100);
    }
    g_tw_reconnect_now = false;
    g_tw_backoff_s *= 2;
    if (g_tw_backoff_s > TW_BACKOFF_MAX_S)
      g_tw_backoff_s = TW_BACKOFF_MAX_S;
  }
  return 0;
}

// ------------------------------------------------------------ irc parsing --

// Must start with '!' for it to be a command.
static bool TwitchIsVerb(const char *v);

// IRC structural check: does `line` start with a ":<prefix>" whose first
// token after the prefix is exactly the (numeric) command `cmd`? Genuine
// server replies look like ":tmi.twitch.tv 001 <nick> :Welcome, GLHF!".
static bool TwitchIrcCommandIs(const char *line, const char *cmd) {
  if (line[0] != ':')
    return false;
  const char *sp = strchr(line, ' ');
  if (!sp)
    return false;
  sp++;
  size_t n = strlen(cmd);
  return strncmp(sp, cmd, n) == 0 && (sp[n] == ' ' || sp[n] == 0);
}

// Parse one IRC line and queue any chat command. Network thread only.
static void TwitchHandleIrcLine(const char *line) {
  if (!strncmp(line, "PING", 4)) {
    TwitchSend("PONG :tmi.twitch.tv");
    return;
  }
  // The welcome check MUST be anchored to the message prefix: IRC numerics
  // always arrive as ":tmi.twitch.tv 001 <nick> :...", while a chat MESSAGE
  // can contain " 001 " anywhere (e.g. "!speed 001"). The old
  // strstr(line, " 001 ") matched that spam too (this runs before the
  // PRIVMSG check below), so the command was silently dropped AND the
  // connect tagline re-fired into the activity feed, viewer-spammable.
  if (TwitchIrcCommandIs(line, "001")) {
    printf("[twitch] connected to chat\n");
    g_tw_backoff_s = 5;                  // real login: reconnect backoff resets
    fflush(stdout);
    // lane's community tagline (a regular's own words) - shown in the
    // stream overlay feed when chat connects. Queued, NOT called directly:
    // Tracker_LogActivity is main-thread-only (see TwitchQueueActivity).
    TwitchQueueActivity("HungSo1o", "will help and hinder - you never know which");
    return;
  }
  const char *pm = strstr(line, " PRIVMSG ");
  if (!pm)
    return;
  // :nick!user@host PRIVMSG #chan :message
  char who[64] = "?", msg[384] = "";
  if (line[0] == ':') {
    const char *bang = strchr(line + 1, '!');
    if (bang && bang < pm) {
      size_t nl = (size_t)(bang - line - 1);
      if (nl >= sizeof(who)) nl = sizeof(who) - 1;
      memcpy(who, line + 1, nl);
      who[nl] = 0;
    }
  }
  const char *m = strstr(pm, " :");
  if (m)
    snprintf(msg, sizeof(msg), "%s", m + 2);
  if (who[0] && who[0] != '?')
    TwitchSeenPush(who);       // every chatter feeds the dialogue name pool, commands or not
  if (msg[0] != '!')
    return;
  char verb[24] = "", arg[64] = "";
  const char *sp = strchr(msg + 1, ' ');
  if (sp) {
    size_t vl = (size_t)(sp - msg - 1);
    if (vl >= sizeof(verb)) vl = sizeof(verb) - 1;
    memcpy(verb, msg + 1, vl);
    verb[vl] = 0;
    snprintf(arg, sizeof(arg), "%s", sp + 1);
  } else {
    snprintf(verb, sizeof(verb), "%s", msg + 1);
  }
  for (char *p = verb; *p; p++) *p = (char)tolower((unsigned char)*p);

  if (!TwitchIsVerb(verb))
    return;                               // unknown verbs never consume the cooldown
  TwCommand cmd;
  snprintf(cmd.verb, sizeof(cmd.verb), "%s", verb);
  snprintf(cmd.arg, sizeof(cmd.arg), "%s", arg);
  snprintf(cmd.who, sizeof(cmd.who), "%s", who);
  TwitchQueuePush(&cmd);
}

// ------------------------------------------------------------- dispatcher --

static bool TwitchInGameplay(void) {
  // 7 = dungeon, 9 = overworld (kMainRouting in misc.c); no menus/dialogue;
// flag_unk1 is the engine-wide cutscene lock (item receipt, medallions,
// dungeon events) - the ZALiA "swarm right after a boss kill" corruption
// class is exactly what this excludes
  return (main_module_index == 7 || main_module_index == 9) && submodule_index == 0 &&
         flag_unk1 == 0;
}

static uint32 g_tw_rng = 0;
static uint32 TwitchRand(void) {
  g_tw_rng ^= g_tw_rng << 13;
  g_tw_rng ^= g_tw_rng >> 17;
  g_tw_rng ^= g_tw_rng << 5;
  return g_tw_rng;
}
static int TwitchRandRange(int n) {  // [0, n)
  return (int)(TwitchRand() % (uint32)n);
}

static int TwitchNum(const char *arg, int def) {
  while (*arg == ' ') arg++;
  if (!*arg)
    return def;
  char *end;
  long v = strtol(arg, &end, 10);
  if (end == arg)
    return def;                          // no digits at all
  if (v > 1000000)
    v = 1000000;                         // atoi would be UB on overflow
  if (v < -1000000)
    v = -1000000;
  return (int)v;                         // negatives allowed; callers clamp
}

// steal pool: inventory bytes chat can take for a while
static const struct { uint8 *addr; const char *name; } kStealPool[] = {
  &link_item_bow, "bow",
  &link_item_boomerang, "boomerang",
  &link_item_hookshot, "hookshot",
  &link_item_mushroom, "mushroom",
  &link_item_fire_rod, "fire rod",
  &link_item_ice_rod, "ice rod",
  &link_item_torch, "torch",
  &link_item_hammer, "hammer",
  &link_item_flute, "flute",
  &link_item_bug_net, "bug net",
  &link_item_book_of_mudora, "book",
  &link_item_cane_somaria, "somaria",
  &link_item_cane_byrna, "byrna",
  &link_item_cape, "cape",
  &link_item_mirror, "mirror",
  &link_item_gloves, "gloves",
  &link_item_boots, "boots",
  &link_item_flippers, "flippers",
  &link_item_moon_pearl, "moon pearl",
};

// timed effect slots - one instance per effect id
enum { kFxConfuse, kFxFlip, kFxHeadFreeze, kFxCurse, kFxRoot, kFxSteal,
       kFxSpeed, kFxSlow, kFxIce, kFxDenyY, kFxDenyBoots, kFxCucco, kFxCount };
typedef struct TwEffect {
  int frames;
  int screens;                           // confuse: countdown in ROOM TRANSITIONS
  int tick;                              // curse sub-counter
  int held;                              // cucco: sprite slot riding Link, -1 none
  uint8 *saved_addr;                     // steal
  uint8 saved_val;                       // steal value / speed saved setting
  char owner[32];                        // viewer who armed it, "" = system/
                                         // exempt source (per-viewer caps)
} TwEffect;
static TwEffect g_tw_fx[kFxCount];
// Ice: twitch-owned shadow of the engine's momentum accumulator
// swimcoll_var7[] (per axis). TileDetect_MainHandler wipes the real one
// every frame on non-ice tiles, so we keep the slide speed here and
// re-seed it pre-frame; see kFxIce in TwitchTickEffects.
static uint16 g_tw_ice_vel[2];

// Pending effect owner: TwitchApply stamps the accepted command's `who`
// here before verb dispatch - "" for the exempt sources (boss / drop
// folder / test=1), so their (re)arms never count against a viewer's
// per-viewer budget. FxStartFrames copies it into the slot it starts; the
// confuse/party branches write their slot directly and stamp via FxSetOwner.
static char g_tw_fx_owner[32];
static void FxSetOwner(int id, const char *who) {
  snprintf(g_tw_fx[id].owner, sizeof(g_tw_fx[id].owner), "%s", who ? who : "");
}
static void FxStartFrames(int id, int frames) {
  g_tw_fx[id].frames = frames;
  FxSetOwner(id, g_tw_fx_owner);
}
// Timed effects run for dur_frames, or the configured default length when 0.
static int EffectFrames(int dur_frames) {
  return (dur_frames > 0) ? dur_frames : g_twc.effect_secs * 60;
}
static bool FxActive(int id) {
  return g_tw_fx[id].frames > 0 || g_tw_fx[id].screens > 0;
}

static const char *const kVsBlocked[] = { "heal", "mp", "refill", "rupees", "bombs", "arrows" };

static bool TwitchVsBlocked(const char *verb) {
  if (!g_twc.vs_mode)
    return false;
  for (size_t i = 0; i < sizeof(kVsBlocked) / sizeof(kVsBlocked[0]); i++)
    if (!strcmp(verb, kVsBlocked[i]))
      return true;
  return false;
}

// ------------------------------------------------------- persistent modes --
// Chat-toggleable persistent modifiers: the live version of vs_mode's
// config-file flag pattern (vs_mode itself is boot-only; modes flip at
// runtime). In-session only by design for v1: twitch_config.txt is re-read
// on every boot and modes are NOT persisted, so a mode always comes back
// OFF at restart and the demo build stays vanilla unless chat arms it.
//
// Adding a future persistent enemy modifier is three steps:
//   1. one entry in kTwModes below (canonical verb + optional alias)
//   2. read it anywhere with TwitchModeOn("<name>")
//   3. if it needs more than a flag (timers, arming, cleanup), add a branch
//      to TwitchModeChanged(); engine-facing gates follow the shape of
//      Twitch_AttritionDamage(): flag check first (zero cost when off),
//      then a module guard, then the mutation.
typedef struct TwMode {
  const char *name;                      // canonical chat verb
  const char *alias;                     // accepted synonym, NULL = none
  bool on;                               // live state, default OFF
} TwMode;

enum { kModeAttrition, kModeDmgUp, kModeMpSteal, kModeRupeeSteal };
static TwMode kTwModes[] = {
  { "attrition", "hardmode", false },
  // enemy-hit modifiers: all applied on the same damage choke point by
  // Twitch_ModifyDamage() (player.c Link_ControlHandler). Composable by
  // design - each flag is independent, "hardmods" just sweeps them all.
  { "dmgup", NULL, false },                      // enemies deal double damage
  { "mpsteal", NULL, false },                    // every hit also drains 16 magic
  { "rupeesteal", NULL, false },                 // every hit also takes 20 rupees
};

static TwMode *TwitchModeLookup(const char *verb) {
  for (size_t i = 0; i < sizeof(kTwModes) / sizeof(kTwModes[0]); i++) {
    TwMode *m = &kTwModes[i];
    if (!strcmp(verb, m->name) || (m->alias && !strcmp(verb, m->alias)))
      return m;
  }
  return NULL;
}

static bool TwitchModeOn(const char *name) {
  TwMode *m = TwitchModeLookup(name);
  return m && m->on;
}

// Hook for future modes whose side effects go beyond the flag itself.
static void TwitchModeChanged(TwMode *m, bool on) {
  (void)m;
  (void)on;
}

// "hardmods" - convenience GROUP toggle over the three enemy-hit modifiers
// (dmgup, mpsteal, rupeesteal). It has no state of its own and is therefore
// NOT a row in kTwModes; it only drives the individual flags:
//   !hardmods on      -> all three modifiers on
//   !hardmods off     -> all three off (the sweep wins over whatever the
//                        individual flags currently say)
//   !hardmods (bare)  -> none on -> all on; ANY on -> all off, so a bare
//                        toggle always collapses the group to a known state
// Individual toggles keep working before/after/between group toggles, so
// "!hardmods on" then "!dmgup off" leaves mpsteal+rupeesteal armed.
// GROUP SCOPE: attrition is deliberately NOT a member. It has its own alias
// (hardmode) and permanent container-loss semantics - arm it separately.
static bool TwitchHardModsDispatch(const char *arg, const char *who) {
  while (*arg == ' ') arg++;
  bool any_on = kTwModes[kModeDmgUp].on || kTwModes[kModeMpSteal].on ||
                kTwModes[kModeRupeeSteal].on;
  bool on = !tw_stricmp(arg, "on") ? true
          : !tw_stricmp(arg, "off") ? false
          : !any_on;                       // bare verb / junk arg = group toggle
  for (int i = kModeDmgUp; i <= kModeRupeeSteal; i++) {
    kTwModes[i].on = on;
    TwitchModeChanged(&kTwModes[i], on);
  }
  printf("[twitch] %s -> hardmods %s (dmgup=%d mpsteal=%d rupeesteal=%d)\n",
         who, on ? "on" : "off", kTwModes[kModeDmgUp].on,
         kTwModes[kModeMpSteal].on, kTwModes[kModeRupeeSteal].on);
  Tracker_LogActivity(who, "hardmods");
  Scoreboard_LogCommand();
  if (g_twc.debug)
    printf("RESULT verb=hardmods mode=%s\n", on ? "on" : "off");
  return true;
}

// Dispatch a persistent-mode toggle. Called from TwitchApply BEFORE the
// gameplay gate: modes are configuration, not world effects, so chat can arm
// one at the title screen or mid-cutscene. Returns true if handled.
static bool TwitchModeDispatch(const char *verb, const char *arg, const char *who) {
  if (!strcmp(verb, "hardmods"))
    return TwitchHardModsDispatch(arg, who);
  TwMode *m = TwitchModeLookup(verb);
  if (!m)
    return false;
  while (*arg == ' ') arg++;
  bool on = !tw_stricmp(arg, "on") ? true
          : !tw_stricmp(arg, "off") ? false
          : !m->on;                      // bare verb / junk arg = toggle
  m->on = on;
  TwitchModeChanged(m, on);
  printf("[twitch] %s -> %s %s\n", who, m->name, on ? "on" : "off");
  Tracker_LogActivity(who, m->name);
  Scoreboard_LogCommand();
  if (g_twc.debug)
    printf("RESULT verb=%s mode=%s\n", m->name, on ? "on" : "off");
  return true;
}

// Core container removal, shared by the engine hook and the "hurt" verb.
// Assumes the caller already vetted the context; checks the flag, the cap>0
// rule and the floor. Returns 1 if a container was lost.
static int AttritionRemoveContainer(void) {
  if (!kTwModes[kModeAttrition].on)
    return 0;                            // mode off: zero cost, vanilla game
  int cap = link_health_capacity;
  if (cap == 0)
    return 0;                            // title / unloaded save: nothing to lose
  int new_cap = cap - 8;
  if (new_cap < 24)
    new_cap = 24;                        // floor: 3 containers always remain
  if (new_cap >= cap)
    return 0;                            // already at the floor: no container lost
  link_health_capacity = (uint8)new_cap;
  if (link_health_current > link_health_capacity)
    link_health_current = link_health_capacity;
  printf("[twitch] attrition: heart container lost - %d left\n", new_cap >> 3);
  if (g_twc.debug)
    printf("RESULT verb=attrition cap=%d->%d\n", cap, new_cap);
  char feed[96];
  snprintf(feed, sizeof(feed), "heart container lost - %d left", new_cap >> 3);
  Tracker_LogActivity("attrition", feed);
  return 1;
}

// Attrition damage hook (declared in twitch.h). Called from the single
// damage choke point Link_ControlHandler in player.c after
// link_health_current was reduced. Every hit also removes one heart
// CONTAINER: capacity -8, floored at 24 (3 containers), current health
// clamped into the new capacity. Turning the mode off stops further loss but
// restores nothing - the loss is permanent for the session.
int Twitch_AttritionDamage(void) {
  // Module guard (this entry point is the engine hook). Live gameplay hits
  // only: 7 = dungeon, 9 = overworld; 18 here means the hit being processed
  // was LETHAL (Link_ControlHandler entered the game-over module a few lines
  // above the call): the container loss still applies exactly once - the
  // death sequence never re-enters the player handler, so it cannot
  // double-fire. Every other module (file select, game over 12, cutscene
  // submodules) is excluded outright.
  if (main_module_index != 7 && main_module_index != 9 && main_module_index != 18)
    return 0;
  return AttritionRemoveContainer();
}

// --------------------------------------------------------- enemy modifiers --
// The composable hit-modifier modes (dmgup / mpsteal / rupeesteal), fanned
// out from ONE engine call. Twitch_ModifyDamage() sits at the same
// Link_ControlHandler choke point as Twitch_AttritionDamage() but runs
// EARLIER in the hit sequence - exact order, and why:
//
//   uint8 new_dmg = link_health_current - dmg;   // raw subtraction
//   Twitch_ModifyDamage(&new_dmg);               // (1) modifiers shape the hit
//   if (new_dmg == 0 || new_dmg >= 0xa8)         // (2) engine death check
//     ...game-over module...
//   link_health_current = new_dmg;               // (3) hit applied
//   Twitch_AttritionDamage();                    // (4) attrition container loss
//
// dmgup doubles the damage of the hit (see below), so it MUST run between
// (1) and (2): a hit doubled into lethality then goes through the engine's
// OWN game-over path instead of stranding Link at 0 HP outside the death
// module. Doubling therefore always happens BEFORE attrition fires at (4) -
// the two are orthogonal and compose: dmgup changes how much CURRENT health
// the hit costs, attrition changes how much MAX health it costs, and both
// still apply to the same hit. Modifiers run before the value is applied
// ("before it's applied" - before (3)); attrition reads capacity only and is
// unaffected by the doubled value.

// Session-lifetime total of rupees rupeesteal has taken from Link. Kept
// separate from the flag on purpose: turning the mode off stops further
// theft but restores nothing and forgets nothing (attrition rule), and the
// total survives re-toggles for the overlay/log. Zero on boot.
static int g_tw_rupees_stolen;

// Core fan-out, shared by the engine hook and the test-only "modprobe" verb.
// Precondition (engine call site): *new_dmg is the post-subtraction health
// value and link_health_current STILL holds the PRE-hit health - dmgup needs
// it to know how much damage the hit dealt. Flag-first on every branch:
// with a mode off its cost is a single bool test. No module guard here (the
// guard lives in Twitch_ModifyDamage; modprobe bypasses it to exercise the
// math at the title screen).
static void TwitchModifyDamageCore(uint8 *new_dmg) {
  // dmgup: enemies deal DOUBLE damage. The engine already subtracted the
  // hit's damage once; dealt recovers it from the untouched pre-hit health,
  // and we set the remaining health to prehit - 2*dealt. int math with a
  // floor at 0 subsumes any byte clamp: health never exceeds 0xE0, so 2 *
  // dealt cannot wrap, the result never EXCEEDS the undoubled value (so the
  // >= 0xa8 wrap-detector downstream can never fire falsely), and the worst
  // case (2*dealt >= prehit) lands exactly on 0 - the engine's own
  // dead-health value, which the death check right after us turns into a
  // normal game over.
  if (kTwModes[kModeDmgUp].on) {
    int dealt = (int)link_health_current - (int)*new_dmg;
    if (dealt > 0) {                       // <= 0: lethal/wrapped hit already,
      int v = (int)link_health_current - 2 * dealt;   // death check owns it
      *new_dmg = (uint8)(v < 0 ? 0 : v);
    }
  }
  // mpsteal: every hit also burns 16 magic (1/8 of the 0x80 bar), floor 0.
  // Silent per-hit (would spam both stdout and the overlay feed); observable
  // via the modprobe RESULT in test builds.
  if (kTwModes[kModeMpSteal].on) {
    int v = (int)link_magic_power - 16;
    link_magic_power = (uint8)(v < 0 ? 0 : v);
  }
  // rupeesteal: every hit also costs 20 rupees, floor 0. Telemetry like
  // attrition's cap= lines: only when something was actually taken, with the
  // running session total so the overlay/log can show what chat stole.
  if (kTwModes[kModeRupeeSteal].on && link_rupees_actual > 0) {
    int taken = link_rupees_actual < 20 ? link_rupees_actual : 20;
    link_rupees_actual = (uint16)(link_rupees_actual - taken);
    g_tw_rupees_stolen += taken;
    if (g_twc.debug)
      printf("RESULT verb=rupeesteal taken=%d total=%d\n", taken, g_tw_rupees_stolen);
    char feed[96];
    snprintf(feed, sizeof(feed), "chat stole %d rupees (%d total)",
             taken, g_tw_rupees_stolen);
    Tracker_LogActivity("rupeesteal", feed);
  }
}

// Engine hook (declared in twitch.h, called from player.c). Flag-first early
// out: all three modifiers off = the vanilla game, one branch. Same module
// guard shape as attrition: live gameplay hits only (7 dungeon / 9
// overworld, plus 18 = the lethal hit already entering the game-over module,
// so a killing blow still pays magic/rupees); everything else excluded.
void Twitch_ModifyDamage(uint8 *new_dmg) {
  if (!kTwModes[kModeDmgUp].on && !kTwModes[kModeMpSteal].on &&
      !kTwModes[kModeRupeeSteal].on)
    return;                              // mode off: zero cost, vanilla game
  if (main_module_index != 7 && main_module_index != 9 && main_module_index != 18)
    return;
  TwitchModifyDamageCore(new_dmg);
}

// ------------------------------------------------------------- chat boss --
// CHAT BOSS v1 - meta-virtual per tools/DESIGN_CHATBOSS_VOTING.md section B
// with the owner's v1 decisions layered on top (see the STATUS note at the
// top of that doc). The boss is a plain int HP living in this module:
//   - every ACCEPTED command (IRC or drop, post cooldown/gates - the exact
//     spot where TwitchApply logs it) deals 15 damage, settled into HP on a
//     2 s pulse; RNG crits deal 50 (owner decision #1, see below).
//   - HP at trigger time scales with the trailing 5 min command rate
//     (clamped 150..900, design B.2 formula with the 0.65 knob).
//   - the boss attacks back through TwitchApply (source "THE BOSS"), so
//     every attack is a shipped, gated verb; politeness rule: never re-arm
//     an effect that is already active, and attacks skip ticks that are not
//     real gameplay (the window itself keeps running so fights conclude).
//   - VICTORY: refill + fairy + fanfare feed line + bosses_won counter.
//     DEFEAT: curse 30 s + swarm 4 + deny 15 s together + shame line +
//     losses counter. In-session only (like the modes): a restart
//     evaporates a live fight.
//   - SINGLE KILL SWITCH: boss_enabled=0 (boot-only, twitch_config.txt)
//     disables the state machine, the damage hook, auto-trigger and the
//     bossstart test verb; only the !bosshp query survives (answers OFF).
//
// CRITS (owner decision #1): base 1% per accepted command, +0.5% per 10
// commands the same viewer sent this session, capped 5%. The loyalty proxy
// is the in-session per-viewer command count - no extra Twitch API tonight.
// SUB-BADGE UPGRADE PATH (v2, documented not wired): request the
// twitch.tv/tags IRCv3 capability in TwitchSocketRun, parse the tags block
// of PRIVMSG lines in TwitchHandleIrcLine (the "badges" tag carries
// subscriber/3 etc.), pass a `sub` flag down with the queued command, and
// weight the roll here (e.g. subs get chance = max(chance, 100) = 10%).
// The crit roll is one call site (TwitchBossAccept), so that is a ~20-line
// change plus one flag in TwCommand/TwChatter.

static void TwitchApply(const char *verb, const char *arg, const char *who,
                        int dur_frames);   // defined in the dispatcher below

enum { kBossDormant, kBossActive, kBossResolve };
#define TW_BOSS_FPS 60                     // frames per second of window time
#define TW_BOSS_ATTACKERS 32               // chatter table cap (design B.2)
#define TW_CHATTER_LEN 32

typedef struct TwChatter {
  char who[TW_CHATTER_LEN];
  int session;                             // accepted commands, whole session
  int fight;                               // boss damage this fight (MVP race)
} TwChatter;

typedef struct {
  int state;                               // kBoss*
  char name[TW_CHATTER_LEN + 24];
  int hp, maxhp;
  int window;                              // frames left in the fight window
  int pulse;                               // 2 s damage-settle countdown
  int atk;                                 // attack countdown
  int rot;                                 // round-robin index, attack table
  int phase;                               // 1..3, by HP ratio
  bool steal_used, desp_used;              // one-time phase-crossing attacks
  int pending;                             // damage accepted since last pulse
  int resolve;                             // RESOLVE hold, frames
  int last_end;                            // frame of last fight end (cooldown)
  int name_idx;                            // round-robin name rotation
  int auto_ctr;                            // frames since last auto check
  char last_attack[24];
} TwBoss;

static TwBoss g_boss;
static TwChatter g_tw_chatters[TW_BOSS_ATTACKERS];

// trailing-rate ring: frame numbers of accepted commands. Auto-trigger
// reads "commands/min over the last 5 min" off this (design B.2).
#define TW_RATE_CAP 512
#define TW_RATE_WINDOW (60 * 60 * 5)       // 5 minutes of frames
static int g_tw_rate[TW_RATE_CAP];
static int g_tw_rate_head, g_tw_rate_len;

// Set while a boss-initiated TwitchApply is in flight. Boss attacks and
// victory/defeat payoffs ride the standard dispatcher so they are gated and
// feed-visible exactly like chat commands, but they are not chat traffic:
// no scoreboard bump, no rate ring push, no loyalty, no self-damage.
static bool g_tw_boss_source;

enum { kBet_Win, kBet_Lose, kBet_Good, kBet_Junk };   // bet kinds (ledger defined with the bets below)
static void BossPickName(void);   // rotation or the last MVP (defined with the bets below)
static void BetsResolve(int won, const char *what);
static void BetsRefund(bool boss_class, const char *why);
static char g_boss_last_mvp[TW_CHATTER_LEN];   // last fight's MVP, for the boss name
// Static rotation table from the design doc (pure flavor).
static const char *const kBossNames[] = {
  "CHATTHOG, DEVOURER OF COOLDOWNS",
  "THE SWARMFATHER",
  "AUGHRA THE LAG WITCH",
  "SIR SPAMALOT",
  "VOTEY THE UNDECIDED",
};

// boss_feed.txt - ONE human-readable line, atomic .tmp+rename (the same rule
// as every stream file here); tools/feed.html's BOSS box regex-parses it and
// hides when the file is absent. The file is left on disk after a fight so
// the overlay keeps showing the last result until the next fight overwrites.
static void BossWriteFeed(const char *line) {
  FILE *f = fopen("boss_feed.txt.tmp", "w");
  if (!f)
    return;
  fprintf(f, "%s\n", line);
  fclose(f);
  remove("boss_feed.txt");
  rename("boss_feed.txt.tmp", "boss_feed.txt");
}

// Find-or-create this viewer's loyalty slot. Lookup is case-insensitive
// (drop-file whos are arbitrary case; real Twitch nicks are lowercase).
// Full table: evict the quietest session so regulars keep their crit rate.
static TwChatter *BossChatter(const char *who) {
  TwChatter *empty = NULL, *lame = NULL;
  if (!who[0])
    return NULL;                           // anonymous input: not tracked
  for (int i = 0; i < TW_BOSS_ATTACKERS; i++) {
    TwChatter *c = &g_tw_chatters[i];
    if (!c->who[0]) {
      if (!empty)
        empty = c;
      continue;
    }
    if (!tw_stricmp(c->who, who))
      return c;
    if (!lame || c->session < lame->session)
      lame = c;
  }
  TwChatter *c = empty ? empty : lame;
  if (!c)
    return NULL;
  snprintf(c->who, sizeof(c->who), "%s", who);
  c->session = 0;
  c->fight = 0;
  return c;
}

// MVP race + distinct-attacker count for the feed line.
static TwChatter *BossTopChatter(int *attackers) {
  TwChatter *top = NULL;
  int n = 0;
  for (int i = 0; i < TW_BOSS_ATTACKERS; i++) {
    TwChatter *c = &g_tw_chatters[i];
    if (c->who[0] && c->fight > 0) {
      n++;
      if (!top || c->fight > top->fight)
        top = c;
    }
  }
  *attackers = n;
  return top;
}

static void BossResetFightStats(void) {
  for (int i = 0; i < TW_BOSS_ATTACKERS; i++)
    g_tw_chatters[i].fight = 0;            // session loyalty persists
}

static void BossRatePush(int frame) {
  g_tw_rate[g_tw_rate_head] = frame;
  g_tw_rate_head = (g_tw_rate_head + 1) % TW_RATE_CAP;
  if (g_tw_rate_len < TW_RATE_CAP)
    g_tw_rate_len++;
}

// Accepted commands per minute over the trailing 5 min window.
static int BossTrailingRate(int now_frame) {
  int n = 0;
  for (int i = 0; i < g_tw_rate_len; i++) {
    int idx = (g_tw_rate_head + TW_RATE_CAP - 1 - i) % TW_RATE_CAP;
    if (now_frame - g_tw_rate[idx] <= TW_RATE_WINDOW)
      n++;
  }
  return n / 5;
}

static bool BossCooldownReady(int now_frame) {
  return now_frame - g_boss.last_end >= g_twc.boss_every_min * 60 * TW_BOSS_FPS;
}

// Accepted-command hook, called from TwitchApply exactly where the command
// is logged (post cooldown/gates). Feeds the rate ring + loyalty table all
// session long; while a fight is live it also deals the damage (owner
// decision: flat 15, crits 50). Boss-sourced calls never reach this.
static void TwitchBossAccept(const char *who) {
  if (!g_twc.boss_enabled)
    return;
  BossRatePush(g_tw_frame_ctr);
  TwChatter *c = BossChatter(who);
  if (c)
    c->session++;
  if (g_boss.state != kBossActive)
    return;
  // CRIT roll (see the section comment): 1% base, +0.5% per 10 session
  // commands, capped 5% - computed in milli-percent so this stays integer.
  int chance = 10 + ((c ? c->session : 0) / 10) * 5;
  if (chance > 50)
    chance = 50;
  bool crit = (int)(TwitchRand() % 1000) < chance;
  int dmg = crit ? 50 : 15;
  if (c)
    c->fight += dmg;
  g_boss.pending += dmg;
  if (crit) {
    printf("RESULT verb=boss crit=1 who=%s dmg=50\n", who);
    Tracker_LogActivity(who, "CRIT! hits the boss for 50");
  }
}

// Boss-initiated TwitchApply: gated + feed-visible, but flagged so the
// accept path does not count it as chat traffic (see g_tw_boss_source).
static void BossApply(const char *verb, const char *arg, const char *who, int dur) {
  g_tw_boss_source = true;
  TwitchApply(verb, arg, who, dur);
  g_tw_boss_source = false;
}

static void BossFeedActive(void) {
  int attackers;
  TwChatter *top = BossTopChatter(&attackers);
  char line[320];
  snprintf(line, sizeof(line),
           "%s | HP: %d/%d | phase: %d | attackers: %d | top: %s (%d) | %ds | last: %s",
           g_boss.name, g_boss.hp, g_boss.maxhp, g_boss.phase, attackers,
           top ? top->who : "-", top ? top->fight : 0,
           (g_boss.window + TW_BOSS_FPS - 1) / TW_BOSS_FPS,
           g_boss.last_attack[0] ? g_boss.last_attack : "-");
  BossWriteFeed(line);
}

static void BossVictory(void) {
  g_boss.state = kBossResolve;
  g_boss.resolve = 300;                    // 5 s banner, then back to DORMANT
  g_boss.last_end = g_tw_frame_ctr;
  // Reward table (design B.2): full heal + the classic post-boss fairy.
  // Both are normal gated verbs - the fairy needs real gameplay and shows
  // its standard DROPPED line otherwise. (Deferred: confetti flash + item
  // fanfare music_control - cosmetics, need PostDraw/music verification.)
  BossApply("refill", "", "CHAT-BOSS", 0);
  BossApply("fairy", "", "CHAT-BOSS", 0);
  Scoreboard_LogBossWin();
  int attackers;
  TwChatter *top = BossTopChatter(&attackers);
  (void)attackers;
  if (top && top->who[0]) snprintf(g_boss_last_mvp, TW_CHATTER_LEN, "%s", top->who);
  BetsResolve(kBet_Win, "boss down");
  char msg[128];
  snprintf(msg, sizeof(msg), "CHAT SLAYS %s! MVP: %s (%d dmg)", g_boss.name,
           top ? top->who : "chat", top ? top->fight : 0);
  Tracker_LogActivity("CHAT-BOSS", msg);   // the fanfare line
  QuickChat_BossWin();                     // "Nice shot!" spam in the box
  if (g_twc.debug)
    printf("RESULT verb=boss state=VICTORY mvp=%s dmg=%d\n",
           top ? top->who : "-", top ? top->fight : 0);
  char line[256];
  snprintf(line, sizeof(line), "VICTORY | CHAT SLAYS %s! | MVP: %s (%d dmg) | HP: 0/%d",
           g_boss.name, top ? top->who : "chat", top ? top->fight : 0, g_boss.maxhp);
  BossWriteFeed(line);
}

static void BossDefeat(void) {
  g_boss.state = kBossResolve;
  g_boss.resolve = 300;
  g_boss.last_end = g_tw_frame_ctr;
  // Punishment bundle (owner decision #3): escalating wave, fires together.
  // curse runs 4x internally -> 450f in = the 30 s the owner asked for;
  // deny explicit 900f = 15 s; swarm 4 is a world verb (needs gameplay -
  // drops with its standard RESULT line otherwise). No Channel Point
  // docking tonight: that needs Helix manage_scopes (documented v2 option).
  BossApply("curse", "", "CHAT-BOSS", 30 * TW_BOSS_FPS / 4);
  BossApply("swarm", "4", "CHAT-BOSS", 0);
  BossApply("deny", "", "CHAT-BOSS", 15 * TW_BOSS_FPS);
  Scoreboard_LogBossLoss();
  {
    int attackers2;
    TwChatter *mvp = BossTopChatter(&attackers2);
    if (mvp && mvp->who[0]) snprintf(g_boss_last_mvp, TW_CHATTER_LEN, "%s", mvp->who);
  }
  BetsResolve(kBet_Lose, "boss survived");
  Tracker_LogActivity("CHAT-BOSS", "the boss survives. chat, you had ONE job");
  QuickChat_BossLoss();                    // "gg ez" spam in the box
  if (g_twc.debug)
    printf("RESULT verb=boss state=DEFEAT hp=%d/%d\n", g_boss.hp, g_boss.maxhp);
  char line[256];
  snprintf(line, sizeof(line), "DEFEAT | %s survives. chat, you had ONE job | HP: %d/%d",
           g_boss.name, g_boss.hp, g_boss.maxhp);
  BossWriteFeed(line);
}

static void BossStart(const char *reason) {
  if (!g_twc.boss_enabled) {
    if (g_twc.debug)
      printf("RESULT verb=bossstart DROPPED reason=boss_disabled\n");
    return;
  }
  if (g_twc.vs_mode) {                     // design C: payoff table is
    if (g_twc.debug)                       // vs-blocked, fights would break
      printf("RESULT verb=bossstart DROPPED reason=vs_mode\n");
    Tracker_LogActivity("CHAT-BOSS", "boss unavailable in VS mode");
    return;
  }
  if (g_boss.state != kBossDormant) {
    if (g_twc.debug)
      printf("RESULT verb=bossstart DROPPED reason=already_active\n");
    return;
  }
  // HP from the trailing 5 min command rate (design B.2): R clamped [6,30],
  // D_expected = R * secs * 15 / 60, HP = 0.65 * D rounded to 10, clamped
  // [150,900]. The 0.65 is the post-fight-one live-tune knob.
  int rate = BossTrailingRate(g_tw_frame_ctr);
  if (rate < 6)
    rate = 6;
  if (rate > 30)
    rate = 30;
  int expected = rate * g_twc.boss_secs / 4;
  int hp = (int)(0.65 * expected + 5) / 10 * 10;
  if (hp < 150)
    hp = 150;
  if (hp > 900)
    hp = 900;
  g_boss.state = kBossActive;
  g_boss.maxhp = hp;
  g_boss.hp = hp;
  g_boss.window = g_twc.boss_secs * TW_BOSS_FPS;
  g_boss.pulse = 120;
  g_boss.atk = 900;                        // first attack a full P1 cadence in
  g_boss.rot = 0;
  g_boss.phase = 1;
  g_boss.steal_used = false;
  g_boss.desp_used = false;
  g_boss.pending = 0;
  g_boss.last_attack[0] = 0;
  BossPickName();
  BossResetFightStats();
  char msg[96];
  snprintf(msg, sizeof(msg), "%s emerges! every command hits for 15", g_boss.name);
  Tracker_LogActivity("CHAT-BOSS", msg);
  printf("[twitch] CHAT-BOSS -> %s (%s) hp=%d\n", g_boss.name, reason, hp);
  if (g_twc.debug)
    printf("RESULT verb=bossstart state=ACTIVE hp=%d/%d rate=%d reason=%s\n",
           hp, hp, rate, reason);
  BossFeedActive();
}

// Attack tables (design B.2): pick the next INACTIVE entry (politeness:
// never re-arm a live effect), round-robin with the rotating index. An
// entry may carry up to two verbs fired together (P3 pairs). swarm is an
// instant world verb with no effect slot, so it is always "free".
typedef struct { const char *verb, *arg; } TwBossMove;
typedef struct { TwBossMove m[2]; int n; } TwBossEntry;
static const TwBossEntry kBossP1[] = {
  { { { "freeze", "" } }, 1 },
  { { { "slow", "" } }, 1 },
  { { { "ice", "" } }, 1 },
};
static const TwBossEntry kBossP2[] = {
  { { { "freeze", "" } }, 1 },
  { { { "root", "" } }, 1 },
  { { { "curse", "" } }, 1 },
  { { { "ice", "" } }, 1 },
  { { { "swarm", "2" } }, 1 },
};
static const TwBossEntry kBossP3[] = {
  { { { "curse", "" }, { "slow", "" } }, 2 },
  { { { "ice", "" }, { "swarm", "3" } }, 2 },
  { { { "confuse", "2" }, { "flip", "" } }, 2 },   // "party"
  { { { "deny", "" } }, 1 },
};
static const int kBossAtkFrames[3] = { 240, 360, 480 };   // P1/P2/P3 explicit
static const int kBossAtkCadence[3] = { 900, 720, 540 };  // P1/P2/P3 every-N

static int BossMoveFx(const char *verb) {
  if (!strcmp(verb, "freeze") || !strcmp(verb, "stun"))
    return kFxHeadFreeze;
  if (!strcmp(verb, "slow"))
    return kFxSlow;
  if (!strcmp(verb, "ice"))
    return kFxIce;
  if (!strcmp(verb, "root"))
    return kFxRoot;
  if (!strcmp(verb, "curse"))
    return kFxCurse;
  if (!strcmp(verb, "deny"))
    return kFxDenyY;
  if (!strcmp(verb, "denyboots"))
    return kFxDenyBoots;
  if (!strcmp(verb, "steal"))
    return kFxSteal;
  if (!strcmp(verb, "confuse"))
    return kFxConfuse;
  if (!strcmp(verb, "flip"))
    return kFxFlip;
  return -1;                               // instant verbs (swarm)
}

static bool BossEntryFree(const TwBossEntry *e) {
  for (int i = 0; i < e->n; i++) {
    int fx = BossMoveFx(e->m[i].verb);
    if (fx >= 0 && FxActive(fx))
      return false;
  }
  return true;
}

// One attack pulse: fire the next fireable table entry through the normal
// dispatcher (gameplay-gated, feed-visible as "THE BOSS").
static void BossAttack(void) {
  const TwBossEntry *tab = (g_boss.phase == 1) ? kBossP1
                         : (g_boss.phase == 2) ? kBossP2 : kBossP3;
  const int n = (g_boss.phase == 1) ? 3 : (g_boss.phase == 2) ? 5 : 4;
  int frames = kBossAtkFrames[g_boss.phase - 1];
  for (int k = 0; k < n; k++) {
    int i = (g_boss.rot + k) % n;
    if (!BossEntryFree(&tab[i]))
      continue;
    g_boss.rot = (i + 1) % n;
    snprintf(g_boss.last_attack, sizeof(g_boss.last_attack), "%s", tab[i].m[0].verb);
    for (int m = 0; m < tab[i].n; m++)
      // steal is fixed 20 s inside the dispatcher; everything else takes
      // the phase's explicit frames (curse runs 4x internally by design)
      BossApply(tab[i].m[m].verb, tab[i].m[m].arg, "THE BOSS",
                !strcmp(tab[i].m[m].verb, "steal") ? 0 : frames);
    if (g_twc.debug)
      printf("RESULT verb=boss attack=%s phase=%d\n", tab[i].m[0].verb, g_boss.phase);
    return;
  }
  if (g_twc.debug)
    printf("RESULT verb=boss attack=skip reason=all_active phase=%d\n", g_boss.phase);
}

// Per-frame state machine. Everything here is main-thread (Twitch_Tick).
static void TwitchBossTick(void) {
  if (!g_twc.boss_enabled)
    return;
  if (g_boss.state == kBossDormant) {
    // Auto-trigger (owner decision #2): trailing command rate crosses the
    // threshold, the inter-fight cooldown has passed, not vs_mode, and real
    // gameplay. test=1 bypasses ONLY the gameplay check so the harness can
    // drive the whole auto path from the title screen; production streams
    // (test=0) never auto-start outside dungeon/overworld gameplay.
    if (++g_boss.auto_ctr < 60)
      return;
    g_boss.auto_ctr = 0;
    if (BossTrailingRate(g_tw_frame_ctr) < g_twc.boss_min_rate)
      return;
    if (!BossCooldownReady(g_tw_frame_ctr))
      return;
    if (g_twc.vs_mode)
      return;
    if (!TwitchInGameplay() && !g_twc.test_mode)
      return;
    BossStart("auto");
    return;
  }
  if (g_boss.state == kBossResolve) {
    if (--g_boss.resolve <= 0)
      g_boss.state = kBossDormant;         // boss_feed.txt persists: the
    return;                                // overlay keeps the last result
  }
  // ---- ACTIVE ----
  if (--g_boss.window <= 0) {
    BossDefeat();
    return;
  }
  if (--g_boss.pulse <= 0) {
    g_boss.pulse = 120;                    // 2 s damage settle (design B.2)
    if (g_boss.pending > 0) {
      g_boss.hp -= g_boss.pending;
      g_boss.pending = 0;
      if (g_boss.hp < 0)
        g_boss.hp = 0;
    }
    if (g_boss.hp == 0) {
      BossVictory();
      return;
    }
    BossFeedActive();                      // rewritten per pulse/change
  }
  // Phase tracking + the one-time phase-crossing attacks (design B.2).
  // Only fired in real gameplay; otherwise they stay armed for the next
  // eligible frame rather than being wasted on a cutscene tick.
  int pct_x1000 = g_boss.hp * 1000 / g_boss.maxhp;
  g_boss.phase = (pct_x1000 > 660) ? 1 : (pct_x1000 >= 330) ? 2 : 3;
  bool gameplay = TwitchInGameplay();
  if (gameplay) {
    if (g_boss.phase >= 2 && !g_boss.steal_used) {
      g_boss.steal_used = true;
      BossApply("steal", "", "THE BOSS", 0);
      if (g_twc.debug)
        printf("RESULT verb=boss attack=steal one_time=1 phase=2\n");
    }
    if (g_boss.phase == 3 && pct_x1000 <= 200 && !g_boss.desp_used) {
      g_boss.desp_used = true;             // "desperation"
      BossApply("denyboots", "", "THE BOSS", kBossAtkFrames[2]);
      BossApply("curse", "", "THE BOSS", kBossAtkFrames[2]);
      if (g_twc.debug)
        printf("RESULT verb=boss attack=denyboots one_time=1 phase=3\n");
    }
  }
  // Attack scheduler: attacks skip ticks outside gameplay (the fight is an
  // event, but we never throw effects during cutscenes/transitions); the
  // window itself keeps running so a fight always concludes.
  if (--g_boss.atk <= 0) {
    g_boss.atk = kBossAtkCadence[g_boss.phase - 1];
    if (!gameplay) {
      if (g_twc.debug)
        printf("RESULT verb=boss attack=skip reason=not_in_gameplay\n");
    } else {
      BossAttack();
    }
  }
}

// Enemy sprite types (positional indices into the 243-entry kSpriteActiveRoutines
// (sprite_main.c:465, entry 0 = :466) and kSpritePrep_Main (:713, entry 0 = :714).
// Ids verified positionally 2026-09-10; the decomp's Sprite_NN_* NAMES drift from
// their positional index in the 0x6D-0xA8 range, so positions govern.
static const struct { int id; const char *name; } kSpawnTable[] = {
  { 0, "raven" },     // kSpriteActiveRoutines[0]=Sprite_Raven (:466), kSpritePrep_Main[0]=SpritePrep_Raven (:714)
  { 8, "octorok" },   // kSpriteActiveRoutines[8]=Sprite_08_Octorok (:474), kSpritePrep_Main[8]=SpritePrep_Octorok (:722). (old 7 = pull switch: SpritePrep_SwitchFacingUp :721)
  { 111, "keese" },   // kSpriteActiveRoutines[111]=Sprite_6F_Keese (:578), kSpritePrep_Main[111]=SpritePrep_Keese (:825). (old 112 = Helmasaur boss fireball, Sprite_70_KingHelmasaurFireball :579, which TriSplits/QuadSplits into more 0x70 fireballs -- the old "keese cloning")
  { 156, "zoro" },    // kSpriteActiveRoutines[156]=Sprite_9C_Zoro (:624), kSpritePrep_Main[156]=SpritePrep_Zoro (:870; prep computes sprite_type-0x9c). (old 157 = zoro variant prepped as Babasu :871)
  { 167, "stalfos" }, // kSpriteActiveRoutines[167]=Sprite_A7_Stalfos (:635), kSpritePrep_Main[167]=SpritePrep_Stalfos (:881). (old 168 = green zirro/bomber: Sprite_A8_GreenZirro :636 / SpritePrep_Bomber :882)
};

static int TwitchSpawnLookup(const char *name) {
  for (size_t i = 0; i < sizeof(kSpawnTable) / sizeof(kSpawnTable[0]); i++)
    if (!strcmp(name, kSpawnTable[i].name))
      return kSpawnTable[i].id;
  return -1;
}

static void TwitchSpawnOne(int type, int slot, bool left_side) {
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(0, (uint8)type, &info);
  if (j < 0)
    return;
  int off = 24 + 16 * slot;
  Sprite_SetX(j, link_x_coord + (left_side ? -off : off));
  Sprite_SetY(j, link_y_coord);
  sprite_floor[j] = link_is_on_lower_level;
  sprite_D[j] = 0;
  sprite_z[j] = 0;
}

// verbs that touch the world's sprite slots; everything else is stat/flag-only
static bool TwitchIsWorldVerb(const char *verb) {
  return !strcmp(verb, "smite") || !strcmp(verb, "clearscreen") ||
         !strcmp(verb, "spawn") || !strcmp(verb, "swarm") ||
         !strcmp(verb, "fairy") || !strcmp(verb, "arise") ||
         !strcmp(verb, "chicken") || !strcmp(verb, "cucco");
}

static const char *const kFxNames[kFxCount] = {
  "confuse", "flip", "freeze", "curse", "root", "steal",
  "speed", "slow", "ice", "deny", "denyboots", "cucco",
};

// ----------------------------------------------------- per-viewer limits --
// GAP_CHECK #6: "no per-user limits - cooldown is global; one account
// spamming at the cooldown rate can keep the game permanently
// confused+flipped+frozen". Three caps layered ON TOP of the existing global
// IRC cooldown (which stays exactly as it was), all boot-only
// twitch_config.txt keys:
//
//   per_user_limits=1         master kill switch for all three (0 = off)
//   per_user_cooldown_secs=30 same viewer + same verb: minimum seconds
//                             between fires (0 = off)
//   per_user_max_effects=2    max simultaneously-active effect slots one
//                             viewer may own (0 = off)
//   global_max_effects=8      max simultaneously-active effect slots from
//                             ALL viewers together (0 = off); extra fires
//                             reject with RESULT reason=stacking_limit
//
// EXEMPTIONS - the limits gate on the "real chatter" signal, the same shape
// the boss loyalty/crit path uses to skip non-chat traffic
// (g_tw_boss_source). NEVER limited:
//   - test=1 traffic (the automated harness runs its whole suite this way)
//   - the drop-folder path (TwitchPollDrop): the harness, the
//     gameplay_verify rig AND the phase-2 suite fire machine-paced
//     back-to-back volleys from fixed fake whos on purpose - and the rig
//     runs test=0, so drop=1 is the only exemption the rig can rely on
//   - boss-initiated effects (g_tw_boss_source: attacks + win/lose payoffs)
//   debug=1 is deliberately NOT an exemption: the production stream runs
//   debug=1 and must stay limited.
// Persistent modes (attrition/dmgup/.../hardmods) and the !bosshp query sit
// BEFORE this gate on purpose: they are configuration/query, not effects.
//
// Counting unit is the effect SLOT (kFxCount of them). A single verb is
// never split by the caps: cucco arms three slots at once (ride + both deny
// gates) and party two (confuse + flip), so a just-accepted multi-slot verb
// can overshoot its owner's / the global count by up to two slots - always
// bounded by kFxCount, and the caps still gate every NEW verb. bunny is not
// counted (it rides the engine's tempbunny timer, not an fx slot).
//
// Bounded memory: the cooldown table is a fixed 64-row ring reused
// round-robin - no malloc, no growth with chat size. A wraparound evicts
// the oldest row, which can only ever let one (viewer, verb) pair fire
// early again; graceful degradation. Twitch nicks cap at 25 chars, so the
// 32-byte rows never truncate a real viewer (drop whos are exempt anyway).

static bool g_tw_drop_source;           // TwitchPollDrop around TwitchApply
                                        // (main thread only, same shape as
                                        // g_tw_boss_source: no mutex needed)

#define TW_UCD_CAP 64                   // (viewer, verb) cooldown rows
#define TW_UCD_WHO 32                   // = TwEffect.owner / TW_CHATTER_LEN
typedef struct TwUserCd {
  char who[TW_UCD_WHO];
  char verb[24];
  int frame;                            // last-fire g_tw_frame_ctr, 0 = free
} TwUserCd;
static TwUserCd g_tw_ucd[TW_UCD_CAP];
static int g_tw_ucd_next;               // next row the ring reuses

// Do the per-viewer limits apply to the TwitchApply call in flight? True
// only for real IRC chatters (exemptions above).
static bool TwitchLimitsApply(void) {
  return g_twc.per_user_limits && !g_twc.test_mode && !g_tw_drop_source &&
         !g_tw_boss_source && !g_tw_external_source;
}

// Same viewer + same verb inside the cooldown window? Case-insensitive on
// the nick (real nicks are lowercase; drop whos are arbitrary case), exact
// on the verb - aliases are distinct rows on purpose (!stun and !freeze
// each get their own window, matching their own RESULT names).
static bool TwitchUserCooldownHit(const char *who, const char *verb) {
  int secs = g_twc.per_user_cd_secs;
  if (secs <= 0 || !who[0])
    return false;
  for (int i = 0; i < TW_UCD_CAP; i++) {
    TwUserCd *e = &g_tw_ucd[i];
    if (e->frame != 0 && !tw_stricmp(e->who, who) && !strcmp(e->verb, verb))
      return g_tw_frame_ctr - e->frame < secs * 60;
  }
  return false;
}

// Record/refresh a last-fire. Called only at TwitchApply's accept point,
// mirroring the global cooldown's semantics: an accepted command consumes
// the window even if the verb then no-ops internally (cucco already_cucco,
// steal nothing_to_steal), while commands dropped by the earlier gates
// (vs_mode, not_in_gameplay, per-viewer limits) never touch it.
static void TwitchUserCooldownMark(const char *who, const char *verb) {
  TwUserCd *e = NULL;
  if (g_twc.per_user_cd_secs <= 0 || !who[0])
    return;
  for (int i = 0; i < TW_UCD_CAP && !e; i++)
    if (g_tw_ucd[i].frame != 0 && !tw_stricmp(g_tw_ucd[i].who, who) &&
        !strcmp(g_tw_ucd[i].verb, verb))
      e = &g_tw_ucd[i];                  // same pair: refresh in place
  if (!e) {
    e = &g_tw_ucd[g_tw_ucd_next];
    g_tw_ucd_next = (g_tw_ucd_next + 1) % TW_UCD_CAP;
  }
  snprintf(e->who, sizeof(e->who), "%s", who);
  snprintf(e->verb, sizeof(e->verb), "%s", verb);
  e->frame = g_tw_frame_ctr;
}

// Active effect slots currently owned by one viewer / by everyone. Unowned
// slots (exempt sources) only ever count toward the global cap.
static int TwitchFxOwned(const char *who) {
  int n = 0;
  if (!who[0])
    return 0;
  for (int i = 0; i < kFxCount; i++)
    if (FxActive(i) && g_tw_fx[i].owner[0] &&
        !tw_stricmp(g_tw_fx[i].owner, who))
      n++;
  return n;
}

static int TwitchFxTotal(void) {
  int n = 0;
  for (int i = 0; i < kFxCount; i++)
    if (FxActive(i))
      n++;
  return n;
}

// How many effect slots a verb (re)arms; 0 = instant/stat verb, never
// cap-checked. Shares BossMoveFx's mapping (freeze/stun, slow, ice, root,
// curse, deny, denyboots, steal, confuse, flip) and adds the multi-slot
// specials below.
static int TwitchVerbFxSlots(const char *verb) {
  if (BossMoveFx(verb) >= 0)
    return 1;
  if (!strcmp(verb, "speed") || !strcmp(verb, "fast"))
    return 1;
  if (!strcmp(verb, "arise") || !strcmp(verb, "chicken") ||
      !strcmp(verb, "cucco"))
    return 3;                            // ride + denyY + denyBoots
  if (!strcmp(verb, "party"))
    return 2;                            // confuse + flip
  return 0;
}

// ---- viewer points + bets (owner 09-12 Discord Q3: "betting on boss kills
// yes clearly, chest contents for keys or progress items") ------------------
// Every chat message earns 1 point, every accepted command 2 more; a new
// viewer starts with 100. points.txt ("who points") is the ledger. Bets:
//   !bet <amount> win|lose   while a CHAT BOSS fight is on, pays 2x
//   !bet <amount> good|junk  on the next dungeon chest Link opens (good =
//                            a progression item of this seed's fill), 2x
// Channel points cannot be taken by a bot, so this ledger is the currency.
#define TW_PTS_MAX 512
#define TW_PTS_START 100
typedef struct { char who[32]; int pts; } TwPoints;
static TwPoints g_tw_pts[TW_PTS_MAX];
static int g_tw_pts_n;
static bool g_tw_pts_dirty;
static int g_tw_pts_flush;

static TwPoints *PointsRow(const char *who, bool create) {
  if (!who || !who[0]) return NULL;
  for (int i = 0; i < g_tw_pts_n; i++)
    if (!strcasecmp(g_tw_pts[i].who, who)) return &g_tw_pts[i];
  if (!create || g_tw_pts_n >= TW_PTS_MAX) return NULL;
  TwPoints *p = &g_tw_pts[g_tw_pts_n++];
  snprintf(p->who, sizeof p->who, "%s", who);
  p->pts = TW_PTS_START;
  g_tw_pts_dirty = true;
  return p;
}
static int PointsGet(const char *who) { TwPoints *p = PointsRow(who, true); return p ? p->pts : 0; }
static void PointsAdd(const char *who, int delta) {
  TwPoints *p = PointsRow(who, true);
  if (!p) return;
  p->pts += delta;
  if (p->pts < 0) p->pts = 0;
  g_tw_pts_dirty = true;
}
static void PointsSave(void) {
  FILE *f = fopen("points.txt.tmp", "wb");
  if (!f) return;
  fprintf(f, "# viewer points: name points (1 per chat message, 2 per command, bets pay 2x)%c", 10);
  for (int i = 0; i < g_tw_pts_n; i++) fprintf(f, "%s %d%c", g_tw_pts[i].who, g_tw_pts[i].pts, 10);
  bool ok = fclose(f) == 0;
#ifdef _WIN32
  if (ok) ok = MoveFileExA("points.txt.tmp", "points.txt", MOVEFILE_REPLACE_EXISTING) != 0;
#else
  if (ok) ok = rename("points.txt.tmp", "points.txt") == 0;
#endif
  if (!ok) remove("points.txt.tmp");
  g_tw_pts_dirty = false;
  g_tw_pts_flush = 0;
}
static void PointsLoad(void) {
  FILE *f = fopen("points.txt", "rb");
  if (!f) return;
  char line[128];
  while (fgets(line, sizeof line, f) && g_tw_pts_n < TW_PTS_MAX) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
    char who[32]; int pts;
    if (sscanf(line, "%31s %d", who, &pts) == 2) {
      TwPoints *p = &g_tw_pts[g_tw_pts_n++];
      snprintf(p->who, sizeof p->who, "%s", who);
      p->pts = pts < 0 ? 0 : pts;
    }
  }
  fclose(f);
  g_tw_pts_dirty = false;
}

#define TW_BET_MAX 64
typedef struct { char who[32]; int amount; int kind; } TwBet;
static TwBet g_tw_bets[TW_BET_MAX];
static int g_tw_bets_n;
static const char *const kBetKindNames[] = { "win", "lose", "good", "junk" };

// pay the bets of the class |won| belongs to: matching kind gets 2x, the
// other side loses its stake (already taken); both are cleared
static void BetsResolve(int won, const char *what) {
  int paid = 0, lost = 0;
  bool boss_class = won == kBet_Win || won == kBet_Lose;
  for (int i = 0; i < g_tw_bets_n;) {
    TwBet *b = &g_tw_bets[i];
    bool same_class = boss_class ? (b->kind == kBet_Win || b->kind == kBet_Lose)
                                 : (b->kind == kBet_Good || b->kind == kBet_Junk);
    if (!same_class) { i++; continue; }
    char feed[96];
    if (b->kind == won) {
      PointsAdd(b->who, b->amount * 2);
      snprintf(feed, sizeof feed, "bet %s: +%d points (%d)", kBetKindNames[b->kind], b->amount * 2, PointsGet(b->who));
      paid++;
    } else {
      snprintf(feed, sizeof feed, "bet %s: lost %d points (%d)", kBetKindNames[b->kind], b->amount, PointsGet(b->who));
      lost++;
    }
    Tracker_LogActivity(b->who, feed);
    g_tw_bets[i] = g_tw_bets[--g_tw_bets_n];
  }
  if (paid || lost) {
    char line[96];
    snprintf(line, sizeof line, "%s: %d bets paid, %d lost", what, paid, lost);
    Tracker_LogActivity("BETS", line);
    PointsSave();
  }
  if (g_twc.debug) printf("RESULT bets resolved=%s paid=%d lost=%d\n", kBetKindNames[won], paid, lost);
}
static void BetsRefund(bool boss_class, const char *why) {
  for (int i = 0; i < g_tw_bets_n;) {
    TwBet *b = &g_tw_bets[i];
    bool same_class = boss_class ? (b->kind == kBet_Win || b->kind == kBet_Lose)
                                 : (b->kind == kBet_Good || b->kind == kBet_Junk);
    if (!same_class) { i++; continue; }
    PointsAdd(b->who, b->amount);
    g_tw_bets[i] = g_tw_bets[--g_tw_bets_n];
  }
  if (why) Tracker_LogActivity("BETS", why);
}

// !bet <amount> <win|lose|good|junk> (either order)
static void TwitchBet(const char *arg, const char *who) {
  char a[16] = {0}, b[16] = {0};
  int amount = 0, kind = -1;
  if (arg) sscanf(arg, "%15s %15s", a, b);
  const char *words[2] = { a, b };
  for (int i = 0; i < 2; i++) {
    if (!words[i][0]) continue;
    if (words[i][0] >= '0' && words[i][0] <= '9') amount = atoi(words[i]);
    for (int k = 0; k < 4; k++) if (!strcasecmp(words[i], kBetKindNames[k])) kind = k;
  }
  char feed[96];
  if (kind < 0 || amount <= 0) {
    snprintf(feed, sizeof feed, "bet how? !bet 50 win  |  !bet 50 good");
  } else if (PointsGet(who) < amount) {
    snprintf(feed, sizeof feed, "bet %d: only %d points", amount, PointsGet(who));
  } else if ((kind == kBet_Win || kind == kBet_Lose) && (!g_twc.boss_enabled || g_boss.state != kBossActive)) {
    snprintf(feed, sizeof feed, "bet %s: no boss fight right now", kBetKindNames[kind]);
  } else if ((kind == kBet_Good || kind == kBet_Junk) && !Randomizer_FileEnabled()) {
    snprintf(feed, sizeof feed, "bet %s: needs the randomizer on", kBetKindNames[kind]);
  } else {
    bool dup = false;
    for (int i = 0; i < g_tw_bets_n; i++) {
      bool same_class = (kind <= kBet_Lose) == (g_tw_bets[i].kind <= kBet_Lose);
      if (same_class && !strcasecmp(g_tw_bets[i].who, who)) dup = true;
    }
    if (dup) snprintf(feed, sizeof feed, "bet: you already have one riding");
    else if (g_tw_bets_n >= TW_BET_MAX) snprintf(feed, sizeof feed, "bet: the book is full");
    else {
      TwBet *bt = &g_tw_bets[g_tw_bets_n++];
      snprintf(bt->who, sizeof bt->who, "%s", who);
      bt->amount = amount;
      bt->kind = kind;
      PointsAdd(who, -amount);
      snprintf(feed, sizeof feed, "bet %d on %s (%d left)", amount, kBetKindNames[kind], PointsGet(who));
      PointsSave();
    }
  }
  printf("[twitch] %s -> bet %s: %s\n", who, arg ? arg : "", feed);
  Tracker_LogActivity(who, feed);
  if (g_twc.debug) printf("RESULT verb=bet who=%s amount=%d kind=%d bets=%d points=%d\n", who, amount, kind, g_tw_bets_n, PointsGet(who));
}

void Twitch_NoteChestOpened(int item) {
  bool any = false;
  for (int i = 0; i < g_tw_bets_n; i++) if (g_tw_bets[i].kind >= kBet_Good) any = true;
  if (!any) return;
  char name[32]; int prog = 0;
  if (Randomizer_ChestRecordInfo(g_last_opened_chest_record, name, sizeof name, &prog)) {
    char what[64];
    snprintf(what, sizeof what, "chest: %s (%s)", name, prog ? "progress" : "junk");
    BetsResolve(prog ? kBet_Good : kBet_Junk, what);
  } else {
    BetsRefund(false, "chest bets refunded: not a randomized chest");
  }
  (void)item;
}

// CHAT BOSS names: the fixed rotation, and every other fight the last
// fight's MVP chatter with a title (the streamer did not pick; best effort)
static void BossPickName(void) {
  static const char *const kTitles[] = {
    "THE UNSTOPPABLE", "EATER OF COOLDOWNS", "THE SPAMLORD", "OF THE THOUSAND EMOTES", "WHO NEVER LURKS",
  };
  int idx = g_boss.name_idx++;
  if (g_boss_last_mvp[0] && (idx & 1)) {
    char up[TW_CHATTER_LEN];
    int n = 0;
    for (const char *p = g_boss_last_mvp; *p && n < TW_CHATTER_LEN - 1; p++) {
      char c = *p;
      if (c >= 'a' && c <= 'z') c -= 32;
      up[n++] = c;
    }
    up[n] = 0;
    snprintf(g_boss.name, sizeof(g_boss.name), "%s %s", up, kTitles[(idx / 2) % (int)(sizeof(kTitles) / sizeof(kTitles[0]))]);
  } else {
    snprintf(g_boss.name, sizeof(g_boss.name), "%s",
             kBossNames[idx % (int)(sizeof(kBossNames) / sizeof(kBossNames[0]))]);
  }
}

static void TwitchApply(const char *verb, const char *arg, const char *who, int dur_frames) {
  // !bosshp (CHAT BOSS v1) - pure query, admitted to chat: never vs-blocked
  // or gameplay-gated, and NOT an accepted command (no damage, no rate, no
  // scoreboard). RESULT line is the harness's boss-state oracle.
  if (!strcmp(verb, "bosshp")) {
    int attackers;
    TwChatter *top = BossTopChatter(&attackers);
    const char *st = !g_twc.boss_enabled ? "OFF"
                   : g_boss.state == kBossActive ? "ACTIVE"
                   : g_boss.state == kBossResolve ? "RESOLVE" : "DORMANT";
    char what[64];
    if (g_twc.boss_enabled && g_boss.state == kBossActive)
      snprintf(what, sizeof(what), "boss at %d/%d (phase %d)", g_boss.hp,
               g_boss.maxhp, g_boss.phase);
    else
      snprintf(what, sizeof(what), "the boss sleeps");
    printf("[twitch] %s -> bosshp %s\n", who, what);
    Tracker_LogActivity(who, what);
    if (g_twc.debug)
      printf("RESULT verb=bosshp state=%s hp=%d/%d phase=%d window=%d "
             "attackers=%d top=%s/%d rate=%d\n",
             st, g_boss.hp, g_boss.maxhp, g_boss.phase,
             g_boss.state == kBossActive ? (g_boss.window + TW_BOSS_FPS - 1) / TW_BOSS_FPS : 0,
             attackers, top ? top->who : "-", top ? top->fight : 0,
             BossTrailingRate(g_tw_frame_ctr));
    return;
  }
  if (!strcmp(verb, "points")) {
    char what[64];
    snprintf(what, sizeof what, "%d points", PointsGet(who));
    Tracker_LogActivity(who, what);
    if (g_twc.debug) printf("RESULT verb=points who=%s points=%d\n", who, PointsGet(who));
    return;
  }
  if (!strcmp(verb, "bet")) {
    TwitchBet(arg, who);
    return;
  }
  // ROCKET LEAGUE quick chat box (src/quickchat.c). Cosmetic overlay only -
  // no game state, so these sit with the query verbs: no vs-block, no
  // gameplay gate, no per-viewer limits, no scoreboard, no boss damage.
  if (!strcmp(verb, "qc")) {
    QuickChat_ChatLine(who, arg);
    if (g_twc.debug) printf("RESULT verb=qc who=%s text=%s\n", who, arg ? arg : "");
    return;
  }
  if (!strcmp(verb, "whatasave")) {
    QuickChat_BurstWhatASave();
    if (g_twc.debug) printf("RESULT verb=whatasave who=%s\n", who);
    return;
  }
  if (!strcmp(verb, "gg")) {
    QuickChat_BurstGG();
    if (g_twc.debug) printf("RESULT verb=gg who=%s\n", who);
    return;
  }
  // MUSIC FOLDER (src/music_player.c): also cosmetic - no game state.
  if (!strcmp(verb, "nowplaying") || !strcmp(verb, "skipsong")) {
    char np[64];
    if (verb[0] == 's')
      MusicPlayer_Skip();                 // the toast announces the new one
    else if (MusicPlayer_NowPlaying(np, sizeof np))
      QuickChat_ChatLine("Music", np);
    else
      QuickChat_ChatLine("Music", "NO CUSTOM TRACK PLAYING");
    if (g_twc.debug) printf("RESULT verb=%s who=%s\n", verb, who);
    return;
  }
  if (TwitchVsBlocked(verb)) {
    printf("[twitch] %s -> %s BLOCKED (VS mode)\n", who, verb);
    if (g_twc.debug) printf("RESULT verb=%s DROPPED reason=vs_mode\n", verb);
    return;
  }
  if (TwitchModeDispatch(verb, arg, who))
    return;                               // persistent modes: no gameplay gate
  if (!TwitchInGameplay() && !(g_twc.test_mode && !TwitchIsWorldVerb(verb))) {
    printf("[twitch] %s -> %s dropped (not in gameplay)\n", who, verb);
    if (g_twc.debug) printf("RESULT verb=%s DROPPED reason=not_in_gameplay\n", verb);
    return;
  }
  // Per-viewer limits (GAP_CHECK #6, see the limits block above): real
  // chatters only. Cooldown first, then the two stacking caps on verbs that
  // start effect slots. All reject BEFORE the accept line, so a limited
  // command never reaches the activity feed, the scoreboard or the boss.
  bool limited = TwitchLimitsApply();
  if (limited) {
    if (TwitchUserCooldownHit(who, verb)) {
      printf("[twitch] %s -> %s dropped (user cooldown %ds)\n", who, verb,
             g_twc.per_user_cd_secs);
      if (g_twc.debug)
        printf("RESULT verb=%s DROPPED reason=user_cooldown\n", verb);
      return;
    }
    if (TwitchVerbFxSlots(verb) > 0) {
      if (g_twc.per_user_max_fx > 0 &&
          TwitchFxOwned(who) >= g_twc.per_user_max_fx) {
        printf("[twitch] %s -> %s dropped (owns %d/%d effects)\n", who, verb,
               TwitchFxOwned(who), g_twc.per_user_max_fx);
        if (g_twc.debug)
          printf("RESULT verb=%s DROPPED reason=user_effect_cap\n", verb);
        return;
      }
      if (g_twc.global_max_fx > 0 && TwitchFxTotal() >= g_twc.global_max_fx) {
        printf("[twitch] %s -> %s dropped (%d/%d effects active)\n", who, verb,
               TwitchFxTotal(), g_twc.global_max_fx);
        if (g_twc.debug)
          printf("RESULT verb=%s DROPPED reason=stacking_limit\n", verb);
        return;
      }
    }
  }
  printf("[twitch] %s -> %s %s\n", who, verb, arg);
  Tracker_LogActivity(who, verb);      // stream overlay feed
  if (!g_tw_boss_source) {             // boss attacks/payoffs ride this same
                                       // dispatcher for the gate + feed line
                                       // but are not chat traffic (boss v1)
    Scoreboard_LogCommand();           // scoreboard.txt tally (kept out of
                                       // Tracker_LogActivity because the IRC
                                       // connect tagline shares that path and
                                       // must not count as an applied command)
    TwitchBossAccept(who);             // CHAT BOSS v1: rate + loyalty + damage
    if (!g_tw_drop_source && !g_tw_external_source) PointsAdd(who, 2);   // viewer points
  }
  // Per-viewer bookkeeping for the limits: real chatters stamp their
  // cooldown row and the pending effect owner; exempt sources clear the
  // pending owner so their (re)arms never count against a viewer's budget.
  snprintf(g_tw_fx_owner, sizeof(g_tw_fx_owner), "%s", limited ? who : "");
  if (limited)
    TwitchUserCooldownMark(who, verb);
  int before;

  // 69/420 reaction: lane's chat already does "!speed 69" unprompted -
  // celebrate it with a free fairy wherever one will actually spawn
  {
    int joke_n = TwitchNum(arg, -1);
    if ((joke_n == 69 || joke_n == 420) && TwitchInGameplay()) {
      ReleaseFairy();
      if (g_twc.debug) printf("RESULT verb=%s nice=1\n", verb);
    }
  }

  if (!strcmp(verb, "msg") && (g_twc.test_mode || g_tw_drop_source)) {  // harness only
    int n = atoi(arg ? arg : "");   // numbered like dialogue.txt (1-based)
    if (n > 0 && n < 512) {
      Sprite_ShowMessageUnconditional((uint16)(n - 1));
      if (g_twc.debug) printf("RESULT verb=msg n=%d\n", n);
    }
    return;
  }

  if (!strcmp(verb, "heal")) {
    before = link_health_current;
    int n = TwitchNum(arg, link_health_capacity);
    if (n < 0) n = 0;
    int v = before + n;
    link_health_current = (v > link_health_capacity) ? link_health_capacity : (uint8)v;
    if (g_twc.debug) printf("RESULT verb=heal hp=%d->%d\n", before, link_health_current);
  } else if (!strcmp(verb, "hurt")) {
    before = link_health_current;
    int n = TwitchNum(arg, 16);
    int v = before - n;                  // negative arg flips to a heal (kept)
    // Clamp the RESULT, not just the byte: a negative arg used to wrap
    // (!hurt -96 at 0xa0 health -> v = 0x100 -> (uint8)0) and a plain
    // overdose used to strand the engine at 0 health OUTSIDE the damage
    // path - the game-over transition lives only in Link_ControlHandler's
    // damage sequence (player.c, new_dmg == 0 || >= 0xa8), so an externally
    // written 0 never enters the death module and Link stands dead-not-
    // dying until some enemy lands a real hit. Chat damage is therefore
    // NON-LETHAL like the curse tick above (floor 1, a sliver of a heart;
    // the next real hit then dies through the engine's own path), and
    // heals from negative args cap at the current capacity.
    if (v > link_health_capacity) v = link_health_capacity;
    if (v < 1)
      v = (before > 0) ? 1 : 0;
    link_health_current = (uint8)v;
    if (g_twc.debug) printf("RESULT verb=hurt hp=%d->%d\n", before, link_health_current);
    // chat hurt is damage taken: attrition applies (production hurt is
    // already gameplay-gated above; at the title capacity is 0 and the
    // removal no-ops unless the harness seeded it with setcap)
    AttritionRemoveContainer();
  } else if (!strcmp(verb, "mp")) {
    before = link_magic_power;
    int n = TwitchNum(arg, 0x80);
    if (n < 0) n = 0;
    int v = before + n;
    link_magic_power = (uint8)(v > 0x80 ? 0x80 : v);
    if (g_twc.debug) printf("RESULT verb=mp magic=%d->%d\n", before, link_magic_power);
  } else if (!strcmp(verb, "drain")) {
    before = link_magic_power;
    int n = TwitchNum(arg, 0x20);
    int v = before - n;                  // negative arg flips to a refill (kept)
    // Same result-clamp rule as hurt: a negative arg used to wrap the byte
    // (!drain -383 -> 0x80 + 383 = 0xff against the 0x80 bar). The engine's
    // magic ceiling is 0x80 everywhere (hud.c refill caps at 128, this
    // file's mp/refill/mpsteal all use 0x80), so clamp [0, 0x80] first.
    if (v > 0x80) v = 0x80;
    if (v < 0) v = 0;
    link_magic_power = (uint8)v;
    if (g_twc.debug) printf("RESULT verb=drain magic=%d->%d\n", before, link_magic_power);
  } else if (!strcmp(verb, "refill")) {
    link_health_current = link_health_capacity;
    link_magic_power = 0x80;
    if (g_twc.debug) printf("RESULT verb=refill hp=%d magic=%d\n", link_health_capacity, 0x80);
  } else if (!strcmp(verb, "rupees")) {
    before = link_rupees_actual;
    int n = TwitchNum(arg, 100);
    int v = before + n;
    link_rupees_actual = (uint16)(v < 0 ? 0 : (v > 9999 ? 9999 : v));
    if (g_twc.debug) printf("RESULT verb=rupees rupees=%d->%d\n", before, link_rupees_actual);
  } else if (!strcmp(verb, "tax")) {
    before = link_rupees_actual;
    int n = TwitchNum(arg, 100);
    int v = before - (n < 0 ? -n : n);
    link_rupees_actual = (uint16)(v < 0 ? 0 : v);
    if (g_twc.debug) printf("RESULT verb=tax rupees=%d->%d\n", before, link_rupees_actual);
  } else if (!strcmp(verb, "givekey")) {
    // a small key for the dungeon Link is in (the streamer's list)
    if (!player_is_indoors) {
      Tracker_LogActivity(who, "givekey: not in a dungeon");
      if (g_twc.debug) printf("RESULT verb=givekey DROPPED reason=outdoors\n");
      return;
    }
    before = link_num_keys;
    if (link_num_keys < 99) link_num_keys++;
    if (g_twc.debug) printf("RESULT verb=givekey keys=%d->%d\n", before, link_num_keys);
  } else if (!strcmp(verb, "bombs")) {
    before = link_item_bombs;
    int n = TwitchNum(arg, 10);
    link_item_bombs = (uint8)(n < 0 ? 0 : (n > 30 ? 30 : n));
    if (g_twc.debug) printf("RESULT verb=bombs bombs=%d->%d\n", before, link_item_bombs);
  } else if (!strcmp(verb, "arrows")) {
    before = link_num_arrows;
    int n = TwitchNum(arg, 30);
    link_num_arrows = (uint8)(n < 0 ? 0 : (n > 40 ? 40 : n));
    if (g_twc.debug) printf("RESULT verb=arrows arrows=%d->%d\n", before, link_num_arrows);
  } else if (!strcmp(verb, "smite") || !strcmp(verb, "clearscreen")) {
    int killed = 0;
    for (int j = 0; j < 16; j++) {
      if (sprite_state[j] != 0 && sprite_health[j] > 0) {
        sprite_health[j] = 0;
        sprite_hit_timer[j] = 0x14;
        killed++;
      }
    }
    if (g_twc.debug) printf("RESULT verb=smite killed=%d\n", killed);
  } else if (!strcmp(verb, "spawn")) {
    char name[64];
    snprintf(name, sizeof(name), "%s", arg);
    char *sp = strchr(name, ' ');
    int count = 1;
    if (sp) {
      *sp = 0;
      int v = atoi(sp + 1);
      if (v > 0) count = v;
    }
    // "3 octorok" and "octorok 3" both accepted; bare number = random type
    // cucco solidarity: while Link IS a cucco, the "enemies" spawn as his
    // own kind instead (approximation of the avenger-horde idea)
    int cucco_mode = FxActive(kFxCucco);
    int type = cucco_mode ? 0x0B : TwitchSpawnLookup(name);
    if (type < 0 && (count = atoi(name)) > 0)
      type = cucco_mode ? 0x0B : kSpawnTable[TwitchRandRange(sizeof(kSpawnTable) / sizeof(kSpawnTable[0]))].id;
    if (type < 0)
      type = cucco_mode ? 0x0B : kSpawnTable[TwitchRandRange(sizeof(kSpawnTable) / sizeof(kSpawnTable[0]))].id;
    if (count > 6) count = 6;
    for (int i = 0; i < count; i++)
      TwitchSpawnOne(type, i, (i & 1) != 0);
    if (g_twc.debug) printf("RESULT verb=spawn type=%d count=%d\n", type, count);
  } else if (!strcmp(verb, "swarm")) {
    int count = TwitchNum(arg, 3);
    if (count > 6) count = 6;
    int type = FxActive(kFxCucco) ? 0x0B : -1;   // cucco solidarity
    for (int i = 0; i < count; i++)
      TwitchSpawnOne(type >= 0 ? type : kSpawnTable[TwitchRandRange(sizeof(kSpawnTable) / sizeof(kSpawnTable[0]))].id, i, (i & 1) != 0);
    if (g_twc.debug) printf("RESULT verb=swarm count=%d cuccos=%d\n", count, type >= 0);
  } else if (!strcmp(verb, "speed") || !strcmp(verb, "fast")) {
    TwEffect *e = &g_tw_fx[kFxSpeed];
    if (!FxActive(kFxSpeed))
      e->saved_val = link_speed_setting;
    FxStartFrames(kFxSpeed, EffectFrames(dur_frames));
    if (g_twc.debug) printf("RESULT verb=speed effect=START frames=%d\n", e->frames);
  } else if (!strcmp(verb, "slow")) {
    TwEffect *e = &g_tw_fx[kFxSlow];
    if (!FxActive(kFxSlow))
      e->saved_val = link_speed_setting;
    FxStartFrames(kFxSlow, EffectFrames(dur_frames));
    if (g_twc.debug) printf("RESULT verb=slow effect=START frames=%d\n", e->frames);
  } else if (!strcmp(verb, "ice") || !strcmp(verb, "icefloor")) {
    FxStartFrames(kFxIce, EffectFrames(dur_frames));
    if (g_twc.debug) printf("RESULT verb=ice effect=START frames=%d\n", g_tw_fx[kFxIce].frames);
  } else if (!strcmp(verb, "illusion") || !strcmp(verb, "bunny")) {
    // engine's own temp-bunny: poof in, timer, poof back - works in the
    // Light World with the pearl, and reverts with correct palettes
    if (link_is_bunny != 0) {
      if (g_twc.debug) printf("RESULT verb=bunny DROPPED reason=already_bunny\n");
      return;
    }
    int f = EffectFrames(dur_frames);
    if (f > 60000) f = 60000;            // timer is uint16
    link_timer_tempbunny = (uint16)f;    // engine poofs and counts down itself
    if (g_twc.debug) printf("RESULT verb=bunny effect=START frames=%d\n", f);
  } else if (!strcmp(verb, "fairy")) {
    ReleaseFairy();
    if (g_twc.debug) printf("RESULT verb=fairy spawned=1\n");
  } else if (!strcmp(verb, "deny")) {
    // blocks use of whatever Y-item is equipped (bottles + remapped buttons too)
    FxStartFrames(kFxDenyY, EffectFrames(dur_frames));
    {
      // controller-aware (owner 09-12): the feed names the physical button
      char m[80];
      int pad = Controls_GetPad(kCtl_Y);
      snprintf(m, sizeof m, "deny: Y item locked %ds (%s on the pad, %s key)", g_tw_fx[kFxDenyY].frames / 60,
               pad >= 0 ? Controls_PadLabel(pad) : "unbound", Controls_KeyLabel(Controls_GetKey(kCtl_Y)));
      Tracker_LogActivity(who, m);
    }
    if (g_twc.debug) printf("RESULT verb=deny effect=START frames=%d\n", g_tw_fx[kFxDenyY].frames);
  } else if (!strcmp(verb, "denyboots")) {
    FxStartFrames(kFxDenyBoots, EffectFrames(dur_frames));
    if (g_twc.debug) printf("RESULT verb=denyboots effect=START frames=%d\n", g_tw_fx[kFxDenyBoots].frames);
  } else if (!strcmp(verb, "arise") || !strcmp(verb, "chicken") || !strcmp(verb, "cucco")) {
    // Link becomes a cucco: a live cucco sprite rides Link's position for
    // the duration (items + dash blocked); on expiry it's released to the
    // engine - attack it and Kakariko's avenger swarm rules apply
    TwEffect *e = &g_tw_fx[kFxCucco];
    if (FxActive(kFxCucco)) {
      if (g_twc.debug) printf("RESULT verb=cucco DROPPED reason=already_cucco\n");
      return;
    }
    SpriteSpawnInfo info;
    int j = Sprite_SpawnDynamically(0, 0x0B, &info);   // 0x0B = cucco
    if (j < 0) {
      if (g_twc.debug) printf("RESULT verb=cucco DROPPED reason=no_free_slot\n");
      return;
    }
    e->held = j;
    Sprite_SetX(j, link_x_coord);
    Sprite_SetY(j, link_y_coord);
    sprite_floor[j] = link_is_on_lower_level;
    sprite_D[j] = 0;
    sprite_z[j] = 0;
    sprite_pause[j] = 1;
    int f = EffectFrames(dur_frames);
    FxStartFrames(kFxCucco, f);
    FxStartFrames(kFxDenyY, f);          // chickens can't open menus
    FxStartFrames(kFxDenyBoots, f);      // ...and can't dash
    if (g_twc.debug) printf("RESULT verb=cucco effect=START frames=%d slot=%d\n", f, j);
  } else if (!strcmp(verb, "confuse")) {
    // lane: confuse is measured in SCREEN TRANSITIONS, not time - if fired
    // mid-boss-fight it lasts the whole fight, so chat can't wait it out
    int scr = TwitchNum(arg, 2);
    if (scr < 1) scr = 1;
    if (scr > 20) scr = 20;
    g_tw_fx[kFxConfuse].screens = scr;
    g_tw_fx[kFxConfuse].frames = 0;      // no time limit; screens drive expiry
    FxSetOwner(kFxConfuse, g_tw_fx_owner);   // direct slot write: stamp here
    if (g_twc.debug) printf("RESULT verb=confuse effect=START screens=%d\n", scr);
  } else if (!strcmp(verb, "flip")) {
    FxStartFrames(kFxFlip, EffectFrames(dur_frames));
    if (g_twc.debug) printf("RESULT verb=flip effect=START frames=%d\n", g_tw_fx[kFxFlip].frames);
  } else if (!strcmp(verb, "party")) {
    g_tw_fx[kFxConfuse].screens = 2;     // party = confuse(2 screens) + flip
    g_tw_fx[kFxConfuse].frames = 0;
    FxSetOwner(kFxConfuse, g_tw_fx_owner);   // direct slot write: stamp here
    FxStartFrames(kFxFlip, EffectFrames(dur_frames));
    if (g_twc.debug) printf("RESULT verb=party effect=START screens=2 frames=%d\n", g_tw_fx[kFxFlip].frames);
  } else if (!strcmp(verb, "freeze") || !strcmp(verb, "stun")) {
    FxStartFrames(kFxHeadFreeze, EffectFrames(dur_frames));
    if (g_twc.debug) printf("RESULT verb=freeze effect=START frames=%d\n", g_tw_fx[kFxHeadFreeze].frames);
  } else if (!strcmp(verb, "curse")) {
    g_tw_fx[kFxCurse].tick = 0;
    FxStartFrames(kFxCurse, EffectFrames(dur_frames) * 4);  // curse runs 4x as long
    if (g_twc.debug) printf("RESULT verb=curse effect=START frames=%d\n", g_tw_fx[kFxCurse].frames);
  } else if (!strcmp(verb, "root")) {
    FxStartFrames(kFxRoot, EffectFrames(dur_frames));
    if (g_twc.debug) printf("RESULT verb=root effect=START frames=%d\n", g_tw_fx[kFxRoot].frames);
  } else if (!strcmp(verb, "steal") || !strcmp(verb, "rob") || !strcmp(verb, "thief") || !strcmp(verb, "pickpocket")) {
    TwEffect *e = &g_tw_fx[kFxSteal];
    if (!FxActive(kFxSteal)) {
      int n = sizeof(kStealPool) / sizeof(kStealPool[0]);
      uint8 *cand[24];
      int nc = 0;
      for (int i = 0; i < n && nc < 24; i++)
        if (*kStealPool[i].addr != 0)
          cand[nc++] = kStealPool[i].addr;
      if (nc == 0) {
        printf("[twitch] nothing to steal\n");
        if (g_twc.debug) printf("RESULT verb=steal DROPPED reason=nothing_to_steal\n");
        return;
      }
      e->saved_addr = cand[TwitchRandRange(nc)];
      e->saved_val = *e->saved_addr;
      *e->saved_addr = 0;
    }
    FxStartFrames(kFxSteal, EffectFrames(20));  // steal fixed at 20s like ZALiA
    if (g_twc.debug) printf("RESULT verb=steal effect=START frames=%d\n", g_tw_fx[kFxSteal].frames);
  } else if (!strcmp(verb, "countdown")) {
    // test-gated: draws a big center-screen 3-2-1 so human-gated sessions
    // (route recording) can sync the owner's walk to the recorder
    if (!g_twc.debug)
      return;
    int secs = TwitchNum(arg, 3);
    if (secs < 1) secs = 1;
    if (secs > 10) secs = 10;
    g_tw_countdown_frames = secs * 60;
    printf("RESULT verb=countdown secs=%d\n", secs);
  } else if (!strcmp(verb, "setcap")) {
    // harness-only (TwitchIsVerb admits it under test=1): seed heart capacity
    // so attrition's container loss is observable at the title screen, where
    // capacity reads 0. Max 0xa0 = the engine's own 20-container cap.
    int n = TwitchNum(arg, 0);
    if (n < 0) n = 0;
    if (n > 0xa0) n = 0xa0;
    before = link_health_capacity;
    link_health_capacity = (uint8)n;
    if (link_health_current > link_health_capacity)
      link_health_current = link_health_capacity;
    if (g_twc.debug) printf("RESULT verb=setcap cap=%d->%d\n", before, link_health_capacity);
  } else if (!strcmp(verb, "modprobe")) {
    // harness-only (TwitchIsVerb admits it under test=1, like setcap): the
    // modifier modes' effects live at the damage choke point, and no damage
    // happens at the title screen - so push a SYNTHETIC hit through the
    // fan-out core and print the modified outputs, making dmgup doubling and
    // the mpsteal/rupeesteal drains harness-verifiable without gameplay.
    // Real state is saved/restored (a probe leaves no trace) and seeded to
    // known values (full 20-heart health / full magic / 300 rupees) so the
    // RESULT deltas are deterministic. Bypasses Twitch_ModifyDamage's module
    // guard on purpose: the death check itself stays in player.c where it
    // belongs - "hp=X->0" here means "this hit would kill through the
    // engine's own game-over path".
    int d = TwitchNum(arg, 8);
    if (d < 0) d = 0;
    if (d > 0xff) d = 0xff;
    int hp0 = link_health_current, mp0 = link_magic_power, rp0 = link_rupees_actual;
    link_health_current = 0xa0;
    link_magic_power = 0x80;
    link_rupees_actual = 300;
    uint8 nd = (uint8)(link_health_current - d);
    TwitchModifyDamageCore(&nd);
    if (g_twc.debug)
      printf("RESULT verb=modprobe dmg=%d hp=%d->%d magic=%d->%d rupees=%d->%d stolen_total=%d\n",
             d, 0xa0, nd, 0x80, link_magic_power, 300, link_rupees_actual,
             g_tw_rupees_stolen);
    link_health_current = (uint8)hp0;
    link_magic_power = (uint8)mp0;
    link_rupees_actual = (uint16)rp0;
  } else if (!strcmp(verb, "bossstart")) {
    // harness/test-only force start (test=1, like setcap - owner decision
    // #2): skips the rate + cooldown conditions but NOT the kill switch,
    // vs_mode or the one-fight-at-a-time rule
    BossStart("manual");
  } else if (!strcmp(verb, "bossset")) {
    // harness-only (test=1): force the boss HP, or with a negative arg
    // force the window (bossset|-5 -> DEFEAT fires ~5 frames later).
    // Authoritative: also clears pending pulse damage.
    int n = TwitchNum(arg, 0);
    if (g_boss.state != kBossActive) {
      if (g_twc.debug)
        printf("RESULT verb=bossset DROPPED reason=not_active\n");
    } else if (n < 0) {
      g_boss.window = -n;
      if (g_twc.debug)
        printf("RESULT verb=bossset window=%d\n", g_boss.window);
    } else {
      if (n > g_boss.maxhp)
        n = g_boss.maxhp;
      g_boss.hp = n;
      g_boss.pending = 0;
      if (g_twc.debug)
        printf("RESULT verb=bossset hp=%d/%d\n", g_boss.hp, g_boss.maxhp);
      BossFeedActive();
    }
  } else if (!strcmp(verb, "bossrate")) {
    // harness-only (test=1): inject N synthetic accepted-command timestamps
    // into the trailing-rate ring, then print the auto-trigger DECISION -
    // unit-checks the trigger math without real chat traffic.
    int n = TwitchNum(arg, 0);
    if (n < 0)
      n = 0;
    if (n > TW_RATE_CAP)
      n = TW_RATE_CAP;
    for (int i = 0; i < n; i++)
      BossRatePush(g_tw_frame_ctr);
    int rate = BossTrailingRate(g_tw_frame_ctr);
    bool would = rate >= g_twc.boss_min_rate && BossCooldownReady(g_tw_frame_ctr) &&
                 !g_twc.vs_mode && (TwitchInGameplay() || g_twc.test_mode);
    if (g_twc.debug)
      printf("RESULT verb=bossrate rate=%d threshold=%d cooldown_ok=%d "
             "gameplay=%d would_start=%d\n",
             rate, g_twc.boss_min_rate,
             BossCooldownReady(g_tw_frame_ctr) ? 1 : 0,
             TwitchInGameplay() ? 1 : 0, would ? 1 : 0);
  } else {
    printf("[twitch] unhandled verb %s\n", verb);
    if (g_twc.debug) printf("RESULT verb=%s DROPPED reason=unhandled\n", verb);
  }
}

static bool TwitchIsVerb(const char *v) {
  static const char *const kVerbs[] = {
    "heal", "hurt", "mp", "drain", "refill", "rupees", "tax", "bombs", "arrows",
    "smite", "clearscreen", "spawn", "swarm", "confuse", "flip", "party",
    "freeze", "stun", "curse", "root", "steal", "rob", "thief", "pickpocket",
    "speed", "fast", "slow", "ice", "icefloor", "illusion", "bunny", "fairy",
    "deny", "denyboots", "arise", "chicken", "cucco", "bosshp",
    "givekey", "bet", "points",
    // rocket league quick chat box (src/quickchat.c): cosmetic, no gate
    "qc", "whatasave", "gg",
    // music folder (src/music_player.c): cosmetic, no gate
    "nowplaying", "skipsong",
  };
  for (size_t i = 0; i < sizeof(kVerbs) / sizeof(kVerbs[0]); i++)
    if (!strcmp(v, kVerbs[i]))
      return true;
  // persistent modes (see the modes table above); alias resolved by lookup
  if (TwitchModeLookup(v))
    return true;
  // "hardmods" group toggle: drives the modifier rows of the table but is
  // not a row itself, so it needs its own admission
  if (!strcmp(v, "hardmods"))
    return true;
  // test scaffold: capacity seeding for the attrition harness, synthetic
  // hits through the modifier fan-out, and the CHAT BOSS controls
  // (force start / force HP+window / fake the rate counter), never live
  // (unknown verbs in production - owner decision #2 keeps !bossstart
  // gated exactly like setcap)
  if (g_twc.test_mode &&
      (!strcmp(v, "setcap") || !strcmp(v, "modprobe") ||
       !strcmp(v, "bossstart") || !strcmp(v, "bossset") || !strcmp(v, "bossrate")))
    return true;
  // countdown: debug-gated big on-screen 3-2-1 for human-gated recording
  // sessions (the handler double-gates on g_twc.debug)
  if (g_twc.debug && !strcmp(v, "countdown"))
    return true;
  return false;
}

// ------------------------------------------------------------------- tick --

static void TwitchTickEffects(void) {
  for (int i = kFxCount - 1; i >= 0; i--) {
    TwEffect *e = &g_tw_fx[i];
    if (i == kFxConfuse) {
      continue;                          // expire via room transitions, not time
    }
    if (e->frames <= 0)
      continue;
    e->frames--;
    if (e->frames > 0) {
      switch (i) {
      case kFxHeadFreeze:                  // reapply: catches mid-freeze spawns
        if (TwitchInGameplay())
          for (int j = 0; j < 16; j++)
            if (sprite_state[j] != 0)
              sprite_pause[j] = 1;
        break;
      case kFxCurse:                       // -1 heart every 30f, never lethal
        if (++e->tick >= 30 && TwitchInGameplay()) {
          e->tick = 0;
          if (link_health_current > 8)
            link_health_current -= 8;
        }
        break;
      case kFxRoot:
        if (TwitchInGameplay())
          link_incapacitated_timer = 4;
        break;
      case kFxSpeed:
        // reapply each frame; never fight dash/swim/hookshot handlers
        if (TwitchInGameplay() && !link_is_running &&
            link_player_handler_state != 4 &&   // swimming
            link_player_handler_state != 17 &&  // start dash
            link_player_handler_state != 18 &&  // stop dash
            link_player_handler_state != 19)    // hookshot drag
          link_speed_setting = 16;              // pegasus-tier speed
        break;
      case kFxSlow:
        if (TwitchInGameplay() && !link_is_running &&
            link_player_handler_state != 4 && link_player_handler_state != 17 &&
            link_player_handler_state != 18 && link_player_handler_state != 19)
          link_speed_setting = 8;               // exactly half speed
        break;
      case kFxIce: {
        // Whole-screen ice. Forcing link_flag_moving alone only routes
        // PlayerHandler_00_Ground_3 into Link_HandleSwimMovements; coasting
        // needs swimcoll_var7[] (the accumulator HandleSwimStroke integrates
        // and drains by -6/frame on release). On non-ice tiles
        // TileDetect_MainHandler -> Link_ResetSwimmingState wipes var7 at the
        // end of EVERY frame, so the engine alone never accumulates speed
        // here (and a zeroed var7 on input release = dead stop). So we mirror
        // what real ice does: 0x180 (the exact value TileDetect seeds on an
        // ice->slide transition, == swimcoll_var9 cap) while a direction is
        // held, then -6/frame (kSwimmingTab4[5], flag=1 friction) after
        // release. joypad1H_last is what this frame's logic will read, so the
        // seed is always consistent with the engine's own input view. Axis
        // split as in Link_SetMomentum: mask 0xC (Up/Down) -> var7[0],
        // mask 0x3 (Left/Right) -> var7[1].
        static const uint8 kIceAxisMask[2] = { 0xC, 0x3 };
        if (TwitchInGameplay() && !link_is_running && !link_is_in_deep_water &&
            (link_player_handler_state == 0 || link_player_handler_state == 23 ||
             link_player_handler_state == 28)) {
          link_flag_moving = 1;
          if (!g_twc.ice_legacy) {
            uint8 joy = joypad1H_last & kJoypadH_AnyDir;
            for (int a = 0; a < 2; a++) {
              if (joy & kIceAxisMask[a]) {
                g_tw_ice_vel[a] = 0x180;
              } else if (g_tw_ice_vel[a] > 6) {
                g_tw_ice_vel[a] -= 6;           // engine's ice friction step
              } else {
                g_tw_ice_vel[a] = 0;
              }
              if (g_tw_ice_vel[a] != 0)
                swimcoll_var7[a] = g_tw_ice_vel[a];
            }
          }
        } else {
          g_tw_ice_vel[0] = g_tw_ice_vel[1] = 0;
        }
        break;
      }
      case kFxCucco:                       // cucco rides Link, pinned every frame
        if (e->held < 0 || sprite_state[e->held] == 0) {
          e->frames = 0;                   // it died - transform ends early
          if (g_twc.debug) printf("RESULT verb=cucco effect=END reason=destroyed\n");
        } else if (TwitchInGameplay()) {
          sprite_pause[e->held] = 1;
          Sprite_SetX(e->held, link_x_coord);
          Sprite_SetY(e->held, link_y_coord);
          sprite_floor[e->held] = link_is_on_lower_level;
        }
        break;
      }
    } else {
      switch (i) {
      case kFxHeadFreeze:
        memset(sprite_pause, 0, 16);
        break;
      case kFxRoot:
        if (TwitchInGameplay())
          link_incapacitated_timer = 0;
        break;
      case kFxSteal:                       // re-equip only if not re-obtained
        if (e->saved_addr && *e->saved_addr == 0)
          *e->saved_addr = e->saved_val;
        e->saved_addr = 0;
        break;
      case kFxSpeed:
      case kFxSlow:
        if (link_is_running)
          e->frames = 10;                  // dash owns the field; retry shortly
        else
          link_speed_setting = e->saved_val;
        break;
      case kFxIce:
        link_flag_moving = 0;                // normal ground path wipes var7 itself
        g_tw_ice_vel[0] = g_tw_ice_vel[1] = 0;
        break;
      case kFxCucco:                       // release the cucco to the engine
        if (e->held >= 0 && sprite_state[e->held] != 0)
          sprite_pause[e->held] = 0;       // it wanders off; attack it if you dare
        e->held = -1;
        break;
      }
      if (g_twc.debug)
        printf("RESULT verb=%s effect=END\n", kFxNames[i]);
    }
  }
}

// --------------------------------------------------- drop-folder polling --
// Fake chat input for automated testing (ZALiA twitch_drop concept): each
// file "verb|arg|who|dur" in twitch_drop/ is one command; dur is FRAMES.
// Files bypass the IRC cooldown by design (test traffic must not be
// throttled). Delete-after-read regardless of parse result; a file that
// fails to open is left for the next frame (half-write tolerance).

#define TW_DROP_CAP 16
#ifdef _WIN32
#define TW_PATHSEP '\\'
#else
#define TW_PATHSEP '/'
#endif

static void TwitchPollDrop(void) {
  if (!g_twc.drop_enabled)
    return;
  char names[TW_DROP_CAP][64];
  int n = 0;
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA("twitch_drop\\*.txt", &fd);
  if (h == INVALID_HANDLE_VALUE)
    return;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && n < TW_DROP_CAP)
      snprintf(names[n], sizeof(names[n]), "%s", fd.cFileName), n++;
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR *d = opendir("twitch_drop");
  if (!d)
    return;
  struct dirent *de;
  while (n < TW_DROP_CAP && (de = readdir(d)) != NULL) {
    const char *x = strrchr(de->d_name, '.');
    if (x && !strcmp(x, ".txt"))
      snprintf(names[n], sizeof(names[n]), "%s", de->d_name), n++;
  }
  closedir(d);
#endif

  for (int i = 0; i < n; i++) {
    char full[300];
    snprintf(full, sizeof(full), "twitch_drop%c%s", TW_PATHSEP, names[i]);
    FILE *f = fopen(full, "r");
    if (!f)
      continue;                          // locked/half-written: retry next frame
    char line[512] = "";
    bool have = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (remove(full) != 0)
      continue;                          // undeletable right now: retry
    if (!have)
      continue;                          // empty file: deleted, done
    line[strcspn(line, "\r\n")] = 0;

    // verb|arg|who|dur  (positional; missing fields default; extras ignored).
    // Plain strtok is fine: this runs on the main thread only.
    char verb[24] = "", arg[64] = "", who[64] = "drop";
    int dur = 0;
    {
      // manual split: strtok would collapse "||" and break positional fields
      char tmp[512];
      snprintf(tmp, sizeof(tmp), "%s", line);
      char *tok[4] = { 0 };
      int ti = 0;
      char *cur = tmp;
      while (ti < 4) {
        tok[ti++] = cur;
        char *bar = strchr(cur, '|');
        if (!bar)
          break;
        *bar = 0;
        cur = bar + 1;
      }
      snprintf(verb, sizeof(verb), "%s", tok[0] ? tok[0] : "");
      snprintf(arg, sizeof(arg), "%s", tok[1] ? tok[1] : "");
      snprintf(who, sizeof(who), "%s", tok[2] && tok[2][0] ? tok[2] : "drop");
      dur = tok[3] ? atoi(tok[3]) : 0;
    }
    if (dur < 0)
      dur = 0;                           // garbage dur -> default length
    for (char *q = verb; *q; q++) *q = (char)tolower((unsigned char)*q);
    // "msg N" (show any in-game message) is a drop-file/harness verb only:
    // never admitted from chat, so it needs no place in TwitchIsVerb
    if (!verb[0] || !(TwitchIsVerb(verb) || !strcmp(verb, "msg"))) {
      if (g_twc.debug) printf("RESULT verb=%s DROPPED reason=unknown\n", verb);
      continue;
    }
    g_tw_drop_source = true;             // drop traffic is exempt from the
    TwitchApply(verb, arg, who, dur);    // per-viewer limits (harness / rig /
    g_tw_drop_source = false;            // phase-2 machine-paced volleys)
  }
}

// Chat senders seen by the network thread, handed to the main thread once
// per frame (the dialogue name pool and chatters.txt are main-thread only).
#define TW_SEEN_RING 32
static char g_tw_seen[TW_SEEN_RING][32];
static int g_tw_seen_n;
static void TwitchSeenPush(const char *who) {
  if (!g_tw_mutex) return;
  SDL_LockMutex(g_tw_mutex);
  if (g_tw_seen_n < TW_SEEN_RING)
    snprintf(g_tw_seen[g_tw_seen_n++], 32, "%s", who);
  SDL_UnlockMutex(g_tw_mutex);
}
static void TwitchSeenDrain(void) {
  char batch[TW_SEEN_RING][32];
  int n = 0;
  if (!g_tw_mutex) return;
  SDL_LockMutex(g_tw_mutex);
  n = g_tw_seen_n;
  memcpy(batch, g_tw_seen, sizeof(batch));
  g_tw_seen_n = 0;
  SDL_UnlockMutex(g_tw_mutex);
  for (int i = 0; i < n; i++) {
    TwitchNoteRecent(batch[i]);
    PointsAdd(batch[i], 1);            // viewer points: 1 per chat message
  }
}

void Twitch_Tick(void) {
  g_tw_frame_ctr++;
  QuickChat_Tick();      // rocket league quick chat: age lines, poll triggers
  if (g_tw_pts_dirty && ++g_tw_pts_flush >= 1800) PointsSave();   // viewer points, every 30 s when changed
  TwitchSeenDrain();
  TwitchPollDrop();
  // Flush internal feed events handed over by the IRC thread BEFORE draining
  // chat commands, so a connect tagline lands in the overlay in order.
  // Tracker_LogActivity is main-thread-only (unsynchronized statics); this
  // is its only net-thread-originated caller path.
  for (;;) {
    char who[64], text[128];
    bool have = false;
    SDL_LockMutex(g_tw_mutex);
    if (g_tw_ev_tail != g_tw_ev_head) {
      snprintf(who, sizeof(who), "%s", g_tw_ev_who[g_tw_ev_tail]);
      snprintf(text, sizeof(text), "%s", g_tw_ev_text[g_tw_ev_tail]);
      g_tw_ev_tail = (g_tw_ev_tail + 1) % TW_EVENT_CAP;
      have = true;
    }
    SDL_UnlockMutex(g_tw_mutex);
    if (!have)
      break;
    Tracker_LogActivity(who, text);
  }
  // drain up to 8 queued commands
  for (int k = 0; k < 8; k++) {
    TwCommand cmd;
    bool have = false;
    SDL_LockMutex(g_tw_mutex);
    if (g_tw_qtail != g_tw_qhead) {
      cmd = g_tw_queue[g_tw_qtail];
      g_tw_qtail = (g_tw_qtail + 1) % TW_QUEUE_SIZE;
      have = true;
    }
    SDL_UnlockMutex(g_tw_mutex);
    if (!have)
      break;
    // The quick chat verbs are pure overlay text: they must not be eaten by
    // the global command cooldown, and must not consume it either (chat
    // spamming "!gg" should never block someone's "!heal").
    bool cosmetic = !strcmp(cmd.verb, "qc") || !strcmp(cmd.verb, "whatasave") ||
                    !strcmp(cmd.verb, "gg") || !strcmp(cmd.verb, "nowplaying") ||
                    !strcmp(cmd.verb, "skipsong");
    if (!cmd.external && !cosmetic &&
        g_tw_frame_ctr - g_tw_last_cmd_frame < g_twc.cooldown_frames) {
      printf("[twitch] %s -> %s dropped (cooldown)\n", cmd.who, cmd.verb);
      if (g_twc.debug) printf("RESULT verb=%s DROPPED reason=cooldown\n", cmd.verb);
      continue;
    }
    if (!cmd.external) {
      if (!cosmetic)
        g_tw_last_cmd_frame = g_tw_frame_ctr;
      TwitchNoteRecent(cmd.who);
    }
    // external (channel points): the viewer already paid; no global cooldown,
    // no per-viewer limits (TwitchLimitsApply checks g_tw_external_source).
    g_tw_external_source = cmd.external;
    TwitchApply(cmd.verb, cmd.arg, cmd.who, 0);
    g_tw_external_source = false;
  }
  // room-transition detection: on the first stable gameplay frame after a
  // transition, the room/screen key changes; counts one "screen" for confuse.
  // ONLY gameplay frames are sampled: transitions themselves are non-gameplay
  // frames, and resyncing on them would re-arm the tracker on arrival so
  // confuse could never expire (found by the gameplay rig in ch6 testing).
  if (TwitchInGameplay()) {
    int key = (main_module_index == 7) ? dungeon_room_index
                                       : (overworld_screen_index & 0xff);
    if (key != g_tw_last_room) {
      if (g_tw_last_room != -1 && g_tw_fx[kFxConfuse].screens > 0 &&
          --g_tw_fx[kFxConfuse].screens <= 0) {
        g_tw_fx[kFxConfuse].screens = 0;
        if (g_twc.debug)
          printf("RESULT verb=confuse effect=END\n");
      }
      g_tw_last_room = key;
    }
  }

  TwitchBossTick();  // CHAT BOSS v1 state machine: runs before the effect
                     // tick so an attack fired this frame counts down like
                     // any other timed effect
  TwitchTickEffects();
  // position telemetry for the automated rig (tools/gameplay_verify/):
  // full Link state snapshot every 2nd gameplay frame so the rig can act
  // on live state (room awareness, grace/invuln gating, blocked-walk
  // detection) instead of verifying outcomes after the fact.  f= is
  // frame_ctr_dbg - the monotonic frame_id (frame_counter itself is uint8
  // and wraps).  spd= is link_speed_setting: variables.h has no
  // link_moving_speed.
  if (g_twc.debug && TwitchInGameplay() && (g_tw_frame_ctr % 2) == 0) {
    printf("RESULT probe f=%d x=%d y=%d r=%d scr=%d mod=%d sub=%d "
           "vx=%d vy=%d dir=%d anim=%d inv=%d st=%d "
           "vz=%d aux=%d cap=%d mv=%d spd=%d dng=%d\n",
           frame_ctr_dbg, link_x_coord, link_y_coord,
           dungeon_room_index, overworld_screen_index,
           main_module_index, submodule_index,
           link_actual_vel_x, link_actual_vel_y,
           link_direction, link_animation_steps, link_incapacitated_timer,
           link_player_handler_state, link_actual_vel_z,
           link_auxiliary_state, link_cape_mode, link_flag_moving,
           link_speed_setting,
           // live chest-opened mask for the current room (save_dung_info
           // low byte carries the per-chest opened bits) - lets the rig
           // verify a chest open from telemetry alone, no save flush
           dungeon_room_index >= 0 &&
               dungeon_room_index < 0x200
               ? (int)save_dung_info[dungeon_room_index]
               : -1);
    // sprite slot dump for the rig's spawn diff: the 16 engine slots,
    // active ones only, every 10th frame (rolling on-demand dump).  x/y
    // attribute each slot to its origin: rig spawns sit at
    // link_x+-(24+16*slot), engine/room spawns at their own nodes.
    if ((g_tw_frame_ctr % 10) == 0) {
      int n_active = 0;
      for (int j = 0; j < 16; j++)
        if (sprite_state[j] != 0)
          n_active++;
      printf("RESULT sprites f=%d n=%d", frame_ctr_dbg, n_active);
      for (int j = 0; j < 16; j++) {
        if (sprite_state[j] == 0)
          continue;
        printf(" %d:t=%d,g=%d,s=%d,x=%d,y=%d", j, sprite_type[j],
               sprite_graphics[j], sprite_state[j],
               Sprite_GetX(j), Sprite_GetY(j));
      }
      if (alt_sprites_flag != 0) {
        printf(" alt=");
        for (int j = 0; j < 16; j++)
          printf("%d:%d,%d,%d;", j, alt_sprite_state[j], alt_sprite_type[j],
                 alt_sprite_graphics[j]);
      }
      printf("\n");
    }
  }
  fflush(stdout);  // redirected stdout is block-buffered; harness reads the log live
}

// Gates queried by player.c: block the equipped Y-item (bottles and
// remapped buttons included) or the Pegasus dash while active.
int TwitchDenyYItem(void) {
  return FxActive(kFxDenyY) ? 1 : 0;
}
int TwitchDenyBoots(void) {
  return FxActive(kFxDenyBoots) ? 1 : 0;
}

int Twitch_PreFrame(int inputs) {
  if (FxActive(kFxConfuse)) {
    // raw inputs are in SNES serial order: bit6=Left, bit7=Right (the
    // kJoypadH_* constants are the post-bit-reversal view, NOT this layout)
    int l = inputs & 0x40, r = inputs & 0x80;
    inputs = (inputs & ~0xc0) | (l << 1) | (r >> 1);
  }
  return inputs;
}

// Big center-screen countdown digit for the debug "countdown" verb: a 5x7
// digit scaled x8, drawn while g_tw_countdown_frames ticks down.  Drawn
// BEFORE the flip effect so the countdown stays readable during !flip.
static const uint8 kTwDigits[10][7] = {
  { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },  // 0
  { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },  // 1
  { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },  // 2
  { 0x1F, 0x10, 0x10, 0x1C, 0x10, 0x10, 0x1F },  // 3 (bit0 = LEFT column)
  { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },  // 4
  { 0x1F, 0x01, 0x1F, 0x10, 0x10, 0x11, 0x0E },  // 5
  { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },  // 6
  { 0x1F, 0x10, 0x08, 0x04, 0x04, 0x04, 0x04 },  // 7
  { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },  // 8
  { 0x0E, 0x11, 0x11, 0x0F, 0x10, 0x10, 0x0C },  // 9
};

static void TwitchDrawCountdown(uint8 *pixels, int pitch, int width,
                                int height, int n) {
  const int sc = 8, gw = 5 * sc, gap = 2 * sc;
  char buf[16];
  int nd = snprintf(buf, sizeof(buf), "%d", n);
  if (nd < 1)
    return;
  int total_w = nd * gw + (nd - 1) * gap;
  int x0 = width / 2 - total_w / 2;
  int y0 = height / 2 - (7 * sc) / 2;
  for (int d = 0; d < nd; d++) {
    const uint8 *g = kTwDigits[buf[d] - '0'];
    for (int gy = 0; gy < 7; gy++) {
      for (int yy = 0; yy < sc; yy++) {
        int py = y0 + gy * sc + yy;
        if (py < 0 || py >= height)
          continue;
        uint32 *row = (uint32 *)(pixels + (size_t)py * pitch);
        uint8 bits = g[gy];
        for (int gx = 0; gx < 5; gx++) {
          if (!(bits & (1 << gx)))
            continue;
          for (int xx = 0; xx < sc; xx++) {
            int px = x0 + d * (gw + gap) + gx * sc + xx;
            if (px >= 0 && px < width)
              row[px] = 0xFFFFFFFFu;
          }
        }
      }
    }
  }
}

void Twitch_PostDraw(uint8 *pixels, int pitch, int width, int height) {
  if (g_tw_countdown_frames > 0) {
    --g_tw_countdown_frames;
    TwitchDrawCountdown(pixels, pitch, width, height,
                        g_tw_countdown_frames / 60 + 1);
  }
  if (FxActive(kFxFlip)) {
    for (int y = 0; y < height; y++) {
      uint8 *row = pixels + (size_t)y * pitch;
      for (int x = 0; x < width / 2; x++) {
        uint8 *a = row + (size_t)x * 4;
        uint8 *b = row + (size_t)(width - 1 - x) * 4;
        for (int c = 0; c < 4; c++) {
          uint8 t = a[c];
          a[c] = b[c];
          b[c] = t;
        }
      }
    }
  }
  // Quick chat goes on LAST, after the flip mirror: text drawn before it
  // would come out backwards for the whole !flip effect.
  QuickChat_PostDraw(pixels, pitch, width, height);
}

// ----------------------------------------------------------------- oauth --

// Boolean getters for the settings-menu status row and the /twitch/status
// route. The token value itself NEVER leaves this file.
bool Twitch_HasToken(void) {
  bool has;
  if (g_tw_mutex)
    SDL_LockMutex(g_tw_mutex);
  has = g_twc.token[0] != 0;
  if (g_tw_mutex)
    SDL_UnlockMutex(g_tw_mutex);
  return has;
}

bool Twitch_IsConnected(void) { return g_tw_connected; }
bool Twitch_TestMode(void) { return g_twc.test_mode; }
bool Twitch_Debug(void) { return g_twc.debug; }

bool Twitch_CopyToken(char *out, size_t cap) {
  if (!g_twc.token[0]) return false;
  snprintf(out, cap, "%s", g_twc.token);
  return true;
}
const char *Twitch_ClientId(void) { return g_twc.client_id; }
const char *Twitch_CpApiUrl(void) { return g_twc.cp_api[0] ? g_twc.cp_api : "https://api.twitch.tv"; }
const char *Twitch_CpWsUrl(void) { return g_twc.cp_ws[0] ? g_twc.cp_ws : "wss://eventsub.wss.twitch.tv/ws"; }

// Recent chatters (main thread): every drained chat command stamps its
// sender here; the dialogue override's {chatter} token picks one at random
// so in-game NPC lines name whoever is actually talking.
#define TW_RECENT_RING 16
static char g_tw_recent[TW_RECENT_RING][32];
static int g_tw_recent_n, g_tw_recent_head;
static void TwitchNoteRecent(const char *who) {
  if (!who || !who[0] || !strcmp(who, "rig") || !strcmp(who, "external") || !strcmp(who, "drop")) return;
  // the streamer and the game's own account count too (owner 09-12: "we can
  // be jokes"); chatters.txt's staff: line keeps them off the top of the list
  DialogueOverride_NoteChatter(who);   // grows the {chatter} pool + chatters.txt (dedupes itself)
  for (int i = 0; i < g_tw_recent_n; i++)
    if (!strcmp(g_tw_recent[i], who)) return;
  snprintf(g_tw_recent[g_tw_recent_head], 32, "%s", who);
  g_tw_recent_head = (g_tw_recent_head + 1) % TW_RECENT_RING;
  if (g_tw_recent_n < TW_RECENT_RING) g_tw_recent_n++;
}
// The current (or most recent) CHAT BOSS name, for the dialogue {boss}
// token: the wizard in the story is whoever chat is fighting right now.
bool Twitch_BossName(char *out, size_t cap) {
  if (!g_boss.name[0]) return false;
  snprintf(out, cap, "%s", g_boss.name);
  return true;
}
bool Twitch_RandomChatter(char *out, size_t cap) {
  if (g_tw_recent_n == 0) return false;
  snprintf(out, cap, "%s", g_tw_recent[(int)(SDL_GetTicks() / 7 % (uint32)g_tw_recent_n)]);
  return true;
}

void Twitch_EnqueueExternal(const char *verb, const char *arg, const char *who) {
  TwCommand c;
  memset(&c, 0, sizeof(c));
  snprintf(c.verb, sizeof(c.verb), "%s", verb ? verb : "");
  snprintf(c.arg, sizeof(c.arg), "%s", arg ? arg : "");
  snprintf(c.who, sizeof(c.who), "%s", who ? who : "external");
  c.external = true;
  if (!g_tw_mutex) return;
  TwitchQueuePush(&c);
}

// Main thread only (called from main.c's OAuthWeb_TakeSavedToken poll).
// Never prints the token. Either starts the IRC thread for the first time
// (config had no token at boot) or nudges the running thread's backoff sleep
// so it reconnects immediately; TwitchSocketRun re-reads the token under the
// mutex, so the new credential is on the wire within ~100ms.
void Twitch_UpdateToken(const char *bare_token) {
  if (!bare_token || !bare_token[0])
    return;
  if (g_tw_mutex)
    SDL_LockMutex(g_tw_mutex);
  snprintf(g_twc.token, sizeof(g_twc.token), "%s", bare_token);
  bool can_start = g_twc.user[0] != 0 && g_twc.channel[0] != 0;
  if (g_tw_mutex)
    SDL_UnlockMutex(g_tw_mutex);
  printf("[twitch] new chat token installed from the oauth flow\n");
  if (g_tw_thread) {
    g_tw_backoff_s = 5;                  // snappy: fresh credential on the dial
    g_tw_reconnect_now = true;
  } else if (can_start) {
    g_tw_running = true;
    g_tw_shutdown = false;
    g_tw_thread = SDL_CreateThread(TwitchThreadFunc, "twitch_irc", NULL);
    printf("[twitch] network thread started\n");
  }
  fflush(stdout);
  CP_TokenChanged();   // channel points re-bootstraps with the new token
}

// ----------------------------------------------------------------- module --

void Twitch_Init(void) {
  PointsLoad();
  TwitchLoadConfig();
  Scoreboard_Init();  // load persisted deaths/commands before anything counts
  // In-game OAuth listener (loopback-only; settings-menu rows register inside
  // OAuthWeb_Start). Config strings must be in before the thread starts.
  OAuthWeb_SetClientId(g_twc.client_id);
  OAuthWeb_SetRedirectUri(g_twc.oauth_redirect);
  OAuthWeb_Start();
#ifdef _WIN32
  _mkdir("twitch_drop");
#else
  mkdir("twitch_drop", 0755);
#endif
  g_tw_rng = (uint32)SDL_GetTicks() | 1;
  memset(g_tw_fx, 0, sizeof(g_tw_fx));
  memset(&g_boss, 0, sizeof(g_boss));    // CHAT BOSS v1: DORMANT, no fight;
  g_boss.last_end = -1000000;            // cooldown ready from boot
  g_tw_last_room = -1;
  g_tw_mutex = SDL_CreateMutex();
  g_tw_running = false;
  g_tw_shutdown = false;
  g_tw_thread = NULL;
  if (g_twc.token[0] && g_twc.user[0] && g_twc.channel[0]) {
    g_tw_running = true;
    g_tw_thread = SDL_CreateThread(TwitchThreadFunc, "twitch_irc", NULL);
    printf("[twitch] network thread started\n");
  }
}

void Twitch_Shutdown(void) {
  if (g_tw_pts_dirty) PointsSave();
  OAuthWeb_Stop();   // join the listener first: it queries Twitch_HasToken()
  if (g_tw_thread) {
    g_tw_running = false;
    // Wake a blocked recv; a blocking connect()/DNS lookup can't be
    // interrupted, so DON'T wait for the thread - detach and let it exit
    // (and close its own socket) on its own schedule. It never touches game
    // state, so racing the process teardown is safe.
    //
    // The old code read g_tw_sock UNLOCKED here, racing the net thread's
    // close/reopen of the same variable: a reconnect in flight could hand us
    // INVALID_SOCKET or a brand-new socket to kill. Raise g_tw_shutdown and
    // snapshot + shutdown() the handle under the mutex instead - the net
    // thread checks the flag (and swaps the variable) under that same lock,
    // so it can never publish a fresh socket after this point.
    SOCKET s;
    if (g_tw_mutex)
      SDL_LockMutex(g_tw_mutex);
    g_tw_shutdown = true;
    s = g_tw_sock;
    if (s != INVALID_SOCKET) {
#ifdef _WIN32
      shutdown(s, SD_BOTH);
#else
      shutdown(s, SHUT_RDWR);
#endif
    }
    if (g_tw_mutex)
      SDL_UnlockMutex(g_tw_mutex);
#ifdef _WIN32
    // Balance the single WSAStartup from TwitchEnsureWsa (the refcount used
    // to grow once per connect attempt, forever). Safe with the detached
    // thread still winding down: the shutdown() above already broke the
    // blocked recv, and the process is exiting by definition here.
    if (g_tw_wsa_up) {
      WSACleanup();
      g_tw_wsa_up = false;
    }
#endif
    SDL_DetachThread(g_tw_thread);
    g_tw_thread = NULL;
    // mutex intentionally not destroyed: the detached thread may still push
    if (g_tw_mutex)
      g_tw_mutex = NULL;
  } else if (g_tw_sock != INVALID_SOCKET) {
    tw_closesocket(g_tw_sock);
    g_tw_sock = INVALID_SOCKET;
  }
}
