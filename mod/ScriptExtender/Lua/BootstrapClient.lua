Ext.Require("Probe.lua").Start("client")

-- The plugin's rigid orbit reads BG3's camera wishes from here.
local okCam, errCam = pcall(function() Ext.Require("CameraPose.lua").Start() end)
if not okCam then Ext.Log.Print("WOW-BG3 camera pose unavailable: " .. tostring(errCam)) end

-- A log line for each BG3 window or notice that shows, with its words.
local okUi, errUi = pcall(function() Ext.Require("UiWatch.lua").Start() end)
if not okUi then Ext.Log.Print("WOW-BG3 ui watch unavailable: " .. tostring(errUi)) end
