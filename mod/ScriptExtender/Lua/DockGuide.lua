-- DockGuide (server, with one client call): the dream guardian at WoW's docks.
--
-- A ship or a zeppelin cannot be ridden in our level (a BG3 platform never turns, and a vessel does), so a crossing
-- is a move of the party from one dock to the other. Who offers it is the player's own dream guardian, a character
-- BG3 keeps for every game and shows nowhere in ours: near a dock it stands at the pier, and talking to it opens a
-- BG3 dialog of our own (tools/bg3_dialog.py): "Travel to <place>?", "Yes, take me to <place>." / "Not now.".
--
-- Files (wow-bg3/):
--   link/dock-panel.json    from the chat helper (wowbg3-chatcmd.py, wowbg3cmd.dock_panel) every few seconds:
--                           {Map, Docks = {{Key, Name, Kind, Wow = {x, y, z}}}}: the crossings from the docks near
--                           the hero; Wow is where a player stands at this end's pier, Name the place at the other.
--   link/dock-click.json    {Key, Click}: the player said yes. The helper runs the crossing as the chat command did.
--   link/dock-refused.json  {Seq, Text}: the helper's refusal (a fight, already on the way), shown in a message box.
--   dock-guide.json         {RunId, Uuid, OnStage, Key, Name}: for the client. WoW draws BG3's characters in world
--                           mode, and only those the client names (WorldMode.lua's figures); the dialog's three
--                           lines are text the client shows, so it writes the place into them as well.
--
-- The guardian is BG3's: DB_GLO_DaisyDreams_Relationships(avatar, guardian) names the one the player made (the
-- story's own table); a game whose hero has none takes the first of BG3's four stock guardians. It is a global
-- character in no level until BG3's story calls it, so it is brought into the hero's level first (TeleportTo the
-- hero) and to the pier a round later. It takes no part in a fight.
--
-- The dialog has no timeline, so BG3 stages nothing: the two stand as they stood and WoW's picture stays, with WoW
-- drawing both (a companion's dialog shows BG3's own picture instead, for BG3's staging of it: Hero.lua's
-- PartyDialog; tried for this dialog, the picture behind the two was black).
local M = {}
local Hero = Ext.Require("Hero.lua")

local YD = 0.9144
-- tools/bg3_dialog.py's names (tools/test_bg3_dialog.py holds the two files together).
M.DIALOG = "WOWBG3_DockGuardian_5e837480-045e-5e70-b54c-312392cd8917"
M.GO = "d6d60414-bb3c-5897-83fb-684b2185e21f"
M.TEXT = {Ask = "hba93f146g5e47g5bdeg9507g2f51a4a34f06", Yes = "hc9e0911bgf623g5d9agbba9gd884af2f990d",
          No = "h80ee5e2bg9992g535eg846agb1d9e3f7cf0c"}
M.NEAR = 55.0   -- metres from the hero within which the guardian stands at a pier: ground is placed that far
M.EVERY = 30    -- ticks between looks

-- The dialog's three lines for a crossing to `place`.
function M.Lines(place)
    return {Ask = "Travel to " .. place .. "?", Yes = "Yes, take me to " .. place .. ".", No = "Not now."}
end

-- The world-mode mapping (WorldMode.lua): BG3 = Centre + (y, z, -x) * 0.9144.
function M.ToBg3(centre, wow)
    return {centre[1] + wow[2] * YD, centre[2] + wow[3] * YD, centre[3] - wow[1] * YD}
end

-- The dock nearest a BG3 point among the panel's, with its place in BG3 and its distance on the ground; nil when
-- none lies within `reach` metres.
function M.Nearest(docks, centre, at, reach)
    local best, bestAt, bestD = nil, nil, reach
    for _, d in ipairs(docks or {}) do
        if type(d) == "table" and type(d.Wow) == "table" and type(d.Key) == "string" and type(d.Name) == "string" then
            local p = M.ToBg3(centre, d.Wow)
            local m = math.sqrt((p[1] - at[1]) ^ 2 + (p[3] - at[3]) ^ 2)
            if m <= bestD then best, bestAt, bestD = d, p, m end
        end
    end
    return best, bestAt, bestD
end

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end
local function readJson(name)
    local raw = Ext.IO.LoadFile("wow-bg3/" .. name)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

-- The three lines written into the text table of this Lua state (the server's for the story, the client's for the
-- screen).
function M.Write(place)
    for k, text in pairs(M.Lines(place)) do pcall(Ext.Loca.UpdateTranslatedString, M.TEXT[k], text) end
end

-- Client (WorldMode.lua, as it reads the party): the guardian's uuid while it stands at a pier, else nil.
local shown = nil
function M.Client(runId)
    local g = readJson("dock-guide.json")
    if not (g and g.RunId == runId and g.OnStage == true and type(g.Uuid) == "string") then return nil end
    if type(g.Name) == "string" and g.Name ~= shown then
        shown = g.Name
        M.Write(g.Name)
    end
    return g.Uuid
end

function M.Start(runId)
    local ticks, placed, written, lastRefused, clicks = 0, nil, nil, false, 0
    M.State = {Placed = nil, Clicks = 0}

    -- BG3's own guardian of this hero (the story's table), else the first of the four it ships.
    local function guardian(hero)
        local rows = q(function() return Osi.DB_GLO_DaisyDreams_Relationships:Get(nil, nil) end) or {}
        for _, r in ipairs(rows) do
            if uuidOf(tostring(r[1])) == hero then return tostring(r[2]) end
        end
        if rows[1] then return tostring(rows[1][2]) end
        local stock = q(function() return Osi.DB_GLO_DaisyDreams_Daisies:Get(nil) end) or {}
        return stock[1] and tostring(stock[1][1]) or nil
    end

    local function publish(g, dock)
        local body = Ext.Json.Stringify({RunId = runId, Uuid = uuidOf(g), OnStage = dock ~= nil, Key = dock and dock.Key or nil,
            Name = dock and dock.Name or nil})
        if body ~= written then
            written = body
            Ext.IO.SaveFile("wow-bg3/dock-guide.json", body)
        end
    end

    local function leave(g)
        if placed and g then pcall(Osi.SetOnStage, g, 0) end
        placed = nil
        M.State.Placed = nil
    end

    -- "Yes, take me to ...": the flag is the player's until it is read here.
    pcall(Ext.Osiris.RegisterListener, "FlagSet", 3, "after", function(flag, speaker, _)
        if uuidOf(tostring(flag)) ~= M.GO then return end
        if pcall(Osi.ClearFlag, flag, speaker, 0) == false then pcall(Osi.ClearFlag, flag, speaker) end
        if not placed then return end
        clicks = clicks + 1
        M.State.Clicks = clicks
        Ext.IO.SaveFile("wow-bg3/link/dock-click.json", Ext.Json.Stringify({Key = placed.Key, Name = placed.Name, Click = clicks, Tick = ticks}))
    end)

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % M.EVERY ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, err = pcall(function()
            local hero = Hero.Uuid()
            local g = hero and guardian(hero) or nil
            local world = readJson("link/world.json")
            local panel = readJson("link/dock-panel.json")
            local on = g and world and world.Active == true and type(world.Centre) == "table" and panel
                and panel.Map == world.Map and type(panel.Docks) == "table"
            local dock, at = nil, nil
            if on then
                local x, y, z = Osi.GetPosition(hero)
                if x then dock, at = M.Nearest(panel.Docks, world.Centre, {x, y, z}, M.NEAR) end
            end
            if not dock then
                leave(g)
                if g then publish(g, nil) end
            elseif not placed or placed.Key ~= dock.Key then
                local e, he = Ext.Entity.Get(uuidOf(g)), Ext.Entity.Get(hero)
                local level = q(function() return e.Level.LevelName end)
                pcall(Osi.SetOnStage, g, 1)
                if level == nil or level ~= q(function() return he.Level.LevelName end) then
                    -- Not in the hero's level yet (BG3 keeps it in none): there first, the pier at the next look.
                    pcall(Osi.TeleportTo, g, hero, "", 0, 0, 0, 0, 0)
                    return
                end
                Osi.TeleportToPosition(g, at[1], at[2], at[3], "", 0, 0, 0, 0, 1)
                pcall(Osi.SetCanJoinCombat, g, 0)
                pcall(Osi.SetCanFight, g, 0)
                pcall(function() Osi.DB_Dialogs(g, M.DIALOG) end)
                pcall(Osi.SteerTo, g, hero, 1)
                M.Write(dock.Name)
                placed = {Key = dock.Key, Name = dock.Name}
                M.State.Placed = dock.Key
                publish(g, dock)
            end
            local no = Ext.IO.LoadFile("wow-bg3/link/dock-refused.json")
            if no ~= lastRefused then
                if lastRefused ~= false and no then
                    -- A message box: ShowNotification draws nothing over the WoW picture (Waypoints.lua).
                    Osi.OpenMessageBox(Osi.GetHostCharacter(), (Ext.Json.Parse(no).Text:gsub("^cross: ", "")))
                end
                lastRefused = no
            end
        end)
        M.State.Error = not ok and tostring(err) or nil
        if ticks % (M.EVERY * 10) == 0 or not ok then
            pcall(Ext.IO.SaveFile, "wow-bg3/dock-guide-status.json", Ext.Json.Stringify({RunId = runId, Tick = ticks,
                Placed = M.State.Placed, Clicks = M.State.Clicks, Error = M.State.Error}))
        end
    end)
end

return M
