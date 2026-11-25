#pragma once

// Pure logic for CosmeticsDisabler: config parsing, skin classification, strip policy, backup lines,
// counters and formatting.
//
// This header deliberately has no dependency on the game or on server API game types (only on the
// nlohmann json.hpp that ships with AsaApi), so it can be compiled and unit-tested on any machine.
// See tests/logic_tests.cpp.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

namespace CosmeticsProbe
{
	inline std::string ToLower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	// ------------------------------------------------------------------------------------------
	// Settings
	// ------------------------------------------------------------------------------------------

	// Where /hideskins choices are kept: a file per server, or a MariaDB/MySQL table shared by a cluster.
	struct PreferenceDb
	{
		std::string host = "127.0.0.1";
		int port = 3306;
		std::string user;
		std::string password;
		std::string name = "cosmeticsdisabler";   // database (schema) name
		std::string table = "hidden_skins_players";
		bool ssl = false;                          // false = plain connection (local / LAN MariaDB without TLS)

		bool operator==(const PreferenceDb& o) const
		{
			return host == o.host && port == o.port && user == o.user && password == o.password && name == o.name && table == o.table && ssl == o.ssl;
		}
	};

	struct Settings
	{
		bool probe_items = false;                // hook the item -> client net info builder
		bool probe_item_entry = false;           // log skinned items entering an inventory (count only, changes nothing)
		bool probe_unlocked_cosmetics = false;   // log, once per login, the skin items in the player's own inventory (changes nothing)
		bool backup_stripped_items = true;       // write a line per item skin removed (items-<time>.jsonl) before removing it
		bool strip_items_on_load = false;        // remove the skin (game removal call) from items as they are rebuilt from the save
		bool strip_items_on_entry = false;       // blank the skin in item data before an item is built (restores, transfers, drops)
		bool probe_players = false;              // hook character pre-replication (hair/eyebrow/facial hair ids)
		bool probe_structure_messages = false;   // hook the structure skin multicast
		bool read_connection_platform = true;    // read UNetConnection::ConnectionPlatform (a byte) for each character's player
		bool block_structure_skins_for_consoles = true;  // blank structure skins on console connections only (ReplicateActor)
		bool probe_structure_channels = false;   // count skinned structures replicated per connection platform (ReplicateActor)
		bool block_item_skins_for_consoles = true;  // blank item skins in the item data sent to console viewers only (ServerRequestItems)
		bool unequip_skin_items_for_consoles = true;  // EXPERIMENTAL: equipped skin items (bIsItemSkin) are sent to console viewers as not equipped
		bool probe_client_rpcs = false;          // count remote functions sent to console connections whose name mentions items/skins/cosmetics/equipment (probe)
		bool probe_item_requests = false;        // hook UPrimalInventoryComponent::ServerRequestItems: who asks for items, equipped gear or not (probe only)
		std::vector<int> block_skins_for_platforms = {3, 4, 5}; // connection platform values that get no skins (3 Series X, 4 Series S, 5 PS5)
		bool block_skins_for_unknown_platforms = true;          // platform values that are not 1, 3, 4 or 5 (2, 6, others, unread) get no skins either
		std::vector<std::string> treat_as_console_players; // testing: EOS IDs whose connection counts as a console
		int summary_interval_seconds = 30;       // how often the counters are written to the log
		int max_detail_lines_per_summary = 50;   // cap on individual "seen one" log lines per summary window
		int scan_actors_per_tick = 20000;        // actors looked at per timer tick by Scan / Strip
		int defer_structure_skins_seconds = 0;  // EXPERIMENTAL: PC connections get skinned structures plain, released after this delay (0 = off)
		int release_structure_skins_per_second = 50; // queued structure skins released per second per PC connection

		bool enable_strip = false;               // Strip with "confirm" and Restore do nothing unless this is true
		std::string strip_mode = "All";          // "All" (custom + official skins) or "CustomOnly"
		std::vector<std::string> allow_skin_classes; // skin class names that are never stripped
		int strip_per_tick = 50;                 // structures changed per timer tick (one tick = one second)

		bool filter_items = false;               // blank item skins in the data sent to clients (server items are untouched)
		bool filter_item_custom_data = false;    // experimental: also clear custom item data on the client copy of filtered items
		bool block_skin_apply = false;           // refuse new structure skin applications that StripMode would strip
		bool strip_on_map_load = false;          // strip automatically once after the map has loaded (needs EnableStrip)
		int strip_on_map_load_delay_seconds = 120;
		int strip_repeat_minutes = 0;            // strip again every N minutes after the first run (0 = never)
		std::string block_skin_apply_message;    // told to the nearest player when a skin apply is refused ("" = silent)

		// Player chat commands (off unless enabled). Command names include the slash, as typed in chat.
		bool enable_player_hide_commands = false; // players opt themselves in and out of console-style skin hiding
		std::string hide_skins_command = "/hideskins";
		std::string show_skins_command = "/showskins";
		bool enable_player_strip = false;        // a player strips the skins of their own tribe's structures (report, then confirm)
		std::string strip_skins_command = "/stripskins";
		int player_strip_cooldown_minutes = 30;  // per tribe, after a confirmed strip
		bool preferences_in_database = false;    // PlayerPreferenceStorage: "File" (false) or "Database" (true)
		PreferenceDb db;
		int database_refresh_seconds = 30;       // how often the table is read again (picks up choices made on other servers)
	};

	// A database or table name used in SQL text without quoting tricks: letters, digits and underscores only.
	inline bool IsValidSqlName(const std::string& name)
	{
		if (name.empty() || name.size() > 64)
			return false;
		for (unsigned char c : name)
			if (!std::isalnum(c) && c != '_')
				return false;
		return true;
	}

