-- WorldFlag (both contexts; plug-and-play plan "Decisions"): world mode
-- only in games started with WOW-BG3 active. A save made without the mod stays vanilla even when the
-- mod is enabled later.
--
-- The flag is a persistent mod variable of the WOW-BG3 module (meta.lsx UUID below), so it lives in
-- the save: bg3le writes it into the savegame ("savegame: wrote ... mod variable(s)") and reads it
-- back on load, before SessionLoaded. Synced to the client.
--   New game: the story's CharacterCreationStarted with this mod's scripts running -> On.
--   Load: the save's own value; a save made without the mod has none -> Off.
-- Note: bg3le runs an enabled mod's scripts even when the save's module list lacks the mod
-- (reference/MOD-LOADING.md), so "the scripts run" never means "world mode"; only this flag does.
--
-- API: WorldFlag.On() -> true (world mode), false (vanilla game), nil (no session yet).
--      WorldFlag.Why() -> "new-game", "save", "none", "override" or "no-session".
-- Status for tools: wow-bg3/worldflag.json {On, Why, State}. The launcher plugin (wowbg3-launcher)
-- gets setting "world" = 1/0 once the session is known, so it holds the loading screen only for
-- world-mode games.
-- Harness override (development only): env WOW_BG3_WORLD_FLAG=1 treats every session as world mode
-- (the harness saves predate the flag); =0 treats every session as vanilla.
-- The notice: a save made without WOW-BG3, loaded with the mod on, says so once in BG3's own message
-- box (OK to close; the save goes on as plain BG3). It waits NOTICE_AFTER ticks of Running, so a new
-- game has set its flag at character creation by then, and never shows in character creation itself.
-- Not with the override. worldflag.json carries Notice (the tick it showed) once it has.
local M = {}
M.UUID = "a97c1258-37c3-434c-9309-c982f6cea252"
M.VAR = "WorldMode"
M.FORMAT = 1
local LAUNCHER = "wowbg3-launcher"
M.NOTICE = "WOW-BG3: this save was not started with WOW-BG3 enabled, so it plays as plain Baldur's Gate 3. "
    .. "Start a new game to play WOW-BG3."
local NOTICE_AFTER = 150 -- ticks of Running after a load before a save without the flag is told
local CC_REGION = "SYS_CC_I" -- character creation's level

local registered = false
local function register()
    if registered then return true end
    local ok, err = pcall(Ext.Vars.RegisterModVariable, M.UUID, M.VAR, {Server = true, Client = true,
        WriteableOnServer = true, WriteableOnClient = false, Persistent = true, SyncToClient = true})
    registered = ok
    if not ok then Ext.Log.Print("WOW-BG3 worldflag: RegisterModVariable failed: " .. tostring(err)) end
    return ok
end

local function vars()
    local ok, v = pcall(Ext.Vars.GetModVariables, M.UUID)
    return ok and v or nil
end

local function stored()
    local v = vars()
    local value = v and v[M.VAR]
    if type(value) == "table" and value.On ~= nil then return value end
    return nil
end

local override = Ext._Internal and Ext._Internal.Getenv and Ext._Internal.Getenv("WOW_BG3_WORLD_FLAG") or nil

function M.On()
    if override == "1" then return true end
    if override == "0" then return false end
    local state = tostring(Ext.Utils.GetGameState())
    local value = stored()
    if value then return value.On == true end
    if state == "Menu" or state == "Init" or state == "LoadModule" or state == "LoadMenu" then return nil end
    -- The client's copy of the mod variable syncs only once the client runs the level; under the
    -- launcher's held loading screen (bg3le patch 0004) world mode starts before that (harness
    -- A load refused world mode on the client, "WorldFlag: false"). The server tells the
    -- launcher plugin this session's flag (tellLauncher, reset at every unload), so the client asks it.
    if Ext.IsClient() then
        local ok, w = pcall(Ext.Plugins.Get, LAUNCHER, "world")
        if ok and tonumber(w) == 1 then return true end
    end
    return false
end

function M.Why()
    if override == "1" or override == "0" then return "override" end
    local value = stored()
    if value then return value.Why or "save" end
    local state = tostring(Ext.Utils.GetGameState())
    if state == "Menu" or state == "Init" or state == "LoadModule" or state == "LoadMenu" then return "no-session" end
    return "none"
end

local function tellLauncher(on)
    local ok, list = pcall(Ext.Plugins.List)
    if not ok or type(list) ~= "table" then return end
    for _, p in ipairs(list) do
        if p.Name == LAUNCHER and p.Loaded then
            pcall(Ext.Plugins.Set, LAUNCHER, "world", on and 1 or 0)
            return
        end
    end
end

local last = nil
local notice = nil -- the tick the notice showed in this session
local function publish(context)
    local on = M.On()
    -- The launcher forgets the session at every unload, so it is told at every state change; only
    -- by the server, which has the save's value at SessionLoaded (the client's copy syncs later).
    if on ~= nil and context == "server" then tellLauncher(on) end
    local key = tostring(on) .. "/" .. M.Why()
    if key == last then return end
    last = key
    if context == "server" then
        Ext.IO.SaveFile("wow-bg3/worldflag.json", Ext.Json.Stringify({On = on, Why = M.Why(), Format = M.FORMAT,
            State = tostring(Ext.Utils.GetGameState()), Time = Ext.Utils.MonotonicTime(), Notice = notice}))
    end
    Ext.Log.Print("WOW-BG3 worldflag (" .. context .. "): " .. tostring(on) .. " (" .. M.Why() .. ")")
end

function M.Start(runId, context)
    register()
    if context == "server" then
        -- A new game: the story starts character creation once, in the new session only.
        pcall(Ext.Osiris.RegisterListener, "CharacterCreationStarted", 0, "after", function()
            if stored() then return end
            local v = vars()
            if not v then return end
            v[M.VAR] = {On = true, Why = "new-game", Format = M.FORMAT, Version = "0.1.0"}
            Ext.Log.Print("WOW-BG3 worldflag: new game with WOW-BG3 -> world mode on")
            publish(context)
        end)
    end
    Ext.Events.SessionLoaded:Subscribe(function() publish(context) end)
    local ticks, running, told = 0, nil, false
    Ext.Events.GameStateChanged:Subscribe(function(e)
        local to = tostring(e.ToState)
        if to == "Menu" or to == "UnloadSession" then last, notice, told, running = nil, nil, false, nil end
        publish(context)
    end)
    if context ~= "server" or override == "1" or override == "0" then return end
    -- The notice (see the header): once per loaded session, on the server, which has the save's flag.
    Ext.Events.Tick:Subscribe(function()
        if told then return end
        ticks = ticks + 1
        if tostring(Ext.Utils.GetGameState()) ~= "Running" then return end
        running = running or ticks
        if ticks - running < NOTICE_AFTER then return end
        told = true
        if stored() then return end
        local ok, host = pcall(Osi.GetHostCharacter)
        if not ok or type(host) ~= "string" or host == "" then told = false; return end
        local okR, region = pcall(Osi.GetRegion, host)
        if okR and region == CC_REGION then return end
        local shown, err = pcall(Osi.OpenMessageBox, host, M.NOTICE)
        notice = ticks - running
        Ext.Log.Print("WOW-BG3 worldflag: this save has no WOW-BG3 flag: " ..
            (shown and "told the player to start a new game" or ("message box failed: " .. tostring(err))))
        last = nil
        publish(context)
    end)
end

return M
