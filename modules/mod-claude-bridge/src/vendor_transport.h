#ifndef MOD_CLAUDE_BRIDGE_VENDOR_TRANSPORT_H
#define MOD_CLAUDE_BRIDGE_VENDOR_TRANSPORT_H

#include "vendor_journey.h"

#include <memory>
#include <string>
#include <vector>

class Player;

enum class VendorTransportLegKind { Walk, AreaTrigger, Transport };

struct VendorTransportLeg
{
    VendorTransportLegKind kind = VendorTransportLegKind::Walk;
    VendorJourneyLocation from;
    VendorJourneyLocation to;
    uint32 entry = 0; // ordinary area trigger ID or ship/zeppelin gameobject entry
    std::vector<VendorJourneyLocation> waypoints;
};

struct VendorTransportPlan
{
    std::vector<VendorTransportLeg> legs;
    float estimatedSeconds = 0;
};

struct VendorTransportPlanningResult
{
    std::shared_ptr<VendorTransportPlan> plan;
    std::string reasonCode;
};

// Copies a filtered graph route while holding TravelNodeMap's read lock. Only
// ordinary walking, validated area-trigger portals, and actual stopped-transport
// edges are allowed. No graph pointers survive planning; no graph mutation.
VendorTransportPlanningResult PlanVendorTransport(Player* actor, VendorJourneyLocation destination);

// Native transitions only: normal area-trigger handler; real deck approach and
// short jump; verified passenger attachment; native scheduled transport transfer;
// short jump onto a verified adjacent shore. Never calls TeleportTo/Relocate.
class VendorTransportJourney
{
public:
    VendorTransportJourney(std::shared_ptr<VendorTransportPlan> plan, uint32 purchaseReserveCopper);
    ~VendorTransportJourney();
    VendorJourneyStage Tick(Player* actor, uint32 elapsedMs);
    void Cancel(Player* actor);
    VendorJourneyStage Stage() const { return _stage; }
    std::string const& Reason() const { return _reason; }
    float RemainingDistance(Player const* actor) const;
    bool AllowsMapTransfer() const;

private:
    void Fail(Player* actor, char const* reason);
    bool TickWalk(Player* actor, VendorJourneyLocation const& destination, uint32 elapsedMs, bool allowTaxi);
    void NextLeg(Player* actor);
    void StopLocal(Player* actor);

    std::shared_ptr<VendorTransportPlan> _plan;
    uint32 _reserve;
    std::size_t _legIndex = 0;
    std::size_t _waypointIndex = 0;
    std::unique_ptr<VendorJourney> _local;
    VendorJourneyStage _stage = VendorJourneyStage::WalkingToDestination;
    std::string _reason = "journey_intermap_ready";
    uint64 _actorGuid = 0;
    uint64 _transportGuid = 0;
    uint64 _elapsedMs = 0;
    uint32 _legElapsedMs = 0;
    uint32 _jumpElapsedMs = 0;
    uint32 _transferElapsedMs = 0;
    VendorJourneyLocation _jumpTarget;
    bool _jumping = false;
    bool _disembarking = false;
    bool _deckWalking = false;
    uint32 _deckWalkMs = 0;
    VendorJourneyLocation _deckWalkTarget;
    bool _portalInvoked = false;
    bool _walkChoiceMade = false;
    bool _usesLocalTaxi = false;
    bool _localTaxiDeparted = false;
    bool _cancelled = false;
};

#endif
