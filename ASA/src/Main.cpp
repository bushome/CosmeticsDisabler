// CosmeticsDisabler 0.6.0 - probe logging, a backed-up structure skin strip, item skin strips, and ways to keep skins away.
//
// Observation (reads and logs only):
//   * UPrimalItem.GetItemNetInfo(bool)           item data built for a client
//   * AShooterCharacter.PreReplication           hair / eyebrow / facial hair mod cosmetic ids + owner platform
//   * APrimalStructure.MultiHandleSkinNetMessage structure skin change multicast
//   * UPrimalInventoryComponent.AddItem / NotifyItemAdded, UPrimalItem.InitFromNetInfo / AddNewItem  item entry
//
// Commands (console + RCON; everything runs on the game thread, a slice per one-second timer tick):
//   CosmeticsDisabler.Scan                 counts structures with an official or custom skin
//   CosmeticsDisabler.Strip                DRY RUN: reports what would be stripped, changes nothing
//   CosmeticsDisabler.Strip confirm        strips (needs EnableStrip=true and a finished dry run this session);
//                                          every change is written to backups/strip-<time>.jsonl first
//   CosmeticsDisabler.Restore [file]       puts the skins from a backup file back (lists backups without a file)
//   CosmeticsDisabler.Status / .Reload
//
// Player chat commands (off unless enabled in config.json; names are configurable):
//   /hideskins, /showskins   a player opts in or out of being handled like a console player (needs a relog); kept in
//                            hidden_skins_players.json; only does something when BlockItemSkinsForConsoles or
//                            BlockStructureSkinsForConsoles is on
//   CosmeticsDisabler.StripTribe <tribe id> [confirm]   admin: report, then strip, one tribe's structure skins (needs
//                            EnableStrip for confirm; no cooldown; backup strip-tribe<id>-<time>.jsonl)
//   /stripskins [confirm]    report, then strip, the skins on the caller's own tribe's structures (backup per tribe,
//                            cooldown per tribe; undo with CosmeticsDisabler.Restore strip-tribe<id>-<time>.jsonl)
//
// Optional (see config.json):
//   FilterItems / FilterItemCustomData  blank item skins in the data sent to clients (server items untouched);
//                                       fixes icons and armor, not a weapon model (the skin swaps the weapon class)
//   BlockSkinApply                      refuse new structure skins (SetStructureSkin) and item skins (UseItemOntoItem)
//   StripOnMapLoad / StripRepeat_Minutes  run the structure strip automatically
//   StripItemsOnEntry                   blank the skin before an item is built from net data (transmitter, vault,
//                                       obelisk, drop); weapon skins swap the weapon class, so it must be early
//   StripSavedItems                     remove skins from items loaded from the save, only when the owner's team
//                                       is a player or tribe (>= 50000), so NPC gear (Thralls, pirates) is left alone
//   BackupStrippedItems                 every item strip is written to backups/items-<time>.jsonl first
//
// Stripping removes the skin with SetStructureSkin(null, 0, 0), the same call another operator ran on
// 1,766 structures at 50 per second. The structure itself is untouched.

#include "API/ARK/Ark.h"

#include <chrono>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <string>
#include <cctype>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ProbeLogic.h"
#include "Database.h"   // MariaDB Connector/C wrapper + worker thread (used only with PlayerPreferenceStorage "Database")

#include <memory>

// Installs the structure channel probe hook when ProbeStructureChannels is on (defined after the hook).
void EnsureChannelHook();

#ifndef PROJECT_NAME
#define PROJECT_NAME "CosmeticsDisabler"
#endif

namespace
{
	using namespace CosmeticsProbe;

	constexpr const char* kReloadCommand = PROJECT_NAME ".Reload";
	constexpr const char* kScanCommand = PROJECT_NAME ".Scan";
	constexpr const char* kStripCommand = PROJECT_NAME ".Strip";
	constexpr const char* kRestoreCommand = PROJECT_NAME ".Restore";
	constexpr const char* kStatusCommand = PROJECT_NAME ".Status";
	constexpr const char* kStripTribeCommand = PROJECT_NAME ".StripTribe";
	constexpr const char* kTimerId = PROJECT_NAME "_Timer";

	const char* const kHookItemNetInfo = "UPrimalItem.GetItemNetInfo(bool)";
	const char* const kHookCharacterPreRep = "AShooterCharacter.PreReplication(IRepChangedPropertyTracker&)";
	const char* const kHookSkinMessage = "APrimalStructure.MultiHandleSkinNetMessage(FName,FSkinNetMessageParams&)";
	// Verified in the offsets cache (AsaApiCache98.9). Not declared in the API headers, so hooked by hand.
	const char* const kHookSkinRefresh = "APrimalStructure.MultiRefreshStructureSkin(TSubclassOf<APrimalStructure>,__int64,int,bool,bool)";
	// Item entry probe. Strings copied from the offsets cache (checked on the test server, 2026-11-23).
	const char* const kHookAddItem = "UPrimalInventoryComponent.AddItem(FItemNetInfo&,bool,bool,bool,FItemNetID*,bool,bool,bool,AShooterCharacter*,bool,bool,bool,bool,bool,bool)";
	const char* const kHookNotifyItemAdded = "UPrimalInventoryComponent.NotifyItemAdded(UPrimalItem*,bool,bool)";
	const char* const kHookInitFromNetInfo = "UPrimalItem.InitFromNetInfo(FItemNetInfo&,UWorld*)";
	const char* const kHookUseItemOntoItem = "UPrimalItem.UseItemOntoItem(UPrimalItem*,int,AShooterPlayerController*)";
	const char* const kHookLoadedFromSave = "UPrimalItem.InventoryLoadedFromSaveGame()";
	const char* const kHookAddNewItem = "UPrimalItem.AddNewItem(TSubclassOf<UPrimalItem>,UPrimalInventoryComponent*,bool,bool,float,bool,int,bool,float,bool,TSubclassOf<UPrimalItem>,float,bool,bool,bool,bool,bool,bool,UWorld*)";
	const char* const kLevelActorsField = "ULevel.Actors2";
	constexpr const char* kPluginVersion = "0.6.0";
	const char* const kReplicatedSkinClassField = "APrimalStructure.ReplicatedStructureMySkinClass";
	const char* const kLocalSkinClassField = "APrimalStructure.StructureSkinClass";
	// Must match the offsets cache exactly: a name that is not in it makes AsaApi terminate the server (it
	// cannot be caught). The cache spells 64-bit integers "__int64"; "int64" crashed a test server.
	const char* const kSetSkinSymbol = "APrimalStructure.SetStructureSkin(TSubclassOf<APrimalStructure>,__int64,int)";

	Settings g_settings;
	bool g_alive = false;
	long long g_clock = 0;
	Counters g_counters;
	DetailBudget g_budget;
	long long g_window_start = 0;

	// Item request probe: items built while a ServerRequestItems call runs (game thread only).
	struct ItemRequest
	{
		bool active = false;
		bool equipped = false;   // request is for worn gear
		long long items = 0;
		long long skinned = 0;
		long long blanked = 0;
		bool console = true;     // viewer is not a known PC connection (unknown counts as console)
	};
	ItemRequest g_request;

	// Item entry probe counters: how often each function ran, and how often the item carried a skin.
	struct EntryCounters
	{
		long long add_item = 0, add_item_skin = 0;
		long long notify = 0, notify_skin = 0;
		long long init = 0, init_skin = 0, init_stripped = 0;
		long long add_new = 0, add_new_skin = 0;
		long long loaded_skin = 0, loaded_stripped = 0, loaded_mod = 0, loaded_mod_stripped = 0;
	};
	EntryCounters g_entry;

	// Structure channel probe (ProbeStructureChannels): UActorChannel::ReplicateActor runs once per connection and
	// actor whenever that actor is replicated to that connection. Counted, never changed.
	struct ChannelCounters
	{
		long long calls = 0, structures = 0, blanked = 0;
		long long skinned = 0, skinned_pc = 0, skinned_console = 0, skinned_unknown = 0;
		long long ns_all = 0, ns_skinned = 0;
		std::unordered_map<std::string, long long> platforms;   // skinned structure channels per platform label
	};
	ChannelCounters g_channels;
	std::unordered_set<unsigned long long> g_channel_pairs;     // (connection, structure) pairs seen this window
	std::atomic<long long> g_channels_off_thread{0};            // calls not on the game thread (left alone)
	std::thread::id g_game_thread;

	// Per-connection platform, read once per summary window (connections can be reused after a disconnect).
	struct ConnectionInfo
	{
		int platform = -1;
		bool test_console = false;   // listed in TreatAsConsolePlayers, or opted in with /hideskins
		bool opted_in = false;       // the reason is /hideskins (for the log label)
	};
	std::unordered_map<UNetConnection*, ConnectionInfo> g_connection_info;

	struct CharacterSnapshot
	{
		bool looked = false;
		__int64 head = 0, eyebrow = 0, facial = 0;
	};
	std::unordered_map<uintptr_t, CharacterSnapshot> g_seen_characters;

	// ---- Run state (one structure run at a time) ----
	enum class Run { Idle, Scan, StripDry, StripApply, Restore };
	Run g_run = Run::Idle;
	size_t g_scan_level = 0;
	int g_scan_actor = 0;
	ScanResult g_scan_result;
	bool g_dry_run_done = false;          // a dry run finished this session (required before "confirm")
	std::ofstream g_item_backup;          // items-<time>.jsonl, opened on the first item skin removal of this run
	std::string g_item_backup_name;
	bool g_item_backup_dirty = false;     // lines written since the last flush
	bool g_item_backup_failed = false;    // could not open or write: item skins are not removed (logged once)
	long long g_item_backup_lines = 0;
	std::ofstream g_backup;               // open while StripApply runs
	std::string g_backup_name;
	bool g_backup_used = false;           // at least one line written to the open backup file
	bool g_internal_skin_call = false;    // the plugin itself is calling SetStructureSkin (the guard lets it through)
	std::unordered_map<uintptr_t, long long> g_last_apply_notice;   // player controller -> clock + 1 of the last notice
	long long g_next_auto_strip = -1;     // clock value of the next automatic strip (-1 = not scheduled yet)

	// ---- Player commands ----
	OptInList g_optin;                                  // /hideskins players (hidden_skins_players.json)
	int g_scope_team = 0;                               // 0 = whole map; else a player strip limited to this team
	std::string g_scope_requester;                      // EOS ID of the player who started the scoped run
	std::unordered_map<int, long long> g_player_report_time;   // team -> epoch seconds of its last report
	std::unordered_map<int, long long> g_player_strip_time;    // team -> epoch seconds of its last confirmed strip
	long long g_prune_clock = 0;
	bool g_scope_admin = false;                         // the scoped run was started by StripTribe (admin), not a player
	std::unordered_map<int, long long> g_admin_report_time;    // team -> epoch seconds of its last StripTribe report
	std::string g_command_caller;                       // EOS ID of the admin running a console command ("" = RCON)
	// PlayerPreferenceStorage "Database": the table is read on a worker thread; results come back through g_db_results
	// and are applied on the game thread by the timer. g_optin stays the in-memory copy that InfoFor reads.
	std::unique_ptr<CosmeticsDb::DbWorker> g_db;
	PreferenceDb g_db_config;                           // config the running worker was started with
	CosmeticsDb::GameQueue g_db_results;
	long long g_optin_write_seq = 0;                    // bumped on every local change; older table reads are dropped
	long long g_next_db_refresh = 0;
	bool g_db_loaded = false;                           // the table has been read at least once
	bool g_db_failing = false;                          // last database job failed (logged once until it recovers)
	Settings g_chat_registered;                         // the chat command names/flags registered at load (a reload does not re-register)

	std::vector<BackupEntry> g_restore_entries;
	size_t g_restore_next = 0;
	long long g_restore_done = 0, g_restore_missing = 0, g_restore_conflict = 0, g_restore_skipped = 0, g_restore_failed = 0;

	// ------------------------------------------------------------------------------------------
	// Helpers
	// ------------------------------------------------------------------------------------------

	std::string ClassName(UClass* cls)
	{
		if (cls == nullptr)
			return "(null)";
		return cls->NameField().ToString().ToString();
	}

	// FItemNetInfo carries the mod cosmetic id, but AsaApi's header does not list it; the keys are in the offsets cache.
	long long& NetModSkinId(FItemNetInfo* info)
	{
		return *GetNativePointerField<long long*>(info, "FItemNetInfo.CustomCosmeticModSkinReplacementID");
	}

	int& NetModSkinVariant(FItemNetInfo* info)
	{
		return *GetNativePointerField<int*>(info, "FItemNetInfo.CustomCosmeticModSkinVariantID");
	}

	std::string ClassNameOfObject(void* object)
	{
		if (object == nullptr)
			return "(null)";
		return ClassName(reinterpret_cast<UObject*>(object)->ClassField());
	}

	// Actor whose inventory holds the item (player body, tame, NPC, storage), or null if not known yet.
	AActor* OwnerActor(UPrimalItem* item)
	{
		if (item == nullptr)
			return nullptr;
		UPrimalInventoryComponent* inventory = item->OwnerInventoryField().Get();
		return inventory != nullptr ? inventory->OwnerPrivateField() : nullptr;
	}

	// Weapon actor class an item will spawn (UPrimalItem.WeaponTemplate). Read from the soft pointer's plain
	// members only (no native calls); "" when unset. Used to see whether an item skin swaps the weapon class.
	std::string WeaponTemplateName(UPrimalItem* item)
	{
		if (item == nullptr)
			return "";
		const FSoftObjectPath& path = item->WeaponTemplateField().ToSoftObjectPath();
		return path.AssetPath.AssetName.ToString().ToString();
	}

	std::string ClassPath(UClass* cls)
	{
		if (cls == nullptr)
			return "";
		FString path;
		cls->GetPathName(nullptr, path);
		return path.ToString();
	}

	int ReadConnectionPlatform(AShooterCharacter* character)
	{
		UNetConnection* connection = character->GetNetConnection();
		if (connection == nullptr)
			return -1;
		return static_cast<int>(*GetNativePointerField<uint8_t*>(connection, "UNetConnection.ConnectionPlatform"));
	}

	// The skin classes are not in the API headers, so they are read by name from the offsets cache.
	UClass*& ReplicatedSkinClass(APrimalStructure* s) { return *GetNativePointerField<UClass**>(s, kReplicatedSkinClassField); }
	UClass*& LocalSkinClass(APrimalStructure* s) { return *GetNativePointerField<UClass**>(s, kLocalSkinClassField); }

	struct SkinState
	{
		UClass* cls = nullptr;
		__int64 id = 0;
		int variant = 0;
		std::string class_name = "(null)";
		SkinKind kind = SkinKind::None;
	};

	SkinState ReadSkin(APrimalStructure* s)
	{
		SkinState st;
		st.cls = ReplicatedSkinClass(s);
		if (st.cls == nullptr)
			st.cls = LocalSkinClass(s);
		st.id = s->MyCustomCosmeticStructureSkinIDField();
		st.variant = s->MyCustomCosmeticStructureSkinVariantIDField();
		st.class_name = ClassName(st.cls);
		st.kind = ClassifySkin(st.class_name, st.id);
		return st;
	}

