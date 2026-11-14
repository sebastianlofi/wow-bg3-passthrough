-- Waypoints (server): the WoW flight points the character has discovered, in BG3's own waypoint panel.
--
-- wow-bg3/link/waypoint-panel.json {Points = {{Slot, Id, Name}}} comes from the chat helper (wowbg3-chatcmd.py,
-- wow_waypoints.panel). A click writes wow-bg3/link/waypoint-click.json {Id}, and the helper runs `waypoint go`
-- as if typed: the same move, the same refusals, shown in BG3's message box (wow-bg3/link/waypoint-refused.json).
--
-- What BG3's panel takes (tried in game):
--   * RegisterWaypoint(id, item), UnlockWaypoint(id, trigger, player). The id shows as the entry's name, so the
--     id is the WoW name. The item only has to exist (a Helper_Waypoint under the hero, invisible).
--   * The trigger must be a point trigger in the current level; any other kind leaves no entry. 36 are below,
--     story and debug spots of the host level BGO_Main_A; a team has at most 33 points. BG3's own panel sends
--     only the trigger with a click, so each point has its own.
--   * The panel groups by level, so all stand under one heading.
-- BG3's story answers a click with TeleportToWaypoint -> PROC_WaypointTeleportTo -> TeleportTo the trigger, and
-- no rule can stop it from Lua. The party was left there for the 3 s Camp.lua takes to notice: the Act 3 story
-- of the place played and the WoW picture went wrong. So each arrival is undone in the listener, the same tick.
local M = {}
local Hero = Ext.Require("Hero.lua")

M.HELPER = "Helper_Waypoint_A_2dab7df1-7914-4748-b329-b4fe8b60fcc9"
M.HOP_TICKS = 30 -- after a click: how long the party's teleports count as the story's hop
M.HEADING = "h258bbcadgec46g444dga457gb0586a94db42" -- BGO_Main_A's name in the panel, "Wyrm's Crossing"
M.TRIGGERS = {
    "bfac8ea1-f1cd-4a7d-bb67-65c502ba5c4f", "60bac7c3-a574-4336-9384-1281aacdef1c",
    "06aaec7a-fd57-403a-8b80-a6d100781bc2", "c5b9f53d-1b21-4636-978b-7525e01ff784",
    "a423e0b9-a35d-41e0-966c-e7a9c311bd81", "016ac9ad-ac85-49a0-a6be-e24fdb0de2bb",
    "5561c476-c82d-4239-b4a3-baaf2985ef71", "71df0a8d-2cda-4e2d-b612-9a5f5a80e771",
    "78407c06-56b9-4838-8a63-05122115d6f3", "7c3c4dae-22b8-4e21-92bf-e1df4fc26d99",
    "8cfee80e-d29c-4fa8-9d6a-7bac178833f1", "0cd69a17-24dd-4fde-878b-bd36178a424e",
    "37d9e34f-4b32-4560-b86d-c0e5cbde5d29", "43942fc9-6b75-4c14-b1a2-e157a4b786c2",
    "414d5ebf-a996-4324-baff-40749ae3b248", "14bc4d24-5fd7-4aae-9d03-8c509e57c804",
    "f3d914d6-c439-4691-a807-143d9036fc21", "562fedbf-e567-424b-b474-daf7f27ceb9e",
    "20d20038-a009-4bae-99a0-9fa93c09c2c6", "64ab41c6-17af-4523-b32e-9c964a686312",
    "a422178b-c322-4777-8382-7954f64c9807", "f8b3cf2c-6481-4ea8-9526-451861f648db",
    "267cc194-a67a-4705-9459-3187e51487db", "943cfcf8-047e-4456-8e3e-800b6b89fec1",
    "2eb2b9f8-45ab-47f9-9472-a80e13a7b87c", "5d7b1276-f516-49f2-b3ea-3564b25cd35c",
    "6dbb3d52-f5a9-445a-a1ce-e4562c1eba58", "270ad218-2c6a-4ebc-a94f-56edd76305d3",
    "dbe8cde1-296d-4852-b77b-dc48ca037f6d", "cbee66cf-6ba2-44ab-832a-d8fd4ce351a3",
    "65978f4d-3fcc-4d1a-82ab-2a7d69ebbcdc", "0c626c44-1a2a-42b7-ab6d-1d43b33531d6",
    "45ca28ff-e52e-4c69-a956-78a8bd8f9760", "adfea280-d985-4325-b05f-a31f2ce3c127",
    "338b4f15-a078-480f-8f6c-7783586169f3", "0b212438-3336-4fe4-a115-a9cd92eec867",
}

