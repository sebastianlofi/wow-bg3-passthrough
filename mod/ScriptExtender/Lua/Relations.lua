-- A stand-in's relation to the party (Mirrors.lua), by its WoW mob's reaction (benilla's mobs.txt
-- REACTION, mirror.json Hostile and Friendly). Pure Lua, no Ext (tools/test_relations.py).
--
-- BG3 decides what a left click on a character does by its relation: on an enemy (a hostile
-- faction) it is an attack, with BG3's attack cursor; on anyone else BG3 walks there or talks.
-- WoW's sword over a neutral (yellow) mob promises an attack, and most of them are there to be
-- killed (playtest feedback: "keep the killing default"), so a WoW-neutral mob's stand-in has the hostile
-- faction, like a hostile (red) one's. Friendly (green) NPCs keep the neutral faction: BG3's left
-- click talks or walks, and attacking one is BG3's crime flow (Ctrl or an aimed attack).
--
-- Who notices whom stays WoW's: in sight mode (server patch 0002) BG3's sight decides a hostile
-- mob's aggro, so a neutral mob's stand-in, hostile in BG3, cannot join a combat (Osiris
-- SetCanJoinCombat 0) until a party member aims at or hits that one (Mirrors.lua's provoke), and
-- BG3 starts no fight when it sees Tav. Once provoked it joins and fights back as any enemy does.
-- After CALM ticks out of every fight and BG3 combat it forgets, as a WoW mob that evaded does:
-- it cannot join a combat again until it is attacked again.
local M = {}

M.CALM = 300 -- ticks (about 10 s) out of every fight before a provoked neutral stand-in forgets

-- The faction id a stand-in of mob gets from plan (mirror.json: Hostile, Neutral), or nil to keep
-- its template's own.
function M.FactionOf(mob, plan)
    if type(mob) ~= "table" or type(plan) ~= "table" then return nil end
    local hostile = type(plan.Hostile) == "string" and plan.Hostile or nil
    local neutral = type(plan.Neutral) == "string" and plan.Neutral or nil
    if mob.Hostile == true then return hostile end
    if mob.Friendly == true then return neutral end
    return hostile or neutral
end

-- Whether the stand-in may join a combat from its spawn (BG3's sight starts its fight): WoW's
-- hostile mobs only. Neutral mobs and friendly NPCs wait until they are provoked.
function M.JoinsOnSight(mob)
    return type(mob) == "table" and mob.Hostile == true
end

-- Whether a provoked stand-in goes back to its unprovoked state now: a neutral mob (not a friendly
-- NPC, whose reset is its crime flow's) out of the WoW fight and out of BG3's combat, alive, whose
-- last aim, hit, fight or combat was more than CALM ticks before now. provokedAt: that tick.
function M.Calms(mob, provokedAt, now, inFight, inCombat, dead)
    if type(mob) ~= "table" or mob.Hostile == true or mob.Friendly == true then return false end
    if type(provokedAt) ~= "number" or inFight or inCombat or dead then return false end
    return now - provokedAt > M.CALM
end

return M