	bool CallSetSkin(APrimalStructure* s, UClass* cls, __int64 id, int variant)
	{
		TSubclassOf<APrimalStructure> skin;
		skin.uClass = cls;
		g_internal_skin_call = true;
		const bool result = NativeCall<bool, TSubclassOf<APrimalStructure>&, __int64, int>(s, kSetSkinSymbol, skin, id, variant);
		g_internal_skin_call = false;
		return result;
	}

	std::string BackupDir()
	{
		return AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" PROJECT_NAME "/backups";
	}

	long long NowSeconds()
	{
		return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	}

	std::string OptInPath()
	{
		return AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" PROJECT_NAME "/hidden_skins_players.json";
	}

	// A missing file is an empty list; a damaged one is logged and treated as empty (nothing is overwritten until a player
	// changes the list, and the damaged file is kept as .bad first).
	void LoadOptIn()
	{
		g_optin.Clear();
		std::ifstream file(OptInPath());
		if (!file.is_open())
			return;
		try
		{
			g_optin = OptInList::FromJson(nlohmann::json::parse(file, nullptr, true, true));
			Log::GetLog()->info("Hidden-skins players loaded: {}", g_optin.Size());
		}
		catch (const std::exception& e)
		{
			Log::GetLog()->error("{} is not readable ({}); starting with an empty list", OptInPath(), e.what());
			file.close();
			std::error_code ec;
			std::filesystem::rename(OptInPath(), OptInPath() + ".bad", ec);
		}
	}

