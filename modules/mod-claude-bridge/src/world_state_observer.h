#ifndef MOD_CLAUDE_BRIDGE_WORLD_STATE_OBSERVER_H
#define MOD_CLAUDE_BRIDGE_WORLD_STATE_OBSERVER_H

#include "Define.h"

#include <string>

class Player;

// Returns the last world-thread-built compact observation for the player's bridge group.
// The function only reads cached strings; it does not scan the map or touch the filesystem.
std::string GetWorldChatContext(Player const* player);

// Record a server-validated action acknowledgement in the same event stream as world observations.
void PublishActionResultEvent(Player* requester, std::string const& actorName,
    std::string const& actionId, std::string const& actionType,
    std::string const& status, std::string const& reasonCode);

// World-thread-only publication of server-validated vendor payloads.
void PublishVendorEvent(Player* actor, std::string const& type, std::string const& payload,
    std::string const& detail);

// Validated journey owners retain the original group stream even if a member logs out.
void PublishVendorJourneyEvent(std::string const& groupId, std::string const& actorName,
    uint32 mapId, std::string const& payload, std::string const& detail);

void PublishNpcDialogEvent(Player* actor, std::string const& actionId);

#endif
