// Stand-alone tests for src/ProbeLogic.h. No game, AsaApi DLL or Windows needed.
//
// Build and run from the project folder (any C++20 compiler):
//   cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp && logic_tests.exe configs\config.json
//   clang++ -std=c++20 -I extern/AsaApi/AsaApi/Core/Public -I src tests/logic_tests.cpp -o logic_tests && ./logic_tests configs/config.json
//
// The optional argument is a config.json to validate. Add --shipped after it to also check the file
// against the expectations for the shipped config at the bottom of this file:
//   logic_tests.exe configs\config.json --shipped
// configs\config.commented.json holds the same values, so it must pass the same check.

#include <fstream>
#include <iostream>
#include <string>

#include "ProbeLogic.h"

using namespace CosmeticsProbe;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition) \
	do { \
		++g_checks; \
		if (!(condition)) { \
			++g_failures; \
			std::cerr << "FAILED line " << __LINE__ << ": " #condition "\n"; \
		} \
	} while (0)

template <typename F>
static bool Throws(F&& f)
{
	try { f(); }
	catch (const std::runtime_error&) { return true; }
	return false;
}

static nlohmann::json Parse(const std::string& text) { return nlohmann::json::parse(text, nullptr, true, true); }

static void TestSettings()
{
	const Settings d = ParseSettings(Parse("{}"));
	CHECK(!d.probe_items && !d.probe_players && !d.probe_structure_messages);   // probes default off
	CHECK(!d.probe_item_entry && !d.strip_items_on_entry && !d.probe_unlocked_cosmetics);
	{
		Settings on;
		on.strip_items_on_entry = true;
		CHECK(ShouldStripItemOnEntry(on, "PrimalItemSkin_Cutlass_Sword_C"));
		CHECK(!ShouldStripItemOnEntry(Settings{}, "PrimalItemSkin_Cutlass_Sword_C"));
		on.allow_skin_classes = {"PrimalItemSkin_Cutlass_Sword_C"};
		CHECK(!ShouldStripItemOnEntry(on, "primalitemskin_cutlass_sword_c"));
	}
	CHECK(ParseSettings(Parse(R"({"StripItemsOnEntry": true})")).strip_items_on_entry);
	CHECK(d.backup_stripped_items);
	CHECK(!ParseSettings(Parse(R"({"BackupStrippedItems": false})")).backup_stripped_items);
	{
		ItemBackupEntry b;
		b.kind = "load";
		b.item_id1 = 12;
		b.item_id2 = 34;
		b.item_class = "PrimalItem_WeaponSword_C";
		b.skin_class_name = "PrimalItemSkin_Cutlass_Sword_C";
		b.mod_id = 0;
		b.owner_class = "StorageBox_Large_CS_C";
		b.owner_team = 1047757211;
		const nlohmann::json j = nlohmann::json::parse(ItemBackupLine(b));
		CHECK(j["kind"] == "load" && j["item"][0] == 12 && j["item"][1] == 34);
		CHECK(j["skin"] == "PrimalItemSkin_Cutlass_Sword_C" && j["team"] == 1047757211);
	}
	CHECK(!d.strip_items_on_load);
	CHECK(ParseSettings(Parse(R"({"StripSavedItems": true})")).strip_items_on_load);
	{
		Settings ld;
		ld.strip_items_on_load = true;
		CHECK(ShouldStripItemOnLoad(ld, "PrimalItemSkin_Cutlass_Sword_C", 1234567890)); // tribe
		CHECK(ShouldStripItemOnLoad(ld, "PrimalItemSkin_Cutlass_Sword_C", 50000));
		CHECK(!ShouldStripItemOnLoad(ld, "PrimalItemSkin_Thrall_Sword_C", 2));          // NPC team
		CHECK(!ShouldStripItemOnLoad(ld, "PrimalItemSkin_Thrall_Sword_C", 0));          // owner unknown
		CHECK(!ShouldStripItemOnLoad(Settings{}, "PrimalItemSkin_Cutlass_Sword_C", 1234567890));
		// Mod cosmetic id with no skin class (e.g. mod 1377647 on a helmet or a sword)
		CHECK(ShouldStripItemModCosmeticOnLoad(ld, 1377647, 1388501420));
		CHECK(ShouldStripItemModCosmeticOnLoad(ld, 1377647, 50000));
		CHECK(!ShouldStripItemModCosmeticOnLoad(ld, 1377647, 2));      // NPC team
		CHECK(!ShouldStripItemModCosmeticOnLoad(ld, 1377647, 0));      // owner unknown
		CHECK(!ShouldStripItemModCosmeticOnLoad(ld, 0, 1388501420));   // no mod id
		CHECK(!ShouldStripItemModCosmeticOnLoad(ld, -1, 1388501420));  // -1 means none
		CHECK(!ShouldStripItemModCosmeticOnLoad(Settings{}, 1377647, 1388501420)); // switch off
		ld.allow_skin_classes = {"PrimalItemSkin_Cutlass_Sword_C"};
		CHECK(!ShouldStripItemOnLoad(ld, "primalitemskin_cutlass_sword_c", 1234567890));
	}
	CHECK(ParseSettings(Parse(R"({"ProbeItemEntry": true})")).probe_item_entry);
	CHECK(ParseSettings(Parse(R"({"ProbeUnlockedCosmetics": true})")).probe_unlocked_cosmetics);
	CHECK(!d.probe_structure_channels);
	CHECK(ParseSettings(Parse(R"({"ProbeStructureChannels": true})")).probe_structure_channels);
	CHECK(!d.probe_item_requests);
	CHECK(d.block_item_skins_for_consoles && d.block_structure_skins_for_consoles);   // console blocking defaults on
	CHECK(!d.probe_client_rpcs);
	CHECK(d.unequip_skin_items_for_consoles);
	CHECK(!ParseSettings(Parse(R"({"UnequipSkinItemsForConsoles": false})")).unequip_skin_items_for_consoles);
	CHECK(!ParseSettings(Parse(R"({"BlockItemSkinsForConsoles": false})")).block_item_skins_for_consoles);
	CHECK(d.defer_structure_skins_seconds == 0);
	CHECK(d.release_structure_skins_per_second == 50);
	CHECK(ParseSettings(Parse(R"({"ReleaseStructureSkinsPerSecond": 200})")).release_structure_skins_per_second == 200);
	CHECK(Throws([] { ParseSettings(Parse(R"({"ReleaseStructureSkinsPerSecond": 0})")); }));
	CHECK(ParseSettings(Parse(R"({"DeferStructureSkins_Seconds": 15})")).defer_structure_skins_seconds == 15);
	CHECK(Throws([] { ParseSettings(Parse(R"({"DeferStructureSkins_Seconds": 5000})")); }));
	CHECK(ParseSettings(Parse(R"({"ProbeClientRpcs": true})")).probe_client_rpcs);
	CHECK(ParseSettings(Parse(R"({"BlockItemSkinsForConsoles": true})")).block_item_skins_for_consoles);
	CHECK(ParseSettings(Parse(R"({"ProbeItemRequests": true})")).probe_item_requests);
	CHECK(GroupOfPlatform(1) == PlatformGroup::PC);
	CHECK(GroupOfPlatform(4) == PlatformGroup::Console && GroupOfPlatform(3) == PlatformGroup::Console && GroupOfPlatform(5) == PlatformGroup::Console);
	{
		const Settings tc = ParseSettings(Parse(R"({"TreatAsConsolePlayers": ["0002841F112C4CEE8F3C828460BA88FF", ""]})"));
		CHECK(tc.treat_as_console_players.size() == 1);
		CHECK(IsTreatedAsConsole(tc, "0002841f112c4cee8f3c828460ba88ff"));
		CHECK(!IsTreatedAsConsole(tc, "00000000000000000000000000000001"));
		CHECK(!IsTreatedAsConsole(tc, ""));
		CHECK(!IsTreatedAsConsole(Settings{}, "0002841f112c4cee8f3c828460ba88ff"));
		CHECK(EffectiveGroup(1, true) == PlatformGroup::Console && EffectiveGroup(1, false) == PlatformGroup::PC);
		bool threw = false;
		try { ParseSettings(Parse(R"({"TreatAsConsolePlayers": "abc"})")); } catch (const std::exception&) { threw = true; }
		CHECK(threw);
	}
	CHECK(GroupOfPlatform(0) == PlatformGroup::Unknown && GroupOfPlatform(6) == PlatformGroup::Unknown && GroupOfPlatform(-1) == PlatformGroup::Unknown);
	CHECK(d.read_connection_platform);
	CHECK(d.summary_interval_seconds == 30);
	CHECK(d.max_detail_lines_per_summary == 50);
	CHECK(d.scan_actors_per_tick == 20000);
	CHECK(!d.enable_strip);                       // strip is off unless the operator turns it on
	CHECK(d.strip_mode == "All");
	CHECK(d.allow_skin_classes.empty());
	CHECK(d.strip_per_tick == 50);
	CHECK(!d.filter_items && !d.filter_item_custom_data && !d.block_skin_apply && !d.strip_on_map_load);
	CHECK(d.strip_on_map_load_delay_seconds == 120 && d.strip_repeat_minutes == 0);
	CHECK(d.block_skin_apply_message.empty());

	const Settings s = ParseSettings(Parse(R"({"ProbeItems": true, "SummaryInterval_Seconds": 60, // comment
		"MaxDetailLinesPerSummary": 0, "Unknown": 1})"));
	CHECK(s.probe_items && !s.probe_players);
	CHECK(s.summary_interval_seconds == 60);
	CHECK(s.max_detail_lines_per_summary == 0);

	CHECK(!ParseSettings(Parse(R"({"ReadConnectionPlatform": false})")).read_connection_platform);
	CHECK(Throws([] { ParseSettings(Parse(R"({"ReadConnectionPlatform": 1})")); }));
	CHECK(Throws([] { ParseSettings(Parse("[]")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"ProbeItems": "yes"})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"SummaryInterval_Seconds": 1})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"SummaryInterval_Seconds": 30.5})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"ScanActorsPerTick": 99})")); }));

	const Settings strip = ParseSettings(Parse(R"({"EnableStrip": true, "StripMode": "CustomOnly", "StripPerTick": 10,
		"AllowSkinClasses": ["StructureSkin_Tileset_LC_C", ""]})"));
	CHECK(strip.enable_strip && strip.strip_mode == "CustomOnly" && strip.strip_per_tick == 10);
	CHECK(strip.allow_skin_classes.size() == 1);       // empty names are dropped
	CHECK(Throws([] { ParseSettings(Parse(R"({"EnableStrip": "yes"})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"StripMode": "Some"})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"StripMode": 3})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"StripPerTick": 0})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"StripPerTick": 1001})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"AllowSkinClasses": "x"})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"AllowSkinClasses": [1]})")); }));

	const Settings f = ParseSettings(Parse(R"({"FilterItems": true, "FilterItemCustomData": true, "BlockSkinApply": true,
		"StripOnMapLoad": true, "StripOnMapLoadDelay_Seconds": 30, "StripRepeat_Minutes": 60})"));
	CHECK(f.filter_items && f.filter_item_custom_data && f.block_skin_apply && f.strip_on_map_load);
	CHECK(f.strip_on_map_load_delay_seconds == 30 && f.strip_repeat_minutes == 60);
	CHECK(ParseSettings(Parse(R"({"BlockSkinApplyMessage": "Skins are off here."})")).block_skin_apply_message == "Skins are off here.");
	CHECK(Throws([] { ParseSettings(Parse(R"({"BlockSkinApplyMessage": 5})")); }));
	CHECK(Throws([] { ParseSettings(Parse("{\"BlockSkinApplyMessage\": \"" + std::string(301, 'a') + "\"}")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"FilterItems": 1})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"StripOnMapLoadDelay_Seconds": 5})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"StripRepeat_Minutes": -1})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"StripRepeat_Minutes": 1441})")); }));
}

