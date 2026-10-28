#include "action_dispatcher.h"

#include "Config.h"
#include "Event.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "world_state_observer.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <deque>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{
    constexpr std::size_t MAX_CACHED_ACTIONS = 512;
    constexpr auto ACTION_CACHE_LIFETIME = std::chrono::minutes(10);
    constexpr auto ACTION_RATE_LIMIT = std::chrono::seconds(2);

    struct CachedAction
    {
        ActionResult result;
        std::chrono::steady_clock::time_point recorded;
    };

    struct ActionTypeInfo
    {
        char const* type;
        char const* playerbotAction;
    };

    constexpr ActionTypeInfo ACTION_TYPES[] = {
        {"follow", "follow"},
        {"stay", "stay"},
        {"attack_master_target", "attack my target"},
        {"attack_nearby", "attack anything"},
        {"loot_nearby", "loot"},
        {"fish_here", "go fishing"},
        {"use_fishing_bobber", "use fishing bobber"},
        {"stop_fishing", "end master fishing"},
    };

    std::unordered_map<std::string, CachedAction> recentActions;
    std::deque<std::pair<std::string, std::chrono::steady_clock::time_point>> actionOrder;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastActionByActor;

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::string Trim(std::string value)
    {
        std::size_t const first = value.find_first_not_of(" \t");
        if (first == std::string::npos)
            return {};
        std::size_t const last = value.find_last_not_of(" \t");
        return value.substr(first, last - first + 1);
    }

    bool IsSafeCharacterName(std::string const& value)
    {
        if (value.empty() || value.size() > 12 || !std::isalpha(static_cast<unsigned char>(value.front())))
            return false;
        return std::all_of(value.begin(), value.end(), [](unsigned char c)
        {
            return std::isalpha(c) || std::isdigit(c);
        });
    }

    bool IsValidActionId(std::string const& value)
    {
        return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char c)
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }

    bool IsDecimalGroupId(std::string const& value)
    {
        return !value.empty() && value.size() <= 20
            && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
    }

    ActionTypeInfo const* FindActionType(std::string const& type)
    {
        auto found = std::find_if(std::begin(ACTION_TYPES), std::end(ACTION_TYPES), [&type](ActionTypeInfo const& item)
        {
            return type == item.type;
        });
        return found == std::end(ACTION_TYPES) ? nullptr : &*found;
    }

    bool IsConfiguredName(std::string const& list, std::string const& expectedName)
    {
        std::size_t start = 0;
        while (start <= list.size())
        {
            std::size_t const end = list.find(',', start);
            std::string const item = Trim(list.substr(start, end == std::string::npos
                ? std::string::npos : end - start));
            if (!item.empty() && Lower(item) == Lower(expectedName))
                return true;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return false;
    }

    bool IsBridgeBot(std::string const& name)
    {
        std::string list = sConfigMgr->GetOption<std::string>("ClaudeBridge.BotNames", "");
        if (list.empty())
            list = sConfigMgr->GetOption<std::string>("ClaudeBridge.BotName", "Claude,Codex");
        return IsConfiguredName(list, name);
    }

    bool IsAuthorizedRequester(std::string const& name)
    {
        std::string const list = sConfigMgr->GetOption<std::string>("ClaudeBridge.AuthorizedPlayers", "");
        return IsConfiguredName(list, name);
    }

    std::string JsonEscape(std::string const& value)
    {
        std::ostringstream out;
        for (unsigned char c : value)
            switch (c)
            {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (c < 0x20)
                        out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
                    else
                        out << static_cast<char>(c);
            }
        return out.str();
    }

    ActionResult MakeResult(std::string const& actionId, std::string const& actorName,
        std::string const& actionType, std::string status, std::string reasonCode)
    {
        ActionResult result;
        result.actionId = IsValidActionId(actionId) ? actionId : std::string(32, '0');
        result.status = std::move(status);
        result.reasonCode = std::move(reasonCode);
        result.actor = IsSafeCharacterName(actorName) && (Lower(actorName) == "claude" || Lower(actorName) == "codex")
            ? actorName : "Codex";
        result.type = FindActionType(actionType) ? actionType : "follow";
        return result;
    }

    void RememberResult(ActionResult const& result)
    {
        auto const now = std::chrono::steady_clock::now();
        recentActions[result.actionId] = {result, now};
        actionOrder.emplace_back(result.actionId, now);
        while (!actionOrder.empty())
        {
            auto const& oldest = actionOrder.front();
            auto found = recentActions.find(oldest.first);
            bool const expired = found == recentActions.end() || now - oldest.second > ACTION_CACHE_LIFETIME;
            bool const overLimit = recentActions.size() > MAX_CACHED_ACTIONS;
            if (!expired && !overLimit)
                break;
            if (found != recentActions.end() && found->second.recorded == oldest.second)
                recentActions.erase(found);
            actionOrder.pop_front();
        }
    }

    ActionResult Finish(Player* requester, ActionResult result, bool publish)
    {
        if (publish && requester && IsValidActionId(result.actionId))
            PublishActionResultEvent(requester, result.actor, result.actionId, result.type,
                result.status, result.reasonCode);
        if (IsValidActionId(result.actionId))
            RememberResult(result);
        return result;
    }
}

