-- A stand-in whose join failed (LinkLive.lua): is it in BG3's fight anyway? A mirror is a BG3
-- character before any fight, and BG3 pulls a hostile one near its combat in by itself (its ally
-- join, Mirrors.lua's pack join); a Join whose answer was lost may have run. So the answer is BG3's,
-- by the server context's word (Mirrors.lua's link/mirror-status.json): Mobs for a mirror, FightSpawned
-- for a stand-in the fight spawned. In the party's combat, or dead (BG3 killed it: that is WoW's kill
-- too), it is a fight member. Pure Lua, no Ext (tools/test_join_watch.py).
local M = {}

-- true: a member; false: not in BG3's fight; nil: the status did not read.
function M.Member(status, guid, uuid, mirror)
    if type(status) ~= "table" then return nil end
    local m
    if mirror then
        m = type(status.Mobs) == "table" and status.Mobs[guid] or nil
        -- Gone, or remade as another stand-in: not the one this fight knows.
        if type(m) ~= "table" or m.Uuid ~= uuid then return false end
    else
        m = type(status.FightSpawned) == "table" and status.FightSpawned[uuid] or nil
        if type(m) ~= "table" then return false end
    end
    return m.InCombat == true or m.Dead == true
end

return M
