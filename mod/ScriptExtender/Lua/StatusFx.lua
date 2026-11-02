-- Which statuses with a visual effect are on whom, for the capture layer (link/fxhold.txt; the comment
-- "a status's mesh" in plugins/capture/wowbg3_fxcap.inc.c). A status's effect can hold an opaque mesh (NET's
-- net), which the void only lets through young; the layer can't tell it from the level, the game can.
--
-- File: "HOLD <n>", then per status "<status> <character> <x> <y> <z>", the place where it began.
-- Logs "WOW-BG3 statusfx:" in bg3le.log, against the layer's "fxhold:" lines in layer.log.
local M = {}
local on, order, dirty = {}, {}, true
local MAX = 64

local function hasEffect(status)
    local ok, stat = pcall(Ext.Stats.Get, status)
    if not ok or not stat then return nil end
    local okE, effect = pcall(function() return stat.StatusEffect end)
    return okE and type(effect) == "string" and effect ~= "" and effect or nil
end

local function write()
    local out = {"HOLD " .. #order}
    for _, k in ipairs(order) do
        local e = on[k]
        out[#out + 1] = string.format("%s %s %.3f %.3f %.3f", e.Status, e.Who, e.X, e.Y, e.Z)
    end
    Ext.IO.SaveFile("wow-bg3/link/fxhold.txt", table.concat(out, "\n") .. "\n")
end

local function drop(k)
    if not on[k] then return end
    on[k] = nil
    for i, o in ipairs(order) do if o == k then table.remove(order, i); break end end
    dirty = true
end

function M.Start()
    Ext.Osiris.RegisterListener("StatusApplied", 4, "after", function(who, status)
        local k = tostring(who) .. "|" .. tostring(status)
        if on[k] then return end
        local effect = hasEffect(status)
        if not effect then return end
        local ok, x, y, z = pcall(Osi.GetPosition, who)
        if not ok or not x then return end
        if #order >= MAX then drop(order[1]) end
        on[k] = {Status = tostring(status), Who = tostring(who), X = x, Y = y, Z = z}
        order[#order + 1] = k
        dirty = true
        Ext.Utils.Print(string.format("WOW-BG3 statusfx: %s (effect %s) on %s at %.1f %.1f %.1f", tostring(status), effect, tostring(who), x, y, z))
    end)
    Ext.Osiris.RegisterListener("StatusRemoved", 4, "after", function(who, status)
        local k = tostring(who) .. "|" .. tostring(status)
        if on[k] then Ext.Utils.Print("WOW-BG3 statusfx: " .. k .. " over") end
        drop(k)
    end)
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        -- A character removed with its statuses on says nothing.
        if ticks % 30 == 0 then
            for i = #order, 1, -1 do
                local e = on[order[i]]
                local ok, has = pcall(Osi.HasActiveStatus, e.Who, e.Status)
                if not ok or has ~= 1 then drop(order[i]) end
            end
        end
        if dirty then
            dirty = false
            pcall(write)
        end
    end)
end

return M
