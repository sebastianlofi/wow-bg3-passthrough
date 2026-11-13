-- Which WoW-BG3 this is, in the extender's line under BG3's main menu: "Script Extender v32 loaded (bg3le, WoW-BG3
-- 0.1.0 build 1a2b3c4d), built on ..." (the extender's own text, bg3le patch 0011, says "WoW-BG3 build" there).
--
-- Which release this is and which build of it: the release builder writes both into Version.lua, the build id from
-- everything the release holds, so two builds of one version read differently. It is written only in a BG3 that
-- loaded the mod's module (Module.lua), so a menu without it is a BG3 in which WoW-BG3 is off.
--
-- BG3 shows its copyright string there; the extender appends its line to that string once BG3's texts are loaded
-- (about a second after these scripts start, before the menu is built). Ours goes into it at the game state change
-- that follows, and again whenever BG3 has loaded its texts anew (back at the menu). An extender whose line has no
-- "-BG3 build" in it gets ours at the end of its line. Its "built on" date becomes this release's (Version.lua's Built).
local M = {}
M.HANDLE = "h5b6e4138g2cf0g4d67gb825gee416cf8c54f" -- BG3's copyright line (bg3le: kCheckHandle)
M.AFTER = "Script Extender"                         -- the extender's line is in the string

M.MARK = { "WoW-BG3 build", "WOW-BG3 build" }    -- the extender's words for this build (bg3le patch 0011; older wording)

function M.Line()
    local v = Ext.Require("Version.lua")
    return string.format("WoW-BG3 %s build %s", tostring(v.Version), tostring(v.Build))
end

-- When this release was built (Version.lua's Built, as "Mon DD YYYY HH:MM:SS"), or nil.
function M.Built()
    local b = Ext.Require("Version.lua").Built
    return type(b) == "string" and b ~= "" and b or nil
end

-- The string with our version in the extender's line, or nil when there is nothing to write (yet). BUILT, when given,
-- takes the place of the extender's own date: its reproducible build fixes that at the bg3le commit's.
function M.With(text, line, built)
    if type(text) ~= "string" or not text:find(M.AFTER, 1, true) or text:find(line, 1, true) then return nil end
    local done
    for _, mark in ipairs(M.MARK) do
        local a, b = text:find(mark, 1, true)
        if a then done = text:sub(1, a - 1) .. line .. text:sub(b + 1); break end
    end
    if not done then
        local nl = text:find("\r\n", (text:find(M.AFTER, 1, true)), true)
        done = nl and (text:sub(1, nl - 1) .. " " .. line .. text:sub(nl)) or (text .. " " .. line)
    end
    if built then
        local a = done:find(M.AFTER, 1, true)   -- the extender's line: its date before or after ours
        local s, e = done:find("built on [^\r\n]-%.", a)
        if s then done = done:sub(1, s - 1) .. "built on " .. built .. "." .. done:sub(e + 1) end
    end
    return done
end

function M.Apply()
    local line = M.Line()
    local text = M.With(Ext.Loca.GetTranslatedString(M.HANDLE), line, M.Built())
    if not text then return false end
    Ext.Loca.UpdateTranslatedString(M.HANDLE, text)
    Ext.Log.Print("WoW-BG3 in the menu line: " .. line)
    return true
end

function M.Start()
    pcall(M.Apply)
    Ext.Events.GameStateChanged:Subscribe(function() pcall(M.Apply) end)
end
return M
