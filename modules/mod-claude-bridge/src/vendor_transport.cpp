#include "vendor_transport.h"

#include "DBCStores.h"
#include "GameObjectModel.h"
#include "EventProcessor.h"
#include "MoveSpline.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "Transport.h"
#include "TransportMgr.h"
#include "TravelNode.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <limits>
#include <queue>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace
{
    constexpr uint32 BOARD_JUMP_ID = 0x574A424F;
    constexpr uint32 SHORE_JUMP_ID = 0x574A5348;
    constexpr uint32 MAX_TRANSFER_MS = 60000;
    constexpr uint32 MAX_TRANSPORT_LEG_MS = 20 * 60 * 1000;
    constexpr uint64 MAX_INTERMAP_JOURNEY_MS = 2ULL * 60 * 60 * 1000;
    constexpr float MAX_JUMP_DISTANCE = 7.5f;
    constexpr float MAX_JUMP_HEIGHT = 2.5f;
    constexpr std::size_t MAX_GRAPH_NODES = 50000;
    constexpr std::size_t MAX_GRAPH_EDGES = 250000;
    constexpr std::size_t MAX_GRAPH_POINTS = 500000;
    constexpr std::size_t MAX_ROUTE_LEGS = 128;

    VendorJourneyLocation Location(Player const* actor)
    {
        return {actor->GetMapId(), actor->GetPositionX(), actor->GetPositionY(), actor->GetPositionZ()};
    }

    VendorJourneyLocation Location(WorldPosition const& point)
    {
        return {point.GetMapId(), point.GetPositionX(), point.GetPositionY(), point.GetPositionZ()};
    }

    float Distance(VendorJourneyLocation const& a, VendorJourneyLocation const& b)
    {
        if (a.mapId != b.mapId)
            return std::numeric_limits<float>::infinity();
        float const x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
        return std::sqrt(x * x + y * y + z * z);
    }

    bool IsWorldLocation(VendorJourneyLocation const& point)
    {
        MapEntry const* map = sMapStore.LookupEntry(point.mapId);
        return map && map->IsWorldMap() && std::isfinite(point.x) && std::isfinite(point.y)
            && std::isfinite(point.z) && std::abs(point.x) < 17066 && std::abs(point.y) < 17066
            && std::abs(point.z) < 17066;
    }

    bool CanUseOrdinaryMovement(Player* actor)
    {
        return !actor->HasUnitState(UNIT_STATE_LOST_CONTROL) && !actor->IsRooted() && !actor->IsCharmed()
            && !actor->isFrozen() && !actor->IsPolymorphed() && !actor->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE);
    }

    bool IsStopAt(TransportTemplate const* info, VendorJourneyLocation const& point)
    {
        if (!info || !info->pathTime)
            return false;
        return std::any_of(info->keyFrames.begin(), info->keyFrames.end(), [&point](KeyFrame const& frame)
        {
            return frame.Node && frame.IsStopFrame()
                && Distance(point, {frame.Node->mapid, frame.Node->x, frame.Node->y, frame.Node->z}) < 20.0f;
        });
    }

    bool StoppedAt(MotionTransport* transport, VendorJourneyLocation const& point, uint32 minimumRemainingMs = 0)
    {
        if (!transport || !transport->IsInWorld() || !transport->GetPeriod()
            || transport->GetMapId() != point.mapId
            || transport->GetExactDist(point.x, point.y, point.z) > 5.0f)
            return false;
        uint32 const progress = transport->GetPathProgress() % transport->GetPeriod();
        for (KeyFrame const& frame : transport->GetKeyFrames())
            if (frame.Node && frame.IsStopFrame() && frame.Node->mapid == point.mapId
                && Distance(point, {frame.Node->mapid, frame.Node->x, frame.Node->y, frame.Node->z}) < 20.0f
                && progress >= frame.ArriveTime && progress < frame.DepartureTime
                && frame.DepartureTime - progress >= minimumRemainingMs)
                return true;
        return false;
    }

    MotionTransport* FindStoppedTransport(Player* actor, VendorTransportLeg const& leg, bool departure)
    {
        for (Transport* transport : actor->GetMap()->GetAllTransports())
            if (transport && transport->GetEntry() == leg.entry && transport->IsInWorld()
                && (transport->GetPhaseMask() & actor->GetPhaseMask())
                && transport->GetGOInfo()->type == GAMEOBJECT_TYPE_MO_TRANSPORT)
            {
                auto* ship = static_cast<MotionTransport*>(transport);
                if (StoppedAt(ship, departure ? leg.from : leg.to, 4000))
                    return ship;
            }
        return nullptr;
    }

    MotionTransport* CurrentShip(Player* actor, uint64 guid)
    {
        Transport* transport = actor->GetTransport();
        return transport && transport->GetGUID().GetRawValue() == guid
            && transport->GetGOInfo()->type == GAMEOBJECT_TYPE_MO_TRANSPORT
            ? static_cast<MotionTransport*>(transport) : nullptr;
    }

    bool DeckHeight(Player* actor, MotionTransport* ship, float x, float y, float& z)
    {
        if (!ship || !ship->m_model)
            return false;
        float const top = actor->GetPositionZ() + 3.0f;
        G3D::Ray const ray(G3D::Vector3(x, y, top), G3D::Vector3(0, 0, -1));
        float distance = 6.0f;
        if (!ship->m_model->intersectRay(ray, distance, false, actor->GetPhaseMask(), VMAP::ModelIgnoreFlags::Nothing))
            return false;
        z = top - distance;
        return std::abs(z - actor->GetPositionZ()) <= MAX_JUMP_HEIGHT;
    }

    bool SettleOnActualDeck(Player* actor, MotionTransport* ship)
    {
        if (!ship || !ship->m_model)
            return false;
        float const top = actor->GetPositionZ() + 3.0f;
        float distance = 12.0f;
        G3D::Ray const ray(G3D::Vector3(actor->GetPositionX(), actor->GetPositionY(), top),
            G3D::Vector3(0, 0, -1));
        if (!ship->m_model->intersectRay(ray, distance, false, actor->GetPhaseMask(), VMAP::ModelIgnoreFlags::Nothing))
            return false;
        float const deck = top - distance;
        if (std::abs(deck - actor->GetPositionZ()) <= 0.75f && actor->movespline->Finalized())
            return true;
        if (actor->movespline->Finalized() && std::abs(deck - actor->GetPositionZ()) <= 8.0f)
            actor->GetMotionMaster()->MoveLand(BOARD_JUMP_ID, actor->GetPositionX(), actor->GetPositionY(), deck, 7.0f);
        return false;
    }

    bool FindJumpPoint(Player* actor, MotionTransport* ship, bool boarding,
        VendorJourneyLocation const& onward, VendorJourneyLocation& result)
    {
        float best = std::numeric_limits<float>::infinity();
        // Only short, physically adjacent deck/shore points; no placement onto
        // remote decks, no midair attach, and no jumping down into water.
        for (float radius = 0; radius <= MAX_JUMP_DISTANCE; radius += 1.5f)
            for (uint32 angleIndex = 0; angleIndex < 24; ++angleIndex)
            {
                float const angle = float(angleIndex) * 6.283185307f / 24;
                float const x = actor->GetPositionX() + std::cos(angle) * radius;
                float const y = actor->GetPositionY() + std::sin(angle) * radius;
                float z = 0;
                if (boarding)
                {
                    if (!DeckHeight(actor, ship, x, y, z))
                        continue;
                }
                else
                {
                    z = actor->GetMap()->GetHeight(actor->GetPhaseMask(), x, y,
                        actor->GetPositionZ() + 3.0f, true, 6.0f);
                    if (!std::isfinite(z) || std::abs(z - actor->GetPositionZ()) > MAX_JUMP_HEIGHT
                        || actor->GetMap()->GetTransportForPos(actor->GetPhaseMask(), x, y, z, actor))
                        continue;
                }
                VendorJourneyLocation const point{actor->GetMapId(), x, y, z};
                if (Distance(Location(actor), point) > MAX_JUMP_DISTANCE
                    || !actor->IsWithinLOS(x, y, z + 1.0f))
                    continue;
                // Board at the closest actual deck point. Leave toward the next
                // local destination where possible, with short jumps preferred.
                float const score = radius + (boarding ? 0.0f : 0.05f * Distance(point, onward));
                if (score < best)
                {
                    best = score;
                    result = point;
                }
            }
        return std::isfinite(best);
    }

    bool FindDeckApproachToShore(Player* actor, MotionTransport* ship,
        VendorJourneyLocation const& onward, VendorJourneyLocation& result)
    {
        float best = std::numeric_limits<float>::infinity();
        for (float radius = 4.0f; radius <= 24.0f; radius += 4.0f)
            for (uint32 angleIndex = 0; angleIndex < 16; ++angleIndex)
            {
                float const angle = float(angleIndex) * 6.283185307f / 16;
                VendorJourneyLocation deck{actor->GetMapId(), actor->GetPositionX() + std::cos(angle) * radius,
                    actor->GetPositionY() + std::sin(angle) * radius, 0};
                if (!DeckHeight(actor, ship, deck.x, deck.y, deck.z)
                    || !actor->IsWithinLOS(deck.x, deck.y, deck.z + 1.0f))
                    continue;
                bool supported = true;
                for (float step = 1.0f; step < radius; step += 1.0f)
                {
                    float height = 0;
                    if (!DeckHeight(actor, ship, actor->GetPositionX() + std::cos(angle) * step,
                        actor->GetPositionY() + std::sin(angle) * step, height)
                        || std::abs(height - actor->GetPositionZ()) > 1.5f)
                    {
                        supported = false;
                        break;
                    }
                }
                if (!supported)
                    continue;
                for (uint32 shoreAngle = 0; shoreAngle < 12; ++shoreAngle)
                {
                    float const direction = float(shoreAngle) * 6.283185307f / 12;
                    float const x = deck.x + std::cos(direction) * 6.0f;
                    float const y = deck.y + std::sin(direction) * 6.0f;
                    float const z = actor->GetMap()->GetHeight(actor->GetPhaseMask(), x, y, deck.z + 3.0f, true, 6.0f);
                    if (!std::isfinite(z) || std::abs(z - deck.z) > MAX_JUMP_HEIGHT
                        || actor->GetMap()->GetTransportForPos(actor->GetPhaseMask(), x, y, z, actor))
                        continue;
                    float const score = radius + 0.05f * Distance({actor->GetMapId(), x, y, z}, onward);
                    if (score < best)
                    {
                        best = score;
                        result = deck;
                    }
                }
            }
        return std::isfinite(best);
    }

    // Finish the physical landing independently of task lifetime. Cancelling a
    // route during a visible jump must not leave a bot standing on a departing
    // deck without a passenger attachment, or attached after landing ashore.
    // Event lifetime belongs to the player, and every operation revalidates the
    // actual landing position and ship GUID; no route continuation occurs here.
    class TransportLandingEvent final : public BasicEvent
    {
    public:
        TransportLandingEvent(Player* actor, uint64 guid, VendorJourneyLocation point, bool boarding)
            : _actor(actor), _guid(guid), _point(point), _boarding(boarding) {}

        bool Execute(uint64 /*time*/, uint32 /*diff*/) override
        {
            if (!_actor->IsInWorld() || _actor->IsBeingTeleported() || !_actor->IsAlive())
                return true;
            Transport* object = _actor->GetMap()->GetTransport(ObjectGuid(_guid));
            if (!object || object->GetGOInfo()->type != GAMEOBJECT_TYPE_MO_TRANSPORT)
                return true;
            auto* ship = static_cast<MotionTransport*>(object);
            if (_boarding && _actor->GetTransport())
            {
                if (_actor->GetTransport() != ship || SettleOnActualDeck(_actor, ship))
                    return true;
            }
            if (!_boarding && _actor->GetTransport() && _actor->GetTransport() != ship)
                return true;
            // The native bot's one-second transport check can detach and stop
            // a jump as soon as its feet leave the deck projection. Finish the
            // remaining short ordinary jump once; never relocate to the shore.
            if (!_boarding && !_actor->GetTransport() && !_restarted
                && _actor->movespline->Finalized() && Distance(Location(_actor), _point) > 1.5f
                && Distance(Location(_actor), _point) <= MAX_JUMP_DISTANCE)
            {
                float const shore = _actor->GetMap()->GetHeight(_actor->GetPhaseMask(), _point.x, _point.y,
                    _point.z + 2.0f, true, 4.0f);
                if (std::abs(shore - _point.z) < 1.5f)
                {
                    _restarted = true;
                    _actor->GetMotionMaster()->MoveJump(_point.x, _point.y, _point.z, 7.0f, 8.0f, SHORE_JUMP_ID);
                }
            }
            if (_actor->movespline->Finalized() && Distance(Location(_actor), _point) < 1.5f)
            {
                if (_boarding)
                {
                    float deck = 0;
                    if (DeckHeight(_actor, ship, _actor->GetPositionX(), _actor->GetPositionY(), deck)
                        && std::abs(deck - _actor->GetPositionZ()) < 1.5f)
                    {
                        ship->AddPassenger(_actor, true);
                        _actor->m_movementInfo.transport.time = ship->GetPathProgress();
                        return true;
                    }
                }
                else
                {
                    float const shore = _actor->GetMap()->GetHeight(_actor->GetPhaseMask(), _point.x, _point.y,
                        _point.z + 2.0f, true, 4.0f);
                    if (std::abs(shore - _actor->GetPositionZ()) < 1.5f
                        && !_actor->GetMap()->GetTransportForPos(_actor->GetPhaseMask(), _actor->GetPositionX(),
                            _actor->GetPositionY(), _actor->GetPositionZ(), _actor))
                    {
                        if (_actor->GetTransport() == ship)
                            ship->RemovePassenger(_actor, true);
                        return true;
                    }
                }
            }
            if (++_attempts >= 25)
                return true;
            _actor->m_Events.AddEvent(this, _actor->m_Events.CalculateTime(200));
            return false;
        }
    private:
        Player* _actor; // event owner; destroyed with that player's EventProcessor
        uint64 _guid;
        VendorJourneyLocation _point;
        bool _boarding;
        bool _restarted = false;
        uint32 _attempts = 0;
    };

    void ScheduleLanding(Player* actor, uint64 guid, VendorJourneyLocation point, bool boarding)
    {
        actor->m_Events.AddEvent(new TransportLandingEvent(actor, guid, point, boarding),
            actor->m_Events.CalculateTime(200));
    }

    struct GraphEdge
    {
        std::size_t to;
        VendorTransportLegKind kind;
        uint32 entry;
        float seconds;
        std::vector<VendorJourneyLocation> points;
    };

    struct GraphNode
    {
        VendorJourneyLocation point;
        bool transport;
        std::vector<GraphEdge> edges;
    };
}