	// Used when config.json cannot be read at load: the defaults, but with the console blocking ON, so a typo in the
	// config never quietly sends skins to consoles. Everything else (strips, player commands, probes) stays off.
	inline Settings SafeFallbackSettings()
	{
		Settings s;
		s.read_connection_platform = true;
		s.block_structure_skins_for_consoles = true;
		s.block_item_skins_for_consoles = true;
		s.probe_items = false;
		s.probe_players = false;
		s.probe_structure_messages = false;
		return s;
	}

	namespace detail
	{
		inline bool ReadBool(const nlohmann::json& root, const char* key, bool fallback)
		{
			if (!root.contains(key))
				return fallback;
			const auto& value = root.at(key);
			if (!value.is_boolean())
				throw std::runtime_error(std::string(key) + " must be true or false");
			return value.get<bool>();
		}

		inline int ReadInt(const nlohmann::json& root, const char* key, int fallback, int lo, int hi)
		{
			if (!root.contains(key))
				return fallback;
			const auto& value = root.at(key);
			if (!value.is_number_integer())
				throw std::runtime_error(std::string(key) + " must be a whole number");
			const long long raw = value.get<long long>();
			if (raw < lo || raw > hi)
				throw std::runtime_error(std::string(key) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
			return static_cast<int>(raw);
		}

		// A chat command: "/" followed by 1-30 letters, digits or underscores.
		inline bool IsValidChatCommand(const std::string& name)
		{
			if (name.size() < 2 || name.size() > 31 || name[0] != '/')
				return false;
			for (size_t i = 1; i < name.size(); ++i)
			{
				const unsigned char c = static_cast<unsigned char>(name[i]);
				if (!std::isalnum(c) && c != '_')
					return false;
			}
			return true;
		}

		inline void ReadText(const nlohmann::json& root, const char* key, std::string& out, size_t max_len)
		{
			if (!root.contains(key))
				return;
			const auto& value = root.at(key);
			if (!value.is_string())
				throw std::runtime_error(std::string("Database.") + key + " must be text");
			if (value.get<std::string>().size() > max_len)
				throw std::runtime_error(std::string("Database.") + key + " is too long");
			out = value.get<std::string>();
		}

		inline void ReadCommand(const nlohmann::json& root, const char* key, std::string& out)
		{
			if (!root.contains(key))
				return;
			const auto& value = root.at(key);
			if (!value.is_string() || !IsValidChatCommand(value.get<std::string>()))
				throw std::runtime_error(std::string(key) + " must be a slash and 1-30 letters, digits or underscores, e.g. /hideskins");
			out = value.get<std::string>();
		}
	} // namespace detail

	// Throws std::runtime_error with a readable message on a bad value. Unknown keys are ignored.
	inline Settings ParseSettings(const nlohmann::json& root)
	{
		if (!root.is_object())
			throw std::runtime_error("config.json must contain a JSON object");

		Settings s;
		s.probe_items = detail::ReadBool(root, "ProbeItems", s.probe_items);
		s.probe_item_entry = detail::ReadBool(root, "ProbeItemEntry", s.probe_item_entry);
		s.probe_unlocked_cosmetics = detail::ReadBool(root, "ProbeUnlockedCosmetics", s.probe_unlocked_cosmetics);
		s.strip_items_on_entry = detail::ReadBool(root, "StripItemsOnEntry", s.strip_items_on_entry);
		s.strip_items_on_load = detail::ReadBool(root, "StripSavedItems", s.strip_items_on_load);
		s.backup_stripped_items = detail::ReadBool(root, "BackupStrippedItems", s.backup_stripped_items);
		s.probe_players = detail::ReadBool(root, "ProbePlayers", s.probe_players);
		s.probe_structure_messages = detail::ReadBool(root, "ProbeStructureMessages", s.probe_structure_messages);
		s.read_connection_platform = detail::ReadBool(root, "ReadConnectionPlatform", s.read_connection_platform);
		s.block_structure_skins_for_consoles = detail::ReadBool(root, "BlockStructureSkinsForConsoles", s.block_structure_skins_for_consoles);
		s.probe_structure_channels = detail::ReadBool(root, "ProbeStructureChannels", s.probe_structure_channels);
		s.probe_item_requests = detail::ReadBool(root, "ProbeItemRequests", s.probe_item_requests);
		s.probe_client_rpcs = detail::ReadBool(root, "ProbeClientRpcs", s.probe_client_rpcs);
		s.unequip_skin_items_for_consoles = detail::ReadBool(root, "UnequipSkinItemsForConsoles", s.unequip_skin_items_for_consoles);
		s.block_item_skins_for_consoles = detail::ReadBool(root, "BlockItemSkinsForConsoles", s.block_item_skins_for_consoles);
		if (root.contains("BlockSkinsForPlatforms"))
		{
			const auto& list = root.at("BlockSkinsForPlatforms");
			if (!list.is_array())
				throw std::runtime_error("BlockSkinsForPlatforms must be a list of numbers, e.g. [3, 4, 5]");
			s.block_skins_for_platforms.clear();
			for (const auto& v : list)
			{
				if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > 255)
					throw std::runtime_error("BlockSkinsForPlatforms entries must be whole numbers from 0 to 255");
				const int value = v.get<int>();
				if (std::find(s.block_skins_for_platforms.begin(), s.block_skins_for_platforms.end(), value) == s.block_skins_for_platforms.end())
					s.block_skins_for_platforms.push_back(value);
			}
		}
		s.block_skins_for_unknown_platforms = detail::ReadBool(root, "BlockSkinsForUnknownPlatforms", s.block_skins_for_unknown_platforms);
		s.summary_interval_seconds = detail::ReadInt(root, "SummaryInterval_Seconds", s.summary_interval_seconds, 5, 3600);
		s.max_detail_lines_per_summary = detail::ReadInt(root, "MaxDetailLinesPerSummary", s.max_detail_lines_per_summary, 0, 1000);
		s.scan_actors_per_tick = detail::ReadInt(root, "ScanActorsPerTick", s.scan_actors_per_tick, 100, 1000000);
		s.defer_structure_skins_seconds = detail::ReadInt(root, "DeferStructureSkins_Seconds", s.defer_structure_skins_seconds, 0, 600);
		s.release_structure_skins_per_second = detail::ReadInt(root, "ReleaseStructureSkinsPerSecond", s.release_structure_skins_per_second, 1, 5000);

