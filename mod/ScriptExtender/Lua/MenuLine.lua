-- WOW-BG3's own line under BG3's main menu: "WOW-BG3 0.1.0 (build 1a2b3c4d)", below the Script Extender's.
--
-- Which release this is and which build of it: the release builder writes both into Version.lua, the build id from
-- everything the release holds, so two builds of one version read differently. The line is written only in a BG3
-- that loaded the mod's module (Module.lua), so a menu without it is a BG3 in which WOW-BG3 is off.
--
-- BG3 shows its copyright string there; the extender appends its line to that string once BG3's texts are loaded
-- (about a second after these scripts start, before the menu is built). Ours goes after the extender's, at the
-- game state change that follows, and again whenever BG3 has loaded its texts anew (back at the menu).
local M = {}
M.HANDLE = "h5b6e4138g2cf0g4d67gb825gee416cf8c54f" -- BG3's copyright line (bg3le: kCheckHandle)
M.AFTER = "Script Extender"                         -- the extender's line is in the string

function M.Line()
    local v = Ext.Require("Version.lua")
    return string.format("WOW-BG3 %s (build %s)", tostring(v.Version), tostring(v.Build))
end

-- The string with our line after the extender's, or nil when there is nothing to write (yet).
function M.With(text, line)
    if type(text) ~= "string" or not text:find(M.AFTER, 1, true) or text:find(line, 1, true) then return nil end
    return text .. "\r\n" .. line
end

function M.Apply()
    local line = M.Line()
    local text = M.With(Ext.Loca.GetTranslatedString(M.HANDLE), line)
    if not text then return false end
    Ext.Loca.UpdateTranslatedString(M.HANDLE, text)
    Ext.Log.Print("WOW-BG3 menu line: " .. line)
    return true
end

function M.Start()
    pcall(M.Apply)
    Ext.Events.GameStateChanged:Subscribe(function() pcall(M.Apply) end)
end
return M
