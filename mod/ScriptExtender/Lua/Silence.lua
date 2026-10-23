-- Stand-ins make no sound (the hidden BG3 wolves howled).
-- WoW draws the world's creatures and plays their sounds (benilla 0030); BG3 plays Tav's sounds, her
-- abilities' and its fight music. A stand-in's own BG3 sounds go to its Wwise game objects on BG3's
-- client: ls::SkeletonSoundObjectsComponent holds up to nine (Lua slot 1 is the character's own
-- ls::SoundComponent, the others are sound objects on bones), and BG3's systems post on them (harness
-- traces): animation text keys on the head object (a wolf's idle howls
-- Wolf_Idle_Random_Peace_*, its attack vocal), vocals (Wolf_Pain_Generic), footsteps, movement and barks
-- on the main object (Movement_Footstep, Dog_Crime_AD), the stand-in's own spell cast (CrSpell_Cast_Bite)
-- and its whooshes on the hand objects.
--
-- Every tick each stand-in's game objects are taken out of Wwise's mix with the plugin's op 150
-- (AK::SoundEngine::SetListeners with no listener: Wwise renders nothing they emit, whatever posts it),
-- except the body object (Lua slot 4, C++ slot 3): BG3 plays an attacker's hit on the victim's body object
-- (Tav's sword on a wolf: Weapon_Combat on the wolf's; a wolf's bite on Tav: Weapon_Combat and
-- CrSpell_Impact_Bite on hers; her misses Character_Combat_Miss_Activate), so Tav's impacts on a
-- stand-in stay. On that object BG3's own game-object mutes take the stand-in's footsteps, armour foley
-- and dialogue off (M.KeepEvents: Wwise Mute actions scoped to the one game object, as BG3's cinematics
-- use them). A dead stand-in's body object goes too, M.DeadKeepMs after its HP reached 0 (the killing
-- blow's impact sounds first; then its ragdoll's Movement_Ragdoll_Impact falls there, harness tr9).
-- Without op 150 (an older plugin) the other objects get BG3's own GLO_Cinematics_MuteEntity instead
-- (mutes BG3's world, cinematic and dialogue sounds on that object; not the action and spell sounds;
-- BG3 posts it on every effect it ends).
-- Stand-ins: every mob Mirrors.lua lists (mirror-status.json, also the ones a fight adopted) and a fight's
-- own spawns (LinkLive status.json Scaled, Uuid). Only in world mode. A game object BG3 registers anew
-- (a respawn, sound activation) is taken the next tick. Status: wow-bg3/link/silence-status.json.
-- Research (harness): the client global WOWBG3Silence is this module (On, Restore(), Stats()).
local M = {}
local N = Ext.Require("WowNative.lua")
local World = Ext.Require("WorldMode.lua")
local DIR = "wow-bg3/link/"
local OP_SOUND_MUTE = 150
M.On = true -- research knobs
M.KeepSlot = 4
M.DeadKeepMs = 600
M.KeepEvents = {"GLO_Cinematics_Mix_Intro_MuteFootsteps", "Cinematic_Systemic_ArmorFoley_MuteGameObject",
    "GLO_Cinematics_MuteDialogue"}
M.UnkeepEvents = {"GLO_Cinematics_Mix_Outro_MuteFootsteps", "Cinematic_Systemic_ArmorFoley_UnMuteGameObject",
    "GLO_Cinematics_UnmuteDialogue"}
M.Fallback = "GLO_Cinematics_MuteEntity"

local function readJson(path)
    local raw = Ext.IO.LoadFile(path)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