		s.enable_strip = detail::ReadBool(root, "EnableStrip", s.enable_strip);
		s.strip_per_tick = detail::ReadInt(root, "StripPerTick", s.strip_per_tick, 1, 1000);

		s.filter_items = detail::ReadBool(root, "FilterItems", s.filter_items);
		s.filter_item_custom_data = detail::ReadBool(root, "FilterItemCustomData", s.filter_item_custom_data);
		s.block_skin_apply = detail::ReadBool(root, "BlockSkinApply", s.block_skin_apply);
		s.strip_on_map_load = detail::ReadBool(root, "StripOnMapLoad", s.strip_on_map_load);
		s.strip_on_map_load_delay_seconds = detail::ReadInt(root, "StripOnMapLoadDelay_Seconds", s.strip_on_map_load_delay_seconds, 10, 3600);
		s.strip_repeat_minutes = detail::ReadInt(root, "StripRepeat_Minutes", s.strip_repeat_minutes, 0, 1440);
		if (root.contains("BlockSkinApplyMessage"))
		{
			const auto& message = root.at("BlockSkinApplyMessage");
			if (!message.is_string())
				throw std::runtime_error("BlockSkinApplyMessage must be text");
			s.block_skin_apply_message = message.get<std::string>();
			if (s.block_skin_apply_message.size() > 300)
				throw std::runtime_error("BlockSkinApplyMessage must be at most 300 characters");
		}

		s.enable_player_hide_commands = detail::ReadBool(root, "EnablePlayerHideCommands", s.enable_player_hide_commands);
		s.enable_player_strip = detail::ReadBool(root, "EnablePlayerStrip", s.enable_player_strip);
		s.player_strip_cooldown_minutes = detail::ReadInt(root, "PlayerStripCooldown_Minutes", s.player_strip_cooldown_minutes, 0, 1440);
		detail::ReadCommand(root, "HideSkinsCommand", s.hide_skins_command);
		detail::ReadCommand(root, "ShowSkinsCommand", s.show_skins_command);
		detail::ReadCommand(root, "StripSkinsCommand", s.strip_skins_command);
		s.database_refresh_seconds = detail::ReadInt(root, "DatabaseRefresh_Seconds", s.database_refresh_seconds, 5, 3600);
		if (root.contains("PlayerPreferenceStorage"))
		{
			const auto& v = root.at("PlayerPreferenceStorage");
			if (!v.is_string() || (v.get<std::string>() != "File" && v.get<std::string>() != "Database"))
				throw std::runtime_error("PlayerPreferenceStorage must be \"File\" or \"Database\"");
			s.preferences_in_database = v.get<std::string>() == "Database";
		}
		if (root.contains("Database"))
		{
			const auto& d = root.at("Database");
			if (!d.is_object())
				throw std::runtime_error("Database must be an object with Host, Port, User, Password, Name, Table, Ssl");
			detail::ReadText(d, "Host", s.db.host, 255);
			s.db.port = detail::ReadInt(d, "Port", s.db.port, 1, 65535);
			detail::ReadText(d, "User", s.db.user, 128);
			detail::ReadText(d, "Password", s.db.password, 256);
			detail::ReadText(d, "Name", s.db.name, 64);
			detail::ReadText(d, "Table", s.db.table, 64);
			s.db.ssl = detail::ReadBool(d, "Ssl", s.db.ssl);
			if (!IsValidSqlName(s.db.name))
				throw std::runtime_error("Database.Name must be 1-64 letters, digits or underscores");
			if (!IsValidSqlName(s.db.table))
				throw std::runtime_error("Database.Table must be 1-64 letters, digits or underscores");
		}
		if (s.preferences_in_database && (s.db.host.empty() || s.db.user.empty()))
			throw std::runtime_error("PlayerPreferenceStorage is \"Database\": set Database.Host and Database.User");

		if (root.contains("StripMode"))
		{
			const auto& mode = root.at("StripMode");
			if (!mode.is_string())
				throw std::runtime_error("StripMode must be \"All\" or \"CustomOnly\"");
			const std::string text = mode.get<std::string>();
			if (text != "All" && text != "CustomOnly")
				throw std::runtime_error("StripMode must be \"All\" or \"CustomOnly\"");
			s.strip_mode = text;
		}

		if (root.contains("TreatAsConsolePlayers"))
		{
			const auto& list = root.at("TreatAsConsolePlayers");
			if (!list.is_array())
				throw std::runtime_error("TreatAsConsolePlayers must be a list of EOS IDs");
			for (const auto& item : list)
			{
				if (!item.is_string())
					throw std::runtime_error("TreatAsConsolePlayers must be a list of EOS IDs");
				const std::string id = item.get<std::string>();
				if (!id.empty())
					s.treat_as_console_players.push_back(id);
			}
		}

		if (root.contains("AllowSkinClasses"))
		{
			const auto& list = root.at("AllowSkinClasses");
			if (!list.is_array())
				throw std::runtime_error("AllowSkinClasses must be a list of class names");
			for (const auto& item : list)
			{
				if (!item.is_string())
					throw std::runtime_error("AllowSkinClasses must be a list of class names");
				const std::string name = item.get<std::string>();
				if (!name.empty())
					s.allow_skin_classes.push_back(name);
			}
		}
		return s;
	}

