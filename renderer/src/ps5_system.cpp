#include "ps5_system.hpp"

#include <dirent.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <dlfcn.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>

#include <mutex>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ctime>
#include <map>
#include <set>

namespace ps5sys {
namespace {

using json = nlohmann::json;

/** Four letters and five digits. */
bool validTitleId(const std::string& s) {
  if (s.size() != 9) return false;
  for (int i = 0; i < 4; ++i)
    if (!std::isalpha(static_cast<unsigned char>(s[i]))) return false;
  for (int i = 4; i < 9; ++i)
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  return true;
}

/** Games and apps the player installed; the system's own apps (NPXS…) aren't. */
bool listed(const std::string& id) { return validTitleId(id) && id.compare(0, 4, "NPXS") != 0; }

/** PPSA: PS5, CUSA: PS4; other prefixes are homebrew (FAKE00000, PLDM00001, …). */
std::string platformOf(const std::string& id) {
  if (id.compare(0, 4, "PPSA") == 0) return "PS5";
  if (id.compare(0, 4, "CUSA") == 0) return "PS4";
  return "homebrew";
}

bool iequals(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

/** Read-only; a database the shell holds may answer busy, so then again as immutable (no locks). */
sqlite3* openReadOnly(const std::string& path, std::string& how) {
  if (::access(path.c_str(), R_OK) != 0) return nullptr;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
    sqlite3_busy_timeout(db, 2000);
    // Opening is lazy: a locked file only shows on the first read.
    sqlite3_stmt* probe = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master LIMIT 1", -1, &probe, nullptr) == SQLITE_OK) {
      int step = sqlite3_step(probe);
      sqlite3_finalize(probe);
      if (step == SQLITE_ROW || step == SQLITE_DONE) {
        how = "app.db";
        return db;
      }
    }
  }
  sqlite3_close(db);
  db = nullptr;
  const std::string uri = "file:" + path + "?immutable=1";
  if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) == SQLITE_OK) {
    how = "app.db (immutable)";
    return db;
  }
  sqlite3_close(db);
  return nullptr;
}

std::vector<std::string> columnsOf(sqlite3* db, const std::string& table) {
  std::vector<std::string> cols;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, ("PRAGMA table_info(\"" + table + "\")").c_str(), -1, &st, nullptr) != SQLITE_OK) return cols;
  while (sqlite3_step(st) == SQLITE_ROW)
    if (auto c = sqlite3_column_text(st, 1)) cols.emplace_back(reinterpret_cast<const char*>(c));
  sqlite3_finalize(st);
  return cols;
}

struct Candidate {
  std::string table, idCol, nameCol;
  std::vector<std::string> columns;
  int score = 0;
  std::string col(const char* want) const {
    for (const auto& c : columns)
      if (iequals(c, want)) return c;
    return "";
  }
};

/** Tables with a titleId column: the ones with a display name first, then the ones that say what's installed and played. */
std::vector<Candidate> candidates(sqlite3* db) {
  std::vector<std::string> tables;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table'", -1, &st, nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW)
      if (auto t = sqlite3_column_text(st, 0)) tables.emplace_back(reinterpret_cast<const char*>(t));
    sqlite3_finalize(st);
  }
  std::vector<Candidate> out;
  for (const auto& t : tables) {
    // Identifiers come from the database itself and are quoted; skip anything that would need escaping.
    if (t.find('"') != std::string::npos) continue;
    Candidate c{t, "", "", columnsOf(db, t), 0};
    c.idCol = c.col("titleId");
    if (c.idCol.empty()) continue;
    c.score = 1;
    for (const char* want : {"titleName", "appName", "name", "title_name", "canonicalTitle"})
      if (c.nameCol.empty()) c.nameCol = c.col(want);
    if (!c.nameCol.empty()) c.score += 2;
    if (!c.col("playedTimeOnConsole").empty()) c.score += 2;  // the per-user titles table (tbl_iconinfo_<user> on 12.70)
    out.push_back(std::move(c));
  }
  std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
  return out;
}