static void TestItemFilterAndGuard()
{
	Settings off;
	CHECK(!ShouldFilterItem(off, "PrimalItemSkin_FancyPirate_Hat_C", 0));       // filter is off by default
	CHECK(!ShouldBlockSkinApply(off, "StructureSkinCustomBP_C", 1234));

	Settings on;
	on.filter_items = true;
	on.block_skin_apply = true;
	CHECK(ShouldFilterItem(on, "PrimalItemSkin_FancyPirate_Hat_C", 0));          // official item skin
	CHECK(ShouldFilterItem(on, "(null)", 1800550));                              // mod cosmetic id only
	CHECK(!ShouldFilterItem(on, "(null)", 0));                                   // nothing to filter
	CHECK(!ShouldFilterItem(on, "(null)", -1));                                  // -1 means none
	CHECK(ItemSkinKind("PrimalItemSkin_X_C", 0) == SkinKind::Official);
	CHECK(ItemSkinKind("(null)", 7) == SkinKind::Custom);
	CHECK(ItemSkinKind("PrimalItemSkin_X_C", -1) == SkinKind::Official);

	on.strip_mode = "CustomOnly";
	CHECK(!ShouldFilterItem(on, "PrimalItemSkin_FancyPirate_Hat_C", 0));         // official kept in CustomOnly
	CHECK(ShouldFilterItem(on, "(null)", 1800550));
	CHECK(ShouldFilterItem(on, "PrimalItemSkin_X_C", 99));                       // mod id present -> custom

	on.strip_mode = "All";
	on.allow_skin_classes = { "PrimalItemSkin_TorchSparkler_Patch_C" };
	CHECK(!ShouldFilterItem(on, "PrimalItemSkin_TorchSparkler_Patch_C", 0));     // allowed class
	CHECK(ShouldFilterItem(on, "PrimalItemSkin_FancyPirate_Hat_C", 0));

	// Structure skin guard: applications are blocked, removals never.
	Settings g;
	g.block_skin_apply = true;
	CHECK(ShouldBlockSkinApply(g, "StructureSkin_Tileset_LC_C", 0));
	CHECK(ShouldBlockItemSkinApply(g, "PrimalItemSkin_Cutlass_Sword_C"));
	CHECK(!ShouldBlockItemSkinApply(g, "PrimalItemDye_Red_C"));
	CHECK(!ShouldBlockItemSkinApply(Settings(), "PrimalItemSkin_Cutlass_Sword_C"));
	CHECK(ShouldBlockSkinApply(g, "StructureSkinCustomBP_C", 1307475));
	CHECK(ShouldBlockSkinApply(g, "(null)", 1307475));
	CHECK(!ShouldBlockSkinApply(g, "(null)", 0));                                // removal request
	g.strip_mode = "CustomOnly";
	CHECK(!ShouldBlockSkinApply(g, "StructureSkin_Tileset_LC_C", 0));
	CHECK(ShouldBlockSkinApply(g, "StructureSkinCustomBP_C", 1307475));
	g.allow_skin_classes = { "StructureSkinCustomBP_C" };
	CHECK(!ShouldBlockSkinApply(g, "StructureSkinCustomBP_C", 1307475));
}

