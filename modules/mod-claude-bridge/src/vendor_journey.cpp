#include "vendor_journey.h"
#include "vendor_transport.h"

#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "GridDefines.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    constexpr uint32 JOURNEY_POINT_ID = 0x574A5652;
    constexpr uint64 MAX_JOURNEY_MS = 60ULL * 60 * 1000;
    constexpr uint32 MAX_STALL_MS = 90000;
    constexpr uint32 REPATH_INTERVAL_MS = 2000;
    constexpr uint32 MAX_BOARDING_WAIT_MS = 20000;
    // After this long descending, drop the flight flags where the bot is.
    constexpr uint32 MAX_LANDING_MS = 15000;
    constexpr uint8 MAX_MOUNT_ATTEMPTS = 3;
    constexpr uint32 MAX_MOUNT_STOP_MS = 3000;
    // Own flight: hops stay inside loaded grids; the landing is picked near the NPC.
    constexpr float FLIGHT_HOP = 200.0f;
    constexpr float FLIGHT_LANDING_SEARCH = 120.0f;
    constexpr float FLIGHT_CLEARANCE = 30.0f;
    constexpr uint32 MAX_FLIGHT_STALL_MS = 30000;
    constexpr std::size_t MAX_SOURCE_FLIGHTMASTERS = 6;
    constexpr std::size_t MAX_TAXI_HOPS = 32;

    VendorJourneyLocation Location(Player const* actor)
    {
        return {actor->GetMapId(), actor->GetPositionX(), actor->GetPositionY(), actor->GetPositionZ()};
    }

    VendorJourneyLocation Location(TaxiNodesEntry const* node)
    {
        return {node->map_id, node->x, node->y, node->z};
    }

    bool ValidLocation(VendorJourneyLocation const& point)
    {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)
            && std::abs(point.x) < 17066.0f && std::abs(point.y) < 17066.0f
            && std::abs(point.z) < 17066.0f;
    }

    float Distance(VendorJourneyLocation const& from, VendorJourneyLocation const& to)
    {
        if (from.mapId != to.mapId)
            return std::numeric_limits<float>::infinity();
        float const x = from.x - to.x;
        float const y = from.y - to.y;
        float const z = from.z - to.z;
        return std::sqrt(x * x + y * y + z * z);
    }

    bool IsOrdinaryKnownNode(Player* actor, uint32 nodeId, uint32 mapId)
    {
        // PlayerTaxi's known-node getter does not bounds-check its mask index.
        if (!nodeId || (nodeId - 1) / 32 >= TaxiMaskSize)
            return false;
        uint32 const field = (nodeId - 1) / 32;
        uint32 const mask = uint32(1) << ((nodeId - 1) % 32);
        TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(nodeId);
        return node && (sTaxiNodesMask[field] & mask) && node->map_id == mapId
            && actor->m_taxi.IsTaximaskNodeKnown(nodeId)
            && node->MountCreatureID[actor->GetTeamId() == TEAM_ALLIANCE ? 1 : 0]
            // Acherus's special class taxi must not become a public transport edge.
            && node->MountCreatureID[0] != 32981;
    }

    bool IsOrdinaryTaxiPath(uint32 pathId, uint32 mapId)
    {
        // ObjectMgr exposes spell paths too; explicitly remove them instead of
        // assuming that membership in its directed edge lookup means public taxi.
        static std::unordered_set<uint32> const spellPaths = []
        {
            std::unordered_set<uint32> paths;
            for (SpellEntry const* spell : sSpellStore)
                for (uint8 effect = 0; effect < MAX_SPELL_EFFECTS; ++effect)
                    if (spell->Effect[effect] == SPELL_EFFECT_SEND_TAXI)
                        paths.insert(spell->EffectMiscValue[effect]);
            return paths;
        }();
        if (!pathId || spellPaths.contains(pathId) || pathId >= sTaxiPathNodesByPath.size()
            || sTaxiPathNodesByPath[pathId].empty())
            return false;
        for (TaxiPathNodeEntry const* point : sTaxiPathNodesByPath[pathId])
            if (!point || point->mapid != mapId)
                return false;
        return true;
    }

    struct SourceFlightmaster
    {
        uint32 spawnId;
        uint32 entry;
        uint32 nodeId;
        VendorJourneyLocation location;
        float walkingDistance;
    };

    struct TaxiEdge
    {
        uint32 to;
        uint32 fare;
        float seconds;
    };

    // Fastest learned mount; with flying, only mounts that can fly.
    uint32 FindMountSpell(Player* actor, bool flying)
    {
        uint32 best = 0;
        int32 bestSpeed = -1;
        for (auto const& [spellId, spell] : actor->GetSpellMap())
        {
            if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || !info->HasAura(SPELL_AURA_MOUNTED)
                || (flying && !info->HasAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)))
                continue;
            int32 speed = 0;
            for (SpellEffectInfo const& effect : info->Effects)
                if (effect.ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED
                    || effect.ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
                    speed = std::max(speed, effect.CalcValue());
            if (speed > bestSpeed)
            {
                best = spellId;
                bestSpeed = speed;
            }
        }
        return best;
    }

    // Highest surface (terrain, roof or tree) under x,y.
    float TopSurface(Player* actor, float x, float y, float fromZ)
    {
        return actor->GetMap()->GetHeight(actor->GetPhaseMask(), x, y, fromZ + 800.0f, true, 2000.0f);
    }

    // Flight altitude that clears every sampled surface between two points.
    float CruiseHeight(Player* actor, float x1, float y1, float x2, float y2, float floorZ)
    {
        float const length = std::hypot(x2 - x1, y2 - y1);
        uint32 const steps = std::max<uint32>(1, static_cast<uint32>(length / 20.0f));
        float top = floorZ;
        for (uint32 i = 0; i <= steps; ++i)
        {
            float const t = static_cast<float>(i) / steps;
            float const surface = TopSurface(actor, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, floorZ);
            if (surface > INVALID_HEIGHT)
                top = std::max(top, surface);
        }
        return top + FLIGHT_CLEARANCE;
    }

    // Open-sky ground near the NPC from which a ground path reaches it.
    bool PickOpenLanding(Player* actor, VendorJourneyLocation const& target, VendorJourneyLocation& landing)
    {
        constexpr float TWO_PI = 6.2831853071795864769f;
        Map* map = actor->GetMap();
        for (float radius : {0.0f, 6.0f, 12.0f, 20.0f, 30.0f, 45.0f, 60.0f, 80.0f, 100.0f, FLIGHT_LANDING_SEARCH})
        {
            uint32 const count = radius > 0.0f ? 16 : 1;
            for (uint32 index = 0; index < count; ++index)
            {
                float const angle = TWO_PI * static_cast<float>(index) / count;
                float const x = target.x + std::cos(angle) * radius;
                float const y = target.y + std::sin(angle) * radius;
                float const z = map->GetHeight(actor->GetPhaseMask(), x, y, target.z + 3.0f, true, 30.0f);
                if (z <= INVALID_HEIGHT || std::abs(TopSurface(actor, x, y, z) - z) > 2.0f)
                    continue;
                if (radius > 0.0f)
                {
                    PathGenerator path(actor);
                    path.CalculatePath(x, y, z, target.x, target.y, target.z, false);
                    PathType const type = path.GetPathType();
                    if (!(type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE))
                        || (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
                        || path.GetPath().size() < 2)
                        continue;
                }
                landing = {target.mapId, x, y, z};
                return true;
            }
        }
        return false;
    }

    Creature* FindFlightmaster(Player* actor, VendorJourneyRoute const& route)
    {
        auto const bounds = actor->GetMap()->GetCreatureBySpawnIdStore().equal_range(route.flightmasterSpawnId);
        for (auto iterator = bounds.first; iterator != bounds.second; ++iterator)
        {
            Creature* creature = iterator->second;
            if (creature && creature->IsInWorld() && creature->IsAlive()
                && creature->GetEntry() == route.flightmasterEntry
                && (creature->GetPhaseMask() & actor->GetPhaseMask()))
                return creature;
        }
        return nullptr;
    }
}

