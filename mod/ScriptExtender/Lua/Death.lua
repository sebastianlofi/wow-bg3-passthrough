-- Death, BG3 rules (server; the party-multiplayer-design notes
-- "Death", the save-sync notes "Death").
--
-- BG3 decides who lives. A party member down (0 HP, DOWNED) or dead in BG3 is a dead WoW character,
-- dead in place; a member BG3 brings back (Help, a revive scroll, Withers, a long rest...) is a WoW
-- character resurrected in place. WoW's release spirit and graveyards are never used (the
-- interaction code hides the button, the server patch refuses the release).
--   * BG3's game over is BG3's again: world mode used to keep it off by adding Dead and Downed to the
--     story's list of defeats that are no game over (DB_GameOver_DefeatCausesNotGameOver, StandIns.lua
--     GameOver, called by WorldMode.lua Start and Stop). This module takes those two rows out whenever
--     they appear (a save made in world mode carries them too), so a whole party down shows BG3's own
--     game over; the player loads a save and save sync rolls every WoW character back.
--   * Every party member's life is watched (every 10 ticks); each change is an event in
--     wow-bg3/death.json {RunId, Session, Seq, Events = [{Seq, Uuid, Name, Hero, Char, What, Hp, MaxHp}]}
--     (What: "down", "dead", "up"; the last 32 kept). The lifecycle watcher (link/tools/lifecycle.py)
--     carries them out on the WoW server: kill in place, resurrect in place at BG3's HP fraction.
--     In a fight the hero's WoW death also comes from the fight bridge (`.wowbg3 hero ... 0`); the
--     server's kill is a no-op on a dead character.
-- The session's first look at the party records states without events (a loaded save: the restored
-- WoW characters already match it).
-- A member down or dead in a BG3 fight is told once the fight is over for it (out of combat), if it is
-- still so then: BG3 brings a downed member back in the fight (Help, a potion, Relentless Endurance
-- at 0 HP), and the WoW character's death ended the server's fight at once, so the mobs went home
-- (a playtest report: Tav at 1 HP through Relentless Endurance, WoW Tav killed, the fight gone). Out
-- of a fight a "down" must also hold for M.DownHold ticks (a 0 HP moment before such a save).
-- That watch sees BG3's changes only, and a WoW character can die under a hero BG3 never counted down: the
-- bridge's `hero ... 0` at a 0 HP moment, a downed hero helped up in the fight, a WoW-side death in BG3's
-- combat. Mirrors.lua, which knows both states, calls M.ReviveWow while they differ.
local M = {}
local Hero = Ext.Require("Hero.lua")
local Party = Ext.Require("Party.lua")
local CHARVAR = "WowBg3Char"
pcall(Ext.Vars.RegisterUserVariable, CHARVAR, {Server = true, Client = false, SyncToClient = false, Persistent = true})

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    return ok and v or nil
end

-- BG3's game over back (see the header). Returns the rows taken out.
function M.FreeGameOver()
    local out = {}
    for _, cause in ipairs({"Dead", "Downed"}) do
        if #(q(function() return Osi.DB_GameOver_DefeatCausesNotGameOver:Get(cause) end) or {}) > 0 then
            if pcall(function() Osi.DB_GameOver_DefeatCausesNotGameOver:Delete(cause) end) then out[#out + 1] = cause end
        end
    end
    return out
end

-- A member's life: "dead" (Osiris IsDead), "down" (DOWNED or 0 HP: BG3's downed player character),
-- or "up".
local function life(u)
    if q(Osi.IsDead, u) == 1 then return "dead" end
    local e = q(Ext.Entity.Get, u)
    local hp = e and q(function() return e.Health.Hp end)
    if q(Osi.HasActiveStatus, u, "DOWNED") == 1 or (type(hp) == "number" and hp <= 0) then return "down" end
    return "up"
end

M.DownHold = 30 -- ticks
function M.Start(runId)
    local session = Hero.SESSION
    local seen, events, seq, ticks = nil, {}, 0, 0
    local held = {} -- uuid -> tick it was first seen down or dead while still counted up
    local freed = 0
    local function save()
        Ext.IO.SaveFile("wow-bg3/death.json", Ext.Json.Stringify({RunId = runId, Session = session, Seq = seq,
            Events = events, GameOverFreed = freed}))
    end
    local function event(u, what, hero)
        local e = q(Ext.Entity.Get, u)
        local h = e and q(function() return {e.Health.Hp, e.Health.MaxHp} end) or {0, 0}
        local char = e and q(function() return e.Vars[CHARVAR] end)
        seq = seq + 1
        events[#events + 1] = {Seq = seq, Uuid = u, Name = q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(u)) end),
            Hero = hero, Char = type(char) == "string" and char or nil, What = what, Hp = h[1], MaxHp = h[2],
            Time = Ext.Utils.MonotonicTime()}
        if #events > 32 then table.remove(events, 1) end
        Ext.Log.Print("WOW-BG3 death: " .. tostring(events[#events].Name) .. " " .. what)
        save()
    end
    -- The WoW character is dead and the hero is up in BG3: BG3 decides, so an "up" event goes out (the
    -- server's revive answers `alive` to a living character). False if BG3 has the hero down or dead too.
    M.ReviveWow = function()
        local u = Hero.Uuid()
        if not u or life(u) ~= "up" then return false end
        event(u, "up", true)
        return true
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 10 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        if ticks % 30 == 0 then
            local out = M.FreeGameOver()
            if #out > 0 then
                freed = freed + #out
                Ext.Log.Print("WOW-BG3 death: BG3's game over back (" .. table.concat(out, ", ") .. " taken off the no-game-over list)")
                save()
            end
        end
        local hero = Hero.Uuid()
        if not hero then return end
        -- A new session (a load): its first look records states without events.
        if session ~= Hero.SESSION then session, seen, events = Hero.SESSION, nil, {}; save() end
        local now = {}
        for _, u in ipairs(Party.Party()) do
            local st = life(u)
            local was = seen and seen[u]
            if st ~= "up" and was == "up" then
                -- Held as "up" until it counts (see the header).
                local first = held[u] or ticks
                held[u] = first
                if q(Osi.IsInCombat, u) == 1 or (st == "down" and ticks - first < M.DownHold) then st = "up" end
            else
                if st == "up" then held[u] = nil end
            end
            now[u] = st
        end
        if seen then
            for u, st in pairs(now) do
                local was = seen[u]
                if was and was ~= st then
                    if st == "up" then event(u, "up", u == hero)
                    elseif was == "up" then event(u, st, u == hero)
                    elseif st == "dead" then event(u, "dead", u == hero) end
                end
            end
        end
        seen = now
    end)
    save()
end

return M