VendorTransportPlanningResult PlanVendorTransport(Player* actor, VendorJourneyLocation destination)
{
    VendorTransportPlanningResult result;
    auto reject = [&result](char const* reason) { result.reasonCode = reason; return result; };
    if (!actor || !actor->IsInWorld() || !actor->IsAlive() || !IsWorldLocation(destination))
        return reject("journey_intermap_invalid_context");
    std::vector<GraphNode> graph;
    {
        TravelNodeMap& map = TravelNodeMap::instance();
        std::shared_lock<std::shared_timed_mutex> lock(map.m_nMapMtx, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(50)))
            return reject("journey_intermap_graph_busy");
        std::size_t copiedEdges = 0, copiedPoints = 0;
        std::vector<TravelNode*> const nodes = map.getNodes();
        if (nodes.empty() || nodes.size() > MAX_GRAPH_NODES)
            return reject("journey_intermap_graph_unavailable");
        std::unordered_map<TravelNode*, std::size_t> ids;
        for (TravelNode* node : nodes)
            if (node && IsWorldLocation(Location(*node->getPosition())))
            {
                ids[node] = graph.size();
                graph.push_back({Location(*node->getPosition()), node->isTransport(), {}});
            }
        for (auto const& [node, index] : ids)
            for (auto const& [next, path] : *node->getLinks())
            {
                auto const to = ids.find(next);
                if (to == ids.end() || !path || !path->getComplete())
                    continue;
                GraphEdge edge{to->second, VendorTransportLegKind::Walk, path->getPathObject(), 0, {}};
                auto const& fromPoint = graph[index].point;
                auto const& toPoint = graph[to->second].point;
                switch (path->getPathType())
                {
                    case TravelNodePathType::walk:
                        if (fromPoint.mapId != toPoint.mapId)
                            continue;
                        edge.seconds = std::max(1.0f, path->getDistance()) / std::max(1.0f, actor->GetSpeed(MOVE_RUN));
                        for (WorldPosition const& point : path->getPath())
                            if (point.GetMapId() == fromPoint.mapId && IsWorldLocation(Location(point)))
                            {
                                VendorJourneyLocation const location = Location(point);
                                if (edge.points.empty() || Distance(edge.points.back(), location) >= 40.0f)
                                    edge.points.push_back(location);
                            }
                        if (edge.points.empty() || Distance(edge.points.back(), toPoint) > 2.0f)
                            edge.points.push_back(toPoint);
                        break;
                    case TravelNodePathType::portal:
                    {
                        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(edge.entry);
                        AreaTriggerTeleport const* target = sObjectMgr->GetAreaTriggerTeleport(edge.entry);
                        if (!edge.entry || !trigger || !target
                            || Distance(fromPoint, {trigger->map, trigger->x, trigger->y, trigger->z}) > 20.0f
                            || Distance(toPoint, {target->target_mapId, target->target_X, target->target_Y, target->target_Z}) > 20.0f)
                            continue;
                        edge.kind = VendorTransportLegKind::AreaTrigger;
                        edge.seconds = 10.0f;
                        break;
                    }
                    case TravelNodePathType::transport:
                    {
                        TransportTemplate const* info = sTransportMgr->GetTransportTemplate(edge.entry);
                        if (!IsStopAt(info, fromPoint) || !IsStopAt(info, toPoint))
                            continue;
                        edge.kind = VendorTransportLegKind::Transport;
                        edge.seconds = float(info->pathTime) / 1000.0f; // conservative full-cycle wait+ride hint
                        break;
                    }
                    default:
                        continue; // no teleportSpell, unnamed portal, or unsupported flight edge
                }
                if (std::isfinite(edge.seconds))
                {
                    copiedPoints += edge.points.size();
                    if (++copiedEdges > MAX_GRAPH_EDGES || copiedPoints > MAX_GRAPH_POINTS)
                        return reject("journey_intermap_graph_too_large");
                    graph[index].edges.push_back(std::move(edge));
                }
            }
    }
    if (graph.empty())
        return reject("journey_intermap_graph_unavailable");
    VendorJourneyLocation const start = Location(actor);
    float const speed = std::max(1.0f, actor->GetSpeed(MOVE_RUN));
    std::vector<std::pair<float, std::size_t>> starts, goals;
    for (std::size_t i = 0; i < graph.size(); ++i)
    {
        if (!graph[i].transport && graph[i].point.mapId == start.mapId)
            starts.emplace_back(Distance(start, graph[i].point) / speed, i);
        if (!graph[i].transport && graph[i].point.mapId == destination.mapId)
            goals.emplace_back(Distance(destination, graph[i].point) / speed, i);
    }
    if (starts.empty() || goals.empty())
        return reject("journey_intermap_no_graph_endpoint");
    std::sort(starts.begin(), starts.end());
    std::sort(goals.begin(), goals.end());
    starts.resize(std::min<std::size_t>(8, starts.size()));
    goals.resize(std::min<std::size_t>(8, goals.size()));
    float const infinity = std::numeric_limits<float>::infinity();
    std::vector<float> costs(graph.size(), infinity);
    std::vector<std::size_t> previous(graph.size(), graph.size()), previousEdge(graph.size(), 0);
    using QueueEntry = std::pair<float, std::size_t>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;
    for (auto const& [cost, node] : starts)
    {
        costs[node] = cost;
        queue.push({cost, node});
    }
    while (!queue.empty())
    {
        auto const [cost, node] = queue.top();
        queue.pop();
        if (cost != costs[node])
            continue;
        for (std::size_t edgeIndex = 0; edgeIndex < graph[node].edges.size(); ++edgeIndex)
        {
            GraphEdge const& edge = graph[node].edges[edgeIndex];
            float const nextCost = cost + edge.seconds;
            if (nextCost >= costs[edge.to])
                continue;
            costs[edge.to] = nextCost;
            previous[edge.to] = node;
            previousEdge[edge.to] = edgeIndex;
            queue.push({nextCost, edge.to});
        }
    }
    std::size_t goal = graph.size();
    float best = infinity;
    for (auto const& [tailCost, node] : goals)
        if (costs[node] + tailCost < best)
        {
            best = costs[node] + tailCost;
            goal = node;
        }
    if (goal == graph.size())
        return reject("journey_intermap_no_supported_route");
    std::vector<std::pair<std::size_t, std::size_t>> route;
    std::size_t cursor = goal;
    while (previous[cursor] != graph.size())
    {
        route.emplace_back(previous[cursor], previousEdge[cursor]);
        cursor = previous[cursor];
        if (route.size() > graph.size())
            return reject("journey_intermap_invalid_graph_cycle");
    }
    std::reverse(route.begin(), route.end());
    auto plan = std::make_shared<VendorTransportPlan>();
    VendorTransportLeg walk;
    walk.from = start;
    walk.to = graph[cursor].point;
    walk.waypoints.push_back(walk.to);
    for (auto const& [node, edgeIndex] : route)
    {
        GraphEdge const& edge = graph[node].edges[edgeIndex];
        if (edge.kind == VendorTransportLegKind::Walk)
        {
            walk.to = graph[edge.to].point;
            walk.waypoints.insert(walk.waypoints.end(), edge.points.begin(), edge.points.end());
        }
        else
        {
            if (!walk.waypoints.empty())
                plan->legs.push_back(std::move(walk));
            plan->legs.push_back({edge.kind, graph[node].point, graph[edge.to].point, edge.entry, {}});
            walk = {};
            walk.from = graph[edge.to].point;
            walk.to = walk.from;
        }
        if (plan->legs.size() > MAX_ROUTE_LEGS)
            return reject("journey_intermap_route_too_complex");
    }
    walk.to = destination;
    walk.waypoints.push_back(destination);
    plan->legs.push_back(std::move(walk));
    plan->estimatedSeconds = best;
    result.plan = std::move(plan);
    result.reasonCode = "journey_intermap_route_planned";
    return result;
}

