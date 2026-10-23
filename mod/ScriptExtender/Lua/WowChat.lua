-- World mode: WoW's chat typed from the BG3 window. BG3 has the keyboard, so a plain Enter
-- (or /) starts a typing session: every key from that one to the closing Enter or Escape goes
-- to benilla through link/bg3keys.bin instead of BG3, and benilla plays it as its own keyboard
-- (WoW's chat edit box opens on Enter or / as usual; benilla patch 0008 reads the file). The
-- key-up of a forwarded key-down is forwarded too, even after the session ends.
--   bg3keys.bin (little endian): "BGK1", newest seq u32, count u32 (at most 32), then count
--   records {seq u32, SDL scancode u32, SDL modifier bits u32, flags u32}, newest seq again.
--   Flags: 1 key down, 2 repeat, 4 the session's first key (Enter or /), 8 its last (Enter or
--   Escape). A reader applies records with seq above the last it applied, oldest first.
--   Scancodes are SDL2's (SDL_SCANCODE_*). Typing starts only once benilla has written
--   link/bg3keys-reader.txt (it reads the file), so an older benilla never swallows BG3's keys.
local M = {}
local RING = 32
local OPEN = {RETURN = true, KP_ENTER = true, SLASH = true}
local CLOSE = {RETURN = true, KP_ENTER = true, ESCAPE = true}

local function enumNumbers(enum)
    local t, i = {}, 0
    while true do
        local label, value = Ext._Internal.EnumValueAt(enum, i)
        if label == nil then break end
        t[label] = value
        i = i + 1
    end
    return t
end

-- Ctrl, Alt or GUI held: a shortcut, never the start of typing. Shift too for / (that is ?).
local function chorded(e, shiftToo)
    for _, label in ipairs(e.Modifiers or {}) do
        local l = string.lower(tostring(label))
        if l:find("ctrl") or l:find("alt") or l:find("gui") or (shiftToo and l:find("shift")) then return true end
    end
    return false
end

function M.Start(runId)
    local world = Ext.Require("WorldMode.lua")
    local scan, mods
    local typing, seq, ring, held = false, 0, {}, {}

    local function publish()
        local parts = {}
        for i, r in ipairs(ring) do parts[i] = string.pack("<I4I4I4I4", r[1], r[2], r[3], r[4]) end
        Ext.IO.SaveFile("wow-bg3/link/bg3keys.bin",
            string.pack("<c4I4I4", "BGK1", seq, #ring) .. table.concat(parts) .. string.pack("<I4", seq))
    end
    -- benilla takes whatever the file holds when it first reads it as history, and a seq that goes
    -- backwards as a BG3 restart, so this run continues the file's seq and writes the file at once
    -- (before any typing), and a first session is never mistaken for history.
    local old = Ext.IO.LoadFile("wow-bg3/link/bg3keys.bin")
    if old and #old >= 12 and old:sub(1, 4) == "BGK1" then
        seq = string.unpack("<I4", old, 5)
    end
    publish()

    local function forward(e, extra)
        scan = scan or enumNumbers("SDLScanCode")
        mods = mods or enumNumbers("SDLKeyModifier")
        local code = scan[e.Key]
        if code == nil then return false end
        local bits = 0
        for _, label in ipairs(e.Modifiers or {}) do bits = bits | (mods[label] or 0) end
        seq = seq + 1
        ring[#ring + 1] = {seq, code, bits, (e.Event == "KeyDown" and 1 or 0) | (e.Repeat and 2 or 0) | extra}
        if #ring > RING then table.remove(ring, 1) end
        publish()
        return true
    end

    Ext.Events.KeyInput:Subscribe(function(e)
        if e.Event ~= "KeyDown" then
            -- Only the key-ups of key-downs that went to WoW; BG3 keeps every other release.
            if held[e.Key] then
                held[e.Key] = nil
                forward(e, 0)
                e:PreventAction()
            end
            return
        end
        if not typing then
            if e.Repeat or not OPEN[e.Key] or not world.IsActive() then return end
            if not Ext.IO.LoadFile("wow-bg3/link/bg3keys-reader.txt") then return end
            if chorded(e, e.Key == "SLASH") then return end
            if not forward(e, 4) then return end
            typing = true
        else
            local last = not e.Repeat and CLOSE[e.Key]
            forward(e, last and 8 or 0)
            if last then typing = false end
        end
        held[e.Key] = true
        e:PreventAction()
    end)
    -- A typing session never outlives world mode.
    Ext.Events.GameStateChanged:Subscribe(function() typing = false end)
end

return M
