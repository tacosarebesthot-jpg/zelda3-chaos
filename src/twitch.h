#ifndef TWITCH_H
#define TWITCH_H

#include "types.h"

// Twitch chat integration - concept port of the ZALiA (zelda 2) GML design.
// A background thread holds the raw IRC socket (irc.chat.twitch.tv:6667) and
// queues parsed commands; the main thread drains the queue once per frame from
// ZeldaRunFrame and applies reversible, timed effects. Nothing touches SRAM.

// Call after ParseConfigFile() in main(): loads twitch_config.txt and starts
// the network thread (connecting only if token/user/channel are configured).
void Twitch_Init(void);

// Call at end of main(): stops the thread and closes the socket.
void Twitch_Shutdown(void);

// Called at the top of ZeldaRunFrame() on the main thread. May swap left/right
// input bits while the "confuse" effect is active.
int Twitch_PreFrame(int inputs);

// Called once per frame on the main thread: drains queued commands and
// ticks/reverts active effects.
void Twitch_Tick(void);
// A dungeon chest was just opened (player.c): resolves !bet good/junk.
void Twitch_NoteChestOpened(int item);

// Called between ZeldaDrawPpuFrame() and EndDraw() in main.c: mirrors the
// framebuffer horizontally while the "flip" effect is active.
void Twitch_PostDraw(uint8 *pixels, int pitch, int width, int height);

// Gates for the "deny" verbs, queried from player.c each frame. Return 1
// while chat-issued deny effects are active (purely a runtime gate - no
// inventory state is touched).
int TwitchDenyYItem(void);
int TwitchDenyBoots(void);

// Chat "attrition" mode damage hook. Call from the single damage choke point
// (player.c Link_ControlHandler) right after link_health_current was reduced,
// and from the "hurt" verb. While the mode is on, every hit also removes one
// heart container (capacity -8, floored at 24 = 3 containers; current health
// clamps to the new capacity). No-op while the mode is off, during
// non-gameplay modules, and when capacity is 0 (title screen). Returns 1 if
// a container was lost. The loss is permanent for the session; "attrition
// off" only stops further loss.
int Twitch_AttritionDamage(void);

// --- OAuth (src/oauth_web.c) support ---------------------------------------
// Twitch_HasToken / Twitch_IsConnected feed the settings-menu status row and
// the /twitch/status route (booleans only - the token itself never leaves
// twitch.c).

bool Twitch_HasToken(void);
bool Twitch_IsConnected(void);

// twitch_config.txt test=1 (harness mode). The F10/F12 keyboard settings
// overlay is only reachable in this mode; players use the in-game pause-menu
// MODS page and the file-select rows instead.
bool Twitch_TestMode(void);
bool Twitch_Debug(void);   // RESULT lines on stdout (harness)

// --- Channel Points (src/channel_points.c) support -------------------------
bool Twitch_CopyToken(char *out, size_t cap);   // bare token, false when none
const char *Twitch_ClientId(void);
const char *Twitch_CpApiUrl(void);              // twitch_config cp_api= override
const char *Twitch_CpWsUrl(void);               // twitch_config cp_ws= override
// Queue a command from a non-chat source (channel points). Bypasses the global
// cooldown and the per-viewer limits, like drop-folder commands do.
void Twitch_EnqueueExternal(const char *verb, const char *arg, const char *who);
// A random recent chatter login (main thread); false when nobody has chatted yet.
bool Twitch_RandomChatter(char *out, size_t cap);
// Current/most recent CHAT BOSS name; false before the first boss.
bool Twitch_BossName(char *out, size_t cap);

// Install a fresh chat token (bare, no "oauth:" prefix) obtained by the
// in-game OAuth flow. Call from the MAIN thread (main.c polls
// OAuthWeb_TakeSavedToken). Updates the in-memory token, then either starts
// the IRC thread if it was never running, or nudges the running thread to cut
// its reconnect backoff and dial again immediately with the new token.
void Twitch_UpdateToken(const char *bare_token);

// Chat enemy-modifier fan-out (composable modes "dmgup" / "mpsteal" /
// "rupeesteal", group-armed by "hardmods"). Call from the same damage choke
// point (player.c Link_ControlHandler) ONE line after the raw subtraction
// `uint8 new_dmg = link_health_current - dmg;` and BEFORE new_dmg is applied
// to link_health_current / the death check runs / Twitch_AttritionDamage()
// fires - so a hit doubled into lethality by dmgup dies through the engine's
// own game-over path, and doubling always precedes attrition on the same hit
// (orthogonal: current-health loss vs max-capacity loss). Precondition:
// link_health_current still holds the PRE-hit health when this is called
// (dmgup needs it to size the hit). Flag-first: one branch total while all
// modifiers are off; same gameplay-module guard as attrition. No-op in
// production until chat arms a mode.
void Twitch_ModifyDamage(uint8 *new_dmg);

#endif