	bool SaveOptIn()
	{
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(OptInPath()).parent_path(), ec);
		const std::string temp = OptInPath() + ".tmp";
		{
			std::ofstream out(temp, std::ios::out | std::ios::trunc);
			if (!out.is_open())
				return false;
			out << g_optin.ToJson().dump(2) << '\n';
			out.flush();
			if (!out.good())
				return false;
		}
		std::filesystem::rename(temp, OptInPath(), ec);
		return !ec;
	}

	std::string MapLabel()
	{
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (world == nullptr)
			return {};
		FString name;
		world->GetMapName(&name);
		std::string map = name.ToString();
		if (map.size() > 3 && map.compare(map.size() - 3, 3, "_WP") == 0)
			map.resize(map.size() - 3);
		return map;
	}

	bool PreferencesInDatabase()
	{
		return g_settings.enable_player_hide_commands && g_settings.preferences_in_database;
	}

	// Called from worker jobs (worker thread): reports a database failure or recovery once, on the game thread.
	void PostDbOutcome(bool ok, const std::string& what, const std::string& error)
	{
		g_db_results.Post([ok, what, error]
		{
			if (!ok && !g_db_failing)
				Log::GetLog()->error("Player preferences database: {} failed: {} (retrying every {} s)", what, error, g_settings.database_refresh_seconds);
			else if (ok && g_db_failing)
				Log::GetLog()->info("Player preferences database: working again");
			g_db_failing = !ok;
		});
	}

	// Reads the whole table (only opted-in players are in it, so it stays small) and replaces the in-memory list,
	// unless a player changed their choice here after the read was queued (then the next read picks it up).
	void RefreshPreferences()
	{
		if (!g_db)
			return;
		const long long seq = g_optin_write_seq;
		const std::string sql = SqlSelectPreferences(g_db_config.table);
		g_db->Post([seq, sql](CosmeticsDb::Db& db)
		{
			auto rows = db.Query(sql);
			if (!rows)
			{
				PostDbOutcome(false, "reading the table", db.LastError());
				return;
			}
			std::vector<std::string> ids;
			for (const auto& row : *rows)
				if (!row.empty() && row[0])
					ids.push_back(*row[0]);
			PostDbOutcome(true, "", "");
			g_db_results.Post([seq, ids = std::move(ids)]
			{
				if (!g_db || seq != g_optin_write_seq)
					return;
				OptInList fresh;
				for (const std::string& id : ids)
					fresh.Add(id);
				if (!g_db_loaded || fresh.Size() != g_optin.Size())
					Log::GetLog()->info("Hidden-skins players (database): {}", fresh.Size());
				g_optin = std::move(fresh);
				g_db_loaded = true;
			});
		});
	}

	void StopPreferenceDb()
	{
		if (g_db)
		{
			g_db->Stop();   // finishes queued jobs; each is bounded by the 5 s connect/read/write timeouts
			g_db.reset();
		}
		g_db_results.Clear();
		g_db_loaded = false;
		g_db_failing = false;
	}

	// Starts (or keeps) the worker for the configured database; creates the table if it does not exist.
	void StartPreferenceDb()
	{
		if (g_db && g_db_config == g_settings.db)
			return;
		StopPreferenceDb();
		g_db_config = g_settings.db;
		CosmeticsDb::DbConfig cfg;
		cfg.host = g_db_config.host;
		cfg.port = static_cast<unsigned int>(g_db_config.port);
		cfg.user = g_db_config.user;
		cfg.password = g_db_config.password;
		cfg.database = g_db_config.name;
		cfg.ssl = g_db_config.ssl;
		g_db = std::make_unique<CosmeticsDb::DbWorker>(cfg);
		g_db->Start();
		const std::string create = SqlCreatePreferenceTable(g_db_config.table);
		g_db->Post([create](CosmeticsDb::Db& db)
		{
			if (db.Exec(create) < 0)
				PostDbOutcome(false, "creating the table", db.LastError());
		});
		g_optin.Clear();
		RefreshPreferences();
		g_next_db_refresh = g_clock + g_settings.database_refresh_seconds;
		Log::GetLog()->info("Player preferences: database {}:{}/{} table {}", g_db_config.host, g_db_config.port, g_db_config.name, g_db_config.table);
	}

	// At load and on Reload: the file, or the database.
	void LoadPreferences()
	{
		if (PreferencesInDatabase())
		{
			StartPreferenceDb();
			return;
		}
		StopPreferenceDb();
		LoadOptIn();
	}

	void DbTick()
	{
		g_db_results.Drain();
		if (g_db && g_clock >= g_next_db_refresh)
		{
			g_next_db_refresh = g_clock + g_settings.database_refresh_seconds;
			RefreshPreferences();
		}
	}

	Settings LoadSettings()
	{
		const std::string path = AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" PROJECT_NAME "/config.json";
		std::ifstream file(path);
		if (!file.is_open())
			throw std::runtime_error("cannot open " + path);

		nlohmann::json root;
		try
		{
			root = nlohmann::json::parse(file, nullptr, true, true);
		}
		catch (const nlohmann::json::exception& error)
		{
			throw std::runtime_error(std::string("config.json is not valid JSON: ") + error.what());
		}
		return ParseSettings(root);
	}

	bool ReloadSettings(std::string& error)
	{
		try
		{
			g_settings = LoadSettings();
			g_budget.Reset(g_settings.max_detail_lines_per_summary);
			g_item_backup_failed = false;   // a reload tries the item backup file again
			g_connection_info.clear();      // TreatAsConsolePlayers may have changed
			return true;
		}
		catch (const std::exception& e)
		{
			error = e.what();
			return false;
		}
	}

	void WriteSummary()
	{
		const int window = static_cast<int>(g_clock - g_window_start);
		Log::GetLog()->info("{}", FormatSummary(g_counters, window));
		if (g_settings.probe_item_entry || g_settings.strip_items_on_entry || g_settings.strip_items_on_load)
		{
			Log::GetLog()->info("  items loaded from save with a skin: {} (skin removed {}), with a mod cosmetic id: {} (cleared {})", g_entry.loaded_skin,
				g_entry.loaded_stripped, g_entry.loaded_mod, g_entry.loaded_mod_stripped);
			Log::GetLog()->info(
				"  item entry (skinned/total): AddItem {}/{}, NotifyItemAdded {}/{}, InitFromNetInfo {}/{} (skin removed {}), AddNewItem {}/{}",
				g_entry.add_item_skin, g_entry.add_item, g_entry.notify_skin, g_entry.notify, g_entry.init_skin, g_entry.init,
				g_entry.init_stripped, g_entry.add_new_skin, g_entry.add_new);
			g_entry = EntryCounters{};
		}
		if (g_settings.probe_structure_channels)
		{
			std::string by_platform;
			for (const auto& [label, count] : g_channels.platforms)
				by_platform += (by_platform.empty() ? "" : ", ") + label + " x" + std::to_string(count);
			Log::GetLog()->info(
				"  structure channels: ReplicateActor calls {} ({} off the game thread), structures {}, skinned {} (PC {}, console {}, unknown {}), "
				"skinned connection/structure pairs {}, blanked for console {}, time in ReplicateActor: all {} us, skinned {} us",
				g_channels.calls, g_channels_off_thread.exchange(0), g_channels.structures, g_channels.skinned, g_channels.skinned_pc,
				g_channels.skinned_console, g_channels.skinned_unknown, g_channel_pairs.size(), g_channels.blanked, g_channels.ns_all / 1000, g_channels.ns_skinned / 1000);
			if (!by_platform.empty())
				Log::GetLog()->info("  skinned structure channels by connection platform: {}", by_platform);
			g_channels = ChannelCounters{};
			g_channel_pairs.clear();
			g_connection_info.clear();
		}
		g_counters.Clear();
		g_seen_characters.clear();
		g_last_apply_notice.clear();
		g_budget.Reset(g_settings.max_detail_lines_per_summary);
		g_window_start = g_clock;
	}

	// ------------------------------------------------------------------------------------------
	// Scan / Strip (structures in the loaded world, a slice per timer tick)
	// ------------------------------------------------------------------------------------------

	bool OpenBackupFile()
	{
		std::error_code ec;
		std::filesystem::create_directories(BackupDir(), ec);
		g_backup_name = g_scope_team != 0 ? TribeBackupName(g_scope_team, NowSeconds())
			: "strip-" + std::to_string(NowSeconds()) + ".jsonl";
		g_backup_used = false;
		g_backup.open(BackupDir() + "/" + g_backup_name, std::ios::out | std::ios::app);
		return g_backup.is_open();
	}

	// Writes the record of an item skin about to be removed. Returns false when it could not be written; the
	// caller then leaves the skin alone. Flushed by the timer, so a crash loses at most the last second.
	bool WriteItemBackup(const ItemBackupEntry& entry)
	{
		if (!g_settings.backup_stripped_items)
			return true;
		if (g_item_backup_failed)
			return false;
		if (!g_item_backup.is_open())
		{
			std::error_code ec;
			std::filesystem::create_directories(BackupDir(), ec);
			g_item_backup_name = "items-" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count()) + ".jsonl";
			g_item_backup.open(BackupDir() + "/" + g_item_backup_name, std::ios::out | std::ios::app);
			if (!g_item_backup.is_open())
			{
				g_item_backup_failed = true;
				Log::GetLog()->error("Item backup: cannot create {}/{}; item skins are no longer removed until the plugin is reloaded", BackupDir(),
					g_item_backup_name);
				return false;
			}
			Log::GetLog()->info("Item backup: {}/{}", BackupDir(), g_item_backup_name);
		}
		g_item_backup << ItemBackupLine(entry) << '\n';
		if (!g_item_backup.good())
		{
			g_item_backup_failed = true;
			Log::GetLog()->error("Item backup: write failed; item skins are no longer removed until the plugin is reloaded");
			return false;
		}
		g_item_backup_dirty = true;
		++g_item_backup_lines;
		return true;
	}

	void FlushItemBackup()
	{
		if (g_item_backup.is_open() && g_item_backup_dirty)
		{
			g_item_backup.flush();
			g_item_backup_dirty = false;
		}
	}

	void CloseItemBackup()
	{
		if (g_item_backup.is_open())
			g_item_backup.close();
		g_item_backup_dirty = false;
	}

	// Closes the backup file; an empty one (nothing was stripped) is removed again.
	void CloseBackupFile()
	{
		if (!g_backup.is_open())
			return;
		g_backup.close();
		if (!g_backup_used)
		{
			std::error_code ec;
			std::filesystem::remove(BackupDir() + "/" + g_backup_name, ec);
		}
	}

	// Tells a player (found by EOS ID) something; false when they are no longer online.
	bool NotifyPlayer(const std::string& eos_id, const std::string& text)
	{
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (world == nullptr || eos_id.empty())
			return false;
		for (auto& weak : world->PlayerControllerListField())
		{
			APlayerController* controller = weak.Get();
			if (controller == nullptr || !controller->IsA(AShooterPlayerController::GetPrivateStaticClass()))
				continue;
			auto* shooter = static_cast<AShooterPlayerController*>(controller);
			if (ToLower(AsaApi::GetApiUtils().GetEOSIDFromController(shooter).ToString()) != ToLower(eos_id))
				continue;
			AsaApi::GetApiUtils().SendServerMessage(shooter, FColorList::Green, "{}", text);
			return true;
		}
		return false;
	}

	// End of a player strip run (finished or stopped): tell the player and update the per-tribe state.
	void FinishScopedRun(bool aborted, const std::string& why)
	{
		const int team = g_scope_team;
		const std::string requester = g_scope_requester;
		const bool apply = g_run == Run::StripApply;
		const std::string backup_name = g_backup_name;
		const bool backup_used = g_backup_used;
		std::string message;
		if (g_scope_admin)
		{
			// StripTribe: no player cooldown; the reply goes to the admin who ran it (console) and the log.
			if (aborted)
				message = "CosmeticsDisabler: tribe " + std::to_string(team) + " strip stopped: " + why;
			else if (!apply)
			{
				if (g_scan_result.would_strip > 0)
					g_admin_report_time[team] = NowSeconds();
				message = "CosmeticsDisabler: tribe " + std::to_string(team) + " report: " + std::to_string(g_scan_result.would_strip)
					+ " structures with a skin (" + std::to_string(g_scan_result.custom) + " custom, " + std::to_string(g_scan_result.official)
					+ " official)" + (g_scan_result.would_strip > 0 ? ". Run " + std::string(kStripTribeCommand) + " " + std::to_string(team)
					+ " confirm within 10 minutes to remove them" : "");
			}
			else
			{
				g_admin_report_time.erase(team);
				message = "CosmeticsDisabler: tribe " + std::to_string(team) + ": skins removed from " + std::to_string(g_scan_result.stripped)
					+ " structures" + (g_scan_result.strip_failed > 0 ? " (" + std::to_string(g_scan_result.strip_failed) + " could not be changed)" : "")
					+ (backup_used ? ", backup " + backup_name + " (undo with " + kRestoreCommand + " " + backup_name + ")" : "");
			}
			Log::GetLog()->info("{}", message);
			NotifyPlayer(requester, message);
			g_scope_team = 0;
			g_scope_requester.clear();
			g_scope_admin = false;
			return;
		}
		if (aborted)
		{
			message = "Skin strip stopped: " + why + ". Nothing further was changed.";
		}
		else if (!apply)
		{
			if (g_scan_result.would_strip == 0)
			{
				message = "Your tribe's structures have no skins to remove.";
			}
			else
			{
				g_player_report_time[team] = NowSeconds();
				message = "Report: " + std::to_string(g_scan_result.would_strip) + " of your tribe's structures have a skin ("
					+ std::to_string(g_scan_result.custom) + " custom, " + std::to_string(g_scan_result.official) + " official). Type "
					+ g_settings.strip_skins_command + " confirm within 10 minutes to remove them. The structures stay as they are.";
			}
		}
		else
		{
			g_player_strip_time[team] = NowSeconds();
			g_player_report_time.erase(team);
			message = "Skins removed from " + std::to_string(g_scan_result.stripped) + " structures"
				+ (g_scan_result.strip_failed > 0 ? " (" + std::to_string(g_scan_result.strip_failed) + " could not be changed)" : "")
				+ ". Relog to see the change.";
		}
		Log::GetLog()->info("Player strip (team {}, {}): {}{}", team, apply ? "confirmed" : "report", message,
			backup_used ? " backup " + backup_name : "");
		NotifyPlayer(requester, message);
		g_scope_team = 0;
		g_scope_requester.clear();
		g_scope_admin = false;
	}

	void FinishRun(const char* what)
	{
		CloseBackupFile();
		Log::GetLog()->info("{} finished (scope: {})\n{}", what, g_scope_team == 0 ? std::string("whole map") : "team " + std::to_string(g_scope_team),
			FormatScan(g_scan_result));
		if (g_run == Run::StripDry && g_scope_team == 0)
			g_dry_run_done = true;
		if (g_run == Run::StripApply && g_backup_used)
			Log::GetLog()->info("Backup: {}/{}  (undo with {} {})", BackupDir(), g_backup_name, kRestoreCommand, g_backup_name);
		if (g_scope_team != 0)
			FinishScopedRun(false, "");
		g_run = Run::Idle;
	}

	void AbortRun(const std::string& why)
	{
		CloseBackupFile();
		Log::GetLog()->error("Run stopped: {}", why);
		if (g_scope_team != 0)
			FinishScopedRun(true, why);
		g_run = Run::Idle;
	}

	// Returns false when the strip budget for this tick is used up (the actor is NOT consumed then).
	bool HandleStructure(APrimalStructure* structure, int& strip_budget)
	{
		// A player strip only looks at structures of the caller's own team.
		if (g_scope_team != 0 && !InPlayerStripScope(structure->TargetingTeamField(), g_scope_team))
			return true;
		const SkinState skin = ReadSkin(structure);
		const bool flag = structure->HasAppliedCustomCosmetic();
		const bool custom = skin.kind == SkinKind::Custom;
		// A player strip removes every skin kind (the operator's allow list still protects its classes).
		const bool strip = skin.kind != SkinKind::None
			&& (g_scope_team != 0 ? !IsAllowed(g_settings, skin.class_name) : ShouldStrip(g_settings, skin.kind, skin.class_name));

		// Budget used up: leave this structure for the next tick without counting anything yet,
		// so it is not counted twice.
		if (strip && g_run == Run::StripApply && strip_budget <= 0)
			return false;

		if (flag && !custom)
			++g_scan_result.flag_without_custom;
		if (!flag && custom)
			++g_scan_result.custom_without_flag;
		if (flag)
			++g_scan_result.flag_custom;

		if (skin.kind == SkinKind::None)
			return true;

		++g_scan_result.structures_with_skin;
		g_scan_result.structure_classes.Add(ClassNameOfObject(structure));
		if (custom)
		{
			++g_scan_result.custom;
			g_scan_result.custom_ids.Add(std::to_string(skin.id));
		}
		else
		{
			++g_scan_result.official;
			g_scan_result.official_classes.Add(skin.class_name);
		}

		if (!strip)
		{
			++g_scan_result.kept_allowed;
			return true;
		}
		if (g_run == Run::StripDry || g_run == Run::Scan)
		{
			if (g_run == Run::StripDry)
				++g_scan_result.would_strip;
			return true;
		}

		// StripApply: back up first, flush, then change.
		--strip_budget;
		BackupEntry entry;
		entry.structure_id = structure->StructureIDField();
		entry.structure_class = ClassNameOfObject(structure);
		entry.skin_class_name = skin.class_name;
		entry.skin_class_path = ClassPath(skin.cls);
		entry.skin_id = skin.id;
		entry.variant = skin.variant;
		if (!g_backup.is_open() && !OpenBackupFile())
		{
			AbortRun("could not create the backup file, nothing was changed");
			return true;
		}
		g_backup << BackupLine(entry) << '\n';
		g_backup.flush();
		g_backup_used = true;
		if (!g_backup.good())
		{
			AbortRun("could not write the backup file, nothing further was changed");
			return true;
		}

		CallSetSkin(structure, nullptr, 0, 0);

		const bool gone = ReplicatedSkinClass(structure) == nullptr && LocalSkinClass(structure) == nullptr
			&& structure->MyCustomCosmeticStructureSkinIDField() == 0;
		if (gone)
			++g_scan_result.stripped;
		else
			++g_scan_result.strip_failed;
		return true;
	}

	void StepRun()
	{
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (world == nullptr)
		{
			FinishRun("Run");
			return;
		}

		UClass* structure_class = APrimalStructure::GetPrivateStaticClass();
		int budget = g_settings.scan_actors_per_tick;
		int strip_budget = g_settings.strip_per_tick;
		auto& levels = world->LevelsField();

		while (budget > 0 && g_run != Run::Idle)
		{
			if (g_scan_level >= static_cast<size_t>(levels.Num()))
			{
				FinishRun(g_run == Run::Scan ? "Scan" : g_run == Run::StripDry ? "Strip dry run" : "Strip");
				return;
			}

			ULevel* level = levels[static_cast<int32>(g_scan_level)].Get();
			if (level == nullptr)
			{
				++g_scan_level;
				g_scan_actor = 0;
				continue;
			}

			TArray<AActor*>& actors = *GetNativePointerField<TArray<AActor*>*>(level, kLevelActorsField);
			while (budget > 0 && g_run != Run::Idle && g_scan_actor < actors.Num())
			{
				AActor* actor = actors[g_scan_actor];
				if (actor != nullptr && actor->IsA(structure_class))
				{
					if (!HandleStructure(static_cast<APrimalStructure*>(actor), strip_budget))
						return;   // strip budget used up: continue with this actor next tick
					++g_scan_result.structures;
				}
				else if (actor != nullptr)
				{
					// not a structure
				}
				if (actor != nullptr)
					++g_scan_result.actors_seen;
				++g_scan_actor;
				--budget;
			}
			if (g_run != Run::Idle && g_scan_actor >= actors.Num())
			{
				++g_scan_level;
				g_scan_actor = 0;
			}
		}
	}

	void StepRestore()
	{
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		int budget = g_settings.strip_per_tick;
		while (budget > 0 && g_restore_next < g_restore_entries.size() && world != nullptr)
		{
			const BackupEntry& e = g_restore_entries[g_restore_next++];
			--budget;

			if (e.skin_class_path.empty())
			{
				++g_restore_skipped;   // the skin had no class to load (id-only skin)
				continue;
			}
			APrimalStructure* s = APrimalStructure::GetFromID(world, static_cast<unsigned int>(e.structure_id));
			if (s == nullptr)
			{
				++g_restore_missing;
				continue;
			}
			if (ReadSkin(s).kind != SkinKind::None)
			{
				++g_restore_conflict;  // it has a skin again; do not toggle or overwrite it
				continue;
			}
			UClass* cls = UVictoryCore::BPLoadClass(FString(e.skin_class_path));
			if (cls == nullptr)
				cls = UVictoryCore::BPLoadClass(FString("Blueprint'" + e.skin_class_path + "'"));
			if (cls == nullptr)
			{
				++g_restore_failed;
				continue;
			}
			CallSetSkin(s, cls, e.skin_id, e.variant);
			if (ReadSkin(s).kind != SkinKind::None)
				++g_restore_done;
			else
				++g_restore_failed;
		}

		if (g_restore_next >= g_restore_entries.size() || world == nullptr)
		{
			Log::GetLog()->info("Restore finished: {} restored, {} structure missing, {} already skinned, {} without a skin class, {} failed",
				g_restore_done, g_restore_missing, g_restore_conflict, g_restore_skipped, g_restore_failed);
			g_restore_entries.clear();
			g_run = Run::Idle;
		}
	}

	std::string StartRun(Run mode);   // defined with the commands below

	// BlockSkinApplyMessage: tell the player standing nearest the structure (at most once per 5 s each).
	void NotifyBlockedApply(APrimalStructure* structure)
	{
		if (g_settings.block_skin_apply_message.empty() || structure == nullptr)
			return;
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (world == nullptr)
			return;

		FVector where;
		structure->GetActorLocation(&where);

		AShooterPlayerController* nearest = nullptr;
		double best = 3000.0;   // centimetres
		for (auto& weak : world->PlayerControllerListField())
		{
			APlayerController* controller = weak.Get();
			if (controller == nullptr)
				continue;
			const double distance = FVector::Distance(where, AsaApi::IApiUtils::GetPosition(controller));
			if (distance < best)
			{
				best = distance;
				nearest = static_cast<AShooterPlayerController*>(controller);
			}
		}
		if (nearest == nullptr)
			return;

		long long& last = g_last_apply_notice[reinterpret_cast<uintptr_t>(nearest)];
		if (last != 0 && g_clock + 1 - last < 5)
			return;
		last = g_clock + 1;
		AsaApi::GetApiUtils().SendServerMessage(nearest, FColorList::Yellow, "{}", g_settings.block_skin_apply_message);
	}

	// StripOnMapLoad / StripRepeat_Minutes: start a strip without a command. The backup file is created only
	// when something is actually stripped.
	void MaybeAutoStrip()
	{
		if (!g_settings.strip_on_map_load || !g_settings.enable_strip || g_run != Run::Idle)
			return;
		if (g_next_auto_strip == LLONG_MAX)
			return;                                       // single run already done
		if (g_next_auto_strip < 0)
			g_next_auto_strip = g_clock + g_settings.strip_on_map_load_delay_seconds;
		if (g_clock < g_next_auto_strip || AsaApi::GetApiUtils().GetWorld() == nullptr)
			return;

		Log::GetLog()->info("Automatic strip starting (StripOnMapLoad)");
		StartRun(Run::StripApply);
		g_next_auto_strip = g_settings.strip_repeat_minutes > 0 ? g_clock + g_settings.strip_repeat_minutes * 60LL : LLONG_MAX;
	}

	void DeferTick();
	void PruneConnectionInfo();   // defined with the player commands below

	void OnTimer()
	{
		++g_clock;
		g_game_thread = std::this_thread::get_id();
		DeferTick();
		DbTick();
		if (g_clock - g_prune_clock >= 5)
		{
			g_prune_clock = g_clock;
			PruneConnectionInfo();
		}
		FlushItemBackup();
		MaybeAutoStrip();
		try
		{
			if (g_run == Run::Restore)
				StepRestore();
			else if (g_run != Run::Idle)
				StepRun();
		}
		catch (const std::exception& e)
		{
			AbortRun(e.what());
		}
		if (g_alive && g_clock - g_window_start >= g_settings.summary_interval_seconds)
			WriteSummary();
	}

	// ------------------------------------------------------------------------------------------
	// Commands
	// ------------------------------------------------------------------------------------------

	std::string FirstWord(const std::string& text)
	{
		const size_t start = text.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
			return "";
		const size_t end = text.find_first_of(" \t\r\n", start);
		return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
	}

	// "CosmeticsDisabler.Strip confirm" -> "confirm"
	std::string ArgsOf(const std::string& full)
	{
		const size_t start = full.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
			return "";
		const size_t space = full.find_first_of(" \t\r\n", start);
		if (space == std::string::npos)
			return "";
		const size_t arg = full.find_first_not_of(" \t\r\n", space);
		return arg == std::string::npos ? "" : full.substr(arg);
	}

	std::string StartRun(Run mode)
	{
		g_scan_result = ScanResult{};
		g_scan_level = 0;
		g_scan_actor = 0;
		g_run = mode;
		return "";
	}

	std::string RunReload(const std::string&)
	{
		std::string error;
		if (ReloadSettings(error))
		{
			LoadPreferences();
			EnsureChannelHook();
			return "CosmeticsDisabler: config reloaded (hook toggles apply immediately)";
		}
		Log::GetLog()->error("Config reload failed: {}", error);
		return "CosmeticsDisabler: reload failed - " + error + " (previous settings kept)";
	}

	std::string RunScan(const std::string&)
	{
		if (g_run != Run::Idle)
			return "CosmeticsDisabler: another run is already in progress";
		StartRun(Run::Scan);
		return "CosmeticsDisabler: scan started, the result is written to the log when it finishes";
	}

	std::string RunStrip(const std::string& args)
	{
		if (g_run != Run::Idle)
			return "CosmeticsDisabler: another run is already in progress";

		const std::string word = FirstWord(args);
		if (word.empty())
		{
			StartRun(Run::StripDry);
			return "CosmeticsDisabler: strip DRY RUN started (nothing is changed); result goes to the log. Then run \"" + std::string(kStripCommand)
				+ " confirm\" to apply";
		}
		if (word != "confirm")
			return std::string("CosmeticsDisabler: unknown argument. Use ") + kStripCommand + " for a dry run or " + kStripCommand + " confirm";
		if (!g_settings.enable_strip)
			return "CosmeticsDisabler: strip refused, EnableStrip is false in config.json";
		if (!g_dry_run_done)
			return std::string("CosmeticsDisabler: strip refused, run a dry run first (") + kStripCommand + ") and check the log";

		if (!OpenBackupFile())
			return "CosmeticsDisabler: strip refused, cannot create the backup file in " + BackupDir();

		StartRun(Run::StripApply);
		return "CosmeticsDisabler: strip started (backup " + g_backup_name + "); result goes to the log";
	}

	std::string RunRestore(const std::string& args)
	{
		if (g_run != Run::Idle)
			return "CosmeticsDisabler: another run is already in progress";

		const std::string name = FirstWord(args);
		if (name.empty())
		{
			std::string list;
			std::error_code ec;
			for (const auto& item : std::filesystem::directory_iterator(BackupDir(), ec))
				if (item.path().extension() == ".jsonl")
					list += (list.empty() ? "" : ", ") + item.path().filename().string();
			return list.empty() ? "CosmeticsDisabler: no backup files found" : "CosmeticsDisabler: backups: " + list + " (use " + kRestoreCommand + " <file>)";
		}
		if (!g_settings.enable_strip)
			return "CosmeticsDisabler: restore refused, EnableStrip is false in config.json";
		if (!IsSafeBackupName(name))
			return "CosmeticsDisabler: restore refused, give just the backup file name";

		std::ifstream file(BackupDir() + "/" + name);
		if (!file.is_open())
			return "CosmeticsDisabler: cannot open backup " + name;

		g_restore_entries.clear();
		std::string line;
		long long bad = 0;
		while (std::getline(file, line))
		{
			if (line.empty())
				continue;
			try { g_restore_entries.push_back(ParseBackupLine(line)); }
			catch (const std::exception&) { ++bad; }
		}
		if (bad > 0)
			Log::GetLog()->warn("Restore: {} unreadable lines in {} were skipped", bad, name);
		if (g_restore_entries.empty())
			return "CosmeticsDisabler: nothing to restore in " + name;

		g_restore_next = 0;
		g_restore_done = g_restore_missing = g_restore_conflict = g_restore_skipped = g_restore_failed = 0;
		g_run = Run::Restore;
		return "CosmeticsDisabler: restoring " + std::to_string(g_restore_entries.size()) + " structures from " + name + "; result goes to the log";
	}

	// StripTribe <tribe id> [confirm]: an admin strips every skin from one tribe's structures on this map. Same run, backup and
	// restore as the player /stripskins, but no cooldown and any tribe. The report must come first (within 10 minutes).
	std::string RunStripTribe(const std::string& args)
	{
		const std::string usage = std::string("CosmeticsDisabler: use ") + kStripTribeCommand + " <tribe id> for a report, then " + kStripTribeCommand
			+ " <tribe id> confirm";
		if (g_run != Run::Idle)
			return "CosmeticsDisabler: another run is already in progress";
		const std::string id_text = FirstWord(args);
		const int team = ParseTribeId(id_text);
		if (team == 0)
			return id_text.empty() ? usage : "CosmeticsDisabler: \"" + id_text + "\" is not a tribe id (a number from 50000 up)";
		const std::string word = FirstWord(ArgsOf(args));

		if (word.empty())
		{
			g_scope_team = team;
			g_scope_requester = g_command_caller;
			g_scope_admin = true;
			StartRun(Run::StripDry);
			return "CosmeticsDisabler: report for tribe " + std::to_string(team) + " started (nothing is changed); the result is sent here and to the log";
		}
		if (word != "confirm")
			return usage;
		if (!g_settings.enable_strip)
			return "CosmeticsDisabler: strip refused, EnableStrip is false in config.json";
		const auto report = g_admin_report_time.find(team);
		if (report == g_admin_report_time.end() || !PlayerReportStillValid(NowSeconds(), report->second))
			return std::string("CosmeticsDisabler: run ") + kStripTribeCommand + " " + std::to_string(team)
				+ " first; confirm is allowed for 10 minutes after a report that found skins";

		g_scope_team = team;
		g_scope_requester = g_command_caller;
		g_scope_admin = true;
		if (!OpenBackupFile())
		{
			g_scope_team = 0;
			g_scope_requester.clear();
			g_scope_admin = false;
			return "CosmeticsDisabler: strip refused, cannot create the backup file in " + BackupDir();
		}
		StartRun(Run::StripApply);
		Log::GetLog()->info("StripTribe confirmed for tribe {} by {} (backup {})", team, g_command_caller.empty() ? std::string("RCON") : g_command_caller,
			g_backup_name);
		return "CosmeticsDisabler: stripping tribe " + std::to_string(team) + " (backup " + g_backup_name + "); the result is sent here and to the log";
	}

	std::string RunStatus(const std::string&)
	{
		Log::GetLog()->info("{}", FormatSummary(g_counters, static_cast<int>(g_clock - g_window_start)));
		return "CosmeticsDisabler: items to clients " + std::to_string(g_counters.items_to_client) + " (with skin "
			+ std::to_string(g_counters.items_with_skin) + "), characters with cosmetic "
			+ std::to_string(g_counters.character_with_cosmetic) + ", structure skin messages "
			+ std::to_string(g_counters.skin_messages) + " - details in the log";
	}

	// ---- Player chat commands ----

	void Tell(AShooterPlayerController* pc, const std::string& text)
	{
		AsaApi::GetApiUtils().SendServerMessage(pc, FColorList::Green, "{}", text);
	}

	std::string EosOf(AShooterPlayerController* pc)
	{
		return AsaApi::GetApiUtils().GetEOSIDFromController(pc).ToString();
	}

	// /hideskins and /showskins: the choice is stored by EOS ID and takes effect when the connection is made again.
	void ChatSetHidden(AShooterPlayerController* pc, bool hide)
	{
		if (pc == nullptr)
			return;
		if (!g_settings.enable_player_hide_commands)
			return Tell(pc, "This command is not enabled on this server.");
		if (!ConsoleBlockingOn(g_settings))
			return Tell(pc, "Skin hiding is not switched on for this server, so this would change nothing.");
		const std::string eos_id = EosOf(pc);
		if (!OptInList::IsValidId(eos_id))
			return Tell(pc, "Could not read your player ID. Try again in a moment.");

		if (PreferencesInDatabase() && !g_db_loaded)
			return Tell(pc, "Your saved choice cannot be reached right now. Try again in a minute.");

		if (hide)
		{
			if (g_optin.Contains(eos_id))
				return Tell(pc, "Skins are already hidden for you. If you still see them, relog.");
			g_optin.Add(eos_id);
		}
		else
		{
			if (!g_optin.Contains(eos_id))
				return Tell(pc, "Skins are not hidden for you. Nothing to change.");
			g_optin.Remove(eos_id);
		}
		const std::string done = hide ? "Skins will be hidden for you like on console. Relog (disconnect and join again) to apply it. Use "
				+ g_settings.show_skins_command + " to undo."
			: "Skins will show normally again. Relog (disconnect and join again) to apply it.";

		if (PreferencesInDatabase())
		{
			// Saved by the worker; the player is told when the database has it (it then counts on every server).
			++g_optin_write_seq;
			const std::string sql = SqlSetPreference(g_db_config.table, eos_id, hide, MapLabel());
			g_db->Post([sql, eos_id, hide, done](CosmeticsDb::Db& db)
			{
				const bool ok = db.Exec(sql) >= 0;
				const std::string error = ok ? "" : db.LastError();
				PostDbOutcome(ok, "saving a choice", error);
				g_db_results.Post([ok, eos_id, hide, done]
				{
					if (!ok)
					{
						++g_optin_write_seq;
						if (hide) g_optin.Remove(eos_id); else g_optin.Add(eos_id);
						NotifyPlayer(eos_id, "Could not save your choice. Try again in a minute.");
						return;
					}
					Log::GetLog()->info("A player {} skins (EOS {}), saved to the database", hide ? "hides" : "shows", eos_id);
					NotifyPlayer(eos_id, done);
				});
			});
			return Tell(pc, "Saving your choice...");
		}

		if (!SaveOptIn())
		{
			// keep memory and file in step: undo the change
			if (hide) g_optin.Remove(eos_id); else g_optin.Add(eos_id);
			Log::GetLog()->error("Could not write {}", OptInPath());
			return Tell(pc, "Could not save your choice. Tell an admin.");
		}
		Log::GetLog()->info("A player {} skins (EOS {}); {} players opted in", hide ? "hides" : "shows", eos_id, g_optin.Size());
		Tell(pc, done);
	}

	void ChatHide(AShooterPlayerController* pc, FString*, int, int) { ChatSetHidden(pc, true); }
	void ChatShow(AShooterPlayerController* pc, FString*, int, int) { ChatSetHidden(pc, false); }

	// /stripskins (report) and /stripskins confirm: removes the skins from the caller's own tribe's structures.
	void ChatStrip(AShooterPlayerController* pc, FString* message, int, int)
	{
		if (pc == nullptr)
			return;
		if (!g_settings.enable_player_strip)
			return Tell(pc, "This command is not enabled on this server.");
		const int team = pc->TargetingTeamField();
		if (team < kFirstPlayerTeam)
			return Tell(pc, "You are not in a tribe, so there is nothing to strip.");
		const std::string eos_id = EosOf(pc);
		if (eos_id.empty())
			return Tell(pc, "Could not read your player ID. Try again in a moment.");
		if (g_run != Run::Idle)
			return Tell(pc, "The server is busy with another run. Try again in a minute.");

		const std::string word = FirstWord(message != nullptr ? ArgsOf(message->ToString()) : std::string());
		const long long now = NowSeconds();
		const auto last = g_player_strip_time.find(team);
		const int wait = CooldownMinutesLeft(now, last != g_player_strip_time.end() ? last->second : 0, g_settings.player_strip_cooldown_minutes);
		if (wait > 0)
			return Tell(pc, "Your tribe stripped skins recently. Try again in " + std::to_string(wait) + " minutes.");

		if (word.empty())
		{
			g_scope_team = team;
			g_scope_requester = eos_id;
			StartRun(Run::StripDry);
			Log::GetLog()->info("Player strip report started for team {} by {}", team, eos_id);
			return Tell(pc, "Counting the skins on your tribe's structures (nothing is changed). I will message you with the result.");
		}
		if (word != "confirm")
			return Tell(pc, "Use " + g_settings.strip_skins_command + " for a report, then " + g_settings.strip_skins_command + " confirm.");

		const auto report = g_player_report_time.find(team);
		if (report == g_player_report_time.end() || !PlayerReportStillValid(now, report->second))
			return Tell(pc, "Run " + g_settings.strip_skins_command + " first and read the report; confirm is allowed for 10 minutes after it.");

		g_scope_team = team;
		g_scope_requester = eos_id;
		if (!OpenBackupFile())
		{
			g_scope_team = 0;
			g_scope_requester.clear();
			return Tell(pc, "Could not create the backup file. Tell an admin.");
		}
		StartRun(Run::StripApply);
		Log::GetLog()->info("Player strip confirmed for team {} by {} (backup {})", team, eos_id, g_backup_name);
		Tell(pc, "Removing the skins from your tribe's structures. This takes a little while, I will message you when it is done.");
	}

	// Entries of connections that no longer exist are dropped, so a returning player is read again (the platform and the
	// /hideskins choice are fixed per connection). Cheap: runs every few seconds over the connected players only.
	void PruneConnectionInfo()
	{
		if (g_connection_info.empty())
			return;
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (world == nullptr)
			return;
		std::unordered_set<UNetConnection*> live;
		for (auto& weak : world->PlayerControllerListField())
		{
			APlayerController* controller = weak.Get();
			if (controller != nullptr)
				live.insert(controller->GetNetConnection());
		}
		for (auto entry = g_connection_info.begin(); entry != g_connection_info.end();)
			entry = live.count(entry->first) != 0 ? std::next(entry) : g_connection_info.erase(entry);
	}

	template <std::string (*Fn)(const std::string&)>
	void ConsoleCommand(APlayerController* player_controller, FString* command, bool /*write_to_log*/)
	{
		auto* shooter_controller = static_cast<AShooterPlayerController*>(player_controller);
		if (shooter_controller == nullptr || !shooter_controller->bIsAdmin()())
			return;
		g_command_caller = AsaApi::GetApiUtils().GetEOSIDFromController(shooter_controller).ToString();
		const std::string reply = Fn(command != nullptr ? ArgsOf(command->ToString()) : std::string());
		g_command_caller.clear();
		AsaApi::GetApiUtils().SendServerMessage(shooter_controller, FColorList::Green, "{}", reply);
	}

	template <std::string (*Fn)(const std::string&)>
	void RconCommand(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld* /*world*/)
	{
		FString reply(Fn(ArgsOf(rcon_packet->Body.ToString())));
		rcon_connection->SendMessageW(rcon_packet->Id, 0, &reply);
	}

	template <std::string (*Fn)(const std::string&)>
	void AddBoth(const char* name)
	{
		auto& commands = AsaApi::GetCommands();
		commands.AddConsoleCommand(name, &ConsoleCommand<Fn>);
		commands.AddRconCommand(name, &RconCommand<Fn>);
	}

	void RemoveBoth(const char* name)
	{
		auto& commands = AsaApi::GetCommands();
		commands.RemoveConsoleCommand(name);
		commands.RemoveRconCommand(name);
	}
} // namespace

