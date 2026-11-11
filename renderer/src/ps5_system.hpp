// What's on the console, read on the console itself (the start of the ps5-system tool pack).
// Read-only. Paths are parameters so the host build can test against a stand-in database.
//
// Where the data lives was learned from PS5Upload's helper (read as documentation, no code taken):
// the shell's app database, /system_data/priv/mms/app.db, has one titles table per user, so the
// table is found by its columns. On 12.70 (the owner's console, 9 Nov 2026) that's
// tbl_iconinfo_<user id>: every title the account has (bought, PS Plus, played once), 292 rows for
// 15 installed, with installStatus (2 = installed), playedTimeOnConsole (seconds, this console),
// gltTotalPlayedTime (seconds, all the account's consoles, as PSN has it) and lastPlayedDate.
// The shell keeps the database open; a normal read-only open can come back busy, so a second try
// opens it as immutable (no locking).
#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace ps5sys {

struct Title {
  std::string id;        // CUSA12345, PPSA01234
  std::string name;      // as the Home screen shows it
  std::string platform;  // "PS5" (PPSA), "PS4" (CUSA) or "homebrew"
  bool installed = true;
  long long secondsOnConsole = -1;   // -1: not known
  long long secondsAllConsoles = -1;
  std::string lastPlayed;  // YYYY-MM-DD, or empty
  std::string icon;        // file:///user/appmeta/<id>/icon0.png if it's there, else empty
};

struct Titles {
  std::vector<Title> titles;
  std::string source;  // "app.db", "app.db (immutable)", "appmeta", or empty if nothing could be read
  std::string error;   // why nothing could be read
  std::string schema;  // the titles table and its columns, for the log (what else could be read)
  std::string installedBy;  // how `installed` was decided: "/user/app", "installStatus", or "" (all are)
};

/**
 * The account's games and apps on this console, without the system's own (NPXS…), each marked
 * installed or not. Installed: the title has a folder in `appDir` (where installs go, homebrew
 * included); if that can't be listed, the table's installStatus (2) when it has one.
 */
Titles consoleTitles(const std::string& appDb = "/system_data/priv/mms/app.db", const std::string& appMeta = "/user/appmeta", const std::string& appDir = "/user/app");

/**
 * What the agent's list_games gets: {games: [{id, title, platform, icon?, hoursPlayed?, lastPlayed?}],
 * notInstalled, source} with the installed titles only, or {error}. notInstalled counts the
 * account's other titles (bought, PS Plus, played before) so the agent can say they exist.
 */
nlohmann::json toJson(const Titles& t);

/**
 * {model, firmware, uptimeMinutes, storage: {totalGB, usedGB, freeGB}}, leaving out what couldn't be read;
 * {error, reason} if nothing could. Model and firmware come from libkernel (the PS5 only), uptime
 * from kern.boottime, storage from the file system holding `storagePath`.
 */
nlohmann::json systemInfo(const std::string& storagePath = "/user");

/**
 * {cpu, soc, ssd} in °C, leaving out what couldn't be read (ssd: only with an M.2 drive fitted);
 * {error, reason} if nothing could. The PS5 only.
 *
 * Call it on the app's main thread: PS5Upload found the sensor calls crash the process from a
 * thread the app created itself. Each libkernel call is fault-guarded, and values outside
 * 1–150 °C are dropped (some firmware returns garbage).
 */
nlohmann::json temperatures();

/** Where saves, captures and the play log live; parameters so the host build can test them. */
struct Paths {
  std::string home = "/user/home";             // <user id (hex)>/savedata_prospero (PS5), savedata (PS4)
  std::string photos = "/user/av_contents/photo";  // screenshots, .jxr (JPEG XR)
  std::string thumbnails = "/user/av_contents/thumbnails/photo";
  std::string playLog = "/system_data/priv/system_logger2/nobackup/database/sl2_log.db";
};

/**
 * {saves: [{id, title, platform, sizeMB, lastSaved (YYYY-MM-DD)}]}, newest first; titles named from
 * `names` (installed titles) when known. A save's folder size is its files and sce_sys/ (the data
 * inside is encrypted; this is disk use, not progress).
 */
nlohmann::json saves(const Titles& names, const Paths& paths = {});

/** {count, newest: [{taken (YYYY-MM-DD), sizeMB}]} (up to 5), each shot once (thumbnails dedup'd). */
nlohmann::json screenshots(const Paths& paths = {});

/**
 * {recentlyPlayed: [{id, title, platform, hoursPlayed, lastPlayed?}]}, most recently played first,
 * from the system log's ApplicationSessionEndBi rows (one per ended session). Hours on this
 * console: the title's playedTimeOnConsole from `names` when known, else the sum of its sessions'
 * totalFgTime (that session's seconds, not a running total: the two matched to the second on the
 * owner's console). `schema` gets the log table's columns, for the app's log.
 */
nlohmann::json playActivity(const Titles& names, const Paths& paths = {}, std::string* schema = nullptr);

/** One of the reads the agent's console tools use (channel/console-reads.ts), by name. */
nlohmann::json read(const std::string& name);

}  // namespace ps5sys