/** The title's name from /user/appmeta/<id>/param.json (PS5 titles), in its default language. */
std::string nameFromParam(const std::string& appMeta, const std::string& id) {
  std::ifstream f(appMeta + "/" + id + "/param.json");
  if (!f) return "";
  json p = json::parse(f, nullptr, false);
  if (!p.is_object() || !p.contains("localizedParameters") || !p["localizedParameters"].is_object()) return "";
  const json& loc = p["localizedParameters"];
  std::string lang = loc.value("defaultLanguage", "en-US");
  for (const std::string& l : {lang, std::string("en-US")})
    if (loc.contains(l) && loc[l].is_object() && loc[l].contains("titleName") && loc[l]["titleName"].is_string()) return loc[l]["titleName"];
  return "";
}

/** Folder names in `dir`, or false if it can't be listed. */
bool folders(const std::string& dir, std::set<std::string>& out) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) return false;
  while (dirent* e = ::readdir(d))
    if (e->d_name[0] != '.') out.insert(e->d_name);
  ::closedir(d);
  return true;
}

std::string text(sqlite3_stmt* st, int i) {
  auto t = sqlite3_column_text(st, i);
  return t ? reinterpret_cast<const char*>(t) : "";
}

/** Each installed title's Home screen icon, as the app can draw it (Tile.image). */
void addIcons(Titles& out, const std::string& appMeta) {
  for (auto& t : out.titles) {
    if (!t.installed) continue;
    const std::string path = appMeta + "/" + t.id + "/icon0.png";
    if (::access(path.c_str(), R_OK) == 0) t.icon = "file://" + path;
  }
}

long long seconds(sqlite3_stmt* st, int i) { return sqlite3_column_type(st, i) == SQLITE_NULL || text(st, i).empty() ? -1 : sqlite3_column_int64(st, i); }

}  // namespace

Titles consoleTitles(const std::string& appDb, const std::string& appMeta, const std::string& appDir) {
  Titles out;
  std::set<std::string> seen, inAppDir;
  const bool appDirListed = folders(appDir, inAppDir);
  std::string how;
  if (sqlite3* db = openReadOnly(appDb, how)) {
    // A console with several users has a table per user; the first that has rows wins.
    auto describe = [](const Candidate& c) {
      std::string d = c.table + "(";
      for (size_t i = 0; i < c.columns.size(); ++i) d += (i ? ", " : "") + c.columns[i];
      return d + ")";
    };
    auto q = [](const std::string& col) { return col.empty() ? std::string("NULL") : "\"" + col + "\""; };
    for (const auto& c : candidates(db)) {
      if (out.schema.empty()) out.schema = describe(c);
      const std::string status = c.col("installStatus");
      const std::string sql = "SELECT " + q(c.idCol) + ", " + q(c.nameCol) + ", " + q(status) + ", " + q(c.col("playedTimeOnConsole")) + ", " +
                              q(c.col("gltTotalPlayedTime")) + ", " + q(c.col("lastPlayedDate")) + " FROM \"" + c.table + "\" WHERE " + q(c.idCol) +
                              " IS NOT NULL ORDER BY " + q(c.idCol);
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) continue;
      while (sqlite3_step(st) == SQLITE_ROW) {
        std::string tid = text(st, 0);
        if (!listed(tid) || !seen.insert(tid).second) continue;
        Title t;
        t.id = tid, t.name = text(st, 1), t.platform = platformOf(tid);
        if (t.name.empty()) t.name = nameFromParam(appMeta, tid);
        if (t.name.empty()) t.name = tid;
        if (appDirListed) t.installed = inAppDir.count(tid) > 0;
        else if (!status.empty()) t.installed = sqlite3_column_int(st, 2) == 2;
        t.secondsOnConsole = seconds(st, 3);
        t.secondsAllConsoles = seconds(st, 4);
        std::string last = text(st, 5);
        if (last.size() >= 10 && last.compare(0, 4, "1970") != 0) t.lastPlayed = last.substr(0, 10);
        out.titles.push_back(std::move(t));
      }
      sqlite3_finalize(st);
      if (!out.titles.empty()) {
        out.schema = describe(c);
        out.installedBy = appDirListed ? appDir : !status.empty() ? "installStatus" : "";
        break;
      }
      seen.clear();
    }
    sqlite3_close(db);
    if (!out.titles.empty()) {
      out.source = how;
      addIcons(out, appMeta);
      return out;
    }
  }
  // No database (or nothing in it we could read): each registered title has a folder here.
  std::set<std::string> meta;
  if (folders(appMeta, meta)) {
    for (const auto& id : meta) {
      if (!listed(id) || !seen.insert(id).second) continue;
      std::string n = nameFromParam(appMeta, id);
      Title t;
      t.id = id, t.name = n.empty() ? id : n, t.platform = platformOf(id);
      if (appDirListed) t.installed = inAppDir.count(id) > 0;
      out.titles.push_back(std::move(t));
    }
    if (!out.titles.empty()) {
      out.source = "appmeta";
      addIcons(out, appMeta);
      out.installedBy = appDirListed ? appDir : "";
      return out;
    }
  }
  out.error = ::access(appDb.c_str(), F_OK) != 0 ? "can't see " + appDb : ::access(appDb.c_str(), R_OK) != 0 ? "not allowed to read " + appDb : "no titles found in " + appDb;
  return out;
}