static void TestClassify()
{
	CHECK(ClassifySkin("(null)", 0) == SkinKind::None);
	CHECK(ClassifySkin("", 0) == SkinKind::None);
	CHECK(ClassifySkin("StructureSkin_Tileset_LC_C", 0) == SkinKind::Official);
	CHECK(ClassifySkin("Tileset_Frontier_C", 0) == SkinKind::Official);
	CHECK(ClassifySkin("StructureSkinCustomBP_C", 1287264) == SkinKind::Custom);
	CHECK(ClassifySkin("StructureSkinCustomBP_C", 0) == SkinKind::Custom);   // custom class even without an id
	CHECK(ClassifySkin("(null)", 12345) == SkinKind::Custom);               // id with both classes null
	CHECK(ClassifySkin("Tileset_Frontier_C", -5) == SkinKind::Custom);       // any non-zero id
	CHECK(std::string(SkinKindName(SkinKind::Official)) == "official");
	CHECK(std::string(SkinKindName(SkinKind::Custom)) == "custom");
	CHECK(std::string(SkinKindName(SkinKind::None)) == "none");
}

static void TestStripPolicy()
{
	Settings all;
	CHECK(ShouldStrip(all, SkinKind::Official, "Tileset_Frontier_C"));
	CHECK(ShouldStrip(all, SkinKind::Custom, "StructureSkinCustomBP_C"));
	CHECK(!ShouldStrip(all, SkinKind::None, "(null)"));

	Settings custom_only;
	custom_only.strip_mode = "CustomOnly";
	CHECK(!ShouldStrip(custom_only, SkinKind::Official, "Tileset_Frontier_C"));
	CHECK(ShouldStrip(custom_only, SkinKind::Custom, "StructureSkinCustomBP_C"));

	Settings allow;
	allow.allow_skin_classes = { "Tileset_Frontier_C" };
	CHECK(!ShouldStrip(allow, SkinKind::Official, "Tileset_Frontier_C"));
	CHECK(!ShouldStrip(allow, SkinKind::Official, "tileset_frontier_c"));     // case-insensitive
	CHECK(ShouldStrip(allow, SkinKind::Official, "ToF_Tileset_C"));
	CHECK(IsAllowed(allow, "TILESET_FRONTIER_C"));
	CHECK(!IsAllowed(allow, "Other_C"));
}

