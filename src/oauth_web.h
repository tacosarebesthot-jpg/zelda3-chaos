#ifndef OAUTH_WEB_H
#define OAUTH_WEB_H

#include "types.h"

// In-game Twitch OAuth (implicit grant) via a LOOPBACK-ONLY HTTP listener.
// Concept port of ZALiA's obj_zalia_web (Other_68.gml) + twitch_callback.html.
//
//   GET /twitch/login     -> prints the authorize URL to the console AND copies
//                            it to the Windows clipboard (owner directive: NO
//                            automatic browser open - he clicks at his leisure)
//   GET /twitch/callback  -> small inline page; its JS reads location.hash (the
//                            token NEVER reaches the server) and hands it to
//                            /twitch/save, then clears the address bar
//   GET /twitch/save      -> writes the token into twitch_config.txt's token=
//                            line ATOMICALLY (temp + MoveFileEx replace), every
//                            other byte preserved; the token is never logged
//                            or echoed anywhere
//   GET /twitch/status    -> {"has_token":true|false} and nothing else
//
// The listener binds loopback only and refuses any peer whose address is not
// 127.0.0.1 / ::1 (belt and suspenders: loopback bind + accept-time check).
// Default port 8795, redirect_uri locked (coordinator) to
//   http://localhost:8795/twitch/callback
// overridable via the twitch_config.txt key `oauth_redirect=`.

// Start/stop the listener thread. Start after config load (Twitch_Init);
// Stop before the IRC thread teardown path in Twitch_Shutdown.
void OAuthWeb_Start(void);
void OAuthWeb_Stop(void);

// Feed the URL builder from twitch.c's config (before Start).
void OAuthWeb_SetClientId(const char *client_id);
void OAuthWeb_SetRedirectUri(const char *redirect_uri);

// Print + clipboard-copy the authorize URL. Called from the F10 overlay
// (RETURN) and from GET /twitch/login. Returns false when no client_id is
// configured (nothing is copied then; console explains why).
bool OAuthWeb_CopyConnectLink(void);

// Main-thread poll: takes the token saved by /twitch/save (parked there by
// the listener thread) so twitch.c's Twitch_UpdateToken() runs on the main
// thread like every other writer of twitch state. Returns false when nothing
// was saved since the last poll.
bool OAuthWeb_TakeSavedToken(char *out, int cap);

// Overlay status getters (F10 box).
bool OAuthWeb_Listening(void);
enum {
  kOAuthSaveNone = 0,     // idle
  kOAuthSavePending,      // link handed out, waiting for the browser round-trip
  kOAuthSaveOk,           // token landed (fades back to idle after a few s)
  kOAuthSaveFailed,       // save rejected (bad token charset, file error, ...)
};
int OAuthWeb_SaveState(void);

// F10 settings overlay (drawn on the game's pixel buffer, same path as
// Twitch_PostDraw). State + rendering live here; main.c only toggles/feeds.
void OAuth_OverlayToggle(void);
bool OAuth_OverlayOpen(void);
void OAuth_PostDraw(uint8 *pixels, int pitch, int width, int height);

#endif
