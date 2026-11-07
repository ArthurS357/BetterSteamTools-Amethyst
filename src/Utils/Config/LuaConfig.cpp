#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/SteamMetadata/StatsClient.h"
#include "Utils/Tickets/AppTicket.h"

#include <lua.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

extern "C" {
    struct lua_State;
}

namespace LuaConfig{
    // ── Concurrency contract ─────────────────────────────────────
    // Lock order, acquire left to right and never the reverse:
    //   ManifestClient::g_mutex -> g_luaMutex -> g_stateMutex
    //   Hooks_Package's g_appIdVecMutex -> g_stateMutex
    // g_luaMutex and g_appIdVecMutex never nest with each other.
    //
    // g_stateMutex: everything below is mutated at runtime (Lua config hot-reload
    // on the LuaFileWatcher / ConfigFileWatcher threads, MarkOwned from the
    // CheckAppOwnership hook) while Steam's IPC / netpacket / pipe threads read
    // it on every message. It guards ALL file-scope config state:
    //   * readers take std::shared_lock, mutators std::unique_lock;
    //   * critical sections are short and touch containers only (plus logging,
    //     which never re-enters LuaConfig). NEVER hold it across Lua
    //     (lua_pcall can run http_get/http_post for seconds), StatsClient/HTTP,
    //     or any hook/callback -- that would stall every IPC message;
    //   * never lock recursively: a public function must not call another
    //     public function that locks (HasDepot uses IsOwnedLocked);
    //   * no luaL_error (longjmp, skips destructors) while the lock is held;
    //   * accessors return copies, never references into the containers.
    //
    // g_luaMutex: the interpreter is not thread-safe, and its global state (GC,
    // string table) is shared by every coroutine. Every Lua API call runs inside
    // a LuaCall, which holds this mutex. The only place it is released mid-call
    // is LuaMutexRelease, around blocking HTTP in http_get/http_post, which no
    // Lua API may be called from.
    //
    // Reload is atomic: while ParseFile runs a script, the lua_* callbacks only
    // record their mutations (t_stagedOps). The commit then swaps the file's old
    // contribution for the new one under one exclusive g_stateMutex section, so a
    // reader sees the old file or the new one, never a gap or a partial parse.
    static std::shared_mutex g_stateMutex;
    static std::mutex g_luaMutex;

    static lua_State* g_lua_state = nullptr;

    // Scope for any code touching Lua: holds g_luaMutex and owns a coroutine, a Lua
    // stack private to this call. That private stack is what lets LuaMutexRelease
    // run other Lua code mid-call: releasing the mutex on a shared stack would let
    // the other caller push onto a frame that is still in use. g_lua_state itself
    // only anchors these coroutines: no chunk or callback is ever pcall'd on it, so an
    // error raised on a coroutine has no main-thread errorJmp to longjmp across threads
    // to. (A script's __gc finalizer is the one thing Lua may run there, when GC fires
    // inside lua_newthread; it is protected and stack-balanced.)
    class LuaCall {
    public:
        LuaCall()
            : lock_(g_luaMutex),
              L_(lua_newthread(g_lua_state)),
              ref_(luaL_ref(g_lua_state, LUA_REGISTRYINDEX)) {}
        ~LuaCall() { luaL_unref(g_lua_state, LUA_REGISTRYINDEX, ref_); }
        LuaCall(const LuaCall&) = delete;
        LuaCall& operator=(const LuaCall&) = delete;

        [[nodiscard]] lua_State* get() const noexcept { return L_; }

    private:
        std::unique_lock<std::mutex> lock_;
        lua_State* L_;
        int ref_;
    };

    // Drops g_luaMutex for the scope of a blocking call made from a Lua C function.
    // No Lua API inside the scope (other threads are running Lua), and no Lua error
    // can skip the re-lock because none can be raised there.
    class LuaMutexRelease {
    public:
        LuaMutexRelease() { g_luaMutex.unlock(); }
        ~LuaMutexRelease() { g_luaMutex.lock(); }
        LuaMutexRelease(const LuaMutexRelease&) = delete;
        LuaMutexRelease& operator=(const LuaMutexRelease&) = delete;
    };

    // Raw on purpose, for lookups made outside lua_pcall: a script can replace _G's
    // metatable, and an __index that raises (strict-mode style) would escape with no
    // handler and abort Steam through the panic function. The contract this sets: only
    // raw fields of _G count, so fetch_manifest_code* defined behind a custom __index
    // or a proxying __newindex is not seen. The default case-insensitive __index never
    // resolved these two names, so ordinary scripts see no difference.
    static void PushRawGlobal(lua_State* L, const char* name) {
        lua_pushglobaltable(L);
        lua_pushstring(L, name);
        lua_rawget(L, -2);
        lua_remove(L, -2);
    }

    // Non-null while this thread runs a file in ParseFile. thread_local, not a
    // global: a manifest callback on another thread may call addappid during that
    // parse, and must not have its mutation attributed to the file being parsed.
    static thread_local std::vector<std::function<void()>>* t_stagedOps = nullptr;

    // Applies a config mutation now, or defers it to the commit of the file this
    // thread is parsing. Scripts cannot read config back, so deferring is invisible
    // to them. `op` runs under exclusive g_stateMutex and must not raise Lua errors.
    template <class Op>
    static void ApplyConfigOp(Op op) {
        if (t_stagedOps) {
            t_stagedOps->emplace_back(std::move(op));
            return;
        }
        std::unique_lock lock(g_stateMutex);
        op();
    }

