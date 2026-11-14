-- Camp (server): BG3's camp in world mode, with a trip there and back. Behind the switch `camp`
-- (wow-bg3/camp.json {"On": true}, written by the coordinator from settings.conf); off, CampOff.lua keeps
-- the party out of camp as before.
--
-- Why camp broke the WoW world (a playtest report: a random spot of the WoW map): the camp of the host
-- level BGO_Main_A is BG3's Act 3 FARM camp (Act3_Camp, entrance near 384, 6, -752), in the same level as
-- the void (centre -2600, 120, -1000). No level swap happens. BG3's Go to Camp (GLO_Camp:
-- PROC_TeleportToFromCamp) is a plain teleport, so WorldMode.lua kept publishing the hero, mapped from the
-- void as ever: about 3.3 km from the spot left. Then:
--   * benilla put the WoW body there ("placed on BG3's character", any big gap);
--   * the bridge moved the hero's window of squares there, cutting WoW ground over the camp;
--   * BG3's camera, leading, took WoW's camera there too;
--   * the squares left the void, so at Leave Camp the story's FindValidPosition found no ground at the
--     hero's return spot (DB_LeaveCamp_ReturnPos) and took its fallbacks.
-- Party.lua's upkeep also read a companion at camp (y 6) as one that fell from the void (y 120).
--
-- The trip: while the hero is at camp, WoW and the void wait. WorldMode.lua (client) keeps publishing the
-- spot the hero left, so the WoW body stands still, the squares stay placed and the bridge has nothing to
-- do; BG3's lead of WoW's camera is off and the bridge moves the compositor's file aside, so camp is BG3's
-- own picture. Hostile mobs do not notice the standing body: sight mode stays armed, it only ends with
-- world mode. On Leave Camp the story's own return works (its spot still has ground) and the party is back
-- on the same squares; this module checks the hero stands there and puts the hero there if not.
--
-- wow-bg3/camp-state.json {RunId, Seq, InCamp, Hero, From, Back} is the one record of a trip, for
-- WorldMode.lua, world_bridge.py and any module that needs it (M.InTrip() on the server).
--
-- Long rests are BG3's own: End the Day gathers the party at camp through the same teleport (the same
-- trip), BG3 checks and spends the supplies, the night and morning run as in BG3, and every rest effect
-- with them. Supplies are WoW food and drink (Inventory.lua, wow_items.py `sv`).
local M = {}
local Hero = Ext.Require("Hero.lua")
local Waypoints = Ext.Require("Waypoints.lua")

M.JUMP = 150.0        -- metres in one teleport: a trip's start, never a walk
M.PENDING = 180       -- ticks a teleported hero may take to count as in camp
M.BACK_NEAR = 2.0     -- metres from the spot left: farther and the hero is put there
M.LEFT_AFTER = 30     -- ticks out of DB_PlayerInCamp before the trip counts as over
M.VOID_REACH = 2000.0 -- metres from the void's centre: a jump that lands within is no one's travel in the level

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end

-- The switch, read once per Lua session on each side (a new setting takes a restart).
local onCached = nil
function M.On()
    if onCached == nil then
        local raw = q(Ext.IO.LoadFile, "wow-bg3/camp.json")
        local ok, c = pcall(Ext.Json.Parse, raw or "")
        onCached = ok and type(c) == "table" and c.On == true
    end
    return onCached
end

local trip = nil
function M.InTrip() return trip ~= nil end

-- world.json while world mode runs, else nil.
local function worldActive()
    local raw = Ext.IO.LoadFile("wow-bg3/link/world.json")
    if not raw then return nil end
    local ok, w = pcall(Ext.Json.Parse, raw)
    return ok and type(w) == "table" and w.Active == true and w or nil
end

local function inCampDb(hero)
    local rows = q(function() return Osi.DB_PlayerInCamp:Get(hero) end)
    return type(rows) == "table" and #rows > 0