	// ------------------------------------------------------------------------------------------
	// Structure skins: classification and strip policy
	// ------------------------------------------------------------------------------------------

	enum class SkinKind
	{
		None,
		Official,   // the skin class is a tileset itself (StructureSkin_Tileset_LC_C, Tileset_Frontier, ...), id 0
		Custom      // StructureSkinCustomBP_C with a non-zero skin id, or a non-zero id with no class
	};

	// skin_class_name is the class name of the replicated skin class, or the local one when that is
	// null ("(null)" or "" when neither is set).
	inline SkinKind ClassifySkin(const std::string& skin_class_name, long long skin_id)
	{
		if (skin_id != 0)
			return SkinKind::Custom;
		if (skin_class_name == "StructureSkinCustomBP_C")
			return SkinKind::Custom;
		if (skin_class_name.empty() || skin_class_name == "(null)")
			return SkinKind::None;
		return SkinKind::Official;
	}

	inline const char* SkinKindName(SkinKind kind)
	{
		switch (kind)
		{
		case SkinKind::Official: return "official";
		case SkinKind::Custom: return "custom";
		default: return "none";
		}
	}

	inline bool IsAllowed(const Settings& s, const std::string& skin_class_name)
	{
		const std::string wanted = ToLower(skin_class_name);
		for (const std::string& name : s.allow_skin_classes)
			if (ToLower(name) == wanted)
				return true;
		return false;
	}

	inline bool ShouldStrip(const Settings& s, SkinKind kind, const std::string& skin_class_name)
	{
		if (kind == SkinKind::None)
			return false;
		if (IsAllowed(s, skin_class_name))
			return false;
		if (s.strip_mode == "CustomOnly")
			return kind == SkinKind::Custom;
		return true;
	}

	// Item skins: official skin class (mod id 0) or a mod cosmetic id; the item's skin class name is
	// "(null)" when only a mod id is set.
	inline SkinKind ItemSkinKind(const std::string& skin_class_name, long long mod_id)
	{
		return ClassifySkin(skin_class_name, mod_id > 0 ? mod_id : 0);
	}

	inline bool ShouldFilterItem(const Settings& s, const std::string& skin_class_name, long long mod_id)
	{
		return s.filter_items && ShouldStrip(s, ItemSkinKind(skin_class_name, mod_id), skin_class_name);
	}

	// An item about to be built from saved or transferred data. The data has no mod cosmetic id, so only the skin
	// class decides (StripMode CustomOnly therefore keeps any skin class it cannot tell apart from an official one).
	inline bool ShouldStripItemOnEntry(const Settings& s, const std::string& skin_class_name)
	{
		return s.strip_items_on_entry && ShouldStrip(s, ItemSkinKind(skin_class_name, 0), skin_class_name);
	}

	// Teams below this belong to the game (wild creatures and every map's NPCs: Thralls, pirates, undead, ...).
	constexpr int kFirstPlayerTeam = 50000;

	// An item rebuilt from the save that still carries a skin. Only items held by a player or tribe (player body,
	// tame, storage) are stripped: NPC gear uses skins as part of the NPC, and an unknown owner (team 0) is skipped.
	inline bool ShouldStripItemOnLoad(const Settings& s, const std::string& skin_class_name, int owner_team)
	{
		return s.strip_items_on_load && owner_team >= kFirstPlayerTeam && ShouldStrip(s, ItemSkinKind(skin_class_name, 0), skin_class_name);
	}

	// An item rebuilt from the save that carries only a mod cosmetic id (no skin class). Same owner rule as above.
	// A mod cosmetic is a custom cosmetic, so both StripMode values remove it.
	inline bool ShouldStripItemModCosmeticOnLoad(const Settings& s, long long mod_id, int owner_team)
	{
		return s.strip_items_on_load && owner_team >= kFirstPlayerTeam && mod_id > 0 &&
			ShouldStrip(s, ItemSkinKind("(null)", mod_id), "(null)");
	}

	// One removed item skin (written before the removal). kind is "load" (rebuilt from the save) or "entry"
	// (blanked before the item was built). Owner fields are empty for "entry": the item has no inventory yet.
	struct ItemBackupEntry
	{
		std::string kind;
		unsigned int item_id1 = 0;
		unsigned int item_id2 = 0;
		std::string item_class;
		std::string skin_class_name;
		std::string skin_class_path;
		long long mod_id = 0;
		int variant = 0;
		std::string owner_class;
		int owner_team = 0;
	};

	inline std::string ItemBackupLine(const ItemBackupEntry& e)
	{
		nlohmann::json j;
		j["v"] = 1;
		j["kind"] = e.kind;
		j["item"] = nlohmann::json::array({e.item_id1, e.item_id2});
		j["iclass"] = e.item_class;
		j["skin"] = e.skin_class_name;
		j["path"] = e.skin_class_path;
		j["mod"] = e.mod_id;
		j["variant"] = e.variant;
		j["owner"] = e.owner_class;
		j["team"] = e.owner_team;
		return j.dump();
	}

	// A player applying an item skin onto an item (UseItemOntoItem runs for dyes and other items too, so only a
	// class whose name contains "skin" counts). Uses the same BlockSkinApply switch and StripMode as structures.
	inline bool ShouldBlockItemSkinApply(const Settings& s, const std::string& skin_class_name)
	{
		if (!s.block_skin_apply)
			return false;
		std::string lower = skin_class_name;
		for (char& c : lower)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (lower.find("skin") == std::string::npos)
			return false;
		return ShouldStrip(s, ItemSkinKind(skin_class_name, 0), skin_class_name);
	}

