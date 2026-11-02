-- TurnQueue (client, no game calls): which turn WoW hears next, from BG3's turn events and the
-- monitor's flag for the hero's turn. Tested without a game: tools/test_turn_queue.py.
--
-- BG3 fires the next TurnStarted before the last TurnEnded, in one tick (seen in the monitor's
-- events): a hero's TurnEnded can arrive while her TurnStarted still waits here. That entry is
-- over: it goes, or everything behind it would wait for her next round.
local M = {}

function M.New()
    return {pending = {}, heroOpen = false, since = nil, endWait = 0}
end

function M.Started(q, id)
    q.pending[#q.pending + 1] = id
    if id == "hero" then q.heroOpen = true end
end

function M.Ended(q, id)
    if id ~= "hero" then return false end
    q.heroOpen = false
    for i, p in ipairs(q.pending) do
        if p == "hero" then
            table.remove(q.pending, i)
            return true
        end
    end
    return false
end

-- A turn of the hero's that WoW was not told about, read from one monitor report:
--   o.Active    the report's flag for her turn
--   o.Tick      the report's tick (events and flag of a report are written together)
--   o.TurnActor whose turn WoW last heard (nil: none yet, or WoW just ended hers)
--   o.Busy      a BG3-run turn is being watched or an action is outstanding
--   o.Step      ticks since the last call
-- Returns nil or why:
--   "no-event"     no TurnStarted past where reading began (the fight began inside her turn)
--   "overlap"      a BG3-run turn started and ended inside hers, and hers runs on
--   "end-ignored"  WoW ended her turn and after 90 ticks BG3 has not
-- The flag can be set a moment before the event of the same tick is written, so a turn with no
-- event counts only when a second report still shows it.
function M.Unheard(q, o)
    if o.Busy or not o.Active or o.TurnActor == "hero" or q.pending[1] then
        q.since, q.endWait = nil, 0
        return nil
    end
    if o.TurnActor == nil and q.heroOpen then
        q.endWait = q.endWait + (o.Step or 0)
        if q.endWait < 90 then return nil end
        q.endWait = 0
        return "end-ignored"
    end
    if q.since == nil then
        q.since = o.Tick
        return nil
    end
    if o.Tick == q.since then return nil end
    q.since = nil
    return q.heroOpen and "overlap" or "no-event"
end

return M
