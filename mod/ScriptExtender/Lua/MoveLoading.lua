-- MoveLoading.lua: a move's cover is BG3's own screen fade (Osiris ScreenFadeTo, as its camp and prison
-- scripts use it). BG3 draws it in its UI (ScreenFade.xaml, in every running game), which the capture layer
-- composites over WoW's picture, so it fades over the world, stays black while the party is away, and fades
-- out at the new place. Its wheel is that widget's too, shown when BG3's ScreenFadeHasSpin says so.
-- link/wowmove.flag: written by tools/ when a move starts, a new text each time; gone when it ends (world_bridge.py
-- removes it once benilla's player stands at the new place).
--
-- A fade from ScreenFadeTo lasts about 10 s once it is black, then BG3 lifts it by itself: in the 0.1.3013.58
-- test (ten moves of 16.5 to 39.5 s) the picture came back 10.3 to 10.7 s after each "to black", whatever the
-- move was doing then (its Stop came 2 to 12 s in), and nothing of ours cleared it. BG3's own covers are
-- short (fade, teleport, clear). A longer move asks for a new fade before the last one lapses (RENEW_MS): it
-- starts black over a black screen, so nothing shows; at the end every one still standing is cleared.
-- The renewals run on the server and BG3's client lifts a fade on its own clock: the lit frames of the
-- 0.1.3013.59 test (17 to 25 s into the fallback moves) were no renewal showing through but the server running no
-- tick for 8 to 13 s after a re-origin's teleport (no renewal, no Osiris event), so the last fade ran out
-- (BG3 collapsed it 10.2 s after it was asked for) up to 3.6 s before the next renewal came. The freezes followed
-- blocked storeys registered at the hero's floor; the plugin's Place now registers a storey without a floor in no
-- patch (wowbg3_plugin.cpp pool_place).
-- ScreenFadeTo's arguments are (Character, Seconds, FadeStyle, FadeID) (the names BG3 registers for Osiris):
-- the third is the style (0 black, 1 bright), not the wheel; ScreenFadeHasSpin is not one of its arguments.
--
-- A far switch (the multi-continent party, phase 2) takes the same cover. When the player takes control
-- of a party member over FAR_SWITCH_M from the one controlled before, both in world mode's void, BG3 makes its
-- own short fade and its camera jumps there; benilla's world round that place streams only once the server's
-- Far Sight is out on that member's bot, so after BG3's fade the picture was void (0.1.3013.58). The fade
-- starts at once here, over BG3's own, and holds (renewed as for a move) until benilla says the world round
-- its eye is loaded (camera.bin flag bit 2, benilla 0112) with the eye at the new member (link/world.json's
-- mapping, EYE_AT_YD), or for SWITCH_MAX_MS at most. A move's flag and a switch share one cover.
local M = {}

local CHECK_EVERY = 5     -- ticks
local FADE_S, RENEW_S, CLEAR_S = 0.5, 0.1, 1.0
local RENEW_MS = 5000     -- a new fade this often while the move lasts (BG3 lifts one ~10 s after it is black)
local OLD_MS = 120000     -- a bridge killed mid-move does not leave the screen black
local ID = "WOWBG3_Move"
local FLAG = "wow-bg3/link/wowmove.flag"
local FAR_SWITCH_M = 100      -- a switch this far (BG3 m, flat) takes the cover
local SWITCH_FADE_S = 0.2     -- over BG3's own switch fade, which is already going
local SWITCH_MAX_MS = 20000   -- a switch's cover at most
local EYE_AT_YD = 60          -- benilla's eye this near the new member (flat, WoW yd) is at it
local VOID_X = -1000          -- world mode's void lies west of this (Party.lua)
local YD = 0.9144
local CAMERA = "wow-bg3/link/camera.bin"
local WORLD = "wow-bg3/link/world.json"

-- The controlled character and its BG3 position, or nil.
local function hostAt()
    local ok, host = pcall(Osi.GetHostCharacter)
    if not ok or not host then return nil end
    local okp, x, y, z = pcall(Osi.GetPosition, host)
    if not okp or not x then return host, nil end
    return host, {x, y, z}
end

-- Whether benilla's world is loaded round its eye and the eye stands at BG3 point `p`: camera.bin (WBC1, seq,
-- eye xyz at 9, seq again at 65, flags at 89) through world.json's mapping (WowOrigin, Centre: WoW x = origin x -
-- (bz - centre z) / YD, WoW y = origin y + (bx - centre x) / YD). nil when there is no mapping (world mode off).
local function eyeLoadedAt(p)
    local okw, w = pcall(function() return Ext.Json.Parse(Ext.IO.LoadFile(WORLD) or "") end)
    if not okw or type(w) ~= "table" or w.Active ~= true or type(w.WowOrigin) ~= "table" or type(w.Centre) ~= "table" then
        return nil
    end
    local raw = Ext.IO.LoadFile(CAMERA)
    if not raw or #raw < 96 or raw:sub(1, 4) ~= "WBC1" then return false end
    local seq, ex, ey = string.unpack("<I4ff", raw, 5)
    local seq2, flags = string.unpack("<I4", raw, 65), string.unpack("<I4", raw, 89)
    if seq ~= seq2 or flags & 4 == 0 then return false end
    local wx = w.WowOrigin[1] - (p[3] - w.Centre[3]) / YD
    local wy = w.WowOrigin[2] + (p[1] - w.Centre[1]) / YD
    return (ex - wx) ^ 2 + (ey - wy) ^ 2 <= EYE_AT_YD * EYE_AT_YD
