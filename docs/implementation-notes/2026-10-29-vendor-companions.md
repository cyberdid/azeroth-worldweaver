# Vendor companions: implementation and validation

The approved flow is shared by Claude and Codex: inspect a live vendor, describe actual goods naturally, buy the item and quantity the authorized player requested, or find a seller and execute a bounded purchase errand.

## Implemented source

- `vendor_inspect` approaches a live merchant and opens the native inventory. The correlated catalog identifies goods, per-bundle prices, bundle sizes, current limited stock, reputation requirements, condition records, honor/arena/item currency requirements, and available money.
- `vendor_buy` requires a fresh catalog for the same actor and NPC. Quantity means individual item units and must be divisible by `BuyCount`. Gold spending is capped. Extended currencies require explicit authorization. The native purchase implementation applies game rules; inventory and currency deltas establish the observed result. Duplicate item rows are rejected rather than guessing a price/extended-cost variant.
- `vendors` is a bounded read-only world-database query that searches items, resolves nested negative vendor references, and returns static seller/spawn records. It explicitly distinguishes configured catalogs and base prices from live stock, discounts and player eligibility.
- `vendor_task_start` is a private persistent bridge worker. It binds user, actor GUID, group, session, exact item, quantity and spending cap. It selects a candidate seller, travels, inspects the live merchant, buys, and optionally returns. It never opens a player trade as part of purchase.
- `vendor_travel` ranks walking and known, affordable directed taxi routes, and uses a bounded cross-map graph executor for ordinary area-trigger portals and scheduled ships/zeppelins. Native pathfinding and paid flight execute the route. No node learning, money grants, or teleport shortcuts are introduced. Stalls, death/context changes and route errors have bounded handling.
- Both adapters use LLM-generated speech for catalog and purchase outcomes. Mechanical task progress never introduces a new user authorization. The current private task is context for progress questions.

## Persistence and cancellation

Before a purchase is dispatched, the worker saves the immutable action ID and waiting state. Restarting the bridge observes that receipt rather than reissuing a purchase. Unknown transport outcomes remain pending and are reported as uncertain, preventing an automatic retry. Event correlation checks actor, action ID, server session and group.

Cancellation removes an active journey and any pending native merchant approach. The server bypasses ordinary action rate limiting for cancellation and records rejection for the original deferred operation. In-flight taxi movement is allowed to finish normally; cancellation does not clear native flight state.

## Boundaries

- Route execution depends on loaded native travel-graph data. Only supported graph edges are used: ordinary walking, eligible known taxi paths, validated area-trigger portals, and real scheduled transports with verified boarding/disembark geometry. Teleport spells and unknown/custom edges are excluded. Physical reachability and transport transitions require in-game validation; this source does not claim every destination is reachable.
- Search and retry have bounds: at most 1000 rows for current-map search plus 1000 global rows; at most sixteen retained candidate spawns. Eligibility and live stock are checked on arrival. A purchase with an uncertain result is never retried at another shop.
- A native atomic purchase is limited to 255 bundles. The model must clarify an incompatible requested quantity instead of rounding it.
- Catalog prices/stock belong to the bot inspecting the merchant. Player reputation or restrictions may differ.
- No runtime task authority, personal memory, logs or credentials belong in Git.

## Validation performed before deployment

Python tests cover typed quantities/budgets, vendor queries against fixture data, MySQL read-only smoke queries, task progression, mismatched receipts, restart/no-repeat behavior, unknown acknowledgements, stage deadlines, session changes and candidate retries. Four changed C++ translation units pass Clang syntax-only checking against the private host's actual AzerothCore/Playerbots headers and build definitions. Syntax checking creates no worldserver binary.

A full worldserver build, install and in-game validation of merchant browsing, exact purchase, insufficient funds/bag space/stock, cancellation while approaching, walking/taxi/portal/ship travel, cancellation aboard transport, return and both LLM adapters remain required. The operator's AGENTS.md requires permission before rebuilding or restarting worldserver.
