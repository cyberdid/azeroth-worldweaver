#ifndef MOD_CLAUDE_BRIDGE_VENDOR_JOURNEY_H
#define MOD_CLAUDE_BRIDGE_VENDOR_JOURNEY_H

#include "Define.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class Creature;
class Player;
class VendorTransportJourney;
struct VendorTransportPlan;

struct VendorJourneyLocation
{
    uint32 mapId = 0;
    float x = 0;
    float y = 0;
    float z = 0;
};

// How the bot covers ground legs: on foot, riding a mount along the ground
// path, or flying its own mount straight to an open landing near the NPC.
enum class VendorTravelMode : uint8
{
    Walk = 0,
    Mount = 1,
    Fly = 2,
};

// A local route contains walking/taxi coordinates on one world map. An intermap
// route owns a validated graph plan of local legs and native portals/transports.
// Estimates are planning hints, not guarantees of terrain reachability.
struct VendorJourneyRoute
{
    VendorJourneyLocation destination;
    VendorJourneyLocation flightmasterLocation;
    uint32 flightmasterSpawnId = 0;
    uint32 flightmasterEntry = 0;
    std::vector<uint32> taxiNodes;
    uint32 maximumFareCopper = 0; // full undiscounted route fare
    float estimatedSeconds = 0;
    VendorTravelMode mode = VendorTravelMode::Walk;

    std::shared_ptr<VendorTransportPlan> transportPlan;
    bool UsesTaxi() const { return taxiNodes.size() >= 2; }
    bool UsesTransport() const { return bool(transportPlan); }
};

struct VendorJourneyPlan
{
    std::string reasonCode;
    std::vector<VendorJourneyRoute> candidates;
};

// Call only on the world thread. Returns a walking fallback and affordable,
// known, faction-eligible directed taxi routes, ranked by approximate duration.
// Foreign-map destinations use supported native portal/transport graph legs.
// candidateLimit is clamped to [1, 16]. Unknown/unsupported routes fail explicitly.
VendorJourneyPlan PlanVendorJourney(Player* actor, VendorJourneyLocation destination,
    uint32 reservedPurchaseCopper, std::size_t candidateLimit = 8);

struct VendorTaxiActivation
{
    bool started = false;
    std::string reasonCode;
    uint32 fareCopper = 0;
};

// Does not learn taxi nodes, grant money, or use instant flight. Requires an
// interactable live NPC and checks every node, directed edge, source and fare.
VendorTaxiActivation ActivateVendorJourneyTaxi(Player* actor, Creature* flightmaster,
    VendorJourneyRoute const& route, uint32 reservedPurchaseCopper);

enum class VendorJourneyStage
{
    WalkingToFlightmaster,
    Flying,
    WalkingToDestination,
    WaitingCombat,
    WaitingTransport,
    BoardingTransport,
    RidingTransport,
    DisembarkingTransport,
    WaitingPortal,
    FlyingMounted,
    Arrived,
    Failed,
    Cancelled,
};

char const* VendorJourneyStageName(VendorJourneyStage stage);

// A bounded, world-thread-only movement executor. Caller owns authentication,
// exclusive task ownership and temporary suspension/restoration of competing
// Playerbots travel/follow/stay/grind/loot strategies. The object stores no live
// Creature/Player pointers. Tick repeatedly with actual world update elapsed ms.
// It never buys anything and never interprets arrival as a successful purchase.
class VendorJourney
{
public:
    VendorJourney(VendorJourneyRoute route, uint32 reservedPurchaseCopper);
    ~VendorJourney();

    VendorJourneyStage Tick(Player* actor, uint32 elapsedMs);
    void Cancel(Player* actor);
    VendorJourneyStage Stage() const { return _stage; }
    std::string const& Reason() const { return _reason; }
    VendorJourneyRoute const& Route() const { return _route; }
    bool IsTerminal() const;
    bool IsNativeTransferPending() const;
    float RemainingDistance(Player const* actor) const;

private:
    void Fail(Player* actor, char const* reason);
    void StopOwnedMovement(Player* actor);
    bool Walk(Player* actor, VendorJourneyLocation const& target, uint32 elapsedMs);
    bool Land(Player* actor, uint32 elapsedMs);
    bool TryMount(Player* actor, bool flying, uint32 elapsedMs);
    void FlyOwnMount(Player* actor, uint32 elapsedMs);
    void TouchDown(Player* actor, char const* reason);

    VendorJourneyRoute _route;
    std::unique_ptr<VendorTransportJourney> _transport;
    uint32 _reserve;
    VendorJourneyStage _stage;
    VendorJourneyStage _resumeStage;
    std::string _reason;
    uint64 _actorGuid = 0;
    uint64 _elapsedMs = 0;
    uint32 _stalledMs = 0;
    uint32 _repathMs = 0;
    uint32 _boardingWaitMs = 0;
    uint32 _landingMs = 0;
    uint8 _mountAttempts = 0;
    uint32 _mountWaitMs = 0;
    uint8 _flightLeg = 0;
    bool _hasLanding = false;
    bool _takenOff = false;
    VendorJourneyLocation _landing;
    VendorJourneyLocation _lastPosition;
    bool _hasLastPosition = false;
    bool _ownsMovement = false;
};

#endif