namespace {
/** Seconds → hours, one decimal (rounded down). */
double hours(long long s) { return static_cast<double>(s / 360) / 10; }
}  // namespace

nlohmann::json toJson(const Titles& t) {
  if (t.titles.empty()) return {{"error", "titles_unreadable"}, {"reason", t.error}};
  json games = json::array();
  int notInstalled = 0;
  for (const auto& g : t.titles) {
    if (!g.installed) {
      ++notInstalled;
      continue;
    }
    json row = {{"id", g.id}, {"title", g.name}, {"platform", g.platform}};
    if (!g.icon.empty()) row["icon"] = g.icon;
    if (g.secondsOnConsole > 0) row["hoursPlayed"] = hours(g.secondsOnConsole);
    if (!g.lastPlayed.empty()) row["lastPlayed"] = g.lastPlayed;
    games.push_back(row);
  }
  return {{"games", games}, {"notInstalled", notInstalled}, {"source", "console"}};
}

namespace {

#ifdef __PROSPERO__
// libkernel's hardware getters, looked up at run time (a firmware without one reads as missing).
// Signatures as the SDK's own shell (ps5-payload-dev/shsrv) declares them.
struct SwVersion {
  unsigned long size;
  char text[0x1c];
  unsigned int binary;
  unsigned long reserved;
};
using ModelFn = int (*)(char*);
using VersionFn = int (*)(SwVersion*);
using CpuTempFn = int (*)(int*);
using SocTempFn = int (*)(int, int*);

template <class F>
F kernelFn(const char* name) {
  return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name));
}

// A getter can fault in some processes (PS5Upload's field reports): a fault inside guarded() jumps
// back and the value reads as missing, instead of ending the app. One guarded call at a time.
std::mutex g_guardMutex;
sigjmp_buf g_guardJump;
pthread_t g_guardThread;
volatile sig_atomic_t g_guarding = 0;

void onFault(int sig) {
  if (g_guarding && pthread_equal(pthread_self(), g_guardThread)) siglongjmp(g_guardJump, 1);
  signal(sig, SIG_DFL);  // not ours: crash as before
  raise(sig);
}