static void TestBackup()
{
	BackupEntry e;
	e.structure_id = 4000000123ULL;                // above 32 bits is fine, ids are unsigned
	e.structure_class = "Floor_Stone_C";
	e.skin_class_name = "StructureSkinCustomBP_C";
	e.skin_class_path = "Blueprint'/Game/Mods/123/StructureSkinCustomBP'";
	e.skin_id = 9007199254740993LL;                // beyond double precision
	e.variant = 3;

	const std::string line = BackupLine(e);
	CHECK(line.find('\n') == std::string::npos);  // one line per entry
	const BackupEntry back = ParseBackupLine(line);
	CHECK(back.structure_id == e.structure_id);
	CHECK(back.structure_class == e.structure_class);
	CHECK(back.skin_class_name == e.skin_class_name);
	CHECK(back.skin_class_path == e.skin_class_path);
	CHECK(back.skin_id == e.skin_id);
	CHECK(back.variant == 3);

	BackupEntry negative;
	negative.structure_id = 7;
	negative.skin_id = -42;
	CHECK(ParseBackupLine(BackupLine(negative)).skin_id == -42);

	CHECK(Throws([] { ParseBackupLine("not json"); }));
	CHECK(Throws([] { ParseBackupLine("[]"); }));
	CHECK(Throws([] { ParseBackupLine(R"({"sid": 1, "id": 2})"); }));            // no path
	CHECK(Throws([] { ParseBackupLine(R"({"sid": "1", "id": 2, "path": ""})"); })); // sid must be a number
	CHECK(!ParseBackupLine(R"({"sid": 1, "id": 2, "path": ""})").skin_class_path.size());

	CHECK(IsSafeBackupName("strip-1700000000.jsonl"));
	CHECK(!IsSafeBackupName(""));
	CHECK(!IsSafeBackupName("../x"));
	CHECK(!IsSafeBackupName("a/b"));
	CHECK(!IsSafeBackupName("a\\b"));
	CHECK(!IsSafeBackupName("C:x"));
	CHECK(!IsSafeBackupName(std::string(101, 'a')));
}

