-- Diagnostic, off unless WOW_BG3_LUAPROF=1: how long the mod's own Lua runs on BG3's thread for this context, by
-- handler. Every Ext.Events subscription and Osiris listener made after Install() is wrapped with a microsecond timer;
-- every 10 s the log gets the total (ms of mod Lua per second of game) and the twelve costliest handlers.
-- Why: on an RTX 4070 Ti PC BG3's ServerWorker thread ran at 98% in world mode and frames came late every ~1.3 s; this
-- tells how much of that thread is the mod's and whose.
local M = {}

-- A part of a handler, timed under its own name while the profiler runs (a no-op otherwise).
local section = nil
function M.Section(name, fn, ...)
    if section then return section(name, fn, ...) end
    return fn(...)
end

function M.Install(context)
    local now = Ext.Timer.MicrosecTime
    local acc, calls, peak = {}, {}, {}
    section = function(name, fn, ...)
        local t0 = now()
        local r = table.pack(fn(...))
        local d = now() - t0
        local key = "section " .. name
        acc[key] = (acc[key] or 0) + d
        calls[key] = (calls[key] or 0) + 1
        if d > (peak[key] or 0) then peak[key] = d end
        return table.unpack(r, 1, r.n)
    end
    local function wrap(tag, fn)
        if type(fn) ~= "function" then return fn end
        local info = debug and debug.getinfo and debug.getinfo(fn, "S") or nil
        local src = info and tostring(info.short_src):match("([^/\\]+)$") or "?"
        local key = string.format("%s %s:%s", tag, src, info and info.linedefined or 0)
        return function(...)
            local t0 = now()
            local r = table.pack(fn(...))
            local d = now() - t0
            acc[key] = (acc[key] or 0) + d
            calls[key] = (calls[key] or 0) + 1
            if d > (peak[key] or 0) then peak[key] = d end
            return table.unpack(r, 1, r.n)
        end
    end
    -- Ext.Events hands out each event's object through its metatable: give back one whose Subscribe wraps.
    local mt = getmetatable(Ext.Events)
    local get = mt.__index
    local proxies = {}
    mt.__index = function(t, name)
        local ev = get(t, name)
        if type(ev) ~= "table" and type(ev) ~= "userdata" then return ev end
        local p = proxies[name]
        if not p then
            p = setmetatable({
                Subscribe = function(_, fn, opts) return ev:Subscribe(wrap("event " .. tostring(name), fn), opts) end,
                Unsubscribe = function(_, id) return ev:Unsubscribe(id) end,
            }, {__index = ev})
            proxies[name] = p
        end
        return p
    end
    if Ext.Osiris and Ext.Osiris.RegisterListener then
        local register = Ext.Osiris.RegisterListener
        Ext.Osiris.RegisterListener = function(name, arity, when, fn) return register(name, arity, when, wrap("osiris " .. tostring(name), fn)) end
    end
    local last = now()
    get(Ext.Events, "Tick"):Subscribe(function()
        local t = now()
        if t - last < 10e6 then return end
        local span = (t - last) / 1e6
        last = t
        local rows, total = {}, 0
        for k, v in pairs(acc) do rows[#rows + 1] = k; total = total + v end
        table.sort(rows, function(a, b) return acc[a] > acc[b] end)
        local out = {string.format("luaprof %s: %.1f ms of mod Lua per s of game (%d handlers)", context, total / 1e3 / span, #rows)}
        for i = 1, math.min(20, #rows) do
            local k = rows[i]
            out[#out + 1] = string.format("  %7.2f ms/s %6d calls, max %6.2f ms  %s", acc[k] / 1e3 / span, calls[k], peak[k] / 1e3, k)
        end
        Ext.Log.Print(table.concat(out, "\n"))
        acc, calls, peak = {}, {}, {}
    end)
    Ext.Log.Print("luaprof " .. context .. ": installed")
end

return M
