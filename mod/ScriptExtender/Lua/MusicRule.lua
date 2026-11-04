-- Which game's music plays in world mode: it follows the character the player controls.
--   Controlled character in a BG3 fight: BG3's fight music; WoW's zone music yields (benilla 0120).
--   Controlled character not in one: WoW's music, even with a companion or a summon fighting elsewhere.
--   At camp: BG3's own camp music, as ever; leaving camp ends it.
--
-- Controlled is Osiris GetHostCharacter (the selected character, a summon too), not Hero.lua's hero, which
-- stays the avatar while a companion is selected. BG3 selects whoever's turn it is in a fight, and then that
-- one is the controlled character. A downed character is still in its fight.
--
-- Leaving camp. BG3's music picks exploration tracks by the Wwise states Music_Set and a per-set state. Level
-- triggers set them: MusicVolumeTrigger_BGO_Camp_Farm (x 261 to 474, z -854 to -644, around the camp) posts
-- Camp_BGO on entry. A later trigger is what ends a set, and none of BGO_Main_A's 41 music triggers reaches
-- the void, so after a trip the camp's set stayed. The server posts Music_Off again when Camp.lua says a trip
-- is over: BG3's own event, the one world mode's Start plays, which sets both states back to None.
--
-- A switch of control. On every selection BG3 posts the new character's last music trigger again (measured
-- after a camp trip: Camp_BGO, Music_Fight_End, Fight_Theme_Percussive_02_On on the music object, whoever is
-- selected, near or far), so the camp's music starts over WoW's. Outside a camp trip the server posts Music_Off
-- after each change of the controlled character. It leaves running fight music alone (measured).
--
-- A far fight. BG3's fight music sits on its own bus under RTPC_Volume_Music_Fight, so BG3's fight state can
-- run on untouched. While someone of the party's side fights and the controlled character does not, that
-- volume is 0; the controlled character entering a fight (a switch to the fighter too) puts it back. It stays
-- at 0 after a far fight's end, so that fight's win stinger is not heard either, until the controlled
-- character next fights or goes to camp. A fight the controlled character was in never closes it, so its
-- stinger plays and WoW comes back 8 s later (benilla 0030); a switch away shuts it at once and benilla brings
-- WoW's music straight back (tav.bin flags 8).
--
-- wow-bg3/music-state.json {RunId, Seq, Uuid, InFight, Anyone, CampOffs, SwitchOffs}: the server's reading, for the client
-- and for anyone checking. wow-bg3/music-gate.json {Value}: the player's volume while it is at 0, since an
-- RTPC outlives a load and this Lua state does not.
local M = {}

local STATE = "wow-bg3/music-state.json"
local GATE = "wow-bg3/music-gate.json"
local RTPC = "RTPC_Volume_Music_Fight"
local EVERY = 5 -- server ticks between readings
local CHECK = 10 -- client calls between looks at the volume (BG3 setting it again: the player's slider)

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end
local function say(s) Ext.Utils.Print("WOW-BG3 music: " .. s) end

-- Server ------------------------------------------------------------------------------------------

function M.Start(runId)
    local Camp = Ext.Require("Camp.lua")
    local Party = Ext.Require("Party.lua")
    local ticks, seq, last, inTrip, campOffs, host, switchOffs = 0, 0, nil, false, 0, nil, 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % EVERY ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local nowTrip = Camp.InTrip()
        if inTrip and not nowTrip then
            local ok, err = pcall(Osi.MusicPlayGeneral, "Music_Off")
            campOffs = campOffs + 1
            say(ok and "left camp, BG3's exploration music off" or ("Music_Off failed: " .. tostring(err)))
        end
        inTrip = nowTrip
        local u = uuidOf(q(Osi.GetHostCharacter))
        if u ~= nil and u ~= host then
            if host ~= nil and not nowTrip then
                local ok, err = pcall(Osi.MusicPlayGeneral, "Music_Off")
                switchOffs = switchOffs + 1
                if not ok then say("Music_Off failed: " .. tostring(err)) end
            end
            host = u
        end
        local anyone = q(Party.Combat) ~= nil
        local inFight = u ~= nil and (q(Osi.IsInCombat, u) == 1
            or (anyone and q(Osi.HasActiveStatus, u, "DOWNED") == 1))
        local key = table.concat({tostring(u), tostring(inFight), tostring(anyone), campOffs, switchOffs}, " ")
        if key == last then return end
        last, seq = key, seq + 1
        Ext.IO.SaveFile(STATE, Ext.Json.Stringify({RunId = runId, Seq = seq, Uuid = u, InFight = inFight,
            Anyone = anyone, CampOffs = campOffs, SwitchOffs = switchOffs}))
    end)
end

-- Client ------------------------------------------------------------------------------------------

local shut, calls, state = false, 0, nil
local function setVolume(v) return q(Ext.Audio.SetRTPC, "Global", RTPC, v, true) end
local function readGate()
    local ok, g = pcall(Ext.Json.Parse, q(Ext.IO.LoadFile, GATE) or "")
    return ok and type(g) == "table" and type(g.Value) == "number" and g.Value or nil
end

local function open()
    local v = readGate()
    shut = false
    if not v then return end
    setVolume(v)
    Ext.IO.SaveFile(GATE, "{}")
    say("fight music back, volume " .. tostring(v))
end

local function close()
    local v = q(Ext.Audio.GetRTPC, "Global", RTPC)
    if type(v) ~= "number" then return end -- no volume to go by: leave BG3's music alone
    shut = true
    if v <= 0 then return end
    Ext.IO.SaveFile(GATE, Ext.Json.Stringify({Value = v}))
    setVolume(0)
    say("fight music shut, volume was " .. tostring(v))
end

-- What a load left behind: this Lua state is new and knows nothing of a volume at 0.
local restored = false
Ext.Events.Tick:Subscribe(function()
    if restored or not Ext.IsClient() or Ext.Utils.GetGameState() ~= "Running" then return end
    restored = true
    open()
end)

-- From WorldMode.lua's publishTav: applies the rule and returns tav.bin's flags 4 (the controlled character is
-- in a BG3 fight) and 8 (BG3's fight music is shut).
function M.Flags(runId, atCamp)
    local ok, s = pcall(Ext.Json.Parse, q(Ext.IO.LoadFile, STATE) or "")
    if ok and type(s) == "table" and s.RunId == runId then state = s end
    local inFight = state ~= nil and state.InFight == true
    if atCamp or inFight then
        if shut or readGate() then open() end
    elseif state and state.Anyone == true and not shut then
        close()
    end
    if shut then
        calls = calls + 1
        if calls % CHECK == 0 then
            local v = q(Ext.Audio.GetRTPC, "Global", RTPC)
            if type(v) == "number" and v > 0 then close() end
        end
    end
    return (inFight and 4 or 0) | (shut and 8 or 0)
end

-- World mode stops: BG3's music is BG3's again.
function M.Release()
    open()
    state = nil
end

return M
