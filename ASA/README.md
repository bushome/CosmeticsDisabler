# CosmeticsDisabler (ASA) - build 0.5.1

CosmeticsDisabler keeps applied cosmetics (structure skins and item skins, official and mod) from reaching clients,
which stops the crashes and slow base loading they cause, mostly on consoles. Skins are only ever removed from
the structure or item; the structure or item stays. Skins stay unlocked on the player's account, so they can be
applied again once the plugin is off.

Everything is off or observe-only by default. The plugin changes game data in four ways, each with its own switch:

- the structure **Strip** command (`EnableStrip`; always starts as a dry run; backup first; `Restore` puts skins back),
- the item strips `StripItemsOnEntry` and `StripSavedItems` (backup first, `BackupStrippedItems`),
- the apply guard `BlockSkinApply` (refuses new skins),
- `FilterItems` (blanks the skin in item data sent to clients only; the server item is untouched).

All other hooks call the original game function first and then only read values and count. Run it on a test
server (full live mod and plugin loadout, with a PC client and a console client connected) before a live map.

## What it logs

| Hook | What is counted |
|---|---|
| `UPrimalItem.GetItemNetInfo(bool)` (only when building data for a client) | items sent, items carrying a skin class, a mod cosmetic id or custom item data; skin class names; mod ids |
| `AShooterCharacter.PreReplication` | characters carrying a hair / eyebrow / facial hair mod cosmetic id (only positive ids count; -1 and 0 mean none), plus every character's platform: the crossplay platform (`Windows`, `XSX`, `PS5`, `Unknown`) and the connection platform byte (1 PC, 3 Xbox Series X, 4 Xbox Series S, 5 PS5 as measured on a test server, 2 / 6 unconfirmed) |
| `APrimalStructure.MultiHandleSkinNetMessage` | structure skin messages (the multicast that pushes a skin change), message names, structure classes |
| `UPrimalInventoryComponent.AddItem`, `NotifyItemAdded`, `UPrimalItem.InitFromNetInfo`, `AddNewItem` (with `ProbeItemEntry`) | how many items entering inventories carry a skin, and which of those the item strip removed |
| `UActorChannel.ReplicateActor()` (with `ProbeStructureChannels`) | skinned structures sent to each connection, by connection platform; distinct connection/structure pairs; time spent in the call |
| `UPrimalItem.InventoryLoadedFromSaveGame` (with `ProbeItemEntry` or `StripSavedItems`) | items loaded from the save with a skin, their owner class and team, and how many were stripped |

A summary is written to the plugin log every `SummaryInterval_Seconds`; up to `MaxDetailLinesPerSummary`
individual lines per window show examples.

## Item skins: where they are removed

| Route | Switch | How |
|---|---|---|
| Transmitter, vault restore (LogoutVaults), obelisk / ARK Data, dropped or moved items | `StripItemsOnEntry` | `InitFromNetInfo`: the skin is blanked in the item data before the game builds the item |
| Items loaded from the saved map (storage, vaults, crafting stations, a player's own inventory after a restart) | `StripSavedItems` | `InventoryLoadedFromSaveGame`, then the game's own `ServerRemoveItemSkinOnly`; only when the owner's team is a player or tribe (50000 and up). A mod cosmetic is a mod id on the item with no skin class; it is cleared directly (mod id and variant set to 0) |
| A player applies a skin from the cosmetics tab | `BlockSkinApply` | `UseItemOntoItem` returns false and the player gets `BlockSkinApplyMessage` |
| Item data on its way to a client | `FilterItems` | skin blanked in the client copy only |

Things to know:

- `FilterItems` does not stop a mod cosmetic: the client still installs the mod when it receives an item that carries a mod
  cosmetic id (tested with a client that did not have the mod). Only the strip (`StripSavedItems`) clears the id.
- A weapon skin works by swapping the item's weapon class. Stripping it returns the normal weapon (a Cutlass skin on a
  sword goes back to the plain sword model). `FilterItems` cannot do that: it fixes icons and armor, but an equipped
  weapon keeps its skinned model. Use the strips for weapons.