end

-- BG3's camp nights are its story (GLO_CampNights picks one at every End the Day): at this Act 3 camp they
-- played Orin's murder of Zevlor in Elwynn. No story runs here, so none stays queued, as BG3's own debug word
-- "nonight" does; rows the story adds later go at the next check. The rest itself goes on without them.
local function noNights()
    for _, r in ipairs(q(function() return Osi.DB_CampNight:Get(nil, nil) end) or {}) do
        pcall(function() Osi.DB_CampNight:Delete(r[1], r[2]) end)
    end
    for _, r in ipairs(q(function() return Osi.DB_CampNight:Get(nil, nil, nil) end) or {}) do
        pcall(function() Osi.DB_CampNight:Delete(r[1], r[2], r[3]) end)
    end
end

-- A level of our own (Hero.OWN) has no camp: BG3's camps are places in the level the party plays in, set up by
-- that act's story (DB_Camp, DB_MainCamp), and our level has neither. Camp is then a BG3 level of its own, by
-- BG3's own level change: INT_Main_A, the mountain pass camp between Acts 2 and 3. Measured from our level: BG3's
-- server runs there after 4.4 s with no more memory (Rivington: 112 s and 540 MB), the story of that level
-- registers its camp on arrival (DB_Camp "INTMAIN", DB_ActiveCamp), and Party.lua's companions wait there.
--   Go to Camp in our level: BG3's story has no camp to go to and does nothing; this module asks for the level
--     change (TeleportPartiesToLevelWithMovie, the story's own call). The party arrives at the coordinates it
--     left, so BG3's own Go to Camp (PROC_TeleportToFromCamp, the story has the camp now) brings it in.
--   The WoW side: the hero is no longer in the host level, so the coordinator takes the WoW side down as at any
--     load (the WoW character logs out where it stood, safe from mobs) and the launcher's loading-screen hold is
--     released by this module once the hero stands in camp.
--   Leave Camp (or a walk off the camp's area): the level change back. The hero is in the host level again, the
--     WoW side comes up as after a load and world mode starts where the WoW character stands: the spot left.
--   A save made at camp loads at camp: this module only looks at the level the party is in.
-- That level's own story must not play: its opening cinematic is marked as played before the first visit
-- (DB_OnlyOnce, as the story marks it), and its nights go with every other (noNights).
M.LEVEL = "INT_Main_A"
M.ENTRY = "S_CAMP_INT_EntryPoint_ffff4e4a-4e43-4bf1-a54f-bbf3798b90c0"
M.BEATS = {"INT_EmperorRevealed_StartCinematic"}
M.ENTER_EVERY = 60    -- ticks between asks to BG3's own Go to Camp while the hero is not in camp yet
M.ENTER_TRIES = 5     -- then the party is put on the camp's entry point
M.LET_IN_AFTER = 600  -- ticks in the camp level: the loading screen is released whatever happened
local LAUNCHER = "wowbg3-launcher"
local function level()
    local rows = q(function() return Osi.DB_CurrentLevel:Get(nil) end)
    return rows and rows[1] and rows[1][1] or nil
end
local function players()
    local out = {}
    for _, r in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do out[#out + 1] = r[1] end
    return out
end
local function startOwn(runId)
    local ticks, asked, visit = 0, nil, nil
    M.State = {Trips = 0, Own = true}
    -- No trip of the borrowed-level kind: world mode's client and the bridge read this file.
    Ext.IO.SaveFile("wow-bg3/camp-state.json", Ext.Json.Stringify({RunId = runId, Seq = 1, InCamp = false}))
    local function send(to, why)
        if asked and ticks < asked.Until then return false end
        asked = {To = to, Until = ticks + 600}
        for _, b in ipairs(M.BEATS) do
            if #(q(function() return Osi.DB_OnlyOnce:Get(b) end) or {1}) == 0 then Osi.DB_OnlyOnce(b) end
        end
        Ext.Utils.Print("WOW-BG3 camp: " .. why .. ": the party goes to " .. to)
        Osi.TeleportPartiesToLevelWithMovie(to, "", "")
        return true
    end
    pcall(Ext.Osiris.RegisterListener, "TeleportToFromCamp", 1, "before", function(who)
        local ok, err = pcall(function()
            if #(q(function() return Osi.DB_Players:Get(who) end) or {}) == 0 then return end
            local lv = level()
            if lv == Hero.REGION then
                -- BG3's own reasons against camp travel (a fight, a dialog, a blocked zone) hold here too: the
                -- story's query for them (its QRY_Camp_IsPlayerBlockedFromTeleportToCamp also fails for having
                -- no camp, which is the case here).
                if q(Osi.QRY_WaypointTeleport_PlayerBlocked, who) then
                    Ext.Utils.Print("WOW-BG3 camp: Go to Camp: BG3 blocks travel now")
                    return
                end
                send(M.LEVEL, "Go to Camp")
            elseif lv == M.LEVEL and inCampDb(who) and #(q(function() return Osi.DB_Camp_NightMode:Get(1) end) or {}) == 0 then
                -- The story's own Leave Camp puts each player on the spot it came from, in this level: the
                -- coordinates of our level, where this one has nothing. No spot, no such move.
                for _, r in ipairs(q(function() return Osi.DB_LeaveCamp_ReturnPos:Get(nil, nil, nil, nil) end) or {}) do
                    pcall(function() Osi.DB_LeaveCamp_ReturnPos:Delete(r[1], r[2], r[3], r[4]) end)
                end
                send(Hero.REGION, "Leave Camp")
            end
        end)
        if not ok then Ext.Utils.Print("WOW-BG3 camp: " .. tostring(err)) end
    end)
    -- Long Rest pressed in our level: BG3 gathers the party at camp first and then asks "Do you want to end the
    -- day?" there; here the gather is this trip. The press is marked as a gather under way before the story sees it
    -- (DB_GoingToCamp(1): GLO_Camp's PROC_Camp_TryEnterNightMode takes such a press as handled), else the story
    -- asked the engine for End the Day in the world at once, the level change took its unanswered prompt away and
    -- the engine refused every later one (RequestEndTheDayFail: Long Rest at camp did nothing). Once the
    -- hero stands in camp the story's own End the Day runs, as after BG3's gather.
    local restFor = nil
    pcall(Ext.Osiris.RegisterListener, "EndTheDayRequested", 1, "before", function(who)
        local ok, err = pcall(function()
            if level() ~= Hero.REGION or #(q(function() return Osi.DB_Players:Get(who) end) or {}) == 0 then return end
            if q(Osi.QRY_WaypointTeleport_PlayerBlocked, who) then return end
            if send(M.LEVEL, "Long Rest") then
                Osi.DB_GoingToCamp(1)
                restFor = who
            end
        end)
        if not ok then Ext.Utils.Print("WOW-BG3 camp: " .. tostring(err)) end
    end)
    M.InTrip = function() return level() == M.LEVEL end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 5 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, err = pcall(function()
            if ticks % 30 == 0 then noNights() end
            local lv = level()
            if asked and lv == asked.To then asked = nil end
            -- The story unlocks camp (the Long Rest button, sending companions to camp) when it has a camp, which
            -- it learns in the camp's level: before the first trip Long Rest was greyed out. Its own fact for
            -- "camp is unlocked" (the story's debug word "campon" sets the same).
            if lv == Hero.REGION and #(q(function() return Osi.DB_Camp_Unlocked:Get(1) end) or {1}) == 0 then
                Osi.DB_Camp_Unlocked(1)
                Ext.Utils.Print("WOW-BG3 camp: camp unlocked for the story (DB_Camp_Unlocked)")
            end
            if lv ~= M.LEVEL then
                if visit then Ext.Utils.Print("WOW-BG3 camp: back from camp") end
                visit = nil
                return
            end
            local hero = Hero.Uuid()
            if not hero then return end
            if not visit then
                -- Long Rest pressed in our level began the story's End the Day (DB_GoingToCamp, a request to the
                -- engine to gather the party at camp) and the level change cut it short before the engine's
                -- answer: the story then took every later Long Rest for one still under way and did nothing.
                -- Its own rule for an answer that says no (RequestGatherAtCampFail) drops the same fact.
                if #(q(function() return Osi.DB_GoingToCamp:Get(1) end) or {}) > 0 then
                    pcall(function() Osi.DB_GoingToCamp:Delete(1) end)
                    Ext.Utils.Print("WOW-BG3 camp: the End the Day begun before the level change is over (DB_GoingToCamp)")
                end
            end
            visit = visit or {Since = ticks, Tries = 0, Next = ticks, In = false, Out = 0, Let = false}
            local inCamp = inCampDb(hero)
            if inCamp and not visit.In then
                visit.In = true
                M.State.Trips = M.State.Trips + 1
                Ext.Utils.Print("WOW-BG3 camp: at camp (" .. M.LEVEL .. "), the WoW side waits")
                if restFor then
                    local who = restFor
                    restFor = nil
                    local okE = pcall(Osi.PROC_Camp_TryEnterNightMode, who)
                    Ext.Utils.Print("WOW-BG3 camp: Long Rest: End the Day at camp " .. (okE and "asked" or "refused"))
                end
            end
            if not visit.In and ticks >= visit.Next then
                visit.Tries, visit.Next = visit.Tries + 1, ticks + M.ENTER_EVERY
                for _, p in ipairs(players()) do
                    if visit.Tries <= M.ENTER_TRIES then
                        pcall(Osi.PROC_TeleportToFromCamp, p)
                    else
                        pcall(Osi.TeleportTo, p, M.ENTRY, "", 1, 1, 1, 0, 1)
                    end
                end
            end
            -- The loading screen (held for a world-mode game until WoW's ground is placed): camp needs no WoW.
            if not visit.Let and (visit.In or ticks - visit.Since > M.LET_IN_AFTER) then
                visit.Let = true
                if not visit.In then Ext.Utils.Print("WOW-BG3 camp: the hero is not in camp after " .. (ticks - visit.Since) .. " ticks; letting BG3 in") end
                pcall(function()
                    for _, p in ipairs(Ext.Plugins.List() or {}) do
                        if p.Name == LAUNCHER and p.Loaded then Ext.Plugins.Set(LAUNCHER, "ready", 1) end
                    end
                end)
            end
            -- A walk off the camp's area is leaving, as Leave Camp is.
            if visit.In then
                visit.Out = inCamp and 0 or visit.Out + 5
                if visit.Out >= M.LEFT_AFTER and #(q(function() return Osi.DB_Camp_NightMode:Get(1) end) or {}) == 0 then
                    send(Hero.REGION, "a walk off camp")
                end
            end
        end)
        if not ok then M.State.Error = tostring(err) end
    end)
end

-- Camp inside our own level (his choice, 2026-11-14): release/levels/azeroth_a.json places BG3's own INTMAIN camp, the
-- level template LT_SCL_Intermezzo_C_ART (sub-level SCL_Intermezzo_C_ART: its ground, scenery, lights, campfire, beds,
-- area, entry and wake-up triggers) and INT_Main_A's camp templates (companion corners, bags, companion triggers) by
-- id, at INT_Main_A's offsets from it, in a corner of the level (references, nothing of Larian's copied). BG3's story
-- knows that camp as INTMAIN of INT_Main_A only (Act2b_CAMP.txt); its rows are written here for our level, as the
-- story writes them, and DB_MainCamp names our level, so Go to Camp, Long Rest and Leave Camp are the story's own
-- teleports inside one level, with no level change: the WoW side stays logged in (the coordinator only takes it down
-- outside the host level, lifecycle.py) and camp is BG3's own picture as in a borrowed level (below).
M.CAMP = "INTMAIN"
M.CAMPFIRE = "S_CAMP_INT_Campfire_29837405-038d-438b-89f5-8d0363f32e16"
local CAMP_ROWS = {
    {"DB_Camp", "INTMAIN", M.CAMPFIRE, "S_CAMP_INT_Area_9ef2ce12-074b-456a-9808-19c80dff20ff",
        "S_CAMP_INT_EntryPoint_ffff4e4a-4e43-4bf1-a54f-bbf3798b90c0"},
    {"DB_Camp_Ambiance", "INTMAIN", "ATM_Intermezzo_C_ART_aec98272-5ee3-473a-b8e8-38cf5467ec09",
        "d9f9c3d0-7835-6bc8-4b04-140bc54f192c", "1795ccf6-2364-16a8-a3f6-b9ee2d709622",
        "LTN_Intermezzo_C_ART_0c1b1b1e-9e74-420a-996c-2dbb4eaad1eb", "fbcc894a-721c-bf4d-6d14-0469a6026756",
        "29e803da-dfea-94a1-f52a-a6c54025e711", "S_CAMP_INT_SoundVolume_54db3a88-53b8-443a-9d4c-827d7db37158"},
    {"DB_InRegionFlag", "S_CAMP_INT_Area_9ef2ce12-074b-456a-9808-19c80dff20ff", "CAMP_INT_State_InRegion_6cbd54fb-ee55-46dc-a97f-2771cc0114d1"},
    {"DB_CampBed", "INTMAIN", 1, "S_Camp_Bedroll1_b57c550d-23ac-861a-369f-62aa1d011b17", "S_CAMP_INT_Bed1_ce371fc0-35e4-4601-b641-42d6063d2c05",
        "LT_CMP_INTMAIN_Day_Bag1_A_5c03f9c3-2833-49ea-905a-b0bc3e602bfa", "NULL_00000000-0000-0000-0000-000000000000",
        "S_CAMP_MorningWakeUp_Trigger1_06229502-d20a-4ed4-87fa-1edc9b0d0473"},
    {"DB_CampBed", "INTMAIN", 2, "S_Camp_Bedroll2_3c608d19-464d-8da0-684a-c9976f035c67", "S_CAMP_INT_Bed2_ae108ddf-e426-4f01-a8a7-265078cb4c7b",
        "LT_CMP_INTMAIN_Day_Bag2_A_8ce891c0-6388-4c82-b9a1-39dfb1f2e017", "NULL_00000000-0000-0000-0000-000000000000",
        "S_CAMP_MorningWakeUp_Trigger2_9f948f7b-e16d-4bf2-b7cb-87e32505e255"},
    {"DB_CampBed", "INTMAIN", 3, "S_Camp_Bedroll3_5bfa7163-8c38-8755-1243-4c7a5fc88f7c", "S_CAMP_INT_Bed3_c0ad058f-c17e-4f56-9c32-e813b64d439c",
        "LT_CMP_INTMAIN_Day_Bag3_A_30567d7d-3f18-4da9-96fa-1abd859c5693", "NULL_00000000-0000-0000-0000-000000000000",
        "S_CAMP_MorningWakeUp_Trigger3_609588b9-973c-462b-bd4a-69749353c225"},
    {"DB_CampBed", "INTMAIN", 4, "S_Camp_Bedroll4_c02b3152-f7a4-88a1-1fdd-230d65312652", "S_CAMP_INT_Bed4_93bc5170-1b9d-45b8-a2c8-172bc75e3df0",
        "LT_CMP_INTMAIN_Day_Bag4_A_9ed7bc89-bbd9-4d73-9c7c-2ad55ca37f5b", "NULL_00000000-0000-0000-0000-000000000000",
        "S_CAMP_MorningWakeUp_Trigger4_973d48b9-69a8-430e-8897-1cabd0db093d"},
}
local CORNERS = {
    {"S_Player_Gale_ad9af97d-75da-406a-ae13-7071c563f604", "LT_CMP_Gale_000_211f5b74-cd22-41d0-bcd7-ce5e08f2bd94"},
    {"S_Player_Astarion_c7c13742-bacd-460a-8f65-f864fe41f255", "LT_CMP_Astarion_000_5d508ba4-1205-4f35-ae36-e6dc9e51f852"},
    {"S_Player_ShadowHeart_3ed74f06-3c60-42dc-83f6-f034cb47c679", "LT_CMP_Shadowheart_000_7e2e16a8-1bee-4428-a26e-eafdb0606c9d"},
    {"S_Player_Laezel_58a69333-40bf-8358-1d17-fff240d7fb12", "LT_CMP_Laezel_000_1cb7018d-2c1c-438f-96df-69af3a2c9b83"},
    {"S_Player_Wyll_c774d764-4a17-48dc-b470-32ace9ce447d", "LT_CMP_Wyll_000_5161ac86-6354-458b-9815-fd73f4c4e4c9"},
    {"S_Player_Karlach_2c76687d-93a2-477b-8b18-8a14b549304c", "LT_CMP_Karlach_000_b83374de-618d-4bbf-8bff-ee89b43eb6ef"},
    {"S_GOB_DrowCommander_25721313-0c15-4935-8176-9f134385451b", "LT_CMP_Minthara_A_000_ebe18958-2683-4f2d-9633-53413b5c41f8"},
    {"S_GLO_Halsin_7628bc0e-52b8-42a7-856a-13a6fd413323", "LT_CMP_Halsin_A_000_07ee3511-8b7b-4721-8690-7c28e8cb9cfa"},
    {"S_Player_Jaheira_91b6b200-7d00-4d62-8dc9-99e8339dfa1a", "LT_CMP_Jaheira_A_000_46d215a0-ac71-4588-9eec-60352a9967da"},
}
for _, c in ipairs(CORNERS) do CAMP_ROWS[#CAMP_ROWS + 1] = {"DB_Camp_PersonalCornerLevelTemplate", "INTMAIN", c[1], c[2]} end

-- The camp stands in this level (the level template is placed and loaded): its campfire is here.
local function gridOn()
    local raw = Ext.IO.LoadFile("wow-bg3/camp-grid.json")  -- the coordinator joined the camp's AI grid to ours
    local g = raw and #raw < 256 and Ext.Json.Parse(raw)
    return type(g) == "table" and g.On == true
end
local function campHere()
    local ok, r = pcall(Osi.GetRegion, M.CAMPFIRE)
    return ok and r == Hero.REGION
end
M.WAIT_CAMP = 1800  -- ticks to wait for the camp's campfire in our level before the camp falls back to its own level

-- The story's rows for INTMAIN in our level, each once (a save keeps them; a row the story has is left alone).
local function registerCamp()
    local added = 0
    for _, row in ipairs(CAMP_ROWS) do
        local db = Osi[row[1]]
        local args = {table.unpack(row, 2)}
        local probe = {table.unpack(args)}
        local have = q(function() return db:Get(table.unpack(probe)) end)
        if not have or #have == 0 then
            if pcall(function() db(table.unpack(args)) end) then added = added + 1 end
        end
    end
    for _, r in ipairs(q(function() return Osi.DB_MainCamp:Get(Hero.REGION, nil) end) or {}) do
        if r[2] ~= M.CAMP then pcall(function() Osi.DB_MainCamp:Delete(r[1], r[2]) end) end
    end
    if #(q(function() return Osi.DB_MainCamp:Get(Hero.REGION, M.CAMP) end) or {}) == 0 then
        Osi.DB_MainCamp(Hero.REGION, M.CAMP); added = added + 1
    end
    if #(q(function() return Osi.DB_Camp_Unlocked:Get(1) end) or {}) == 0 then pcall(function() Osi.DB_Camp_Unlocked(1) end) end
    -- GLO_Camp makes the main camp active at LevelLoaded when none is: these rows come after it.
    if #(q(function() return Osi.DB_ActiveCamp:Get(nil) end) or {}) == 0 then pcall(function() Osi.DB_ActiveCamp(M.CAMP) end) end
    Ext.Utils.Print(string.format("WOW-BG3 camp: INTMAIN in %s (%d story rows written, active camp %s)", Hero.REGION, added,
        Ext.Json.Stringify(q(function() return Osi.DB_ActiveCamp:Get(nil) end) or {})))
end

local function startSame(runId)
    local ticks, seq, pending, outFor = 0, 0, nil, 0
    local own = nil -- where this module just sent the hero: its Teleported arrives ticks later, no one's travel
    local regroup = nil -- after a walk off camp: the hero's walking away undid the party's following
    M.State = {Trips = 0, Corrected = 0}
    local function publish(inCamp, extra)
        seq = seq + 1
        local s = {RunId = runId, Seq = seq, InCamp = inCamp, Hero = trip and trip.Hero or nil,
            From = trip and trip.From or nil}
        for k, v in pairs(extra or {}) do s[k] = v end
        Ext.IO.SaveFile("wow-bg3/camp-state.json", Ext.Json.Stringify(s))
    end
    -- Nothing left from an earlier session (a load starts a new one).
    publish(false)

    -- Fires before the frame's client tick reads the state (server and client share the process, and the
    -- client holds its publish over a jump until it hears: WorldMode.lua).
    local function onTeleported(who, cause, ox, oy, oz, nx, ny, nz, reason)
        if trip or reason == "RegionSwap" or Waypoints.Hopping() then return end
        local w = worldActive()
        if not w then return end
        local hero = Hero.Uuid()
        local dx, dz = nx - ox, nz - oz
        local jump = math.sqrt(dx * dx + dz * dz)
        if jump < M.JUMP then return end
        -- Who made a long jump, on the log's clock: a dismissal sent the hero to camp along with Shadowheart (a test
        -- soak) and no log said which teleport moved the hero. The hero's and every companion's
        -- (DB_PartOfTheTeam: a dismissed one is no player any more), with Osiris's cause; linked characters go
        -- along with a companion's TeleportTo when the story leaves that argument at its default.
        local u = uuidOf(who)
        if u == hero or #(q(function() return Osi.DB_PartOfTheTeam:Get(who) end) or {}) > 0 then
            Ext.Utils.Print(string.format("WOW-BG3 camp: %s jumped %.0f m (%.1f %.1f %.1f to %.1f %.1f %.1f), cause %s, %s",
                u == hero and "the hero" or tostring(who), jump, ox, oy, oz, nx, ny, nz, tostring(cause), tostring(reason)))
        end
        if u ~= hero then return end
        -- Into the void: world mode's own Start. Undoing it put the hero on the host level's spot, inside camp's
        -- ground, about 5 s after each load whose world.json a killed bridge had left Active.
        local c = w.Spot or w.Centre
        if type(c) == "table" and math.sqrt((nx - c[1]) ^ 2 + (nz - c[3]) ^ 2) < M.VOID_REACH then return end
        local mine = own and ticks <= own.Until and math.sqrt((nx - own.X) ^ 2 + (nz - own.Z) ^ 2) < M.JUMP
        own = nil
        pending = {Hero = hero, From = {ox, oy, oz}, Until = ticks + M.PENDING, Own = mine}
    end
    pcall(Ext.Osiris.RegisterListener, "Teleported", 9, "before", onTeleported)

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 5 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, err = pcall(function()
            if ticks % 30 == 0 then noNights() end
            if regroup and ticks >= regroup.At then
                for _, r in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
                    if uuidOf(r[1]) ~= regroup.Hero then pcall(Osi.AttachToPartyGroup, r[1], regroup.Hero) end
                end
                regroup = nil
            end
            if pending then
                if inCampDb(pending.Hero) then
                    trip, pending = {Hero = pending.Hero, From = pending.From}, nil
                    outFor = 0
                    M.State.Trips = M.State.Trips + 1
                    publish(true)
                    Ext.Utils.Print("WOW-BG3 camp: at camp, the WoW side waits at "
                        .. Ext.Json.Stringify(trip.From))
                elseif ticks > pending.Until and pending.Own then
                    pending = nil
                elseif ticks > pending.Until then
                    -- A long teleport that is not to camp (the panel's own clicks are Waypoints.lua's): its
                    -- target is in the level, not on WoW's ground, and the next record would move the WoW body
                    -- there, so the party goes back. Moves on the WoW map are never this: they run with
                    -- world.json Active false.
                    local f, pendingHero = pending.From, pending.Hero
                    pending = nil
                    own = {X = f[1], Z = f[3], Until = ticks + M.PENDING}
                    Osi.TeleportToPosition(pendingHero, f[1], f[2], f[3], "", 1, 1, 1, 0, 1)
                    M.State.Refused = (M.State.Refused or 0) + 1
                    Ext.Utils.Print("WOW-BG3 camp: BG3's own travel in the level undone, the party back on WoW's ground")
                end
            elseif trip then
                outFor = inCampDb(trip.Hero) and 0 or outFor + 5
                if outFor >= M.LEFT_AFTER then
                    local x, y, z = Osi.GetPosition(trip.Hero)
                    local f = trip.From
                    local d = x and math.sqrt((x - f[1]) ^ 2 + (z - f[3]) ^ 2) or 1e9
                    if d > M.BACK_NEAR then
                        own = {X = f[1], Z = f[3], Until = ticks + M.PENDING}
                        Osi.TeleportToPosition(trip.Hero, f[1], f[2], f[3], "", 0, 0, 0, 0, 1)
                        Osi.PROC_Camp_AllPlayersLeave() -- a walk off camp: the party still there comes along
                        regroup = {Hero = trip.Hero, At = ticks + 60}
                        M.State.Corrected = M.State.Corrected + 1
                        Ext.Utils.Print(string.format("WOW-BG3 camp: the story left the hero %.1f m from the spot; put back", d))
                    end
                    local from = trip.From
                    Ext.Utils.Print("WOW-BG3 camp: back from camp")
                    trip = nil
                    publish(false, {Back = from, Off = d})
                end
            end
        end)
        if not ok then M.State.Error = tostring(err) end
    end)
end

-- In our level the camp is ours once its campfire is here: a loaded save has it at the session's start, a New Game only
-- some seconds later (its first session started before the level's templates were in: the camp took its own level, a
-- New Game on slot B). Checked each second until it is, then the story's rows and the same-level trip; with no
-- joined grid, or no campfire by M.WAIT_CAMP, the trip to INT_Main_A as before.
function M.Start(runId)
    if not M.On() then return end
    if not Hero.OWN then return startSame(runId) end
    if not gridOn() then return startOwn(runId) end
    local waited, sub, decided = 0, nil, false
    sub = Ext.Events.Tick:Subscribe(function()
        if decided then return end
        waited = waited + 1
        if waited % 30 ~= 0 then return end
        local here = Ext.Utils.GetGameState() == "Running" and campHere()
        if not here and waited < M.WAIT_CAMP then return end
        decided = true
        pcall(function() Ext.Events.Tick:Unsubscribe(sub) end)
        if here then
            registerCamp()
            startSame(runId)
        else
            Ext.Utils.Print("WOW-BG3 camp: no camp in our level after " .. waited .. " ticks: camp is INT_Main_A")
            startOwn(runId)
        end
    end)
end

return M