// ----------------------------------------------------------------------------------------------
// Hooks. Each calls the original first, then only reads.
// ----------------------------------------------------------------------------------------------

DECLARE_HOOK(UPrimalItem_GetItemNetInfo, FItemNetInfo*, UPrimalItem*, FItemNetInfo*, bool);
// A client asks for an inventory's items (also another character's equipped gear when it comes into range).
DECLARE_HOOK(UPrimalInventoryComponent_ServerRequestItems, void, UPrimalInventoryComponent*, AShooterPlayerController*, bool, bool);
DECLARE_HOOK(AShooterPlayerController_ClientAddActorItem, void, AShooterPlayerController*, UPrimalInventoryComponent*, FItemNetInfo&, bool, bool, bool);
DECLARE_HOOK(AShooterCharacter_PreReplication, void, AShooterCharacter*, IRepChangedPropertyTracker&);
DECLARE_HOOK(APrimalStructure_MultiHandleSkinNetMessage, void, APrimalStructure*, FName, const FSkinNetMessageParams&);
DECLARE_HOOK(APrimalStructure_SetStructureSkin, bool, APrimalStructure*, TSubclassOf<APrimalStructure>&, __int64, int);
DECLARE_HOOK(APrimalStructure_MultiRefreshStructureSkin, void, APrimalStructure*, TSubclassOf<APrimalStructure>&, __int64, int, bool, bool);
// Item entry probe (count and log only). Signatures follow the game's functions: references are pointers.
DECLARE_HOOK(UPrimalInventoryComponent_AddItem, UPrimalItem*, UPrimalInventoryComponent*, FItemNetInfo*, bool, bool, bool, FItemNetID*, bool, bool, bool,
	AShooterCharacter*, bool, bool, bool, bool, bool, bool);
DECLARE_HOOK(UPrimalInventoryComponent_NotifyItemAdded, void, UPrimalInventoryComponent*, UPrimalItem*, bool, bool);
DECLARE_HOOK(UPrimalItem_InitFromNetInfo, void, UPrimalItem*, FItemNetInfo*, UWorld*);
DECLARE_HOOK(UPrimalItem_UseItemOntoItem, bool, UPrimalItem*, UPrimalItem*, int, AShooterPlayerController*);
DECLARE_HOOK(UPrimalItem_InventoryLoadedFromSaveGame, void, UPrimalItem*);
DECLARE_HOOK(UPrimalItem_AddNewItem, UPrimalItem*, TSubclassOf<UPrimalItem>*, UPrimalInventoryComponent*, bool, bool, float, bool, int, bool, float, bool,
	TSubclassOf<UPrimalItem>*, float, bool, bool, bool, bool, bool, bool, UWorld*);

