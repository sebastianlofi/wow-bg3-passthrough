-- Only in a BG3 that loaded WOW-BG3's module (Module.lua).
Ext.Require("Module.lua").Start("server", function()
    Ext.Require("Probe.lua").Start("server")
end)
