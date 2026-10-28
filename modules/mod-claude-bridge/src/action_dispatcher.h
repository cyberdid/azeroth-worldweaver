#ifndef MOD_CLAUDE_BRIDGE_ACTION_DISPATCHER_H
#define MOD_CLAUDE_BRIDGE_ACTION_DISPATCHER_H

#include <string>

struct ActionResult
{
    std::string actionId;
    std::string status;
    std::string reasonCode;
    std::string actor;
    std::string type;

    std::string ToJson() const;
};

// Called only by the world-thread console command. All inputs are typed scalar tokens;
// model-generated free-form text is never interpreted as a server command.
ActionResult DispatchTypedAction(std::string const& actorName, std::string const& requesterName,
    std::string const& groupId, std::string const& actionId, std::string const& actionType);

#endif