VendorTransportJourney::VendorTransportJourney(std::shared_ptr<VendorTransportPlan> plan, uint32 purchaseReserveCopper)
    : _plan(std::move(plan)), _reserve(purchaseReserveCopper)
{
}
VendorTransportJourney::~VendorTransportJourney() = default;

bool VendorTransportJourney::AllowsMapTransfer() const
{
    return _stage == VendorJourneyStage::RidingTransport || _stage == VendorJourneyStage::DisembarkingTransport
        || (_stage == VendorJourneyStage::WaitingPortal && _portalInvoked);
}

void VendorTransportJourney::StopLocal(Player* actor)
{
    if (_local)
    {
        _local->Cancel(actor);
        _local.reset();
    }
}

void VendorTransportJourney::Fail(Player* actor, char const* reason)
{
    StopLocal(actor);
    _stage = VendorJourneyStage::Failed;
    _reason = reason;
    // Passenger attachment is deliberately preserved on failure or cancellation.
    // Detaching at sea would drop the bot into water or out of the world.
}

void VendorTransportJourney::Cancel(Player* actor)
{
    StopLocal(actor);
    _cancelled = true;
    _stage = VendorJourneyStage::Cancelled;
    _reason = "journey_cancelled";
}

void VendorTransportJourney::NextLeg(Player* actor)
{
    StopLocal(actor);
    ++_legIndex;
    _waypointIndex = 0;
    _legElapsedMs = 0;
    _jumpElapsedMs = 0;
    _transferElapsedMs = 0;
    _jumping = false;
    _disembarking = false;
    _deckWalking = false;
    _deckWalkMs = 0;
    _portalInvoked = false;
    _walkChoiceMade = false;
    _usesLocalTaxi = false;
    _localTaxiDeparted = false;
    _transportGuid = 0;
    _stage = _legIndex == _plan->legs.size() ? VendorJourneyStage::Arrived : VendorJourneyStage::WalkingToDestination;
    _reason = _stage == VendorJourneyStage::Arrived ? "journey_destination_reached" : "journey_next_leg";
}

