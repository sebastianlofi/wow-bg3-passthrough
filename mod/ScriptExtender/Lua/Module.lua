-- Whether BG3 itself loaded WOW-BG3's module in this session, asked before anything of the mod starts.
--
-- The extender loads a mod's scripts when modsettings.lsx names the mod. That is a file: BG3 reads it once, at its
-- start, and it can change afterwards. Steam Cloud keeps it for every machine of an account and put another
-- machine's copy in place four seconds after BG3 had started with the mod not enabled: BG3 ran without WOW-BG3's
-- module (no level, no stats, no lighting of ours), the scripts loaded all the same, took a New Game as theirs and
-- sent the party to a level that BG3 did not have. The party stayed in the character-creation level.
--
-- BG3's own module list is what counts. Measured: with the mod enabled it holds this module at every point from the
-- scripts' first line (client at the menu, server at a session's load) to a running game; started without it, at none.
-- So: nothing of the mod starts unless the list has the module. Should BG3 take the module in later with these
-- scripts still loaded (it reloads its modules for a save's own list), they start then.
-- The launcher plugin is told either way (its "module" setting): it starts the WoW side and holds loading screens
-- only for a BG3 that has the module.
local M = {}
M.UUID = "a97c1258-37c3-434c-9309-c982f6cea252"
local LAUNCHER = "wowbg3-launcher"

-- true or false, and how many modules BG3's list holds now.
function M.Loaded()
    local n = Ext._Internal.ModCount() or 0
    for i = 0, n - 1 do
        if Ext._Internal.ModUuidAt(i) == M.UUID then return true, n end
    end
    return false, n
end

-- Runs start() once: now if BG3 has the module, else at the game state change after which it has.
function M.Start(context, start)
    local started, said = false, nil
    local function check(when)
        if started then return end
        local ok, has, n = pcall(M.Loaded)
        has = ok and has == true
        if has ~= said then
            said = has
            Ext.Log.Print(string.format("WOW-BG3 module (%s, %s): BG3 %s it (%s modules)%s", context, when,
                has and "loaded" or "did not load", ok and tostring(n) or "list unreadable",
                has and "" or ": the mod does nothing in this session"))
            pcall(Ext.Plugins.Set, LAUNCHER, "module", has and 1 or 0)
        end
        if has then
            started = true
            start()
        end
    end
    check("start")
    if not started then
        Ext.Events.GameStateChanged:Subscribe(function(e) check(tostring(e.ToState)) end)
    end
end
return M