VendorJourneyPlan PlanVendorJourney(Player* actor, VendorJourneyLocation destination,
    uint32 reservedPurchaseCopper, std::size_t candidateLimit)
{
    VendorJourneyPlan plan;
    if (!actor || !actor->IsInWorld() || !actor->GetSession() || !actor->IsAlive())
    {
        plan.reasonCode = "journey_actor_unavailable";
        return plan;
    }
    if (!ValidLocation(destination) || !sMapStore.LookupEntry(destination.mapId))
    {
        plan.reasonCode = "journey_invalid_destination";
        return plan;
    }
    if (actor->GetMap()->IsDungeon() || actor->InBattleground() || actor->GetTransport() || actor->GetVehicle())
    {
        plan.reasonCode = "journey_world_ground_route_required";
        return plan;
    }
    if (reservedPurchaseCopper > actor->GetMoney())
    {
        plan.reasonCode = "journey_purchase_reserve_unavailable";
        return plan;
    }
    if (destination.mapId != actor->GetMapId())
    {
        VendorTransportPlanningResult route = PlanVendorTransport(actor, destination);
        plan.reasonCode = route.reasonCode;
        if (route.plan)
        {
            VendorJourneyRoute candidate;
            candidate.destination = destination;
            candidate.estimatedSeconds = route.plan->estimatedSeconds;
            candidate.transportPlan = std::move(route.plan);
            plan.candidates.push_back(std::move(candidate));
        }
        return plan;
    }
    candidateLimit = std::clamp<std::size_t>(candidateLimit, 1, 16);
    VendorJourneyLocation const start = Location(actor);
    float const runSpeed = std::max(1.0f, actor->GetSpeed(MOVE_RUN));
    VendorJourneyRoute walk;
    walk.destination = destination;
    walk.estimatedSeconds = Distance(start, destination) / runSpeed;
    plan.candidates.push_back(walk);

    std::vector<SourceFlightmaster> sources;
    for (auto const& [spawnId, spawn] : sObjectMgr->GetAllCreatureData())
    {
        if (spawn.mapid != start.mapId || (spawn.phaseMask && !(spawn.phaseMask & actor->GetPhaseMask())))
            continue;
        // Alternate spawn entries cannot be predicted; validate the actual NPC again at boarding.
        for (uint32 entry : {spawn.id, spawn.id2, spawn.id3})
        {
            CreatureTemplate const* creature = entry ? sObjectMgr->GetCreatureTemplate(entry) : nullptr;
            FactionTemplateEntry const* faction = creature ? sFactionTemplateStore.LookupEntry(creature->faction) : nullptr;
            if (!creature || !(creature->npcflag & UNIT_NPC_FLAG_FLIGHTMASTER) || !faction
                || Unit::GetFactionReactionTo(actor->GetFactionTemplateEntry(), faction) < REP_NEUTRAL)
                continue;
            uint32 const node = sObjectMgr->GetNearestTaxiNode(spawn.posX, spawn.posY, spawn.posZ,
                spawn.mapid, actor->GetTeamId());
            if (!IsOrdinaryKnownNode(actor, node, start.mapId))
                continue;
            VendorJourneyLocation const location{spawn.mapid, spawn.posX, spawn.posY, spawn.posZ};
            if (Distance(location, Location(sTaxiNodesStore.LookupEntry(node))) > 50.0f)
                continue;
            sources.push_back({spawnId, entry, node, location, Distance(start, location)});
        }
    }
    std::sort(sources.begin(), sources.end(), [](auto const& left, auto const& right)
    {
        return left.walkingDistance < right.walkingDistance;
    });
    // Different NPCs at a common taxi node should not consume the entire source budget.
    std::unordered_set<uint32> seenNodes;
    std::erase_if(sources, [&seenNodes](SourceFlightmaster const& source)
    {
        return !seenNodes.insert(source.nodeId).second;
    });
    if (sources.size() > MAX_SOURCE_FLIGHTMASTERS)
        sources.resize(MAX_SOURCE_FLIGHTMASTERS);

    std::unordered_map<uint32, std::vector<TaxiEdge>> edges;
    for (uint32 row = 0; row < sTaxiPathStore.GetNumRows(); ++row)
    {
        TaxiPathEntry const* edge = sTaxiPathStore.LookupEntry(row);
        if (!edge || !IsOrdinaryKnownNode(actor, edge->from, start.mapId)
            || !IsOrdinaryKnownNode(actor, edge->to, start.mapId))
            continue;
        uint32 path = 0;
        uint32 fare = 0;
        sObjectMgr->GetTaxiPath(edge->from, edge->to, path, fare);
        if (path != edge->ID || !IsOrdinaryTaxiPath(path, start.mapId))
            continue;
        float const seconds = Distance(Location(sTaxiNodesStore.LookupEntry(edge->from)),
            Location(sTaxiNodesStore.LookupEntry(edge->to))) / 32.0f;
        edges[edge->from].push_back({edge->to, fare, seconds});
    }
    uint64 const budget = actor->GetMoney() - reservedPurchaseCopper;
    for (SourceFlightmaster const& source : sources)
    {
        // Least-fare paths are deliberately conservative. Estimates select among
        // affordable routes; this is not a globally optimal multimodal planner.
        using QueueEntry = std::pair<uint64, uint32>;
        std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;
        std::unordered_map<uint32, uint64> fare;
        std::unordered_map<uint32, uint32> previous;
        std::unordered_map<uint32, float> seconds;
        fare[source.nodeId] = 0;
        seconds[source.nodeId] = 0;
        queue.push({0, source.nodeId});
        while (!queue.empty())
        {
            auto const [cost, node] = queue.top();
            queue.pop();
            if (fare[node] != cost)
                continue;
            auto const outgoing = edges.find(node);
            if (outgoing == edges.end())
                continue;
            for (TaxiEdge const& edge : outgoing->second)
            {
                uint64 const nextCost = cost + edge.fare;
                if (nextCost > budget)
                    continue;
                auto const existing = fare.find(edge.to);
                if (existing != fare.end() && existing->second <= nextCost)
                    continue;
                fare[edge.to] = nextCost;
                seconds[edge.to] = seconds[node] + edge.seconds;
                previous[edge.to] = node;
                queue.push({nextCost, edge.to});
            }
        }
        for (auto const& [node, cost] : fare)
        {
            if (node == source.nodeId)
                continue;
            VendorJourneyRoute route;
            route.destination = destination;
            route.flightmasterLocation = source.location;
            route.flightmasterEntry = source.entry;
            route.flightmasterSpawnId = source.spawnId;
            route.maximumFareCopper = static_cast<uint32>(cost);
            route.estimatedSeconds = source.walkingDistance / runSpeed + seconds[node]
                + Distance(Location(sTaxiNodesStore.LookupEntry(node)), destination) / runSpeed + 15.0f;
            if (route.estimatedSeconds >= walk.estimatedSeconds)
                continue;
            uint32 cursor = node;
            while (cursor != source.nodeId && route.taxiNodes.size() < MAX_TAXI_HOPS)
            {
                route.taxiNodes.push_back(cursor);
                cursor = previous.at(cursor);
            }
            if (cursor != source.nodeId)
                continue;
            route.taxiNodes.push_back(source.nodeId);
            std::reverse(route.taxiNodes.begin(), route.taxiNodes.end());
            plan.candidates.push_back(std::move(route));
        }
    }
    std::stable_sort(plan.candidates.begin(), plan.candidates.end(), [](auto const& left, auto const& right)
    {
        return left.estimatedSeconds < right.estimatedSeconds;
    });
    if (plan.candidates.size() > candidateLimit)
    {
        plan.candidates.resize(candidateLimit);
        // Always retain the zero-fare fallback for callers that decline air travel.
        if (std::none_of(plan.candidates.begin(), plan.candidates.end(), [](auto const& route) { return !route.UsesTaxi(); }))
            plan.candidates.back() = walk;
    }
    plan.reasonCode = "journey_routes_planned";
    return plan;
}

