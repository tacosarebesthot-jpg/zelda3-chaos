#ifndef CHANNEL_POINTS_H
#define CHANNEL_POINTS_H

#include "types.h"

// In-game Twitch Channel Points (EventSub websocket over WinHTTP). Replaces
// tools/channel_points/cp_listen.py so the streamer never runs a script:
// the same in-game CONNECT token that drives chat (scope
// channel:read:redemptions is requested with it) drives this.
//
//   worker thread:  Helix /users -> broadcaster id
//                   Helix /channel_points/custom_rewards -> reward list
//                   wss://eventsub.wss.twitch.tv/ws -> session_welcome
//                   POST /eventsub/subscriptions (websocket transport)
//                   notifications -> mapping -> Twitch_EnqueueExternal()
//   main thread:    the pause-menu REWARDS page reads the reward list and
//                   assigns a verb per reward (persisted in cp_rewards.ini,
//                   written by the game, never edited by hand).
//
// Rewards seen only through a redemption (list call failed, or a reward was
// created mid-stream) are added to the table as they arrive, so they can be
// mapped from the screen without a restart. Reconnects with backoff; a
// rejected token flips the status to NEED CONNECT instead of dying quietly.

void CP_Init(void);          // after Twitch_Init(); starts the worker if a token exists
void CP_Shutdown(void);
void CP_TokenChanged(void);  // twitch.c calls this after a new token is installed

enum {
  kCpOff = 0,        // no token yet / channel points disabled
  kCpConnecting,
  kCpOnline,
  kCpRetrying,       // transient failure, reconnecting with backoff
  kCpNeedConnect,    // token rejected (expired/revoked): redo CONNECT
};
int CP_Status(void);
const char *CP_StatusText(void);   // caps, <= 12 chars, for the HUD font

// Reward table + mapping (main-thread readers; the worker fills it under a lock)
int CP_RewardCount(void);
const char *CP_RewardTitle(int i);  // caps, <= 24 chars
const char *CP_RewardVerb(int i);   // "NONE" or the verb name, caps
void CP_RewardCycle(int i, int delta);  // change the mapping; persists at once

#endif
