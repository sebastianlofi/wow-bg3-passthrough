-- Only in a BG3 that loaded WOW-BG3's module (Module.lua).
Ext.Require("Module.lua").Start("client", function()
    -- Which WOW-BG3 this is, under the main menu.
    local okLine, errLine = pcall(function() Ext.Require("MenuLine.lua").Start() end)
    if not okLine then Ext.Log.Print("WOW-BG3 menu line: " .. tostring(errLine)) end

    Ext.Require("Probe.lua").Start("client")

    -- The plugin's rigid orbit reads BG3's camera wishes from here.
    local okCam, errCam = pcall(function() Ext.Require("CameraPose.lua").Start() end)
    if not okCam then Ext.Log.Print("WOW-BG3 camera pose unavailable: " .. tostring(errCam)) end
end)