VendorTaxiActivation ActivateVendorJourneyTaxi(Player* actor, Creature* flightmaster,
    VendorJourneyRoute const& route, uint32 reservedPurchaseCopper)
{
    VendorTaxiActivation result;
    auto reject = [&result](char const* reason) { result.reasonCode = reason; return result; };
    if (!actor || !actor->IsInWorld() || !actor->IsAlive() || !actor->GetSession())
        return reject("journey_actor_unavailable");
    if (!route.UsesTaxi() || route.taxiNodes.size() > MAX_TAXI_HOPS + 1)
        return reject("journey_invalid_taxi_route");
    if (route.destination.mapId != actor->GetMapId() || route.flightmasterLocation.mapId != actor->GetMapId())
        return reject("unsupported_cross_map");
    if (!flightmaster || !flightmaster->IsInWorld() || !flightmaster->IsAlive()
        || flightmaster->GetMapId() != actor->GetMapId()
        || !(flightmaster->GetPhaseMask() & actor->GetPhaseMask()) || flightmaster->IsInCombat()
        || flightmaster->GetEntry() != route.flightmasterEntry
        || flightmaster->GetSpawnId() != route.flightmasterSpawnId
        || actor->GetNPCIfCanInteractWith(flightmaster->GetGUID(), UNIT_NPC_FLAG_FLIGHTMASTER) != flightmaster)
        return reject("journey_flightmaster_not_interactable");
    uint32 const source = sObjectMgr->GetNearestTaxiNode(flightmaster->GetPositionX(), flightmaster->GetPositionY(),
        flightmaster->GetPositionZ(), flightmaster->GetMapId(), actor->GetTeamId());
    TaxiNodesEntry const* sourceNode = sTaxiNodesStore.LookupEntry(source);
    if (!sourceNode || route.taxiNodes.front() != source
        || Distance(Location(actor), Location(sourceNode)) > 50.0f)
        return reject("journey_taxi_source_changed");
    uint64 fare = 0;
    for (std::size_t i = 0; i < route.taxiNodes.size(); ++i)
    {
        if (!IsOrdinaryKnownNode(actor, route.taxiNodes[i], actor->GetMapId()))
            return reject("journey_taxi_node_unknown_or_ineligible");
        if (!i)
            continue;
        uint32 path = 0;
        uint32 cost = 0;
        sObjectMgr->GetTaxiPath(route.taxiNodes[i - 1], route.taxiNodes[i], path, cost);
        if (!IsOrdinaryTaxiPath(path, actor->GetMapId()))
            return reject("journey_taxi_edge_unavailable");
        fare += cost;
    }
    // Conservative reservation uses the full undiscounted fare. Core charges
    // normal reputation-discounted prices; no money is granted or altered here.
    if (fare > route.maximumFareCopper || fare + reservedPurchaseCopper > actor->GetMoney())
        return reject("journey_taxi_fare_or_reserve_changed");
    if (actor->IsMounted() || actor->IsInDisallowedMountForm() || actor->IsInCombat()
        || actor->IsNonMeleeSpellCast(false) || actor->HasUnitState(UNIT_STATE_IN_FLIGHT)
        || actor->GetTransport() || actor->GetVehicle())
        return reject("journey_taxi_actor_busy");
    // spellid=1 is the core's documented custom-call mode: real flight even
    // when CONFIG_INSTANT_TAXI is enabled. NPC and ordinary fare checks remain.
    result.started = actor->ActivateTaxiPathTo(route.taxiNodes, flightmaster, 1);
    result.reasonCode = result.started ? "journey_taxi_started" : "journey_taxi_rejected";
    result.fareCopper = static_cast<uint32>(fare);
    return result;
}