- NPC gear keeps its skins. Patrol Thralls, pirates and other NPC items belong to NPC teams (below 50000), and
  `StripSavedItems` skips them. In tests with `StripItemsOnEntry` on, patrol Thralls spawned with their normal gear.
- Items that already carry a skin when you turn the strips on stay skinned until they are reloaded from the save or
  moved (a logout and login does not reload them). A server restart strips them.
- Every strip writes `backups/items-<time>.jsonl` first (one line per removed skin: item ids, item class, skin class and
  full class path, mod id, variant, owner class and team). There is no item restore command yet; the file is the record.

## Commands (console as admin, and RCON)

| Command | What it does |
|---|---|
| `CosmeticsDisabler.Scan` | counts structures with an official or custom skin (class names, skin ids); done a slice per second, the result goes to the log |
| `CosmeticsDisabler.Strip` | **dry run**: reports what would be stripped, changes nothing |
| `CosmeticsDisabler.Strip confirm` | strips structure skins. Needs `EnableStrip: true` and a finished dry run since the server started. Each change is first written to `ArkApi/Plugins/CosmeticsDisabler/backups/strip-<time>.jsonl`. The structures themselves stay |
| `CosmeticsDisabler.Restore [file]` | puts the skins from a backup file back; without a file it lists backups. Structures that have a skin again, or no longer exist, are skipped |
| `CosmeticsDisabler.Status` | writes the current counters to the log and replies with a one-line summary |
| `CosmeticsDisabler.Reload` | re-reads `config.json` |

## Config (`ArkApi/Plugins/CosmeticsDisabler/config.json`)