	// A new structure skin being applied (skin_class_name "(null)" with id 0 is a removal and is always allowed).
	inline bool ShouldBlockSkinApply(const Settings& s, const std::string& skin_class_name, long long skin_id)
	{
		if (!s.block_skin_apply)
			return false;
		return ShouldStrip(s, ClassifySkin(skin_class_name, skin_id), skin_class_name);
	}

	// ------------------------------------------------------------------------------------------
	// Backup lines (one JSON object per line, appended before each structure is changed)
	// ------------------------------------------------------------------------------------------

	struct BackupEntry
	{
		unsigned long long structure_id = 0;
		std::string structure_class;
		std::string skin_class_name;   // for logs
		std::string skin_class_path;   // Blueprint'/Game/...' path, loadable with BPLoadClass; empty when the class was null
		long long skin_id = 0;
		int variant = 0;
	};

	inline std::string BackupLine(const BackupEntry& e)
	{
		nlohmann::json j;
		j["v"] = 1;
		j["sid"] = e.structure_id;
		j["sclass"] = e.structure_class;
		j["skin"] = e.skin_class_name;
		j["path"] = e.skin_class_path;
		j["id"] = e.skin_id;
		j["variant"] = e.variant;
		return j.dump();
	}

	inline BackupEntry ParseBackupLine(const std::string& line)
	{
		nlohmann::json j;
		try
		{
			j = nlohmann::json::parse(line);
		}
		catch (const nlohmann::json::exception&)
		{
			throw std::runtime_error("not valid JSON");
		}
		if (!j.is_object() || !j.contains("sid") || !j.at("sid").is_number_unsigned() || !j.contains("id") || !j.at("id").is_number_integer()
			|| !j.contains("path") || !j.at("path").is_string())
			throw std::runtime_error("missing or wrong-typed fields");

		BackupEntry e;
		e.structure_id = j.at("sid").get<unsigned long long>();
		e.skin_id = j.at("id").get<long long>();
		e.skin_class_path = j.at("path").get<std::string>();
		if (j.contains("sclass") && j.at("sclass").is_string())
			e.structure_class = j.at("sclass").get<std::string>();
		if (j.contains("skin") && j.at("skin").is_string())
			e.skin_class_name = j.at("skin").get<std::string>();
		if (j.contains("variant") && j.at("variant").is_number_integer())
			e.variant = j.at("variant").get<int>();
		return e;
	}

	// A backup file name must be a plain file name inside the backups folder.
	inline bool IsSafeBackupName(const std::string& name)
	{
		if (name.empty() || name.size() > 100)
			return false;
		if (name.find("..") != std::string::npos || name.find('/') != std::string::npos || name.find('\\') != std::string::npos
			|| name.find(':') != std::string::npos)
			return false;
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Counting helpers
	// ------------------------------------------------------------------------------------------

	// Hands out a fixed number of "log this individual event" tokens per summary window, so a busy
	// server cannot flood the log.
	class DetailBudget
	{
	public:
		void Reset(int max_lines) { remaining_ = max_lines < 0 ? 0 : max_lines; }
		bool Take()
		{
			if (remaining_ <= 0)
				return false;
			--remaining_;
			return true;
		}
		int Remaining() const { return remaining_; }

	private:
		int remaining_ = 0;
	};

	// Counts occurrences per string key and reports the busiest keys.
	class KeyCounter
	{
	public:
		void Add(const std::string& key, long long amount = 1) { counts_[key] += amount; }
		void Clear() { counts_.clear(); }
		size_t Distinct() const { return counts_.size(); }
		long long Total() const
		{
			long long total = 0;
			for (const auto& entry : counts_)
				total += entry.second;
			return total;
		}

		// Highest count first; ties broken by key so output is stable.
		std::vector<std::pair<std::string, long long>> Top(size_t n) const
		{
			std::vector<std::pair<std::string, long long>> rows(counts_.begin(), counts_.end());
			std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
				if (a.second != b.second)
					return a.second > b.second;
				return a.first < b.first;
			});
			if (rows.size() > n)
				rows.resize(n);
			return rows;
		}

		std::string Format(size_t n) const
		{
			const auto rows = Top(n);
			if (rows.empty())
				return "none";
			std::string out;
			for (const auto& row : rows)
			{
				if (!out.empty())
					out += ", ";
				out += row.first + " x" + std::to_string(row.second);
			}
			if (Distinct() > rows.size())
				out += ", +" + std::to_string(Distinct() - rows.size()) + " more";
			return out;
		}