char const* VendorJourneyStageName(VendorJourneyStage stage)
{
    switch (stage)
    {
        case VendorJourneyStage::WalkingToFlightmaster: return "walking_to_flightmaster";
        case VendorJourneyStage::Flying: return "flying";
        case VendorJourneyStage::WalkingToDestination: return "walking_to_destination";
        case VendorJourneyStage::WaitingCombat: return "waiting_combat";
        case VendorJourneyStage::WaitingTransport: return "waiting_transport";
        case VendorJourneyStage::BoardingTransport: return "boarding_transport";
        case VendorJourneyStage::RidingTransport: return "riding_transport";
        case VendorJourneyStage::DisembarkingTransport: return "disembarking_transport";
        case VendorJourneyStage::WaitingPortal: return "waiting_portal";
        case VendorJourneyStage::FlyingMounted: return "flying_mounted";
        case VendorJourneyStage::Arrived: return "arrived";
        case VendorJourneyStage::Failed: return "failed";
        case VendorJourneyStage::Cancelled: return "cancelled";
    }
    return "unknown";
}

VendorJourney::VendorJourney(VendorJourneyRoute route, uint32 reservedPurchaseCopper)
    : _route(std::move(route)), _reserve(reservedPurchaseCopper),
      _stage(_route.mode == VendorTravelMode::Fly ? VendorJourneyStage::FlyingMounted
          : _route.UsesTaxi() ? VendorJourneyStage::WalkingToFlightmaster : VendorJourneyStage::WalkingToDestination),
      _resumeStage(_stage), _reason("journey_ready")
{
    if (_route.transportPlan)
        _transport = std::make_unique<VendorTransportJourney>(_route.transportPlan, _reserve);
}

