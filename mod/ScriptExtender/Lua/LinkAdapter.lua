-- WOW-BG3 link adapter, BG3 role. Turns validated WoW requests into backend calls and
-- backend events into outgoing protocol messages. It owns no rules: the backend must use
-- BG3's normal engine paths, and a fixture backend is used only for protocol cross-tests.
--
-- Backend interface (all calls validate first and perform side effects only on success):
--   begin(m) -> true [, "deferred"] | nil, why   move(m) -> true | nil, why
--   (deferred: the backend sets the encounter up first and calls ready() when done)
--   cast(m)  -> true | nil, why        end_turn(m) -> true | nil, why
--   cleanup(m) -> true | nil, why      abort(reason)
--   join(m) -> true | nil, why  (world fights: more enemies; the backend sets them up and
--   reports roster(joins, actors) with joins = Join messages handled so far)
--   poll(adapter)  -- deliver asynchronous engine events through the adapter methods below
--
-- An accepted move/cast is an attempt. If BG3's normal engine path then refuses it, the
-- backend reports rejected(action, reason, detail); the action id stays spent and the turn
-- continues. Protocol violations by WoW are still refused (never acknowledged).
--
-- On the turn of a combatant BG3 controls (any actor but "hero" for now), the backend
-- reports what it did with acted(action, spell, target, points) and the damage with
-- effect(action, ...) under the same action id; WoW sends nothing for that turn.
local A = {}
A.__index = A
local MAX_QUEUE = 8

function A.new(Link, backend)
    return setmetatable({Link = Link, backend = backend, phase = "idle", turn = 0, queue = {},
        used = {}, actors = {}}, A)
end

local function refuse(why) return nil, why end