-- In a level of our own (Hero.OWN) the panel's points are its own: tools/wowbg3_level.py writes one point trigger per
-- slot ("Waypoints" in release/levels/*.json, waypoint_uid). The 36 above are BGO_Main_A's, the borrowed host before:
-- a click on one from our level or from camp is BG3's teleport into that level, which no undo can catch (a play
-- report: the Undercity flight from camp loaded Act 3's map and world mode never started). test_waypoint_triggers.py
-- holds the two lists together.
M.OWN_TRIGGERS = {
    "b6f7c1a9-5e1d-55d8-a7d2-c4dac79d8f2b", "e28bf3b4-5e21-5205-8cbf-af90a059dfa3",
    "d9988c85-5c43-5ad6-b85d-f548be141c52", "dede610e-d56f-5e87-833f-3ade86ef968f",
    "6c090d83-88ba-5a85-a66c-1ccdef333b5b", "5208028f-d711-5269-87cc-55dbe8a05637",
    "2e7930df-bf6a-5892-8271-151fcb51fae8", "6dc8d561-01e3-53c6-9028-7f852f9d0cb1",
    "da5eac30-a80d-5d85-854c-1cfa209eb15c", "8f004ee8-5f09-5eaa-8a83-3a731b4882d7",
    "c370f6a6-7db5-5435-983c-b7502673939a", "ea3b1baa-766e-580f-9f76-65f899da1317",
    "f8a0f211-8889-5aff-9528-dc9bae358224", "f4da39d3-b4a2-5036-90cc-a965ed34c6d1",
    "586e8980-855a-577f-b482-b94187f8175f", "f21394d8-a577-5c6c-8b9b-61da261110e9",
    "21eb7b9a-7472-56f0-b028-ed7509863f42", "8a97cfcd-53dc-5a8b-9317-dc7af77c69c6",
    "0cbaf580-c193-56c5-b9f5-93f76b0060af", "dfd81b08-7f00-58cb-bb5e-d2b724846b57",
    "4ac05719-cedd-54ed-ba26-4a263ed00876", "0a78dd5f-12a0-56f3-80ac-0f57ac932589",
    "c8f3c1b8-c713-57b8-a9af-f1a8d749e16a", "b26c9c05-a316-5da4-83d1-dc0210489132",
    "f3c84a30-f03a-5992-b2f7-d1ce4ed2945f", "2b311516-cd7f-54cb-9c70-391ac89513f1",
    "db880b30-5f51-53d4-900f-e3b6f8236c5f", "a67110a9-6d98-5479-a6f3-67e5a4c7ff8c",
    "3ef1e5c5-548b-5d41-8f8b-0add71c28ba6", "322392ca-b4fd-5f22-a26c-5ec7f8481462",
    "355b4710-edf0-589f-9410-043103b0a085", "6a94680a-c1d2-5208-ab26-ce2b7f0158db",
    "a39a2cfc-2ce4-5e2e-bd85-67e7f63de0fb",
}

-- A save made before our own level's triggers (the release before 0.1.2354) keeps its entries on BGO_Main_A's: BG3
-- stores each unlocked entry's trigger in the save, and offer() never offers an entry twice, so a click there still
-- loaded Act 3's map (his Gale save, slot B: "entering a different region", then BGO_Main_A). Each party member carries
-- this persistent mark once their entries sit on our level's triggers; without it they are locked and unlocked again
-- on ours, once a save (BG3 shows its Waypoint Discovered notice that once).
local BOUND = "WowBg3WaypointsOwn"
pcall(Ext.Vars.RegisterUserVariable, BOUND, {Server = true, Client = false, SyncToClient = false, Persistent = true})

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end

local hop = nil
function M.Hopping() return hop ~= nil end

function M.Start()
    if Hero.OWN then M.TRIGGERS = M.OWN_TRIGGERS end
    local ticks, lastRaw, item, lastRefused = 0, nil, nil, false -- false: nothing read yet, an old refusal stays quiet
    local bySlot, done = {}, {}  -- slot -> discovered point; id -> registered in this session
    local slotOf = {}
    for i, t in ipairs(M.TRIGGERS) do slotOf[t] = i - 1 end
    M.State = {Points = 0, Clicks = 0, Undone = 0}
    pcall(Ext.Loca.UpdateTranslatedString, M.HEADING, "Flight Points")

    local function players()
        local out = {}
        for _, r in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do out[#out + 1] = r[1] end
        return out
    end

    -- A save keeps what the panel was given, so BG3's story table says what is on it: nothing is offered twice (each
    -- offer is a Waypoint Discovered notice), and a point this character does not know goes.
    local function offered(name, who)
        local rows = q(function() return Osi.DB_WaypointUnlocked:Get(name, who) end)
        return type(rows) == "table" and #rows > 0
    end

    local function offer(points)
        bySlot = {}
        local party = players()
        if Hero.OWN and #points > 0 then  -- with the list in hand (an empty first read would mark nothing moved)
            for _, who in ipairs(party) do
                local e = q(Ext.Entity.Get, who)
                if e and q(function() return e.Vars[BOUND] end) ~= true then
                    local moved = 0
                    for _, p in ipairs(points) do
                        if offered(p.Name, who) then
                            q(Osi.LockWaypoint, p.Name, who)
                            q(function() Osi.DB_WaypointUnlocked:Delete(p.Name, who) end)
                            moved = moved + 1
                        end
                    end
                    pcall(function() e.Vars[BOUND] = true end)
                    if moved > 0 then
                        Ext.Utils.Print(string.format("WOW-BG3 waypoints: %d entries of %s moved to our level's triggers", moved, tostring(who)))
                    end
                end
            end
        end
        for _, p in ipairs(points) do
            local trigger = M.TRIGGERS[p.Slot + 1]
            if trigger and p.Known then bySlot[p.Slot] = p end
            -- In our level an entry has an id of its own: BG3 keeps the trigger a waypoint id was first unlocked on, for
            -- good in a save (locking and unlocking it on our trigger brought back BGO_Main_A's: the click still loaded Act
            -- 3's map, slot B). The name with a no-break space after it reads the same in the panel and binds anew.
            local id = Hero.OWN and (p.Name .. "\194\160") or p.Name
            for _, who in ipairs(party) do
                if trigger and p.Known and not offered(id, who) then
                    if not item then
                        local x, y, z = Osi.GetPosition(who)
                        -- Off the squares: an item on one keeps it from going home clean, and the pool loses it.
                        item = x and q(Osi.CreateAt, M.HELPER, x, y - 30, z + 500, 1, 0, "") or nil
                    end
                    if item then
                        if not done[id] then
                            q(Osi.RegisterWaypoint, id, item)
                            done[id] = true
                        end
                        q(Osi.UnlockWaypoint, id, trigger, who)
                        q(function() Osi.DB_WaypointUnlocked(id, who) end)
                    end
                elseif not p.Known and offered(id, who) then
                    q(Osi.LockWaypoint, id, who)
                    q(function() Osi.DB_WaypointUnlocked:Delete(id, who) end)
                end
            end
        end
        M.State.Points = #points
    end

    pcall(Ext.Osiris.RegisterListener, "TeleportToWaypoint", 2, "before", function(_, trigger)
        local slot = slotOf[uuidOf(trigger)]
        if not slot then return end
        local party = {}
        for _, who in ipairs(players()) do party[uuidOf(who) or who] = true end
        -- From another level (camp) BG3's answer is a level change into ours: nothing to undo there (Camp.lua puts the
        -- party on its way-back spot); the WoW flight follows once camp is over (wowbg3cmd.py waits, panel clicks).
        -- At camp inside our level (Camp.lua, INTMAIN) the same holds: the hop takes the party off camp and the story's
        -- way back (Camp.lua) puts it on the spot it left, from where the flight goes.
        local lv = q(function() local r = Osi.DB_CurrentLevel:Get(nil); return r and r[1] and r[1][1] end)
        local hero = Hero.Uuid and Hero.Uuid() or nil
        local inCamp = hero and #(q(function() return Osi.DB_PlayerInCamp:Get(hero) end) or {}) > 0
        hop = {Until = ticks + M.HOP_TICKS, Party = party, Back = {}, Away = lv ~= Hero.REGION or inCamp}
        M.State.Clicks = M.State.Clicks + 1
        local p = bySlot[slot]
        if not p then return end
        Ext.IO.SaveFile("wow-bg3/link/waypoint-click.json",
            Ext.Json.Stringify({Id = p.Id, Name = p.Name, Click = M.State.Clicks, Tick = ticks}))
    end)
    pcall(Ext.Osiris.RegisterListener, "Teleported", 9, "before", function(who, _, ox, oy, oz)
        local u = hop and uuidOf(who)
        if not u or not hop.Party[u] or hop.Away then return end
        if hop.Back[u] then hop.Back[u] = nil return end
        hop.Back[u] = true
        Osi.TeleportToPosition(who, ox, oy, oz, "", 0, 0, 0, 0, 1)
        M.State.Undone = M.State.Undone + 1
    end)

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if hop and ticks > hop.Until then hop = nil end
        if ticks % 30 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, err = pcall(function()
            local raw = Ext.IO.LoadFile("wow-bg3/link/waypoint-panel.json")
            if raw and raw ~= lastRaw then
                lastRaw = raw
                offer(Ext.Json.Parse(raw).Points or {})
            end
            local no = Ext.IO.LoadFile("wow-bg3/link/waypoint-refused.json")
            if no ~= lastRefused then
                if lastRefused ~= false and no then
                    -- A message box: ShowNotification draws nothing over the WoW picture.
                    Osi.OpenMessageBox(Osi.GetHostCharacter(), (Ext.Json.Parse(no).Text:gsub("^waypoint: ", "")))
                end
                lastRefused = no
            end
        end)
        if not ok then M.State.Error = tostring(err) end
    end)
end

return M