VendorJourney::~VendorJourney() = default;

bool VendorJourney::IsNativeTransferPending() const
{
    return _transport && _transport->AllowsMapTransfer();
}

bool VendorJourney::IsTerminal() const
{
    return _stage == VendorJourneyStage::Arrived || _stage == VendorJourneyStage::Failed
        || _stage == VendorJourneyStage::Cancelled;
}

void VendorJourney::StopOwnedMovement(Player* actor)
{
    if (_ownsMovement && actor && actor->GetGUID().GetRawValue() == _actorGuid
        && !actor->HasUnitState(UNIT_STATE_IN_FLIGHT))
    {
        actor->StopMoving();
        // Caller guarantees exclusive task ownership while the journey is live.
        actor->GetMotionMaster()->Clear();
    }
    _ownsMovement = false;
}

void VendorJourney::Fail(Player* actor, char const* reason)
{
    StopOwnedMovement(actor);
    _stage = VendorJourneyStage::Failed;
    _reason = reason;
}

void VendorJourney::Cancel(Player* actor)
{
    if (IsTerminal())
        return;
    if (_transport)
        _transport->Cancel(actor);
    StopOwnedMovement(actor);
    _stage = VendorJourneyStage::Cancelled;
    _reason = "journey_cancelled"; // an in-progress native flight finishes normally
}

float VendorJourney::RemainingDistance(Player const* actor) const
{
    if (_transport)
        return _transport->RemainingDistance(actor);
    if (!actor)
        return std::numeric_limits<float>::infinity();
    auto const& target = _stage == VendorJourneyStage::WalkingToFlightmaster
        || (_stage == VendorJourneyStage::WaitingCombat && _resumeStage == VendorJourneyStage::WalkingToFlightmaster)
        ? _route.flightmasterLocation : _route.destination;
    return Distance(Location(actor), target);
}

bool VendorJourney::Walk(Player* actor, VendorJourneyLocation const& target, uint32 elapsedMs)
{
    VendorJourneyLocation const here = Location(actor);
    if (here.mapId != target.mapId)
    {
        Fail(actor, "unsupported_cross_map");
        return false;
    }
    if (_hasLastPosition && Distance(here, _lastPosition) >= 1.0f)
    {
        _stalledMs = 0;
        _lastPosition = here;
    }
    else
        _stalledMs += elapsedMs;
    if (!_hasLastPosition)
    {
        _lastPosition = here;
        _hasLastPosition = true;
    }
    if (_stalledMs > MAX_STALL_MS)
    {
        Fail(actor, "journey_route_stalled");
        return false;
    }
    if (Distance(here, target) <= 5.5f)
    {
        StopOwnedMovement(actor);
        _stalledMs = 0;
        _hasLastPosition = false;
        return true;
    }
    if (_repathMs < REPATH_INTERVAL_MS || actor->isMoving())
        return false;
    _repathMs = 0;
    // PathGenerator rejects unloaded mmap tiles. Load the terrain for a
    // nearby destination without spawning its objects or bypassing pathfinding.
    // Long journeys keep their existing bounded path behavior.
    if (Distance(here, target) <= 300.0f)
        actor->GetMap()->EnsureGridCreated(Acore::ComputeGridCoord(target.x, target.y));
    std::vector<VendorJourneyLocation> goals{target};
    constexpr std::array<float, 2> approachRadii{2.5f, 4.5f};
    constexpr float TWO_PI = 6.2831853071795864769f;
    for (float radius : approachRadii)
        for (uint32 index = 0; index < 8; ++index)
        {
            float const angle = TWO_PI * static_cast<float>(index) / 8.0f;
            VendorJourneyLocation candidate{target.mapId,
                target.x + std::cos(angle) * radius,
                target.y + std::sin(angle) * radius,
                target.z};
            actor->UpdateAllowedPositionZ(candidate.x, candidate.y, candidate.z);
            // Height correction can snap an upstairs NPC's approach point to
            // the ground below. Keep only points that remain interactable.
            if (Distance(candidate, target) <= 5.5f)
                goals.push_back(candidate);
        }

    bool foundPath = false;
    float bestScore = std::numeric_limits<float>::infinity();
    float bestX = 0.0f;
    float bestY = 0.0f;
    float bestZ = 0.0f;
    for (VendorJourneyLocation const& goal : goals)
    {
        // No slope check: it rejects ordinary stairs (e.g. in Ironforge) as
        // too steep, while players and Playerbot movement climb them fine.
        PathGenerator path(actor);
        path.CalculatePath(goal.x, goal.y, goal.z, false);
        PathType const type = path.GetPathType();
        if (!(type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_SHORT))
            || (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY_START))
            || path.GetPath().size() < 2)
            continue;

        // Prefer paths that end close to the actual NPC, while allowing a
        // navigable prefix for long trips that the mmap path buffer truncates.
        auto const& endpoint = path.GetPath().back();
        VendorJourneyLocation const end{target.mapId, endpoint.x, endpoint.y, endpoint.z};
        float const score = Distance(end, target) * 100.0f
            + Distance(end, goal) * 10.0f + Distance(goal, target);
        if (score < bestScore)
        {
            foundPath = true;
            bestScore = score;
            bestX = endpoint.x;
            bestY = endpoint.y;
            bestZ = endpoint.z;
        }
    }
    if (!foundPath)
    {
        _reason = "journey_waiting_for_ground_path";
        return false;
    }
    if (actor->GetExactDist(bestX, bestY, bestZ) < 1.0f)
        return false;
    actor->GetMotionMaster()->MovePoint(JOURNEY_POINT_ID, bestX, bestY, bestZ,
        FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, false);
    _ownsMovement = true;
    _reason = "journey_walking";
    return false;
}