static void TestBudget()
{
	DetailBudget b;
	CHECK(!b.Take());           // nothing until Reset
	b.Reset(2);
	CHECK(b.Take());
	CHECK(b.Take());
	CHECK(!b.Take());
	b.Reset(-5);
	CHECK(!b.Take());
}

static void TestKeyCounter()
{
	KeyCounter k;
	CHECK(k.Format(3) == "none");
	k.Add("b"); k.Add("a"); k.Add("a"); k.Add("c", 5);
	const auto top = k.Top(2);
	CHECK(top.size() == 2 && top[0].first == "c" && top[0].second == 5 && top[1].first == "a");
	CHECK(k.Total() == 8 && k.Distinct() == 3);
	CHECK(k.Format(2) == "c x5, a x2, +1 more");
	// Ties sort by key.
	KeyCounter t;
	t.Add("z"); t.Add("y");
	CHECK(t.Top(2)[0].first == "y");
	k.Clear();
	CHECK(k.Distinct() == 0 && k.Format(3) == "none");
}

static void TestPlatform()
{
	CHECK(PlatformLabel(0) == "Unknown");
	CHECK(PlatformLabel(1) == "Windows");
	CHECK(PlatformLabel(2) == "XSX");
	CHECK(PlatformLabel(3) == "PS5");
	CHECK(PlatformLabel(9) == "Other(9)");
	CHECK(ConnectionPlatformLabel(-1) == "NotRead");
	CHECK(ConnectionPlatformLabel(1) == "PC(1)");
	CHECK(ConnectionPlatformLabel(3) == "XboxSeriesX?(3)");
	CHECK(ConnectionPlatformLabel(4) == "XboxSeriesS?(4)");
	CHECK(ConnectionPlatformLabel(6) == "Unknown(6)");
	CHECK(ConnectionPlatformLabel(42) == "Other(42)");
	CHECK(!HasCosmeticId(0) && !HasCosmeticId(-1) && HasCosmeticId(123));
}