function A:push(message)
    local ok, why = self.Link.validate(message, "bg3")
    if not ok then return self:fail("backend produced invalid message: " .. why) end
    if #self.queue >= MAX_QUEUE then return self:fail("outgoing queue full") end
    self.queue[#self.queue + 1] = message
    return true
end
function A:next() return table.remove(self.queue, 1) end
function A:idle() return #self.queue == 0 end

-- Called by the endpoint only for a new, in-sequence, decoded WoW message.
function A:apply(m)
    if #self.queue >= MAX_QUEUE then return refuse("adapter queue full") end
    if m.type == "abort" then
        if self.phase ~= "idle" and m.encounter ~= self.encounter then return refuse("abort for another encounter") end
        self.backend.abort(m.reason)
        self.queue, self.phase, self.pending = {}, "aborted", nil
        return true
    end
    if m.type == "begin" then
        if self.phase ~= "idle" then return refuse("begin outside idle phase") end
        local ok, why = self.backend.begin(m)
        if not ok then return refuse(why or "backend refused begin") end
        self.encounter, self.phase = m.encounter, "active"
        for _, a in ipairs(m.actors) do self.actors[a.id] = true end
        if why == "deferred" then self.phase = "starting"; return true end
        self:push({type = "ready", encounter = m.encounter})
        return true
    end
    if m.encounter ~= self.encounter then return refuse("wrong encounter") end
    if m.type == "join" then
        -- A Join crossing BG3's Finished is taken and dropped: refusing it would leave WoW's
        -- link waiting on it and block its Cleanup.
        if self.phase == "finished" or self.phase == "cleaning" or self.phase == "done" then return true end
        if self.phase ~= "active" then return refuse("join outside an active encounter") end
        for _, a in ipairs(m.actors) do
            if self.actors[a.id] then return refuse("actor " .. a.id .. " is already in the encounter") end
        end
        if not self.backend.join then return refuse("backend takes no joins") end
        local ok, why = self.backend.join(m)
        if not ok then return refuse(why or "backend refused join") end
        for _, a in ipairs(m.actors) do self.actors[a.id] = true end
        self.joins = (self.joins or 0) + 1
        return true
    end
    if m.type == "move" or m.type == "cast" then
        if self.phase ~= "active" or self.pending then return refuse("no action allowed now") end
        if m.turn ~= self.turn or m.actor ~= self.turn_actor then return refuse("not this actor's turn") end
        if self.used[m.action] then return refuse("duplicate action id") end
        if m.type == "cast" and not self.actors[m.target] then return refuse("unknown target") end
        local ok, why = self.backend[m.type](m)
        if not ok then return refuse(why or "backend refused action") end
        self.used[m.action], self.pending = true, m.action
        return true
    end
    if m.type == "end_turn" then
        if self.phase ~= "active" or self.pending or m.turn ~= self.turn or not self.turn_actor then
            return refuse("cannot end this turn now")
        end
        local ok, why = self.backend.end_turn(m)
        if not ok then return refuse(why or "backend refused end turn") end
        self.turn_actor = nil
        return true
    end
    if m.type == "cleanup" then
        if self.phase ~= "active" and self.phase ~= "finished" then return refuse("cleanup outside encounter") end
        local ok, why = self.backend.cleanup(m)
        if not ok then return refuse(why or "backend refused cleanup") end
        self.phase, self.pending, self.turn_actor = "cleaning", nil, nil
        return true
    end
    return refuse("unexpected message")
end

-- Backend events.
function A:ready()
    if self.phase ~= "starting" then return self:fail("ready outside setup") end
    self.phase = "active"
    return self:push({type = "ready", encounter = self.encounter})
end
function A:turn_started(actor)
    if self.phase ~= "active" or not self.actors[actor] or self.pending then return self:fail("unexpected turn event") end
    self.turn, self.turn_actor, self.acted_action = self.turn + 1, actor, nil
    return self:push({type = "turn", encounter = self.encounter, turn = self.turn, actor = actor})
end
function A:path(action, points)
    if self.pending ~= action then return self:fail("path for unknown action") end
    return self:push({type = "path", encounter = self.encounter, turn = self.turn, action = action,
        actor = self.turn_actor, points = points})
end
-- A combatant BG3 controls acted on its own turn (no WoW request is pending).
function A:acted(action, spell, target, points)
    if self.phase ~= "active" or self.pending or not self.turn_actor or self.turn_actor == "hero" then
        return self:fail("combatant action outside a BG3-controlled turn")
    end
    if self.used[action] then return self:fail("duplicate combatant action id") end
    if target ~= "none" and not self.actors[target] then return self:fail("combatant action at an unknown target") end
    self.used[action], self.acted_action = true, action
    return self:push({type = "acted", encounter = self.encounter, turn = self.turn, action = action,
        actor = self.turn_actor, spell = spell, target = target, points = points})
end
-- The combatants as BG3 has them (scaled stand-ins), after setup, after each join and when
-- hit points changed without an effect. `joins` counts the Join messages handled.
function A:roster(joins, actors)
    -- Once the fight is over a late roster (a join finishing) is moot.
    if self.phase == "finished" or self.phase == "cleaning" or self.phase == "done" then return true end
    if self.phase ~= "starting" and self.phase ~= "active" then return self:fail("roster outside an encounter") end
    for _, a in ipairs(actors) do
        if not self.actors[a.id] then return self:fail("roster names an unknown actor") end
    end
    return self:push({type = "roster", encounter = self.encounter, joins = joins, actors = actors})
end
function A:effect(action, target, hp, max_hp, animation)
    if self.pending ~= action and self.acted_action ~= action then return self:fail("effect for unknown action") end
    return self:push({type = "effect", encounter = self.encounter, turn = self.turn, action = action,
        target = target, hp = hp, max_hp = max_hp, animation = animation})
end
function A:rejected(action, reason, detail)
    if self.pending ~= action then return self:fail("rejection for unknown action") end
    if not self.Link.REJECTIONS[reason] then return self:fail("unknown rejection reason") end
    self.pending = nil
    return self:push({type = "rejected", encounter = self.encounter, turn = self.turn, action = action,
        reason = reason, detail = detail or ""})
end
function A:action_done(action)
    if self.pending ~= action then return self:fail("completion for unknown action") end
    self.pending = nil
    return true
end
function A:finished(outcome)
    if self.phase ~= "active" then return self:fail("finish outside encounter") end
    self.phase, self.pending, self.turn_actor = "finished", nil, nil
    return self:push({type = "finished", encounter = self.encounter, outcome = outcome})
end
function A:cleaned()
    if self.phase ~= "cleaning" then return self:fail("cleanup event outside cleanup") end
    self.phase = "done"
    return self:push({type = "cleaned", encounter = self.encounter})
end
-- Any internal inconsistency aborts the encounter. WoW must reconcile; nothing is replayed.
function A:fail(reason)
    if self.phase == "aborted" then return nil end
    self.backend.abort(reason)
    self.phase, self.pending = "aborted", nil
    self.queue = {}
    if self.encounter then
        self.queue[1] = {type = "abort", encounter = self.encounter, reason = string.sub(reason, 1, 256)}
    end
    return nil
end

return A
