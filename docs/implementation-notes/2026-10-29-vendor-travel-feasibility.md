# Merchant journey: native travel feasibility

Source inspection on 2026-10-29. This document records integration boundaries. The `vendor_journey.h/.cpp` helper implements local ground execution and validated real taxi travel. The new `vendor_transport.h/.cpp` adds filtered graph planning and execution of ordinary area-trigger portals and scheduled ships/zeppelins. These sources require syntax/build validation and in-game verification; no universal route guarantee is made. No live build, restart, database change, teleport, or game action was performed.

## What the installed sources actually support

`modules/mod-claude-bridge/src/action_dispatcher.cpp` already sets a custom `TravelDestination`, enables `travel`, and temporarily removes `follow`, `stay`, `grind`, and `loot`. `MoveToTravelTargetAction` then approaches the target with local movement.

In the live source tree `modules/mod-playerbots/src/`:

- `Ai/Base/Actions/MoveToTravelTargetAction.cpp:12`: reads the destination and calls `MoveNear` or `MoveTo`.
- `Ai/Base/Actions/MovementActions.cpp:169`: active `MoveTo` performs local pathfinding. The supplied `mapId` is saved in movement bookkeeping; it does not switch maps.
- `Ai/Base/Actions/MovementActions.cpp:278` onward: the old `TravelNodeMap::getFullPath` dispatcher, including portal, transport, and taxi execution, is commented out. Lifting the bridge's cross-map guard would move toward foreign coordinates on the current map.
- `Ai/Base/Actions/MoveToTravelTargetAction.cpp:126`: `isUseful` rejects `IsFlying()` and `UNIT_STATE_IN_FLIGHT`. A bot already on a flying mount cannot execute this guide movement. This is separate from taking a taxi.
- `Mgr/Travel/TravelMgr.cpp:4217`: `mapTransDistance` estimates distance using known map connections. A finite distance does not establish an executable route.
- `Mgr/Travel/TravelNode.cpp:1402`: `getFullPath(start, end, actor)` can return typed route points. The graph planner exists, but the current movement action does not execute its taxi/boat/portal legs.

Current guide travel can be reused for local merchant approach and a local return leg. Each leg still needs progress detection and failure reporting; acceptance of a coordinate is not proof that the target is reachable.

## Exact native taxi integration

The relevant core methods are:

```cpp
bool Player::ActivateTaxiPathTo(std::vector<uint32> const& nodes,
                               Creature* npc = nullptr, uint32 spellid = 1);
Creature* Player::GetNPCIfCanInteractWith(ObjectGuid guid, uint32 npcflagmask);
uint32 ObjectMgr::GetNearestTaxiNode(float x, float y, float z,
                                    uint32 mapid, uint32 teamId);
bool PlayerTaxi::IsTaximaskNodeKnown(uint32 nodeidx) const;
```

Look up exact declarations in the checked-out headers before implementation; the code above documents the call shape.

`DBCStores.h` exports `sTaxiNodesStore` and `sTaxiPathStore`. `TaxiNodesEntry` has `ID`, `map_id`, `x`, `y`, `z`, `MountCreatureID[2]`. `TaxiPathEntry` has `ID`, `from`, `to`, `price`.

A safe real-flight leg must do all of the following on the world thread:

1. Choose a flightmaster spawn that is on the current map and phase and belongs to a faction the actor can interact with. Plan a walking approach to that spawn. Reacquire the live creature after reaching it; never retain a `Creature*` between ticks.
2. Validate the live NPC with `actor->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER)`. This checks ordinary interaction rules and proximity. Compute its source node with `GetNearestTaxiNode` and require equality with the first planned taxi node.
3. Require every taxi node to be normally known with `actor->m_taxi.IsTaximaskNodeKnown(node)`. Core `ActivateTaxiPathTo` itself does not perform the known-node checks made by the client opcode handlers. Do not use taxi-cheat status as a bypass. Learning the physically visited source through `WorldSession::SendLearnNewTaxiNode` is the normal discovery path, but do not grant distant destination nodes.
4. Require all graph edges to exist through `sObjectMgr->GetTaxiPath(from, to, path, cost)`, preserve route direction, and confirm team eligibility. Reject special spell-only taxi paths.
5. Reserve the item purchase money and the flight fare; the core applies the live flightmaster reputation discount and checks the complete route's affordability. Do not temporarily change actor money.
6. Dismount through the normal cancel-mount handler if required and wait for casting/combat to end. Validate actor life, session, group and ownership before starting.
7. Start the flight with `actor->ActivateTaxiPathTo(nodes, npc, 1)`. The explicit `1` disables the configurable instant taxi branch. This matters: `spellid=0` can invoke `TeleportTo` under `CONFIG_INSTANT_TAXI` in `Player.cpp:10575`. The custom-call `1` still uses the real NPC and regular flight costs.
8. Record `flying` only after the native start succeeds. While `UNIT_STATE_IN_FLIGHT` remains set, do not issue walking orders or repeatedly call activation. After landing, verify actual map and proximity to the expected endpoint, then begin the next walking leg.

