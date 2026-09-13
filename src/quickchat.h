#ifndef QUICKCHAT_H
#define QUICKCHAT_H

#include "types.h"

// Rocket League style QUICK CHAT overlay (src/quickchat.c).
//
// A little four line chat box in the top-left of the game picture (just under
// the status bar, see the LAYOUT note in quickchat.c) that fills with RL
// quick chats and chat's own washed-gold-rank memes whenever
// something happens: a one-heart survival ("What a save!"), a death ("gg ez"),
// a fairy revive, a dungeon boss going down, the CHAT BOSS fight resolving,
// a big item, and an idle meme every couple of minutes.
//
// Everything is cosmetic: this module only READS game variables and paints on
// the ARGB frame buffer after the PPU. It never writes game state.

// Boot: loads the optional quickchat.txt (extra meme lines). Call once from
// main() next to DialogueOverride_Init().
void QuickChat_Init(void);

// Once per frame from Twitch_Tick (main thread): ages the visible lines,
// releases queued lines at the spam rate, and polls the event triggers.
void QuickChat_Tick(void);

// Paint the box. Call at the END of Twitch_PostDraw, i.e. AFTER the !flip
// mirror, so the text is never drawn backwards.
void QuickChat_PostDraw(uint8 *pixels, int pitch, int width, int height);

// --- pushes from twitch.c ---------------------------------------------------
// !qc <text>: one line under the sender's own name (already sanitized here).
void QuickChat_ChatLine(const char *who, const char *text);
// !whatasave / !gg: the two chat-callable bursts.
void QuickChat_BurstWhatASave(void);
void QuickChat_BurstGG(void);
// CHAT BOSS resolution (twitch.c BossVictory / BossDefeat).
void QuickChat_BossWin(void);
void QuickChat_BossLoss(void);

#endif