bool VendorJourney::Land(Player* actor, uint32 elapsedMs)
{
    // A bot that followed a flying master keeps its flight flags while it
    // hovers or stands; the ground route needs it on its feet first.
    if (!actor->IsFlying() || actor->HasUnitState(UNIT_STATE_IN_FLIGHT)
        || actor->GetTransport() || actor->GetVehicle())
        return true;
    float const x = actor->GetPositionX();
    float const y = actor->GetPositionY();
    float const groundZ = actor->GetMapWaterOrGroundLevel(x, y, actor->GetPositionZ());
    _landingMs += elapsedMs;
    if (actor->GetPositionZ() - groundZ > 1.5f && _landingMs < MAX_LANDING_MS)
    {
        if (!actor->isMoving())
        {
            actor->GetMotionMaster()->MovePoint(JOURNEY_POINT_ID, x, y, groundZ,
                FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, true);
            _ownsMovement = true;
        }
        _reason = "journey_landing";
        return false;
    }
    StopOwnedMovement(actor);
    actor->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_CAN_FLY);
    if (_route.mode == VendorTravelMode::Walk)
        actor->RemoveAurasByType(SPELL_AURA_MOUNTED);
    actor->SendMovementFlagUpdate();
    _landingMs = 0;
    _repathMs = REPATH_INTERVAL_MS;
    _hasLastPosition = false;
    _stalledMs = 0;
    return !actor->IsFlying();
}

// Returns true while a mount cast is in progress (the caller waits). Indoors,
// in combat or without a suitable mount the bot simply goes on without one.
bool VendorJourney::TryMount(Player* actor, bool flying, uint32 elapsedMs)
{
    if (actor->IsNonMeleeSpellCast(false))
    {
        _reason = "journey_mounting";
        return true;
    }
    if (actor->IsMounted() || _mountAttempts >= MAX_MOUNT_ATTEMPTS || !actor->IsOutdoors()
        || actor->IsInCombat() || actor->IsInWater() || actor->IsInDisallowedMountForm())
        return false;
    uint32 const spell = FindMountSpell(actor, flying);
    if (!spell)
    {
        _mountAttempts = MAX_MOUNT_ATTEMPTS;
        return false;
    }
    // A mount has a cast time and fails while moving: halt, then cast next tick.
    if (actor->isMoving() || !actor->movespline->Finalized())
    {
        StopOwnedMovement(actor);
        actor->StopMoving();
        _mountWaitMs += elapsedMs;
        if (_mountWaitMs < MAX_MOUNT_STOP_MS)
        {
            _reason = "journey_stopping_to_mount";
            return true;
        }
    }
    _mountWaitMs = 0;
    ++_mountAttempts;
    SpellCastResult const cast = actor->CastSpell(actor, spell, false);
    _reason = cast == SPELL_CAST_OK ? "journey_mounting" : "journey_mount_failed";
    return actor->IsNonMeleeSpellCast(false);
}

void VendorJourney::TouchDown(Player* actor, char const* reason)
{
    StopOwnedMovement(actor);
    actor->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_CAN_FLY);
    actor->SendMovementFlagUpdate();
    // The rest of the way is on the ground; keep riding the mount there.
    _route.mode = VendorTravelMode::Mount;
    _stage = VendorJourneyStage::WalkingToDestination;
    _reason = reason;
    _repathMs = REPATH_INTERVAL_MS;
    _hasLastPosition = false;
    _stalledMs = 0;
}