FItemNetInfo* Hook_UPrimalItem_GetItemNetInfo(UPrimalItem* item, FItemNetInfo* result, bool for_client)
{
	FItemNetInfo* info = UPrimalItem_GetItemNetInfo_original(item, result, for_client);
	if (!g_alive || !for_client || info == nullptr || item == nullptr)
		return info;
	if (!g_settings.probe_items && !g_settings.filter_items && !g_settings.probe_item_requests && !g_settings.block_item_skins_for_consoles
		&& !g_settings.unequip_skin_items_for_consoles)
		return info;

	try
	{
		UClass* skin = info->ItemSkinTemplateField().uClass;
		const __int64 mod_id = item->CustomCosmeticModSkinReplacementIDField();
		const bool has_custom_data = info->CustomItemDatasField().Num() > 0;

		if (g_settings.probe_item_requests)
		{
			const bool skinned = skin != nullptr || HasCosmeticId(mod_id);
			if (g_request.active && std::this_thread::get_id() == g_game_thread)
			{
				++g_request.items;
				if (skinned)
					++g_request.skinned;
				if (g_request.equipped)
					g_counters.equipped_item_classes.Add(ClassNameOfObject(item) + (skinned ? " (skinned)" : ""));
			}
			else
			{
				++g_counters.outside_items;
				if (skinned)
					++g_counters.outside_items_skinned;
			}
		}

		if (g_settings.probe_items)
		{
			++g_counters.items_to_client;
			if (skin != nullptr)
			{
				++g_counters.items_with_skin;
				g_counters.skin_classes.Add(ClassName(skin));
			}
			if (HasCosmeticId(mod_id))
			{
				++g_counters.items_with_mod_id;
				g_counters.mod_ids.Add(std::to_string(mod_id));
			}
			if (has_custom_data)
				++g_counters.items_with_custom_data;

			if ((skin != nullptr || HasCosmeticId(mod_id)) && g_budget.Take())
				Log::GetLog()->info("item to client: {} skin={} modId={} customData={} weaponTemplate={} customClass={} archetype={} netModId={}", ClassNameOfObject(item),
					ClassName(skin), mod_id, has_custom_data ? "yes" : "no", WeaponTemplateName(item), ClassName(info->ItemCustomClassField().uClass),
					ClassName(info->ItemArchetypeField().uClass), NetModSkinId(info));

			// Probe: what the custom data of a skinned or mod-skinned item holds, to find where a mod cosmetic is named.
			if (HasCosmeticId(mod_id) && has_custom_data)
			{
				TArray<FCustomItemData, TSizedDefaultAllocator<32>>& datas = info->CustomItemDatasField();
				for (int d = 0; d < datas.Num() && d < 6; ++d)
				{
					FCustomItemData& data = datas[d];
					std::string strings, classes, names, doubles;
					for (int i = 0; i < data.CustomDataDoubles.Doubles.Num() && i < 6; ++i)
						doubles += (i ? "|" : "") + std::to_string(static_cast<long long>(data.CustomDataDoubles.Doubles[i]));
					for (int i = 0; i < data.CustomDataStrings.Num() && i < 4; ++i)
						strings += (i ? "|" : "") + data.CustomDataStrings[i].ToString();
					for (int i = 0; i < data.CustomDataClasses.Num() && i < 4; ++i)
						classes += (i ? "|" : "") + ClassName(data.CustomDataClasses[i]);
					for (int i = 0; i < data.CustomDataNames.Num() && i < 4; ++i)
						names += (i ? "|" : "") + data.CustomDataNames[i].ToString().ToString();
					if (g_budget.Take())
						Log::GetLog()->info("  custom data {}: name={} strings={} [{}] floats={} objects={} classes={} [{}] names={} [{}] softClasses={} doubles={} [{}] byteArrays={}", d,
							data.CustomDataName.ToString().ToString(), data.CustomDataStrings.Num(), strings, data.CustomDataFloats.Num(),
							data.CustomDataObjects.Num(), data.CustomDataClasses.Num(), classes, data.CustomDataNames.Num(), names,
							data.CustomDataSoftClasses.Num(), data.CustomDataDoubles.Doubles.Num(), doubles, data.CustomDataBytes.ByteArrays.Num());
				}
			}
		}

		// Filter: only the copy being sent to the client is changed; the item on the server keeps its skin.
		if (g_settings.filter_items && (skin != nullptr || HasCosmeticId(mod_id)) && ShouldFilterItem(g_settings, ClassName(skin), mod_id))
		{
			info->ItemSkinTemplateField().uClass = nullptr;
			++g_counters.items_filtered;
			if (g_settings.filter_item_custom_data && has_custom_data)
			{
				info->CustomItemDatasField().Reset();
				++g_counters.items_custom_data_cleared;
			}
		}

		// Experimental: a skin item that is equipped (the outfit worn on a character) is sent to a console viewer as not
		// equipped, so the client does not apply the look. Only inside a request from a console connection.
		if (g_settings.unequip_skin_items_for_consoles && g_request.active && g_request.console && std::this_thread::get_id() == g_game_thread
			&& (item->bIsItemSkin().Get() || ClassNameOfObject(item).find("PrimalItemSkin") != std::string::npos) && info->bIsEquipped().Get())
		{
			info->bIsEquipped() = false;
			++g_counters.skin_items_unequipped;
			g_counters.unequipped_skin_classes.Add(ClassNameOfObject(item));
		}

		// Console viewers only: blank the skin in the copy built for a ServerRequestItems call made by a console connection.
		// PC viewers and builds outside a request are untouched. The server item keeps its skin. Mod cosmetic ids are not
		// part of the net info, so they cannot be blanked here.
		if (g_settings.block_item_skins_for_consoles && g_request.active && g_request.console && std::this_thread::get_id() == g_game_thread
			&& (info->ItemSkinTemplateField().uClass != nullptr || NetModSkinId(info) != 0))
		{
			info->ItemSkinTemplateField().uClass = nullptr;
			if (NetModSkinId(info) != 0)
			{
				NetModSkinId(info) = 0;
				NetModSkinVariant(info) = 0;
				++g_counters.item_mod_ids_blanked_for_console;
			}
			++g_counters.items_blanked_for_console;
			++g_request.blanked;
			if (g_settings.filter_item_custom_data && info->CustomItemDatasField().Num() > 0)
			{
				info->CustomItemDatasField().Reset();
				++g_counters.items_custom_data_cleared;
			}
		}
	}
	catch (...)
	{
	}
	return info;
}

UPrimalItem* Hook_UPrimalInventoryComponent_AddItem(UPrimalInventoryComponent* inv, FItemNetInfo* info, bool equip, bool slot, bool no_stack,
	FItemNetID* insert_after, bool hud, bool no_spoil, bool force_incomplete, AShooterCharacter* owner, bool ignore_max, bool insert_at, bool version_check,
	bool no_refresh, bool no_milestones, bool no_broadcast)
{
	const bool probe = g_alive && g_settings.probe_item_entry && info != nullptr;
	UClass* skin = nullptr;
	if (probe)
	{
		skin = info->ItemSkinTemplateField().uClass;
		++g_entry.add_item;
		if (skin != nullptr)
			++g_entry.add_item_skin;
	}
	UPrimalItem* result = UPrimalInventoryComponent_AddItem_original(inv, info, equip, slot, no_stack, insert_after, hud, no_spoil, force_incomplete, owner,
		ignore_max, insert_at, version_check, no_refresh, no_milestones, no_broadcast);
	if (probe && skin != nullptr && g_budget.Take())
		Log::GetLog()->info("item entry: AddItem archetype={} skin={} equip={}", ClassName(info->ItemArchetypeField().uClass), ClassName(skin), equip ? "yes" : "no");
	return result;
}

void Hook_UPrimalInventoryComponent_NotifyItemAdded(UPrimalInventoryComponent* inv, UPrimalItem* item, bool equipped, bool no_broadcast)
{
	UPrimalInventoryComponent_NotifyItemAdded_original(inv, item, equipped, no_broadcast);
	if (!g_alive || !g_settings.probe_item_entry || item == nullptr)
		return;
	++g_entry.notify;
	UClass* skin = item->ItemSkinTemplateField().uClass;
	if (skin == nullptr)
		return;
	++g_entry.notify_skin;
	if (g_budget.Take())
		Log::GetLog()->info("item entry: NotifyItemAdded item={} skin={} weaponTemplate={}", ClassNameOfObject(item), ClassName(skin), WeaponTemplateName(item));
}

void Hook_UPrimalItem_InitFromNetInfo(UPrimalItem* item, FItemNetInfo* info, UWorld* world)
{
	const bool active = g_alive && info != nullptr && (g_settings.probe_item_entry || g_settings.strip_items_on_entry);
	UClass* skin = nullptr;
	bool stripped = false;
	if (active)
	{
		skin = info->ItemSkinTemplateField().uClass;
		++g_entry.init;
		if (skin != nullptr)
		{
			++g_entry.init_skin;
			// Before the original runs: the item is then built without the skin, so nothing is applied and a weapon
			// keeps its own weapon template. Only the incoming data is changed.
			if (ShouldStripItemOnEntry(g_settings, ClassName(skin)))
			{
				ItemBackupEntry record;
				record.kind = "entry";
				record.item_id1 = info->ItemIDField().ItemID1;
				record.item_id2 = info->ItemIDField().ItemID2;
				record.item_class = ClassName(info->ItemArchetypeField().uClass);
				record.skin_class_name = ClassName(skin);
				record.skin_class_path = ClassPath(skin);
				if (WriteItemBackup(record))
				{
					info->ItemSkinTemplateField().uClass = nullptr;
					stripped = true;
					++g_entry.init_stripped;
				}
			}
		}
	}
	UPrimalItem_InitFromNetInfo_original(item, info, world);
	if (active && skin != nullptr && g_budget.Take())
		Log::GetLog()->info("item entry: InitFromNetInfo archetype={} skin={} removed={}", ClassName(info->ItemArchetypeField().uClass), ClassName(skin),
			stripped ? "yes" : "no");
}

UPrimalItem* Hook_UPrimalItem_AddNewItem(TSubclassOf<UPrimalItem>* archetype, UPrimalInventoryComponent* inv, bool equip, bool no_stack, float quality,
	bool no_bp, int quantity, bool force_bp, float max_difficulty, bool create_on_client, TSubclassOf<UPrimalItem>* apply_skin, float min_random_quality,
	bool clamp, bool ignore_max, bool skip_deny, bool hud, bool no_milestones, bool no_broadcast, UWorld* world)
{
	const bool probe = g_alive && g_settings.probe_item_entry;
	UClass* skin = (probe && apply_skin != nullptr) ? apply_skin->uClass : nullptr;
	if (probe)
	{
		++g_entry.add_new;
		if (skin != nullptr)
			++g_entry.add_new_skin;
	}
	UPrimalItem* result = UPrimalItem_AddNewItem_original(archetype, inv, equip, no_stack, quality, no_bp, quantity, force_bp, max_difficulty, create_on_client,
		apply_skin, min_random_quality, clamp, ignore_max, skip_deny, hud, no_milestones, no_broadcast, world);
	if (probe && skin != nullptr && g_budget.Take())
		Log::GetLog()->info("item entry: AddNewItem archetype={} skin={}", archetype != nullptr ? ClassName(archetype->uClass) : "(null)", ClassName(skin));
	return result;
}

void Hook_AShooterCharacter_PreReplication(AShooterCharacter* character, IRepChangedPropertyTracker& tracker)
{
	AShooterCharacter_PreReplication_original(character, tracker);
	if (!g_alive || !g_settings.probe_players || character == nullptr)
		return;

	try
	{
		++g_counters.character_prerep;

		const __int64 head = character->HeadHairCustomCosmeticModIDField();
		const __int64 eyebrow = character->EyebrowCustomCosmeticModIDField();
		const __int64 facial = character->FacialHairCustomCosmeticModIDField();
		const bool carries = HasCosmeticId(head) || HasCosmeticId(eyebrow) || HasCosmeticId(facial);
		if (carries)
			++g_counters.character_with_cosmetic;

		CharacterSnapshot& seen = g_seen_characters[reinterpret_cast<uintptr_t>(character)];
		if (seen.looked && seen.head == head && seen.eyebrow == eyebrow && seen.facial == facial)
			return;
		seen = CharacterSnapshot{ true, head, eyebrow, facial };

		const std::string crossplay = PlatformLabel(static_cast<int>(character->GetCrossplayPlatform()));
		const std::string connection = ConnectionPlatformLabel(g_settings.read_connection_platform ? ReadConnectionPlatform(character) : -1);
		g_counters.connection_platforms.Add(connection);
		if (carries)
		{
			g_counters.platforms.Add(crossplay);
			for (const __int64 id : { head, eyebrow, facial })
				if (HasCosmeticId(id))
					g_counters.mod_ids.Add(std::to_string(id));
		}

		if (g_budget.Take())
			Log::GetLog()->info("character: crossplay={} connection={} head={} eyebrow={} facial={} cosmetic={}", crossplay, connection, head,
				eyebrow, facial, carries ? "yes" : "no");
	}
	catch (...)
	{
	}
}

void Hook_APrimalStructure_MultiHandleSkinNetMessage(APrimalStructure* structure, FName message_name, const FSkinNetMessageParams& params)
{
	APrimalStructure_MultiHandleSkinNetMessage_original(structure, message_name, params);
	if (!g_alive || !g_settings.probe_structure_messages)
		return;

	try
	{
		++g_counters.skin_messages;
		const std::string name = message_name.ToString().ToString();
		g_counters.message_names.Add(name);
		g_counters.message_structures.Add(ClassNameOfObject(structure));
		if (g_budget.Take())
			Log::GetLog()->info("structure skin message: {} on {}", name, ClassNameOfObject(structure));
	}
	catch (...)
	{
	}
}

// Probe: the skin change multicast that carries the class, id and variant as arguments (not as properties),
// so the ReplicateActor blanking does not touch it. Counts and logs only.
void Hook_APrimalStructure_MultiRefreshStructureSkin(APrimalStructure* structure, TSubclassOf<APrimalStructure>& skin_class, __int64 skin_id, int variant, bool a, bool b)
{
	APrimalStructure_MultiRefreshStructureSkin_original(structure, skin_class, skin_id, variant, a, b);
	if (!g_alive || !g_settings.probe_structure_messages)
		return;

	try
	{
		++g_counters.skin_refreshes;
		const std::string cls = skin_class.uClass != nullptr ? ClassName(skin_class.uClass) : "id " + std::to_string(skin_id);
		g_counters.refresh_skin_classes.Add(cls);
		if (g_budget.Take())
			Log::GetLog()->info("structure skin refresh multicast: {} (id {}, variant {}, flags {} {}) on {}", cls, skin_id, variant, a, b,
				ClassNameOfObject(structure));
	}
	catch (...)
	{
	}
}