float VendorTransportJourney::RemainingDistance(Player const* actor) const
{
    if (!actor || !_plan || _legIndex >= _plan->legs.size())
        return 0;
    if (_local)
        return _local->RemainingDistance(actor);
    VendorTransportLeg const& leg = _plan->legs[_legIndex];
    return Distance(Location(actor), actor->GetMapId() == leg.from.mapId ? leg.from : leg.to);
}

bool VendorTransportJourney::TickWalk(Player* actor, VendorJourneyLocation const& destination,
    uint32 elapsedMs, bool allowTaxi)
{
    if (!_local)
    {
        VendorJourneyRoute route;
        route.destination = destination;
        if (allowTaxi)
        {
            VendorJourneyPlan const options = PlanVendorJourney(actor, destination, _reserve);
            if (options.candidates.empty())
            {
                Fail(actor, options.reasonCode.c_str());
                return false;
            }
            route = options.candidates.front();
        }
        _local = std::make_unique<VendorJourney>(std::move(route), _reserve);
    }
    VendorJourneyStage const stage = _local->Tick(actor, elapsedMs);
    _stage = stage;
    _localTaxiDeparted = _localTaxiDeparted || stage == VendorJourneyStage::Flying;
    _reason = _local->Reason();
    if (stage == VendorJourneyStage::Arrived)
    {
        _local.reset();
        _stage = VendorJourneyStage::WalkingToDestination;
        _reason = "journey_local_leg_reached";
        return true;
    }
    if (stage == VendorJourneyStage::Failed)
        Fail(actor, _reason.c_str());
    return false;
}

