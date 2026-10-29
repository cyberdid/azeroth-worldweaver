#include "vendor_journey.h"
#include "vendor_transport.h"

#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
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
    constexpr float ARRIVAL_DISTANCE = 2.0f;
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
        case VendorJourneyStage::Arrived: return "arrived";
        case VendorJourneyStage::Failed: return "failed";
        case VendorJourneyStage::Cancelled: return "cancelled";
    }
    return "unknown";
}

VendorJourney::VendorJourney(VendorJourneyRoute route, uint32 reservedPurchaseCopper)
    : _route(std::move(route)), _reserve(reservedPurchaseCopper),
      _stage(_route.UsesTaxi() ? VendorJourneyStage::WalkingToFlightmaster : VendorJourneyStage::WalkingToDestination),
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
    if (Distance(here, target) <= ARRIVAL_DISTANCE)
    {
        StopOwnedMovement(actor);
        _stalledMs = 0;
        _hasLastPosition = false;
        return true;
    }
    if (_repathMs < REPATH_INTERVAL_MS || actor->isMoving())
        return false;
    _repathMs = 0;
    PathGenerator path(actor);
    path.SetSlopeCheck(true);
    path.CalculatePath(target.x, target.y, target.z, false);
    PathType const type = path.GetPathType();
    if (!(type & (PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_SHORT))
        || (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY_START))
        || path.GetPath().size() < 2)
    {
        _reason = "journey_waiting_for_ground_path";
        return false;
    }
    // A long ground path may be incomplete because the mmap path buffer is
    // bounded. Advance along its actual navigable prefix, then recalculate.
    auto const& endpoint = path.GetPath().back();
    if (actor->GetExactDist(endpoint.x, endpoint.y, endpoint.z) < 1.0f)
        return false;
    actor->GetMotionMaster()->MovePoint(JOURNEY_POINT_ID, endpoint.x, endpoint.y, endpoint.z,
        FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, false);
    _ownsMovement = true;
    _reason = "journey_walking";
    return false;
}

VendorJourneyStage VendorJourney::Tick(Player* actor, uint32 elapsedMs)
{
    if (IsTerminal())
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
    if (Walk(actor, _route.destination, elapsedMs))
    {
        _stage = VendorJourneyStage::Arrived;
        _reason = "journey_destination_reached";
    }
    return _stage;
}
