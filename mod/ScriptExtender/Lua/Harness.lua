-- Harness switch: the test and research channels run only in a test setup. BG3 started with
-- WOW_BG3_HARNESS=1 in its environment turns them on (release mode's tests pass it through
-- RELEASE_BG3_ENV); a player's game never has it, so a file dropped in the mod's data folder can run
-- no Lua and no Osiris call. Gated: the research files of Interact.lua, Inventory.lua, NewGame.lua,
-- Portraits.lua, Progression.lua and WorldMode.lua, StandIns.lua's Osi action and ActionProbe.lua's Research
-- action.
local M = {}
local v = Ext._Internal and Ext._Internal.Getenv and Ext._Internal.Getenv("WOW_BG3_HARNESS") or nil
M.On = v == "1"
return M