void VendorJourney::FlyOwnMount(Player* actor, uint32 elapsedMs)
{
    VendorJourneyLocation const here = Location(actor);
    if (!_takenOff)
    {
        // Under a roof (Ironforge counts as outdoors for mounts) the climb would
        // pass through rock: come down first and walk out; take off only under
        // open sky, i.e. no surface above the bot.
        if (!actor->IsOutdoors() || TopSurface(actor, here.x, here.y, here.z) > here.z + 3.0f)
        {
            if (!Land(actor, elapsedMs))
                return;
            if (Walk(actor, _route.destination, elapsedMs))
            {
                _stage = VendorJourneyStage::Arrived;
                _reason = "journey_destination_reached";
            }
            else if (!IsTerminal())
                _reason = "journey_walking_outdoors_to_fly";
            return;
        }
        if (!actor->IsMounted())
        {
            if (TryMount(actor, true, elapsedMs))
                return;
            if (!actor->IsMounted())
            {
                TouchDown(actor, "journey_flight_unavailable");
                return;
            }
        }
        StopOwnedMovement(actor);
        actor->AddUnitMovementFlag(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_FLYING);
        actor->SendMovementFlagUpdate();
        _takenOff = true;
        _flightLeg = 0;
        _hasLastPosition = false;
        _stalledMs = 0;
    }

    if (_hasLastPosition && Distance(here, _lastPosition) >= 1.0f)
        _stalledMs = 0;
    else
        _stalledMs += elapsedMs;
    _lastPosition = here;
    _hasLastPosition = true;
    if (_stalledMs > MAX_FLIGHT_STALL_MS)
    {
        // Leave the air via the normal descent and finish on the ground.
        _route.mode = VendorTravelMode::Mount;
        _stage = VendorJourneyStage::WalkingToDestination;
        _reason = "journey_flight_stalled";
        _stalledMs = 0;
        _hasLastPosition = false;
        return;
    }
    if (!actor->movespline->Finalized())
        return;

    VendorJourneyLocation const& target = _route.destination;
    float const remaining = std::hypot(target.x - here.x, target.y - here.y);
    float const speed = std::max(actor->GetSpeed(MOVE_FLIGHT), actor->GetSpeed(MOVE_RUN));
    auto flyTo = [&](float x, float y, float z, char const* reason)
    {
        actor->GetMotionMaster()->MovePoint(JOURNEY_POINT_ID, x, y, z, FORCED_MOVEMENT_NONE, speed, 0.0f, false, true);
        _ownsMovement = true;
        _reason = reason;
    };

    if (_flightLeg == 0 && remaining > FLIGHT_LANDING_SEARCH)
    {
        // Hop toward the NPC inside loaded grids; climb first when the hop
        // needs more height so the straight line never cuts a hillside.
        float const hop = std::min(FLIGHT_HOP, remaining - FLIGHT_LANDING_SEARCH * 0.5f);
        float const x = here.x + (target.x - here.x) / remaining * hop;
        float const y = here.y + (target.y - here.y) / remaining * hop;
        float const z = CruiseHeight(actor, here.x, here.y, x, y, std::max(here.z, target.z) - FLIGHT_CLEARANCE);
        if (z > here.z + 3.0f)
            flyTo(here.x, here.y, z, "journey_flying_climb");
        else
            flyTo(x, y, z, "journey_flying");
        return;
    }
    if (_flightLeg == 0)
    {
        if (actor->GetDistance2d(target.x, target.y) <= 300.0f)
            actor->GetMap()->EnsureGridCreated(Acore::ComputeGridCoord(target.x, target.y));
        if (!PickOpenLanding(actor, target, _landing))
        {
            TouchDown(actor, "journey_no_open_landing");
            return;
        }
        _hasLanding = true;
        _flightLeg = 1;
    }
    if (_flightLeg == 1)
    {
        float const z = CruiseHeight(actor, here.x, here.y, _landing.x, _landing.y, _landing.z);
        if (z > here.z + 3.0f)
        {
            flyTo(here.x, here.y, z, "journey_flying_climb");
            return;
        }
        if (std::hypot(_landing.x - here.x, _landing.y - here.y) > 2.0f)
        {
            flyTo(_landing.x, _landing.y, std::max(z, here.z), "journey_flying_to_landing");
            return;
        }
        _flightLeg = 2;
    }
    if (here.z - _landing.z > 1.5f)
    {
        flyTo(_landing.x, _landing.y, _landing.z, "journey_descending");
        return;
    }
    TouchDown(actor, "journey_landed");
}