| Key | Default | Meaning |
|---|---|---|
| `ProbeItems` | `true` | log item data built for clients |
| `ProbeItemEntry` | `false` | log skinned items entering an inventory and count which game functions fire (changes nothing) |
| `ProbeUnlockedCosmetics` | `false` | at login, log the skin items in the player's own inventory with the game's unlocked-cosmetic flag, once per player per session (changes nothing) |
| `ProbePlayers` | `true` | log characters with hair / eyebrow / facial hair cosmetics |
| `ProbeStructureMessages` | `true` | log structure skin messages |
| `ProbeStructureChannels` | `false` | count skinned structures replicated to each connection by platform (PC / console / unknown) and the time spent; hooks `UActorChannel::ReplicateActor` (installed when first switched on). Test servers only |
| `BlockItemSkinsForConsoles` | `false` | blank item skins and mod cosmetic ids in the item data sent to console viewers only: a console player's own items, single items they receive, the gear they see on other characters, and weapons other players hold; PC viewers keep skins; the server item keeps its skin. Hooks `ServerRequestItems`, `ClientAddActorItem` and `ReplicateActor`; needs `ReadConnectionPlatform`. See *Console viewers* below |
| `UnequipSkinItemsForConsoles` | `false` | skin items worn on a character (outfit pieces from the cosmetics tab, e.g. the Dapper set) are left out of the equipped list while a console viewer's request is built, then put back at once; PC viewers untouched; same `ServerRequestItems` hook; needs `ReadConnectionPlatform` |
| `DeferStructureSkins_Seconds` | `0` | EXPERIMENTAL: PC players get skinned structures plain first; after this delay each player's queue is released `ReleaseStructureSkinsPerSecond` at a time and each structure is sent again with its skin; consoles unaffected; 0 = off |
| `ReleaseStructureSkinsPerSecond` | `50` | structure skins released per second per PC player (1 - 5000) |
| `ProbeClientRpcs` | `false` | count item / skin / cosmetic / equipment RPCs sent to console connections, by function and target class; probe only. Test servers only |
| `ProbeItemRequests` | `false` | log who asks for which items (platform of the viewer, equipped gear or not, skinned item count) and count skinned items built outside any request; hooks `UPrimalInventoryComponent::ServerRequestItems`; probe only, changes nothing. Test servers only |
| `TreatAsConsolePlayers` | `[]` | testing only: EOS IDs whose connection is handled as a console whatever platform it reports, so the console path can be tested from a PC; leave empty on live |
| `ReadConnectionPlatform` | `true` | read `UNetConnection::ConnectionPlatform` for each player; turn off if it ever misbehaves |
| `SummaryInterval_Seconds` | `30` | seconds between summaries (5 - 3600) |
| `MaxDetailLinesPerSummary` | `50` | cap on example lines per summary (0 - 1000) |
| `ScanActorsPerTick` | `20000` | actors Scan / Strip check per second (100 - 1000000) |
| `EnableStrip` | `false` | master switch for `Strip confirm` and `Restore` |
| `StripMode` | `All` | `All` strips custom and official structure skins; `CustomOnly` strips custom cosmetics only |
| `AllowSkinClasses` | `[]` | skin class names never stripped (case-insensitive; names as printed by Scan) |
| `StripPerTick` | `50` | structures changed per second (1 - 1000) |
| `FilterItems` | `false` | blank item skins in the item data sent to clients; the item on the server keeps its skin |
| `FilterItemCustomData` | `false` | **experimental**: also clear the custom item data on the client copy of filtered items |
| `BackupStrippedItems` | `true` | record every removed item skin in `backups/items-<time>.jsonl` before removing it; if the file cannot be written nothing is removed |
| `StripSavedItems` | `false` | remove the skin (the game's own removal call) from player- and tribe-owned items (team 50000 and up; NPC gear is skipped) as they are rebuilt from the save at map load; the item stays and a weapon returns to its normal model; changes saved data, test first |
| `StripItemsOnEntry` | `false` | blank the skin in item data before the game builds an item from saved or transferred data (LogoutVaults restores, inventory moves, drops); items already in the world are unchanged until moved |
| `BlockSkinApply` | `false` | refuse new structure skins and new item skins (applied from the cosmetics tab) that `StripMode` would strip; removing a skin is always allowed |
| `BlockSkinApplyMessage` | `""` | message sent when `BlockSkinApply` refuses a skin: to the player who tried to apply an item skin; for a structure skin to the player standing nearest the structure (within 30 m, at most once per 5 s each), because the server cannot tell who clicked; empty = silent |
| `StripOnMapLoad` | `false` | run the structure strip automatically after the map has loaded (needs `EnableStrip`) |
| `StripOnMapLoadDelay_Seconds` | `120` | wait after startup before that automatic strip (10 - 3600) |
| `StripRepeat_Minutes` | `0` | repeat the automatic strip every N minutes (0 = once); catches cells that stream in later |

`FilterItems`, `BlockSkinApply` and the strip all follow `StripMode` and `AllowSkinClasses`. Item mod cosmetic ids
cannot be blanked in the client copy (the net info has no field for them); with `FilterItemCustomData` on, the custom
item data is cleared instead, which may carry them. Test that on a test server first.

`BlockSkinApply` hooks `SetStructureSkin` (structures) and `UseItemOntoItem` (items). If you see "blocked skin apply" lines
at startup, the game applies saved structure skins through the same call, and the guard is covering those too.

Strip needs the structure skin class fields (`ReplicatedStructureMySkinClass`, `StructureSkinClass`), which are
not in the AsaApi headers and are read by name. On a first run, check that Scan's official/custom counts look
right (and that its sanity line `HasAppliedCustomCosmetic check` shows 0 mismatches) before any `Strip confirm`.

`config.commented.json` holds the same values with comments. A bad config logs an error and keeps the previous
settings; it never stops the plugin from loading.

## Build

Shared API checkout (optional): the project finds AsaApi through the `AsaApiDir` MSBuild property (for example
set by a `Directory.Build.props` above the repo, as on the dev machine) and falls back to the
`extern\AsaApi` submodule.

1. Make sure AsaApi is available (shared checkout, or `git submodule update --init --recursive`) and that
   `out_lib\AsaApi.lib` exists there (build `AsaApi.sln` Release x64 once, or copy it from the AsaApi release zip).
2. Open `CosmeticsDisabler.sln`, select **Release | x64**, build.
3. Output: `out\CosmeticsDisabler.dll` (plus `.dll.arkapi` for hot reload) and the ready-to-copy folder
   `out\CosmeticsDisabler\`.

vcpkg (bundled with Visual Studio) restores `fmt` automatically in manifest mode.

## Install

Copy the `out\CosmeticsDisabler\` folder to `ShooterGame\Binaries\Win64\ArkApi\Plugins\` on the server. It needs
no other plugin. The server needs the Visual C++ runtime that AsaApi itself requires (see the ASABinaries repo).

## What to look for

1. Do `GetItemNetInfo(true)` counters move when a player opens an inventory with a cosmetic in it? (`items with skin` > 0)
2. Does the `connection=` value match each tester's device (PC 1, PS5 3, Xbox Series 4) while `crossplay=` stays `Unknown`?
3. After applying a skin to a structure, does a `structure skin message` line appear?
4. Run `CosmeticsDisabler.Scan` and compare the number of structures with a cosmetic against what you expect.
5. With the item strips on and `ProbeItemEntry` on, a skinned item from a transmitter or obelisk shows
   `InitFromNetInfo ... removed=yes`, and one from the save shows `item loaded from save: ... removed=yes` with
   `weaponTemplate` back to the plain class. Patrol Thralls should show no `removed=yes`.
6. Is the server tick time affected? The hooks only count; watch the usual performance numbers.

## Tests

`tests\logic_tests.cpp` tests the game-free logic in `src\ProbeLogic.h` (config parsing, counters, formatting).
It needs no game and no Windows:

```
cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp && logic_tests.exe configs\config.json --shipped
```

## Console viewers (`BlockItemSkinsForConsoles`, `UnequipSkinItemsForConsoles`)

Mode for "allow PC players, block consoles": PC players keep every cosmetic; console players get plain items. Nothing is
removed from the server items, so a console player who moves to PC still has their skins.

| What the console player would get | Route | What the plugin does |
|---|---|---|
| Their own inventory, and the worn gear of characters they see | `ServerRequestItems` | skin class, mod cosmetic id and variant blanked in the copy sent to them; custom data cleared with `FilterItemCustomData` |
| Outfit skins from the cosmetics tab worn on a character | `ServerRequestItems` (equipped list) | with `UnequipSkinItemsForConsoles`, skin items are left out of the equipped list for that request only |
| A single item (skin applied, picked up, moved, crafted) | `ClientAddActorItem` | same blanking as above |
| A weapon another player holds | `ReplicateActor` (`AShooterWeapon.AssociatedItemNetInfo`) | skin and mod id blanked for console connections only, restored right after |

Things to know:

- Verified with PC players handled as consoles (`TreatAsConsolePlayers`): worn outfits, applied skins, mod shirt and mod
  sword hidden, and the test client started no mod download. Not yet verified with a real console.
- A console player who applies a skin sees the item plain. That is intended, not a bug.
- Official skins that swap the weapon class (for example the Cutlass on a sword) stay visible: there is one weapon actor
  for everyone, so its class cannot differ per viewer. These models ship with the game; nothing is downloaded.
- The held-weapon blanking runs on every replication of a held skinned weapon (about 15 to 28 times a second per weapon
  and console viewer). It is logged once per weapon and viewer; the summary counts every pass as `held weapons N`.
- The mod cosmetic id fields (`FItemNetInfo.CustomCosmeticModSkinReplacementID`, `...VariantID`) are read through the
  offsets cache; AsaApi's headers do not list them. They must be in the server's cache.
- Empty `TreatAsConsolePlayers` on live servers; it is for testing only.

## Troubleshooting

- **Log says `Hooks: ... FAILED`:** AsaApi could not resolve that function in its offsets cache (for example after a
  game update). The other hooks still run. Update the cache as usual.
- **No counters move:** check the plugin log folder for `Hooks: ... ok`, and that the matching `Probe...` switch is `true`.

## Files

- `src\Main.cpp` - hooks, commands, scan, plugin entry points
- `src\ProbeLogic.h` - config parsing, counters, formatting (no game types)
- `configs\` - `config.json`, `config.commented.json`, `PluginInfo.json`
- `tests\logic_tests.cpp` - stand-alone tests
- `vcpkg.json` - `fmt`
