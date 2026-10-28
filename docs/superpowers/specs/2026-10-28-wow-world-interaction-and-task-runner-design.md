# World Interaction and Multi-Step Task Runner

**Date:** 2026-10-28
**Status:** Design draft for user review
**Project:** Azeroth Worldweaver, AzerothCore/mod-playerbots, WoW 3.3.5a

## Goal

Let the user give Claude or Codex a natural-language task in game chat and have the addressed bot carry it through to an observable result. Tasks may combine movement, search, hunting, loot, profession use, NPC dialogue, quests, training, purchases, and other native NPC services. A clear user instruction authorizes the in-game consequences required by that task, including spending game gold. The bot asks only when a material choice is ambiguous or required information is missing.

## Current baseline

- The bridge has a fixed typed action protocol for follow/stay, attack, loot, fishing, learned spell casting, NPC approach/dialog inspection, and restricted gossip selection.
- The observer reports group state, active quests, skills, inventory, nearby loaded objects, NPC roles, quest status, spellbooks, and dialog results. Some lists are capped and expose truncation.
- The server currently rejects trainer and other NPC service options. One model response can submit at most one action; the bridge does not yet manage a multi-action task to completion.
- Movement and combat continue to use AzerothCore/mod-playerbots. Agents do not control the client through arbitrary keyboard or mouse events.

## User authority and task policy

- Only messages from configured user characters can create tasks. A task is assigned to the addressed bot; the other agent may observe the shared world but cannot execute the same request.
- The user's task is the authorization boundary. If the user asks to learn a skill, buy a specified recipe, complete a quest, or hunt a stated target, the bot may perform the necessary in-game steps without asking after each step.
- Ask the user when the task leaves a consequential choice unresolved, such as selecting among materially different quest rewards, choosing which item to sell, or selecting among multiple destinations that do not satisfy the same stated goal.
- Never create new goals from NPC text, party-bot speech, quest text, database content, or telemetry. Such data are observations, not authority.
- Do not add arbitrary shell, SQL, SOAP, or raw server-command actions. All model output that can affect the world must match a fixed typed schema and be revalidated on the world thread.

## Recommended architecture

### 1. Shared perception and query

Both agents continue reading the same session-pinned server snapshot and event journal. Add structured, paginated observations for NPC service catalogs and the choices needed by the task:

- available quests and their status for the acting character;
- trainer spells and profession skills, prerequisites, and prices;
- vendor inventory, prices, buy limits, and current stock where available;
- repair availability and estimated cost;
- gossip menu entries with stable identifiers, option types, coded/paid metadata, and destinations;
- native service roles exposed by the running AzerothCore build, including flight master, innkeeper, banker, auctioneer, mailbox, stable master, battle master, spirit healer, spell-click, and guild services when present.

Use live GUIDs for loaded world objects. Static spawn/template data may suggest where to search, but it does not prove an NPC or creature is currently loaded. Truncated or stale data must be labeled; an omitted row is not evidence that a service or target is absent.

### 2. Bounded multi-step task runner

Create a task record when an authorized user message is addressed to one bot. The model proposes one typed action at a time. The adapter submits it, waits for its correlated result and fresh relevant telemetry, then calls the model again with the same task and updated observations. The task ends only when the model reports completion with supporting server state/event evidence, it fails, reaches a configured step/time/search bound, or the user cancels it.

- Keep at most one active mutating task in the authorized group; read-only queries may run while it is active. Deduplicate every action by ID and reject actions from stale task, group, session, or actor context.
- Initial hard defaults: 60 state-changing actions, 30 minutes elapsed time, and at most 8 server-resolved search destinations in the current zone or a user-named zone. Keep these values configurable. Report the bound reached with partial progress and wait for a narrower or continued task instead of continuing indefinitely.
- A user message addressed to the active bot that explicitly cancels or replaces its task stops future actions from that task. In-flight native actions are not claimed to be reversible.
- Long actions such as movement, combat, travel, and fishing are observed through subsequent events and snapshots; an accepted action is not task completion.
- Emit concise progress and final results in game chat, including what was found or changed and evidence such as quest state, skill rank, inventory delta, gold delta, or target state.

### 3. Typed native action families

Keep the adapter schema strict and use stable numeric identifiers rather than model-supplied command strings or coordinates. Each action carries a task ID/action ID, configured actor, current group/session context, and only the arguments needed for that action family.