// BlockSkinApply: refuses a new skin that StripMode / AllowSkinClasses would strip. Removing a skin
// (null class, id 0) always goes through, and so do the plugin's own Restore calls.
bool Hook_APrimalStructure_SetStructureSkin(APrimalStructure* structure, TSubclassOf<APrimalStructure>& skin_class, __int64 skin_id, int variant)
{
	if (g_alive && g_settings.probe_structure_messages && !g_internal_skin_call)
	{
		try
		{
			++g_counters.skin_sets;
			const std::string cls = skin_class.uClass != nullptr ? ClassName(skin_class.uClass) : "id " + std::to_string(skin_id);
			g_counters.set_skin_classes.Add(cls);
			if (g_budget.Take())
				Log::GetLog()->info("structure skin set: {} (id {}, variant {}) on {}", cls, skin_id, variant, ClassNameOfObject(structure));
		}
		catch (...)
		{
		}
	}
	if (g_alive && g_settings.block_skin_apply && !g_internal_skin_call && structure != nullptr)
	{
		try
		{
			const std::string name = ClassName(skin_class.uClass);
			if ((skin_class.uClass != nullptr || skin_id != 0) && ShouldBlockSkinApply(g_settings, name, skin_id))
			{
				++g_counters.skin_applies_blocked;
				g_counters.blocked_skin_classes.Add(skin_class.uClass != nullptr ? name : "id " + std::to_string(skin_id));
				if (g_budget.Take())
					Log::GetLog()->info("blocked skin apply: {} (id {}) on {}", name, skin_id, ClassNameOfObject(structure));
				NotifyBlockedApply(structure);
				return false;
			}
		}
		catch (...)
		{
		}
	}
	return APrimalStructure_SetStructureSkin_original(structure, skin_class, skin_id, variant);
}

// BlockSkinApply for items: a player using a skin item onto another item (cosmetics tab). Refusing here happens
// before the skin is set, so the item and its weapon template stay untouched.
bool Hook_UPrimalItem_UseItemOntoItem(UPrimalItem* skin_item, UPrimalItem* destination, int additional_data, AShooterPlayerController* pc)
{
	if (g_alive && g_settings.block_skin_apply && skin_item != nullptr)
	{
		try
		{
			const std::string name = ClassNameOfObject(skin_item);
			if (ShouldBlockItemSkinApply(g_settings, name))
			{
				if (g_budget.Take())
					Log::GetLog()->info("blocked item skin apply: {} onto {}", name, ClassNameOfObject(destination));
				if (pc != nullptr && !g_settings.block_skin_apply_message.empty())
					AsaApi::GetApiUtils().SendServerMessage(pc, FColorList::Yellow, "{}", g_settings.block_skin_apply_message);
				return false;
			}
		}
		catch (...)
		{
		}
	}
	return UPrimalItem_UseItemOntoItem_original(skin_item, destination, additional_data, pc);
}

// Items rebuilt from the save (a character carrying a skinned item across a relog). Probe only for now: logs
// skinned items with the weapon template they will spawn, then calls the original unchanged.
void Hook_UPrimalItem_InventoryLoadedFromSaveGame(UPrimalItem* item)
{
	UPrimalItem_InventoryLoadedFromSaveGame_original(item);
	if (!g_alive || !(g_settings.probe_item_entry || g_settings.strip_items_on_load) || item == nullptr)
		return;
	try
	{
		UClass* skin = item->ItemSkinTemplateField().uClass;
		const __int64 mod_id_before = item->CustomCosmeticModSkinReplacementIDField();
		if (skin == nullptr)
		{
			// A mod cosmetic is carried as a mod id on the item, with no skin class.
			if (mod_id_before <= 0)
				return;
			++g_entry.loaded_mod;
			AActor* mod_owner = OwnerActor(item);
			const int mod_team = mod_owner != nullptr ? mod_owner->TargetingTeamField() : 0;
			const int variant_before = item->CustomCosmeticModSkinVariantIDField();
			bool mod_cleared = false;
			if (ShouldStripItemModCosmeticOnLoad(g_settings, mod_id_before, mod_team))
			{
				ItemBackupEntry record;
				record.kind = "load";
				record.item_id1 = item->ItemIDField().ItemID1;
				record.item_id2 = item->ItemIDField().ItemID2;
				record.item_class = ClassNameOfObject(item);
				record.skin_class_name = "(null)";
				record.mod_id = mod_id_before;
				record.variant = variant_before;
				record.owner_class = mod_owner != nullptr ? ClassNameOfObject(mod_owner) : "";
				record.owner_team = mod_team;
				if (WriteItemBackup(record))
				{
					item->CustomCosmeticModSkinReplacementIDField() = 0;
					item->CustomCosmeticModSkinVariantIDField() = 0;
					mod_cleared = true;
					++g_entry.loaded_mod_stripped;
				}
			}
			if (g_budget.Take())
				Log::GetLog()->info("item loaded from save (mod cosmetic): {} owner={} team={} mod={} variant={} cleared={} after: mod={}", ClassNameOfObject(item),
					mod_owner != nullptr ? ClassNameOfObject(mod_owner) : "(none)", mod_team, mod_id_before, variant_before,
					mod_cleared ? "yes" : "no", item->CustomCosmeticModSkinReplacementIDField());
			return;
		}
		++g_entry.loaded_skin;
		const std::string skin_name = ClassName(skin);
		const std::string before = WeaponTemplateName(item);
		bool removed = false;
		AActor* owner = OwnerActor(item);
		const int owner_team = owner != nullptr ? owner->TargetingTeamField() : 0;
		if (ShouldStripItemOnLoad(g_settings, skin_name, owner_team))
		{
			ItemBackupEntry record;
			record.kind = "load";
			record.item_id1 = item->ItemIDField().ItemID1;
			record.item_id2 = item->ItemIDField().ItemID2;
			record.item_class = ClassNameOfObject(item);
			record.skin_class_name = skin_name;
			record.skin_class_path = ClassPath(skin);
			record.mod_id = item->CustomCosmeticModSkinReplacementIDField();
			record.variant = item->CustomCosmeticModSkinVariantIDField();
			record.owner_class = owner != nullptr ? ClassNameOfObject(owner) : "";
			record.owner_team = owner_team;
			if (WriteItemBackup(record))
			{
				// The game's own removal (UPrimalItem::ServerRemoveItemSkinOnly). The AsaApi header wrapper adds a stray
				// argument, so the call is made here with none.
				NativeCall<void>(item, "UPrimalItem.ServerRemoveItemSkinOnly()");
				// The removal can leave a mod cosmetic id on the item (seen in memory until a save and reload).
				if (ShouldStripItemModCosmeticOnLoad(g_settings, mod_id_before, owner_team))
				{
					item->CustomCosmeticModSkinReplacementIDField() = 0;
					item->CustomCosmeticModSkinVariantIDField() = 0;
				}
				removed = true;
				++g_entry.loaded_stripped;
			}
		}
		if (g_budget.Take())
			Log::GetLog()->info("item loaded from save: {} owner={} team={} skin={} weaponTemplate={} removed={} after: skin={} weaponTemplate={}", ClassNameOfObject(item), owner != nullptr ? ClassNameOfObject(owner) : "(none)", owner_team,
				skin_name, before, removed ? "yes" : "no", removed ? ClassName(item->ItemSkinTemplateField().uClass) : "-",
				removed ? WeaponTemplateName(item) : "-");
	}
	catch (...)
	{
	}
}

// ----------------------------------------------------------------------------------------------
// Structure channel probe (ProbeStructureChannels)
// ----------------------------------------------------------------------------------------------
// UActorChannel::ReplicateActor() replicates one actor to one connection. This is the hook a platform-aware mode
// would use to blank a structure's skin for console connections only; for now it only counts which connection
// platforms receive skinned structures, and how long the call takes. It runs for every replicated actor on every
// connection, so it exits early and reads fields through offsets looked up once (GetNativePointerField looks the
// name up on every call).

const char* const kHookRequestItems = "UPrimalInventoryComponent.ServerRequestItems(AShooterPlayerController*,bool,bool)";
const char* const kHookAddActorItem = "AShooterPlayerController.ClientAddActorItem(UPrimalInventoryComponent*,FItemNetInfo,bool,bool,bool)";
const char* const kHookReplicateActor = "UActorChannel.ReplicateActor()";
DECLARE_HOOK(UActorChannel_ReplicateActor, __int64, UActorChannel*);
// Per-connection send of a remote function (RPC). Unknown types are plain pointers: only the layout matters.
// Signature copied from the offsets cache (AsaApiCache98.9); a missing key terminates the server.
const char* const kHookRemoteFunction = "UNetDriver.ProcessRemoteFunctionForChannelPrivate(UActorChannel*,FClassNetCache*,FFieldNetCache*,UObject*,UNetConnection*,UFunction*,void*,FOutParmRec*,FFrame*,bool,UNetDriver::ERemoteFunctionSendPolicy,EProcessRemoteFunctionFlags&)";
DECLARE_HOOK(UNetDriver_ProcessRemoteFunctionForChannelPrivate, void, void*, UActorChannel*, void*, void*, UObject*, UNetConnection*, UObject*, void*, void*, void*, bool, int, unsigned int&);

namespace
{
	bool g_channel_hook = false;
	std::ptrdiff_t g_off_channel_actor = -1, g_off_channel_connection = -1, g_off_connection_platform = -1;
	std::ptrdiff_t g_off_object_class = -1, g_off_skin_class = -1, g_off_skin_id = -1, g_off_skin_variant = -1;
	std::unordered_map<UClass*, bool> g_structure_class_cache;   // actor class -> is a structure
	std::unordered_map<UClass*, bool> g_weapon_class_cache;      // actor class -> is a held weapon (AShooterWeapon)
	UClass* g_weapon_class = nullptr;
	std::unordered_set<unsigned long long> g_weapon_logged;   // weapon/connection pairs already logged (log once each)

	// EXPERIMENTAL chunked skins for PC connections (DeferStructureSkins_Seconds, ReleaseStructureSkinsPerSecond): every
	// skinned structure a PC connection receives goes out plain first and is queued for that connection. After the initial
	// delay the queue is released a few per second: the structure is marked released for that connection and flushed, so
	// it replicates again and this time carries its skin. Structures are tracked by StructureID (no raw actor pointers).
	struct DeferState
	{
		long long first_clock = 0;     // g_clock when the connection first replicated a skinned structure
		long long last_clock = 0;      // g_clock of its latest skinned structure replication
		long long deferred = 0;        // skinned structure sends that went out plain
		long long released = 0;        // structures released so far
		long long flush_us = 0;        // time spent flushing for this connection
		long long max_tick_us = 0;     // longest single tick of flushing
		long long start_clock = 0;     // g_clock when releasing started (0 = not started)
		bool reported = false;         // "queue drained" logged
		std::vector<unsigned int> queue;
		size_t next = 0;
		std::unordered_set<unsigned int> queued;
		std::unordered_set<unsigned int> released_ids;
	};
	std::unordered_map<UNetConnection*, DeferState> g_defer;
	UClass* g_structure_class = nullptr;

	std::ptrdiff_t FieldOffset(void* base, const char* name)
	{
		return reinterpret_cast<char*>(GetNativePointerField<char*>(base, name)) - reinterpret_cast<char*>(base);
	}

	template <typename T>
	void WriteAt(void* base, std::ptrdiff_t offset, T value)
	{
		*reinterpret_cast<T*>(reinterpret_cast<char*>(base) + offset) = value;
	}

	template <typename T>
	T ReadAt(void* base, std::ptrdiff_t offset)
	{
		return *reinterpret_cast<T*>(reinterpret_cast<char*>(base) + offset);
	}

	// Platform of a connection, plus the TreatAsConsolePlayers test override. Cached; not cached while the EOS ID
	// cannot be read yet (the player controller is set up after the connection opens).
	ConnectionInfo InfoFor(UNetConnection* connection)
	{
		const auto found = g_connection_info.find(connection);
		if (found != g_connection_info.end())
			return found->second;
		ConnectionInfo info;
		if (g_settings.read_connection_platform)
		{
			if (g_off_connection_platform < 0)
				g_off_connection_platform = FieldOffset(connection, "UNetConnection.ConnectionPlatform");
			info.platform = ReadAt<uint8_t>(connection, g_off_connection_platform);
		}
		if (!g_settings.treat_as_console_players.empty() || (g_settings.enable_player_hide_commands && !g_optin.Empty()))
		{
			AActor* owner = *GetNativePointerField<AActor**>(connection, "UNetConnection.OwningActor");
			if (owner == nullptr || !owner->IsA(AShooterPlayerController::GetPrivateStaticClass()))
				return info;
			const std::string eos_id = AsaApi::GetApiUtils().GetEOSIDFromController(static_cast<AShooterPlayerController*>(owner)).ToString();
			if (eos_id.empty())
				return info;
			info.opted_in = g_settings.enable_player_hide_commands && g_optin.Contains(eos_id);
			info.test_console = IsTreatedAsConsole(g_settings, g_optin, eos_id);
			if (info.test_console)
				Log::GetLog()->info("{}: {} (connection platform {}) is handled as a console player",
					info.opted_in ? "/hideskins" : "TreatAsConsolePlayers", eos_id, ConnectionPlatformLabel(info.platform));
		}
		g_connection_info[connection] = info;
		return info;
	}

	std::string ConnectionLabel(const ConnectionInfo& info)
	{
		return info.test_console ? ConnectionPlatformLabel(info.platform) + (info.opted_in ? " as console (opt-in)" : " as console (test)")
			: ConnectionPlatformLabel(info.platform);
	}
}

// A single item sent to a client on its own (picked up, moved, crafted) does not pass through ServerRequestItems. For a console
// viewer the copy being sent has its skin blanked and custom data cleared; the server item keeps its skin.
void Hook_AShooterPlayerController_ClientAddActorItem(AShooterPlayerController* pc, UPrimalInventoryComponent* inventory, FItemNetInfo& info, bool a, bool b,
	bool c)
{
	if (g_alive && g_settings.block_item_skins_for_consoles && pc != nullptr && std::this_thread::get_id() == g_game_thread)
	{
		try
		{
			UNetConnection* connection = *GetNativePointerField<UNetConnection**>(pc, "APlayerController.NetConnection");
			bool console = true;
			std::string viewer = "no connection";
			if (connection != nullptr)
			{
				const ConnectionInfo ci = InfoFor(connection);
				viewer = ConnectionLabel(ci);
				console = EffectiveGroup(ci.platform, ci.test_console) != PlatformGroup::PC;
			}
			const long long net_mod_id = NetModSkinId(&info);
			if (console && (info.ItemSkinTemplateField().uClass != nullptr || net_mod_id != 0))
			{
				const std::string skin = ClassName(info.ItemSkinTemplateField().uClass) + (net_mod_id != 0 ? " mod " + std::to_string(net_mod_id) : "");
				info.ItemSkinTemplateField().uClass = nullptr;
				if (net_mod_id != 0)
				{
					NetModSkinId(&info) = 0;
					NetModSkinVariant(&info) = 0;
					++g_counters.item_mod_ids_blanked_for_console;
				}
				++g_counters.items_blanked_for_console;
				if (g_settings.filter_item_custom_data && info.CustomItemDatasField().Num() > 0)
				{
					info.CustomItemDatasField().Reset();
					++g_counters.items_custom_data_cleared;
				}
				if (g_budget.Take())
					Log::GetLog()->info("single item to {}: skin {} blanked", viewer, skin);
			}
		}
		catch (...)
		{
		}
	}
	AShooterPlayerController_ClientAddActorItem_original(pc, inventory, info, a, b, c);
}