function M.Start(runId)
    WOWBG3Silence = M
    local ticks, listAt = 0, -1000
    local standins = {} -- uuid -> "mirror" | "fight"
    local done = {}     -- uuid -> {[slot] = {id, how}}
    local deadAt = {}   -- uuid -> MonotonicTime (ms) its HP was first seen at 0
    local native = nil  -- true: op 150 works, false: fallback (retried every 600 ticks), nil: not tried
    local nativeAt = -1000
    local stats = {Standins = 0, Objects = 0, Muted = 0, Kept = 0, Fallback = 0, Errors = 0, Ticks = 0,
        CostMs = 0, CostMax = 0, Party = 0, DeadQuiet = 0}
    local function readList()
        local out = {}
        local st = readJson(DIR .. "mirror-status.json")
        if st and (st.RunId == nil or st.RunId == runId) and type(st.Mobs) == "table" then
            for _, m in pairs(st.Mobs) do
                if type(m) == "table" and type(m.Uuid) == "string" then out[m.Uuid] = "mirror" end
            end
        end
        local ls = readJson(DIR .. "status.json")
        if ls and (ls.RunId == nil or ls.RunId == runId) and type(ls.Scaled) == "table" then
            for _, s in pairs(ls.Scaled) do
                if type(s) == "table" then
                    for _, k in ipairs({"Adopted", "Uuid"}) do
                        if type(s[k]) == "string" and not out[s[k]] then out[s[k]] = "fight" end
                    end
                end
            end
        end
        standins = out
    end
    local function soundId(se)
        local ok, id = pcall(function() return se.Sound.ActiveData.SoundObjectId end)
        if ok and math.type(id) == "integer" and id > 0 then return id end
    end
    -- Op 150 on one game object (mode 1 no listener, 0 the default ones back).
    local function op(id, mode)
        local out = N.Call(OP_SOUND_MUTE, id & 0xffff, (id >> 16) & 0xffff, (id >> 32) & 0xffff, (id >> 48) & 0xffff, mode, 0)
        if out.Err ~= 0 then return false, tostring(out.Why or out.Err) end
        return true
    end
    local function take(se, id)
        if native ~= false then
            local ok, res, why = pcall(op, id, 1)
            if ok and res == true then native = true; return "listeners" end
            local msg = tostring(ok and why or res)
            stats.LastError = msg
            -- No op 150 (an older plugin, none, its functions not found): BG3's own entity mute until
            -- it is tried again.
            local off = false
            for _, w in ipairs({"unknown plugin op", "channel", "not installed", "plugin failed", "Ext.Plugins",
                "listener functions", "differs", "resolver", "did not take"}) do
                off = off or msg:find(w, 1, true) ~= nil
            end
            if off then
                native, nativeAt = false, ticks
            else
                stats.Errors = stats.Errors + 1
                return nil
            end
        end
        local okp, r = pcall(Ext.Audio.PostEvent, se, M.Fallback)
        if okp and r then stats.Fallback = stats.Fallback + 1; return "bg3-mute" end
        stats.Errors = stats.Errors + 1
        stats.LastError = "fallback: " .. tostring(r)
        return nil
    end
    local function pass()
        if ticks - listAt >= 5 then listAt = ticks; readList() end
        if native == false and ticks - nativeAt > 600 then native = nil end
        local nStand, nObj = 0, 0
        local now = Ext.Utils.MonotonicTime()
        for uuid in pairs(done) do if not standins[uuid] then done[uuid] = nil end end
        for uuid in pairs(deadAt) do if not standins[uuid] then deadAt[uuid] = nil end end
        for uuid in pairs(standins) do
            local e = Ext.Entity.Get(uuid)
            local okS, sk = pcall(function() return e and e.SkeletonSoundObjects end)
            -- Never the player's own characters (a stand-in has no party membership).
            local okP, party = pcall(function() return e and e.PartyMember ~= nil end)
            if okP and party then
                stats.Party = stats.Party + 1
            elseif okS and sk then
                nStand = nStand + 1
                local d = done[uuid] or {}
                done[uuid] = d
                local okH, hp = pcall(function() return e.Health.Hp end)
                if okH and type(hp) == "number" and hp <= 0 then deadAt[uuid] = deadAt[uuid] or now else deadAt[uuid] = nil end
                local quiet = deadAt[uuid] ~= nil and now - deadAt[uuid] >= M.DeadKeepMs
                for i = 1, 9 do
                    local okE, se = pcall(function() return sk.SoundEntities[i] end)
                    local id = okE and se and soundId(se)
                    if not id then
                        d[i] = nil
                    else
                        nObj = nObj + 1
                        local rec = d[i]
                        local fresh = not rec or rec[1] ~= id
                        if i == M.KeepSlot and not quiet then
                            if fresh or rec[2] ~= "kept" then
                                -- (alive again: the listeners its death took come back)
                                if not fresh and rec[2] == "listeners" then pcall(op, id, 0) end
                                if not fresh and rec[2] == "bg3-mute" then pcall(Ext.Audio.PostEvent, se, "GLO_Cinematics_UnMuteEntity") end
                                for _, ev in ipairs(M.KeepEvents) do pcall(Ext.Audio.PostEvent, se, ev) end
                                d[i] = {id, "kept"}
                                stats.Kept = stats.Kept + 1
                            end
                        elseif fresh or rec[2] == "kept" then
                            local how = take(se, id)
                            if how then
                                d[i] = {id, how}
                                if i == M.KeepSlot then stats.DeadQuiet = stats.DeadQuiet + 1 else stats.Muted = stats.Muted + 1 end
                            end
                        end
                    end
                end
            end
        end
        stats.Standins, stats.Objects = nStand, nObj
    end
    -- Research: every handled object back as BG3 had it (op 150 mode 0, the keep events undone); M.On off.
    function M.Restore()
        M.On = false
        local n = 0
        for uuid, d in pairs(done) do
            local e = Ext.Entity.Get(uuid)
            local okS, sk = pcall(function() return e and e.SkeletonSoundObjects end)
            for i, rec in pairs(d) do
                local okE, se = pcall(function() return okS and sk and sk.SoundEntities[i] end)
                se = okE and se or nil
                if se and soundId(se) == rec[1] then
                    if rec[2] == "listeners" then pcall(op, rec[1], 0)
                    elseif rec[2] == "kept" then for _, ev in ipairs(M.UnkeepEvents) do pcall(Ext.Audio.PostEvent, se, ev) end
                    elseif rec[2] == "bg3-mute" then pcall(Ext.Audio.PostEvent, se, "GLO_Cinematics_UnMuteEntity") end
                    n = n + 1
                end
            end
        end
        done = {}
        return n
    end
    function M.Stats() return stats end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if not M.On or Ext.Utils.GetGameState() ~= "Running" or not World.IsActive() then return end
        local t0 = Ext.Utils.MonotonicTime()
        local ok, err = pcall(pass)
        if not ok then stats.Errors = stats.Errors + 1; stats.LastError = tostring(err) end
        local ms = Ext.Utils.MonotonicTime() - t0
        stats.Ticks = stats.Ticks + 1
        stats.CostMs = stats.CostMs + ms
        if ms > stats.CostMax then stats.CostMax = ms end
        if ticks % 60 == 0 then
            stats.RunId, stats.Mode = runId, native == true and "listeners" or (native == false and "bg3-mute" or "untried")
            stats.CostAvg = stats.Ticks > 0 and stats.CostMs / stats.Ticks or 0
            Ext.IO.SaveFile(DIR .. "silence-status.json", Ext.Json.Stringify(stats))
        end
    end)
end
return M
