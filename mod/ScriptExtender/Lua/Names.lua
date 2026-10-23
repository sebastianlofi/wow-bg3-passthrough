-- WoW names on BG3 stand-ins. Server context.
-- Osiris SetStoryDisplayName takes the text itself: BG3 makes a runtime "ResStr_..." handle for it
-- (the same text gives the same handle) and points the character's DisplayName at it; the client
-- shares that string table (one process), so its hover, turn order and target frames read the WoW
-- name. Harness: a Young Wolf stand-in's client DisplayName resolved to "Young Wolf".
-- (A made-up "h...g..." handle passed here showed as its own text: not a handle lookup.)
local M = {}

-- The stand-in shows this name. Returns "story" and the name BG3 resolves right after the call
-- (still the template's, e.g. "Wolf": the new name lands a moment later), or nil and why.
function M.Apply(uuid, name)
    if type(name) ~= "string" or name == "" or #name > 128 then return nil, "no name" end
    local ok, err = pcall(Osi.SetStoryDisplayName, uuid, name)
    if not ok then return nil, tostring(err) end
    local okr, got = pcall(function()
        return Ext.Loca.GetTranslatedString(Ext.Entity.Get(uuid).DisplayName.Name.Handle.Handle)
    end)
    return "story", okr and got or nil
end
return M