// Log-only (ProbeUnlockedCosmetics): once per controller per session, lists the skin items in the player's own inventory
// when it is first requested, with the game's unlocked-cosmetic flag. Used to find where unlocked skins are kept.
void ProbeOwnedCosmetics(UPrimalInventoryComponent* inventory, AShooterPlayerController* for_pc)
{
	static std::unordered_set<AShooterPlayerController*> seen;
	if (for_pc == nullptr || inventory == nullptr)
		return;
	try
	{
		AActor* owner = inventory->OwnerPrivateField();
		if (owner == nullptr || owner != static_cast<AActor*>(for_pc->GetPlayerCharacter()) || !seen.insert(for_pc).second)
			return;
		const std::string eos_id = AsaApi::GetApiUtils().GetEOSIDFromController(for_pc).ToString();
		TArray<UPrimalItem*, TSizedDefaultAllocator<32>>& items = inventory->InventoryItemsField();
		int skins = 0, unlocked = 0, mods = 0;
		for (int i = 0; i < items.Num(); ++i)
		{
			UPrimalItem* item = items[i];
			if (item == nullptr || !(item->bIsItemSkin().Get() || ClassNameOfObject(item).find("PrimalItemSkin") != std::string::npos))
				continue;
			++skins;
			const bool is_unlocked = item->IsUnlockedCosmetic();
			if (is_unlocked)
				++unlocked;
			const __int64 mod_id = item->CustomCosmeticModSkinReplacementIDField();
			if (mod_id > 0)
				++mods;
			Log::GetLog()->info("owned cosmetics: {} item {} unlockedCosmetic={} modId={}", eos_id, ClassNameOfObject(item), is_unlocked ? "yes" : "no", mod_id);
		}
		Log::GetLog()->info("owned cosmetics: {} total {} items, {} skin items ({} flagged unlocked, {} mod)", eos_id, items.Num(), skins, unlocked, mods);
	}
	catch (...)
	{
	}
}

void Hook_UPrimalInventoryComponent_ServerRequestItems(UPrimalInventoryComponent* inventory, AShooterPlayerController* for_pc, bool equipped, bool first_spawn)
{
	if (g_alive && g_settings.probe_unlocked_cosmetics && std::this_thread::get_id() == g_game_thread)
		ProbeOwnedCosmetics(inventory, for_pc);
	if (!g_alive || !(g_settings.probe_item_requests || g_settings.block_item_skins_for_consoles || g_settings.unequip_skin_items_for_consoles) || g_request.active
		|| std::this_thread::get_id() != g_game_thread)
	{
		UPrimalInventoryComponent_ServerRequestItems_original(inventory, for_pc, equipped, first_spawn);
		return;
	}

	std::string viewer = "no controller";
	bool console = true;
	try
	{
		if (for_pc != nullptr)
		{
			UNetConnection* connection = *GetNativePointerField<UNetConnection**>(for_pc, "APlayerController.NetConnection");
			if (connection != nullptr)
			{
				const ConnectionInfo info = InfoFor(connection);
				viewer = ConnectionLabel(info);
				console = EffectiveGroup(info.platform, info.test_console) != PlatformGroup::PC;
			}
			else
				viewer = "no connection";
		}
	}
	catch (...)
	{
	}

	g_request = ItemRequest{};
	g_request.active = true;
	g_request.equipped = equipped;
	g_request.console = console;

	// Experimental: skin items worn as cosmetics arrive with their equipped flag off, so they are told apart only by being in
	// the equipped list. For a console viewer's equipped request they are taken out of that list while the original builds the
	// request, then put back at once. Nothing else reads the list in between: this runs on the game thread inside one call.
	std::vector<UPrimalItem*> saved_equipped;
	bool equipped_trimmed = false;
	if (g_settings.unequip_skin_items_for_consoles && equipped && console && inventory != nullptr)
	{
		try
		{
			TArray<UPrimalItem*, TSizedDefaultAllocator<32>>& list = inventory->EquippedItemsField();
			for (int i = 0; i < list.Num(); ++i)
				saved_equipped.push_back(list[i]);
			for (int i = list.Num() - 1; i >= 0; --i)
			{
				UPrimalItem* entry = list[i];
				if (entry != nullptr && entry->bIsItemSkin().Get())
				{
					g_counters.unequipped_skin_classes.Add(ClassNameOfObject(entry));
					++g_counters.skin_items_unequipped;
					list.RemoveAt(i, 1, false);
					equipped_trimmed = true;
				}
			}
		}
		catch (...)
		{
		}
	}
	UPrimalInventoryComponent_ServerRequestItems_original(inventory, for_pc, equipped, first_spawn);
	if (equipped_trimmed)
	{
		try
		{
			TArray<UPrimalItem*, TSizedDefaultAllocator<32>>& list = inventory->EquippedItemsField();
			list.Reset();
			for (UPrimalItem* entry : saved_equipped)
				list.Add(entry);
		}
		catch (...)
		{
		}
	}
	const ItemRequest done = g_request;
	g_request = ItemRequest{};

	++g_counters.item_requests;
	if (equipped)
		++g_counters.item_requests_equipped;
	g_counters.request_items += done.items;
	g_counters.request_items_skinned += done.skinned;
	g_counters.request_viewers.Add(viewer + (equipped ? " equipped" : " inventory") + (done.skinned > 0 ? " skinned" : " plain")
		+ (done.blanked > 0 ? " blanked" : ""));
	if (g_settings.probe_item_requests && (done.skinned > 0 || equipped) && g_budget.Take())
	{
		std::string owner = "?";
		try
		{
			if (inventory != nullptr)
				owner = ClassNameOfObject(inventory->OwnerPrivateField());
		}
		catch (...)
		{
		}
		Log::GetLog()->info("item request: viewer={} owner={} equipped={} firstSpawn={} items={} skinned={} blanked={}", viewer, owner, equipped ? "yes" : "no",
			first_spawn ? "yes" : "no", done.items, done.skinned, done.blanked);
	}
}

bool g_request_hook = false;
bool g_add_item_hook = false;

__int64 Hook_UActorChannel_ReplicateActor(UActorChannel* channel)
{
	if (!g_alive || !(g_settings.probe_structure_channels || g_settings.block_structure_skins_for_consoles || g_settings.block_item_skins_for_consoles
		|| g_settings.defer_structure_skins_seconds > 0)
		|| channel == nullptr)
		return UActorChannel_ReplicateActor_original(channel);
	if (std::this_thread::get_id() != g_game_thread)
	{
		++g_channels_off_thread;
		return UActorChannel_ReplicateActor_original(channel);
	}

	++g_channels.calls;
	bool skinned = false;
	ConnectionInfo info;
	uintptr_t pair_connection = 0, pair_actor = 0;
	AActor* blank_actor = nullptr;
	UClass* saved_class = nullptr;
	__int64 saved_id = 0;
	int saved_variant = 0;
	FItemNetInfo* weapon_info = nullptr;
	UClass* weapon_saved_skin = nullptr;
	long long weapon_saved_mod = 0;
	int weapon_saved_variant = 0;
	try
	{
		if (g_off_channel_actor < 0)
		{
			g_off_channel_actor = FieldOffset(channel, "UActorChannel.Actor");
			g_off_channel_connection = FieldOffset(channel, "UChannel.Connection");
		}
		AActor* actor = ReadAt<AActor*>(channel, g_off_channel_actor);
		if (actor != nullptr)
		{
			if (g_off_object_class < 0)
				g_off_object_class = FieldOffset(actor, "UObjectBase.ClassPrivate");
			UClass* cls = ReadAt<UClass*>(actor, g_off_object_class);
			bool is_structure = false;
			const auto found = g_structure_class_cache.find(cls);
			if (found != g_structure_class_cache.end())
				is_structure = found->second;
			else
			{
				if (g_structure_class == nullptr)
					g_structure_class = APrimalStructure::GetPrivateStaticClass();
				is_structure = cls != nullptr && g_structure_class != nullptr && actor->IsA(g_structure_class);
				g_structure_class_cache[cls] = is_structure;
			}
			// A held weapon carries a copy of its item's net info, replicated with the weapon actor. For a console connection
			// the skin and mod id in that copy are blanked for this connection only, then put back.
			if (!is_structure && g_settings.block_item_skins_for_consoles)
			{
				bool is_weapon = false;
				const auto wfound = g_weapon_class_cache.find(cls);
				if (wfound != g_weapon_class_cache.end())
					is_weapon = wfound->second;
				else
				{
					if (g_weapon_class == nullptr)
						g_weapon_class = AShooterWeapon::GetPrivateStaticClass();
					is_weapon = cls != nullptr && g_weapon_class != nullptr && actor->IsA(g_weapon_class);
					g_weapon_class_cache[cls] = is_weapon;
				}
				if (is_weapon)
				{
					FItemNetInfo* winfo = &static_cast<AShooterWeapon*>(actor)->AssociatedItemNetInfoField();
					UClass* wskin = winfo->ItemSkinTemplateField().uClass;
					const long long wmod = NetModSkinId(winfo);
					if (wskin != nullptr || wmod != 0)
					{
						UNetConnection* connection = ReadAt<UNetConnection*>(channel, g_off_channel_connection);
						ConnectionInfo winfo_conn;
						if (connection != nullptr)
							winfo_conn = InfoFor(connection);
						if (EffectiveGroup(winfo_conn.platform, winfo_conn.test_console) != PlatformGroup::PC)
						{
							weapon_info = winfo;
							weapon_saved_skin = wskin;
							weapon_saved_mod = wmod;
							weapon_saved_variant = NetModSkinVariant(winfo);
							winfo->ItemSkinTemplateField().uClass = nullptr;
							NetModSkinId(winfo) = 0;
							NetModSkinVariant(winfo) = 0;
							++g_counters.weapon_skins_blanked_for_console;
							const unsigned long long wkey = (static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(connection)) * 1000003ULL)
								^ static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(actor));
							if (g_weapon_logged.size() > 4096)
								g_weapon_logged.clear();
							if (g_weapon_logged.insert(wkey).second && g_budget.Take())
								Log::GetLog()->info("held weapon: {} skin={} mod={} blanked for {}", ClassNameOfObject(actor), ClassName(wskin), wmod,
									ConnectionLabel(winfo_conn));
						}
					}
				}
			}
			if (is_structure)
			{
				++g_channels.structures;
				if (g_off_skin_class < 0)
				{
					g_off_skin_class = FieldOffset(actor, kReplicatedSkinClassField);
					g_off_skin_id = FieldOffset(actor, "APrimalStructure.MyCustomCosmeticStructureSkinID");
					g_off_skin_variant = FieldOffset(actor, "APrimalStructure.MyCustomCosmeticStructureSkinVariantID");
				}
				UClass* skin = ReadAt<UClass*>(actor, g_off_skin_class);
				const __int64 skin_id = ReadAt<__int64>(actor, g_off_skin_id);
				skinned = skin != nullptr || skin_id != 0;
				if (skinned)
				{
					UNetConnection* connection = ReadAt<UNetConnection*>(channel, g_off_channel_connection);
					if (connection != nullptr)
						info = InfoFor(connection);
					pair_connection = reinterpret_cast<uintptr_t>(connection);
					pair_actor = reinterpret_cast<uintptr_t>(actor);
					const bool is_console = EffectiveGroup(info.platform, info.test_console) != PlatformGroup::PC;
					bool defer = false;
					if (!is_console && g_settings.defer_structure_skins_seconds > 0 && connection != nullptr)
					{
						DeferState& st = g_defer[connection];
						if (st.first_clock == 0)
							st.first_clock = g_clock;
						st.last_clock = g_clock;
						const unsigned int sid = static_cast<APrimalStructure*>(actor)->StructureIDField();
						if (st.released_ids.find(sid) == st.released_ids.end())
						{
							defer = true;
							++st.deferred;
							if (st.queued.insert(sid).second)
							{
								st.queue.push_back(sid);
								st.reported = false;
							}
						}
					}
					if ((g_settings.block_structure_skins_for_consoles && is_console) || defer)
					{
						// Blank the skin fields for this connection only; the other connections replicate the real values.
						blank_actor = actor;
						saved_class = skin;
						saved_id = skin_id;
						saved_variant = ReadAt<int>(actor, g_off_skin_variant);
						WriteAt<UClass*>(actor, g_off_skin_class, nullptr);
						WriteAt<__int64>(actor, g_off_skin_id, 0);
						WriteAt<int>(actor, g_off_skin_variant, 0);
					}
					if (g_settings.probe_structure_channels && g_budget.Take())
						Log::GetLog()->info("structure channel: {} skin={} id={} connection={}", ClassNameOfObject(actor), ClassName(skin), skin_id,
							ConnectionLabel(info));
				}
			}
		}
	}
	catch (...)
	{
		skinned = false;
	}

	const auto start = std::chrono::steady_clock::now();
	const __int64 result = UActorChannel_ReplicateActor_original(channel);
	if (weapon_info != nullptr)
	{
		weapon_info->ItemSkinTemplateField().uClass = weapon_saved_skin;
		NetModSkinId(weapon_info) = weapon_saved_mod;
		NetModSkinVariant(weapon_info) = weapon_saved_variant;
	}
	if (blank_actor != nullptr)
	{
		WriteAt<UClass*>(blank_actor, g_off_skin_class, saved_class);
		WriteAt<__int64>(blank_actor, g_off_skin_id, saved_id);
		WriteAt<int>(blank_actor, g_off_skin_variant, saved_variant);
		++g_channels.blanked;
	}
	const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
	g_channels.ns_all += ns;
	if (skinned)
	{
		++g_channels.skinned;
		g_channels.ns_skinned += ns;
		switch (EffectiveGroup(info.platform, info.test_console))
		{
		case PlatformGroup::PC: ++g_channels.skinned_pc; break;
		case PlatformGroup::Console: ++g_channels.skinned_console; break;
		default: ++g_channels.skinned_unknown; break;
		}
		++g_channels.platforms[ConnectionLabel(info)];
		g_channel_pairs.insert((static_cast<unsigned long long>(pair_connection) * 1000003ULL) ^ static_cast<unsigned long long>(pair_actor));
	}
	return result;
}

// The skin change multicasts (SetStructureSkin sends MultiRefreshStructureSkin) carry the class, id and variant as
// arguments, so the ReplicateActor blanking never sees them. They are not sent to a console connection at all: the
// console already holds the blanked values and has nothing to refresh. PC connections are untouched.
// Probe: name of a remote function if it mentions items, skins, cosmetics or equipment, else empty. Cached by pointer.
const std::string& InterestingRpcName(UObject* function)
{
	static std::unordered_map<UObject*, std::string> cache;   // game thread only
	const auto found = cache.find(function);
	if (found != cache.end())
		return found->second;
	std::string name = function->NameField().ToString().ToString();
	std::string lower = name;
	for (char& ch : lower)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	const bool match = lower.find("item") != std::string::npos || lower.find("skin") != std::string::npos
		|| lower.find("cosmetic") != std::string::npos || lower.find("equip") != std::string::npos || lower.find("costume") != std::string::npos;
	return cache[function] = match ? name : std::string();
}

