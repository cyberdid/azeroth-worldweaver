#ifndef MOD_CLAUDE_BRIDGE_WORLD_STATE_OBSERVER_H
#define MOD_CLAUDE_BRIDGE_WORLD_STATE_OBSERVER_H

#include <string>

class Player;

// Returns the last world-thread-built compact observation for the player's bridge group.
// The function only reads cached strings; it does not scan the map or touch the filesystem.
std::string GetWorldChatContext(Player const* player);

#endif
