// Ending credits (stream build, owner 09-12 Discord Q3 "end credits with
// community names: yes clearly"; port info + Patreon added 09-12 evening,
// no Discord mention): the staff name rows of the credits roll become a
// PORT INFO / CHAOS STREAM BUILD / PATREON SUPPORTERS section (data-driven
// from credits_port.txt) followed by the Patreon supporters (patrons.txt)
// and the top chatters of chatters.txt, when text.ini credits=1
// (OPTIONS > GAME FEATURES > CREDITS = CHAT). The dungeon death counts stay
// as they are.
#pragma once
#include "types.h"
// The replacement for credits row |r18| in the asset's own format
// (x column, tile count word, letter tiles), or NULL to keep the original.
const uint8 *Credits_CommunityRow(int r18);