static void TestFormatting()
{
	Counters c;
	c.items_to_client = 10;
	c.items_with_skin = 3;
	c.skin_classes.Add("SkinA", 2);
	c.character_prerep = 7;
	c.platforms.Add("PS5");
	c.connection_platforms.Add("XboxSeries(4)");
	c.items_filtered = 4;
	c.skin_applies_blocked = 2;
	c.blocked_skin_classes.Add("StructureSkinCustomBP_C", 2);
	const std::string text = FormatSummary(c, 30);
	CHECK(text.find("filtered: items 4 (custom data cleared 0), structure skin applications blocked 2: StructureSkinCustomBP_C x2") != std::string::npos);
	CHECK(text.find("last 30 s") != std::string::npos);
	CHECK(text.find("items to clients: 10, with skin: 3") != std::string::npos);
	CHECK(text.find("SkinA x2") != std::string::npos);
	CHECK(text.find("PS5 x1") != std::string::npos);
	CHECK(text.find("connection platform of all characters: XboxSeries(4) x1") != std::string::npos);
	CHECK(text.find("item requests") == std::string::npos);
	c.item_requests = 3;
	c.item_requests_equipped = 2;
	c.request_items = 40;
	c.request_items_skinned = 5;
	c.outside_items = 7;
	c.outside_items_skinned = 1;
	c.request_viewers.Add("XboxSeriesS?(4) equipped skinned", 2);
	c.items_blanked_for_console = 4;
	c.equipped_item_classes.Add("PrimalItemArmor_X_C (skinned)", 2);
	c.console_rpcs.Add("ClientAddActorItem on ShooterPC_C", 3);
	c.skin_items_unequipped = 4;
	c.unequipped_skin_classes.Add("PrimalItemSkin_DapperShirt_C", 2);
	const std::string req = FormatSummary(c, 30);
	CHECK(req.find("item requests: 3 (equipped 2), items built inside requests: 40 (skinned 5), outside requests: 7 (skinned 1), blanked for consoles: 4 (mod ids 0, held weapons 0), skin items unequipped for consoles: 4 PrimalItemSkin_DapperShirt_C x2") != std::string::npos);
	CHECK(req.find("request viewers: XboxSeriesS?(4) equipped skinned x2") != std::string::npos);
	CHECK(req.find("equipped item classes: PrimalItemArmor_X_C (skinned) x2") != std::string::npos);
	CHECK(req.find("item/skin RPCs to consoles: ClientAddActorItem on ShooterPC_C x3") != std::string::npos);
	c.Clear();
	CHECK(c.items_to_client == 0 && c.skin_classes.Distinct() == 0);

	ScanResult r;
	CHECK(r.Percent() == 0.0);
	r.actors_seen = 100; r.structures = 40; r.structures_with_skin = 10; r.official = 4; r.custom = 6;
	r.official_classes.Add("Tileset_Frontier_C", 4);
	r.custom_ids.Add("1287264", 6);
	r.structure_classes.Add("Wall_Stone_C", 10);
	r.flag_custom = 6;
	CHECK(r.Percent() == 25.0);
	std::string scan = FormatScan(r);
	CHECK(scan.find("10 with a skin (25.00%): 4 official, 6 custom") != std::string::npos);
	CHECK(scan.find("Tileset_Frontier_C x4") != std::string::npos);
	CHECK(scan.find("1287264 x6") != std::string::npos);
	CHECK(scan.find("Wall_Stone_C x10") != std::string::npos);
	CHECK(scan.find("6 true, 0 true but not custom") != std::string::npos);
	CHECK(scan.find("strip:") == std::string::npos);        // no strip line for a plain scan

	r.would_strip = 10;
	scan = FormatScan(r);
	CHECK(scan.find("strip: would strip 10, stripped 0, failed 0, kept 0") != std::string::npos);
}

