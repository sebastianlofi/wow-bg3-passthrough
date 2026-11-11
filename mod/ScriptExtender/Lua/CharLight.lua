-- CharLight.lua: BG3's own character light follows WoW's ambient level (client).
--
-- BG3 lights every character with a small light of its own that follows the head (the CharacterLight component, a
-- setup of up to three lights), so a character stays readable wherever it stands. That light does not know WoW's time
-- of day: by WoW's night it kept the party twice as bright as WoW shows a unit there (measured against WoW's own
-- drawing of the hero), and with it off the party fell to half. So its strength follows WoW's ambient colour (link/
-- wowsky.txt, benilla patch 0125): as BG3 set it at the ambient the game's light was matched at (Northshire's noon),
-- less in proportion under a darker one, never more. Outside WoW's world (camp) it is BG3's again.
local M = {}

M.REF = 0.2157      -- linear luminance of the ambient the light was matched under (0.4078 0.5098 0.6039)
M.EVERY = 30        -- ticks

local function lin(c) return math.max(0, c) ^ 2.2 end

-- The share of BG3's own strength for one wowsky.txt line, or nil. Pure.
function M.Share(line)
    local v = {}
    for x in tostring(line):gmatch("%S+") do v[#v + 1] = tonumber(x) or x end
    if v[1] ~= "WSK1" or #v < 10 or type(v[8]) ~= "number" or type(v[9]) ~= "number" or type(v[10]) ~= "number" then return nil end
    -- The ambient WoW lights the hero's figure with when the file has it (benilla patch 0132: a room's own indoors).
    local a = (type(v[16]) == "number" and type(v[17]) == "number" and type(v[18]) == "number") and {v[16], v[17], v[18]} or {v[8], v[9], v[10]}
    local luma = 0.2126 * lin(a[1]) + 0.7152 * lin(a[2]) + 0.0722 * lin(a[3])
    return math.max(0, math.min(1, luma / M.REF))
end

local own = {}      -- "<entity> <setup> <index>" -> the Amount BG3 gave that light
local last = nil

local function apply(share)
    local n = 0
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("CharacterLight")) do
        local cl = e.CharacterLight
        for setup, lists in pairs(cl and cl.Lights or {}) do
            for _, list in pairs(lists) do
                for i, l in pairs(list) do
                    local d = l.TimelineLight and l.TimelineLight.LightDesc
                    if d then
                        local key = tostring(e) .. " " .. tostring(setup) .. " " .. tostring(i)
                        if own[key] == nil then own[key] = d.Amount end
                        local want = own[key] * share
                        if math.abs(d.Amount - want) > 1e-4 then d.Amount = want; n = n + 1 end
                    end
                end
            end
        end
    end
    return n
end

function M.Start(runId)
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % M.EVERY ~= 7 or Ext.Utils.GetGameState() ~= "Running" then return end
        local ok, err = pcall(function()
            local share = 1
            local okW, w = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/link/world.json") or "")
            if okW and type(w) == "table" and w.Active == true then
                share = M.Share(Ext.IO.LoadFile("wow-bg3/link/wowsky.txt")) or 1
            end
            local n = apply(share)
            if n > 0 and (not last or math.abs(share - last) > 0.02) then
                Ext.Log.Print(string.format("WOW-BG3 charlight: BG3's character light at %.2f of its own (%d lights set)", share, n))
                last = share
            end
        end)
        if not ok then Ext.Log.Print("WOW-BG3 charlight: " .. tostring(err)) end
    end)
    Ext.Log.Print("WOW-BG3 charlight: started " .. tostring(runId))
end

return M