VendorJourneyStage VendorTransportJourney::Tick(Player* actor, uint32 elapsedMs)
{
    if (_cancelled || _stage == VendorJourneyStage::Arrived || _stage == VendorJourneyStage::Failed)
        return _stage;
    _elapsedMs += elapsedMs;
    _legElapsedMs += elapsedMs;
    if (!actor || !actor->GetSession() || !_plan || _legIndex >= _plan->legs.size())
    {
        Fail(actor, "journey_intermap_invalid_context");
        return _stage;
    }
    if (!_actorGuid)
        _actorGuid = actor->GetGUID().GetRawValue();
    if (_actorGuid != actor->GetGUID().GetRawValue())
    {
        Fail(nullptr, "journey_actor_changed");
        return _stage;
    }
    if (_elapsedMs > MAX_INTERMAP_JOURNEY_MS || !actor->IsAlive())
    {
        Fail(actor, actor->IsAlive() ? "journey_timeout" : "journey_actor_dead");
        return _stage;
    }
    if (!actor->IsInWorld() || actor->IsBeingTeleported())
    {
        _transferElapsedMs += elapsedMs;
        if (!AllowsMapTransfer() || _transferElapsedMs > MAX_TRANSFER_MS)
            Fail(actor, "journey_map_transfer_timeout");
        return _stage;
    }
    _transferElapsedMs = 0;
    VendorTransportLeg const& leg = _plan->legs[_legIndex];
    if (leg.kind == VendorTransportLegKind::Walk)
    {
        if (actor->GetMapId() != leg.to.mapId)
        {
            Fail(actor, "journey_unexpected_map_change");
            return _stage;
        }
        if (!_walkChoiceMade)
        {
            _walkChoiceMade = true;
            VendorJourneyPlan const options = PlanVendorJourney(actor, leg.to, _reserve);
            if (!options.candidates.empty() && options.candidates.front().UsesTaxi())
            {
                _local = std::make_unique<VendorJourney>(options.candidates.front(), _reserve);
                _usesLocalTaxi = true;
            }
        }
        // When the next leg is a ship, deck boarding takes over before an
        // unreachable ship-center waypoint could time out on ground mmaps.
        bool const approachingShip = _legIndex + 1 < _plan->legs.size()
            && _plan->legs[_legIndex + 1].kind == VendorTransportLegKind::Transport;
        bool const localFlightDone = !_local || (_local->Stage() != VendorJourneyStage::Flying
            && _local->Stage() != VendorJourneyStage::WalkingToFlightmaster
            && !actor->HasUnitState(UNIT_STATE_IN_FLIGHT));
        if (approachingShip && localFlightDone && Distance(Location(actor), leg.to) < 60.0f)
        {
            StopLocal(actor);
            NextLeg(actor);
            return _stage;
        }
        auto const& target = _usesLocalTaxi || leg.waypoints.empty() ? leg.to : leg.waypoints[_waypointIndex];
        if (TickWalk(actor, target, elapsedMs, false))
        {
            ++_waypointIndex;
            if (_usesLocalTaxi || _waypointIndex >= leg.waypoints.size())
                NextLeg(actor);
        }
        else if (_stage == VendorJourneyStage::Failed && _usesLocalTaxi && !_localTaxiDeparted
            && (_reason.find("journey_taxi_") == 0 || _reason.find("journey_flightmaster_") == 0
                || _reason == "journey_route_stalled"))
        {
            StopLocal(actor);
            _usesLocalTaxi = false;
            _stage = VendorJourneyStage::WalkingToDestination;
            _reason = "journey_intermap_local_walking_fallback";
        }
        return _stage;
    }
    if (leg.kind == VendorTransportLegKind::AreaTrigger)
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(leg.entry);
        AreaTriggerTeleport const* destination = sObjectMgr->GetAreaTriggerTeleport(leg.entry);
        if (!trigger || !destination)
        {
            Fail(actor, "journey_area_trigger_unavailable");
            return _stage;
        }
        if (_portalInvoked)
        {
            _stage = VendorJourneyStage::WaitingPortal;
            if (actor->GetMapId() == leg.to.mapId && Distance(Location(actor), leg.to) < 50.0f)
                NextLeg(actor);
            else if (_legElapsedMs > MAX_TRANSFER_MS)
                Fail(actor, "journey_area_trigger_rejected");
            return _stage;
        }
        if (actor->GetMapId() != leg.from.mapId)
        {
            Fail(actor, "journey_unexpected_map_change");
            return _stage;
        }
        if (!actor->IsInAreaTriggerRadius(trigger))
        {
            TickWalk(actor, {trigger->map, trigger->x, trigger->y, trigger->z}, elapsedMs, true);
            return _stage;
        }
        if (actor->IsInCombat())
        {
            _stage = VendorJourneyStage::WaitingCombat;
            _reason = "journey_waiting_combat";
            return _stage;
        }
        StopLocal(actor);
        _portalInvoked = true;
        _legElapsedMs = 0;
        _stage = VendorJourneyStage::WaitingPortal;
        _reason = "journey_area_trigger_activated";
        WorldPacket packet(CMSG_AREATRIGGER);
        packet << leg.entry;
        packet.rpos(0);
        actor->GetSession()->HandleAreaTriggerOpcode(packet);
        return _stage;
    }
    if (_legElapsedMs > MAX_TRANSPORT_LEG_MS)
    {
        Fail(actor, "journey_transport_timeout");
        return _stage;
    }
    if (_transportGuid && actor->GetTransport())
    {
        MotionTransport* ship = CurrentShip(actor, _transportGuid);
        if (!ship)
        {
            Fail(actor, "journey_transport_changed");
            return _stage;
        }
        if (_jumping && !_disembarking) // native auto-attach or our landing event boarded the passenger
        {
            if (!SettleOnActualDeck(actor, ship))
            {
                _stage = VendorJourneyStage::RidingTransport;
                _reason = "journey_landing_on_transport_deck";
                return _stage;
            }
            _jumping = false;
            _jumpElapsedMs = 0;
        }
        _stage = _disembarking ? VendorJourneyStage::DisembarkingTransport : VendorJourneyStage::RidingTransport;
        _reason = _disembarking ? "journey_disembarking" : "journey_riding_transport";
        if (_jumping)
        {
            _jumpElapsedMs += elapsedMs;
            if (actor->GetMapId() == _jumpTarget.mapId && Distance(Location(actor), _jumpTarget) < 1.5f
                && actor->movespline->Finalized())
            {
                float const shore = actor->GetMap()->GetHeight(actor->GetPhaseMask(), _jumpTarget.x, _jumpTarget.y,
                    _jumpTarget.z + 2.0f, true, 4.0f);
                if (std::abs(shore - actor->GetPositionZ()) < 1.5f
                    && !actor->GetMap()->GetTransportForPos(actor->GetPhaseMask(), actor->GetPositionX(),
                        actor->GetPositionY(), actor->GetPositionZ(), actor))
                {
                    ship->RemovePassenger(actor, true);
                    NextLeg(actor);
                }
            }
            else if (_jumpElapsedMs > 5000)
            {
                // Keep riding rather than detach at sea. Retry a safe shore on
                // the next stop; if the ship has departed, wait for its cycle.
                _jumping = false;
                _disembarking = false;
                _jumpElapsedMs = 0;
            }
            return _stage;
        }
        if (!CanUseOrdinaryMovement(actor) || actor->IsInCombat())
        {
            _reason = "journey_waiting_ship_exit_ready";
            return _stage;
        }
        if (_deckWalking)
        {
            _deckWalkMs += elapsedMs;
            if ((actor->movespline->Finalized() && Distance(Location(actor), _deckWalkTarget) < 2.0f)
                || _deckWalkMs > 8000)
            {
                _deckWalking = false;
                _deckWalkMs = 0;
            }
            else
            {
                _reason = "journey_walking_to_ship_exit";
                return _stage;
            }
        }
        if (actor->GetMapId() == leg.to.mapId && StoppedAt(ship, leg.to, 4000))
        {
            VendorJourneyLocation onward = leg.to;
            if (_legIndex + 1 < _plan->legs.size() && _plan->legs[_legIndex + 1].to.mapId == leg.to.mapId)
                onward = _plan->legs[_legIndex + 1].to;
            if (FindJumpPoint(actor, ship, false, onward, _jumpTarget))
            {
                _disembarking = true;
                _jumping = true;
                _jumpElapsedMs = 0;
                _stage = VendorJourneyStage::DisembarkingTransport;
                actor->GetMotionMaster()->MoveJump(_jumpTarget.x, _jumpTarget.y, _jumpTarget.z,
                    7.0f, 8.0f, SHORE_JUMP_ID);
                ScheduleLanding(actor, _transportGuid, _jumpTarget, false);
            }
            else if (StoppedAt(ship, leg.to, 8000)
                && FindDeckApproachToShore(actor, ship, onward, _deckWalkTarget))
            {
                _deckWalking = true;
                _deckWalkMs = 0;
                _reason = "journey_walking_to_ship_exit";
                // Passenger-relative native spline, only over a ray-verified
                // continuous deck segment with line of sight; mmaps do not
                // represent dynamic decks. Attachment remains throughout.
                actor->GetMotionMaster()->MovePoint(SHORE_JUMP_ID, _deckWalkTarget.x, _deckWalkTarget.y,
                    _deckWalkTarget.z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, false);
            }
            else
                _reason = "journey_waiting_safe_shore";
        }
        return _stage;
    }
    if (_disembarking && !actor->GetTransport())
    {
        _stage = VendorJourneyStage::DisembarkingTransport;
        _jumpElapsedMs += elapsedMs;
        if (actor->GetMapId() == _jumpTarget.mapId && Distance(Location(actor), _jumpTarget) < 2.0f
            && actor->movespline->Finalized())
            NextLeg(actor);
        else if (_jumpElapsedMs > 8000)
            Fail(actor, "journey_transport_shore_landing_failed");
        return _stage;
    }
    if (_transportGuid && !_jumping)
    {
        Fail(actor, "journey_transport_detached");
        return _stage;
    }
    if (actor->GetMapId() != leg.from.mapId || actor->GetTransport() || actor->HasUnitState(UNIT_STATE_IN_FLIGHT)
        || actor->IsFlying() || actor->GetVehicle())
    {
        Fail(actor, "journey_transport_boarding_context_changed");
        return _stage;
    }
    MotionTransport* ship = FindStoppedTransport(actor, leg, true);
    if (_jumping)
    {
        _stage = VendorJourneyStage::BoardingTransport;
        _jumpElapsedMs += elapsedMs;
        if (ship && ship->GetGUID().GetRawValue() == _transportGuid
            && Distance(Location(actor), _jumpTarget) < 1.5f && actor->movespline->Finalized())
        {
            float deck = 0;
            if (DeckHeight(actor, ship, actor->GetPositionX(), actor->GetPositionY(), deck)
                && std::abs(deck - actor->GetPositionZ()) < 1.5f)
            {
                actor->StopMoving();
                actor->GetMotionMaster()->Clear();
                ship->AddPassenger(actor, true);
                if (actor->GetTransport() != ship)
                {
                    Fail(actor, "journey_transport_attachment_failed");
                    return _stage;
                }
                actor->m_movementInfo.transport.time = ship->GetPathProgress();
                _jumping = false;
                _stage = VendorJourneyStage::RidingTransport;
                _reason = "journey_transport_boarded";
            }
        }
        else if (_jumpElapsedMs > 5000)
        {
            _jumping = false;
            _transportGuid = 0;
            _reason = "journey_transport_boarding_retry";
        }
        return _stage;
    }
    if (actor->IsInCombat() || actor->IsNonMeleeSpellCast(false) || !CanUseOrdinaryMovement(actor))
    {
        StopLocal(actor);
        _stage = VendorJourneyStage::WaitingCombat;
        _reason = "journey_waiting_boarding_ready";
        return _stage;
    }
    if (ship && FindJumpPoint(actor, ship, true, leg.from, _jumpTarget))
    {
        StopLocal(actor);
        _transportGuid = ship->GetGUID().GetRawValue();
        _jumping = true;
        _jumpElapsedMs = 0;
        _stage = VendorJourneyStage::BoardingTransport;
        _reason = "journey_boarding_transport";
        actor->GetMotionMaster()->MoveJump(_jumpTarget.x, _jumpTarget.y, _jumpTarget.z,
            7.0f, 8.0f, BOARD_JUMP_ID);
        ScheduleLanding(actor, _transportGuid, _jumpTarget, true);
        return _stage;
    }
    if (Distance(Location(actor), leg.from) > 8.0f)
    {
        TickWalk(actor, leg.from, elapsedMs, false);
        // A ground path can stop short of a ship center. Keep waiting at the
        // navigable dock while the scheduled boat arrives; don't fabricate it.
        if (_stage == VendorJourneyStage::Failed && _reason == "journey_route_stalled")
        {
            _local.reset();
            _stage = VendorJourneyStage::WaitingTransport;
            _reason = "journey_waiting_transport_at_dock";
        }
    }
    else
    {
        StopLocal(actor);
        _stage = VendorJourneyStage::WaitingTransport;
        _reason = ship ? "journey_waiting_safe_boarding_point" : "journey_waiting_transport";
    }
    return _stage;
}