	private:
		std::map<std::string, long long> counts_;
	};

	// Label for AShooterCharacter::GetCrossplayPlatform() (ECrossplayPlatform::Type: 0 Unknown, 1 Windows, 2 XSX, 3 PS5).
	inline std::string PlatformLabel(int platform)
	{
		switch (platform)
		{
		case 0: return "Unknown";
		case 1: return "Windows";
		case 2: return "XSX";
		case 3: return "PS5";
		default: return "Other(" + std::to_string(platform) + ")";
		}
	}

	// Label for UNetConnection::ConnectionPlatform (a byte), as observed by another operator's monitor:
	// 1 = "PC", 2 = "Xbox" (unclear), 3 = "PS5", 4 = Xbox Series (S and X both report 4), 5 = probably PS5, 6 = unknown.
	// -1 means the value could not be read. Series S and Series X cannot be told apart.
	// Platform group of a UNetConnection::ConnectionPlatform byte: 1 = PC, 2 - 5 = console (Xbox / PS5 values seen),
	// anything else = unknown (a later platform-aware mode treats unknown as console).
	enum class PlatformGroup { PC, Console, Unknown };
	inline PlatformGroup GroupOfPlatform(int platform)
	{
		if (platform == 1)
			return PlatformGroup::PC;
		if (platform >= 2 && platform <= 5)
			return PlatformGroup::Console;
		return PlatformGroup::Unknown;
	}

	// Testing override: a player listed in TreatAsConsolePlayers is handled as a console player, whatever the
	// connection reports, so the console path can be tested from a PC. EOS IDs compare case-insensitively.
	inline bool IsTreatedAsConsole(const Settings& s, const std::string& eos_id)
	{
		if (eos_id.empty())
			return false;
		const std::string wanted = ToLower(eos_id);
		for (const std::string& id : s.treat_as_console_players)
			if (ToLower(id) == wanted)
				return true;
		return false;
	}

	// Players who chose /hideskins: handled as console players, like TreatAsConsolePlayers, but kept by the plugin in a
	// small file next to the config and changed by the player. EOS IDs are stored lower case.
	class OptInList
	{
	public:
		bool Contains(const std::string& eos_id) const
		{
			return !eos_id.empty() && std::find(ids_.begin(), ids_.end(), ToLower(eos_id)) != ids_.end();
		}
		// Returns true when the list changed.
		bool Add(const std::string& eos_id)
		{
			if (!IsValidId(eos_id) || Contains(eos_id))
				return false;
			ids_.push_back(ToLower(eos_id));
			return true;
		}
		bool Remove(const std::string& eos_id)
		{
			const auto found = std::find(ids_.begin(), ids_.end(), ToLower(eos_id));
			if (found == ids_.end())
				return false;
			ids_.erase(found);
			return true;
		}
		size_t Size() const { return ids_.size(); }
		bool Empty() const { return ids_.empty(); }
		void Clear() { ids_.clear(); }

		nlohmann::json ToJson() const
		{
			nlohmann::json list = nlohmann::json::array();
			for (const std::string& id : ids_)
				list.push_back(id);
			return nlohmann::json{ {"HiddenSkinsPlayers", list} };
		}
		// Bad entries are skipped; a file that is not the expected shape throws.
		static OptInList FromJson(const nlohmann::json& root)
		{
			if (!root.is_object() || !root.contains("HiddenSkinsPlayers") || !root.at("HiddenSkinsPlayers").is_array())
				throw std::runtime_error("expected {\"HiddenSkinsPlayers\": [ ... ]}");
			OptInList out;
			for (const auto& item : root.at("HiddenSkinsPlayers"))
				if (item.is_string())
					out.Add(item.get<std::string>());
			return out;
		}
		static bool IsValidId(const std::string& id)
		{
			if (id.empty() || id.size() > 64)
				return false;
			for (unsigned char c : id)
				if (!std::isalnum(c))
					return false;
			return true;
		}

	private:
		std::vector<std::string> ids_;
	};

	inline bool IsTreatedAsConsole(const Settings& s, const OptInList& opted_in, const std::string& eos_id)
	{
		return IsTreatedAsConsole(s, eos_id) || (s.enable_player_hide_commands && opted_in.Contains(eos_id));
	}

	// The hide commands only change anything when the plugin blanks skins for console viewers at all.
	inline bool ConsoleBlockingOn(const Settings& s)
	{
		return s.block_structure_skins_for_consoles || s.block_item_skins_for_consoles;
	}

	// Player strip: only structures of the caller's own team, and only a real player or tribe team.
	inline bool InPlayerStripScope(int structure_team, int caller_team)
	{
		return caller_team >= kFirstPlayerTeam && structure_team == caller_team;
	}

	// Tribe id typed by an admin: digits only, a player or tribe team (>= 50000). Returns 0 when not valid.
	inline int ParseTribeId(const std::string& text)
	{
		if (text.empty() || text.size() > 10)
			return 0;
		long long value = 0;
		for (unsigned char c : text)
		{
			if (!std::isdigit(c))
				return 0;
			value = value * 10 + (c - '0');
		}
		return value >= kFirstPlayerTeam && value <= INT32_MAX ? static_cast<int>(value) : 0;
	}

	// Minutes left before the tribe may strip again (0 = allowed now). Times are seconds.
	inline int CooldownMinutesLeft(long long now_seconds, long long last_seconds, int cooldown_minutes)
	{
		if (cooldown_minutes <= 0 || last_seconds <= 0)
			return 0;
		const long long left = last_seconds + cooldown_minutes * 60LL - now_seconds;
		return left <= 0 ? 0 : static_cast<int>((left + 59) / 60);
	}

	// A confirmed strip needs this tribe's report from at most this many seconds ago.
	constexpr long long kPlayerStripReportValidSeconds = 600;
	inline bool PlayerReportStillValid(long long now_seconds, long long report_seconds)
	{
		return report_seconds > 0 && now_seconds - report_seconds <= kPlayerStripReportValidSeconds;
	}

	inline std::string TribeBackupName(int team, long long epoch_seconds)
	{
		return "strip-tribe" + std::to_string(team) + "-" + std::to_string(epoch_seconds) + ".jsonl";
	}

	// SQL for the shared preference table. Table names pass IsValidSqlName and EOS IDs pass OptInList::IsValidId
	// (letters and digits), so the text needs no escaping; the builders refuse anything else.
	inline std::string SqlCreatePreferenceTable(const std::string& table)
	{
		if (!IsValidSqlName(table))
			throw std::runtime_error("bad table name");
		return "CREATE TABLE IF NOT EXISTS `" + table + "` ("
			"`eos_id` VARCHAR(64) NOT NULL PRIMARY KEY, "
			"`changed_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
			"`changed_on` VARCHAR(64) NOT NULL DEFAULT ''"
			") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
	}

	inline std::string SqlSelectPreferences(const std::string& table)
	{
		if (!IsValidSqlName(table))
			throw std::runtime_error("bad table name");
		return "SELECT `eos_id` FROM `" + table + "`";
	}

	inline std::string SqlSetPreference(const std::string& table, const std::string& eos_id, bool hide, const std::string& server)
	{
		if (!IsValidSqlName(table) || !OptInList::IsValidId(eos_id))
			throw std::runtime_error("bad table name or EOS ID");
		std::string where;   // server label: keep letters, digits, - and _ only
		for (unsigned char c : server.substr(0, 64))
			if (std::isalnum(c) || c == '-' || c == '_')
				where.push_back(static_cast<char>(c));
		if (hide)
			return "INSERT INTO `" + table + "` (`eos_id`, `changed_on`) VALUES ('" + ToLower(eos_id) + "', '" + where
				+ "') ON DUPLICATE KEY UPDATE `changed_at` = CURRENT_TIMESTAMP, `changed_on` = VALUES(`changed_on`)";
		return "DELETE FROM `" + table + "` WHERE `eos_id` = '" + ToLower(eos_id) + "'";
	}

	inline PlatformGroup EffectiveGroup(int platform, bool treated_as_console)
	{
		return treated_as_console ? PlatformGroup::Console : GroupOfPlatform(platform);
	}

	// True when a connection gets no skins. A player in TreatAsConsolePlayers / opted in with /hideskins is always blocked.
	// Otherwise the platform value decides: listed in BlockSkinsForPlatforms = blocked; 1, 3, 4 and 5 not listed = shown;
	// any other value (2, 6, unset, unread = -1) follows BlockSkinsForUnknownPlatforms. Platform 1 (PC) is only blocked
	// if the operator lists it.
	inline bool IsBlockedPlatform(const Settings& s, int platform, bool treated_as_console)
	{
		if (treated_as_console)
			return true;
		if (std::find(s.block_skins_for_platforms.begin(), s.block_skins_for_platforms.end(), platform) != s.block_skins_for_platforms.end())
			return true;
		if (platform == 1 || platform == 3 || platform == 4 || platform == 5)
			return false;
		return s.block_skins_for_unknown_platforms;
	}

	inline std::string ConnectionPlatformLabel(int platform)
	{
		switch (platform)
		{
		case -1: return "NotRead";
		case 0: return "Unset(0)";
		case 1: return "PC(1)";
		case 2: return "Xbox?(2)";
		case 3: return "XboxSeriesX?(3)";
		case 4: return "XboxSeriesS?(4)";
		case 5: return "PS5(5)";
		case 6: return "Unknown(6)";
		default: return "Other(" + std::to_string(platform) + ")";
		}
	}

	// True when a replicated cosmetic mod id means "something is applied". Real mod ids are positive;
	// 0 and -1 are "none" (a server test showed every character reporting -1 when nothing is applied).
	inline bool HasCosmeticId(long long id) { return id > 0; }

	// Everything the probe counts between two summaries.
	struct Counters
	{
		long long items_to_client = 0;        // GetItemNetInfo(true) calls
		long long items_with_skin = 0;        //   ... whose result carried an ItemSkinTemplate
		long long items_with_mod_id = 0;      //   ... whose item has a custom cosmetic mod skin id
		long long items_with_custom_data = 0; //   ... whose result carried CustomItemDatas entries
		long long character_prerep = 0;       // AShooterCharacter.PreReplication calls
		long long character_with_cosmetic = 0;//   ... where a hair/eyebrow/facial hair mod id was set
		long long skin_messages = 0;          // APrimalStructure.MultiHandleSkinNetMessage calls
		long long skin_sets = 0;              // APrimalStructure.SetStructureSkin calls (every call, blocked or not)
		long long skin_refreshes = 0;         // APrimalStructure.MultiRefreshStructureSkin multicast calls
		long long skin_rpc_withheld = 0;      // skin change multicasts not sent to a console connection
		KeyCounter set_skin_classes;          // skin classes passed to SetStructureSkin
		KeyCounter refresh_skin_classes;      // skin classes passed to MultiRefreshStructureSkin
		KeyCounter skin_classes;              // item skin class names seen going to clients
		KeyCounter mod_ids;                   // cosmetic mod ids seen (items, characters)
		KeyCounter platforms;                 // GetCrossplayPlatform() of characters carrying a cosmetic
		KeyCounter connection_platforms;      // ConnectionPlatform of every character's player (once per window)
		KeyCounter message_names;             // structure skin message names
		KeyCounter message_structures;        // structure classes receiving skin messages
		long long items_filtered = 0;         // items whose client copy had its skin blanked
		long long items_custom_data_cleared = 0; // ... and whose custom data was cleared too
		long long skin_applies_blocked = 0;   // structure skin applications refused
		KeyCounter blocked_skin_classes;
		long long item_requests = 0;          // ServerRequestItems calls (probe)
		long long item_requests_equipped = 0; //   ... asking for equipped gear
		long long request_items = 0;          // GetItemNetInfo(true) results built inside a request
		long long request_items_skinned = 0;  //   ... that carried a skin or mod cosmetic id
		long long outside_items = 0;          // GetItemNetInfo(true) results built outside any request
		long long outside_items_skinned = 0;  //   ... that carried a skin or mod cosmetic id
		long long skin_items_unequipped = 0;  // equipped skin items sent to a console viewer as not equipped
		KeyCounter unequipped_skin_classes;   // ... by item class
		long long items_blanked_for_console = 0; // item skins blanked in the copy sent to a console viewer
		long long item_mod_ids_blanked_for_console = 0; // mod cosmetic ids cleared in the copy sent to a console viewer
		long long weapon_skins_blanked_for_console = 0; // held weapon actors replicated to a console with skin/mod blanked
		KeyCounter request_viewers;           // viewer platform + equipped flag + skinned items of each request
		KeyCounter console_rpcs;              // remote functions (item/skin/cosmetic/equip names) sent to console connections
		KeyCounter equipped_item_classes;     // item classes sent in equipped-gear requests (what other characters wear), skinned or not

		void Clear() { *this = Counters{}; }
	};

	inline std::string FormatSummary(const Counters& c, int window_seconds)
	{
		std::string out = "Probe summary (last " + std::to_string(window_seconds) + " s)\n";
		out += "  items to clients: " + std::to_string(c.items_to_client) + ", with skin: " + std::to_string(c.items_with_skin)
			+ ", with mod id: " + std::to_string(c.items_with_mod_id) + ", with custom data: " + std::to_string(c.items_with_custom_data) + "\n";
		out += "  item skin classes: " + c.skin_classes.Format(8) + "\n";
		out += "  character pre-replications: " + std::to_string(c.character_prerep) + ", carrying a cosmetic: "
			+ std::to_string(c.character_with_cosmetic) + "\n";
		out += "  crossplay platform of cosmetic carriers: " + c.platforms.Format(6) + "\n";
		out += "  connection platform of all characters: " + c.connection_platforms.Format(8) + "\n";
		out += "  cosmetic mod ids: " + c.mod_ids.Format(8) + "\n";
		out += "  structure skin messages: " + std::to_string(c.skin_messages) + ", names: " + c.message_names.Format(6)
			+ ", structures: " + c.message_structures.Format(6);
		out += "\n  structure skin sets: " + std::to_string(c.skin_sets) + " " + c.set_skin_classes.Format(6)
			+ ", skin refresh multicasts: " + std::to_string(c.skin_refreshes) + " " + c.refresh_skin_classes.Format(6)
			+ ", withheld from consoles: " + std::to_string(c.skin_rpc_withheld);
		out += "\n  filtered: items " + std::to_string(c.items_filtered) + " (custom data cleared " + std::to_string(c.items_custom_data_cleared)
			+ "), structure skin applications blocked " + std::to_string(c.skin_applies_blocked) + ": " + c.blocked_skin_classes.Format(6);
		if (c.item_requests > 0 || c.outside_items > 0 || c.items_blanked_for_console > 0 || c.skin_items_unequipped > 0 || c.weapon_skins_blanked_for_console > 0)
		{
			out += "\n  item requests: " + std::to_string(c.item_requests) + " (equipped " + std::to_string(c.item_requests_equipped)
				+ "), items built inside requests: " + std::to_string(c.request_items) + " (skinned " + std::to_string(c.request_items_skinned)
				+ "), outside requests: " + std::to_string(c.outside_items) + " (skinned " + std::to_string(c.outside_items_skinned) + ")"
				+ ", blanked for consoles: " + std::to_string(c.items_blanked_for_console) + " (mod ids " + std::to_string(c.item_mod_ids_blanked_for_console) + ", held weapons " + std::to_string(c.weapon_skins_blanked_for_console) + ")"
				+ ", skin items unequipped for consoles: " + std::to_string(c.skin_items_unequipped) + " " + c.unequipped_skin_classes.Format(8);
			out += "\n  request viewers: " + c.request_viewers.Format(8);
			out += "\n  equipped item classes: " + c.equipped_item_classes.Format(12);
		}
		if (c.console_rpcs.Distinct() > 0)
			out += "\n  item/skin RPCs to consoles: " + c.console_rpcs.Format(16);
		return out;
	}

	// Result of a Scan / Strip run over the structures in the loaded world.
	struct ScanResult
	{
		long long actors_seen = 0;
		long long structures = 0;
		long long structures_with_skin = 0;     // official + custom
		long long official = 0;
		long long custom = 0;
		long long flag_custom = 0;              // HasAppliedCustomCosmetic() returned true
		long long flag_without_custom = 0;      //   ... but the skin fields say not custom (sanity check)
		long long custom_without_flag = 0;      // skin fields say custom, flag false (sanity check)
		long long kept_allowed = 0;             // skin present but not stripped (allow list / StripMode)
		long long would_strip = 0;              // dry run
		long long stripped = 0;                 // changed
		long long strip_failed = 0;             // skin still there after the call
		KeyCounter official_classes;            // official skin class names
		KeyCounter custom_ids;                  // custom skin ids
		KeyCounter structure_classes;           // structure classes that carry a skin

		double Percent() const
		{
			return structures == 0 ? 0.0 : 100.0 * static_cast<double>(structures_with_skin) / static_cast<double>(structures);
		}
	};

	inline std::string FormatScan(const ScanResult& r)
	{
		char pct[32];
		std::snprintf(pct, sizeof(pct), "%.2f", r.Percent());
		std::string out = "Scan: " + std::to_string(r.actors_seen) + " actors, " + std::to_string(r.structures) + " structures, "
			+ std::to_string(r.structures_with_skin) + " with a skin (" + pct + "%): " + std::to_string(r.official) + " official, "
			+ std::to_string(r.custom) + " custom\n";
		out += "  official skin classes: " + r.official_classes.Format(10) + "\n";
		out += "  custom skin ids: " + r.custom_ids.Format(10) + "\n";
		out += "  structure classes: " + r.structure_classes.Format(10) + "\n";
		out += "  HasAppliedCustomCosmetic check: " + std::to_string(r.flag_custom) + " true, " + std::to_string(r.flag_without_custom)
			+ " true but not custom by skin fields, " + std::to_string(r.custom_without_flag) + " custom by skin fields but false";
		if (r.would_strip > 0 || r.stripped > 0 || r.strip_failed > 0 || r.kept_allowed > 0)
			out += "\n  strip: would strip " + std::to_string(r.would_strip) + ", stripped " + std::to_string(r.stripped) + ", failed "
				+ std::to_string(r.strip_failed) + ", kept " + std::to_string(r.kept_allowed);
		return out;
	}
} // namespace CosmeticsProbe