std::string ActionResult::ToJson() const
{
    return "{\"action_id\":\"" + JsonEscape(actionId)
        + "\",\"status\":\"" + JsonEscape(status)
        + "\",\"reason_code\":\"" + JsonEscape(reasonCode)
        + "\",\"actor\":\"" + JsonEscape(actor)
        + "\",\"type\":\"" + JsonEscape(type) + "\"}";
}

ActionResult DispatchTypedAction(std::string const& actorName, std::string const& requesterName,
    std::string const& groupId, std::string const& actionId, std::string const& actionType)
{
    if (!IsSafeCharacterName(actorName) || !IsSafeCharacterName(requesterName)
        || !IsValidActionId(actionId) || !IsDecimalGroupId(groupId))
        return MakeResult(actionId, actorName, actionType, "rejected", "invalid_request");

    if (auto found = recentActions.find(actionId); found != recentActions.end())
        return found->second.result;

    Player* requester = ObjectAccessor::FindPlayerByName(requesterName);
    ActionResult result = MakeResult(actionId, actorName, actionType, "rejected", "unauthorized_sender");
    if (!IsAuthorizedRequester(requesterName))
        return Finish(requester, std::move(result), false);
    if (!requester || !requester->IsInWorld())
    {
        result.reasonCode = "requester_offline";
        return Finish(nullptr, std::move(result), false);
    }
    if (!IsBridgeBot(actorName))
    {
        result.reasonCode = "actor_mismatch";
        return Finish(requester, std::move(result), true);
    }

    Player* actor = ObjectAccessor::FindPlayerByName(actorName);
    if (!actor || !actor->IsInWorld())
    {
        result.reasonCode = "actor_unavailable";
        return Finish(requester, std::move(result), true);
    }
    PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(actor);
    if (!botAI)
    {
        result.reasonCode = "actor_unavailable";
        return Finish(requester, std::move(result), true);
    }

    Group* requesterGroup = requester->GetGroup();
    Group* actorGroup = actor->GetGroup();
    if (!requesterGroup || requesterGroup != actorGroup
        || std::to_string(requesterGroup->GetLeaderGUID().GetRawValue()) != groupId)
    {
        result.reasonCode = "actor_not_in_group";
        return Finish(requester, std::move(result), true);
    }
    if (botAI->GetMaster() != requester)
    {
        result.reasonCode = "actor_not_owned_by_requester";
        return Finish(requester, std::move(result), true);
    }

    ActionTypeInfo const* actionInfo = FindActionType(actionType);
    if (!actionInfo)
    {
        result.reasonCode = "unsupported_action";
        return Finish(requester, std::move(result), false);
    }

    if (actionType == "attack_master_target")
    {
        Unit* target = requester->GetSelectedUnit();
        if (!target || !target->ToCreature() || !target->IsAlive()
            || target->GetMapId() != actor->GetMapId()
            || target->GetInstanceId() != actor->GetInstanceId()
            || !actor->InSamePhase(target)
            || !actor->IsWithinLOSInMap(target)
            || !requester->IsValidAttackTarget(target)
            || !actor->IsValidAttackTarget(target))
        {
            result.reasonCode = "invalid_target";
            return Finish(requester, std::move(result), true);
        }
    }

    std::string const rateKey = Lower(requesterName) + ":" + Lower(actorName);
    auto const now = std::chrono::steady_clock::now();
    auto previous = lastActionByActor.find(rateKey);
    if (previous != lastActionByActor.end() && now - previous->second < ACTION_RATE_LIMIT)
    {
        result.reasonCode = "rate_limited";
        return Finish(requester, std::move(result), true);
    }
    lastActionByActor[rateKey] = now;

    if (!botAI->DoSpecificAction(actionInfo->playerbotAction, Event(), true))
    {
        result.reasonCode = "action_unavailable";
        return Finish(requester, std::move(result), true);
    }

    result.status = "accepted";
    result.reasonCode = "started";
    return Finish(requester, std::move(result), true);
}