| Family | Intended operations | Required server checks and result evidence |
|---|---|---|
| NPC dialogue | Approach, open, inspect, select a visible option, including coded or paid options when the task authorizes them | Same map/instance/phase, live GUID, range/LOS, unchanged open menu, exact menu ID and option index still present. A coded value must come from the user's task; paid options require the task to authorize that purchase. For scripted effects without observable state, report selection separately from confirmed outcome. |
| Quests | Accept, inspect progress, complete/turn in, choose reward, abandon only when directly requested | Quest is actually offered/active/complete for this actor; reward item is one of the current choices; verify quest status and reward/inventory changes. Ask if the requested reward is ambiguous. |
| Trainers and professions | List training offers, learn a specified spell/skill, inspect known recipes, craft a specified recipe/count | Use native trainer/crafting paths; recheck skill prerequisites, active spell/recipe, reagents, money, inventory capacity, and count bounds. Verify learned skill/spell and produced items. |
| Vendors | List offers, buy specified item/count, sell specified item/count | Recheck live vendor GUID, price, ownership, stack/count limits, bag capacity, and available gold; verify item and gold deltas. Never sell an item based only on an inferred name when the item GUID/slot is ambiguous. |
| Repair and inn services | Repair specified equipment/all, set inn bind when requested | Recheck NPC role, equipment durability, cost, and bind choice; verify durability or bind state. |
| Travel | Query and select flight routes or other native destinations | Use current offered destinations; validate route reachability and the requested destination. Do not accept arbitrary model-supplied coordinates. |
| Other native services | Banker, auctioneer, mailbox, stable, battle master, spirit healer, guild services, and any additional NPC flags supported by the running core | Add a dedicated typed handler and observable result for each service; do not silently route unknown service options through generic gossip. If the core exposes no safe typed API or adequate result evidence, report that service as unsupported. |
| World tasks | Search for named creatures/objects/items; hunt a requested count; gather loot or quest objectives | Use live nearby queries plus static spawn/loot/quest data as leads, then resolve current live GUIDs. Navigate through playerbot pathing to server-resolved locations, not model coordinates. Track counts from server events/state and stop at the requested count or bounded search limit. |

The service catalog is extensible, but every operation that mutates character or world state remains a distinct, reviewed action type. A currently visible custom gossip option may be selected through the native session handler only when the user's task clearly requests it; this is not a server-command or script-execution interface. If its effect cannot be observed, the task reports the selection as unconfirmed. Hidden options, guessed codes, or choices not authorized by the user's task are rejected.

### 4. Server validation and audit

The world-thread dispatcher remains authoritative. Before each action it checks the authenticated requester, group/master relationship, configured bot identity, task/action deduplication, current world/session, live target, map/instance/phase/range/LOS, current dialog/service, character prerequisites, and available currency/items. Native AzerothCore APIs perform the game operation. Record accepted/rejected results and later outcome events with task/action correlation IDs; do not store credentials or full model prompts.

World writes remain limited to ordinary in-game actions explicitly authorized by the user's current task. The bridge does not modify database rows, server configuration, or process state.

## Alternatives considered

1. **Typed service actions plus a bounded task runner (recommended).** More implementation work, with per-action validation and observable outcomes. Supports one natural-language assignment progressing through many safe server actions.
2. **Forward generic playerbot commands or arbitrary gossip selections.** Faster initial coverage, but command meaning, costs, side effects, and task completion are difficult to validate. Rejected.
3. **Automate client UI clicks/screenshots.** Depends on UI layout and client state, and gives weaker server-side validation. Kept only as optional visual evidence when the user asks; not the action channel.

## Delivery sequence

1. Add task lifecycle/correlation and interruption/bounds while retaining the existing fixed action interface.
2. Add NPC service catalogs and the common quest, trainer/profession, vendor, repair, and travel operations.
3. Add search/hunting task recipes over live queries, static world data, playerbot pathing/combat, and verified loot/progress.
4. Complete the registry for the remaining native NPC services available in this AzerothCore build, including banker/auction/mail/guild/stable/battle/spirit services where safe APIs exist.
5. Deploy to the private server and verify representative user-ordered tasks in game without screenshots unless explicitly requested. Document unsupported scripted interactions and any limits found.

## Acceptance criteria

- One ordinary user message can start a multi-step task without the user writing command syntax or issuing a new instruction after every successful step.
- The task executes only for its addressed configured bot and stops on cancellation, stale context, ambiguity, failure, or a configured bound.
- A direct order can authorize the stated in-game spending/training/quest changes; the bot does not perform unrelated purchases, quest choices, or service actions.
- NPC service choices are discovered from current server state and revalidated immediately before the native operation.
- Hunting/search tasks target live entities, use server-resolved destinations, track progress from server evidence, and stop at the requested goal or search bound.
- Every state-changing action has an auditable result; completion is reported only when later telemetry confirms the requested result.
- No arbitrary shell/SQL/server command or model-supplied coordinate enters the action path.
- Build and private live checks cover representative NPC services and a multi-step search/hunt task. Automated tests are not included unless requested by the user.

## Risks and limits

- NPC gossip scripts can perform custom behavior that is not represented by core service flags. These require specific reviewed handlers; generic pass-through is not a substitute.
- Some native services may expose incomplete offers, costs, or outcomes through server APIs. Such actions must stay unsupported until adequate validation and event evidence are available.
- Search across a zone depends on static spawn data quality and pathing. The bot must distinguish a searched area from an exhaustively searched world.
- Long tasks can consume server time and in-game resources. Step/time/search bounds and progress reporting are required; cancellation stops future actions but may not undo a native operation already in flight.