    static std::atomic<bool> g_hasManifestCodeFunc{false};
    static std::atomic<bool> g_hasManifestCodeFuncEx{false};
    // `static`: no other TU can reach these and bypass g_stateMutex.
    static std::unordered_map<AppId_t, std::string> DepotKeySet{};
    static std::unordered_map<AppId_t, uint64_t> AccessTokenSet{};
    static std::unordered_map<AppId_t, std::string> LegacyCDKeySet{};
    static std::unordered_map<uint64_t, ManifestOverride> ManifestOverrides{};
    static std::unordered_map<AppId_t, uint64_t> StatSteamIdSet{};
    static std::unordered_set<AppId_t> OwnedAppIdSet{};
    // Process exe name (lowercase) → appid; populated by addprocess() in Lua config.
    static std::unordered_map<std::string, AppId_t> ProcessNameAppIdMap{};
    // App IDs that should bypass ProtectionScan and be treated as Denuvo games.
    static std::unordered_set<AppId_t> ForcedDenuvoSet{};
    // On-demand eticket mint endpoint, set via seteticketurl() in Lua config.
    // Empty = disabled (EticketClient falls back to credential-store ticket).
    static std::string EticketUrl{};

    // Per-file tracking: which depots each .lua file contributed.
    // g_currentFile is non-empty only inside a commit, under exclusive g_stateMutex.
    static std::string g_currentFile;
    static std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileDepots;
    static std::unordered_map<std::string, std::unordered_map<uint64_t, ManifestOverride>> g_fileManifestOverrides;
    static std::unordered_map<std::string, uint64_t> g_fileParseSequence;
    static uint64_t g_nextFileParseSequence = 0;
    // Reference count: how many files provide each depot.
    static std::unordered_map<AppId_t, uint32_t> g_depotRefCount;
    // mtime (unix epoch seconds) of each parsed .lua file, captured at ParseFile entry.
    static std::unordered_map<std::string, uint32_t> g_fileMtime;
    // Per-appId purchase time: max(mtime) across every file that currently contributes it.
    // Simple variant: never lowered on UnloadFile unless the refcount drops to zero.
    static std::unordered_map<AppId_t, uint32_t> g_purchaseTime;
    // Depot IDs removed by UnloadFile / added by ParseFile, consumed by NotifyLicenseChanged.
    static std::vector<AppId_t> g_pendingRemovals;
    static std::vector<AppId_t> g_pendingAdditions;
    constexpr uint64_t kDefaultStatSteamId = 76561198028121353ULL;

    // Case-insensitive function registry: lowercase name → C function
    static std::unordered_map<std::string, lua_CFunction> g_func_registry;

    static bool ParseUInt64Decimal(const char* text, uint64_t* out) {
        if (!text || !out) return false;
        const auto parsed = OSTPlatform::Numbers::ParseUInt64(text);
        if (!parsed) return false;
        *out = *parsed;
        return true;
    }

    static uint8 ParseHexByte(std::string_view text) {
        return OSTPlatform::Numbers::ParseHexUInt8(text).value_or(0);
    }

    // The three helpers below require g_stateMutex held exclusively by the caller.
    static void SetActiveManifestOverride(uint64_t depotId, const ManifestOverride& override) {
        ManifestOverrides[depotId] = override;
    }

    static void ClearActiveManifestOverride(uint64_t depotId) {
        ManifestOverrides.erase(depotId);
    }

    static void RebuildManifestOverride(uint64_t depotId) {
        const ManifestOverride* best = nullptr;
        uint64_t bestSeq = 0;

        for (const auto& [file, overrides] : g_fileManifestOverrides) {
            auto overrideIt = overrides.find(depotId);
            if (overrideIt == overrides.end()) continue;

            auto seqIt = g_fileParseSequence.find(file);
            if (seqIt == g_fileParseSequence.end()) continue;

            if (!best || seqIt->second > bestSeq) {
                best = &overrideIt->second;
                bestSeq = seqIt->second;
            }
        }

        if (best) {
            SetActiveManifestOverride(depotId, *best);
        } else {
            ClearActiveManifestOverride(depotId);
        }
    }

    // ── Lua HTTP helpers ──────────────────────────────────────────
    //   http_get(url [, headers]) → body, status_code
    //     headers: optional table, e.g. {["Accept"]="application/json"}
    //
    //   http_post(url, post_body [, headers]) → body, status_code
    //     post_body: string payload
    // ────────────────────────────────────────────────────────────────