end

function M.StartServer()
    local ticks, seen, on, since, first = 0, nil, false, 0, true
    local moving = false                  -- a move's flag is up
    local lastHost, lastPos, switch = nil, nil, nil   -- switch: {Host, At} while a far switch is covered
    local live, n, renewed = {}, 0, 0  -- fades asked for in this move: {Id, At}
    local function fadeTo(seconds, now)
        n = n + 1
        local id = ID .. "_" .. n
        local ok, err = pcall(Osi.ScreenFadeTo, Osi.GetHostCharacter(), seconds, 0, id)
        if ok then live[#live + 1] = {Id = id, At = now} end
        renewed = now
        Ext.Log.Print("WOW-BG3 move fade: " .. id .. " to black in " .. seconds .. " s" .. (ok and "" or " failed: " .. tostring(err)))
    end
    local function clearAll()
        local host = Osi.GetHostCharacter()
        local bad = {}
        for _, f in ipairs(live) do
            local ok, err = pcall(Osi.ClearScreenFade, host, CLEAR_S, f.Id, 0)
            if not ok then bad[#bad + 1] = f.Id .. ": " .. tostring(err) end
        end
        Ext.Log.Print("WOW-BG3 move fade: cleared " .. #live .. (#bad > 0 and " (failed: " .. table.concat(bad, "; ") .. ")" or ""))
        live = {}
    end
    -- What BG3 says of our fades: when it reports one done (black) and when one is cleared, with its age. A
    -- clear while the move is on is BG3's own lifting it (the ~10 s above).
    local function heard(event)
        return function(user, fadeId)
            local s = tostring(fadeId)
            if s:sub(1, #ID) ~= ID then return end
            local age = "?"
            for _, f in ipairs(live) do
                if f.Id == s then age = string.format("%.1f", (Ext.Utils.MonotonicTime() - f.At) / 1000) end
            end
            Ext.Log.Print(string.format("WOW-BG3 move fade: BG3 %s %s at %s s (move %s)", event, s, age, on and "on" or "off"))
        end
    end
    pcall(Ext.Osiris.RegisterListener, "ScreenFadeDone", 2, "after", heard("done"))
    pcall(Ext.Osiris.RegisterListener, "ScreenFadeCleared", 2, "after", heard("cleared"))
    -- One cover for a move and a far switch: black while either lasts, cleared when both are over.
    local function cover(now, seconds)
        local want = moving or switch ~= nil
        if want == on then return end
        on = want
        if on then fadeTo(seconds, now) else clearAll() end
    end
    -- A far switch: the controlled character changed to one over FAR_SWITCH_M away, both in the void.
    local function watchSwitch(now)
        local host, pos = hostAt()
        if host ~= lastHost and lastHost ~= nil then
            local far = pos and lastPos and pos[1] < VOID_X and lastPos[1] < VOID_X
                and (pos[1] - lastPos[1]) ^ 2 + (pos[3] - lastPos[3]) ^ 2 > FAR_SWITCH_M * FAR_SWITCH_M
            -- false: world mode runs and the eye is not loaded there yet (nil: no world mode; true: it already is).
            if far and eyeLoadedAt(pos) == false then
                switch = {Host = host, At = now}
                Ext.Log.Print(string.format("WOW-BG3 move fade: far switch to %s, %.0f m: covered until WoW's world is there",
                    tostring(host), math.sqrt((pos[1] - lastPos[1]) ^ 2 + (pos[3] - lastPos[3]) ^ 2)))
            elseif switch then
                Ext.Log.Print("WOW-BG3 move fade: switched again, near: the switch's cover ends")
                switch = nil
            end
        elseif switch and pos then
            local loaded = eyeLoadedAt(pos)
            if loaded then
                Ext.Log.Print(string.format("WOW-BG3 move fade: WoW's world is loaded at %s after %.1f s", tostring(host), (now - switch.At) / 1000))
                switch = nil
            elseif loaded == nil then
                Ext.Log.Print("WOW-BG3 move fade: world mode stopped during a switch: its cover ends")
                switch = nil
            elseif now - switch.At > SWITCH_MAX_MS then
                Ext.Log.Print("WOW-BG3 move fade: no world from benilla at " .. tostring(host) .. " in " .. SWITCH_MAX_MS / 1000 .. " s: the switch's cover ends")
                switch = nil
            end
        end
        lastHost, lastPos = host, pos
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % CHECK_EVERY ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local now = Ext.Utils.MonotonicTime()
        local flag = Ext.IO.LoadFile(FLAG) or ""
        if first then
            first = false
            seen = flag -- from before this load: no move, but a save or reload may have kept its fade
            if flag ~= "" then
                live = {{Id = ID .. "_1", At = now}}
                clearAll()
            end
            lastHost, lastPos = hostAt()
            return
        end
        watchSwitch(now)
        if flag ~= seen then
            seen = flag
            since = now
            moving = flag ~= ""
        elseif moving and now - since > OLD_MS then
            Ext.Log.Print("WOW-BG3 move fade: no end of the move in " .. OLD_MS / 1000 .. " s")
            moving = false
        end
        local was = on
        cover(now, moving and FADE_S or SWITCH_FADE_S)
        if on and was and now - renewed >= RENEW_MS then
            fadeTo(RENEW_S, now)
        end
    end)
end

-- What the in-game check needs: BG3's own values for the fade while a move is on, each time they change
-- (visibility, opacity, wheel), with the time since the move began.
function M.StartClient()
    local ticks, seen, at, last = 0, Ext.IO.LoadFile(FLAG) or "", nil, nil
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 30 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        local flag = Ext.IO.LoadFile(FLAG) or ""
        if flag ~= seen then
            if (flag ~= "") ~= (seen ~= "") then at, last = flag ~= "" and Ext.Utils.MonotonicTime() or nil, nil end
            seen = flag
        end
        if not at then return end
        local state = "no ScreenFade widget"
        local function walk(node, depth)
            if not node or depth > 5 then return end
            if node.Type == "ls.UIWidget" then
                if node.Name == "ScreenFade" then
                    local d = node.DataContext.CurrentPlayer.UIData
                    state = string.format("visibility %s, opacity %.2f, spin %s", tostring(d.ScreenFadeVisibility),
                        tonumber(d.ScreenFadeOpacity) or -1, tostring(d.ScreenFadeHasSpin))
                end
                return
            end
            for i = 1, math.min(node.VisualChildrenCount or 0, 128) do walk(node:VisualChild(i), depth + 1) end
        end
        local ok, err = pcall(walk, Ext.UI.GetRoot(), 0)
        if not ok then state = "unreadable: " .. tostring(err) end
        if state ~= last then
            last = state
            Ext.Log.Print(string.format("WOW-BG3 move fade: BG3 says %s, %.1f s into the move",
                state, (Ext.Utils.MonotonicTime() - at) / 1000))
        end
    end)
end

return M
