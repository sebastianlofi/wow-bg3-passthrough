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

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end

local hop = nil
function M.Hopping() return hop ~= nil end

function M.Start()
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
        for _, p in ipairs(points) do
            local trigger = M.TRIGGERS[p.Slot + 1]
            if trigger and p.Known then bySlot[p.Slot] = p end
            for _, who in ipairs(party) do
                if trigger and p.Known and not offered(p.Name, who) then
                    if not item then
                        local x, y, z = Osi.GetPosition(who)
                        -- Off the squares: an item on one keeps it from going home clean, and the pool loses it.
                        item = x and q(Osi.CreateAt, M.HELPER, x, y - 30, z + 500, 1, 0, "") or nil
                    end
                    if item then
                        if not done[p.Name] then
                            q(Osi.RegisterWaypoint, p.Name, item)
                            done[p.Name] = true
                        end
                        q(Osi.UnlockWaypoint, p.Name, trigger, who)
                        q(function() Osi.DB_WaypointUnlocked(p.Name, who) end)
                    end
                elseif not p.Known and offered(p.Name, who) then
                    q(Osi.LockWaypoint, p.Name, who)
                    q(function() Osi.DB_WaypointUnlocked:Delete(p.Name, who) end)
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
        hop = {Until = ticks + M.HOP_TICKS, Party = party, Back = {}}
        M.State.Clicks = M.State.Clicks + 1
        local p = bySlot[slot]
        if not p then return end
        Ext.IO.SaveFile("wow-bg3/link/waypoint-click.json",
            Ext.Json.Stringify({Id = p.Id, Name = p.Name, Click = M.State.Clicks, Tick = ticks}))
    end)
    pcall(Ext.Osiris.RegisterListener, "Teleported", 9, "before", function(who, _, ox, oy, oz)
        local u = hop and uuidOf(who)
        if not u or not hop.Party[u] then return end
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