bool IsSkinMulticast(UObject* function)
{
	static std::unordered_map<UObject*, bool> cache;   // game thread only
	const auto found = cache.find(function);
	if (found != cache.end())
		return found->second;
	const std::string name = function->NameField().ToString().ToString();
	const bool match = name == "MultiRefreshStructureSkin" || name == "MultiHandleSkinNetMessage";
	cache[function] = match;
	return match;
}

void Hook_UNetDriver_ProcessRemoteFunctionForChannelPrivate(void* driver, UActorChannel* channel, void* class_cache, void* field_cache, UObject* target,
	UNetConnection* connection, UObject* function, void* parms, void* out_parms, void* stack, bool is_server, int send_policy, unsigned int& flags)
{
	if (g_alive && g_settings.probe_client_rpcs && function != nullptr && connection != nullptr && std::this_thread::get_id() == g_game_thread)
	{
		const std::string& name = InterestingRpcName(function);
		if (!name.empty())
		{
			const ConnectionInfo info = InfoFor(connection);
			if (EffectiveGroup(info.platform, info.test_console) != PlatformGroup::PC)
				g_counters.console_rpcs.Add(name + " on " + ClassNameOfObject(target));
		}
	}
	if (g_alive && g_settings.block_structure_skins_for_consoles && function != nullptr && connection != nullptr
		&& std::this_thread::get_id() == g_game_thread && IsSkinMulticast(function))
	{
		const ConnectionInfo info = InfoFor(connection);
		if (EffectiveGroup(info.platform, info.test_console) != PlatformGroup::PC)
		{
			++g_counters.skin_rpc_withheld;
			if (g_budget.Take())
				Log::GetLog()->info("skin multicast {} withheld from {} on {}", function->NameField().ToString().ToString(), ConnectionLabel(info),
					ClassNameOfObject(target));
			return;
		}
	}
	UNetDriver_ProcessRemoteFunctionForChannelPrivate_original(driver, channel, class_cache, field_cache, target, connection, function, parms, out_parms, stack,
		is_server, send_policy, flags);
}

namespace
{
	// Runs every second from OnTimer. For each PC connection past its initial delay, releases up to
	// ReleaseStructureSkinsPerSecond queued structures: marks them released for that connection and flushes them so they
	// replicate again with the skin. Other connections are unaffected (not released for them = still plain or blocked).
	void DeferTick()
	{
		if (!g_alive || g_settings.defer_structure_skins_seconds <= 0 || g_defer.empty())
			return;
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (world == nullptr)
			return;
		for (auto it = g_defer.begin(); it != g_defer.end();)
		{
			DeferState& st = it->second;
			// Forget a connection with no skinned replication for an hour (disconnected, or its address reused later).
			if (g_clock - st.last_clock > 3600)
			{
				it = g_defer.erase(it);
				continue;
			}
			if (g_clock - st.first_clock >= g_settings.defer_structure_skins_seconds && st.next < st.queue.size())
			{
				if (st.start_clock == 0)
				{
					st.start_clock = g_clock;
					Log::GetLog()->info("defer: releasing skins for a PC connection after {} s ({} structures queued, {} per second)",
						g_settings.defer_structure_skins_seconds, st.queue.size() - st.next, g_settings.release_structure_skins_per_second);
				}
				const auto start = std::chrono::steady_clock::now();
				int budget = g_settings.release_structure_skins_per_second;
				try
				{
					while (budget > 0 && st.next < st.queue.size())
					{
						const unsigned int sid = st.queue[st.next++];
						st.queued.erase(sid);
						st.released_ids.insert(sid);
						APrimalStructure* structure = APrimalStructure::GetFromID(world, sid);
						if (structure == nullptr)
							continue;   // destroyed since: nothing to send
						structure->FlushNetDormancy();
						structure->ForceNetUpdate(false, false, false);
						++st.released;
						--budget;
					}
				}
				catch (...)
				{
				}
				const long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
				st.flush_us += us;
				st.max_tick_us = (std::max)(st.max_tick_us, us);
				if (st.next >= st.queue.size())
				{
					st.queue.clear();
					st.next = 0;
				}
			}
			if (st.start_clock != 0 && st.queue.empty() && !st.reported)
			{
				st.reported = true;
				Log::GetLog()->info("defer: queue drained for a PC connection: {} released over {} s, {} plain sends, flush time {} us total, {} us longest tick",
					st.released, g_clock - st.start_clock, st.deferred, st.flush_us, st.max_tick_us);
			}
			++it;
		}
	}
}

bool g_rpc_hook = false;

void EnsureChannelHook()
{
	if (!g_request_hook && (g_settings.probe_item_requests || g_settings.block_item_skins_for_consoles || g_settings.unequip_skin_items_for_consoles || g_settings.probe_unlocked_cosmetics))
	{
		g_request_hook = AsaApi::GetHooks().SetHook(kHookRequestItems, &Hook_UPrimalInventoryComponent_ServerRequestItems,
			&UPrimalInventoryComponent_ServerRequestItems_original);
		Log::GetLog()->info("Hook (item request filter/probe): ServerRequestItems {}", g_request_hook ? "ok" : "FAILED");
	}
	if (!g_add_item_hook && g_settings.block_item_skins_for_consoles)
	{
		g_add_item_hook = AsaApi::GetHooks().SetHook(kHookAddActorItem, &Hook_AShooterPlayerController_ClientAddActorItem,
			&AShooterPlayerController_ClientAddActorItem_original);
		Log::GetLog()->info("Hook (single item filter): ClientAddActorItem {}", g_add_item_hook ? "ok" : "FAILED");
	}
	if (!g_channel_hook && (g_settings.probe_structure_channels || g_settings.block_structure_skins_for_consoles || g_settings.block_item_skins_for_consoles
		|| g_settings.defer_structure_skins_seconds > 0))
	{
		g_channel_hook = AsaApi::GetHooks().SetHook(kHookReplicateActor, &Hook_UActorChannel_ReplicateActor, &UActorChannel_ReplicateActor_original);
		Log::GetLog()->info("Hook (structure channel probe): ReplicateActor {}", g_channel_hook ? "ok" : "FAILED");
	}
	if (!g_rpc_hook && (g_settings.block_structure_skins_for_consoles || g_settings.probe_client_rpcs))
	{
		g_rpc_hook = AsaApi::GetHooks().SetHook(kHookRemoteFunction, &Hook_UNetDriver_ProcessRemoteFunctionForChannelPrivate,
			&UNetDriver_ProcessRemoteFunctionForChannelPrivate_original);
		Log::GetLog()->info("Hook (skin multicast filter): ProcessRemoteFunctionForChannelPrivate {}", g_rpc_hook ? "ok" : "FAILED");
	}
}

// ----------------------------------------------------------------------------------------------
// Plugin entry points
// ----------------------------------------------------------------------------------------------

extern "C" __declspec(dllexport) void Plugin_Init()
{
	Log::Get().Init(PROJECT_NAME);

	std::string error;
	if (!ReloadSettings(error))
	{
		Log::GetLog()->error("Config not loaded ({}); using defaults. Fix config.json and run {}", error, kReloadCommand);
		g_budget.Reset(g_settings.max_detail_lines_per_summary);
	}

	auto& hooks = AsaApi::GetHooks();
	const bool ok_item = hooks.SetHook(kHookItemNetInfo, &Hook_UPrimalItem_GetItemNetInfo, &UPrimalItem_GetItemNetInfo_original);
	const bool ok_char = hooks.SetHook(kHookCharacterPreRep, &Hook_AShooterCharacter_PreReplication, &AShooterCharacter_PreReplication_original);
	const bool ok_skin = hooks.SetHook(kHookSkinMessage, &Hook_APrimalStructure_MultiHandleSkinNetMessage,
		&APrimalStructure_MultiHandleSkinNetMessage_original);
	const bool ok_apply = hooks.SetHook(kSetSkinSymbol, &Hook_APrimalStructure_SetStructureSkin, &APrimalStructure_SetStructureSkin_original);
	const bool ok_refresh = hooks.SetHook(kHookSkinRefresh, &Hook_APrimalStructure_MultiRefreshStructureSkin, &APrimalStructure_MultiRefreshStructureSkin_original);
	Log::GetLog()->info("Hook (skin refresh multicast probe): {}", ok_refresh ? "ok" : "FAILED");
	Log::GetLog()->info("Hooks: item net info {}, character pre-replication {}, structure skin message {}, skin apply guard {}",
		ok_item ? "ok" : "FAILED", ok_char ? "ok" : "FAILED", ok_skin ? "ok" : "FAILED", ok_apply ? "ok" : "FAILED");
	const bool ok_add = hooks.SetHook(kHookAddItem, &Hook_UPrimalInventoryComponent_AddItem, &UPrimalInventoryComponent_AddItem_original);
	const bool ok_notify = hooks.SetHook(kHookNotifyItemAdded, &Hook_UPrimalInventoryComponent_NotifyItemAdded, &UPrimalInventoryComponent_NotifyItemAdded_original);
	const bool ok_init = hooks.SetHook(kHookInitFromNetInfo, &Hook_UPrimalItem_InitFromNetInfo, &UPrimalItem_InitFromNetInfo_original);
	const bool ok_new = hooks.SetHook(kHookAddNewItem, &Hook_UPrimalItem_AddNewItem, &UPrimalItem_AddNewItem_original);
	const bool ok_load = hooks.SetHook(kHookLoadedFromSave, &Hook_UPrimalItem_InventoryLoadedFromSaveGame, &UPrimalItem_InventoryLoadedFromSaveGame_original);
	Log::GetLog()->info("Hook (item load probe): InventoryLoadedFromSaveGame {}", ok_load ? "ok" : "FAILED");
	const bool ok_use = hooks.SetHook(kHookUseItemOntoItem, &Hook_UPrimalItem_UseItemOntoItem, &UPrimalItem_UseItemOntoItem_original);
	Log::GetLog()->info("Hook (item skin apply guard): UseItemOntoItem {}", ok_use ? "ok" : "FAILED");
	Log::GetLog()->info("Hooks (item entry probe): AddItem {}, NotifyItemAdded {}, InitFromNetInfo {}, AddNewItem {}", ok_add ? "ok" : "FAILED",
		ok_notify ? "ok" : "FAILED", ok_init ? "ok" : "FAILED", ok_new ? "ok" : "FAILED");

	AddBoth<&RunReload>(kReloadCommand);
	AddBoth<&RunScan>(kScanCommand);
	AddBoth<&RunStrip>(kStripCommand);
	AddBoth<&RunRestore>(kRestoreCommand);
	AddBoth<&RunStatus>(kStatusCommand);
	AddBoth<&RunStripTribe>(kStripTribeCommand);
	LoadPreferences();
	{
		auto& commands = AsaApi::GetCommands();
		if (g_settings.enable_player_hide_commands)
		{
			commands.AddChatCommand(FString(g_settings.hide_skins_command), &ChatHide);
			commands.AddChatCommand(FString(g_settings.show_skins_command), &ChatShow);
		}
		if (g_settings.enable_player_strip)
			commands.AddChatCommand(FString(g_settings.strip_skins_command), &ChatStrip);
		g_chat_registered = g_settings;   // remembered so unload removes exactly what was added
	}
	AsaApi::GetCommands().AddOnTimerCallback(kTimerId, &OnTimer);

	g_game_thread = std::this_thread::get_id();
	EnsureChannelHook();

	g_alive = true;
	Log::GetLog()->info("CosmeticsDisabler {} loaded: strip {}, filter items {}, strip items on entry {}, block skin apply {}, strip on map load {}",
		kPluginVersion, g_settings.enable_strip ? "ENABLED" : "off", g_settings.filter_items ? "ON" : "off", g_settings.strip_items_on_entry ? "ON" : "off",
		g_settings.block_skin_apply ? "ON" : "off", g_settings.strip_on_map_load ? "ON" : "off");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	g_alive = false;
	g_run = Run::Idle;
	CloseBackupFile();
	CloseItemBackup();

	AsaApi::GetCommands().RemoveOnTimerCallback(kTimerId);
	RemoveBoth(kReloadCommand);
	RemoveBoth(kScanCommand);
	RemoveBoth(kStripCommand);
	RemoveBoth(kRestoreCommand);
	RemoveBoth(kStatusCommand);
	RemoveBoth(kStripTribeCommand);
	{
		auto& commands = AsaApi::GetCommands();
		if (g_chat_registered.enable_player_hide_commands)
		{
			commands.RemoveChatCommand(FString(g_chat_registered.hide_skins_command));
			commands.RemoveChatCommand(FString(g_chat_registered.show_skins_command));
		}
		if (g_chat_registered.enable_player_strip)
			commands.RemoveChatCommand(FString(g_chat_registered.strip_skins_command));
	}
	g_scope_team = 0;
	g_scope_requester.clear();
	g_scope_admin = false;
	StopPreferenceDb();

	auto& hooks = AsaApi::GetHooks();
	hooks.DisableHook(kHookItemNetInfo, &Hook_UPrimalItem_GetItemNetInfo);
	hooks.DisableHook(kHookCharacterPreRep, &Hook_AShooterCharacter_PreReplication);
	hooks.DisableHook(kHookSkinMessage, &Hook_APrimalStructure_MultiHandleSkinNetMessage);
	hooks.DisableHook(kSetSkinSymbol, &Hook_APrimalStructure_SetStructureSkin);
	hooks.DisableHook(kHookSkinRefresh, &Hook_APrimalStructure_MultiRefreshStructureSkin);
	hooks.DisableHook(kHookAddItem, &Hook_UPrimalInventoryComponent_AddItem);
	hooks.DisableHook(kHookNotifyItemAdded, &Hook_UPrimalInventoryComponent_NotifyItemAdded);
	hooks.DisableHook(kHookInitFromNetInfo, &Hook_UPrimalItem_InitFromNetInfo);
	hooks.DisableHook(kHookAddNewItem, &Hook_UPrimalItem_AddNewItem);
	hooks.DisableHook(kHookLoadedFromSave, &Hook_UPrimalItem_InventoryLoadedFromSaveGame);
	hooks.DisableHook(kHookUseItemOntoItem, &Hook_UPrimalItem_UseItemOntoItem);
	if (g_channel_hook)
	{
		hooks.DisableHook(kHookReplicateActor, &Hook_UActorChannel_ReplicateActor);
		g_channel_hook = false;
	}
	if (g_rpc_hook)
	{
		hooks.DisableHook(kHookRemoteFunction, &Hook_UNetDriver_ProcessRemoteFunctionForChannelPrivate);
		g_rpc_hook = false;
	}
	if (g_add_item_hook)
	{
		hooks.DisableHook(kHookAddActorItem, &Hook_AShooterPlayerController_ClientAddActorItem);
		g_add_item_hook = false;
	}
	if (g_request_hook)
	{
		hooks.DisableHook(kHookRequestItems, &Hook_UPrimalInventoryComponent_ServerRequestItems);
		g_request_hook = false;
		g_request = ItemRequest{};
	}
	g_structure_class_cache.clear();
	g_weapon_class_cache.clear();
	g_weapon_logged.clear();
	g_defer.clear();
	g_weapon_class = nullptr;

	g_seen_characters.clear();
	g_counters.Clear();
}
