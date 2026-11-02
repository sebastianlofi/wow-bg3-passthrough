-- The stand-in under WoW's model (world mode, client; driven by WowCursor.lua's tick, plugin op 32).
-- WoW draws a mirrored mob or NPC with its own model and BG3's cursor picks only the stand-in's own body,
-- which is often smaller (a Thunder Lizard's stand-in is BG3's wolf) and not under the same pixels: on the
-- model BG3 picked ground, so a click walked and an attack, spell, shove, throw, talk or pickpocket aimed at
-- it found nobody. While WoW has such a unit under the pointer (wowhover.bin: no WoW UI there, the unit's
-- guid) and it has a living stand-in, this names that stand-in's feet to the plugin every tick; the plugin
-- casts BG3's cursor character query toward its body when BG3 picks no character itself
-- (plugins/wowbg3_standin_aim.h). BG3 then picks its own stand-in: its own cursor shows, and its click does
-- what BG3 does there, by BG3's rules. One path for every action; nothing here clicks or moves the pointer.
-- Pure (no Ext at load): tools/test_standin_aim.py runs it without a game.
local M = {}

-- The stand-in to name, or nil. flags, unit: wowhover.bin's flags and unit guid (hex, lower case); window: a
-- BG3 window is open (trade, pickpocket, examine, its menu: the pointer is that window's); byGuid: WoW guid ->
-- uuid of each living stand-in (link/mirror-status.json).
function M.Target(flags, unit, window, byGuid)
    if not flags or flags & 1 ~= 0 or window or type(unit) ~= "string" or type(byGuid) ~= "table" then return nil end
    return byGuid[unit:lower()]
end

-- One tick. st: this module's state ({} at first); target: M.Target's answer; feet(uuid): its client entity's
-- position {x, y, z} or nil; call(mode, x, y, z): the plugin op (mode 1 named there, 0 none), returns its out
-- or nil. Named every tick while it holds (the plugin forgets it 0.5 s after the last), none once when it
-- ends. Returns a line for the log when the named stand-in changes, else nil.
function M.Step(st, target, feet, call)
    local p = target and feet(target) or nil
    if p then
        local out = call(1, p[1], p[2], p[3])
        if out then st.Out = out end
        if st.Uuid == target then return nil end
        st.Uuid, st.Named = target, (st.Named or 0) + 1
        return "stand-in aim on " .. target
    end
    if not st.Uuid then return nil end
    local out = call(0, 0, 0, 0)
    if out then st.Out = out end
    local was = st.Uuid
    st.Uuid = nil
    local o = st.Out
    local counts = o and o.R and string.format(" (queries so far: %d turned to a stand-in, %d missed it, %d with BG3's own character first)",
        o.R[2] or 0, o.R[3] or 0, o.R[4] or 0) or ""
    return "stand-in aim off " .. was .. counts
end

return M