    // Serialise a Lua {key=val, ...} table at stack index idx into a
    // wstring of "Key: Val\r\n" lines. Returns L"" if no table.
    static std::wstring LuaHeadersToWstr(lua_State* L, int idx) {
        std::wstring headers;
        if (idx < 1 || !lua_istable(L, idx)) return headers;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            // lua_tostring returns null for a table or boolean, and on a number key
            // converts it in place, which breaks lua_next. Number values are fine
            // ("Content-Length": 5). A malformed entry costs that header, not Steam.
            // The entry is not logged: header values can hold credentials.
            if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) {
                std::string key(lua_tostring(L, -2));
                std::string val(lua_tostring(L, -1));
                headers += std::wstring(key.begin(), key.end())
                        + L": " + std::wstring(val.begin(), val.end()) + L"\r\n";
            } else {
                LOG_WARN("http_get/http_post: skipped a header whose key or value is not a string");
            }
            lua_pop(L, 1);
        }
        return headers;
    }

    static int lua_http_get(lua_State* L) {
        // http_get(url [, headers]) → body, status_code
        // url stays valid while unlocked: slot 1 of this coroutine's stack anchors it.
        const char* url = luaL_checkstring(L, 1);
        auto hdrs = LuaHeadersToWstr(L, lua_gettop(L) >= 2 ? 2 : -1);
        const auto r = [&] {
            LuaMutexRelease unlocked;
            return OSTPlatform::Http::Execute(L"GET", url, nullptr, 0,
                                              hdrs.empty() ? nullptr : hdrs.c_str());
        }();
        if (r.ok) {
            lua_pushstring(L, r.body.c_str());
            lua_pushinteger(L, r.status);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "HTTP request failed");
        }
        return 2;
    }

    static int lua_http_post(lua_State* L) {
        // http_post(url, post_body [, headers]) → body, status_code
        // url and body stay valid while unlocked: this coroutine's stack anchors them.
        size_t bodyLen = 0;
        const char* body = luaL_checklstring(L, 2, &bodyLen);
        const char* url = luaL_checkstring(L, 1);
        auto hdrs = LuaHeadersToWstr(L, lua_gettop(L) >= 3 ? 3 : -1);
        const auto r = [&] {
            LuaMutexRelease unlocked;
            return OSTPlatform::Http::Execute(L"POST", url, body, static_cast<uint32_t>(bodyLen),
                                              hdrs.empty() ? nullptr : hdrs.c_str());
        }();
        if (r.ok) {
            lua_pushstring(L, r.body.c_str());
            lua_pushinteger(L, r.status);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "HTTP request failed");
        }
        return 2;
    }

    // ── Case-insensitive global function lookup ─────────────────
    // __index metamethod on _G: when a global name isn't found,
    // lower-case the name and look it up in g_func_registry.
    static int case_insensitive_global_index(lua_State* L) {
        const char* name = lua_tostring(L, 2);
        if (!name) {
            lua_pushnil(L);
            return 1;
        }
        std::string lower;
        for (const char* p = name; *p; ++p) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        }
        auto it = g_func_registry.find(lower);
        if (it != g_func_registry.end()) {
            lua_pushcfunction(L, it->second);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    // Register a C function both in _G (lowercase name) and in the
    // case-insensitive lookup table so any case variant resolves.
    static void register_func(lua_State* L, const char* lowercase_name, lua_CFunction fn) {
        g_func_registry[lowercase_name] = fn;
        lua_pushcfunction(L, fn);
        lua_setglobal(L, lowercase_name);
    }

    // ── Lua: addappid / addtoken / setManifestid ────────
    static int lua_addappid(lua_State* L) {
        // addappid(integer, integer, string)
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as app/depot id.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t DepotId = (uint32_t)value;
        // Read the optional third argument as a key.
        std::string Key = "";
        if (argc > 2) {
            if (!lua_isstring(L, 3))
                return luaL_error(L, "");
            const char* key = lua_tostring(L, 3);
            // Keep only keys with exactly 64 characters.
            if (strlen(key) == 64) {
                Key = std::string(key);
            }
        }
        // Every error path is above: no luaL_error (longjmp) past this point.
        ApplyConfigOp([DepotId, Key = std::move(Key)] {
            // Non-empty keys have priority over existing empty keys.
            if (!Key.empty() || !DepotKeySet.count(DepotId)) {
                DepotKeySet[DepotId] = Key;
            }

            if (!g_currentFile.empty()) {
                if (g_fileDepots[g_currentFile].insert(DepotId).second) {
                    if (++g_depotRefCount[DepotId] == 1)
                        g_pendingAdditions.push_back(DepotId);

                    // Update the appId's purchase time with the current file's mtime,
                    // keeping the maximum across every contributing file.
                    auto mtIt = g_fileMtime.find(g_currentFile);
                    if (mtIt != g_fileMtime.end()) {
                        uint32_t mt = mtIt->second;
                        auto& slot = g_purchaseTime[DepotId];
                        if (mt > slot) slot = mt;
                    }
                }
            }
        });

        return 0;
    }

    static int lua_addtoken(lua_State* L) {
        // addtoken(integer, string(uint64_t))
        int argc = lua_gettop(L);
        // Validate argument count and required argument types.
        if (argc == 0) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }

        // Read the first argument as app/depot id.
        lua_Integer value = lua_tointeger(L, 1);
        // Ensure the value fits into uint32_t range.
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t AppId = (uint32_t)value;
        // Read the second argument as a token.
        if (argc > 1) {
            if (!lua_isstring(L, 2))
                return luaL_error(L, "");
            const char* token = lua_tostring(L, 2);
            uint64_t parsedToken = 0;
            if (!ParseUInt64Decimal(token, &parsedToken)) {
                return luaL_error(L, "");
            }
            ApplyConfigOp([AppId, parsedToken] { AccessTokenSet[AppId] = parsedToken; });
        }

        return 0;
    }

    static int lua_setlegacycdkey(lua_State* L) {
        // setlegacycdkey(integer appid, string key)
        int argc = lua_gettop(L);
        if (argc < 2) {
            return luaL_error(L, "");
        }
        if (!lua_isinteger(L, 1)) {
            return luaL_error(L, "");
        }
        // Read the first argument as appid.
        lua_Integer value = lua_tointeger(L, 1);
        if (value < 0 || value > UINT32_MAX)
            return luaL_error(L, "");
        AppId_t AppId = (uint32_t)value;
        // Read the second argument as the CD key, stored verbatim.
        if (!lua_isstring(L, 2))
            return luaL_error(L, "");
        ApplyConfigOp([AppId, cdKey = std::string(lua_tostring(L, 2))] { LegacyCDKeySet[AppId] = cdKey; });
        return 0;
    }

    static int lua_addprocess(lua_State* L) {
        // addprocess(appid, "ExeName.exe")
        // Maps a process exe name to an appid so OST can identify games
        // that launch without exporting SteamAppId env vars.
        int argc = lua_gettop(L);
        if (argc < 2 || !lua_isinteger(L, 1) || !lua_isstring(L, 2))
            return luaL_error(L, "addprocess requires (appid: integer, exename: string)");
        lua_Integer value = lua_tointeger(L, 1);
        if (value <= 0 || value > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "addprocess: appid out of range");
        std::string name(lua_tostring(L, 2));
        for (char& ch : name)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        ApplyConfigOp([name = std::move(name), value] {
            ProcessNameAppIdMap[name] = static_cast<AppId_t>(value);
        });
        return 0;
    }

    static int lua_forcedenuvo(lua_State* L) {
        // forcedenuvo(appid) — bypass ProtectionScan for games where the heuristic fails.
        if (lua_gettop(L) < 1 || !lua_isinteger(L, 1))
            return luaL_error(L, "forcedenuvo requires (appid: integer)");
        lua_Integer value = lua_tointeger(L, 1);
        if (value <= 0 || value > static_cast<lua_Integer>(UINT32_MAX))
            return luaL_error(L, "forcedenuvo: appid out of range");
        ApplyConfigOp([value] { ForcedDenuvoSet.insert(static_cast<AppId_t>(value)); });
        return 0;
    }

    static int lua_seteticketurl(lua_State* L) {
        // seteticketurl("http://your-backend/eticket")
        // Endpoint that mints fresh nonce-bound encrypted app tickets for
        // strict Denuvo titles. Set to "" (or omit the call) to disable.
        if (lua_gettop(L) < 1 || !lua_isstring(L, 1))
            return luaL_error(L, "seteticketurl requires (url: string)");
        ApplyConfigOp([url = std::string(lua_tostring(L, 1))] { EticketUrl = url; });
        return 0;
    }

    static int lua_setManifestid(lua_State* L)
    {
        // setManifestid(depotId, gid_string [, size])
        // size is always forced to 0 to prevent incorrect size from breaking Steam.
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setManifestid: need depotId, gid");

        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setManifestid: depotId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setManifestid: gid must be decimal string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setManifestid: depotId out of range");

        uint64_t depotId = (uint64_t)(uint32_t)val;
        const char* gidStr = lua_tostring(L, 2);

        uint64_t gid = 0;
        if (!ParseUInt64Decimal(gidStr, &gid))
            return luaL_error(L, "setManifestid: gid must be all digits");

        ApplyConfigOp([depotId, override = ManifestOverride{ gid, 0 }] {
            if (!g_currentFile.empty()) {
                g_fileManifestOverrides[g_currentFile][depotId] = override;
                RebuildManifestOverride(depotId);
            } else {
                SetActiveManifestOverride(depotId, override);
            }
        });
        return 0;
    }

    // ── Lua: setAppTicket / setETicket ──────────────────────────
    static int lua_setAppticket(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setAppTicket: need appId and hex string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setAppTicket: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setAppTicket: ticket must be hex string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setAppTicket: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        size_t hexLen;
        const char* hex = lua_tolstring(L, 2, &hexLen);

        std::vector<uint8_t> binary;
        binary.reserve((hexLen + 1) / 2);
        for (size_t i = 0; i < hexLen; i += 2) {
            char byteStr[3] = {
                hex[i],
                i + 1 < hexLen ? hex[i + 1] : '0',
                '\0'
            };
            binary.push_back(ParseHexByte(byteStr));
        }

        if (!AppTicket::WriteAppOwnershipTicket(appId, binary))
            return luaL_error(L, "setAppTicket: failed to write credential store");

        return 0;
    }

    static int lua_setEticket(lua_State* L) {
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setETicket: need appId and hex string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setETicket: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setETicket: ticket must be hex string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setETicket: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        size_t hexLen;
        const char* hex = lua_tolstring(L, 2, &hexLen);

        std::vector<uint8_t> binary;
        binary.reserve((hexLen + 1) / 2);
        for (size_t i = 0; i < hexLen; i += 2) {
            char byteStr[3] = {
                hex[i],
                i + 1 < hexLen ? hex[i + 1] : '0',
                '\0'
            };
            binary.push_back(ParseHexByte(byteStr));
        }

        if (!AppTicket::WriteEncryptedTicket(appId, binary))
            return luaL_error(L, "setETicket: failed to write credential store");

        return 0;
    }

    
    // ── Lua: setStat ────────────────────────────────────────────
    static int lua_setStat(lua_State* L) {
        // setStat(appid, "steamid")
        int argc = lua_gettop(L);
        if (argc < 2)
            return luaL_error(L, "setStat: need appId and steamId string");
        if (!lua_isinteger(L, 1))
            return luaL_error(L, "setStat: appId must be integer");
        if (!lua_isstring(L, 2))
            return luaL_error(L, "setStat: steamId must be string");

        lua_Integer val = lua_tointeger(L, 1);
        if (val < 0 || val > UINT32_MAX)
            return luaL_error(L, "setStat: appId out of range");
        AppId_t appId = static_cast<uint32_t>(val);

        const char* sidStr = lua_tostring(L, 2);
        uint64_t steamId = 0;
        if (!ParseUInt64Decimal(sidStr, &steamId))
            return luaL_error(L, "setStat: steamId must be all digits");

        ApplyConfigOp([appId, steamId] { StatSteamIdSet[appId] = steamId; });
        return 0;
    }

    // ── init ─────────────────────────────────────────────────────
    static bool Initialize() {
        std::scoped_lock lock(g_luaMutex);
        if (g_lua_state)
            return true;
        g_lua_state = luaL_newstate();
        if (!g_lua_state)
            return false;
        // Load standard Lua libraries.
        luaL_openlibs(g_lua_state);

        // Set up case-insensitive global lookup via __index on _G's metatable.
        lua_getglobal(g_lua_state, "_G");
        if (!lua_getmetatable(g_lua_state, -1)) {
            lua_newtable(g_lua_state);
        }
        lua_pushcfunction(g_lua_state, case_insensitive_global_index);
        lua_setfield(g_lua_state, -2, "__index");
        lua_setmetatable(g_lua_state, -2);
        lua_pop(g_lua_state, 1);  // pop _G

        // Register custom helper functions for scripts.
        // All functions use register_func() so any case variant works
        // (e.g. setAppTICKET, addAppId, SETManifestid, etc.).
        register_func(g_lua_state, "addappid", lua_addappid);
        register_func(g_lua_state, "addtoken", lua_addtoken);
        register_func(g_lua_state, "setlegacycdkey", lua_setlegacycdkey);
        register_func(g_lua_state, "addprocess", lua_addprocess);
        register_func(g_lua_state, "forcedenuvo", lua_forcedenuvo);
        register_func(g_lua_state, "seteticketurl", lua_seteticketurl);
        register_func(g_lua_state, "setmanifestid", lua_setManifestid);
        register_func(g_lua_state, "http_get", lua_http_get);
        register_func(g_lua_state, "http_post", lua_http_post);
        register_func(g_lua_state, "setappticket", lua_setAppticket);
        register_func(g_lua_state, "seteticket", lua_setEticket);
        register_func(g_lua_state, "setstat", lua_setStat);
        return true;
    }

    // ── public query API ─────────────────────────────────────────
    AppId_t GetAppIdForProcess(const std::string& imageName) {
        std::string lower(imageName);
        for (char& ch : lower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::shared_lock lock(g_stateMutex);
        const auto it = ProcessNameAppIdMap.find(lower);
        return it != ProcessNameAppIdMap.end() ? it->second : k_uAppIdInvalid;
    }

    bool IsForcedDenuvo(AppId_t appId) {
        std::shared_lock lock(g_stateMutex);
        return ForcedDenuvoSet.count(appId) > 0;
    }

    std::string GetEticketUrl() {
        std::shared_lock lock(g_stateMutex);
        return EticketUrl;
    }

    // Caller holds g_stateMutex (shared or exclusive).
    static bool IsOwnedLocked(AppId_t AppId) {
        return OwnedAppIdSet.count(AppId) > 0;
    }

    bool HasDepot(AppId_t DepotId,bool excludeOwned) {
        std::shared_lock lock(g_stateMutex);
        return DepotKeySet.count(DepotId) && (!excludeOwned || !IsOwnedLocked(DepotId));
    }

    bool IsOwned(AppId_t AppId) {
        std::shared_lock lock(g_stateMutex);
        return IsOwnedLocked(AppId);
    }

    void MarkOwned(AppId_t AppId) {
        bool inserted = false;
        {
            std::unique_lock lock(g_stateMutex);
            inserted = OwnedAppIdSet.insert(AppId).second;
        }
        if (inserted) {
            LOG_PACKAGE_INFO("Marking app {} as owned", AppId);
        }
    }

    std::vector<AppId_t> GetAllDepotIds() {
        std::shared_lock lock(g_stateMutex);
        std::vector<AppId_t> DepotIds;
        DepotIds.reserve(DepotKeySet.size());
        for (const auto& pair : DepotKeySet) {
            DepotIds.push_back(pair.first);
        }
        return DepotIds;
    }

    std::vector<uint8> GetDecryptionKey(AppId_t DepotId) {
        std::vector<uint8> keyBytes;
        std::shared_lock lock(g_stateMutex);
        const auto it = DepotKeySet.find(DepotId);
        if (it != DepotKeySet.end()) {
            const std::string& keyStr = it->second;  // valid only while the lock is held
            // Convert hex string to byte vector.
            for (size_t i = 0; i < keyStr.length(); i += 2) {
                keyBytes.push_back(ParseHexByte(std::string_view(keyStr).substr(i, 2)));
            }
        }
        return keyBytes;
    }

    uint64_t GetAccessToken(AppId_t AppId) {
        std::shared_lock lock(g_stateMutex);
        const auto it = AccessTokenSet.find(AppId);
        return it != AccessTokenSet.end() ? it->second : 0;
    }

    std::optional<std::string> GetLegacyCDKey(AppId_t AppId) {
        std::shared_lock lock(g_stateMutex);
        auto it = LegacyCDKeySet.find(AppId);
        if (it != LegacyCDKeySet.end())
            return it->second;
        return std::nullopt;
    }

    uint64_t GetStatSteamId(AppId_t AppId) {
        {
            std::shared_lock lock(g_stateMutex);
            const auto it = StatSteamIdSet.find(AppId);
            if (it != StatSteamIdSet.end())
                return it->second;
        }
        // Network call: must run with the lock released.
        uint64_t apiSteamId = 0;
        if (StatsClient::FetchStatSteamId(AppId, &apiSteamId))
            return apiSteamId;
        return kDefaultStatSteamId;
    }

    uint32_t GetPurchaseTime(AppId_t AppId) {
        std::shared_lock lock(g_stateMutex);
        auto it = g_purchaseTime.find(AppId);
        return it != g_purchaseTime.end() ? it->second : 0;
    }

    // ponytail: returns a copy of the whole table (a reference into ManifestOverrides
    // would dangle across a reload). Only BuildDepotDependency needs the table, once
    // per call, and it is a handful of lua-pinned depots; if it ever grows large, add
    // a per-depot GetManifestOverride(depotId) lookup.
    std::unordered_map<uint64_t, ManifestOverride> GetManifestOverrides() {
        std::shared_lock lock(g_stateMutex);
        return ManifestOverrides;
    }

    bool HasManifestOverride(uint64_t depotId) {
        std::shared_lock lock(g_stateMutex);
        return ManifestOverrides.count(depotId) != 0;
    }

    bool HasManifestCodeFunc() {
        return g_hasManifestCodeFunc;
    }

    bool CallManifestFetchCode(uint64_t gid, uint64_t* outCode) {
        // The flag is only ever set after g_lua_state exists.
        if (!g_hasManifestCodeFunc)
            return false;

        LuaCall call;
        lua_State* L = call.get();

        PushRawGlobal(L, "fetch_manifest_code");
        lua_pushinteger(L, static_cast<lua_Integer>(gid));

        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) error: {}", gid,
                             lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }

        // nil → fallback to config url (normal, not an error)
        if (lua_isnil(L, -1)) {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) returned nil", gid);
            lua_pop(L, 1);
            return false;
        }

        // The function must return a digit string (uint64 as decimal).
        // tonumber() loses precision for values > 2^53; string is safe.
        if (lua_isinteger(L, -1)) {
            *outCode = static_cast<uint64_t>(lua_tointeger(L, -1));
        } else if (lua_isstring(L, -1)) {
            const char* s = lua_tostring(L, -1);
            uint64_t parsed = 0;
            if (!ParseUInt64Decimal(s, &parsed)) {
                LOG_MANIFEST_WARN("fetch_manifest_code({}) returned invalid numeric string '{}'",
                                 gid, s);
                lua_pop(L, 1);
                return false;
            }
            *outCode = parsed;
        } else {
            LOG_MANIFEST_WARN("fetch_manifest_code({}) unexpected type (expected digit-string)",
                             gid);
            lua_pop(L, 1);
            return false;
        }

        LOG_MANIFEST_INFO("fetch_manifest_code({}) = {}", gid, *outCode);
        lua_pop(L, 1);
        return true;
    }

    bool HasManifestCodeFuncEx() {
        return g_hasManifestCodeFuncEx;
    }

    bool CallManifestFetchCodeEx(uint64_t app_id, uint64_t depot_id, uint64_t gid, uint64_t* outCode) {
        if (!g_hasManifestCodeFuncEx)
            return false;

        LuaCall call;
        lua_State* L = call.get();

        PushRawGlobal(L, "fetch_manifest_code_ex");
        lua_pushinteger(L, static_cast<lua_Integer>(app_id));
        lua_pushinteger(L, static_cast<lua_Integer>(depot_id));
        lua_pushinteger(L, static_cast<lua_Integer>(gid));

        if (lua_pcall(L, 3, 1, 0) != LUA_OK) {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) error: {}", app_id, depot_id, gid,
                             lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }

        if (lua_isnil(L, -1)) {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) returned nil", app_id, depot_id, gid);
            lua_pop(L, 1);
            return false;
        }

        if (lua_isinteger(L, -1)) {
            *outCode = static_cast<uint64_t>(lua_tointeger(L, -1));
        } else if (lua_isstring(L, -1)) {
            const char* s = lua_tostring(L, -1);
            uint64_t parsed = 0;
            if (!ParseUInt64Decimal(s, &parsed)) {
                LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) returned invalid numeric string '{}'",
                                 app_id, depot_id, gid, s);
                lua_pop(L, 1);
                return false;
            }
            *outCode = parsed;
        } else {
            LOG_MANIFEST_WARN("fetch_manifest_code_ex({}, {}, {}) unexpected type (expected digit-string)",
                             app_id, depot_id, gid);
            lua_pop(L, 1);
            return false;
        }

        LOG_MANIFEST_INFO("fetch_manifest_code_ex({}, {}, {}) = {}", app_id, depot_id, gid, *outCode);
        lua_pop(L, 1);
        return true;
    }

    // ── per-file unload ────────────────────────────────────────
    struct UnloadStats {
        size_t depots = 0;
        size_t overrides = 0;
    };

    // Caller holds g_stateMutex exclusively. DepotKeySet / g_depotRefCount /
    // g_fileDepots must change together or a reader could see a depot whose
    // bookkeeping is already gone. No Lua, network or logging in here (a
    // debug-level log is a file write); log the returned counts after unlocking.
    static UnloadStats UnloadFileLocked(const std::string& filePath) {
        UnloadStats stats;
        auto depotsIt = g_fileDepots.find(filePath);
        auto manifestIt = g_fileManifestOverrides.find(filePath);
        if (depotsIt == g_fileDepots.end() && manifestIt == g_fileManifestOverrides.end()) return stats;

        if (depotsIt != g_fileDepots.end()) {
            for (AppId_t id : depotsIt->second) {
                if (--g_depotRefCount[id] == 0) {
                    g_depotRefCount.erase(id);
                    DepotKeySet.erase(id);
                    g_purchaseTime.erase(id);
                    g_pendingRemovals.push_back(id);
                }
            }

            stats.depots = depotsIt->second.size();
            g_fileDepots.erase(depotsIt);
        }

        if (manifestIt != g_fileManifestOverrides.end()) {
            std::vector<uint64_t> affectedDepots;
            affectedDepots.reserve(manifestIt->second.size());
            for (const auto& [depotId, _] : manifestIt->second) {
                affectedDepots.push_back(depotId);
            }

            g_fileManifestOverrides.erase(manifestIt);
            for (uint64_t depotId : affectedDepots) {
                RebuildManifestOverride(depotId);
            }

            stats.overrides = affectedDepots.size();
        }

        g_fileParseSequence.erase(filePath);
        g_fileMtime.erase(filePath);
        return stats;
    }

    static void LogUnload([[maybe_unused]] const std::string& filePath, const UnloadStats& stats) {
        if (stats.depots)
            LOG_PACKAGE_INFO("UnloadFile: removed {} depots from {}", stats.depots, filePath);
        if (stats.overrides)
            LOG_MANIFEST_INFO("UnloadFile: removed {} manifest override(s) from {}", stats.overrides, filePath);
    }

    void UnloadFile(const std::string& filePath) {
        UnloadStats stats;
        {
            std::unique_lock lock(g_stateMutex);
            stats = UnloadFileLocked(filePath);
        }
        LogUnload(filePath, stats);
    }

    std::vector<AppId_t> TakePendingRemovals() {
        std::vector<AppId_t> result;
        std::unique_lock lock(g_stateMutex);
        result.swap(g_pendingRemovals);
        return result;
    }

    std::vector<AppId_t> TakePendingAdditions() {
        std::vector<AppId_t> result;
        std::unique_lock lock(g_stateMutex);
        result.swap(g_pendingAdditions);
        return result;
    }

    static std::vector<std::string> CollectLuaFiles(const std::string& directory) {
        std::vector<std::string> files;

        // directory is UTF-8 (threaded through from LuaDir/[lua].paths in dllmain.cpp,
        // ultimately OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath() or TOML
        // content). Decode once via Utf8ToPath instead of letting each call below
        // implicitly construct a path(std::string) (ANSI codepage on MSVC) -- for a
        // non-ASCII Steam install path that silently enumerates zero files instead of
        // the real stplug-in directory.
        const std::filesystem::path dirPath = OSTPlatform::Encoding::Utf8ToPath(directory);

        std::error_code ec;
        if (!std::filesystem::exists(dirPath, ec))
            std::filesystem::create_directories(dirPath, ec);
        if (!std::filesystem::exists(dirPath, ec) || !std::filesystem::is_directory(dirPath, ec))
            return files;

        // entry.path() here is native (wide) from directory_iterator. Narrow it via
        // PathToUtf8 (not .string(), which is ANSI-codepage) so this list is on the
        // same footing as LuaFileWatcher::ToFileChanges, which builds its paths as
        // UTF-8 (dir + Encoding::WideToUtf8(relativePath) from ReadDirectoryChangesW).
        // Both producers feeding ParseFile/UnloadFile must agree on encoding: besides
        // the open() call inside ParseFile, filePath is also used as a map key there,
        // so a startup scan (ANSI) and a later hot-reload (UTF-8) of the same
        // non-ASCII path used to decode to different byte strings -- silently
        // duplicating the entry instead of replacing it.
        for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".lua") continue;
            files.push_back(OSTPlatform::Encoding::PathToUtf8(entry.path()));
        }
        return files;
    }

    // ── single-file parser ──────────────────────────────────────
    void ParseFile(const std::string& filePath) {
        if (!Initialize()) return;

        // filePath is UTF-8 from both callers (CollectLuaFiles' startup scan and
        // LuaFileWatcher's hot-reload -- see the comment there); decode via
        // Utf8ToPath rather than the ANSI-codepage path(std::string) constructor.
        const std::filesystem::path path = OSTPlatform::Encoding::Utf8ToPath(filePath);
        std::ifstream file(path);
        if (!file) {
            // Fail-safe: a writer holding the file mid-save must not unload the games
            // it contributed. A real deletion arrives as a Removed event (UnloadFile).
            LOG_WARN("ParseFile: failed to open {}, keeping any loaded version",
                     path.filename().string());
            return;
        }

        // Capture the file's last-modified time (unix epoch, seconds) so
        // lua_addappid can stamp it onto every appId this file contributes.
        // Portable conversion that does not require C++20 clock_cast.
        uint32_t mtime = 0;
        {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(path, ec);
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
                mtime = static_cast<uint32_t>(std::chrono::system_clock::to_time_t(sctp));
            }
        }

        // Neither lock is held across HTTP: lua_pcall may block in http_get/http_post
        // (which release g_luaMutex), and g_stateMutex is not taken until the commit.
        std::vector<std::function<void()>> stagedOps;
        bool incomplete = false;
        {
            LuaCall call;
            lua_State* L = call.get();

            t_stagedOps = &stagedOps;
            std::string chunk, line;
            int lineNo = 0;
            while (std::getline(file, line)) {
                ++lineNo;
                if (!chunk.empty()) chunk += '\n';
                chunk += line;

                lua_settop(L, 0);
                int rc = luaL_loadstring(L, chunk.c_str());
                if (rc == LUA_OK) {
                    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                        [[maybe_unused]] const char* err = lua_tostring(L, -1);
                        LOG_WARN("{}:{}: {}", path.filename().string(), lineNo,
                                 err ? err : "unknown");
                    }
                    chunk.clear();
                } else if (rc == LUA_ERRSYNTAX) {
                    lua_pop(L, 1);
                } else {
                    [[maybe_unused]] const char* err = lua_tostring(L, -1);
                    LOG_WARN("{}:{}: {}", path.filename().string(), lineNo, err ? err : "unknown");
                    lua_pop(L, 1);
                    chunk.clear();
                }
            }
            t_stagedOps = nullptr;
            lua_settop(L, 0);

            // Zero lines counts: truncating is the first step of a non-atomic save.
            incomplete = lineNo == 0 || !chunk.empty() || file.bad();
            if (incomplete) {
                LOG_WARN("{}: empty, or ends mid-statement", path.filename().string());
            }

            // Check for manifest code functions after parsing. Each flag is stored
            // once (not reset to false first) so a concurrent HasManifestCodeFunc*()
            // never sees a transient false while the function is still defined.
            PushRawGlobal(L, "fetch_manifest_code");
            const bool hasCodeFunc = lua_isfunction(L, -1);
            lua_pop(L, 1);
            g_hasManifestCodeFunc = hasCodeFunc;
            if (hasCodeFunc) {
                LOG_INFO("manifest.lua: fetch_manifest_code found");
            }

            PushRawGlobal(L, "fetch_manifest_code_ex");
            const bool hasCodeFuncEx = lua_isfunction(L, -1);
            lua_pop(L, 1);
            g_hasManifestCodeFuncEx = hasCodeFuncEx;
            if (hasCodeFuncEx) {
                LOG_INFO("manifest.lua: fetch_manifest_code_ex found");
            }
        }

        // Commit: readers see the old file or the new one, nothing in between.
        // A truncated file (half-saved, or a syntax error that swallowed the rest)
        // must not replace a loaded version. With nothing loaded yet its complete
        // statements still apply, and a failing statement never blocks a reload: one
        // bad line in an otherwise valid file has always been skipped, not fatal.
        // ponytail: truncation that lands exactly on a statement boundary looks like a
        // complete file; only an atomic replace by the writer can rule that out. A file
        // emptied on purpose keeps its old version until deleted or Steam restarts.
        // Discarding also only covers config state: setAppTicket/setETicket and Lua
        // globals took effect during the run, and are redone by the next good parse.
        UnloadStats stats;
        bool keptPrevious = false;
        {
            std::unique_lock lock(g_stateMutex);
            keptPrevious = incomplete && (g_fileDepots.contains(filePath) ||
                                          g_fileManifestOverrides.contains(filePath));
            if (!keptPrevious) {
                stats = UnloadFileLocked(filePath);
                g_fileParseSequence[filePath] = ++g_nextFileParseSequence;
                g_fileMtime[filePath] = mtime;
                g_currentFile = filePath;
                for (const auto& op : stagedOps) {
                    op();
                }
                g_currentFile.clear();
            }
        }

        LogUnload(filePath, stats);
        if (keptPrevious) {
            LOG_WARN("{}: keeping the previously loaded version", path.filename().string());
        }
    }

    // ── directory scanner ────────────────────────────────────────
    std::vector<std::string> MergeWatchDirs(const std::vector<std::string>& configured,
                                            const std::string& defaultDir) {
        namespace fs = std::filesystem;

        // Canonical, case-folded key for a directory so relative and absolute spellings
        // of the same location compare equal. weakly_canonical resolves against the
        // current working directory (Steam's install root at runtime), matching how the
        // paths are later opened.
        auto key = [](const std::string& p) -> std::string {
            // p is UTF-8 (same origin as CollectLuaFiles's directory); decode once via
            // Utf8ToPath rather than the ANSI-codepage path(std::string) constructor,
            // matching how CollectLuaFiles/ParseDirectory later open the same directory.
            const std::filesystem::path utf8Path = OSTPlatform::Encoding::Utf8ToPath(p);
            std::error_code ec;
            fs::path c = fs::weakly_canonical(utf8Path, ec);
            if (ec || c.empty()) c = fs::absolute(utf8Path, ec);
            if (ec || c.empty()) c = utf8Path.lexically_normal();
            std::string s = OSTPlatform::Encoding::PathToUtf8(c);
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return s;
        };

        std::vector<std::string> out;
        std::vector<std::string> seen;
        auto add = [&](const std::string& dir) {
            if (dir.empty()) return;
            const std::string k = key(dir);
            if (std::find(seen.begin(), seen.end(), k) != seen.end()) return;
            seen.push_back(k);
            out.push_back(dir);
        };

        for (const auto& d : configured) add(d);
        add(defaultDir);   // appended only if it isn't already covered above
        return out;
    }

    void ParseDirectory(const std::string& directory) {
        if (!Initialize()) return;

        for (const auto& filePath : CollectLuaFiles(directory)) {
            ParseFile(filePath);
        }

        // Initial parse — discard pending additions so NotifyLicenseChanged
        // only sees changes that happen after startup.
        std::unique_lock lock(g_stateMutex);
        g_pendingAdditions.clear();
    }

    void ReloadDirectories(const std::vector<std::string>& directories, bool clearPendingAdditions) {
        if (!Initialize()) return;

        std::unordered_set<std::string> activeFiles;
        std::vector<std::string> orderedFiles;

        for (const auto& directory : directories) {
            for (const auto& filePath : CollectLuaFiles(directory)) {
                if (activeFiles.insert(filePath).second) {
                    orderedFiles.push_back(filePath);
                }
            }
        }

        std::unordered_set<std::string> trackedSet;
        std::vector<std::string> trackedFiles;
        auto rememberTracked = [&](const std::string& filePath) {
            if (trackedSet.insert(filePath).second) {
                trackedFiles.push_back(filePath);
            }
        };

        {
            // Snapshot the tracked file names, then release before UnloadFile/
            // ParseFile (which lock exclusively themselves).
            std::shared_lock lock(g_stateMutex);
            for (const auto& [filePath, _] : g_fileDepots) {
                rememberTracked(filePath);
            }
            for (const auto& [filePath, _] : g_fileManifestOverrides) {
                rememberTracked(filePath);
            }
        }

        for (const auto& filePath : trackedFiles) {
            if (!activeFiles.contains(filePath)) {
                UnloadFile(filePath);
            }
        }

        for (const auto& filePath : orderedFiles) {
            ParseFile(filePath);
        }

        if (clearPendingAdditions) {
            std::unique_lock lock(g_stateMutex);
            g_pendingAdditions.clear();
        }
    }

}