template <class F>
bool guarded(F call) {
  static std::once_flag installed;
  std::call_once(installed, [] {
    struct sigaction sa {};
    sa.sa_handler = onFault;
    sa.sa_flags = SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL}) sigaction(sig, &sa, nullptr);
  });
  std::lock_guard lock(g_guardMutex);
  g_guardThread = pthread_self();
  if (sigsetjmp(g_guardJump, 1) != 0) {
    g_guarding = 0;
    return false;
  }
  g_guarding = 1;
  call();
  g_guarding = 0;
  return true;
}

bool plausibleCelsius(int c) { return c >= 1 && c <= 150; }
#endif

double gb(unsigned long long bytes) { return static_cast<double>(bytes * 10 / (1000ULL * 1000 * 1000)) / 10; }

}  // namespace

nlohmann::json systemInfo(const std::string& storagePath) {
  json out = json::object();
#ifdef __PROSPERO__
  if (auto getModel = kernelFn<ModelFn>("sceKernelGetHwModelName")) {
    static char model[1024];  // the getter's buffer size is undocumented: generous
    volatile int rc = -1;
    std::fill(std::begin(model), std::end(model), '\0');
    if (guarded([&] { rc = getModel(model); }) && rc == 0 && model[0]) out["model"] = std::string(model, strnlen(model, sizeof model));
  }
  if (auto getVersion = kernelFn<VersionFn>("sceKernelGetProsperoSystemSwVersion")) {
    static SwVersion v;
    volatile int rc = -1;
    v = SwVersion{};
    v.size = sizeof v;
    if (guarded([&] { rc = getVersion(&v); }) && rc == 0 && v.text[0]) {
      std::string text(v.text, strnlen(v.text, sizeof v.text));
      while (!text.empty() && text.back() == ' ') text.pop_back();
      out["firmware"] = text;
    }
  }
#endif
  struct timeval boot {};
  size_t len = sizeof boot;
  if (sysctlbyname("kern.boottime", &boot, &len, nullptr, 0) == 0 && boot.tv_sec > 0) {
    struct timeval now {};
    gettimeofday(&now, nullptr);
    if (now.tv_sec >= boot.tv_sec) out["uptimeMinutes"] = (now.tv_sec - boot.tv_sec) / 60;
  }
  struct statvfs fs {};
  if (statvfs(storagePath.c_str(), &fs) == 0 && fs.f_blocks > 0) {
    const unsigned long long unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
    // Used too, so the model doesn't do the arithmetic (MiniMax got "38% used" for 312 of 825 GB free).
    const unsigned long long total = fs.f_blocks * unit, avail = fs.f_bavail * unit;
    out["storage"] = {{"totalGB", gb(total)}, {"usedGB", gb(total - std::min(total, avail))}, {"freeGB", gb(avail)}};
  }
  if (out.empty()) return {{"error", "unreadable"}, {"reason", "no system facts could be read"}};
  return out;
}

nlohmann::json temperatures() {
#ifdef __PROSPERO__
  json out = json::object();
  if (auto cpuTemp = kernelFn<CpuTempFn>("sceKernelGetCpuTemperature")) {
    static int t;
    volatile int rc = -1;
    t = 0;
    if (guarded([&] { rc = cpuTemp(&t); }) && rc == 0 && plausibleCelsius(t)) out["cpu"] = t;
  }
  // Channels 0 (the SoC) and 2 (the M.2 slot; an empty slot reads 0) only: the ones known to be
  // read safely elsewhere. Probing the others risks a fault on an untested model.
  if (auto socTemp = kernelFn<SocTempFn>("sceKernelGetSocSensorTemperature")) {
    for (int channel : {0, 2}) {
      static int t;
      volatile int rc = -1;
      t = 0;
      if (!guarded([&] { rc = socTemp(channel, &t); }) || rc != 0 || !plausibleCelsius(t)) continue;
      if (channel == 0) out["soc"] = t;
      else if (t >= 20) out["ssd"] = t;
    }
  }
  if (out.empty()) return {{"error", "unreadable"}, {"reason", "no temperature sensor answered"}};
  return out;
#else
  return {{"error", "unreadable"}, {"reason", "not a PS5"}};
#endif
}