Do not route this through `botAI->DoSpecificAction("taxi", Event(...))`: `TaxiAction.cpp:64` has a follower-specific branch that ignores the requested route and repeats cached `last taxi` nodes from the master's taxi request. Its return value can mean only that a delayed callback was scheduled.

Cancellation during a taxi should cancel the task and suppress future legs, while leaving the normal in-progress flight to finish. Do not clear flight state, teleport to the destination, or forcibly place the character on terrain.

## Cross-map and transport boundaries

`TravelNodePathType` in `Mgr/Travel/TravelNode.h:55` distinguishes `walk`, `portal`, `transport`, `flightPath`, and `teleportSpell`. `TravelPath::getPath()` exposes typed `PathNodePoint`s. A future executor must validate every leg before accepting the task and must refuse unsupported leg types. A path containing a hearthstone or teleport-spell edge cannot be silently treated as walking.

Ordinary area-trigger portals have native actions in `Ai/Base/Actions/AreaTriggerAction.cpp`:

- `ReachAreaTriggerAction` requires the source map and walks to the source trigger.
- `AreaTriggerAction` calls `WorldSession::HandleAreaTriggerOpcode` with a `CMSG_AREATRIGGER` packet; the core performs the normal transfer checks.

These are legitimate gameplay transfers. The new bridge transport executor now selects these edges and waits for the actual destination map and location after invoking the normal handler; action invocation alone does not establish transfer success.

Automatic ships and zeppelins are not covered by the existing Playerbots local movement executor; the new bridge transport executor supplies the missing leg handling. The old commented dispatcher calls `Transport::AddPassenger` directly, which is not a validated embark/wait/disembark journey. The bridge now implements source approach, scheduled stop detection, collision-confirmed deck boarding, attachment verification, the core native ride across map boundaries, destination stop detection, and collision-confirmed shore landing. Unsupported or unreachable dock geometry fails rather than fabricating placement.

Free flying mounts are another distinct executor: current guide actions refuse `IsFlying()`, and local ground mmaps do not define a safe three-dimensional flight corridor. A taxi is usable without inventing a free-flight planner.

## Suggested integration boundary

A future independent `vendor_journey.h/.cpp` should expose one journey object owned by the purchase task. It should own all `TravelDestination`/`WorldPosition` storage used by Playerbots until it clears its owned travel target. Actor/requester identities must use GUIDs; live player/creature pointers are reacquired on each world tick.

Suggested interface:

```text
Plan(actor, merchant_spawn, purchase_reserve, return_policy) -> legs or precise rejection
Start(actor, plan) -> accepted or precise rejection
Tick(actor, requester) -> stage, reason, current_leg, distance, elapsed
Cancel(actor) -> cancellation state
```

The states should be `walking_to_flightmaster`, `flying`, `walking_to_vendor`, `purchasing`, `returning`, `completed`, `failed`, and `cancelled`, with explicit `waiting_combat` if needed. The purchase task should only count the item inventory delta observed after a normal vendor purchase as success. Route arrival alone is not purchase success.

For current local support, preserve the exact pre-task noncombat strategies, reject an overlapping hunt/guide/purchase task, keep the destination target ownership check before clearing it, and restore strategies on every terminal path. Use a separate arrival radius sufficient for normal merchant interaction; `TravelDestination`'s generic 20-yard radius is too large to establish that the vendor can be used.

An explicit return request should create another travel leg to the requester's current position, with a bounded update interval while the requester moves. Stop with a precise reason if the requester changes map/phase and no supported route exists. Keep purchase outcome and return outcome separate, so a return failure does not imply the item was not bought.

A useful local stall detector records the actor's last actual position and best remaining distance. Update progress when the actor moves or materially approaches the destination; pause stall accounting for combat and taxi; stop after a bounded period without progress. Also retain an overall journey deadline. Do not keep retrying failed route points indefinitely.

The `Follow()` teleport-looking calls at `MovementActions.cpp:1120` and `:1148` are inside a commented block in this checkout. They are not active shortcuts. A dedicated return leg still gives clearer route ownership, completion and failure semantics than restoring follow and treating that restoration as a successful return.

