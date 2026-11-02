Ext.Require("Probe.lua").Start("client")

-- The plugin's rigid orbit reads BG3's camera wishes from here.
local okCam, errCam = pcall(function() Ext.Require("CameraPose.lua").Start() end)
if not okCam then Ext.Log.Print("WOW-BG3 camera pose unavailable: " .. tostring(errCam)) end

