-- Stubbed run of the real MusicRule.lua on both sides: lua tools/test_music_rule.lua mod/ScriptExtender/Lua
local dir = arg[1]
local files, rtpc, posted, gameState = {}, {RTPC_Volume_Music_Fight = 70}, {}, "Running"
local ticks = {}
local function json(t) local o = {} for k, v in pairs(t) do o[#o + 1] = k .. "\1" .. tostring(v) .. "\1" .. math.type(v and type(v)=="number" and v or 0) end return table.concat(o, "\2") end
local function parse(s)
  if s == "" or s == "{}" then return {} end
  local t = {}
  for kv in s:gmatch("[^\2]+") do
    local k, v, ty = kv:match("^(.-)\1(.-)\1(.*)$")
    if v == "true" then v = true elseif v == "false" then v = false elseif tonumber(v) then v = tonumber(v) end
    t[k] = v
  end
  return t
end
local client = true
local world = {host = "Tav_aaaaaaaa-0000-0000-0000-000000000001", combat = {}, trip = false, down = {}}
local A, G = "aaaaaaaa-0000-0000-0000-000000000001", "bbbbbbbb-0000-0000-0000-000000000002"
Ext = {
  IsClient = function() return client end,
  Utils = {Print = function(s) print("  " .. s) end, GetGameState = function() return gameState end},
  Events = {Tick = {Subscribe = function(_, f) ticks[#ticks + 1] = f end}},
  IO = {SaveFile = function(p, s) files[p] = s end, LoadFile = function(p) return files[p] end},
  Json = {Stringify = json, Parse = parse},
  Audio = {GetRTPC = function(_, n) return rtpc[n] end, SetRTPC = function(_, n, v) rtpc[n] = v return true end},
  Require = function(n)
    if n == "Camp.lua" then return {InTrip = function() return world.trip end} end
    if n == "Party.lua" then return {Combat = function() return next(world.combat) and "c1" or nil end} end
  end,
}
Osi = {
  GetHostCharacter = function() return world.host end,
  IsInCombat = function(u) return world.combat[u] and 1 or 0 end,
  HasActiveStatus = function(u) return world.down[u] and 1 or 0 end,
  MusicPlayGeneral = function(e) posted[#posted + 1] = e end,
}
local M = dofile(dir .. "/MusicRule.lua")
M.Start("run")
local function step(atCamp)
  for _ = 1, 5 do for _, f in ipairs(ticks) do f() end end
  return M.Flags("run", atCamp)
end
local fails = 0
local function want(name, got, flags, vol)
  local ok = got == flags and rtpc.RTPC_Volume_Music_Fight == vol
  print((ok and "ok   " or "FAIL ") .. name .. " flags " .. got .. " vol " .. rtpc.RTPC_Volume_Music_Fight)
  if not ok then fails = fails + 1 end
end
want("nobody fights: WoW's music", step(), 0, 70)
world.combat[G] = true
want("far companion fights: WoW's, BG3's shut", step(), 8, 0)
world.host = "Gale_" .. G
want("switch to the fighter: BG3's", step(), 4, 70)
world.host = "Tav_" .. A
want("switch away: WoW's, shut", step(), 8, 0)
print(#posted == 2 and "ok   Music_Off after each switch" or "FAIL posted " .. #posted)
if #posted ~= 2 then fails = fails + 1 end
for i = #posted, 1, -1 do posted[i] = nil end
world.combat[G] = nil
want("far fight over: stays shut (its stinger)", step(), 8, 0)
world.combat[A] = true
want("own fight: BG3's", step(), 4, 70)
world.combat[A] = nil
want("own fight over: open for its stinger", step(), 0, 70)
world.combat[G], world.down[A] = true, true
want("downed in a running fight: still in it", step(), 4, 70)
world.combat[G], world.down[A] = nil, nil
world.trip = true
want("at camp", step(true), 0, 70)
assert(#posted == 0)
world.host = "Gale_" .. G
want("a switch at camp: camp's music stays", step(true), 0, 70)
assert(#posted == 0)
world.host = "Tav_" .. A
step(true); assert(#posted == 0)
world.trip = false
want("left camp", step(), 0, 70)
print((#posted == 1 and posted[1] == "Music_Off") and "ok   Music_Off once on leaving" or "FAIL posted " .. table.concat(posted, ","))
if not (#posted == 1) then fails = fails + 1 end
step(); assert(#posted == 1)
world.combat[G] = true; want("far fight, closed", step(), 8, 0)
rtpc.RTPC_Volume_Music_Fight = 40 -- BG3 applied the slider
for _ = 1, 10 do step() end
want("BG3 set the volume again: closed again", step(), 8, 0)
world.host = "Gale_" .. G; want("reopens at the new slider", step(), 4, 40)
world.host = "Tav_" .. A; want("closed", step(), 8, 0)
M.Release(); want("world mode stop restores", 0, 0, 40)
print(fails == 0 and "ALL OK" or (fails .. " FAILED"))