namespace {

std::string day(time_t t) {
  struct tm tm {};
  gmtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

double mb(long long bytes) { return static_cast<double>(bytes * 10 / (1000LL * 1000)) / 10; }

std::vector<std::string> entries(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d))
      if (e->d_name[0] != '.') out.emplace_back(e->d_name);
    ::closedir(d);
  }
  return out;
}

/** Files in `dir` and one level below (a save's sce_sys/), without going further. */
long long folderBytes(const std::string& dir) {
  long long total = 0;
  for (const auto& name : entries(dir)) {
    const std::string path = dir + "/" + name;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) continue;
    if (S_ISREG(st.st_mode)) total += st.st_size;
    else if (S_ISDIR(st.st_mode))
      for (const auto& inner : entries(path)) {
        struct stat ist {};
        if (::stat((path + "/" + inner).c_str(), &ist) == 0 && S_ISREG(ist.st_mode)) total += ist.st_size;
      }
  }
  return total;
}

std::map<std::string, std::string> nameMap(const Titles& names) {
  std::map<std::string, std::string> m;
  for (const auto& t : names.titles) m[t.id] = t.name;
  return m;
}

/** "Shot.jxr" and its thumbnail "Shot.jxr.jxr" are one shot. */
std::string stem(std::string name) {
  for (auto dot = name.rfind('.'); dot != std::string::npos; dot = name.rfind('.')) {
    std::string ext = name.substr(dot);
    if (ext != ".jxr" && ext != ".jpg" && ext != ".jpeg") break;
    name.erase(dot);
  }
  return name;
}

struct Shot {
  time_t taken;
  long long bytes;
};

void walkShots(const std::string& dir, int depth, bool thumbnails, std::set<std::string>& seen, std::vector<Shot>& out) {
  if (depth <= 0 || out.size() > 100000) return;
  for (const auto& name : entries(dir)) {
    const std::string path = dir + "/" + name;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      walkShots(path, depth - 1, thumbnails, seen, out);
      continue;
    }
    auto dot = name.rfind('.');
    if (dot == std::string::npos) continue;
    std::string ext = name.substr(dot);
    if (ext != ".jxr" && ext != ".jpg" && ext != ".jpeg") continue;  // not the .dat/.meta sidecars
    // Full-size shots are walked first; a thumbnail counts only if its shot is gone.
    if (!seen.insert(stem(name)).second && thumbnails) continue;
    out.push_back({st.st_mtime, st.st_size});
  }
}

}  // namespace