// Expectations for configs/config.json and configs/config.commented.json as shipped.
static void TestPlayerCommands()
{
	const Settings d;
	CHECK(!d.enable_player_hide_commands && !d.enable_player_strip);
	CHECK(d.hide_skins_command == "/hideskins" && d.show_skins_command == "/showskins" && d.strip_skins_command == "/stripskins");
	CHECK(d.player_strip_cooldown_minutes == 30);
	CHECK(ParseSettings(Parse(R"({"EnablePlayerHideCommands": true, "HideSkinsCommand": "/nocosm", "StripSkinsCommand": "/clean_up"})")).hide_skins_command == "/nocosm");
	CHECK(ParseSettings(Parse(R"({"EnablePlayerStrip": true})")).enable_player_strip);
	CHECK(ParseSettings(Parse(R"({"PlayerStripCooldown_Minutes": 0})")).player_strip_cooldown_minutes == 0);
	CHECK(Throws([] { ParseSettings(Parse(R"({"PlayerStripCooldown_Minutes": 2000})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"HideSkinsCommand": "hideskins"})")); }));      // no slash
	CHECK(Throws([] { ParseSettings(Parse(R"({"ShowSkinsCommand": "/show skins"})")); }));    // space
	CHECK(Throws([] { ParseSettings(Parse(R"({"StripSkinsCommand": "/"})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"StripSkinsCommand": 5})")); }));

	// opt-in list
	OptInList list;
	CHECK(list.Empty() && !list.Contains("0002841F112C4CEE8F3C828460BA88FF"));
	CHECK(list.Add("0002841F112C4CEE8F3C828460BA88FF"));
	CHECK(!list.Add("0002841f112c4cee8f3c828460ba88ff"));      // same ID, other case
	CHECK(list.Contains("0002841f112c4cee8f3c828460ba88ff") && list.Size() == 1);
	CHECK(!list.Add("") && !list.Add("bad id") && !list.Add("../x"));
	CHECK(!list.Remove("00000000000000000000000000000001"));
	const OptInList again = OptInList::FromJson(list.ToJson());
	CHECK(again.Size() == 1 && again.Contains("0002841F112C4CEE8F3C828460BA88FF"));
	CHECK(OptInList::FromJson(Parse(R"({"HiddenSkinsPlayers": ["abc123", 5, "", "abc123"]})")).Size() == 1);
	CHECK(Throws([] { OptInList::FromJson(Parse("[]")); }));
	CHECK(Throws([] { OptInList::FromJson(Parse(R"({"HiddenSkinsPlayers": 3})")); }));
	CHECK(list.Remove("0002841F112C4CEE8F3C828460BA88FF") && list.Empty());

	// who counts as a console
	Settings s;
	OptInList opted;
	opted.Add("aaaa1111");
	CHECK(!IsTreatedAsConsole(s, opted, "aaaa1111"));           // commands not enabled: the file is ignored
	s.enable_player_hide_commands = true;
	CHECK(IsTreatedAsConsole(s, opted, "AAAA1111"));
	CHECK(!IsTreatedAsConsole(s, opted, "bbbb2222"));
	s.treat_as_console_players.push_back("bbbb2222");
	CHECK(IsTreatedAsConsole(s, opted, "bbbb2222"));            // the test list still works on its own
	s.block_item_skins_for_consoles = false;
	s.block_structure_skins_for_consoles = false;
	CHECK(!ConsoleBlockingOn(s));
	s.block_item_skins_for_consoles = true;
	CHECK(ConsoleBlockingOn(s));
	s.block_item_skins_for_consoles = false;
	s.block_structure_skins_for_consoles = true;
	CHECK(ConsoleBlockingOn(s));

	// player strip scope: only the caller's own team, and only a player or tribe team
	CHECK(InPlayerStripScope(123456, 123456));
	CHECK(!InPlayerStripScope(123457, 123456));
	CHECK(!InPlayerStripScope(0, 0));                           // no team: never in scope
	CHECK(!InPlayerStripScope(49999, 49999));                   // below the first player team (NPC / wild)
	CHECK(InPlayerStripScope(50000, 50000));

	// cooldown and report validity
	CHECK(CooldownMinutesLeft(1000, 0, 30) == 0);               // never stripped
	CHECK(CooldownMinutesLeft(1000, 900, 0) == 0);              // cooldown off
	CHECK(CooldownMinutesLeft(1000 + 60, 1000, 30) == 29);      // 29 min 0 s left
	CHECK(CooldownMinutesLeft(1000 + 30 * 60, 1000, 30) == 0);
	CHECK(CooldownMinutesLeft(1000 + 29 * 60 + 1, 1000, 30) == 1);   // 59 s left rounds up
	CHECK(PlayerReportStillValid(1500, 1000) && PlayerReportStillValid(1600, 1000));
	CHECK(!PlayerReportStillValid(1601, 1000) && !PlayerReportStillValid(1000, 0));
	CHECK(TribeBackupName(123456, 1700000000) == "strip-tribe123456-1700000000.jsonl");
	CHECK(IsSafeBackupName(TribeBackupName(123456, 1700000000)));
	CHECK(ParseTribeId("1234567890") == 1234567890 && ParseTribeId("50000") == 50000);
	CHECK(ParseTribeId("") == 0 && ParseTribeId("49999") == 0 && ParseTribeId("12a45") == 0 && ParseTribeId("-50000") == 0);
	CHECK(ParseTribeId("2147483648") == 0 && ParseTribeId("99999999999") == 0);

	// preference storage
	CHECK(!d.preferences_in_database && d.database_refresh_seconds == 30);
	CHECK(d.db.port == 3306 && d.db.name == "cosmeticsdisabler" && d.db.table == "hidden_skins_players" && !d.db.ssl);
	{
		const Settings db = ParseSettings(Parse(R"({"PlayerPreferenceStorage": "Database", "DatabaseRefresh_Seconds": 60,
			"Database": {"Host": "10.0.0.5", "Port": 3307, "User": "ark", "Password": "x", "Name": "cluster_prefs", "Table": "hide_v2", "Ssl": true}})"));
		CHECK(db.preferences_in_database && db.database_refresh_seconds == 60);
		CHECK(db.db.host == "10.0.0.5" && db.db.port == 3307 && db.db.user == "ark" && db.db.name == "cluster_prefs" && db.db.table == "hide_v2" && db.db.ssl);
	}
	CHECK(Throws([] { ParseSettings(Parse(R"({"PlayerPreferenceStorage": "Mongo"})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"PlayerPreferenceStorage": "Database"})")); }));               // no user
	CHECK(Throws([] { ParseSettings(Parse(R"({"Database": {"Table": "x; DROP TABLE y"}})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"Database": {"Name": "a`b"}})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"Database": {"Port": 70000}})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"Database": 5})")); }));
	CHECK(Throws([] { ParseSettings(Parse(R"({"DatabaseRefresh_Seconds": 1})")); }));
	CHECK(IsValidSqlName("hidden_skins_players") && !IsValidSqlName("") && !IsValidSqlName("a-b") && !IsValidSqlName(std::string(65, 'a')));
	CHECK(SqlSelectPreferences("t1") == "SELECT `eos_id` FROM `t1`");
	CHECK(SqlCreatePreferenceTable("t1").find("CREATE TABLE IF NOT EXISTS `t1`") == 0);
	CHECK(SqlSetPreference("t1", "AAAA1111", true, "TheIsland").find("VALUES ('aaaa1111', 'TheIsland')") != std::string::npos);
	CHECK(SqlSetPreference("t1", "AAAA1111", false, "") == "DELETE FROM `t1` WHERE `eos_id` = 'aaaa1111'");
	CHECK(SqlSetPreference("t1", "aaaa1111", true, "Lost'Colony;--").find("'LostColony--'") != std::string::npos);
	CHECK(Throws([] { SqlSetPreference("t1", "aa' OR 1=1", true, ""); }));
	CHECK(Throws([] { SqlSelectPreferences("t 1"); }));
}

static void TestFallback()
{
	const Settings f = SafeFallbackSettings();
	CHECK(f.block_structure_skins_for_consoles && f.block_item_skins_for_consoles && f.read_connection_platform);
	CHECK(!f.enable_strip && !f.strip_on_map_load && !f.strip_items_on_load && !f.strip_items_on_entry);
	CHECK(!f.enable_player_hide_commands && !f.enable_player_strip && !f.preferences_in_database);
	CHECK(!f.probe_structure_channels && !f.probe_item_requests && !f.probe_client_rpcs && f.defer_structure_skins_seconds == 0);
	CHECK(f.unequip_skin_items_for_consoles && f.treat_as_console_players.empty());
	CHECK(!f.probe_items && !f.probe_players && !f.probe_structure_messages && !f.probe_item_entry && !f.probe_unlocked_cosmetics);
}

static void CheckShipped(const Settings& s)
{
	CHECK(!s.enable_player_hide_commands && !s.enable_player_strip);   // player commands ship switched off
	CHECK(!s.preferences_in_database && s.db.user.empty() && s.db.password.empty());   // no credentials in the shipped config
	CHECK(s.player_strip_cooldown_minutes == 30);
	CHECK(!s.probe_items);                  // probes ship off
	CHECK(!s.probe_players);
	CHECK(!s.probe_structure_messages);
	CHECK(!s.probe_structure_channels && !s.probe_item_entry && !s.probe_item_requests && !s.probe_client_rpcs);
	CHECK(s.block_structure_skins_for_consoles);   // console blocking ships on: it is what the plugin is for
	CHECK(s.block_item_skins_for_consoles);
	CHECK(s.unequip_skin_items_for_consoles);
	CHECK(s.treat_as_console_players.empty());
	CHECK(s.read_connection_platform);
	CHECK(s.summary_interval_seconds == 30);
	CHECK(s.max_detail_lines_per_summary == 50);
	CHECK(s.scan_actors_per_tick == 20000);
	CHECK(!s.enable_strip);                 // must ship switched off
	CHECK(s.strip_mode == "All");
	CHECK(s.allow_skin_classes.empty());
	CHECK(s.strip_per_tick == 50);
	CHECK(!s.filter_items);                 // every new switch ships off
	CHECK(!s.filter_item_custom_data);
	CHECK(!s.block_skin_apply);
	CHECK(!s.strip_on_map_load);
	CHECK(s.strip_on_map_load_delay_seconds == 120);
	CHECK(s.strip_repeat_minutes == 0);
	CHECK(s.block_skin_apply_message.empty());   // silent unless the operator sets a message
}

int main(int argc, char** argv)
{
	TestSettings();
	TestClassify();
	TestStripPolicy();
	TestItemFilterAndGuard();
	TestBackup();
	TestBudget();
	TestKeyCounter();
	TestPlatform();
	TestFormatting();
	TestPlayerCommands();
	TestFallback();

	if (argc >= 2)
	{
		std::ifstream file(argv[1]);
		if (!file.is_open())
		{
			std::cerr << "cannot open " << argv[1] << "\n";
			return 2;
		}
		try
		{
			const Settings s = ParseSettings(nlohmann::json::parse(file, nullptr, true, true));
			++g_checks;
			if (argc >= 3 && std::string(argv[2]) == "--shipped")
				CheckShipped(s);
		}
		catch (const std::exception& e)
		{
			++g_failures;
			std::cerr << argv[1] << ": " << e.what() << "\n";
		}
	}

	std::cout << g_checks << " checks, " << g_failures << " failed\n";
	return g_failures == 0 ? 0 : 1;
}
