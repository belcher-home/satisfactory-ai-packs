# Native MKI Bridge

This SML 3.12 Game Feature plugin is the in-game construction/control half of the project. It must provide the community release's required game observations and operations without requiring Ficsit Remote Monitoring or any other uncontrolled third-party gameplay mod. Optional interoperability may complement the bridge, but it is never a prerequisite.

## First-party companion mods

The bridge declares first-party companion mods from this repository as required (non-optional), versioned plugin dependencies in `MKIBridge.uplugin`, so the engine and mod managers load the bridge only with a compatible version of each companion installed. Companions never depend on the bridge. Each companion still gates its features at runtime, fail-closed.

| Companion | Declared range | Gated feature | Source |
| --- | --- | --- | --- |
| MKI Roads (`MKIRoads`) | `^0.8.0` | `road_network` | `packages/road-pack` |

At runtime the bridge reads the declared range from its own descriptor and the installed version from the companion's descriptor, and compares them with SML's SemVer rules. `GET /v1/capabilities` reports each companion (`declared`, `required`, `requiredVersion`, `present`, `version`, `compatible`, `mountPoint`, `features`, `reason`) and each gated feature (`features.<name>.available`, `requires`, `reason`). Generic `POST /v1/place` accepts class and recipe paths under a companion's mount (for example `/MKIRoads/`) only while that companion is compatible, and otherwise answers `409 companion_unavailable`. The companion list and the features it gates are in `CompanionDeclarations()`; the version range lives only in the descriptor.

Generic `POST /v1/place` also accepts `nativeSnap` (default `true`). With `nativeSnap: false`, the caller's resolved transform is authoritative. The native hologram update still runs, but on a hit without an actor, which the shipped `AFGHologram::IsValidHitResult` (CL502094) treats as a valid blocking hit. `TrySnapToActor` therefore finds nothing to attachment-, floor-, or grid-snap to, and the bridge imposes the requested location and yaw before native validation. Clearance and placement checks thus judge exactly the requested transform. Road-kit module requests always send `nativeSnap: false`, so a module placed beside another road never jumps to that road's attachment point. Responses echo `nativeSnap` and, when the hologram did snap, `nativeSnappedToId`. An exact lightweight anchor (`anchorId: "lw|<classPath>|<index>"`) is presented to the hologram as the engine's dedicated unsaved temporary buildable at the runtime instance transform. That actor is held by a shared lease through native validation and destroyed exactly once on every outcome; it never enters the converter pool, and the original lightweight instance is never destroyed or invalidated. Because a hologram snapped to a temporary parent exposes the pool sentinel as its actor transform, the bridge uses it only to validate the caller's explicit transform, then finalizes that validated transform through the buildable subsystem after destroying the temporary. The response reports `lightweightAnchorFinalized: true` and the validated transform.

### Lightweight buildables in the inventory

`GET /v1/buildables` lists actor buildables and, separately, every valid record in the lightweight buildable subsystem within the observation radius, whatever its class: foundations, ramps, walls, barriers, railings, catwalks, walkways, roofs, frames, beams, and any other class the game or a mod registers there. No class filter or allowlist applies. A lightweight record reports:

- `id` `lw|<classPath>|<index>`, plus `lightweight: true` and `lightweightIndex`. The index is stable while the record lives (removal leaves an invalid slot), and the same ID is accepted as an exact `anchorId` and by exact dismantling.
- `className`, `classPath`, and `recipePath` (when the record holds its build recipe), `location`, and `rotation` from the runtime transform.
- `boundsMin`/`boundsMax`: the record's local bounding box transformed to world space. They are omitted when the record's box is unset.
- `customization`: the same descriptor paths, override colors, and resolved values as actor buildables, read from the record's customization data.

Not supported for lightweight records:

- `ports` and `connections` are always empty. Lightweight records own no components, so inspecting snap points, attachment components, or connection ports would require materializing a temporary actor; inventory never does. That inspection is outside the scope of #101.
- Type-specific runtime data (for example a beam's length) is not decoded; use `boundsMin`/`boundsMax` for its extent.
- `playingBuildEffect` and `hasBuildEffectActor` are not reported, because a record has no actor.
- A record whose class key is not loaded is skipped.

### Filtered, paged, revisioned listing (#113, `buildables_query`)

Each conveyor entry in an actor buildable's `connections` also reports what it takes and where it sits (#303): `accepts` (`fuel` for a station's or generator's fuel input, read from its `mFuelConnections` or a connection feeding its `mFuelInventory`; `cargo` for a station's storage inputs; else `any`), `side` (`front`, `back`, `left`, `right`, `top` or `bottom` in the building's frame), `level` (how many same-direction ports stand directly below it), and `connectedBuildableId`/`connectedComponent` when attached. Fluid pipe connections report `accepts`: `fluid`.

An actor buildable with legs (a truck station, a machine) also reports `legOffsetsCm`: each foot's offset below the building from when it was placed, so a station that grew legs under a deck shows it (#321).

With no listing parameters, `GET /v1/buildables` is unchanged: every buildable in the observation radius, every field, and no paging. Any of the parameters below selects the filtered listing. The observation radius around the player always applies as well; a filter only narrows it.

- `center=x,y,z` and `radiusCm` (together; `0 < radiusCm <=` the observation radius), `bounds=minX,minY,minZ,maxX,maxY,maxZ`, `classPath` (comma-separated exact class paths, at most 64), and `ids` (comma-separated exact IDs, at most 500).
- `fields=compact|ports|full` (default `full`):
  - `compact`: identity, class, recipe, `location`/`position`, `rotation`, and bounds;
  - `ports`: adds `ports` and `lift`;
  - `full`: everything the unfiltered listing reports.
- Paging: `limit` (default 500, 1 to 5000) and `cursor`. Pass `nextCursor` back unchanged. Items are ordered by ID (case-sensitive).
  - The response reports `total` (matches across all pages), `returned`, and `truncated`.
  - A cursor from another world answers `409 cursor_expired`.
- Revisions:
  - Each filtered request rescans the world. Anything added, moved, rotated, or whose connections changed gets the next `revision`.
  - The response reports the current `epoch` (new for every loaded world) and `revision`, and each item reports its own `revision`.
  - `since=<revision>&epoch=<epoch>` returns only items changed after that revision, plus `tombstones` (`id`, `classPath`, `location`, `revision`) for removed ones.
  - The bridge keeps the latest 4096 tombstones. A `since` from another epoch, newer than the current revision, or older than the retained tombstones returns the full filtered listing with `resync: true`; the caller must replace, not merge, its view.
- `coverage` reports `observer` and `observationRadiusCm`.
  - For a sphere query it also reports `center` and `radiusCm`: the part of the requested sphere that lies inside the observation radius. A caller must not treat anything outside it as absent.
  - For a `bounds` query it reports `boundsComplete`.
- `ids` that name a conventional actor one of this bridge's placements created, and that has since become a lightweight instance, are resolved: `resolvedIds` maps the old ID to the new one, and the instance is listed.

### Batch operations (#59, `batch`)

`POST /v1/batch` takes `{ ops, confirm, requestId, atomic }`. `ops` lists up to 64 operations, which expand to at most 256. Each operation is `{ opId, op, request, repeat? }`:
- `op` is `place`, `connect`, `dismantle`, or `set_recipe`, and `request` is that route's body.
- `opId` is unique: 1 to 40 letters, digits, `_` or `-`.
- `request` must not set `confirm`, `requestId`, or `validateHologram`; the batch sets them.

**Repeat.** `repeat: { count, offsetCm, yawStep }` applies to a placement with an absolute `x` and `y`. It expands the placement into `count` (at most 64) copies named `<opId>#<i>`. Copy `i` is offset by `i * offsetCm` and turned by `i * yawStep`.

**References.** A request string `"$<opId>.buildableId"` names the buildable an earlier operation created; a repeated copy is `$<opId>#<i>.buildableId`. A reference to a later or unknown operation is rejected with `400 invalid_reference`.

**Preview pass.** Every operation is first run as a dry run, in order, against the current world.
- Placements of `type: "generic"` and the convenience building types drive the native hologram through placement (`validateHologram: true`) and report `hologramValid`, `multiStepFinished`, and `disqualifiers`. Nothing is charged or built, and host authority is not required.
- Other placement types use their existing dry run.
- An operation that references a buildable the batch has not built yet cannot be previewed. It is reported `deferred` and is validated by its own handler during the confirmed pass.
- The preview does not see the batch's own earlier pieces.
- If any preview fails, the answer is `409 batch_preview_failed` with every operation's result, and nothing is built.
- `confirm: false` stops after the preview.

**Confirmed pass.** `confirm: true` requires a `requestId` of at most 80 characters without `:`. Operations then run one per tick through the same route wrapper as direct calls, each with requestId `<requestId>:<opId>`. Each therefore has its own receipt, idempotency, authority check, and 25 s watchdog. The batch as a whole has a 600 s watchdog.

**Atomic batches.** `atomic` defaults to true. An atomic batch accepts only `place` and `connect`, because dismantling and recipe changes cannot be undone. If an operation fails:
- every buildable the batch created, including belt and spline children, is dismantled in reverse order through the checked dismantle route. A placement the game converted to a lightweight instance (a foundation or wall) is first resolved to that instance through its placement origin (#113), because its actor ID no longer exists; the answer maps those IDs in `rollbackResolvedIds` (live, a rollback without this leaked the foundation);
- every operation's receipt is dropped;
- the answer is `409 batch_failed` with `rolledBack` and `leaked`.

**Non-atomic batches.** A failure leaves completed operations built and answers `409 batch_failed`. Retrying the same batch `requestId` resumes it: operations that already completed are not previewed again, and they replay their recorded results instead of building twice.

**Success.** The answer is `201` with `results`, `createdIds`, and `buildableIds` (opId to primary ID). A retry after success replays the whole batch.

The gameplay driver uses the batch only for a one-shot hologram preview of a `pristinePreflight` plan's independent placements. Confirmed plan operations still run one by one, because site surveys, journaling, and receipt ownership proofs run between them.

### Observation routes (#114)

All of these routes are read-only and authenticated. Each was exercised live in the AI Sandbox (game CL-502094, SML 3.12.0); the `producing` and `output_blocked` machine states and a tripped fuse were not observed there.

**`GET /v1/machines` (`machine_state`)**

Lists factory buildables (manufacturers, extractors, generators, and other `AFGBuildableFactory` classes) inside the observation radius.
- Filters: `ids`, `center`+`radiusCm`, `bounds`, and `limit` (default 200, at most 2000). Results are ordered by ID and report `total` and `truncated`.
- Each machine reports:
  - `kind`, `recipePath`/`recipeName`/`recipeDurationSeconds`;
  - `producing`, `standby`, `hasPower`;
  - `productivity`, `currentPotential`/`pendingPotential` (1.0 = 100 % clock), `productionProgress`;
  - power consumption and the `circuitId`;
  - `inputInventory`/`outputInventory`/`fuelInventory` stacks;
  - `resourceNodeId` for extractors, and `powerProductionCapacityMW` for generators;
  - `connectedBuildableIds` (direct neighbours), plus `upstreamIds`/`downstreamIds`: the first non-conveyor buildable reached through belts and lifts.
- `state` is derived, in this order:
  1. `standby`;
  2. `unpowered`;
  3. `producing`;
  4. `idle` (no recipe);
  5. `input_starved` (an ingredient is short in the input inventory);
  6. `output_blocked` (a manufacturer with power, a recipe and ingredients that is not producing; inferred, not read from the game).

**`GET /v1/conveyors` (`conveyor_contents`, #304)**

What each belt and lift carries. Filters: `ids`, or `center=x,y,z` with `radiusCm` (100 to 200000, default 5000); `limit` (default 500, at most 2000), ordered by id with `total` and `truncated`. Each conveyor reports `id`, `className`, `position`, `lengthCm`, `speedCmPerS`, `itemCount` and `items` (`{ item, count, fuelEnergyMJ }` per item class, read from its segment of the game's conveyor chain; `fuelEnergyMJ` is 0 for an item that is not a fuel), its chain (`chainId`, `chainItems`, `chainAvailableSpaceCm`, `chainFull` when less than one 120 cm item spacing is free at the chain's entry, `chainOutputWaiting` when an item waits at its exit that nothing has taken), and `input`/`output` ends with `connectedBuildableId`/`connectedComponent`. The MCP tool `factory_diagnose` reads it with the machines, stations and circuits to name why items stop.

**`GET /v1/power-circuits` (`power_circuits`)**

Reports every circuit with a powered buildable inside the observation radius:
- production, consumption, capacity, and maximum consumption in MW (capacity and maximum consumption are read from the circuit's own getters);
- `fuseTriggered`;
- `noPowerCheat`: the "no power" advanced game setting is on. The game then reports production equal to consumption with no generator capacity (seen live in the AI Sandbox: 15 MW produced, 0 MW capacity);
- battery input, stored energy, capacity, and seconds until empty or full.

**`GET /v1/inventory/player` (`player_inventory`)**

Reports the player inventory's stacks per slot, the `totals` per item, and `noBuildCost`.

**`GET /v1/catalog/buildables` (`catalog`)**

Lists every unlocked recipe that builds a buildable:
- class, recipe and descriptor paths, display name, and cost.

With `details=true`, or with a `classPath` filter, each entry also reports:
- `ports`: conveyor, fluid, hypertube and power connection templates, with `localPosition` and `localFacing`;
- `localBounds`: approximate, the union of top-level static mesh bounds.

Port positions are each template's own relative transform. A template attached below another component is flagged `nested: true`, because its parent's offset is not composed.

**`GET /v1/catalog/recipes` (`catalog`)**

Lists unlocked production recipes (those produced in a manufacturer; `all=true` lists every recipe). Each recipe reports:
- `ingredients`/`products` with `form`;
- `durationSeconds` and `producedIn`.

Liquid and gas amounts are in thousandths of a cubic metre.

**`GET /v1/progression` (`progression`)**

Reports purchased schematics (path, name, type, tier), the highest purchased milestone tier, and the active schematic.

**`GET /v1/resource-nodes` (`resource_nodes_v2`)**

Lists ore nodes and geysers as well as fracking cores and satellites. Each node reports:
- `id`;
- `kind`: `ore_node`, `geyser`, `fracking_core` or `fracking_satellite`;
- `resource`: the item descriptor class name, for example `Desc_OreIron_C`; omitted for geysers;
- `form`: `solid`, `liquid`, `gas` or `heat`;
- `purity`: `impure`, `normal` or `pure`; omitted for fracking cores, which have none;
- `occupied`, and `extractorId` when an extractor or activator is bound;
- `location`.

Fracking entries keep `satelliteCount`, `state`, and `coreId`.

Filters: `center`+`radiusCm`, `bounds`, `kind`, and `resource`, the last two as comma-separated exact values.

### Vehicles, stations and inventories (#263, `vehicles`)

- `GET /v1/vehicles` lists every wheeled vehicle and every docking station on the vehicle-path networks. A vehicle reports `id`, `name`, `className`, `location`, `yaw`, `speedCmPerS`, `autopilotEnabled`, `canEnableAutopilot`, `autopilotError` (`none`, `station_unreachable`, `not_on_path`, `too_few_stations`, `no_fuel`, `deadlocked`), `docking`, `dockingStationId`, `onPath`, `route` (path-node GUIDs), `targetWaypointIndex` and `inventories`. A station reports `id` (its buildable id), `name`, `pathNodeGuid`, `status` (`operational`, `fuel_type_warning`, `no_power`), `loadMode`, `loadUnloading`, `dockedVehicleId` and `inventories`. `paths` gives the game's own validation of the vehicle paths (#312): `segments` (the count) and `invalid`, one entry per segment and vehicle type it rejects, with `id`, `preset`, `result` (`turn_too_sharp`, `no_floor`, `terrain_too_steep`, `static_obstacle`, `invalid_shape`, `submerged_in_water`), `failDistanceCm`, `from`, `to` and `failAt` (where the check failed). A blocked segment is why a truck reports `station_unreachable`.
- `GET /v1/game-rules` (`game_rules`, #309) reports the session's `noPower`, `noBuildCost` and `noFuelCost` rules. `POST /v1/game-rules/fuel` `{ noFuelCost, confirm }` sets only the fuel rule, so vehicles can be run burning fuel (`false`) or without it (`true`); `changed` says whether it moved.
- `GET /v1/inventories?id=<buildable or vehicle id>` lists an actor's inventory components by name (for example `FuelInventory`, `StorageInventory`, `InputInventory`), each with its slots and stacks.
- `POST /v1/vehicle-paths/revalidate` `{ min: {x,y,z}, max: {x,y,z}, confirm }` asks the game to re-validate every vehicle path segment in the box (its own batched `InvalidateVehiclePathsInVolume`, #312). The game keeps a segment's validation until then, so a removed obstacle still reads blocked. Answers the number of segments in the box; the work is spread over the next frames. Feature `vehicle_path_validation`.
- **Pipe supports and pipe joints (#308).** `POST /v1/place` `{ type: "pipe_support", position, yaw, heightCm }` builds a Pipeline Support standing its pipe 175, 275, 375 or 475 cm up (other heights round to these); a support built at another height is rolled back with `built_differs_from_request`. A pipe `connect` to or from a support rests its end on the support's snap point; from a support with a pipe already resting on it, the new pipe starts at that pipe's free end, and to such a support it joins that pipe, so pipes chain and splice support to support. A support must face back toward the pipe's source.
- **Build receipts (#302, feature `build_receipts`).** Confirmed pole, lift, belt and generic placements answer with `receipt`: `id`, `className`, `position`, `rotation`, `heightCm` (poles and lifts), `ports` (each with `worldPosition`, `worldFacing`, `accepts`, `side`, `level`, `connectedBuildableId`), `matchesRequest` and `deltas` (`{ field, requested, built, toleranceCm }`). Generic placements add `snapMovedCm`, how far snapping moved the building; that move is reported, not rolled back. A pole built at another height than requested is rolled back with `built_differs_from_request` (`requestedHeightCm`, `builtHeightCm`, the rejected `receipt`). A belt that does not attach fails with `connection_verification_failed` naming the unattached end.
- `POST /v1/vehicle-route` `{ vehicleId, stationIds?: [docking station buildable ids], autopilot?: boolean, confirm }` sets the vehicle's station route (the game's own SetVehicleRoute, as in the vehicle menu) and turns self-driving on or off. It answers `409 autopilot_unavailable` when the game refuses self-driving.
- `POST /v1/docking-station` `{ stationId, loadMode, confirm }` sets a station to load or unload.
- `POST /v1/inventory/transfer` `{ fromId, fromInventory?, toId, toInventory?, item, count, confirm }` moves items between two inventories. `"player"` names the player's inventory. An inventory is named by component (it may be omitted when the actor has only one). Whatever the target will not take goes back.

### Events (#117, `events`)

`GET /v1/events?since=<seq>&epoch=<epoch>&timeoutMs=<0..30000>&types=<a,b>` long-polls for world and bridge events.

- **Answer.** `{ ok, epoch, latestSeq, nextSince, resync, more, timedOut, events: [{ seq, type, time, data }] }`. Pass `nextSince` and `epoch` back as the next cursor.
- **Waiting.** When events after `since` exist, the bridge answers at once with up to 256 of them (`more: true` when there are further events). Otherwise it holds the request until a matching event arrives or `timeoutMs` passes; deadlines are checked every 0.25 s. At most 16 waits may be open at once; more answer `503 too_many_waiters` (retryable).
- **Cursor.** Without `since` the cursor starts at the latest event, so a first call never replays the backlog. `since` requires the `epoch` it was read in, because sequence numbers restart with every epoch. A new epoch starts whenever the play world changes (every load). A cursor from another epoch, from the future, or older than the retained backlog (4096 events) answers `resync: true` with no events: re-observe the world, then continue from `nextSince`.
- **Types.** `types` filters to exact names; an unknown name answers `400 invalid_query`.
  - `receipt_completed`: `{ requestId, path, code, ok }` for every confirmed mutation with a `requestId`, including failures and abandonment (`ok: false`). Batch operations report `<requestId>:<opId>`.
  - `buildable_added`, `buildable_changed`, `buildable_removed`: `{ id, classPath, location, revision, buildablesEpoch }`. `revision` and `buildablesEpoch` match the filtered listing (#113), so a caller can fetch details with `since`. A lightweight conversion appears as the actor's removal plus the instance's addition. A sample with more than 256 changes emits one `buildables_bulk_change` `{ added, changed, removed, revision, buildablesEpoch }` instead.
  - `machine_state_changed`: `{ id, classPath, previousState, state }`, with the states of `/v1/machines`.
  - `power_circuit_tripped`, `power_circuit_restored`: `{ circuitId, fuseTriggered }`.
- **Sampling.** Receipt events are immediate. Buildable, machine, and fuse events come from a world sample every 5 s, which runs only while a client has polled events in the last 60 s or a wait is open. The first sample of an epoch is a silent baseline. A machine or circuit seen for the first time is recorded without an event. Only changes inside the observation radius are emitted.

The MCP tool `wait_for_events` exposes the stream to a supervisor loop, which can re-observe only when something happens instead of re-querying on a timer. The tool fails when the bridge does not advertise `events`.

### Receipt persistence (#38, `persistent_receipts`)

Completed mutation receipts survive a game restart, so a retry after a crash replays its result instead of building twice.

- **Scope.** Receipts belong to the loaded world. Whenever the play world changes (every load), the bridge clears every receipt held in memory, together with the live placement bookkeeping.
- **Side file.** Completed receipts are written to `Saved/MKIBridge/receipts/<session>-<crc>.json`, keyed by save session name. The file is written at most every 2 s while receipts change, on world change, and on shutdown, through a temporary file that is then moved over the old one. A receipt whose recorded answer exceeds 64 KiB, or one completed without a game state, stays memory-only.
- **What a load restores.** Each receipt records the session play time (`AFGGameState::GetTotalPlayDuration`) at completion. When a world of the same session loads, only receipts completed at a play time strictly before the loaded play time are restored, because their effects were saved. Later receipts belong to play that was not saved, or to a branch abandoned by loading an older save. They are discarded and removed from the file. The 1024-receipt bound still applies.
- **Limitations.**
  - A receipt completed in the same second as the save is discarded, so a retry builds it again.
  - Loading an older save discards the newer branch's receipts. Loading the newer save afterwards does not bring them back.
  - An unnamed session keeps receipts in memory only.
- **Answers.** A replay of a restored receipt adds `restored: true`. `GET /v1/placement-status` for a restored receipt answers `{ ok, requestId, restored: true, path, code, completedAt, result }` and makes no `exists` or final-identity claim, because the actor bookkeeping did not survive the load. Callers observe the world to resolve identity; the TypeScript final-ID adoption fails closed on it.

### Final placement identity (#113, `placement_final_ids`)

When a confirmed single placement succeeds, the bridge records the actor's name, class, and transform, and which lightweight instances of that class already sat at that transform. The game converts foundations and walls to lightweight instances while they finish spawning, before the response is sent (verified live), so the generic and single-foundation placements take the class's instances before constructing and record the origin at the placement site; only instances that existed before construction count as pre-existing.

`GET /v1/placement-status` then reports:
- `receiptBuildableId`;
- `finalResolution`:
  - `actor`: the actor still exists;
  - `lightweight_conversion`: exactly one new lightweight instance of the class sits at the recorded transform within 1 cm and 0.1 degrees;
  - `gone`, `ambiguous`, or `unavailable`;
- `finalBuildableId`.

For a conversion, `buildableId` is the lightweight ID and `exists` is true. A pre-existing instance at the same transform is never reported as the placement's.

The gameplay driver uses this resolution only when its own receipt is not listed. It still requires that:
- the instance is listed exactly once, with the planned class and recipe;
- the instance is unclaimed, and absent from the pre-placement baseline by ID and by location;
- the instance is at the receipt's reported transform.

## Source location and native build

The plugin source root is `mods/MKIBridge/MKIBridge`. `npm run road-pack -- sync` mirrors its descriptor, `Config/`, and `Source/` into `<SML>/Mods/GameFeatures/MKIBridge` (backing up overwritten files under `.local/road-pack`). Do not point tooling at the retired `bridge-mod` path.

The module's private sources are one translation unit per operation family (#44). All of them include `BridgeInternal.h`:

| File | Holds |
| --- | --- |
| `MKIBridge.cpp` | module lifecycle, the route table and dispatch (auth and receipt wrapping), the status indicator, `health`, `capabilities` |
| `BridgeAuth.cpp` | the loopback bearer token and request authorization |
| `BridgeReceipts.cpp` | mutation receipts (completion, replay, per-session persistence), placement origins, `placement-status` |
| `BridgeObservation.cpp` | read-only routes: player, volumes, buildables and queries, machines, power, inventory, catalog, progression, resource nodes |
| `BridgeEvents.cpp` | the event stream: scope, emission, sampling, long-poll waiters |
| `BridgeProbes.cpp` | non-mutating probes: placement validation and site survey, column probe, hologram probe |
| `BridgePlacement.cpp` | placement and dismantle routes |
| `BridgeConnections.cpp` | belt, spline, structure, and power connections |
| `BridgeInteraction.cpp` | player transform and respawn, saves, machine recipes |
| `BridgeBatch.cpp` | atomic batches, repeat expansion, rollback |
| `BridgeInternal.h` / `.cpp` | shared engine includes, limits, asset paths, the console variables, and the helpers more than one family uses (declared in the header, defined once in the `.cpp`, in `namespace MKIBridgeInternal`) |

A helper only one family uses stays in an anonymous namespace in that family's file. `test/native-layout.test.ts` pins the layout. The Node tests read all the private sources as one text (`nativeBridgeSource()` in `src/workspace-paths.ts`).

The packaged bridge is a game-feature mod. Install it at `FactoryGame/Mods/GameFeatures/MKIBridge`, where its containers mount. Its companion MKI Roads is an ordinary mod and goes at `FactoryGame/Mods/MKIRoads`, not under `GameFeatures`. `npm run road-pack:install -- check|install --game <root>` installs both from verified packages with backups and checks the layout; see Installing into the game.

Native compilation depends on the external SML/Unreal development environment and is not performed by the Node.js CI job. docs/NATIVE_BUILD.md lists the supported versions, toolchain, and steps for a reproducible build. `npm run road-pack -- package --plugins MKIBridge` compiles and packages the plugin with the Alpakit `PackagePlugin` automation (Win64 Shipping, Steam and Epic targets) and verifies the archive, including `"BuildId": "SML"` in every `.modules` manifest; see mods/MKIRoads/README.md. A controlled self-hosted Windows runner can compile and package both plugins from a clean checkout with `.github/workflows/native.yml` (#41; see Native build in CI). Generated DLL, PDB, PAK, UCAS, and UTOC files belong beneath `.local/packages` or a release artifact, never in source control.


Never install a raw UBT `.modules` manifest. UBT stamps it with the development engine's numeric BuildId, which the shipped game refuses to load (#75). Packaging rewrites it to `"BuildId": "SML"`, as SML and every SML-distributed mod use.`npm run road-pack -- package` verifies this on every archive it writes.

When the native HTTP listener is active, the mod refreshes a small in-game status message: amber while the world/player is initializing and green when the bridge is ready. Set `MKIBridge.ShowStatus=0` in the Unreal console or `[SystemSettings]` configuration to hide it.

## Current contract

Routes (all under `http://127.0.0.1:8081`; every route except `health` and `capabilities` requires the bearer token):

| Route | Method | Kind |
| --- | --- | --- |
| `/v1/health` | GET | public |
| `/v1/capabilities` | GET | public |
| `/v1/buildables` | GET | read |
| `/v1/resource-nodes` | GET | read |
| `/v1/player` | GET | read |
| `/v1/player/vitals` | GET | read |
| `/v1/world/volumes` | GET | read |
| `/v1/saves` | GET | read |
| `/v1/machine-connections` | GET | read |
| `/v1/power-connections` | GET | read |
| `/v1/placement-status` | GET | read |
| `/v1/site-survey` | POST | read (no mutation) |
| `/v1/region/loaded` | GET | read (no mutation) |
| `/v1/terrain/column-probe` | POST | read (no mutation) |
| `/v1/hologram-probe` | POST | read (no mutation) |
| `/v1/world/actor` | GET | read |
| `/v1/world/static-meshes` | POST | read (no mutation) |
| `/v1/terrain/removed-rocks` | GET | read (needs MKI Tools) |
| `/v1/terrain/remove-rock` | POST | mutation (needs MKI Tools) |
| `/v1/terrain/restore-rock` | POST | mutation (needs MKI Tools) |
| `/v1/conveyors` | GET | read |
| `/v1/machines` | GET | read |
| `/v1/power-circuits` | GET | read |
| `/v1/inventory/player` | GET | read |
| `/v1/catalog/buildables` | GET | read |
| `/v1/catalog/recipes` | GET | read |
| `/v1/progression` | GET | read |
| `/v1/place` | POST | mutation (`requestId` required to confirm) |
| `/v1/place-spline-road` | POST | mutation (`requestId` required to confirm); temporary, spline road prototype |
| `/v1/build-gun` | POST | mutation (`requestId` required to confirm) |
| `/v1/connect` | POST | mutation (`requestId` required to confirm) |
| `/v1/dismantle` | POST | mutation (a buildable, or a vehicle by its exact actor name) |
| `/v1/set-machine-recipe` | POST | mutation |
| `/v1/machine-clock` | POST | mutation: a machine's clock speed (`buildableId`, `clockPercent` within its range; a dry run reports the range) |
| `/v1/splitter-rules` | POST | mutation: a Smart or Programmable Splitter's sort rules (`buildableId`, `rules: [{ item: overflow|any|none|undefined|<item descriptor>, output: 0-2 }]`; a dry run reports the current ones) |
| `/v1/player-transform` | POST | mutation |
| `/v1/player/respawn` | POST | mutation |
| `/v1/load-save` | POST | mutation |
| `/v1/batch` | POST | mutation (`requestId` required to confirm) |
| `/v1/events` | GET | read (long-poll) |
| `/v1/game-rules` | GET | read |
| `/v1/game-rules/fuel` | POST | mutation |
| `/v1/vehicles` | GET | read |
| `/v1/inventories` | GET | read |
| `/v1/vehicle-paths/revalidate` | POST | mutation |
| `/v1/vehicle-route` | POST | mutation |
| `/v1/docking-station` | POST | mutation |
| `/v1/inventory/transfer` | POST | mutation |

- Listen on a dedicated local port (`8081`) separate from FRM (`8080`), bound to `127.0.0.1` (#37). Before requesting its router the bridge replaces any `[HTTPServer.Listeners] ListenerOverrides` entry for its port with `(Port=8081,BindAddress=127.0.0.1)`, logs the effective bind at startup, and reports `bindAddress`, `port`, and `loopbackOnly` in `GET /v1/health`. Every route also rejects a request whose peer address is known and not loopback (`403 forbidden_peer`); if the HTTP server reports no peer address the bridge logs a warning once and relies on the bind.
- Expose versioned `/v1` routes with authenticated dry-run and confirmed construction operations. One route wrapper runs, in order, the loopback check, authentication (a constant-time token comparison), and then, for confirmed mutations, the requestId, host-authority, and idempotency checks. No request body is parsed before authentication succeeds.
- Answer every error with one shape: `{ ok: false, error, message, retryable }`. `retryable` is true only when repeating the identical request can succeed unchanged (`503` world not ready, `request_in_flight`, `mutation_abandoned`). Common codes: `invalid_json` (400), `request_id_required` (400), `world_unavailable` (503), `outside_safety_radius` (403), `server_authority_required` (403), `unauthorized` (401).
- Apply one idempotency contract to every confirmed mutation route (`/v1/place`, `/v1/connect`, `/v1/dismantle`, `/v1/set-machine-recipe`, `/v1/player-transform`, `/v1/load-save`, `/v1/batch`). `place`, `connect`, and `batch` require a `requestId` when `confirm: true`; the other routes apply the contract whenever a `requestId` is supplied. The receipt is registered as in flight before the handler (and any deferred hologram timer) runs. A retry of the same `requestId` while it runs answers `409 request_in_flight` (retryable); after success it replays the recorded response with its original status plus `replayed: true` and `duplicatePrevented: true`; reusing it for a different route or body answers `409 request_id_conflict`. A failed mutation is rolled back and refunded, so its `requestId` may be retried. The bridge keeps the latest 1024 completed receipts per loaded world and persists them per save session (see "Receipt persistence"). `GET /v1/placement-status` answers `409 request_in_flight` while a request runs and includes the recorded confirmed body as `response`.
- Deferred hologram work holds only weak references to the world, player, holograms, anchors, endpoints, and recipes. If any of them disappears before the timer runs, the bridge destroys the hologram, refunds the cost when the world still exists, and answers `409 deferred_target_unavailable`. If the deferred work has not answered within 25 s (for example the world was unloaded), a watchdog answers `503 mutation_abandoned`; the deferred work checks that state first and never builds afterwards.
- Match buildable and resource-node IDs case-sensitively, through a name index that is rebuilt on a miss. Accept lightweight IDs only in the canonical form `lw|<classPath>|<index>`; a malformed index is rejected instead of resolving to instance 0. Port names are still matched case-insensitively.
- Write every response as condensed JSON.
- Report `modVersion` from the plugin descriptor in `/v1/health` and `/v1/capabilities`, and list every routed read (including `resource_nodes`) in `capabilities.read`. Capabilities also report a `contract` object (`errorShape`, `idempotency`, `maximumMutationReceipts`, `mutationWatchdogSeconds`, `maximumSurveySamples`, `loopbackOnly`, `caseSensitiveIds`, `receiptPersistence`, `maximumEventBacklog`, `maximumEventWaitMs`, `eventSampleIntervalSeconds`) and the bridge-native feature flags `idempotent_replay`, `site_survey_summary`, `power_connections_bounded`, `lightweight_dismantle_checked`, and `belt_support_endpoints`, `buildables_query`, `placement_final_ids`, `machine_state`, `power_circuits`, `player_inventory`, `catalog`, `progression`, `resource_nodes_v2`, `batch`, `events`, and `persistent_receipts` under `features` (each `available: true`, `requires: []`). Callers gate newer behaviour on these flags and fail closed when a flag is absent.
- Companion-mod status is resolved from the descriptors once per process and cached.
- Accept the shared field names `sourceId`/`targetId` for power connections (`aId`/`bId` remain aliases for one release), `buildableId` for `machine-connections` and `set-machine-recipe` (`machineId` remains an alias), and report `buildableId` for every created belt and power line (`beltId`/`wireId` remain). Buildable listings report `position` alongside `location`.
- Roll back unverified connections. A belt whose requested endpoints did not both attach, or a power line that left either endpoint unconnected, is destroyed with its hologram children and refunded (`409 connection_verification_failed`) instead of being reported as an unverified success.
- Connect belts to and through conveyor supports (#111, pole-to-pole half; `belt_support_endpoints`). A belt endpoint is, in order: a free port of the right direction; otherwise, on a conveyor support (a buildable whose factory connections are all snap-only, such as a conveyor pole or wall/ceiling mount), the free end of a belt already resting on that support's snap point (within 10 cm), so the new belt chains through the support; otherwise the support's snap point itself, where the new belt's end rests and stays free for the next belt. The preview and result report `sourceEndpoint`/`targetEndpoint` (`port`, `belt_end`, `support_snap`) and `sourceChainedBeltId`/`targetChainedBeltId`. Verification requires a port or belt end to be connected, and a belt end placed on a snap point to rest within 10 cm of it. Conveyor lifts have ordinary input and output ports and connect like any machine. The game builds a belt through a support snap only against the snap facing: the belt leaves or arrives travelling opposite to the support's snap normal, so a support faces back toward the belt's source (verified live: two poles at yaw 0, whose snap normal is +X, connect toward -X; toward +X the hologram rejects the route with `FGCDConveyorInvalidShape`). A support facing along the flow (or across it) is refused before any cost is charged with `409 support_facing_mismatch`; rotate the support 180° or swap source and target.
- Place the MKI Roads spline road prototype from explicit points with `POST /v1/place-spline-road` (`place_spline_road`; temporary, until the road has a build-gun hologram): `{ points: [{ location, arriveTangent, leaveTangent }] (2-16, world cm, the deck top centerline), lanes: 1-10, confirm, requestId }`. `dryRun` may be sent too but must be the opposite of `confirm`; only `confirm: true` builds, because the dispatcher applies authority and idempotency to that field. Every point must be inside the placement safety radius. Each segment is sampled on the same cubic Hermite curve the spline component builds and must be 4-100 m long, never vertical, at most a 25° (46.63 %) grade, as vanilla rail, and no tighter in horizontal radius than the deck width (8 m per lane). The preview reports `lengthCm`, `maxGradePercent`, `minRadiusCm`, per-`segments` facts, `foundationEquivalents` (`ceil(length x width / 64 m2)`), and `boundsOverlapIds` (buildables inside the road's bounds, reported but not refused, since a road end may touch another road). Confirmation charges the asphalt 8x4 foundation recipe `foundationEquivalents` times, spawns `MKIRoadSplineRoad` at the first point (unrotated, the points made relative to it), and verifies the built spline length, width, and swept deck (`deckSliceCount`), rolling back with a refund otherwise (`409 placement_verification_failed`). The class is resolved by its native path; with an older MKI Roads the route answers `409 spline_road_unavailable` before charging anything. `/v1/dismantle` refunds a spline road the same multiple it was charged. A body with `start` and `end` (`{ recipePath, start: { eye, target }, end: { eye, target }, lanes, straight?, confirm, requestId }`) drives the road's build-gun hologram instead. It spawns the hologram of an MKI Roads spline road recipe, traces the build gun's channel from each eye through its target, and clicks the start. It then aims at the end and reports the hologram's `plannedLengthCm`, `foundationEquivalents`, `disqualifiers`, and `canConstruct`. When confirmed, it clicks again, charges the recipe `foundationEquivalents` times, constructs, and verifies the deck, refunding on failure.
- Drive the player's own build gun with `POST /v1/build-gun` (`build_gun`): `{ recipePath, steps: [{ op }] (1-64), inspect?: [actor names] (0-16), confirm, requestId }`. Besides `aim`, `scroll`, `snap` (Ctrl), `press`, `release`, `wait` and `report`, a step can be `lock` (`enabled`: the hologram lock, H) or `nudge` (`x` forward/back, `y` right/left, `z` up/down, each 1 or -1: one press of the nudge keys while locked, #377); every report carries the hologram's `locked` and `nudgeOffset`. A customization recipe (a swatch or pattern, `FGCustomizationRecipe`) runs the paint state, as choosing it in the customizer does; the preview then reports `customizationRecipeAvailable` (whether the build gun offers it, that is, it is unlocked), and every paint-state report adds `patternHitValid`, `swatchHitValid`, and `activePatternPath` for the aimed buildable. After the run, `inspected` reads back each named actor's `customization` and, for a spline road, its `lanes`, `markingLines`, `markingVertices`, and `deckPatternId` (the pattern ID its deck material receives).
  - **Steps:** `aim` (`position`, `yaw`, `pitch`: the player's location and view), `scroll` (`delta`: the rotate input), `snap` (`enabled`: the build gun's snap-mode modifier, which holding Ctrl drives), `press` and `release` (primary fire), `wait` (`seconds`, at most 10), and `report`.
  - **Run:** a confirmed run equips the build gun in build state with the recipe, then runs the steps a short beat apart, so the build gun ticks and traces from the camera between them, as in play.
  - **Result:** after every step it reports the build gun's hologram (class, location, yaw, `splineBuildStep`, `disqualifiers`, `canConstruct`, `materialState` (the colour state the build gun set: 0 ok, 1 warning, 2 error), and, when the hologram previews a deck of its own, `previewYaw`, `previewStencil` (the custom-depth stencil it renders with), `clearanceBoxes`, `straight`, and `plannedLengthCm`). At the end it lists every buildable or vehicle actor that appeared during the run, except temporaries of lightweight instances (`created`), then leaves build state.
  - **Cost:** the build gun charges as in play; the route itself charges nothing. A preview only validates the request.
- Charge a single `type: "foundation"` placement its recipe cost, exactly like a grid cell, and refund it if the spawn fails.
- Dismantle lightweight instances with the same checks as actors: the canonical ID, the mutation safety radius, `CanDismantle` (evaluated on the instance's temporary actor, which is destroyed before removal), host authority, and a refund. Both paths refund the build recipe's ingredients once (`refunded`, `refundRecipePath`), the same unit the bridge charges when it builds; spline length is not modelled, and nothing is added with no-build-cost enabled. Both paths now enforce the mutation safety radius.
- Bound `GET /v1/power-connections`: `radiusCm` (default 10000, the longest power line; clamped to the observation radius) and `limit` (default 200, 1–2000), nearest first, with `radiusCm`, `limit`, `totalAvailable`, and `truncated` in the response.
- Cap `/v1/site-survey` at 4096 support samples. A larger grid is sampled more coarsely; the response reports `sampleCount`, `sampleSpacingCm` (effective), `requestedSampleSpacingCm`, `samplesCapped`, and `maximumSamples`. `summaryOnly: true` omits the per-sample `samples` array and keeps every count, `supportIds`, and `overlaps`.
- Seven never-routed handlers were removed (`HandleLoadGeneratorFuel`, `HandleConnectBelt`, `HandleLoadInventory`, `HandlePlaceBiomassPower`, `HandlePlaceStorageAtBelt`, `HandlePlaceMachineBelt`, `HandlePlaceConstructor`; native part of #112).
- `POST /v1/place` with `type: "conveyor_lift"` takes `bottom` and `top` endpoints, each `{ position, facing, role }`. `facing` points outward from the lift along the belt attached at that end: the receiving end faces back toward the incoming belt and the delivering end faces along the outgoing belt. For a straight-through lift the two are horizontally opposite. The bottom and top turn independently, so a lift may also turn the belt: the bottom accepts any horizontal facing, and the top turns relative to the bottom in the hologram's 90° steps, so the angle between the two facings must be a multiple of 90° (`400 invalid_lift_facing` otherwise, and for a vertical or zero facing). The bridge orients the hologram from the bottom facing and sets the top's scroll rotation to the top facing's yaw relative to the bottom (verified live: built top yaw = bottom yaw + scroll value, in 90° steps). This matches the outward connector normals the buildables listing reports for an existing lift (`lift.bottom.facing`, `lift.top.facing`). After construction the bridge reads the built connector normals back: the answer reports `builtBottomFacing`, `builtTopFacing` and `facingVerified: true`, and a lift whose built facings differ from the request is rolled back and refunded with `409 lift_facing_mismatch`.
- Resolve output-anchored placement position and direction from the selected output connector.
- Save the loaded world with `POST /v1/save-game` (`{ saveName?, overwrite?, confirm, requestId }`) through the game's own save session. A preview reports the resolved name, `sessionName`, and `exists`. The default name is a new `<session>_ai_<yyyyMMdd-HHmmss>` (with the first free `_2`…`_99` suffix when that second is already taken), so autosaves and player saves are never replaced. An existing save of the same session is replaced only by its exact name with `overwrite: true` (`409 save_exists` otherwise), and a save of another session is never replaced (`409 save_exists_in_other_session`). A confirmed save requires a `requestId`. The response is sent once the game reports the file written and its header reads back (`saved`, `savedAt`, `elapsedMs`), or with `500 save_timeout` after 120 s. Retrying a completed `requestId` replays its result with `replayed: true` instead of saving again, and a second save while one is being written answers `409 save_in_progress`.
- Support exact-ID buildable cleanup through a guarded preview/confirm route. Toolkit batch cleanup reacquires each target by recorded class and transform before every deletion, because lightweight indices can renumber.
- Find placement support with a vertical trace through the request. If that ray grazes a seam between structures and finds nothing, retry 5 cm inside each neighbour before reporting `surface_not_found` (`supportProbe`, `seam_nudge_v1`). The request position and hologram validation are unchanged.
- Let `/v1/site-survey` exclude up to 64 caller-named buildables (`ignoreActorIds`) from support traces and overlap checks: conventional actor names, and canonical lightweight IDs (`lw|<classPath>|<index>`), which exclude only that instance and never the shared `AbstractInstanceManager`. The survey echoes `ignoredActorIds`, `ignoredInstances`, `unresolvedIgnoreActorIds`, and `unresolvedIgnores` (with reasons). Terrain and every other actor or instance remain checked.
- Ask whether the world around a point is loaded with `GET /v1/region/loaded?x=&y=&z=&radiusCm=` (`region_loaded`, #305): `streamingCompleted` is the world partition's own state for every cell within `radiusCm` (100 to 50000, default 5000) being activated, `groundFound`/`groundZ`/`groundActorClass` a downward trace at the point, and `loaded` both. The gameplay driver waits on it after standing the player over a step.
- Report the vertical structure of world collision with `POST /v1/terrain/column-probe` (`terrain_column_probe`): `{ points: [{x, y}] (1-256), zTop, zBottom (at most 2 km apart), channel?: "worldStatic" | "visibility", includeBuildables?: false, excludeIds?, traceComplex?: false, includeSurfaces?: true }`. Each column is traced downward from `zTop` and upward from `zBottom`, restarting 1 cm past every surface (at most 64 surfaces and 256 traces per direction, else `truncated`). Downward traces meet faces that point up and upward traces meet undersides, so merging them top-down (a top enters a solid, an underside leaves it) yields `layers` (`top`, `bottom`, kinds `landscape` | `terrain` | `buildable`, `topBeyondRange`/`bottomBeyondRange`) and the open `gaps` between them (`heightCm`, `openAbove`, `openBelow`, floor kind). An overhang or arch is a layer with open space below it; rock sunk into the ground hides its underside inside another solid and counts in `embeddedSurfaces`. A trace that starts inside a solid that reports its start steps on (50 cm, doubling to 4 m; `insideStepsDown`/`insideStepsUp`). A step never jumps over another component's face: inside a solid the probe also traces once ignoring that solid, and a face it finds before the next step would land (landscape under a sunk tree trunk, a rock top under the landscape) is recorded (`embeddedHits`). Built structures are skipped unless `includeBuildables` is true. `water` lists each `FGWaterVolume` containing part of the column with its bisected `top` and `bottom`, and `waterSurface` is the highest top. Points beyond the observation radius answer `ok: false`, `reason: "outside_observation_radius"`. Landscape answers upward traces from below too, but it has no underside: a landscape hit from below is listed in `surfaces` (`facing: "down"`) and never ends a solid, so everything under the landscape surface reads as ground down to `zBottom`. Because the landscape itself roofs some caves, each column also reports raw `openIntervals`: open space from `zTop` (unless the trace started inside a solid) and from every underside, including a landscape hit from below, down to the next upward face. Each interval carries `topBound` (`underside` | `range`) with the roof kind and class, `bottomBound` (`floor` | `range`) with the floor kind, class and normal, and `underLandscape`. Space under a landscape is either a landscape-roofed cave or buried ground; the bridge does not decide. The MCP helper `connectedOpenSpace` (`column-space.ts`) keeps only intervals connected to known open air, treating the landscape as a shell, and `groupCellLevels` splits an 8 m cell into levels instead of mixing a ledge with the space under it. Surfaces the player can clear carry `removable`: `destructible` (`AFGDestructibleActor`: nobelisk rocks, gas pillars), `deposit` (`AFGResourceDeposit` rubble), or `foliage` (an `InstancedFoliageActor` component whose foliage type `AFGFoliageRemovalSubsystem::IsRemovable` accepts); intervals carry it as `topRemovable`/`bottomRemovable`. `skipRemovable: true` traces through them as if cleared (`skippedRemovableHits`). Pawns such as creatures are always skipped (`skippedPawnHits`).
- Identify world actors (`world_identity`). `POST /v1/terrain/column-probe` with `includeIdentity: true` adds an `identity` object to every surface: `actorName`, `actorClass`, `actorClassPath`, `actorPath`, `levelPlaced`, `hidden`, `collision`, `hlod`, `level` (`name`, `package`, `persistent`, `worldPartitionCell`: a world partition runtime cell is its own level, named after the cell), actor `bounds` (`min`, `max`, `size`), and the hit `component` (`name`, `class`, `mobility`, `visible`, `collision`, `bounds`, `mesh` path, and for an instanced component `instanced`, `instanceCount`, `instanceIndex`, `instanceLocation`, `instanceScale`). `GET /v1/world/actor?name=` answers the same for a loaded actor by its exact name, with every primitive component (at most 64), within the observation radius. `POST /v1/world/static-meshes { mesh, x, y, z?, radiusCm? (100-50000, default 10000), limit? (1-200, default 50) }` lists the loaded actors whose static mesh components draw a mesh (`mesh` is a full object path, or a case-insensitive part of the mesh name) within `radiusCm` of the point (3D with `z`, else 2D); an instanced component (foliage, HLOD, buildable instances) reports `instancesInRange` and the first 20 `instances`. `matched` counts every match, `truncated` says whether `results` stops at `limit`.
- Remove and restore level rocks through MKI Tools' Terrain dismantle mode (`terrain_rocks`). The bridge calls MKI Tools by reflection and never requires it: without it these routes answer `501 mki_tools_missing`, and before its subsystem exists `503 terrain_subsystem_unavailable`. `GET /v1/terrain/removed-rocks` lists the removed rocks (`actorName`, `actorPath`, `mesh`, `location`, `bounds`, `removedBy`, `removedAt`, `loaded`, `hidden`) and `hlodInstancesHidden`. `POST /v1/terrain/remove-rock { actorName, confirm }` removes a loaded rock by its exact actor name (as the column probe reports `actorId`); a dry run reports `removable`. The rock must be within the mutation safety radius, and MKI Tools refuses anything but a plain level-placed static mesh rock (`409` with its reason: `not_static_mesh_actor`, `not_level_placed`, `not_static`, `reserved_mesh`, `hidden`, ...). A removed rock is hidden and loses its collision, never destroyed; the answer reports `verified` and `hlodInstancesHidden`. `POST /v1/terrain/restore-rock { actorName, confirm }` brings one back (`404 not_removed` when it is not on the list). Neither refunds anything. Only the host can confirm either.
- Ask where the build gun would put a hologram with `POST /v1/hologram-probe` (`hologram_probe`, #148): `{ recipePath, cases: [{ eye: {x, y, z}, target: {x, y, z}, scrollRotation? }] (1-256) }`. One hologram of the recipe is spawned; each case traces the build gun's own channel (`TC_BuildGun`) from `eye` through `target`, sets the scroll rotation, and hands the hit to the hologram's `SetHologramLocationAndRotation` and then its `TrySnapToActor` (the native snap; headless, the first call alone records the snap target without moving the hologram, so both are called, as `/v1/place` does). A lightweight hit is handed over as that instance's temporary buildable, as the build gun does. `afterSetLocation` and `trySnapToActor` report the two steps. Each result reports the hit (`hitTarget`, with lightweight foundations resolved to `lw|<classPath>|<index>`), the hologram `transform` (location, yaw, pitch, roll), `snappedToId`, `canConstruct`, and the native `disqualifiers`. The hologram is destroyed afterwards and the control rotation restored; nothing is built, charged, or saved. Every eye and target must be within the placement safety radius of the player. The modular MKI Roads snapping harness used it instead of the camera (removed with the modular pieces, #180).
- Keep an unattended survey alive with `player_safety` (#109). `GET /v1/player/vitals` answers whenever a local player controller exists, also while the pawn is dead or respawning (when `/v1/player` answers 503): `hasPawn`, `alive`, `dead`, `needRespawn`, `respawning`, `health`/`maxHealth`, `position`, `insideVolumes` (every volume actor of any class that encompasses the player, with class and bounds), `mapArea` (the display name of the `AFGVolumeMapArea` region there), the minimap `worldBounds`, and `killZ`. `GET /v1/world/volumes` lists every volume actor with its class and bounds plus a per-class count; `classContains=a,b` keeps classes whose name contains any substring, and `geometry=1` (only with `classContains`) adds each volume's convex brush hulls as world-space vertices, so a caller can store an irregular play-area edge. Map-area volumes report `mapArea`. `POST /v1/player/respawn { confirm }` starts the game's own respawn (`AFGPlayerController::StartRespawn`) only when the controller needs one; otherwise it reports `started: false`. None of these routes builds or dismantles anything.
- Report survey support and overlaps with stable per-instance identities (`identityModel: "instance_v1"`, `supportIds`, overlap `id`/`actorId`/`instanceResolved`). Only the exact supporting structure is exempt from overlap reporting, and an instanced hit the manager cannot resolve is reported rather than masked. The module links `AbstractInstance` for `ResolveHit`/`ResolveOverlap`. The descriptor declares it as a `BasePlugin` dependency, with no `SemVersion`. The game ships AbstractInstance as an engine-level plugin: `FactoryGame.uproject` enables it, FactoryGame's own module links it, and its DLL is installed under `FactoryGame/Plugins/AbstractInstance`. It is a platform dependency like SML, not a separately installed mod. The entry makes the load order and the runtime dependency explicit.
- Default mutating operations to a configurable 2 km player safety radius (`MKIBridge.MaximumMutationDistanceCm`); keep observation independent of that mutation guard.
- Default spatial observation to a separately configurable 10 km radius (`MKIBridge.MaximumObservationDistanceCm`). Configure Shipping builds under `[SystemSettings]` in `Engine.ini`; native code clamps invalid or excessive values.

## Safety invariants for future write routes

- Bind to loopback and reject non-loopback clients (both enforced; see Current contract).
- Require a generated authentication token.
- Require a non-mutating `confirm=false` preview before confirmed requests at the planning layer.
- Require exact buildable class identifiers and explicit transforms.
- Restrict construction to configured world-space bounds.
- Validate resources, unlocks, collision, snapping, and placement disqualifiers in the game thread.
- Return stable actor and connection IDs, retain request receipts for timeout reconciliation, and verify placement through native observations.
- Log every request and result without logging the authentication token.