## Verification required before advertising expanded travel

A local unit test can cover directed route selection, unknown nodes, insufficient travel reserve, unsupported leg rejection, cancellation and state transitions. Native integration additionally needs an approved build/restart and in-game checks: visible walking, paid real taxi takeoff, actual landing, approach to a live vendor, inventory-confirmed purchase, and explicitly requested return. Include source/destination on different continents through a real ship before claiming boats or general cross-map travel.

## Implemented helper integration

The new `modules/mod-claude-bridge/src/vendor_journey.h/.cpp` exposes:

```cpp
auto plan = PlanVendorJourney(actor, {mapId, x, y, z}, purchaseReserveCopper);
if (plan.candidates.empty())
    return plan.reasonCode;
auto journey = std::make_unique<VendorJourney>(plan.candidates.front(), purchaseReserveCopper);
// Persist this object in the authenticated actor's exclusive purchase task.
// Suspend travel/follow/stay/grind/loot while this executor owns movement.
auto stage = journey->Tick(actor, actualWorldElapsedMilliseconds);
// VendorJourneyStage::Arrived -> reacquire and interact with the actual merchant.
// Stage()/Reason()/RemainingDistance(actor) expose progress/failure.
// journey->Cancel(actor) stops owned ground movement; leaves an active taxi intact.
```

Candidates carry full taxi node IDs, flightmaster spawn and entry, exact coordinates, conservative fare and estimated duration. Every plan includes a walking fallback; callers can explicitly select it or construct a new journey using that candidate after a taxi approach fails. Approximate duration uses straight-line distances, so it is not a guarantee of ground reachability or a globally optimal multimodal route. The executor walks navigable mmap prefixes and retries with a 90-second stall bound and one-hour overall deadline; it refuses non-mmap shortcuts. It does not silently activate portals, ships, free-flight mounts or foreign coordinates.

The native taxi helper separately revalidates ordinary network-mask membership, known nodes, faction mounts, every directed edge, every path point's map, spell-only path exclusion, source node and proximity, the live flightmaster's spawn/entry/phase and interaction rules, all flight costs, and the retained purchase reserve. It uses the core's paid custom taxi mode `spellid=1` to force a real flight under instant-taxi configuration.

## Concrete intermap executor

`PlanVendorJourney` now routes foreign-map destinations through `PlanVendorTransport`. It copies a read-locked graph snapshot, filters out teleport spells, unspecified portals, incomplete walks, spell-only travel and unsupported flight edges, and runs a directed shortest-path search. Portal endpoints must match native area-trigger source/destination data; ship endpoints must be real stop frames in the native transport template. Consecutive walking edges retain sampled graph waypoints. Local paid taxi travel can shorten an individual same-map walking section.

The resulting `VendorJourneyRoute::transportPlan` exposes `legs` with kind, entry, source/destination maps/coordinates and walking waypoints. `UsesTransport()` distinguishes these routes from pure walking fallbacks. Existing `VendorJourney::Tick/Cancel/Stage/Reason` APIs stay stable. `IsNativeTransferPending()` identifies executor-owned native portal/boat transitions; callers must retain the connected actor by GUID during brief native world-transfer windows instead of immediately failing `!IsInWorld`. The executor bounds such a transfer window to 60 seconds, an individual ship leg to 20 minutes, and an intermap task to two hours.

Ships/zeppelins are handled through native scheduled `MotionTransport`s. Boarding waits for a stop with sufficient remaining dwell time. The bot takes a short normal jump to a nearby point verified against the actual ship collision mesh; passenger attachment only follows physical arrival or the existing Playerbots transport-under-feet check. The ride uses the server's ordinary passenger transfer, without direct bridge teleport/relocation calls. At the destination, it jumps to verified adjacent static shore; if needed, it first walks over a continuously supported, line-of-sight-checked deck segment toward an exit. Player-owned bounded landing events finish the physical attach/detach safely if a task is cancelled during a jump, and handle native Playerbots' one-second transport checks truncating a jump.

Cancellation/failure never removes a passenger at sea. The caller must hold safe movement strategies while the actor remains attached or in native transfer, since restoring follow on a ship can walk the bot toward a requester on another map. Boat/portal graph availability, safe dock geometry, and normal core eligibility still determine whether a particular itinerary succeeds. Free-flight mount planning, spell teleports and arbitrary cross-map coordinates remain unsupported. Runtime verification on actual routes remains outstanding.
