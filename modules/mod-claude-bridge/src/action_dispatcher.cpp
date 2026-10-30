#include "action_dispatcher.h"
#include "vendor_journey.h"
#include "vendor_transport.h"

#include "Config.h"
#include "ConditionMgr.h"
#include "DBCStores.h"
#include "Bag.h"
#include "AttackAction.h"
#include "LootAction.h"
#include "LootObjectStack.h"
#include "LootMgr.h"
#include "Creature.h"
#include "CreatureData.h"
#include "Event.h"
#include "GossipDef.h"
#include "Group.h"
#include "Item.h"
#include "ItemPackets.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "AiObjectContext.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "MovementActions.h"
#include "MotionMaster.h"
#include "SpellMgr.h"
#include "TradeData.h"
#include "Trainer.h"
#include "TravelMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "world_state_observer.h"
#include "world_state_writer.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <limits>
#include <deque>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr std::size_t MAX_CACHED_ACTIONS = 512;
    constexpr std::size_t MAX_PENDING_NPC_INTERACTIONS = 32;
    constexpr std::size_t MAX_HUNT_SPAWN_POINTS = 128;
    constexpr std::size_t MAX_HUNT_LOOT_STACKS = TRADE_SLOT_TRADED_COUNT;
    constexpr auto ACTION_CACHE_LIFETIME = std::chrono::minutes(10);
    constexpr auto ACTION_RATE_LIMIT = std::chrono::seconds(2);
    constexpr auto NPC_APPROACH_TIMEOUT = std::chrono::seconds(60);
    constexpr auto HUNT_TIMEOUT = std::chrono::minutes(60);
    constexpr auto GUIDE_TIMEOUT = std::chrono::minutes(60);
    constexpr float GUIDE_ARRIVAL_DISTANCE = 45.0f;

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
        {"use_item", nullptr},
        {"equip_item", nullptr},
        {"interact_npc", nullptr},
        {"gossip_select", nullptr},
        {"trainer_action", nullptr},
        {"vendor_inspect", nullptr},
        {"vendor_buy", nullptr},
        {"vendor_travel", nullptr},
        {"vendor_travel_cancel", nullptr},
        {"offer_items", nullptr},
        {"hunt_start", nullptr},
        {"hunt_cancel", nullptr},
        {"hunt_loot", nullptr},
        {"guide_start", nullptr},
        {"guide_cancel", nullptr},
        {"playerbot_action", nullptr},
    };

    struct PendingNpcInteraction
    {
        std::string actor;
        std::string requester;
        std::string actionId;
        std::string groupId;
        uint64 targetGuid = 0;
        std::chrono::steady_clock::time_point expires;
        std::string type = "interact_npc";
        std::string taskToken = "-";
    };

    struct VendorBuyRequest
    {
        uint32 itemEntry = 0;
        uint32 quantity = 0;
        uint32 maxTotalCopper = 0;
        bool allowExtendedCost = false;
    };

    struct InspectedVendorItem
    {
        uint32 slot = 0; // Native zero-based slot; telemetry uses client one-based slot.
        uint32 entry = 0;
        uint32 buyCount = 0;
        int32 buyPrice = 0;
        uint32 extendedCost = 0;
    };

    struct InspectedVendor
    {
        uint64 actorGuid = 0;
        uint64 npcGuid = 0;
        uint32 vendorEntry = 0;
        std::vector<InspectedVendorItem> items;
        std::chrono::steady_clock::time_point expires;
    };

    struct VendorTravelRequest
    {
        VendorJourneyLocation destination;
        uint32 purchaseReserveCopper = 0;
    };

    struct VendorTravelTask
    {
        std::string actor;
        std::string requester;
        std::string groupId;
        std::string actionId;
        uint64 actorGuid = 0;
        uint64 requesterGuid = 0;
        uint32 purchaseReserveCopper = 0;
        std::vector<std::pair<BotState, std::string>> restoreStrategies;
        std::unique_ptr<VendorJourney> journey;
        VendorJourneyRoute walkingFallback;
        bool hasWalkingFallback = false;
        bool fallbackTried = false;
        bool taxiDeparted = false;
        VendorJourneyStage lastPublishedStage = VendorJourneyStage::WalkingToDestination;
        std::chrono::steady_clock::time_point lastTick;
    };

    struct DeferredVendorStrategyRestore
    {
        std::string actor;
        std::string groupId;
        std::string actionId;
        uint64 actorGuid = 0;
        uint32 mapId = 0;
        std::vector<std::pair<BotState, std::string>> restoreStrategies;
    };

    struct ItemOfferRequest
    {
        uint32 itemEntry = 0;
        uint32 stackCount = 0;
    };

    struct ItemLocationRequest
    {
        uint32 itemEntry = 0;
        uint8 bag = 0;
        uint8 slot = 0;
    };

    struct HuntRequest
    {
        uint32 creatureEntry = 0;
        std::vector<uint32> creatureEntries;
        uint16 mapId = 0;
        uint32 zoneId = 0;
        uint32 areaId = 0;
        uint32 killCount = 0;
        bool deliverLoot = false;
        bool collectLoot = false;
    };

    struct GuideRequest
    {
        std::vector<uint32> creatureEntries;
        uint16 mapId = 0;
        uint32 zoneId = 0;
        uint32 areaId = 0;
    };

    enum class AgentHuntStage
    {
        Hunting,
        Looting,
        Returning,
        Complete,
        Cancelled,
        Failed,
    };

    enum class AgentGuideStage
    {
        Traveling,
        WaitingForUser,
        Arrived,
        Cancelled,
        Failed,
    };

    class AgentHuntDestination final : public TravelDestination
    {
    public:
        AgentHuntDestination(uint32 creatureEntry, std::vector<WorldPosition> const& locations)
            : TravelDestination(25.0f, 125.0f), _creatureEntry(creatureEntry)
        {
            _locations.reserve(locations.size());
            for (WorldPosition const& location : locations)
            {
                _locations.push_back(std::make_unique<WorldPosition>(location));
                addPoint(_locations.back().get());
            }
            setExpireDelay(2 * MINUTE * IN_MILLISECONDS);
            setCooldownDelay(0);
            setMaxVisitors(1, 1);
        }

        bool isActive([[maybe_unused]] Player* bot) override { return true; }
        std::string const getName() override { return "AgentHuntDestination"; }
        int32 getEntry() override { return static_cast<int32>(_creatureEntry); }
        std::string const getTitle() override { return "agent hunt target"; }

    private:
        uint32 _creatureEntry;
        std::vector<std::unique_ptr<WorldPosition>> _locations;
    };

    class AgentGuideDestination final : public TravelDestination
    {
    public:
        AgentGuideDestination(uint32 creatureEntry, std::vector<WorldPosition> const& locations)
            : TravelDestination(20.0f, 40.0f), _creatureEntry(creatureEntry)
        {
            _locations.reserve(locations.size());
            for (WorldPosition const& location : locations)
            {
                _locations.push_back(std::make_unique<WorldPosition>(location));
                addPoint(_locations.back().get());
            }
            setExpireDelay(2 * MINUTE * IN_MILLISECONDS);
            setCooldownDelay(0);
            setMaxVisitors(1, 1);
        }

        bool isActive([[maybe_unused]] Player* bot) override { return true; }
        std::string const getName() override { return "AgentGuideDestination"; }
        int32 getEntry() override { return static_cast<int32>(_creatureEntry); }
        std::string const getTitle() override { return "agent guide target"; }

    private:
        uint32 _creatureEntry;
        std::vector<std::unique_ptr<WorldPosition>> _locations;
    };

    struct AgentHuntTask
    {
        std::string actor;
        std::string requester;
        std::string targetName;
        std::string zoneName;
        uint32 creatureEntry = 0;
        std::vector<uint32> creatureEntries;
        uint16 mapId = 0;
        uint32 zoneId = 0;
        uint32 areaId = 0;
        uint32 requiredKills = 0;
        uint32 kills = 0;
        bool deliverLoot = false;
        bool collectLoot = false;
        AgentHuntStage stage = AgentHuntStage::Hunting;
        std::chrono::steady_clock::time_point startedAt;
        std::chrono::steady_clock::time_point lastTargetKill;
        std::unordered_map<uint64, uint32> existingItemCounts;
        std::unordered_map<uint64, uint32> lootedItemStackCounts;
        std::unordered_set<uint64> targetCorpseGuids;
        std::unordered_set<uint64> pendingLootCorpses;
        std::set<uint32> addedLootEntries;
        std::chrono::steady_clock::time_point lastRouteProgress;
        std::map<uint32, uint32> lootedItems;
        std::unordered_set<WorldPosition*> visitedSpawnPoints;
        WorldPosition* currentSpawnPoint = nullptr;
        std::unique_ptr<AgentHuntDestination> destination;
        bool hadFollowStrategy = false;
        bool hadStayStrategy = false;
        bool hadTravelStrategy = false;
        bool hadGrindStrategy = false;
        bool hadLootStrategy = false;
        uint32 stagedStacks = 0;
        uint32 stagedItems = 0;
    };

    struct AgentGuideTask
    {
        std::string actor;
        std::string requester;
        std::string targetName;
        std::string zoneName;
        std::vector<uint32> creatureEntries;
        uint16 mapId = 0;
        uint32 zoneId = 0;
        uint32 areaId = 0;
        AgentGuideStage stage = AgentGuideStage::Traveling;
        std::chrono::steady_clock::time_point startedAt;
        WorldPosition* currentSpawnPoint = nullptr;
        std::unique_ptr<AgentGuideDestination> destination;
        bool hadFollowStrategy = false;
        bool hadStayStrategy = false;
        bool hadTravelStrategy = false;
        bool hadGrindStrategy = false;
        bool hadLootStrategy = false;
    };

    std::unordered_map<std::string, CachedAction> recentActions;
    std::deque<std::pair<std::string, std::chrono::steady_clock::time_point>> actionOrder;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastActionByActor;
    std::unordered_map<std::string, PendingNpcInteraction> pendingNpcInteractions;
    std::unordered_map<std::string, InspectedVendor> inspectedVendors;
    std::unordered_map<std::string, std::unique_ptr<VendorTravelTask>> vendorTravelTasks;
    std::unordered_map<uint64, DeferredVendorStrategyRestore> deferredVendorStrategyRestores;
    std::mutex agentHuntMutex;
    std::unordered_map<std::string, std::unique_ptr<AgentHuntTask>> agentHuntTasks;
    std::unordered_map<std::string, std::unique_ptr<AgentGuideTask>> agentGuideTasks;

    class BridgeMovementAction final : public MovementAction
    {
    public:
        explicit BridgeMovementAction(PlayerbotAI* ai) : MovementAction(ai, "bridge npc approach") { }
        bool Execute(Event) override { return false; }
        bool Approach(Creature* target) { return MoveNear(target, 2.0f, MovementPriority::MOVEMENT_FORCED); }
    };

    class BridgeAttackAction final : public AttackAction
    {
    public:
        explicit BridgeAttackAction(PlayerbotAI* ai) : AttackAction(ai, "agent hunt") { }
        bool Start(Unit* target) { return Attack(target); }
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

    bool ParseVendorBuyRequest(std::string const& token, VendorBuyRequest& request)
    {
        if (token.size() > 64 || token.compare(0, 2, "v:") != 0)
            return false;
        std::vector<std::string> fields;
        std::size_t start = 2;
        while (start <= token.size())
        {
            std::size_t const end = token.find(':', start);
            fields.push_back(token.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        uint32 allow = 0;
        if (fields.size() != 4 || !ParseInteger(fields[0], request.itemEntry)
            || !ParseInteger(fields[1], request.quantity) || !ParseInteger(fields[2], request.maxTotalCopper)
            || !ParseInteger(fields[3], allow) || !request.itemEntry || !request.quantity || allow > 1
            || request.maxTotalCopper > MAX_MONEY_AMOUNT)
            return false;
        request.allowExtendedCost = allow == 1;
        return true;
    }

    bool ParseVendorTravelRequest(std::string const& token, VendorTravelRequest& request)
    {
        if (token.size() > 160 || token.compare(0, 2, "r:") != 0)
            return false;
        std::vector<std::string> fields;
        std::size_t start = 2;
        while (start <= token.size())
        {
            std::size_t const end = token.find(':', start);
            fields.push_back(token.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        auto parseCoordinate = [](std::string const& field, float& result)
        {
            if (field.empty())
                return false;
            auto const parsed = std::from_chars(field.data(), field.data() + field.size(), result);
            return parsed.ec == std::errc{} && parsed.ptr == field.data() + field.size()
                && std::isfinite(result) && std::abs(result) < 17066.0f;
        };
        return fields.size() == 5 && ParseInteger(fields[0], request.destination.mapId)
            && parseCoordinate(fields[1], request.destination.x) && parseCoordinate(fields[2], request.destination.y)
            && parseCoordinate(fields[3], request.destination.z) && ParseInteger(fields[4], request.purchaseReserveCopper)
            && request.purchaseReserveCopper <= MAX_MONEY_AMOUNT;
    }

    bool ParseHexBytes(std::string const& token, std::string& result, std::size_t maxBytes,
        bool allowEmpty)
    {
        if (token.size() % 2 || token.size() / 2 > maxBytes || (!allowEmpty && token.empty()))
            return false;

        auto hexValue = [](char value) -> int
        {
            if (value >= '0' && value <= '9')
                return value - '0';
            if (value >= 'a' && value <= 'f')
                return value - 'a' + 10;
            if (value >= 'A' && value <= 'F')
                return value - 'A' + 10;
            return -1;
        };

        result.clear();
        result.reserve(token.size() / 2);
        for (std::size_t i = 0; i < token.size(); i += 2)
        {
            int const high = hexValue(token[i]);
            int const low = hexValue(token[i + 1]);
            if (high < 0 || low < 0)
                return false;
            result.push_back(static_cast<char>((high << 4) | low));
        }
        return true;
    }

    bool ParsePlayerbotActionToken(std::string const& token, std::string& actionName,
        std::string& qualifier)
    {
        if (token.size() < 4 || token.front() != 'p')
            return false;
        std::size_t const separator = token.find('.', 1);
        if (separator == std::string::npos || token.find('.', separator + 1) != std::string::npos)
            return false;

        if (!ParseHexBytes(token.substr(1, separator - 1), actionName, 128, false)
            || !ParseHexBytes(token.substr(separator + 1), qualifier, 256, true))
            return false;
        if (actionName.empty() || actionName.front() == ' ' || actionName.back() == ' ')
            return false;
        for (unsigned char value : actionName)
            if (value < 0x20 || value > 0x7E)
                return false;
        for (unsigned char value : qualifier)
            if (value < 0x20 || value == 0x7F)
                return false;
        return true;
    }

    bool ParseItemOfferBundle(std::string const& token, std::vector<ItemOfferRequest>& requests)
    {
        if (token == "-")
            return true;
        if (token.size() > 128)
            return false;

        std::set<uint32> seenEntries;
        uint32 totalStacks = 0;
        std::size_t start = 0;
        while (start < token.size())
        {
            std::size_t const comma = token.find(',', start);
            std::string const item = token.substr(start, comma == std::string::npos
                ? std::string::npos : comma - start);
            std::size_t const colon = item.find(':');
            ItemOfferRequest request;
            if (colon == std::string::npos || item.find(':', colon + 1) != std::string::npos
                || !ParseInteger(item.substr(0, colon), request.itemEntry)
                || !ParseInteger(item.substr(colon + 1), request.stackCount)
                || !request.itemEntry || !request.stackCount
                || request.stackCount > TRADE_SLOT_TRADED_COUNT
                || !seenEntries.insert(request.itemEntry).second)
                return false;

            totalStacks += request.stackCount;
            if (totalStacks > TRADE_SLOT_TRADED_COUNT)
                return false;
            requests.push_back(request);
            if (comma == std::string::npos)
                break;
            if (comma + 1 == token.size())
                return false;
            start = comma + 1;
        }
        return !requests.empty();
    }

    bool ParseItemLocation(std::string const& token, ItemLocationRequest& request)
    {
        if (token.empty() || token.size() > 32)
            return false;

        std::size_t const first = token.find(':');
        std::size_t const second = first == std::string::npos ? std::string::npos : token.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos
            || token.find(':', second + 1) != std::string::npos
            || !ParseInteger(token.substr(0, first), request.itemEntry))
            return false;

        uint32 bag = 0;
        uint32 slot = 0;
        if (!ParseInteger(token.substr(first + 1, second - first - 1), bag)
            || !ParseInteger(token.substr(second + 1), slot)
            || !request.itemEntry || bag > 0xFF || slot > 0xFF
            || (bag != INVENTORY_SLOT_BAG_0 && (bag < INVENTORY_SLOT_BAG_START || bag >= INVENTORY_SLOT_BAG_END)))
            return false;

        request.bag = static_cast<uint8>(bag);
        request.slot = static_cast<uint8>(slot);
        return true;
    }

    bool ParseHuntRequest(std::string const& token, HuntRequest& request)
    {
        if (token.empty() || token.size() > 256)
            return false;

        std::vector<std::string> fields;
        std::size_t start = 0;
        while (start <= token.size())
        {
            std::size_t const separator = token.find(':', start);
            fields.push_back(token.substr(start, separator == std::string::npos
                ? std::string::npos : separator - start));
            if (separator == std::string::npos)
                break;
            start = separator + 1;
        }

        bool const legacy = fields.size() == 5;
        if (!legacy && fields.size() != 6 && fields.size() != 7)
            return false;

        std::unordered_set<uint32> seenEntries;
        std::size_t entryStart = 0;
        while (entryStart <= fields[0].size())
        {
            std::size_t const comma = fields[0].find(',', entryStart);
            uint32 entry = 0;
            if (!ParseInteger(fields[0].substr(entryStart, comma == std::string::npos
                ? std::string::npos : comma - entryStart), entry)
                || !entry || !seenEntries.insert(entry).second || request.creatureEntries.size() >= 16)
                return false;
            request.creatureEntries.push_back(entry);
            if (comma == std::string::npos)
                break;
            entryStart = comma + 1;
            if (entryStart == fields[0].size())
                return false;
        }

        uint32 deliverLoot = 0;
        if (!ParseInteger(fields[1], request.mapId) || !ParseInteger(fields[2], request.zoneId))
            return false;
        if (legacy)
        {
            if (!ParseInteger(fields[3], request.killCount) || !ParseInteger(fields[4], deliverLoot))
                return false;
        }
        else if (!ParseInteger(fields[3], request.areaId) || !ParseInteger(fields[4], request.killCount)
            || !ParseInteger(fields[5], deliverLoot))
            return false;

        request.creatureEntry = request.creatureEntries.front();
        request.deliverLoot = deliverLoot == 1;
        uint32 collectLoot = deliverLoot;
        if (fields.size() == 7 && (!ParseInteger(fields[6], collectLoot) || collectLoot > 1))
            return false;
        request.collectLoot = collectLoot == 1 || request.deliverLoot;
        return request.creatureEntry && request.zoneId <= 0xFFFF && request.areaId <= 0xFFFF && request.killCount
            && request.killCount <= 100 && deliverLoot <= 1;
    }

    bool ParseGuideRequest(std::string const& token, GuideRequest& request)
    {
        if (token.empty() || token.size() > 256)
            return false;

        std::vector<std::string> fields;
        std::size_t start = 0;
        while (start <= token.size())
        {
            std::size_t const separator = token.find(':', start);
            fields.push_back(token.substr(start, separator == std::string::npos
                ? std::string::npos : separator - start));
            if (separator == std::string::npos)
                break;
            start = separator + 1;
        }
        if (fields.size() != 4)
            return false;

        bool const zoneOnly = fields[0] == "-";
        std::unordered_set<uint32> seenEntries;
        std::size_t entryStart = 0;
        while (!zoneOnly && entryStart <= fields[0].size())
        {
            std::size_t const comma = fields[0].find(',', entryStart);
            uint32 entry = 0;
            if (!ParseInteger(fields[0].substr(entryStart, comma == std::string::npos
                ? std::string::npos : comma - entryStart), entry)
                || !entry || !seenEntries.insert(entry).second || request.creatureEntries.size() >= 16)
                return false;
            request.creatureEntries.push_back(entry);
            if (comma == std::string::npos)
                break;
            entryStart = comma + 1;
            if (entryStart == fields[0].size())
                return false;
        }

        uint32 mapId = 0;
        if (!ParseInteger(fields[1], mapId) || !ParseInteger(fields[2], request.zoneId)
            || !ParseInteger(fields[3], request.areaId)
            || (!zoneOnly && request.creatureEntries.empty()) || (zoneOnly && !request.zoneId)
            || mapId > 0xFFFF || request.zoneId > 0xFFFF || request.areaId > 0xFFFF)
            return false;

        request.mapId = static_cast<uint16>(mapId);
        return true;
    }

    std::string AgentHuntStageName(AgentHuntStage stage)
    {
        switch (stage)
        {
            case AgentHuntStage::Hunting: return "hunting";
            case AgentHuntStage::Looting: return "looting";
            case AgentHuntStage::Returning: return "returning";
            case AgentHuntStage::Complete: return "complete";
            case AgentHuntStage::Cancelled: return "cancelled";
            case AgentHuntStage::Failed: return "failed";
        }
        return "unknown";
    }

    std::string AgentGuideStageName(AgentGuideStage stage)
    {
        switch (stage)
        {
            case AgentGuideStage::Traveling: return "traveling";
            case AgentGuideStage::WaitingForUser: return "waiting_for_user";
            case AgentGuideStage::Arrived: return "arrived";
            case AgentGuideStage::Cancelled: return "cancelled";
            case AgentGuideStage::Failed: return "failed";
        }
        return "unknown";
    }

    bool IsAgentHuntActive(AgentHuntTask const& task)
    {
        return task.stage == AgentHuntStage::Hunting || task.stage == AgentHuntStage::Looting
            || task.stage == AgentHuntStage::Returning;
    }

    bool IsAgentGuideActive(AgentGuideTask const& task)
    {
        return task.stage == AgentGuideStage::Traveling || task.stage == AgentGuideStage::WaitingForUser;
    }

    bool IsCreatureDataForEntry(CreatureData const& data, uint32 entry)
    {
        return data.id == entry || data.id2 == entry || data.id3 == entry;
    }

    bool IsSpawnInRequestedZone(WorldPosition point, uint32 zoneId, uint32 requestedAreaId)
    {
        uint16 const actualAreaId = point.getAreaId();
        if (requestedAreaId && requestedAreaId != actualAreaId)
            return false;
        if (!zoneId)
            return true;

        AreaTableEntry const* area = sAreaTableStore.LookupEntry(actualAreaId);
        return actualAreaId == zoneId || (area && area->zone == zoneId);
    }

    std::vector<WorldPosition> FindAgentCreatureSpawns(Player* actor, std::vector<uint32> const& creatureEntries,
        uint16 mapId, uint32 zoneId, uint32 areaId)
    {
        std::vector<WorldPosition> locations;
        if (!actor)
            return locations;

        std::unordered_set<uint32> seenSpawns;
        for (auto const& [guid, data] : sObjectMgr->GetAllCreatureData())
        {
            bool const matchesCreature = creatureEntries.empty() || std::find_if(creatureEntries.begin(), creatureEntries.end(),
                    [&data](uint32 entry) { return IsCreatureDataForEntry(data, entry); })
                != creatureEntries.end();
            if (!matchesCreature || data.mapid != mapId
                || (data.phaseMask && !(data.phaseMask & actor->GetPhaseMask())))
                continue;

            WorldPosition point(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
            if (!IsSpawnInRequestedZone(point, zoneId, areaId)
                || !seenSpawns.insert(guid).second)
                continue;
            locations.push_back(point);
        }

        WorldPosition const actorLocation(actor);
        std::sort(locations.begin(), locations.end(), [&actorLocation](WorldPosition const& left,
            WorldPosition const& right)
        {
            return TravelMgr::instance().mapTransDistance(actorLocation, left)
                < TravelMgr::instance().mapTransDistance(actorLocation, right);
        });
        if (locations.size() > MAX_HUNT_SPAWN_POINTS)
            locations.resize(MAX_HUNT_SPAWN_POINTS);
        return locations;
    }

    std::unordered_map<uint64, uint32> GetExistingBagItemCounts(Player* player)
    {
        std::unordered_map<uint64, uint32> itemCounts;
        if (!player)
            return itemCounts;

        auto append = [&itemCounts](Item* item)
        {
            if (item)
                itemCounts[item->GetGUID().GetRawValue()] = item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            append(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    append(player->GetItemByPos(bagSlot, static_cast<uint8>(slot)));
        return itemCounts;
    }

    Item* FindBagItemByGuid(Player* player, uint64 guid)
    {
        if (!player || !guid)
            return nullptr;

        auto find = [guid](Item* item)
        {
            return item && item->GetGUID().GetRawValue() == guid;
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); find(item))
                return item;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = player->GetItemByPos(bagSlot, static_cast<uint8>(slot)); find(item))
                        return item;
        return nullptr;
    }

    Item* FindItemAtLocation(Player* player, ItemLocationRequest const& location)
    {
        if (!player)
            return nullptr;

        if (location.bag == INVENTORY_SLOT_BAG_0)
        {
            if (location.slot < INVENTORY_SLOT_ITEM_START || location.slot >= INVENTORY_SLOT_ITEM_END)
                return nullptr;
        }
        else
        {
            Bag* bag = player->GetBagByPos(location.bag);
            if (!bag || location.slot >= bag->GetBagSize())
                return nullptr;
        }

        Item* item = player->GetItemByPos(location.bag, location.slot);
        return item && item->GetEntry() == location.itemEntry ? item : nullptr;
    }

    Item* SplitTaskLootStack(Player* player, Item* source, uint32 count)
    {
        if (!player || !source || !count || count >= source->GetCount() || source->IsInTrade())
            return nullptr;

        auto splitInto = [player, source, count](uint8 bag, uint8 slot) -> Item*
        {
            if (player->GetItemByPos(bag, slot))
                return nullptr;
            ItemPosCountVec destination;
            if (player->CanStoreItem(bag, slot, destination, source, false) != EQUIP_ERR_OK)
                return nullptr;

            uint16 const sourcePosition = (uint16(source->GetBagSlot()) << 8) | source->GetSlot();
            uint16 const destinationPosition = (uint16(bag) << 8) | slot;
            player->SplitItem(sourcePosition, destinationPosition, count);
            Item* split = player->GetItemByPos(bag, slot);
            return split != source && split && split->GetEntry() == source->GetEntry() ? split : nullptr;
        };

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* split = splitInto(INVENTORY_SLOT_BAG_0, slot))
                return split;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* split = splitInto(bagSlot, static_cast<uint8>(slot)))
                        return split;
        return nullptr;
    }

    bool IsSameWorldPhase(Player const* actor, WorldObject const* target)
    {
        return actor && target && actor->GetMap() == target->GetMap()
            && actor->GetInstanceId() == target->GetInstanceId() && actor->InSamePhase(target);
    }

    Creature* ResolveTalkableNpc(Player* actor, uint64 guid, bool vendorOnly = false)
    {
        if (!actor || !guid)
            return nullptr;
        Creature* npc = ObjectAccessor::GetCreature(*actor, ObjectGuid(guid));
        if (!npc || !npc->IsAlive() || !IsSameWorldPhase(actor, npc)
            || actor->GetDistance(npc) > 100.0f || npc->IsInCombat())
            return nullptr;
        if (vendorOnly ? !npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR)
            : (!npc->IsQuestGiver() && !npc->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP
                | UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS | UNIT_NPC_FLAG_TRAINER_PROFESSION)))
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

    // Mirror SendListInventory's per-player visibility rules. Never enumerate DB-only rows
    // or use an unbound session alternate vendor as the source of a catalog.
    bool IsVisibleVendorItem(Player* actor, Creature* npc, VendorItem const* item, ItemTemplate const* proto)
    {
        if (!item || !proto)
            return false;
        if (!actor->IsGameMaster())
        {
            if (!(proto->AllowableClass & actor->getClassMask()) && proto->Bonding == BIND_WHEN_PICKED_UP)
                return false;
            if ((proto->HasFlag2(ITEM_FLAG2_FACTION_HORDE) && actor->GetTeamId() == TEAM_ALLIANCE)
                || (proto->HasFlag2(ITEM_FLAG2_FACTION_ALLIANCE) && actor->GetTeamId() == TEAM_HORDE))
                return false;
            if (item->maxcount && !npc->GetVendorItemCurrentCount(item))
                return false;
        }
        ConditionList const conditions = sConditionMgr->GetConditionsForNpcVendorEvent(npc->GetEntry(), item->item);
        return sConditionMgr->IsObjectMeetToConditions(actor, npc, conditions);
    }

    VendorItemData const* OpenVendorInventory(Player* actor, Creature* npc)
    {
        if (!actor || !npc || !actor->IsAlive() || !actor->GetSession()
            || !actor->IsWithinLOSInMap(npc)
            || actor->GetNPCIfCanInteractWith(npc->GetGUID(), UNIT_NPC_FLAG_VENDOR) != npc)
            return nullptr;
        // Reset inherited alternate inventory. The native hook may select the alternate
        // inventory for THIS interaction; read that selection only after opening it.
        actor->GetSession()->SendListInventory(npc->GetGUID(), 0);
        uint32 const entry = actor->GetSession()->GetCurrentVendor();
        return entry ? sObjectMgr->GetNpcVendorItemList(entry) : npc->GetVendorItems();
    }

    void AppendVendorIdentity(std::ostringstream& out, Player* actor, Creature* npc,
        std::string const& actionId, uint64 targetGuid = 0)
    {
        out << "\"action_id\":\"" << JsonEscape(actionId)
            << "\",\"character\":\"" << JsonEscape(actor->GetName())
            << "\",\"character_guid\":\"" << actor->GetGUID().GetRawValue()
            << "\",\"npc_guid\":\"" << (npc ? npc->GetGUID().GetRawValue() : targetGuid)
            << "\",\"npc_entry\":" << (npc ? npc->GetEntry() : 0)
            << ",\"npc_name\":\"" << JsonEscape(npc ? npc->GetName() : "") << '"';
    }

    bool InspectVendor(Player* actor, Creature* npc, std::string const& actionId)
    {
        inspectedVendors.erase(Lower(actor->GetName()));
        VendorItemData const* items = OpenVendorInventory(actor, npc);
        if (!items)
            return false;
        InspectedVendor catalog;
        catalog.actorGuid = actor->GetGUID().GetRawValue();
        catalog.npcGuid = npc->GetGUID().GetRawValue();
        catalog.vendorEntry = actor->GetSession()->GetCurrentVendor();
        catalog.expires = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        std::ostringstream out;
        out << '{';
        AppendVendorIdentity(out, actor, npc, actionId);
        out << ",\"vendor_entry\":" << catalog.vendorEntry << ",\"money_copper\":" << actor->GetMoney()
            << ",\"honor_points\":" << actor->GetHonorPoints() << ",\"arena_points\":" << actor->GetArenaPoints()
            << ",\"items\":[";
        for (uint32 slot = 0; slot < items->GetItemCount() && catalog.items.size() < MAX_VENDOR_ITEMS; ++slot)
        {
            VendorItem const* item = items->GetItem(slot);
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            if (!IsVisibleVendorItem(actor, npc, item, proto))
                continue;
            if (!catalog.items.empty())
                out << ',';
            catalog.items.push_back({slot, item->item, proto->BuyCount, proto->BuyPrice, item->ExtendedCost});
            float const discount = actor->GetReputationPriceDiscount(npc);
            bool const validPrice = proto->BuyPrice >= 0;
            uint32 const price = validPrice && item->IsGoldRequired(proto)
                ? uint32(std::floor(proto->BuyPrice * discount)) : 0;
            out << "{\"entry\":" << item->item << ",\"name\":\"" << JsonEscape(proto->Name1)
                << "\",\"vendor_slot\":" << slot + 1 << ",\"buy_count\":" << proto->BuyCount
                << ",\"price_per_bundle_copper\":";
            if (validPrice)
                out << price;
            else
                out << "null";
            out << ",\"base_price_per_bundle_copper\":" << std::max<int32>(0, proto->BuyPrice)
                << ",\"valid_gold_price\":" << (validPrice ? "true" : "false")
                << ",\"gold_required\":" << (item->IsGoldRequired(proto) ? "true" : "false")
                << ",\"reputation_discount\":" << std::setprecision(std::numeric_limits<float>::max_digits10) << discount
                << ",\"stock\":";
            if (item->maxcount)
                out << npc->GetVendorItemCurrentCount(item);
            else
                out << "null";
            out << ",\"stock_max\":" << item->maxcount << ",\"stock_restock_seconds\":" << item->incrtime
                << ",\"extended_cost\":{\"id\":" << item->ExtendedCost;
            ItemExtendedCostEntry const* cost = item->ExtendedCost ? sItemExtendedCostStore.LookupEntry(item->ExtendedCost) : nullptr;
            out << ",\"valid\":" << (!item->ExtendedCost || cost ? "true" : "false")
                << ",\"honor_per_bundle\":" << (cost ? cost->reqhonorpoints : 0)
                << ",\"arena_per_bundle\":" << (cost ? cost->reqarenapoints : 0)
                << ",\"rating\":" << (cost ? cost->reqpersonalarenarating : 0)
                << ",\"arena_slot\":" << (cost ? cost->reqarenaslot : 0) << ",\"items\":[";
            bool firstCost = true;
            if (cost)
                for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
                    if (cost->reqitem[i])
                    {
                        ItemTemplate const* currency = sObjectMgr->GetItemTemplate(cost->reqitem[i]);
                        if (!firstCost)
                            out << ',';
                        firstCost = false;
                        out << "{\"entry\":" << cost->reqitem[i] << ",\"name\":\""
                            << JsonEscape(currency ? currency->Name1 : "") << "\",\"count_per_bundle\":"
                            << cost->reqitemcount[i] << ",\"available\":" << actor->GetItemCount(cost->reqitem[i], false) << '}';
                    }
            out << "]},\"requirements\":{\"reputation_faction\":" << proto->RequiredReputationFaction
                << ",\"reputation_rank\":" << proto->RequiredReputationRank
                << ",\"reputation_met\":" << (!proto->RequiredReputationFaction
                    || uint32(actor->GetReputationRank(proto->RequiredReputationFaction)) >= proto->RequiredReputationRank ? "true" : "false")
                << "},\"conditions_passed\":true,\"conditions\":[";
            ConditionList const conditions = sConditionMgr->GetConditionsForNpcVendorEvent(npc->GetEntry(), item->item);
            bool firstCondition = true;
            for (Condition const* condition : conditions)
            {
                if (!condition)
                    continue;
                if (!firstCondition)
                    out << ',';
                firstCondition = false;
                out << "{\"type\":" << uint32(condition->ConditionType) << ",\"reference_id\":" << condition->ReferenceId
                    << ",\"else_group\":" << condition->ElseGroup << ",\"target\":" << uint32(condition->ConditionTarget)
                    << ",\"negative\":" << (condition->NegativeCondition ? "true" : "false")
                    << ",\"values\":[" << condition->ConditionValue1 << ',' << condition->ConditionValue2 << ','
                    << condition->ConditionValue3 << "]}";
            }
            out << "]}";
        }
        out << "]}";
        inspectedVendors[Lower(actor->GetName())] = std::move(catalog);
        PublishVendorEvent(actor, "vendor_catalog", out.str(), "action_id=" + actionId + " npc=" + npc->GetName());
        return true;
    }

    void PublishVendorPurchaseFailure(Player* actor, Creature* npc, uint64 targetGuid,
        std::string const& actionId, VendorBuyRequest const& request, std::string const& reason)
    {
        std::ostringstream out;
        out << '{';
        AppendVendorIdentity(out, actor, npc, actionId, targetGuid);
        out << ",\"entry\":" << request.itemEntry << ",\"quantity_units\":" << request.quantity
            << ",\"status\":\"failed\",\"reason_code\":\"" << JsonEscape(reason) << "\"}";
        PublishVendorEvent(actor, "vendor_purchase", out.str(), "action_id=" + actionId + " " + reason);
    }

    bool BuyFromVendor(Player* actor, Creature* npc, std::string const& actionId,
        VendorBuyRequest const& request, std::string& reason)
    {
        auto reject = [&](char const* why)
        {
            reason = why;
            PublishVendorPurchaseFailure(actor, npc, npc->GetGUID().GetRawValue(), actionId, request, reason);
            return false;
        };
        auto found = inspectedVendors.find(Lower(actor->GetName()));
        if (found == inspectedVendors.end() || found->second.actorGuid != actor->GetGUID().GetRawValue()
            || found->second.npcGuid != npc->GetGUID().GetRawValue()
            || found->second.expires <= std::chrono::steady_clock::now())
            return reject("vendor_catalog_required");
        // Copy: refreshing a native list may invoke scripts. A catalog is bound to both
        // the actual creature GUID and the alternate inventory selected for that creature.
        InspectedVendor const catalog = found->second;
        InspectedVendorItem const* selected = nullptr;
        for (InspectedVendorItem const& candidate : catalog.items)
            if (candidate.entry == request.itemEntry)
            {
                if (selected)
                    return reject("vendor_item_ambiguous");
                selected = &candidate;
            }
        if (!selected)
            return reject("vendor_item_not_in_catalog");
        VendorItemData const* items = OpenVendorInventory(actor, npc);
        if (!items || actor->GetSession()->GetCurrentVendor() != catalog.vendorEntry)
            return reject("vendor_catalog_changed");
        VendorItem const* item = items->GetItem(selected->slot);
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (!IsVisibleVendorItem(actor, npc, item, proto) || item->item != selected->entry
            || proto->BuyCount != selected->buyCount || proto->BuyPrice != selected->buyPrice
            || item->ExtendedCost != selected->extendedCost)
            return reject("vendor_catalog_changed");
        if (!proto->BuyCount || request.quantity % proto->BuyCount)
            return reject("vendor_quantity_not_bundle_multiple");
        uint32 const bundles = request.quantity / proto->BuyCount;
        if (!bundles || bundles > std::numeric_limits<uint8>::max())
            return reject("vendor_quantity_out_of_range");
        if (item->ExtendedCost && !request.allowExtendedCost)
            return reject("vendor_extended_cost_not_allowed");
        // Native core clamps overflowing gold purchases. Reject instead, preserving the
        // exact requested quantity, and use native TOTAL rounding (not rounded unit price).
        if (proto->BuyPrice < 0)
            return reject("vendor_invalid_price");
        uint64 const rawGold = item->IsGoldRequired(proto) ? uint64(proto->BuyPrice) * bundles : 0;
        if (rawGold > MAX_MONEY_AMOUNT)
            return reject("vendor_price_overflow");
        uint32 const price = uint32(std::floor(uint32(rawGold) * actor->GetReputationPriceDiscount(npc)));
        if (price > request.maxTotalCopper)
            return reject("vendor_max_cost_exceeded");
        if (!actor->HasEnoughMoney(price))
            return reject("vendor_not_enough_money");
        if (item->maxcount && npc->GetVendorItemCurrentCount(item) < request.quantity)
            return reject("vendor_insufficient_stock");
        ItemExtendedCostEntry const* cost = item->ExtendedCost ? sItemExtendedCostStore.LookupEntry(item->ExtendedCost) : nullptr;
        if (item->ExtendedCost && !cost)
            return reject("vendor_invalid_extended_cost");
        uint64 const honorCost = cost ? uint64(cost->reqhonorpoints) * bundles : 0;
        uint64 const arenaCost = cost ? uint64(cost->reqarenapoints) * bundles : 0;
        if (honorCost > uint64(std::numeric_limits<int32>::max()) || arenaCost > uint64(std::numeric_limits<int32>::max()))
            return reject("vendor_currency_overflow");
        std::map<uint32, uint64> currencyCosts;
        if (cost)
            for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
                if (cost->reqitem[i])
                    currencyCosts[cost->reqitem[i]] += uint64(cost->reqitemcount[i]) * bundles;
        for (auto const& [entry, count] : currencyCosts)
            if (count > std::numeric_limits<uint32>::max() || actor->GetItemCount(entry, false) < count)
                return reject("vendor_missing_currency_items");
        uint32 const moneyBefore = actor->GetMoney();
        uint32 const honorBefore = actor->GetHonorPoints();
        uint32 const arenaBefore = actor->GetArenaPoints();
        std::map<uint32, uint32> countsBefore;
        countsBefore[request.itemEntry] = actor->GetItemCount(request.itemEntry, false);
        for (auto const& [entry, count] : currencyCosts)
            countsBefore[entry] = actor->GetItemCount(entry, false);
        // Native core enforces inventory capacity, uniqueness, reputation, honor, arena,
        // turn-ins and stock. Its bool is a LIMITED-STOCK flag, not a success flag.
        bool const nativeReturn = actor->BuyItemFromVendorSlot(npc->GetGUID(), selected->slot,
            request.itemEntry, static_cast<uint8>(bundles), NULL_BAG, NULL_SLOT);
        bool verified = int64(actor->GetMoney()) == int64(moneyBefore) - price
            && int64(actor->GetHonorPoints()) == int64(honorBefore) - int64(honorCost)
            && int64(actor->GetArenaPoints()) == int64(arenaBefore) - int64(arenaCost);
        bool observedChange = actor->GetMoney() != moneyBefore || actor->GetHonorPoints() != honorBefore
            || actor->GetArenaPoints() != arenaBefore;
        for (auto const& [entry, before] : countsBefore)
        {
            uint32 const after = actor->GetItemCount(entry, false);
            int64 const expected = int64(before) + (entry == request.itemEntry ? request.quantity : 0) - int64(currencyCosts[entry]);
            verified = verified && int64(after) == expected;
            observedChange = observedChange || after != before;
        }
        bool const success = verified && observedChange;
        reason = success ? "vendor_purchase_verified" : "native_purchase_rejected_or_unverified";
        std::ostringstream out;
        out << '{';
        AppendVendorIdentity(out, actor, npc, actionId);
        out << ",\"entry\":" << request.itemEntry << ",\"name\":\"" << JsonEscape(proto->Name1)
            << "\",\"vendor_slot\":" << selected->slot + 1 << ",\"quantity_units\":" << request.quantity
            << ",\"bundles\":" << bundles << ",\"expected_cost_copper\":" << price
            << ",\"money_before\":" << moneyBefore << ",\"money_after\":" << actor->GetMoney()
            << ",\"item_count_before\":" << countsBefore[request.itemEntry]
            << ",\"item_count_after\":" << actor->GetItemCount(request.itemEntry, false)
            << ",\"native_return\":" << (nativeReturn ? "true" : "false")
            << ",\"status\":\"" << (success ? "success" : "failed") << "\",\"reason_code\":\"" << reason << "\"}";
        PublishVendorEvent(actor, "vendor_purchase", out.str(), "action_id=" + actionId + " " + reason);
        return success;
    }

    void PublishVendorTravelUpdate(VendorTravelTask const& task, Player* actor,
        std::string const& status, std::string const& reason, VendorJourneyPlan const* plan = nullptr)
    {
        VendorJourneyLocation const& destination = task.journey->Route().destination;
        std::ostringstream out;
        out << "{\"action_id\":\"" << JsonEscape(task.actionId) << "\",\"character\":\"" << JsonEscape(task.actor)
            << "\",\"status\":\"" << JsonEscape(status) << "\",\"reason_code\":\"" << JsonEscape(reason)
            << "\",\"map_id\":" << destination.mapId << ",\"x\":" << destination.x << ",\"y\":" << destination.y
            << ",\"z\":" << destination.z << ",\"stage\":\"" << VendorJourneyStageName(task.journey->Stage())
            << "\",\"uses_taxi\":" << (task.journey->Route().UsesTaxi() ? "true" : "false")
            << ",\"uses_transport\":" << (task.journey->Route().UsesTransport() ? "true" : "false")
            << ",\"purchase_reserve_copper\":" << task.purchaseReserveCopper
            << ",\"strategy_restore_pending\":"
            << (deferredVendorStrategyRestores.contains(task.actorGuid) ? "true" : "false");
        if (actor && actor->IsInWorld() && actor->GetGUID().GetRawValue() == task.actorGuid)
            out << ",\"actual_map_id\":" << actor->GetMapId() << ",\"actual_x\":" << actor->GetPositionX()
                << ",\"actual_y\":" << actor->GetPositionY() << ",\"actual_z\":" << actor->GetPositionZ();
        if (plan)
        {
            out << ",\"candidates\":[";
            bool first = true;
            for (VendorJourneyRoute const& route : plan->candidates)
            {
                if (!first)
                    out << ',';
                first = false;
                out << "{\"uses_taxi\":" << (route.UsesTaxi() ? "true" : "false")
                    << ",\"uses_transport\":" << (route.UsesTransport() ? "true" : "false")
                    << ",\"estimated_seconds\":" << route.estimatedSeconds
                    << ",\"maximum_fare_copper\":" << route.maximumFareCopper
                    << ",\"flightmaster_entry\":" << route.flightmasterEntry
                    << ",\"flightmaster_spawn_id\":" << route.flightmasterSpawnId << ",\"taxi_nodes\":[";
                for (std::size_t i = 0; i < route.taxiNodes.size(); ++i)
                {
                    if (i)
                        out << ',';
                    out << route.taxiNodes[i];
                }
                out << "],\"transport_legs\":[";
                if (route.transportPlan)
                {
                    bool firstLeg = true;
                    for (VendorTransportLeg const& leg : route.transportPlan->legs)
                    {
                        if (!firstLeg)
                            out << ',';
                        firstLeg = false;
                        char const* kind = leg.kind == VendorTransportLegKind::Walk ? "walk"
                            : leg.kind == VendorTransportLegKind::AreaTrigger ? "area_trigger" : "transport";
                        out << "{\"kind\":\"" << kind << "\",\"entry\":" << leg.entry
                            << ",\"from\":{\"map_id\":" << leg.from.mapId << ",\"x\":" << leg.from.x
                            << ",\"y\":" << leg.from.y << ",\"z\":" << leg.from.z
                            << "},\"to\":{\"map_id\":" << leg.to.mapId << ",\"x\":" << leg.to.x
                            << ",\"y\":" << leg.to.y << ",\"z\":" << leg.to.z
                            << "},\"waypoint_count\":" << leg.waypoints.size() << '}';
                    }
                }
                out << "]}";
            }
            out << ']';
        }
        out << '}';
        PublishVendorJourneyEvent(task.groupId, task.actor, destination.mapId, out.str(),
            "action_id=" + task.actionId + " status=" + status + " reason=" + reason);
    }

    void PauseVendorTravelStrategies(PlayerbotAI* botAI, VendorTravelTask& task)
    {
        // These registered strategies can initiate independent travel or NPC/object
        // approaches. Defensive and combat behavior remains enabled during the trip.
        constexpr char const* movementStrategies[] = {
            "follow", "stay", "travel", "grind", "loot", "gather", "rpg", "new rpg", "quest", "mount"
        };
        std::string disable;
        for (char const* strategy : movementStrategies)
        {
            if (!disable.empty())
                disable += ',';
            disable += std::string("-") + strategy;
        }
        for (BotState state : {BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT, BOT_STATE_DEAD, BOT_STATE_REACTION})
        {
            std::string restore = disable;
            for (char const* strategy : movementStrategies)
                if (botAI->HasStrategy(strategy, state))
                    restore += std::string(",+") + strategy;
            task.restoreStrategies.emplace_back(state, std::move(restore));
            botAI->ChangeStrategy(disable, state);
        }
    }

    bool CanRestoreVendorMovement(Player const* actor)
    {
        return actor && actor->IsInWorld() && !actor->IsBeingTeleported()
            && !actor->GetTransport() && !actor->HasUnitState(UNIT_STATE_IN_FLIGHT)
            && !actor->IsFlying() && !actor->IsFalling();
    }

    void HoldVendorMovementForSafeRestore(PlayerbotAI* botAI)
    {
        if (!botAI)
            return;
        for (BotState state : {BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT, BOT_STATE_DEAD, BOT_STATE_REACTION})
        {
            bool needsHold = !botAI->HasStrategy("stay", state);
            for (char const* strategy : {"follow", "travel", "grind", "loot", "gather", "rpg", "new rpg", "quest", "mount"})
                needsHold = needsHold || botAI->HasStrategy(strategy, state);
            if (needsHold)
                botAI->ChangeStrategy("-follow,+stay,-travel,-grind,-loot,-gather,-rpg,-new rpg,-quest,-mount", state);
        }
    }

    void RestoreVendorTravelStrategies(Player* actor, VendorTravelTask const& task)
    {
        if (!actor || actor->GetGUID().GetRawValue() != task.actorGuid)
            return;
        PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(actor);
        if (!CanRestoreVendorMovement(actor))
        {
            // Cancelling a native trip never detaches a passenger at sea or stops
            // a native flight. Keep its original strategies until normal landing
            // or physical disembarkation, instead of following across the deck.
            deferredVendorStrategyRestores[task.actorGuid] = {
                task.actor, task.groupId, task.actionId, task.actorGuid, task.journey->Route().destination.mapId,
                task.restoreStrategies
            };
            HoldVendorMovementForSafeRestore(botAI);
            return;
        }
        if (botAI)
            for (auto const& [state, strategies] : task.restoreStrategies)
                botAI->ChangeStrategy(strategies, state);
    }

    void UpdateDeferredVendorStrategyRestores()
    {
        for (auto iter = deferredVendorStrategyRestores.begin(); iter != deferredVendorStrategyRestores.end(); )
        {
            DeferredVendorStrategyRestore const& pending = iter->second;
            Player* actor = ObjectAccessor::FindConnectedPlayer(ObjectGuid(pending.actorGuid));
            PlayerbotAI* botAI = actor ? sPlayerbotsMgr.GetPlayerbotAI(actor) : nullptr;
            if (!CanRestoreVendorMovement(actor) || !botAI || !actor->GetSession())
            {
                // Native transport membership updates continue independently of
                // Playerbots movement strategies. Never force a disembark here.
                HoldVendorMovementForSafeRestore(botAI);
                ++iter;
                continue;
            }
            for (auto const& [state, strategies] : pending.restoreStrategies)
                botAI->ChangeStrategy(strategies, state);
            std::string const payload = "{\"action_id\":\"" + JsonEscape(pending.actionId)
                + "\",\"character\":\"" + JsonEscape(pending.actor)
                + "\",\"status\":\"restored\",\"reason_code\":\"journey_strategies_restored\","
                  "\"strategy_restore_pending\":false}";
            PublishVendorJourneyEvent(pending.groupId, pending.actor, actor->GetMapId(), payload,
                "action_id=" + pending.actionId + " journey_strategies_restored");
            iter = deferredVendorStrategyRestores.erase(iter);
        }
    }

    void UpdateVendorTravelTasks()
    {
        auto const now = std::chrono::steady_clock::now();
        for (auto iter = vendorTravelTasks.begin(); iter != vendorTravelTasks.end(); )
        {
            VendorTravelTask& task = *iter->second;
            // Connected lookup retains the same authenticated Player object while
            // native portal/boat handlers briefly remove it from the world map.
            Player* actor = ObjectAccessor::FindConnectedPlayer(ObjectGuid(task.actorGuid));
            Player* requester = ObjectAccessor::FindConnectedPlayer(ObjectGuid(task.requesterGuid));
            PlayerbotAI* botAI = actor ? sPlayerbotsMgr.GetPlayerbotAI(actor) : nullptr;
            bool const ownedTransfer = task.journey->IsNativeTransferPending();
            bool const actorWorldValid = actor
                && ((actor->IsInWorld() && !actor->IsBeingTeleported()) || ownedTransfer);
            bool const valid = actorWorldValid && requester && requester->IsInWorld()
                && actor->GetSession() && actor->GetSession()->GetPlayer() == actor && requester->GetSession()
                && actor->GetGUID().GetRawValue() == task.actorGuid
                && requester->GetGUID().GetRawValue() == task.requesterGuid
                && actor->GetGroup() && actor->GetGroup() == requester->GetGroup()
                && std::to_string(actor->GetGroup()->GetLeaderGUID().GetRawValue()) == task.groupId
                && botAI && botAI->GetMaster() == requester && IsAuthorizedRequester(requester->GetName());
            if (!valid)
            {
                task.journey->Cancel(actor);
                RestoreVendorTravelStrategies(actor, task);
                PublishVendorTravelUpdate(task, actor, "failed", "journey_context_changed");
                iter = vendorTravelTasks.erase(iter);
                continue;
            }
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - task.lastTick).count();
            task.lastTick = now;
            uint32 const elapsedMs = static_cast<uint32>(std::clamp<int64>(elapsed, 0, std::numeric_limits<uint32>::max()));
            VendorJourneyStage const stage = task.journey->Tick(actor, elapsedMs);
            task.taxiDeparted = task.taxiDeparted || stage == VendorJourneyStage::Flying;
            std::string const& reason = task.journey->Reason();
            bool const taxiDepartureFailure = reason.find("journey_taxi_") == 0
                || reason.find("journey_flightmaster_") == 0 || reason == "journey_route_stalled";
            if (stage == VendorJourneyStage::Failed && task.journey->Route().UsesTaxi() && !task.taxiDeparted
                && !task.fallbackTried && task.hasWalkingFallback && taxiDepartureFailure)
            {
                task.fallbackTried = true;
                task.journey = std::make_unique<VendorJourney>(task.walkingFallback, task.purchaseReserveCopper);
                task.lastPublishedStage = task.journey->Stage();
                PublishVendorTravelUpdate(task, actor, "routing", "journey_walking_fallback");
                ++iter;
                continue;
            }
            if (!task.journey->IsTerminal() && stage != task.lastPublishedStage)
            {
                task.lastPublishedStage = stage;
                PublishVendorTravelUpdate(task, actor, "routing", reason);
            }
            if (task.journey->IsTerminal())
            {
                RestoreVendorTravelStrategies(actor, task);
                PublishVendorTravelUpdate(task, actor,
                    stage == VendorJourneyStage::Arrived ? "arrived" : "failed", reason);
                iter = vendorTravelTasks.erase(iter);
                continue;
            }
            ++iter;
        }
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

std::vector<Item*> FindTradeableStacks(Player* actor, Player* requester, uint32 entry,
    std::unordered_set<uint64> const* allowedItemGuids = nullptr)
{
    std::vector<Item*> stacks;
    auto append = [&](Item* item)
    {
        if (item && item->GetEntry() == entry && !item->IsInTrade()
            && (!allowedItemGuids || allowedItemGuids->contains(item->GetGUID().GetRawValue()))
            && item->CanBeTraded(false, true) && !item->IsBindedNotWith(requester))
            stacks.push_back(item);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        append(actor->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = actor->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                append(actor->GetItemByPos(bagSlot, static_cast<uint8>(slot)));

    return stacks;
}

bool BuildTradeOfferPlan(Player* actor, Player* requester,
    std::vector<ItemOfferRequest> const& requests, std::vector<Item*>& items,
    std::vector<uint8>& slots, std::string& reason,
    std::unordered_set<uint64> const* allowedItemGuids = nullptr)
{
    for (uint8 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
    {
        Item* offered = actor->GetTradeData()
            ? actor->GetTradeData()->GetItem(TradeSlots(slot)) : nullptr;
        if (!offered)
            slots.push_back(slot);
    }

    for (ItemOfferRequest const& request : requests)
    {
        if (allowedItemGuids)
        {
            std::vector<Item*> available = FindTradeableStacks(actor, requester, request.itemEntry, allowedItemGuids);
            if (available.size() < request.stackCount)
            {
                reason = "item_not_tradeable_or_insufficient_stacks";
                return false;
            }
            items.insert(items.end(), available.begin(), available.begin() + request.stackCount);
            continue;
        }

        uint32 alreadyOffered = 0;
        if (actor->GetTradeData())
            for (uint8 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
                if (Item* offered = actor->GetTradeData()->GetItem(TradeSlots(slot));
                    offered && offered->GetEntry() == request.itemEntry)
                    ++alreadyOffered;
        if (alreadyOffered > request.stackCount)
        {
            reason = "trade_item_already_overoffered";
            return false;
        }

        uint32 const stacksToAdd = request.stackCount - alreadyOffered;
        std::vector<Item*> available = FindTradeableStacks(actor, requester, request.itemEntry, allowedItemGuids);
        if (available.size() < stacksToAdd)
        {
            reason = "item_not_tradeable_or_insufficient_stacks";
            return false;
        }
        items.insert(items.end(), available.begin(), available.begin() + stacksToAdd);
    }

    if (slots.size() < items.size())
    {
        reason = "trade_slots_full";
        return false;
    }
    if (items.empty())
        reason = "items_already_offered";
    return true;
}

std::string OfferItemStacks(Player* actor, Player* requester,
    std::vector<ItemOfferRequest> const& requests,
    std::unordered_set<uint64> const* allowedItemGuids = nullptr)
{
    if (!actor->GetTradeData() || !requester->GetTradeData()
        || actor->GetTrader() != requester || requester->GetTrader() != actor)
        return "trade_not_open";

    std::vector<Item*> items;
    std::vector<uint8> slots;
    std::string reason;
    if (!BuildTradeOfferPlan(actor, requester, requests, items, slots, reason, allowedItemGuids))
        return reason;
    if (items.empty())
        return reason;

    std::vector<uint8> placedSlots;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        Item* item = items[i];
        uint8 const tradeSlot = slots[i];
        WorldPacket packet(CMSG_SET_TRADE_ITEM, 3);
        packet << tradeSlot;
        packet << item->GetBagSlot();
        packet << item->GetSlot();
        actor->GetSession()->HandleSetTradeItemOpcode(packet);
        TradeData* tradeData = actor->GetTradeData();
        if (!tradeData || tradeData->GetItem(TradeSlots(tradeSlot)) != item)
        {
            for (uint8 placedSlot : placedSlots)
            {
                if (!actor->GetTradeData())
                    break;
                WorldPacket clearPacket(CMSG_CLEAR_TRADE_ITEM, 1);
                clearPacket << placedSlot;
                actor->GetSession()->HandleClearTradeItemOpcode(clearPacket);
            }
            return "trade_offer_rejected";
        }
        placedSlots.push_back(tradeSlot);
    }

    return "items_offered";
}

namespace
{
    std::string StageAgentHuntLoot(AgentHuntTask& task, Player* actor, Player* requester)
    {
        if (!actor || !requester || !actor->GetSession())
            return "trade_unavailable";

        bool const tradeAlreadyOpen = actor->GetTradeData() || requester->GetTradeData();
        if (tradeAlreadyOpen && (actor->GetTrader() != requester || requester->GetTrader() != actor))
            return "trade_busy";

        std::unordered_set<uint64> allowedItemGuids;
        std::vector<ItemOfferRequest> requests;
        std::map<uint32, uint32> remainingLoot = task.lootedItems;
        uint32 offeredStacks = 0;
        uint32 offeredItems = 0;
        for (auto const& [itemGuid, lootedCount] : task.lootedItemStackCounts)
        {
            if (!lootedCount || offeredStacks >= MAX_HUNT_LOOT_STACKS)
                continue;

            Item* item = FindBagItemByGuid(actor, itemGuid);
            if (!item || !item->CanBeTraded(false, true) || item->IsBindedNotWith(requester))
                continue;

            uint32 const entry = item->GetEntry();
            uint32 const taskLootRemaining = remainingLoot[entry];
            if (!taskLootRemaining)
                continue;

            uint32 availableCount = item->GetCount();
            auto baseline = task.existingItemCounts.find(itemGuid);
            if (baseline != task.existingItemCounts.end())
                availableCount = availableCount > baseline->second ? availableCount - baseline->second : 0;

            uint32 const countToOffer = std::min({lootedCount, taskLootRemaining, availableCount});
            if (!countToOffer)
                continue;

            if (countToOffer < item->GetCount())
            {
                item = SplitTaskLootStack(actor, item, countToOffer);
                if (!item || !item->CanBeTraded(false, true) || item->IsBindedNotWith(requester))
                    continue;
            }

            uint64 const stagedGuid = item->GetGUID().GetRawValue();
            allowedItemGuids.insert(stagedGuid);
            auto request = std::find_if(requests.begin(), requests.end(), [entry](ItemOfferRequest const& itemRequest)
            {
                return itemRequest.itemEntry == entry;
            });
            if (request == requests.end())
                requests.push_back({entry, 1});
            else
                ++request->stackCount;
            ++offeredStacks;
            offeredItems += countToOffer;
            remainingLoot[entry] -= countToOffer;
        }

        if (requests.empty())
            return "no_new_tradeable_loot";

        std::vector<Item*> plannedItems;
        std::vector<uint8> plannedSlots;
        std::string planReason;
        if (!BuildTradeOfferPlan(actor, requester, requests, plannedItems, plannedSlots, planReason, &allowedItemGuids))
            return planReason;

        if (!tradeAlreadyOpen)
        {
            WorldPacket initiatePacket(CMSG_INITIATE_TRADE);
            initiatePacket << requester->GetGUID();
            actor->GetSession()->HandleInitiateTradeOpcode(initiatePacket);
        }

        std::string const offerResult = OfferItemStacks(actor, requester, requests, &allowedItemGuids);
        if (offerResult != "items_offered" && offerResult != "items_already_offered")
            return offerResult;

        task.stagedStacks = offeredStacks;
        task.stagedItems = offeredItems;
        return "items_staged";
    }

    void RestoreAgentHuntStrategies(PlayerbotAI* botAI, AgentHuntTask const& task)
    {
        if (botAI)
        {
            auto& alwaysLoot = botAI->GetAiObjectContext()->GetValue<std::set<uint32>&>("always loot list")->Get();
            for (uint32 entry : task.addedLootEntries)
                alwaysLoot.erase(entry);
            std::string strategies = "-grind,-travel,-loot,-follow,-stay";
            if (task.hadGrindStrategy)
                strategies += ",+grind";
            if (task.hadTravelStrategy)
                strategies += ",+travel";
            if (task.hadLootStrategy)
                strategies += ",+loot";
            if (task.hadFollowStrategy)
                strategies += ",+follow";
            if (task.hadStayStrategy)
                strategies += ",+stay";
            botAI->ChangeStrategy(strategies, BOT_STATE_NON_COMBAT);
        }
    }

    void ResetAgentHuntTravel(Player* actor, PlayerbotAI* botAI, AgentHuntTask const& task, bool returning)
    {
        if (botAI)
            botAI->SetTaskHuntTarget(0, 0, 0);
        TravelMgr::instance().setNullTravelTarget(actor);
        if (botAI)
        {
            if (returning)
                botAI->ChangeStrategy("-grind,-travel,-loot,+follow,-stay", BOT_STATE_NON_COMBAT);
            else
                RestoreAgentHuntStrategies(botAI, task);
        }
    }

    void RestoreAgentGuideStrategies(PlayerbotAI* botAI, AgentGuideTask const& task)
    {
        if (!botAI)
            return;

        std::string strategies = "-grind,-travel,-loot,-follow,-stay";
        if (task.hadGrindStrategy)
            strategies += ",+grind";
        if (task.hadTravelStrategy)
            strategies += ",+travel";
        if (task.hadLootStrategy)
            strategies += ",+loot";
        if (task.hadFollowStrategy)
            strategies += ",+follow";
        if (task.hadStayStrategy)
            strategies += ",+stay";
        botAI->ChangeStrategy(strategies, BOT_STATE_NON_COMBAT);
    }

    void ResetAgentGuideTravel(Player* actor, PlayerbotAI* botAI, AgentGuideTask const& task, bool waitForUser)
    {
        TravelTarget* travelTarget = botAI
            ? botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get() : nullptr;
        bool const ownsTravelTarget = travelTarget && task.destination
            && travelTarget->getDestination() == task.destination.get();
        if (actor && ownsTravelTarget)
            TravelMgr::instance().setNullTravelTarget(actor);
        if (!botAI)
            return;

        if (waitForUser)
            botAI->ChangeStrategy("-grind,-travel,-loot,-follow,+stay", BOT_STATE_NON_COMBAT);
        else
            RestoreAgentGuideStrategies(botAI, task);
    }

    void PublishAgentGuideUpdate(AgentGuideTask const& task, std::string const& event, std::string const& reason)
    {
        std::ostringstream out;
        out << "{\"event\":\"" << JsonEscape(event)
            << "\",\"actor\":\"" << JsonEscape(task.actor)
            << "\",\"requester\":\"" << JsonEscape(task.requester)
            << "\",\"stage\":\"" << AgentGuideStageName(task.stage)
            << "\",\"target\":\"" << JsonEscape(task.targetName)
            << "\",\"zone\":\"" << JsonEscape(task.zoneName)
            << "\",\"map_id\":" << task.mapId
            << ",\"zone_id\":" << task.zoneId
            << ",\"area_id\":" << task.areaId
            << ",\"reason\":\"" << JsonEscape(reason) << "\"}";
        GetWorldStateWriter().PublishChatLine(task.actor, "TASK", "WorldServer", out.str());
    }

    bool MoveAgentHuntToNextSpawn(AgentHuntTask& task, Player* actor, PlayerbotAI* botAI)
    {
        if (!task.destination || !actor || !botAI)
            return false;

        TravelTarget* travelTarget = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (!travelTarget)
            return false;

        std::vector<WorldPosition*> points = task.destination->getPoints(true);
        if (points.empty())
            return false;

        std::vector<WorldPosition*> candidates;
        std::copy_if(points.begin(), points.end(), std::back_inserter(candidates), [&task](WorldPosition* point)
        {
            return point && !task.visitedSpawnPoints.contains(point);
        });
        if (candidates.empty())
        {
            task.visitedSpawnPoints.clear();
            candidates = points;
            if (candidates.size() > 1)
                std::erase(candidates, task.currentSpawnPoint);
        }
        if (candidates.empty())
            return false;

        WorldPosition actorLocation(actor);
        std::sort(candidates.begin(), candidates.end(), [&actorLocation](WorldPosition* left, WorldPosition* right)
        {
            return TravelMgr::instance().mapTransDistance(actorLocation, *left)
                < TravelMgr::instance().mapTransDistance(actorLocation, *right);
        });
        task.currentSpawnPoint = candidates.front();
        task.visitedSpawnPoints.insert(task.currentSpawnPoint);
        travelTarget->setTarget(task.destination.get(), task.currentSpawnPoint);
        return true;
    }

    void PauseAgentHuntForLoot(Player* actor, PlayerbotAI* botAI)
    {
        if (actor)
            TravelMgr::instance().setNullTravelTarget(actor);
        if (!botAI)
            return;

        botAI->SetTaskHuntTarget(0, 0, 0);
        botAI->ChangeStrategy("-grind,-travel,-follow,-stay,-loot", BOT_STATE_NON_COMBAT);
    }

    bool ResumeAgentHuntAfterLoot(AgentHuntTask& task, Player* actor, PlayerbotAI* botAI)
    {
        if (!actor || !botAI || !task.destination)
            return false;

        botAI->SetTaskHuntTargets(task.creatureEntries, task.mapId, task.zoneId, task.areaId);
        botAI->ChangeStrategy("+travel,-grind,-loot,-follow,-stay", BOT_STATE_NON_COMBAT);
        return MoveAgentHuntToNextSpawn(task, actor, botAI);
    }

    void UpdateAgentGuideTasks()
    {
        std::lock_guard<std::mutex> lock(agentHuntMutex);
        auto const now = std::chrono::steady_clock::now();
        for (auto& [key, taskPointer] : agentGuideTasks)
        {
            static_cast<void>(key);
            AgentGuideTask& task = *taskPointer;
            if (!IsAgentGuideActive(task))
                continue;

            Player* actor = ObjectAccessor::FindPlayerByName(task.actor);
            Player* requester = ObjectAccessor::FindPlayerByName(task.requester);
            PlayerbotAI* botAI = actor && actor->IsInWorld() ? sPlayerbotsMgr.GetPlayerbotAI(actor) : nullptr;
            auto fail = [&](char const* reason)
            {
                ResetAgentGuideTravel(actor, botAI, task, false);
                task.stage = AgentGuideStage::Failed;
                task.destination.reset();
                if (requester && requester->IsInWorld())
                    PublishAgentGuideUpdate(task, "guide_failed", reason);
            };

            if (now - task.startedAt > GUIDE_TIMEOUT)
            {
                fail("task_timeout");
                continue;
            }
            if (!actor || !actor->IsInWorld() || !requester || !requester->IsInWorld())
                continue;
            if (!botAI || actor->GetGroup() != requester->GetGroup() || botAI->GetMaster() != requester)
            {
                fail("group_or_master_changed");
                continue;
            }

            if (task.stage == AgentGuideStage::Traveling)
            {
                TravelTarget* travelTarget = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                if (!travelTarget)
                {
                    fail("travel_unavailable");
                    continue;
                }
                if (!task.destination || travelTarget->getDestination() != task.destination.get())
                {
                    fail("route_interrupted");
                    continue;
                }

                if (travelTarget->getStatus() == TRAVEL_STATUS_WORK)
                {
                    ResetAgentGuideTravel(actor, botAI, task, true);
                    task.stage = AgentGuideStage::WaitingForUser;
                    PublishAgentGuideUpdate(task, "guide_waiting", "target_reached");
                }
                else if (travelTarget->getStatus() == TRAVEL_STATUS_EXPIRED
                    || travelTarget->getStatus() == TRAVEL_STATUS_COOLDOWN
                    || travelTarget->getStatus() == TRAVEL_STATUS_NONE)
                {
                    fail("route_failed");
                    continue;
                }
            }

            if (task.stage == AgentGuideStage::WaitingForUser
                && IsSameWorldPhase(actor, requester)
                && actor->IsWithinDistInMap(requester, GUIDE_ARRIVAL_DISTANCE, false))
            {
                RestoreAgentGuideStrategies(botAI, task);
                task.stage = AgentGuideStage::Arrived;
                task.destination.reset();
                PublishAgentGuideUpdate(task, "guide_arrived", "user_caught_up");
            }
        }
    }

    void PublishAgentHuntUpdate(AgentHuntTask const& task, std::string const& event, std::string const& reason)
    {
        std::ostringstream out;
        out << "{\"event\":\"" << JsonEscape(event)
            << "\",\"actor\":\"" << JsonEscape(task.actor)
            << "\",\"requester\":\"" << JsonEscape(task.requester)
            << "\",\"stage\":\"" << AgentHuntStageName(task.stage)
            << "\",\"target\":\"" << JsonEscape(task.targetName)
            << "\",\"target_entry\":" << task.creatureEntry
            << ",\"target_entries\":[";
        for (std::size_t i = 0; i < task.creatureEntries.size(); ++i)
        {
            if (i)
                out << ',';
            out << task.creatureEntries[i];
        }
        out << "],\"zone\":\"" << JsonEscape(task.zoneName)
            << "\",\"area_id\":" << task.areaId
            << ",\"kills\":" << task.kills
            << ",\"kills_requested\":" << task.requiredKills
            << ",\"collect_loot\":" << (task.collectLoot ? "true" : "false")
            << ",\"deliver_loot\":" << (task.deliverLoot ? "true" : "false")
            << ",\"looted_items\":[";
        bool first = true;
        for (auto const& [entry, count] : task.lootedItems)
        {
            if (!first)
                out << ',';
            first = false;
            out << "{\"entry\":" << entry << ",\"count\":" << count << '}';
        }
        out << "],\"staged_stacks\":" << task.stagedStacks
            << ",\"staged_items\":" << task.stagedItems
            << ",\"reason\":\"" << JsonEscape(reason) << "\"}";
        GetWorldStateWriter().PublishChatLine(task.actor, "TASK", "WorldServer", out.str());
    }

    void UpdateAgentHuntTasks()
    {
        std::lock_guard<std::mutex> lock(agentHuntMutex);
        auto const now = std::chrono::steady_clock::now();
        for (auto& [key, taskPointer] : agentHuntTasks)
        {
            static_cast<void>(key);
            AgentHuntTask& task = *taskPointer;
            if (!IsAgentHuntActive(task))
                continue;

            Player* actor = ObjectAccessor::FindPlayerByName(task.actor);
            Player* requester = ObjectAccessor::FindPlayerByName(task.requester);
            if (now - task.startedAt > HUNT_TIMEOUT)
            {
                PlayerbotAI* botAI = actor && actor->IsInWorld() ? sPlayerbotsMgr.GetPlayerbotAI(actor) : nullptr;
                if (actor && actor->IsInWorld())
                    ResetAgentHuntTravel(actor, botAI, task, false);
                task.stage = AgentHuntStage::Failed;
                task.destination.reset();
                if (requester && requester->IsInWorld())
                    PublishAgentHuntUpdate(task, "hunt_failed", "task_timeout");
                continue;
            }

            if (!actor || !actor->IsInWorld() || !requester || !requester->IsInWorld())
                continue;

            PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(actor);
            if (!botAI || actor->GetGroup() != requester->GetGroup() || botAI->GetMaster() != requester)
            {
                ResetAgentHuntTravel(actor, botAI, task, false);
                task.stage = AgentHuntStage::Failed;
                task.destination.reset();
                PublishAgentHuntUpdate(task, "hunt_failed", "group_or_master_changed");
                continue;
            }

            if (actor->IsInCombat() || !actor->IsAlive())
            {
                task.lastRouteProgress = now;
                continue;
            }

            if (task.stage == AgentHuntStage::Hunting)
            {
                std::list<Creature*> candidates;
                actor->GetCreatureListWithEntryInGrid(candidates, task.creatureEntries, 120.0f);
                candidates.remove_if([&](Creature* target)
                {
                    return !target || !target->IsAlive() || !IsSameWorldPhase(actor, target)
                        || !actor->IsValidAttackTarget(target)
                        || !botAI->IsTaskHuntTarget(target->GetEntry(), target->GetMapId(),
                            target->GetZoneId(), target->GetAreaId());
                });
                candidates.sort([actor](Creature* left, Creature* right)
                {
                    return actor->GetDistance(left) < actor->GetDistance(right);
                });
                if (!candidates.empty())
                {
                    Creature* target = candidates.front();
                    if (actor->IsWithinLOSInMap(target))
                    {
                        BridgeAttackAction attack(botAI);
                        if (attack.Start(target))
                        {
                            task.lastRouteProgress = now;
                            continue;
                        }
                    }
                    if (!actor->isMoving())
                    {
                        BridgeMovementAction movement(botAI);
                        movement.Approach(target);
                    }
                }
                TravelTarget* travelTarget = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                bool const activeRoute = travelTarget && travelTarget->isActive();
                if (actor->isMoving())
                    task.lastRouteProgress = now;
                if (!activeRoute || travelTarget->getStatus() == TRAVEL_STATUS_WORK
                    || now - task.lastRouteProgress > std::chrono::seconds(10))
                {
                    task.lastRouteProgress = now;
                    if (!MoveAgentHuntToNextSpawn(task, actor, botAI))
                    {
                        ResetAgentHuntTravel(actor, botAI, task, false);
                        task.stage = AgentHuntStage::Failed;
                        task.destination.reset();
                        PublishAgentHuntUpdate(task, "hunt_failed", "route_failed");
                    }
                }
                continue;
            }

            if (task.stage == AgentHuntStage::Looting)
            {
                if (now - task.lastTargetKill < std::chrono::seconds(1))
                    continue;
                Creature* corpse = nullptr;
                for (auto it = task.pendingLootCorpses.begin(); it != task.pendingLootCorpses.end();)
                {
                    Creature* candidate = ObjectAccessor::GetCreature(*actor, ObjectGuid(*it));
                    if (!candidate || !candidate->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
                        it = task.pendingLootCorpses.erase(it);
                    else
                    {
                        if (!corpse || actor->GetDistance(candidate) < actor->GetDistance(corpse))
                            corpse = candidate;
                        ++it;
                    }
                }
                if (corpse)
                {
                    if (now - task.lastTargetKill > std::chrono::seconds(60))
                    {
                        ResetAgentHuntTravel(actor, botAI, task, false);
                        task.stage = AgentHuntStage::Failed;
                        task.destination.reset();
                        PublishAgentHuntUpdate(task, "hunt_failed", "corpse_loot_blocked");
                        continue;
                    }
                    if (!actor->IsWithinDistInMap(corpse, INTERACTION_DISTANCE - 2.0f)
                        || !actor->IsWithinLOSInMap(corpse))
                    {
                        if (!actor->isMoving())
                        {
                            BridgeMovementAction movement(botAI);
                            movement.Approach(corpse);
                        }
                        continue;
                    }
                    AiObjectContext* context = botAI->GetAiObjectContext();
                    auto& alwaysLoot = context->GetValue<std::set<uint32>&>("always loot list")->Get();
                    for (LootItem const& item : corpse->loot.items)
                        if (alwaysLoot.insert(item.itemid).second)
                            task.addedLootEntries.insert(item.itemid);
                    context->GetValue<LootObject>("loot target")->Set(LootObject(actor, corpse->GetGUID()));
                    OpenLootAction open(botAI);
                    open.Execute(Event());
                    continue;
                }
                if (actor->GetLootGUID()
                    || botAI->GetAiObjectContext()->GetValue<LootObjectStack*>("available loot")->Get()->IsLootPending())
                    continue;
                if (task.kills < task.requiredKills)
                {
                    task.stage = AgentHuntStage::Hunting;
                    task.lastRouteProgress = now;
                    if (!ResumeAgentHuntAfterLoot(task, actor, botAI))
                    {
                        ResetAgentHuntTravel(actor, botAI, task, false);
                        task.stage = AgentHuntStage::Failed;
                        task.destination.reset();
                        PublishAgentHuntUpdate(task, "hunt_failed", "route_failed_after_loot");
                    }
                    continue;
                }
                if (!task.deliverLoot)
                {
                    ResetAgentHuntTravel(actor, botAI, task, false);
                    task.stage = AgentHuntStage::Complete;
                    task.destination.reset();
                    PublishAgentHuntUpdate(task, "hunt_completed", "loot_kept_in_bags");
                    continue;
                }
                ResetAgentHuntTravel(actor, botAI, task, true);
                task.stage = AgentHuntStage::Returning;
            }

            if (task.stage != AgentHuntStage::Returning || actor->IsInCombat() || requester->IsInCombat()
                || !IsSameWorldPhase(actor, requester))
                continue;

            float const returnDistance = task.deliverLoot ? TRADE_DISTANCE : 35.0f;
            if (!actor->IsWithinDistInMap(requester, returnDistance, false))
                continue;

            std::string reason = task.deliverLoot ? StageAgentHuntLoot(task, actor, requester) : "hunt_complete";
            task.stage = AgentHuntStage::Complete;
            task.targetCorpseGuids.clear();
            task.destination.reset();
            RestoreAgentHuntStrategies(botAI, task);
            PublishAgentHuntUpdate(task, "hunt_completed", reason);
        }
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
    std::string const& spellToken, std::string const& targetToken, std::string const& optionToken,
    std::string const& itemOfferToken, std::string const& taskToken)
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
    std::vector<ItemOfferRequest> itemOffers;
    ItemLocationRequest itemLocation;
    HuntRequest huntRequest;
    GuideRequest guideRequest;
    VendorBuyRequest vendorBuyRequest;
    VendorTravelRequest vendorTravelRequest;
    bool const isOfferItems = actionType == "offer_items";
    bool const isUseItem = actionType == "use_item";
    bool const isEquipItem = actionType == "equip_item";
    bool const isItemAction = isUseItem || isEquipItem;
    bool const isHuntStart = actionType == "hunt_start";
    bool const isHuntCancel = actionType == "hunt_cancel";
    bool const isGuideStart = actionType == "guide_start";
    bool const isGuideCancel = actionType == "guide_cancel";
    bool const isVendorTravel = actionType == "vendor_travel";
    bool const isVendorTravelCancel = actionType == "vendor_travel_cancel";
    bool const isVendorBuy = actionType == "vendor_buy";
    bool const isVendor = isVendorBuy || actionType == "vendor_inspect";
    bool const isPlayerbotAction = actionType == "playerbot_action";
    bool const isTrainerAction = actionType == "trainer_action";
    std::string playerbotActionName;
    std::string playerbotQualifier;
    bool const validPlayerbotAction = !isPlayerbotAction
        || ParsePlayerbotActionToken(taskToken, playerbotActionName, playerbotQualifier);
    bool const validItemToken = isOfferItems ? ParseItemOfferBundle(itemOfferToken, itemOffers)
        : isItemAction ? ParseItemLocation(itemOfferToken, itemLocation) : itemOfferToken == "-";
    if (!ParseInteger(spellToken, spellId) || !ParseInteger(targetToken, targetGuid)
        || !ParseInteger(optionToken, optionIndex) || !validItemToken)
    {
        result.reasonCode = "invalid_action_arguments";
        return Finish(nullptr, std::move(result), false);
    }
    if ((isVendorTravel && !ParseVendorTravelRequest(taskToken, vendorTravelRequest))
        || (isVendorBuy && !ParseVendorBuyRequest(taskToken, vendorBuyRequest))
        || (isHuntStart && !ParseHuntRequest(taskToken, huntRequest))
        || (isHuntCancel && taskToken != "cancel")
        || (isGuideStart && !ParseGuideRequest(taskToken, guideRequest))
        || (isGuideCancel && taskToken != "cancel")
        || (isTrainerAction && taskToken != "inspect" && taskToken != "learn")
        || (!isHuntStart && !isHuntCancel && !isGuideStart && !isGuideCancel && !isPlayerbotAction && !isVendorBuy && !isVendorTravel
            && !isTrainerAction
            && taskToken != "-")
        || !validPlayerbotAction)
    {
        result.reasonCode = "invalid_action_arguments";
        return Finish(nullptr, std::move(result), false);
    }
    bool const isCast = actionType == "cast_spell";
    bool const isNpcAction = actionType == "interact_npc" || actionType == "gossip_select" || isVendor || isTrainerAction;
    bool const isUnitTargetAction = isCast || isPlayerbotAction;
    bool const isGossipSelect = actionType == "gossip_select";
    if ((isCast ? spellId == 0 : spellId != 0)
        || (isNpcAction ? targetGuid == 0 : (targetGuid != 0 && !isUnitTargetAction))
        || (isGossipSelect ? optionIndex < 0 || optionIndex > 255 : optionIndex != -1)
        || (isOfferItems ? itemOffers.empty() : (!isItemAction && itemOfferToken != "-")))
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
    if (!isVendorTravelCancel && previous != lastActionByActor.end() && now - previous->second < ACTION_RATE_LIMIT)
    {
        result.reasonCode = "rate_limited";
        return Finish(requester, std::move(result), true);
    }

    Player* actor = ObjectAccessor::FindPlayerByName(actorName, !isVendorTravelCancel);
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

    auto transferJourney = vendorTravelTasks.find(Lower(actorName));
    bool const canCancelNativeTransfer = isVendorTravelCancel && actor
        && transferJourney != vendorTravelTasks.end()
        && transferJourney->second->actorGuid == actor->GetGUID().GetRawValue()
        && transferJourney->second->journey->IsNativeTransferPending();
    if (!actor || (!actor->IsInWorld() && !canCancelNativeTransfer))
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

    if (deferredVendorStrategyRestores.contains(actor->GetGUID().GetRawValue()))
    {
        // A follow/generic action arriving immediately after cancellation must
        // not undo the safe passenger hold before the native trip has ended.
        UpdateDeferredVendorStrategyRestores();
        if (deferredVendorStrategyRestores.contains(actor->GetGUID().GetRawValue())
            && !isVendorTravelCancel && actionType != "stay")
        {
            result.reasonCode = "journey_waiting_safe_transfer_end";
            return Finish(requester, std::move(result), true);
        }
    }

    auto existingJourney = vendorTravelTasks.find(Lower(actorName));
    if (existingJourney != vendorTravelTasks.end() && !isVendorTravelCancel)
    {
        result.reasonCode = "vendor_journey_busy";
        return Finish(requester, std::move(result), true);
    }
    if (isVendorTravelCancel)
    {
        auto pending = pendingNpcInteractions.find(Lower(actorName));
        bool const pendingVendor = pending != pendingNpcInteractions.end()
            && (pending->second.type == "vendor_inspect" || pending->second.type == "vendor_buy");
        if ((existingJourney != vendorTravelTasks.end()
                && (existingJourney->second->requesterGuid != requester->GetGUID().GetRawValue()
                    || existingJourney->second->groupId != groupId))
            || (pendingVendor && (Lower(pending->second.requester) != Lower(requesterName)
                || pending->second.groupId != groupId)))
        {
            result.reasonCode = "journey_owner_mismatch";
            return Finish(requester, std::move(result), true);
        }
        if (pendingVendor)
        {
            PendingNpcInteraction const cancelled = pending->second;
            // Clear the delayed executor before publishing any acknowledgement: a
            // cancelled approach must never later reach BuyItemFromVendorSlot.
            pendingNpcInteractions.erase(pending);
            ActionResult const cancelledResult = MakeResult(cancelled.actionId, cancelled.actor,
                cancelled.type, "rejected", "vendor_task_cancelled");
            RememberResult(cancelledResult);
            PublishActionResultEvent(requester, cancelled.actor, cancelled.actionId, cancelled.type,
                "rejected", "vendor_task_cancelled");
            if (cancelled.type == "vendor_buy")
            {
                VendorBuyRequest request;
                if (ParseVendorBuyRequest(cancelled.taskToken, request))
                    PublishVendorPurchaseFailure(actor, nullptr, cancelled.targetGuid, cancelled.actionId,
                        request, "vendor_task_cancelled");
            }
            if (!actor->HasUnitState(UNIT_STATE_IN_FLIGHT))
            {
                actor->StopMoving();
                actor->GetMotionMaster()->Clear();
            }
        }
        bool const hadJourney = existingJourney != vendorTravelTasks.end();
        if (hadJourney)
        {
            VendorTravelTask& task = *existingJourney->second;
            task.journey->Cancel(actor);
            RestoreVendorTravelStrategies(actor, task);
            PublishVendorTravelUpdate(task, actor, "failed", "journey_cancelled");
            vendorTravelTasks.erase(existingJourney);
        }
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = hadJourney ? "vendor_journey_cancelled"
            : pendingVendor ? "vendor_approach_cancelled" : "vendor_journey_already_idle";
        return Finish(requester, std::move(result), true);
    }
    if (isVendorTravel)
    {
        if (!actor->IsAlive() || !actor->GetSession() || !requester->GetSession())
        {
            result.reasonCode = "journey_actor_unavailable";
            return Finish(requester, std::move(result), true);
        }
        if (actor->HasUnitState(UNIT_STATE_IN_FLIGHT) || actor->IsFlying())
        {
            result.reasonCode = "journey_world_ground_route_required";
            return Finish(requester, std::move(result), true);
        }
        if (pendingNpcInteractions.find(Lower(actorName)) != pendingNpcInteractions.end())
        {
            result.reasonCode = "npc_approach_busy";
            return Finish(requester, std::move(result), true);
        }
        {
            std::lock_guard<std::mutex> lock(agentHuntMutex);
            auto hunt = agentHuntTasks.find(Lower(actorName));
            auto guide = agentGuideTasks.find(Lower(actorName));
            if ((hunt != agentHuntTasks.end() && IsAgentHuntActive(*hunt->second))
                || (guide != agentGuideTasks.end() && IsAgentGuideActive(*guide->second)))
            {
                result.reasonCode = "actor_task_busy";
                return Finish(requester, std::move(result), true);
            }
        }
        VendorJourneyPlan const plan = PlanVendorJourney(actor, vendorTravelRequest.destination,
            vendorTravelRequest.purchaseReserveCopper);
        if (plan.candidates.empty())
        {
            result.reasonCode = plan.reasonCode;
            return Finish(requester, std::move(result), true);
        }
        auto task = std::make_unique<VendorTravelTask>();
        task->actor = actorName;
        task->requester = requesterName;
        task->groupId = groupId;
        task->actionId = actionId;
        task->actorGuid = actor->GetGUID().GetRawValue();
        task->requesterGuid = requester->GetGUID().GetRawValue();
        task->purchaseReserveCopper = vendorTravelRequest.purchaseReserveCopper;
        task->lastTick = now;
        task->journey = std::make_unique<VendorJourney>(plan.candidates.front(), task->purchaseReserveCopper);
        task->lastPublishedStage = task->journey->Stage();
        for (VendorJourneyRoute const& route : plan.candidates)
            if (!route.UsesTaxi() && !route.UsesTransport())
            {
                task->walkingFallback = route;
                task->hasWalkingFallback = true;
                break;
            }
        PauseVendorTravelStrategies(botAI, *task);
        if (!actor->IsInCombat())
        {
            actor->StopMoving();
            actor->GetMotionMaster()->Clear();
        }
        PublishVendorTravelUpdate(*task, actor, "started", plan.reasonCode, &plan);
        vendorTravelTasks[Lower(actorName)] = std::move(task);
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "vendor_journey_started";
        return Finish(requester, std::move(result), true);
    }

    if (isHuntStart)
    {
        FactionTemplateEntry const* actorFaction = actor->GetFactionTemplateEntry();
        std::vector<std::string> targetNames;
        for (uint32 entry : huntRequest.creatureEntries)
        {
            CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(entry);
            FactionTemplateEntry const* targetFaction = creatureTemplate
                ? sFactionTemplateStore.LookupEntry(creatureTemplate->faction) : nullptr;
            if (!creatureTemplate || !targetFaction || !actorFaction || !actorFaction->IsHostileTo(*targetFaction))
            {
                result.reasonCode = "hunt_target_not_hostile_creature";
                return Finish(requester, std::move(result), true);
            }
            targetNames.emplace_back(creatureTemplate->Name);
        }

        std::vector<WorldPosition> const locations = FindAgentCreatureSpawns(actor, huntRequest.creatureEntries,
            huntRequest.mapId, huntRequest.zoneId, huntRequest.areaId);
        if (locations.empty())
        {
            result.reasonCode = "hunt_target_has_no_spawn_in_area";
            return Finish(requester, std::move(result), true);
        }

        std::string const huntKey = Lower(actorName);
        std::lock_guard<std::mutex> lock(agentHuntMutex);
        auto previousTask = agentHuntTasks.find(huntKey);
        if (previousTask != agentHuntTasks.end() && IsAgentHuntActive(*previousTask->second))
        {
            result.reasonCode = "hunt_already_running";
            return Finish(requester, std::move(result), true);
        }
        auto previousGuide = agentGuideTasks.find(huntKey);
        if (previousGuide != agentGuideTasks.end() && IsAgentGuideActive(*previousGuide->second))
        {
            result.reasonCode = "agent_task_already_running";
            return Finish(requester, std::move(result), true);
        }

        auto task = std::make_unique<AgentHuntTask>();
        task->actor = actorName;
        task->requester = requesterName;
        for (std::size_t i = 0; i < targetNames.size(); ++i)
        {
            if (i)
                task->targetName += ", ";
            task->targetName += targetNames[i];
        }
        task->creatureEntry = huntRequest.creatureEntry;
        task->creatureEntries = huntRequest.creatureEntries;
        task->mapId = huntRequest.mapId;
        task->zoneId = huntRequest.zoneId;
        task->areaId = huntRequest.areaId;
        task->requiredKills = huntRequest.killCount;
        task->deliverLoot = huntRequest.deliverLoot;
        task->collectLoot = huntRequest.collectLoot;
        task->lastRouteProgress = now;
        task->startedAt = std::chrono::steady_clock::now();
        task->existingItemCounts = GetExistingBagItemCounts(actor);
        task->hadFollowStrategy = botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT);
        task->hadStayStrategy = botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT);
        task->hadTravelStrategy = botAI->HasStrategy("travel", BOT_STATE_NON_COMBAT);
        task->hadGrindStrategy = botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT);
        task->hadLootStrategy = botAI->HasStrategy("loot", BOT_STATE_NON_COMBAT);

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(huntRequest.zoneId);
        task->zoneName = zone ? zone->area_name[sWorld->GetDefaultDbcLocale()] : "";
        task->destination = std::make_unique<AgentHuntDestination>(huntRequest.creatureEntry, locations);
        std::vector<WorldPosition*> points = task->destination->getPoints(true);
        if (points.empty())
        {
            result.reasonCode = "hunt_route_unavailable";
            return Finish(requester, std::move(result), true);
        }

        TravelTarget* travelTarget = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (!travelTarget)
        {
            result.reasonCode = "hunt_travel_unavailable";
            return Finish(requester, std::move(result), true);
        }

        task->currentSpawnPoint = points.front();
        task->visitedSpawnPoints.insert(task->currentSpawnPoint);
        botAI->SetTaskHuntTargets(huntRequest.creatureEntries, huntRequest.mapId, huntRequest.zoneId,
            huntRequest.areaId);
        botAI->ChangeStrategy("+travel,-grind,-loot,-follow,-stay", BOT_STATE_NON_COMBAT);
        travelTarget->setTarget(task->destination.get(), task->currentSpawnPoint);
        agentHuntTasks[huntKey] = std::move(task);
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "hunt_started";
        return Finish(requester, std::move(result), true);
    }

    if (isHuntCancel)
    {
        std::lock_guard<std::mutex> lock(agentHuntMutex);
        auto task = agentHuntTasks.find(Lower(actorName));
        if (task == agentHuntTasks.end() || !IsAgentHuntActive(*task->second))
        {
            result.reasonCode = "no_active_hunt";
            return Finish(requester, std::move(result), true);
        }

        ResetAgentHuntTravel(actor, botAI, *task->second, false);
        task->second->stage = AgentHuntStage::Cancelled;
        task->second->destination.reset();
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "hunt_cancelled";
        return Finish(requester, std::move(result), true);
    }

    if (actionType == "hunt_loot")
    {
        std::lock_guard<std::mutex> lock(agentHuntMutex);
        auto task = agentHuntTasks.find(Lower(actorName));
        if (task == agentHuntTasks.end() || !IsAgentHuntActive(*task->second)
            || task->second->stage == AgentHuntStage::Returning)
        {
            result.reasonCode = "no_active_hunt";
            return Finish(requester, std::move(result), true);
        }
        AgentHuntTask& hunt = *task->second;
        if (!hunt.kills)
        {
            result.reasonCode = "no_hunt_corpses_yet";
            return Finish(requester, std::move(result), true);
        }
        hunt.collectLoot = true;
        hunt.pendingLootCorpses.insert(hunt.targetCorpseGuids.begin(), hunt.targetCorpseGuids.end());
        hunt.stage = AgentHuntStage::Looting;
        hunt.lastTargetKill = now;
        PauseAgentHuntForLoot(actor, botAI);
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "hunt_loot_started";
        return Finish(requester, std::move(result), true);
    }

    if (isGuideStart)
    {
        if (!actor->IsAlive())
        {
            result.reasonCode = "actor_dead";
            return Finish(requester, std::move(result), true);
        }
        if (actor->IsInCombat() || requester->IsInCombat())
        {
            result.reasonCode = "guide_unavailable_in_combat";
            return Finish(requester, std::move(result), true);
        }
        if (!IsSameWorldPhase(actor, requester))
        {
            result.reasonCode = "guide_user_not_in_same_world";
            return Finish(requester, std::move(result), true);
        }

        std::vector<std::string> targetNames;
        for (uint32 entry : guideRequest.creatureEntries)
        {
            CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(entry);
            if (!creatureTemplate)
            {
                result.reasonCode = "guide_target_not_found";
                return Finish(requester, std::move(result), true);
            }
            targetNames.emplace_back(creatureTemplate->Name);
        }

        // Playerbot travel-target movement walks within the bot's current map. It cannot
        // use a cross-map coordinate as a local waypoint; reject those routes explicitly.
        if (guideRequest.mapId != actor->GetMapId())
        {
            result.reasonCode = "guide_map_transition_unsupported";
            return Finish(requester, std::move(result), true);
        }

        std::vector<WorldPosition> const locations = FindAgentCreatureSpawns(actor, guideRequest.creatureEntries,
            guideRequest.mapId, guideRequest.zoneId, guideRequest.areaId);
        if (locations.empty())
        {
            result.reasonCode = guideRequest.creatureEntries.empty()
                ? "guide_zone_has_no_route" : "guide_target_has_no_spawn_in_area";
            return Finish(requester, std::move(result), true);
        }

        std::string const guideKey = Lower(actorName);
        std::lock_guard<std::mutex> lock(agentHuntMutex);
        auto previousHunt = agentHuntTasks.find(guideKey);
        if (previousHunt != agentHuntTasks.end() && IsAgentHuntActive(*previousHunt->second))
        {
            result.reasonCode = "agent_task_already_running";
            return Finish(requester, std::move(result), true);
        }
        auto previousGuide = agentGuideTasks.find(guideKey);
        if (previousGuide != agentGuideTasks.end() && IsAgentGuideActive(*previousGuide->second))
        {
            result.reasonCode = "guide_already_running";
            return Finish(requester, std::move(result), true);
        }

        auto task = std::make_unique<AgentGuideTask>();
        task->actor = actorName;
        task->requester = requesterName;
        for (std::size_t i = 0; i < targetNames.size(); ++i)
        {
            if (i)
                task->targetName += ", ";
            task->targetName += targetNames[i];
        }
        task->creatureEntries = guideRequest.creatureEntries;
        task->mapId = guideRequest.mapId;
        task->zoneId = guideRequest.zoneId;
        task->areaId = guideRequest.areaId;
        task->startedAt = now;
        task->hadFollowStrategy = botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT);
        task->hadStayStrategy = botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT);
        task->hadTravelStrategy = botAI->HasStrategy("travel", BOT_STATE_NON_COMBAT);
        task->hadGrindStrategy = botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT);
        task->hadLootStrategy = botAI->HasStrategy("loot", BOT_STATE_NON_COMBAT);

        uint32 const destinationAreaId = guideRequest.areaId ? guideRequest.areaId : guideRequest.zoneId;
        AreaTableEntry const* zone = destinationAreaId ? sAreaTableStore.LookupEntry(destinationAreaId) : nullptr;
        task->zoneName = zone ? zone->area_name[sWorld->GetDefaultDbcLocale()] : "";
        if (task->targetName.empty())
            task->targetName = task->zoneName.empty() ? "zone " + std::to_string(guideRequest.zoneId) : task->zoneName;
        uint32 const destinationEntry = guideRequest.creatureEntries.empty() ? 0 : guideRequest.creatureEntries.front();
        task->destination = std::make_unique<AgentGuideDestination>(destinationEntry, locations);
        std::vector<WorldPosition*> points = task->destination->getPoints(true);
        if (points.empty())
        {
            result.reasonCode = "guide_route_unavailable";
            return Finish(requester, std::move(result), true);
        }

        TravelTarget* travelTarget = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (!travelTarget)
        {
            result.reasonCode = "guide_travel_unavailable";
            return Finish(requester, std::move(result), true);
        }

        task->currentSpawnPoint = points.front();
        botAI->ChangeStrategy("+travel,-grind,-loot,-follow,-stay", BOT_STATE_NON_COMBAT);
        travelTarget->setTarget(task->destination.get(), task->currentSpawnPoint);
        agentGuideTasks[guideKey] = std::move(task);
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "guide_started";
        return Finish(requester, std::move(result), true);
    }

    if (isGuideCancel)
    {
        std::lock_guard<std::mutex> lock(agentHuntMutex);
        auto task = agentGuideTasks.find(Lower(actorName));
        if (task == agentGuideTasks.end() || !IsAgentGuideActive(*task->second))
        {
            result.reasonCode = "no_active_guide";
            return Finish(requester, std::move(result), true);
        }

        ResetAgentGuideTravel(actor, botAI, *task->second, false);
        task->second->stage = AgentGuideStage::Cancelled;
        task->second->destination.reset();
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "guide_cancelled";
        return Finish(requester, std::move(result), true);
    }

    if (isOfferItems)
    {
        if (!actor->GetSession())
        {
            result.reasonCode = "trade_session_unavailable";
            return Finish(requester, std::move(result), true);
        }
        if (!actor->IsAlive() || !requester->IsAlive() || actor->IsInCombat() || requester->IsInCombat())
        {
            result.reasonCode = "trade_unavailable_in_combat_or_dead";
            return Finish(requester, std::move(result), true);
        }
        if (!IsSameWorldPhase(actor, requester) || !actor->IsWithinDistInMap(requester, TRADE_DISTANCE, false))
        {
            result.reasonCode = "trade_too_far";
            return Finish(requester, std::move(result), true);
        }

        bool const tradeAlreadyOpen = actor->GetTradeData() || requester->GetTradeData();
        if (tradeAlreadyOpen)
        {
            if (actor->GetTrader() != requester || requester->GetTrader() != actor)
            {
                result.reasonCode = "trade_busy";
                return Finish(requester, std::move(result), true);
            }
        }
        std::vector<Item*> plannedItems;
        std::vector<uint8> plannedSlots;
        std::string planReason;
        if (!BuildTradeOfferPlan(actor, requester, itemOffers, plannedItems, plannedSlots, planReason))
        {
            result.reasonCode = planReason;
            return Finish(requester, std::move(result), true);
        }

        if (!tradeAlreadyOpen)
        {
            WorldPacket initiatePacket(CMSG_INITIATE_TRADE);
            initiatePacket << requester->GetGUID();
            actor->GetSession()->HandleInitiateTradeOpcode(initiatePacket);
        }

        std::string const offerResult = OfferItemStacks(actor, requester, itemOffers);
        if (offerResult != "items_offered" && offerResult != "items_already_offered")
        {
            result.reasonCode = offerResult;
            return Finish(requester, std::move(result), true);
        }
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = offerResult;
        return Finish(requester, std::move(result), true);
    }

    if (isItemAction)
    {
        if (!actor->IsAlive())
        {
            result.reasonCode = "actor_dead";
            return Finish(requester, std::move(result), true);
        }
        if (!actor->GetSession())
        {
            result.reasonCode = "actor_session_unavailable";
            return Finish(requester, std::move(result), true);
        }

        Item* item = FindItemAtLocation(actor, itemLocation);
        if (!item || !item->GetTemplate())
        {
            result.reasonCode = "item_not_in_requested_slot";
            return Finish(requester, std::move(result), true);
        }

        if (isUseItem)
        {
            ItemTemplate const* itemTemplate = item->GetTemplate();
            uint32 itemSpellId = 0;
            for (_Spell const& itemSpell : itemTemplate->Spells)
                if (itemSpell.SpellId && (itemSpell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE
                    || itemSpell.SpellTrigger == ITEM_SPELLTRIGGER_ON_NO_DELAY_USE))
                {
                    itemSpellId = itemSpell.SpellId;
                    break;
                }

            if (!itemSpellId || actor->CanUseItem(itemTemplate) != EQUIP_ERR_OK)
            {
                result.reasonCode = "item_not_usable";
                return Finish(requester, std::move(result), true);
            }

            uint8 castCount = 0;
            uint32 glyphIndex = 0;
            uint8 castFlags = 0;
            WorldPacket packet(CMSG_USE_ITEM);
            packet << itemLocation.bag << itemLocation.slot << castCount << itemSpellId
                << item->GetGUID() << glyphIndex << castFlags;
            uint32 const targetFlags = TARGET_FLAG_UNIT;
            packet << targetFlags << actor->GetPackGUID();
            actor->GetSession()->HandleUseItemOpcode(packet);
            lastActionByActor[rateKey] = now;
            result.status = "accepted";
            result.reasonCode = "item_use_started";
            return Finish(requester, std::move(result), true);
        }

        uint8 const equipSlot = actor->FindEquipSlot(item->GetTemplate(), NULL_SLOT, !item->IsBag());
        if (equipSlot == NULL_SLOT)
        {
            result.reasonCode = "item_not_equippable";
            return Finish(requester, std::move(result), true);
        }

        uint16 destination = (INVENTORY_SLOT_BAG_0 << 8) | equipSlot;
        if (actor->CanEquipItem(NULL_SLOT, destination, item, !item->IsBag()) != EQUIP_ERR_OK)
        {
            result.reasonCode = "item_not_equippable";
            return Finish(requester, std::move(result), true);
        }

        WorldPacket packet(CMSG_AUTOEQUIP_ITEM);
        packet << itemLocation.bag << itemLocation.slot;
        WorldPackets::Item::AutoEquipItem equipRequest(std::move(packet));
        equipRequest.SourceBag = itemLocation.bag;
        equipRequest.SourceSlot = itemLocation.slot;
        actor->GetSession()->HandleAutoEquipItemOpcode(equipRequest);
        if (actor->GetItemByPos(INVENTORY_SLOT_BAG_0, equipSlot) != item)
        {
            result.reasonCode = "item_equip_rejected";
            return Finish(requester, std::move(result), true);
        }

        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "item_equipped";
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

    if (isVendor)
    {
        auto reject = [&](char const* reason)
        {
            result.reasonCode = reason;
            if (isVendorBuy)
                PublishVendorPurchaseFailure(actor, nullptr, targetGuid, actionId, vendorBuyRequest, reason);
            return Finish(requester, std::move(result), true);
        };
        if (!actor->IsAlive())
            return reject("actor_dead");
        if (actor->IsInCombat())
            return reject("actor_in_combat");
        if (!actor->GetSession())
            return reject("actor_session_unavailable");
        Creature* npc = ResolveTalkableNpc(actor, targetGuid, true);
        if (!npc)
            return reject("npc_unavailable_or_out_of_range");
        if (pendingNpcInteractions.find(Lower(actorName)) != pendingNpcInteractions.end())
            return reject("npc_approach_busy");
        if (actor->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
        {
            std::string reason;
            bool const completed = isVendorBuy ? BuyFromVendor(actor, npc, actionId, vendorBuyRequest, reason)
                : InspectVendor(actor, npc, actionId);
            result.status = completed ? "accepted" : "rejected";
            result.reasonCode = isVendorBuy ? reason : completed ? "vendor_catalog_published" : "vendor_inspection_rejected";
            lastActionByActor[rateKey] = now;
            return Finish(requester, std::move(result), true);
        }
        if (pendingNpcInteractions.size() >= MAX_PENDING_NPC_INTERACTIONS
            || pendingNpcInteractions.find(Lower(actorName)) != pendingNpcInteractions.end())
            return reject("npc_approach_busy");
        BridgeMovementAction movement(botAI);
        if (!movement.Approach(npc))
            return reject("npc_approach_failed");
        pendingNpcInteractions.emplace(Lower(actorName), PendingNpcInteraction{
            actorName, requesterName, actionId, groupId, targetGuid, now + NPC_APPROACH_TIMEOUT, actionType, taskToken});
        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "approaching_npc";
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
            || (item->OptionType != GOSSIP_OPTION_GOSSIP && item->OptionType != GOSSIP_OPTION_QUESTGIVER
                && item->OptionType != GOSSIP_OPTION_TRAINER))
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

    if (isTrainerAction)
    {
        if (actor->IsInCombat())
        {
            result.reasonCode = "actor_in_combat";
            return Finish(requester, std::move(result), true);
        }
        Creature* trainerNpc = actor->GetNPCIfCanInteractWith(ObjectGuid(targetGuid),
            UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS | UNIT_NPC_FLAG_TRAINER_PROFESSION);
        Trainer::Trainer const* trainer = trainerNpc ? sObjectMgr->GetTrainer(trainerNpc->GetEntry()) : nullptr;
        if (!trainerNpc || !trainer || !trainer->IsTrainerValidForPlayer(actor))
        {
            result.reasonCode = "trainer_unavailable_or_out_of_range";
            return Finish(requester, std::move(result), true);
        }

        // Playerbot reports "nothing teachable now" as an unavailable action;
        // name it, so the agent can explain the real reason (skill or level).
        auto const& trainerSpells = trainer->GetSpells();
        if (std::none_of(trainerSpells.begin(), trainerSpells.end(),
            [&](Trainer::Spell const& spell) { return trainer->CanTeachSpell(actor, &spell); }))
        {
            result.reasonCode = "trainer_nothing_to_learn";
            return Finish(requester, std::move(result), true);
        }

        actor->SetSelection(trainerNpc->GetGUID());
        Event const trainerEvent("bridge request", taskToken, requester);
        if (!botAI->DoSpecificAction("trainer", trainerEvent, true, taskToken))
        {
            result.reasonCode = "trainer_action_unavailable";
            return Finish(requester, std::move(result), true);
        }

        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = taskToken == "learn" ? "trainer_learning_started" : "trainer_inspection_started";
        return Finish(requester, std::move(result), true);
    }

    if (isPlayerbotAction)
    {
        // Purchases must retain the typed quantity, budget and currency checks.
        if (playerbotActionName == "buy" || playerbotActionName == "rpg buy")
        {
            result.reasonCode = "use_typed_vendor_purchase";
            return Finish(requester, std::move(result), true);
        }
        AiObjectContext* context = botAI->GetAiObjectContext();
        std::set<std::string> const supportedActions = context ? context->GetSupportedActions()
            : std::set<std::string>{};
        if (supportedActions.find(playerbotActionName) == supportedActions.end())
        {
            result.reasonCode = "unknown_playerbot_action";
            return Finish(requester, std::move(result), true);
        }

        if (targetGuid)
        {
            Unit* target = ObjectAccessor::GetUnit(*actor, ObjectGuid(targetGuid));
            Player* targetPlayer = target ? target->ToPlayer() : nullptr;
            bool const isGroupUnit = targetPlayer && targetPlayer->GetGroup() == requesterGroup;
            bool const isActor = target == actor;
            if (!target || !target->IsInWorld() || !IsSameWorldPhase(actor, target)
                || !actor->IsWithinDistInMap(target, 100.0f) || !actor->IsWithinLOSInMap(target)
                || (targetPlayer && !isGroupUnit && !isActor))
            {
                result.reasonCode = "invalid_target";
                return Finish(requester, std::move(result), true);
            }
            actor->SetSelection(target->GetGUID());
        }

        Event const actionEvent("bridge request", playerbotQualifier, requester);
        if (!botAI->DoSpecificAction(playerbotActionName, actionEvent, true, playerbotQualifier))
        {
            result.reasonCode = "action_unavailable";
            return Finish(requester, std::move(result), true);
        }

        lastActionByActor[rateKey] = now;
        result.status = "accepted";
        result.reasonCode = "playerbot_action_started";
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

void RecordAgentHuntKill(Player* player, Creature* killed)
{
    if (!player || !killed || !IsBridgeBot(player->GetName()))
        return;

    std::lock_guard<std::mutex> lock(agentHuntMutex);
    auto task = agentHuntTasks.find(Lower(player->GetName()));
    if (task == agentHuntTasks.end()
        || (task->second->stage != AgentHuntStage::Hunting && task->second->stage != AgentHuntStage::Looting)
        || std::find(task->second->creatureEntries.begin(), task->second->creatureEntries.end(), killed->GetEntry())
            == task->second->creatureEntries.end())
        return;

    AgentHuntTask& hunt = *task->second;
    if (!hunt.targetCorpseGuids.insert(killed->GetGUID().GetRawValue()).second)
        return;
    ++hunt.kills;
    hunt.lastTargetKill = std::chrono::steady_clock::now();
    if (hunt.collectLoot)
    {
        hunt.pendingLootCorpses.insert(killed->GetGUID().GetRawValue());
        hunt.stage = AgentHuntStage::Looting;
        if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(player))
            PauseAgentHuntForLoot(player, botAI);
    }
    else if (hunt.kills >= hunt.requiredKills)
    {
        hunt.stage = AgentHuntStage::Looting;
        if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(player))
            botAI->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);
    }
}

void RecordAgentHuntLoot(Player* player, Item* item, uint32 count, uint64 lootGuid)
{
    if (!player || !item || !count || !IsBridgeBot(player->GetName()))
        return;

    std::lock_guard<std::mutex> lock(agentHuntMutex);
    auto task = agentHuntTasks.find(Lower(player->GetName()));
    if (task == agentHuntTasks.end() || !task->second->collectLoot
        || !IsAgentHuntActive(*task->second) || !task->second->targetCorpseGuids.contains(lootGuid))
        return;

    task->second->lootedItems[item->GetEntry()] += count;
    task->second->lootedItemStackCounts[item->GetGUID().GetRawValue()] += count;
}

std::string GetAgentHuntStatusJson(std::string const& actorName)
{
    std::lock_guard<std::mutex> lock(agentHuntMutex);
    auto guide = agentGuideTasks.find(Lower(actorName));
    auto task = agentHuntTasks.find(Lower(actorName));
    bool const activeHunt = task != agentHuntTasks.end() && IsAgentHuntActive(*task->second);
    if (guide != agentGuideTasks.end() && (IsAgentGuideActive(*guide->second) || !activeHunt))
    {
        AgentGuideTask const& route = *guide->second;
        std::ostringstream out;
        out << "{\"type\":\"guide\",\"stage\":\"" << AgentGuideStageName(route.stage)
            << "\",\"target\":\"" << JsonEscape(route.targetName)
            << "\",\"target_entries\":[";
        for (std::size_t i = 0; i < route.creatureEntries.size(); ++i)
        {
            if (i)
                out << ',';
            out << route.creatureEntries[i];
        }
        out << "],\"zone\":\"" << JsonEscape(route.zoneName)
            << "\",\"zone_id\":" << route.zoneId
            << ",\"area_id\":" << route.areaId
            << ",\"map_id\":" << route.mapId << '}';
        return out.str();
    }
    if (task == agentHuntTasks.end())
        return "null";

    AgentHuntTask const& hunt = *task->second;
    std::ostringstream out;
    out << "{\"type\":\"hunt\",\"stage\":\"" << AgentHuntStageName(hunt.stage)
        << "\",\"target\":\"" << JsonEscape(hunt.targetName)
        << "\",\"target_entry\":" << hunt.creatureEntry
        << ",\"target_entries\":[";
    for (std::size_t i = 0; i < hunt.creatureEntries.size(); ++i)
    {
        if (i)
            out << ',';
        out << hunt.creatureEntries[i];
    }
    out << "],\"zone\":\"" << JsonEscape(hunt.zoneName)
        << "\",\"zone_id\":" << hunt.zoneId
        << ",\"area_id\":" << hunt.areaId
        << ",\"map_id\":" << hunt.mapId
        << ",\"kills\":" << hunt.kills
        << ",\"kills_requested\":" << hunt.requiredKills
        << ",\"collect_loot\":" << (hunt.collectLoot ? "true" : "false")
        << ",\"deliver_loot\":" << (hunt.deliverLoot ? "true" : "false")
        << ",\"looted_item_entries\":" << hunt.lootedItems.size()
        << ",\"staged_stacks\":" << hunt.stagedStacks << '}';
    return out.str();
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
                PublishActionResultEvent(requester, pending.actor, pending.actionId, pending.type, "rejected", reason);
            if (pending.type == "vendor_buy" && actor && actor->IsInWorld())
            {
                VendorBuyRequest request;
                if (ParseVendorBuyRequest(pending.taskToken, request))
                    PublishVendorPurchaseFailure(actor, nullptr, pending.targetGuid, pending.actionId, request, reason);
            }
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
        bool const vendor = pending.type == "vendor_inspect" || pending.type == "vendor_buy";
        PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(actor);
        if (!botAI || botAI->GetMaster() != requester || !actor->IsAlive() || !actor->GetSession())
        {
            fail("npc_approach_context_changed");
            continue;
        }
        Creature* npc = ResolveTalkableNpc(actor, pending.targetGuid, vendor);
        if (!npc || actor->IsInCombat())
        {
            fail("npc_lost_or_actor_in_combat");
            continue;
        }
        if (actor->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
        {
            if (vendor)
            {
                std::string reason;
                bool completed = false;
                if (pending.type == "vendor_buy")
                {
                    VendorBuyRequest request;
                    completed = ParseVendorBuyRequest(pending.taskToken, request)
                        && BuyFromVendor(actor, npc, pending.actionId, request, reason);
                }
                else
                {
                    completed = InspectVendor(actor, npc, pending.actionId);
                    reason = completed ? "vendor_catalog_published" : "vendor_inspection_rejected";
                }
                PublishActionResultEvent(requester, pending.actor, pending.actionId, pending.type,
                    completed ? "accepted" : "rejected", reason);
                iter = pendingNpcInteractions.erase(iter);
                continue;
            }
            if (!actor->IsWithinLOSInMap(npc) || !OpenNpcDialog(actor, npc, pending.actionId))
                fail("npc_interaction_rejected");
            else
                iter = pendingNpcInteractions.erase(iter);
            continue;
        }
        ++iter;
    }

    UpdateDeferredVendorStrategyRestores();
    UpdateVendorTravelTasks();
    UpdateAgentHuntTasks();
    UpdateAgentGuideTasks();
}