nlohmann::json saves(const Titles& names, const Paths& paths) {
  const auto known = nameMap(names);
  std::map<std::string, std::string> icons;
  for (const auto& t : names.titles)
    if (!t.icon.empty()) icons[t.id] = t.icon;
  struct Save {
    std::string id;
    bool ps5;
    long long bytes;
    time_t saved;
  };
  std::vector<Save> found;
  for (const auto& user : entries(paths.home)) {
    // Users are 8 hex digits ("179a0cd8").
    if (user.size() != 8 || user.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
    for (bool ps5 : {true, false}) {
      const std::string root = paths.home + "/" + user + (ps5 ? "/savedata_prospero" : "/savedata");
      for (const auto& id : entries(root)) {
        const std::string path = root + "/" + id;
        struct stat st {};
        if (!validTitleId(id) || ::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        found.push_back({id, ps5, folderBytes(path), st.st_mtime});
      }
    }
  }
  std::sort(found.begin(), found.end(), [](const Save& a, const Save& b) { return a.saved > b.saved; });
  json list = json::array();
  for (const auto& sv : found) {
    auto it = known.find(sv.id);
    json row = {{"id", sv.id}, {"title", it != known.end() ? it->second : sv.id}, {"platform", sv.ps5 ? "PS5" : "PS4"}, {"sizeMB", mb(sv.bytes)}, {"lastSaved", day(sv.saved)}};
    auto ic = icons.find(sv.id);
    if (ic != icons.end()) row["icon"] = ic->second;  // same cover list_games gives, when installed
    list.push_back(row);
  }
  if (list.empty() && entries(paths.home).empty()) return {{"error", "unreadable"}, {"reason", "can't see " + paths.home}};
  return {{"saves", list}};
}

nlohmann::json screenshots(const Paths& paths) {
  std::set<std::string> seen;
  std::vector<Shot> shots;
  walkShots(paths.photos, 5, false, seen, shots);
  walkShots(paths.thumbnails, 5, true, seen, shots);
  std::sort(shots.begin(), shots.end(), [](const Shot& a, const Shot& b) { return a.taken > b.taken; });
  json newest = json::array();
  for (size_t i = 0; i < shots.size() && i < 5; ++i) newest.push_back({{"taken", day(shots[i].taken)}, {"sizeMB", mb(shots[i].bytes)}});
  return {{"count", shots.size()}, {"newest", newest}};
}

nlohmann::json playActivity(const Titles& names, const Paths& paths, std::string* schema) {
  std::string how;
  sqlite3* db = openReadOnly(paths.playLog, how);
  if (!db) return {{"error", "unreadable"}, {"reason", ::access(paths.playLog.c_str(), F_OK) != 0 ? "can't see the play log" : "not allowed to read the play log"}};
  if (schema) {
    *schema = "tbl_log(";
    auto cols = columnsOf(db, "tbl_log");
    for (size_t i = 0; i < cols.size(); ++i) *schema += (i ? ", " : "") + cols[i];
    *schema += ")";
  }
  std::map<std::string, const Title*> known;
  for (const auto& t : names.titles) known[t.id] = &t;
  // Newest first; each row is one session, so a title's sessions add up.
  std::vector<std::string> order;
  std::map<std::string, long long> summed;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT log FROM tbl_log WHERE event_id = 'ApplicationSessionEndBi' ORDER BY rowid DESC LIMIT 5000", -1, &st, nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      auto text = sqlite3_column_text(st, 0);
      if (!text) continue;
      json log = json::parse(reinterpret_cast<const char*>(text), nullptr, false);
      if (!log.is_object() || !log.contains("appTitleId") || !log["appTitleId"].is_string()) continue;
      const std::string id = log["appTitleId"];
      if (!listed(id)) continue;
      if (!summed.count(id)) order.push_back(id);
      if (log.contains("totalFgTime") && log["totalFgTime"].is_number()) summed[id] += log["totalFgTime"].get<long long>();
      else summed[id] += 0;
    }
    sqlite3_finalize(st);
  }
  json recent = json::array();
  for (const auto& id : order) {
    if (recent.size() >= 20) break;
    auto it = known.find(id);
    const Title* t = it != known.end() ? it->second : nullptr;
    json row = {{"id", id}, {"title", t ? t->name : id}, {"platform", platformOf(id)}};
    if (t && !t->icon.empty()) row["icon"] = t->icon;  // same cover the agent gets from list_games
    row["hoursPlayed"] = hours(t && t->secondsOnConsole >= 0 ? t->secondsOnConsole : summed[id]);
    if (t && !t->lastPlayed.empty()) row["lastPlayed"] = t->lastPlayed;
    recent.push_back(row);
  }
  sqlite3_close(db);
  return {{"recentlyPlayed", recent}};
}

nlohmann::json read(const std::string& name) {
  if (name == "installed_titles") return toJson(consoleTitles());
  if (name == "system_info") return systemInfo();
  if (name == "temperatures") return temperatures();
  if (name == "saves") return saves(consoleTitles());
  if (name == "screenshots") return screenshots();
  if (name == "play_activity") return playActivity(consoleTitles());
  return {{"error", "unknown"}, {"reason", name}};
}

}  // namespace ps5sys
