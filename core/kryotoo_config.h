// kryoto-online.ini, read once at startup.
//
// The file is looked for beside the RUNNING EXECUTABLE, not beside
// this DLL. Those are the same folder for a plain game and are not
// for a Unity one, where the Steamworks DLL lives under
// <Game>_Data\Plugins\x86_64\ and the exe sits at the root. Reading
// it from beside the DLL is the bug that made Unity titles launch
// with every setting silently at its default.
//
// A missing ini is not an error - the defaults are a working
// configuration for a game that needs nothing but the Spacewar
// spoof. It IS reported (KryotoO_Config::HaveIni) because "I wrote
// the ini and nothing changed" is nearly always the ini being in
// the wrong folder.
#pragma once

#include <Windows.h>
#include <Shlwapi.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include <vector>

namespace kryotoo {

/// Spacewar. Free, so every account already owns it, which is the
/// whole trick: the ownership check passes against a title nobody
/// has to buy.
constexpr uint32_t kSpacewarAppId = 480;

class Config
{
public:
    /// Locate the ini. Safe to call when there is no ini; every
    /// getter then returns its default.
    void Read()
    {
        m_IniPath[0] = '\0';

        char exeDir[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(nullptr, exeDir, MAX_PATH) == 0)
            return;
        if (!PathRemoveFileSpecA(exeDir))
            return;

        char path[MAX_PATH] = { 0 };
        if (_snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\kryoto-online.ini", exeDir) == _TRUNCATE)
            return;

        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            return;

        memcpy(m_IniPath, path, sizeof(path));
    }

    bool HaveIni() const { return m_IniPath[0] != '\0'; }
    const char* IniPath() const { return m_IniPath; }

    /// What the game is told it is.
    uint32_t AppId() const
    {
        uint32_t id = ReadU32("AppId", kSpacewarAppId);
        // Zero would spoof the game as "no app", which resolves to
        // nothing and fails in a way that looks like the DLL never
        // loaded. Treat a junk value as unset.
        return id == 0 ? kSpacewarAppId : id;
    }

    /// What it really is. Zero means unset, and the spoof hooks
    /// stand down entirely when it is - there is nothing to map to.
    uint32_t OgAppId() const { return ReadU32("ogAppId", 0); }

    bool EmulateTicket() const { return ReadBool("EmulateTicket", false); }

    /// Defeat SteamStub at runtime by flipping its ownership branch.
    ///
    /// Off by default because the packaged path strips the stub with
    /// Steamless instead, and patching a stub that is not there is a
    /// hook scanning for a signature it will never find. Turn it on
    /// for a build whose executable is still wrapped.
    bool SteamStubEnabled() const { return ReadBool("GetStubbedLol", false); }

    /// Keep this game's lobbies apart from every other title sharing
    /// the spoofed AppId. On by default whenever there is a spoof to
    /// speak of; see KryotoO_Config::LobbyIsolation.
    bool LobbyIsolation() const { return ReadBool("LobbyIsolation", true); }

    /// Search lobbies worldwide unless the game asked for a distance
    /// itself. Steam's default is "nearby regions", which on a pool as
    /// small as one cracked title is usually nobody.
    bool LobbyWorldwide() const { return ReadBool("LobbyWorldwide", true); }

    /// What this game's lobbies are tagged with: [Settings]LobbyTag,
    /// else ogAppId, else the executable's name. Two installs only see
    /// each other's lobbies when this matches, so the defaults are the
    /// things two copies of one game agree on without being told.
    void LobbyTag(char* out, size_t cch, uint32_t ogAppId) const
    {
        if (cch == 0) return;
        out[0] = '\0';

        if (HaveIni())
        {
            GetPrivateProfileStringA("Settings", "LobbyTag", "", out, (DWORD)cch, m_IniPath);
            if (out[0] != '\0')
                return;
        }

        if (ogAppId != 0)
        {
            _snprintf_s(out, cch, _TRUNCATE, "%u", ogAppId);
            return;
        }

        char exe[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0)
            return;
        const char* name = PathFindFileNameA(exe);
        _snprintf_s(out, cch, _TRUNCATE, "exe:%s", name);
        for (char* c = out; *c; ++c)
            *c = (char)tolower((unsigned char)*c);
    }

    /// Folder of plugin DLLs, relative to the executable. Empty
    /// means "load none".
    void PluginsFolder(char* out, size_t cch) const
    {
        if (cch == 0) return;
        out[0] = '\0';
        if (!HaveIni()) return;
        GetPrivateProfileStringA("Settings", "PluginsFolder", "", out, (DWORD)cch, m_IniPath);
    }

    /// AppIDs to report as owned and installed.
    std::vector<uint32_t> UnlockDlc() const
    {
        std::vector<uint32_t> ids;
        if (!HaveIni())
            return ids;

        char buf[1024] = { 0 };
        GetPrivateProfileStringA("Settings", "UnlockDLC", "", buf, sizeof(buf), m_IniPath);
        if (buf[0] == '\0')
            return ids;

        // strtok_s, not strtok: plugins run on their own threads and
        // a plugin that parses a list while this runs would corrupt
        // both. The state is cheap to carry and the bug is not.
        char* ctx = nullptr;
        for (char* tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx))
        {
            while (*tok == ' ' || *tok == '\t')
                tok++;
            uint32_t id = (uint32_t)strtoul(tok, nullptr, 10);
            if (id != 0)
                ids.push_back(id);
        }
        return ids;
    }

private:
    uint32_t ReadU32(const char* key, uint32_t fallback) const
    {
        if (!HaveIni())
            return fallback;

        char buf[16] = { 0 };
        GetPrivateProfileStringA("Settings", key, "", buf, sizeof(buf), m_IniPath);
        if (buf[0] == '\0')
            return fallback;

        return (uint32_t)strtoul(buf, nullptr, 10);
    }

    bool ReadBool(const char* key, bool fallback) const
    {
        if (!HaveIni())
            return fallback;

        char buf[16] = { 0 };
        GetPrivateProfileStringA("Settings", key, "", buf, sizeof(buf), m_IniPath);
        if (buf[0] == '\0')
            return fallback;

        return _stricmp(buf, "true") == 0
            || _stricmp(buf, "1") == 0
            || _stricmp(buf, "yes") == 0
            || _stricmp(buf, "on") == 0;
    }

    char m_IniPath[MAX_PATH] = { 0 };
};

} // namespace kryotoo
