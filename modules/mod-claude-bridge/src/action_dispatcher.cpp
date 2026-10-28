#include "action_dispatcher.h"

#include "Config.h"
#include "Creature.h"
#include "Event.h"
#include "GossipDef.h"
#include "Group.h"
#include "ObjectGuid.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "MovementActions.h"
#include "SpellMgr.h"
#include "world_state_observer.h"

#include <algorithm>
#include <charconv>
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
    constexpr std::size_t MAX_PENDING_NPC_INTERACTIONS = 32;
    constexpr auto ACTION_CACHE_LIFETIME = std::chrono::minutes(10);
    constexpr auto ACTION_RATE_LIMIT = std::chrono::seconds(2);
    constexpr auto NPC_APPROACH_TIMEOUT = std::chrono::seconds(60);

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
        {"follow", "follow chat shortcut"},
        {"stay", "stay chat shortcut"},
        {"attack_master_target", "attack my target"},
        {"attack_nearby", "attack anything"},
        {"loot_nearby", "loot"},
        {"fish_here", "go fishing"},
        {"use_fishing_bobber", "use fishing bobber"},
        {"stop_fishing", "end master fishing"},
        {"ensure_online", nullptr},
        {"cast_spell", nullptr},
        {"interact_npc", nullptr},
        {"gossip_select", nullptr},
    };

    struct PendingNpcInteraction
    {
        std::string actor;
        std::string requester;
        std::string actionId;
        std::string groupId;
        uint64 targetGuid = 0;
        std::chrono::steady_clock::time_point expires;
    };

    std::unordered_map<std::string, CachedAction> recentActions;
    std::deque<std::pair<std::string, std::chrono::steady_clock::time_point>> actionOrder;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastActionByActor;
    std::unordered_map<std::string, PendingNpcInteraction> pendingNpcInteractions;

    class BridgeMovementAction final : public MovementAction
    {
    public:
        explicit BridgeMovementAction(PlayerbotAI* ai) : MovementAction(ai, "bridge npc approach") { }
        bool Execute(Event) override { return false; }
        bool Approach(Creature* target) { return MoveNear(target, 2.0f, MovementPriority::MOVEMENT_FORCED); }
    };

    template <typename T>
    bool ParseInteger(std::string const& token, T& result)
    {
        if (token.empty())
            return false;
        T parsed{};
        auto const conversion = std::from_chars(token.data(), token.data() + token.size(), parsed);
        if (conversion.ec != std::errc{} || conversion.ptr != token.data() + token.size())
            return false;
        result = parsed;
        return true;
    }

    bool IsSameWorldPhase(Player const* actor, WorldObject const* target)
    {
        return actor && target && actor->GetMap() == target->GetMap()
            && actor->GetInstanceId() == target->GetInstanceId() && actor->InSamePhase(target);
    }

    Creature* ResolveTalkableNpc(Player* actor, uint64 guid)
    {
        if (!actor || !guid)
            return nullptr;
        Creature* npc = ObjectAccessor::GetCreature(*actor, ObjectGuid(guid));
        if (!npc || !npc->IsAlive() || !IsSameWorldPhase(actor, npc)
            || actor->GetDistance(npc) > 100.0f || npc->IsInCombat())
            return nullptr;
        if (!npc->IsQuestGiver() && !npc->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
            return nullptr;
        return npc;
    }

    bool OpenNpcDialog(Player* actor, Creature* npc, std::string const& actionId)
    {
        if (!actor || !npc || !actor->GetSession() || !actor->GetNPCIfCanInteractWith(npc->GetGUID(), UNIT_NPC_FLAG_NONE))
            return false;
        WorldPacket packet;
        packet << npc->GetGUID();
        actor->GetSession()->HandleGossipHelloOpcode(packet);
        PublishNpcDialogEvent(actor, actionId);
        return true;
    }

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
    std::string const& groupId, std::string const& actionId, std::string const& actionType,
    std::string const& spellToken, std::string const& targetToken, std::string const& optionToken)
{
    if (!IsSafeCharacterName(actorName) || !IsSafeCharacterName(requesterName)
        || !IsValidActionId(actionId) || !IsDecimalGroupId(groupId))
        return MakeResult(actionId, actorName, actionType, "rejected", "invalid_request");

    if (auto found = recentActions.find(actionId); found != recentActions.end())
        return found->second.result;

    ActionResult result = MakeResult(actionId, actorName, actionType, "rejected", "invalid_request");
    ActionTypeInfo const* actionInfo = FindActionType(actionType);
    if (!actionInfo)
    {
        result.reasonCode = "unsupported_action";
        return Finish(nullptr, std::move(result), false);
    }

    uint32 spellId = 0;
    uint64 targetGuid = 0;
    int32 optionIndex = -1;
    if (!ParseInteger(spellToken, spellId) || !ParseInteger(targetToken, targetGuid)
        || !ParseInteger(optionToken, optionIndex))
    {
        result.reasonCode = "invalid_action_arguments";
        return Finish(nullptr, std::move(result), false);
    }
    bool const isCast = actionType == "cast_spell";
    bool const isNpcAction = actionType == "interact_npc" || actionType == "gossip_select";
    bool const isGossipSelect = actionType == "gossip_select";
    if ((isCast ? spellId == 0 : spellId != 0)
        || (isNpcAction ? targetGuid == 0 : (targetGuid != 0 && !isCast))
        || (isGossipSelect ? optionIndex < 0 || optionIndex > 255 : optionIndex != -1))
    {
        result.reasonCode = "invalid_action_arguments";
        return Finish(nullptr, std::move(result), false);
    }

    Player* requester = ObjectAccessor::FindPlayerByName(requesterName);
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

    Group* requesterGroup = requester->GetGroup();
    if (!requesterGroup || std::to_string(requesterGroup->GetLeaderGUID().GetRawValue()) != groupId)
    {
        result.reasonCode = "requester_group_changed";
        return Finish(requester, std::move(result), true);
    }

    std::string const rateKey = Lower(requesterName) + ":" + Lower(actorName);
    auto const now = std::chrono::steady_clock::now();
    auto previous = lastActionByActor.find(rateKey);
    if (previous != lastActionByActor.end() && now - previous->second < ACTION_RATE_LIMIT)
    {
        result.reasonCode = "rate_limited";
        return Finish(requester, std::move(result), true);
    }

    Player* actor = ObjectAccessor::FindPlayerByName(actorName);
    if (actionType == "ensure_online")
    {
        if (actor && actor->IsInWorld())
        {
            PlayerbotAI* actorAI = sPlayerbotsMgr.GetPlayerbotAI(actor);
            if (actor->GetGroup() != requesterGroup)
                result.reasonCode = "actor_not_in_group";
            else if (!actorAI || actorAI->GetMaster() != requester)
                result.reasonCode = "actor_not_owned_by_requester";
            else
            {
                result.status = "accepted";
                result.reasonCode = "already_online";
            }
            lastActionByActor[rateKey] = now;
            return Finish(requester, std::move(result), true);
        }

        PlayerbotMgr* manager = sPlayerbotsMgr.GetPlayerbotMgr(requester);
        if (!manager)
        {
            result.reasonCode = "playerbot_manager_unavailable";
            return Finish(requester, std::move(result), true);
        }
        std::string command = "add " + actorName;
        std::vector<std::string> messages = manager->HandlePlayerbotCommand(command.data(), requester);
        bool const queued = std::any_of(messages.begin(), messages.end(), [](std::string const& message)
        {
            return message.size() >= 5 && message.compare(message.size() - 5, 5, " - ok") == 0;
        });
        lastActionByActor[rateKey] = now;
        if (!queued)
        {
            result.reasonCode = "ensure_online_failed";
            return Finish(requester, std::move(result), true);
        }
        result.status = "accepted";
        result.reasonCode = "login_requested";
        return Finish(requester, std::move(result), true);
    }

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
    if (actor->GetGroup() != requesterGroup)
    {
        result.reasonCode = "actor_not_in_group";
        return Finish(requester, std::move(result), true);
    }
    if (botAI->GetMaster() != requester)
    {
        result.reasonCode = "actor_not_owned_by_requester";
        return Finish(requester, std::move(result), true);
    }

    if (actionType == "cast_spell")
    {
        auto spell = actor->GetSpellMap().find(spellId);
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (spell == actor->GetSpellMap().end() || !spell->second
            || spell->second->State == PLAYERSPELL_REMOVED || !spell->second->Active
            || !spellInfo || spellInfo->IsPassive())
        {
            result.reasonCode = "spell_not_active_known";
            return Finish(requester, std::move(result), true);
        }

        Unit* target = targetGuid ? ObjectAccessor::GetUnit(*actor, ObjectGuid(targetGuid)) : actor;
        if (!target || !IsSameWorldPhase(actor, target) || !actor->IsWithinLOSInMap(target))
        {
            result.reasonCode = "invalid_spell_target";
            return Finish(requester, std::move(result), true);
        }
        if (target != actor)
        {
            Player* targetPlayer = target->ToPlayer();
            Creature* targetCreature = target->ToCreature();
            bool const groupMember = targetPlayer && targetPlayer->GetGroup() == requesterGroup;
            bool const validHostile = targetCreature && targetCreature->IsAlive()
                && actor->IsValidAttackTarget(targetCreature);
            if (!groupMember && !validHostile)
            {
                result.reasonCode = "invalid_spell_target";
                return Finish(requester, std::move(result), true);
            }
        }
        if (!botAI->CanCastSpell(spellId, target))
        {
            result.reasonCode = "native_cast_rejected";
            return Finish(requester, std::move(result), true);
        }
        if (!botAI->CastSpell(spellId, target))
        {
            result.reasonCode = "cast_start_failed";
            return Finish(requester, std::move(result), true);
        }
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "cast_started";
        return Finish(requester, std::move(result), true);
    }

    if (actionType == "interact_npc")
    {
        if (actor->IsInCombat())
        {
            result.reasonCode = "actor_in_combat";
            return Finish(requester, std::move(result), true);
        }
        Creature* npc = ResolveTalkableNpc(actor, targetGuid);
        if (!npc)
        {
            result.reasonCode = "npc_unavailable_or_out_of_range";
            return Finish(requester, std::move(result), true);
        }
        if (actor->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
        {
            if (!actor->IsWithinLOSInMap(npc) || !OpenNpcDialog(actor, npc, actionId))
            {
                result.reasonCode = "npc_interaction_rejected";
                return Finish(requester, std::move(result), true);
            }
            lastActionByActor[rateKey] = now;
            result.status = "accepted";
            result.reasonCode = "dialog_opened";
            return Finish(requester, std::move(result), true);
        }
        if (pendingNpcInteractions.size() >= MAX_PENDING_NPC_INTERACTIONS
            || pendingNpcInteractions.find(Lower(actorName)) != pendingNpcInteractions.end())
        {
            result.reasonCode = "npc_approach_busy";
            return Finish(requester, std::move(result), true);
        }
        BridgeMovementAction movement(botAI);
        if (!movement.Approach(npc))
        {
            result.reasonCode = "npc_approach_failed";
            return Finish(requester, std::move(result), true);
        }
        pendingNpcInteractions.emplace(Lower(actorName), PendingNpcInteraction{
            actorName, requesterName, actionId, groupId, targetGuid, now + NPC_APPROACH_TIMEOUT});
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "approaching_npc";
        return Finish(requester, std::move(result), true);
    }

    if (actionType == "gossip_select")
    {
        Creature* npc = actor->GetNPCIfCanInteractWith(ObjectGuid(targetGuid), UNIT_NPC_FLAG_NONE);
        if (!npc || !actor->PlayerTalkClass
            || actor->PlayerTalkClass->GetGossipMenu().GetSenderGUID() != ObjectGuid(targetGuid))
        {
            result.reasonCode = "npc_dialog_not_open";
            return Finish(requester, std::move(result), true);
        }
        GossipMenu& menu = actor->PlayerTalkClass->GetGossipMenu();
        GossipMenuItem const* item = menu.GetItem(static_cast<uint32>(optionIndex));
        if (!item)
        {
            result.reasonCode = "gossip_option_missing";
            return Finish(requester, std::move(result), true);
        }
        if (item->IsCoded || item->BoxMoney != 0
            || (item->OptionType != GOSSIP_OPTION_GOSSIP && item->OptionType != GOSSIP_OPTION_QUESTGIVER))
        {
            result.reasonCode = "gossip_option_not_supported";
            return Finish(requester, std::move(result), true);
        }
        WorldPacket packet;
        packet << ObjectGuid(targetGuid);
        packet << menu.GetMenuId() << static_cast<uint32>(optionIndex);
        packet << std::string();
        actor->GetSession()->HandleGossipSelectOptionOpcode(packet);
        PublishNpcDialogEvent(actor, actionId);
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "gossip_option_selected";
        return Finish(requester, std::move(result), true);
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

    if (actionType == "stop_fishing")
    {
        botAI->ChangeStrategy("-master fishing", BOT_STATE_NON_COMBAT);
        botAI->ChangeStrategy("-use bobber", BOT_STATE_NON_COMBAT);
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "strategies_cleared";
        return Finish(requester, std::move(result), true);
    }

    if (!actionInfo->playerbotAction || !botAI->DoSpecificAction(actionInfo->playerbotAction, Event(), true))
    {
        result.reasonCode = "action_unavailable";
        return Finish(requester, std::move(result), true);
    }

    lastActionByActor[rateKey] = now;
    result.status = "accepted";
    result.reasonCode = "started";
    return Finish(requester, std::move(result), true);
}

void UpdatePendingTypedActions()
{
    auto const now = std::chrono::steady_clock::now();
    for (auto iter = pendingNpcInteractions.begin(); iter != pendingNpcInteractions.end(); )
    {
        PendingNpcInteraction const pending = iter->second;
        Player* requester = ObjectAccessor::FindPlayerByName(pending.requester);
        Player* actor = ObjectAccessor::FindPlayerByName(pending.actor);
        auto fail = [&](char const* reason)
        {
            if (requester && requester->IsInWorld())
                PublishActionResultEvent(requester, pending.actor, pending.actionId, "interact_npc", "rejected", reason);
            iter = pendingNpcInteractions.erase(iter);
        };
        if (now >= pending.expires)
        {
            fail("npc_approach_timeout");
            continue;
        }
        if (!requester || !requester->IsInWorld() || !actor || !actor->IsInWorld()
            || !actor->GetGroup() || actor->GetGroup() != requester->GetGroup()
            || std::to_string(actor->GetGroup()->GetLeaderGUID().GetRawValue()) != pending.groupId)
        {
            fail("npc_approach_context_changed");
            continue;
        }
        Creature* npc = ResolveTalkableNpc(actor, pending.targetGuid);
        if (!npc || actor->IsInCombat())
        {
            fail("npc_lost_or_actor_in_combat");
            continue;
        }
        if (actor->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
        {
            if (!actor->IsWithinLOSInMap(npc) || !OpenNpcDialog(actor, npc, pending.actionId))
                fail("npc_interaction_rejected");
            else
                iter = pendingNpcInteractions.erase(iter);
            continue;
        }
        ++iter;
    }
}