VendorJourneyStage VendorJourney::Tick(Player* actor, uint32 elapsedMs)
{
    if (IsTerminal())
        return _stage;
    if (_stage != VendorJourneyStage::FlyingMounted && actor && actor->IsInWorld() && !Land(actor, elapsedMs))
        return _stage;
    if (_transport)
    {
        _stage = _transport->Tick(actor, elapsedMs);
        _reason = _transport->Reason();
        return _stage;
    }
    _elapsedMs += elapsedMs;
    if (!actor || !actor->IsInWorld() || !actor->GetSession())
    {
        Fail(actor, "journey_actor_unavailable");
        return _stage;
    }
    if (!_actorGuid)
        _actorGuid = actor->GetGUID().GetRawValue();
    if (_actorGuid != actor->GetGUID().GetRawValue())
    {
        Fail(nullptr, "journey_actor_changed");
        return _stage;
    }
    if (_elapsedMs > MAX_JOURNEY_MS || !actor->IsAlive())
    {
        Fail(actor, actor->IsAlive() ? "journey_timeout" : "journey_actor_dead");
        return _stage;
    }
    if (actor->GetMapId() != _route.destination.mapId || !ValidLocation(_route.destination)
        || (_route.UsesTaxi() && !ValidLocation(_route.flightmasterLocation)))
    {
        Fail(actor, "unsupported_cross_map");
        return _stage;
    }
    if (_stage == VendorJourneyStage::Flying)
    {
        if (actor->HasUnitState(UNIT_STATE_IN_FLIGHT))
            return _stage;
        TaxiNodesEntry const* endpoint = sTaxiNodesStore.LookupEntry(_route.taxiNodes.back());
        if (!endpoint || Distance(Location(actor), Location(endpoint)) > 100.0f)
        {
            Fail(actor, "journey_taxi_landing_mismatch");
            return _stage;
        }
        _stage = VendorJourneyStage::WalkingToDestination;
        _reason = "journey_taxi_landed";
        _repathMs = REPATH_INTERVAL_MS;
        _hasLastPosition = false;
        _stalledMs = 0;
    }
    if (actor->HasUnitState(UNIT_STATE_IN_FLIGHT))
    {
        Fail(actor, "journey_external_taxi_started");
        return _stage;
    }
    if (_stage == VendorJourneyStage::FlyingMounted && !actor->GetTransport() && !actor->GetVehicle())
    {
        FlyOwnMount(actor, elapsedMs);
        return _stage;
    }
    if (actor->IsFlying() || actor->GetTransport() || actor->GetVehicle()
        || actor->GetMap()->IsDungeon() || actor->InBattleground())
    {
        Fail(actor, "journey_world_ground_route_required");
        return _stage;
    }
    if (actor->IsInCombat())
    {
        if (_stage != VendorJourneyStage::WaitingCombat)
        {
            _resumeStage = _stage;
            StopOwnedMovement(actor);
        }
        _stage = VendorJourneyStage::WaitingCombat;
        _reason = "journey_waiting_combat";
        return _stage;
    }
    if (_stage == VendorJourneyStage::WaitingCombat)
    {
        _stage = _resumeStage;
        _hasLastPosition = false;
        _stalledMs = 0;
    }
    _repathMs = std::min<uint32>(REPATH_INTERVAL_MS, _repathMs + elapsedMs);
    if (_stage == VendorJourneyStage::WalkingToFlightmaster)
    {
        Creature* flightmaster = FindFlightmaster(actor, _route);
        bool const interactable = flightmaster
            && actor->GetNPCIfCanInteractWith(flightmaster->GetGUID(), UNIT_NPC_FLAG_FLIGHTMASTER) == flightmaster;
        if (!interactable)
        {
            if (_route.mode != VendorTravelMode::Walk && TryMount(actor, false, elapsedMs))
                return _stage;
            if (Walk(actor, _route.flightmasterLocation, elapsedMs))
            {
                Fail(actor, "journey_flightmaster_unavailable");
                return _stage;
            }
            return _stage;
        }
        StopOwnedMovement(actor);
        _boardingWaitMs += elapsedMs;
        if (actor->IsMounted())
        {
            WorldPacket cancel(CMSG_CANCEL_MOUNT_AURA);
            actor->GetSession()->HandleCancelMountAuraOpcode(cancel);
            _reason = "journey_dismounting_for_taxi";
            if (_boardingWaitMs > MAX_BOARDING_WAIT_MS)
                Fail(actor, "journey_taxi_actor_busy");
            return _stage;
        }
        if (actor->IsNonMeleeSpellCast(false))
        {
            if (_boardingWaitMs > MAX_BOARDING_WAIT_MS)
                Fail(actor, "journey_taxi_actor_busy");
            return _stage;
        }
        VendorTaxiActivation const result = ActivateVendorJourneyTaxi(actor, flightmaster, _route, _reserve);
        if (!result.started)
            Fail(actor, result.reasonCode.c_str());
        else
        {
            _stage = VendorJourneyStage::Flying;
            _reason = result.reasonCode;
            _ownsMovement = false;
        }
        return _stage;
    }
    if (actor->IsNonMeleeSpellCast(false))
    {
        _reason = "journey_waiting_cast";
        return _stage;
    }
    if (_route.mode != VendorTravelMode::Walk && TryMount(actor, false, elapsedMs))
        return _stage;
    if (Walk(actor, _route.destination, elapsedMs))
    {
        _stage = VendorJourneyStage::Arrived;
        _reason = "journey_destination_reached";
    }
    return _stage;
}
