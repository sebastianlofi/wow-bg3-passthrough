-- Compatibility reporting plus explicitly authorized first-run licence acceptance.
-- Probe itself mutates no actors. ActionProbe (client) may press allowlisted normal HotBar
-- turn controls for the disposable save only; ActionMonitor and EncounterSurvey (server)
-- are read-only. StandIns (server) performs approved Osiris stand-in setup only.
-- Arena (both) stamps WoW arena tiles into the AI grid through patch 0003 and restores them.
local Probe = {}
function Probe.Start(context)
    local report = {
        Schema = 1, RunId = Ext._Internal.Getenv("WOW_BG3_RUN_ID"), Context = context, ExtenderVersion = Ext.Utils.Version(),
        FileRoundTrip = false, Stage = "bootstrap",
        Candidates = {
            Entity = type(Ext.Entity.Get), Osiris = type(Ext.Osiris.RegisterListener),
            Path = type(Ext.Level.GetPathById), Json = type(Ext.Json.Stringify)
        }
    }
    local prefix = "wow-bg3/m3-" .. context
    local value = "WOW-BG3 private IO probe " .. context
    Ext.IO.SaveFile(prefix .. "-roundtrip.txt", value)
    report.FileRoundTrip = Ext.IO.LoadFile(prefix .. "-roundtrip.txt") == value
    local function save(stage)
        report.Stage = stage
        report.GameState = tostring(Ext.Utils.GetGameState())
        -- An empty audit at the menu is not compatibility proof.
        if context == "server" and report.GameState == "Running" then
            local ok, audit = pcall(Ext._Internal.SizeAudit)
            report.SizeAuditOK = ok
            report.SizeAudit = ok and audit or tostring(audit)
        end
        Ext.IO.SaveFile(prefix .. ".json", Ext.Json.Stringify(report))
        Ext.Log.Print("WOW-BG3 probe: " .. context .. " " .. stage .. " IO=" .. tostring(report.FileRoundTrip))
    end
    local menuTicks, captures, acceptedEula = 0, 0, false
    local function captureMenu()
        if context ~= "client" or captures >= 10 or Ext.Utils.GetGameState() ~= "Menu" then return end
        menuTicks = menuTicks + 1
        if menuTicks % 120 ~= 0 then return end
        captures = captures + 1
        local rows, seen, named = {}, {}, {}
        local function field(node, key)
            local ok, value = pcall(function() return node[key] end)
            if ok and (type(value) == "string" or type(value) == "number" or type(value) == "boolean") then return value end
        end
        local function visit(node, depth, parent)
            if not node or seen[tostring(node)] or depth > 24 or #rows >= 512 then return end
            seen[tostring(node)] = true
            local row = {Index = #rows + 1, Parent = parent, Type = node.Type,
                Name = field(node, "Name"), Text = field(node, "Text"),
                Enabled = field(node, "IsEnabled"), Visible = field(node, "Visibility")}
            local ok, dc = pcall(function() return node.DataContext end)
            if ok and dc then row.DataContextType = tostring(dc) end
            rows[#rows + 1] = row
            if row.Name then named[row.Name] = node end
            for i = 1, math.min(node.VisualChildrenCount or 0, 128) do
                visit(node:VisualChild(i), depth + 1, row.Index)
            end
        end
        local ok, err = pcall(function() visit(Ext.UI.GetRoot(), 0, 0) end)
        Ext.IO.SaveFile("wow-bg3/m3-menu.json", Ext.Json.Stringify({RunId = report.RunId, OK = ok, Error = ok and "" or tostring(err), Nodes = rows, Capture = captures}))
        if named.Eula and named.AcceptBtn and not acceptedEula and Ext._Internal.Getenv("WOW_BG3_ACCEPT_EULA") == "1" then
            acceptedEula = true
            local accepted, result = pcall(Ext._Internal.UiRaise, named.AcceptBtn, "Click")
            report.EulaClick = {OK = accepted, Result = tostring(result)}
            save("authorized-eula-click")
        end
    end
    if context == "client" then
        -- GUI overrides first: they must be in before BG3's GUI loads the files (WowUi.lua).
        local okUi, errUi = pcall(function() Ext.Require("WowUi.lua").Start(report.RunId) end)
        if not okUi then Ext.Log.Print("WOW-BG3 ui: " .. tostring(errUi)) end
        pcall(function() Ext.Require("Abilities.lua").Start(report.RunId) end)
        local okSpC, errSpC = pcall(function() Ext.Require("WowSpells.lua").Start("client") end)
        if not okSpC then Ext.Log.Print("WOW-BG3 spells: " .. tostring(errSpC)) end
        local okInvC, errInvC = pcall(function() Ext.Require("InventoryClient.lua").Start(report.RunId) end)
        if not okInvC then Ext.Log.Print("WOW-BG3 inventory client: " .. tostring(errInvC)) end
        -- World mode only in games started with WOW-BG3.
        local okWf, errWf = pcall(function() Ext.Require("WorldFlag.lua").Start(report.RunId, "client") end)
        if not okWf then Ext.Log.Print("WOW-BG3 worldflag: " .. tostring(errWf)) end
        -- BG3's own 3D screens (respec, level-up, mirror) shown as BG3 draws them.
        local okRs, errRs = pcall(function() Ext.Require("RenderScreen.lua").Start(report.RunId) end)
        -- The world's turn in turn-based mode outside combat (TurnHold.lua releases WoW's creatures then).
        pcall(function() Ext.Require("TurnHold.lua").StartClient(report.RunId) end)
        if not okRs then Ext.Log.Print("WOW-BG3 render screen: " .. tostring(errRs)) end
        local okMl, errMl = pcall(function() Ext.Require("MoveLoading.lua").StartClient() end)
        if not okMl then Ext.Log.Print("WOW-BG3 move loading: " .. tostring(errMl)) end
        -- What the WoW side is doing, on BG3's loading screen while a load waits for it.
        local okLn, errLn = pcall(function() Ext.Require("LoadingNote.lua").Start(report.RunId) end)
        if not okLn then Ext.Log.Print("WOW-BG3 loading note: " .. tostring(errLn)) end
        Ext.Events.Tick:Subscribe(captureMenu)
        Ext.Require("MenuProbe.lua").Start(report.RunId)
        -- New game straight into world mode: research hook in any game state.
        local okNg, errNg = pcall(function() Ext.Require("NewGame.lua").Start(report.RunId, "client") end)
        if not okNg then Ext.Log.Print("WOW-BG3 new game: " .. tostring(errNg)) end
        -- BG3's menus and character creation through their own commands (harness new game, named saves).
        local okMd, errMd = pcall(function() Ext.Require("MenuDrive.lua").Start(report.RunId) end)
        if not okMd then Ext.Log.Print("WOW-BG3 menu drive: " .. tostring(errMd)) end
        -- The hero (Hero.lua): on the client from the server's wow-bg3/hero.json.
        local okHero, errHero = pcall(function() Ext.Require("Hero.lua").Start(report.RunId) end)
        if not okHero then Ext.Log.Print("WOW-BG3 hero: " .. tostring(errHero)) end
        Ext.Require("ActionProbe.lua").Start(report.RunId)
        Ext.Require("LinkLive.lua").Start(report.RunId)
        Ext.Require("Arena.lua").Start(report.RunId)
        Ext.Require("WorldMode.lua").Start(report.RunId)
        Ext.Require("WowChat.lua").Start(report.RunId)
        Ext.Require("WowCursor.lua").Start(report.RunId)
        Ext.Require("RightAttack.lua").Start(report.RunId)
        -- Mirrored mobs' client copies where WoW draws them, every frame.
        local okMc, errMc = pcall(function() Ext.Require("MirrorsClient.lua").Start(report.RunId) end)
        if not okMc then Ext.Log.Print("WOW-BG3 mirrors client: " .. tostring(errMc)) end
        -- Stand-ins make no sound: WoW plays its creatures' own.
        local okSi, errSi = pcall(function() Ext.Require("Silence.lua").Start(report.RunId) end)
        if not okSi then Ext.Log.Print("WOW-BG3 silence: " .. tostring(errSi)) end
    end
    if context == "server" then
        local okMl, errMl = pcall(function() Ext.Require("MoveLoading.lua").StartServer() end)
        if not okMl then Ext.Log.Print("WOW-BG3 move fade: " .. tostring(errMl)) end
        -- New game straight into world mode: the nautiloid intro skipped with the
        -- story's own data, only when BG3 was started for a new game (WOW_BG3_NEW_GAME=1).
        local okNg, errNg = pcall(function() Ext.Require("NewGame.lua").Start(report.RunId, "server") end)
        if not okNg then Ext.Log.Print("WOW-BG3 new game: " .. tostring(errNg)) end
        -- The hero: the character the player controls, for every module and, through
        -- wow-bg3/hero.json, for the tools outside BG3 (Hero.lua).
        local okHero, errHero = pcall(function() Ext.Require("Hero.lua").Start(report.RunId) end)
        if not okHero then Ext.Log.Print("WOW-BG3 hero: " .. tostring(errHero)) end
        -- World mode only in games started with WOW-BG3: the save's flag.
        local okWf, errWf = pcall(function() Ext.Require("WorldFlag.lua").Start(report.RunId, "server") end)
        if not okWf then Ext.Log.Print("WOW-BG3 worldflag: " .. tostring(errWf)) end
        Ext.Require("WorldProbe.lua").Start(report.RunId)
        Ext.Require("ActionMonitor.lua").Start(report.RunId)
        Ext.Require("EncounterSurvey.lua").Start(report.RunId)
        Ext.Require("StandIns.lua").Start(report.RunId)
        Ext.Require("Mirrors.lua").Start(report.RunId)
        -- The party: companions added and removed BG3's way (link/tools/party.sh) and kept
        -- with the hero in world mode (Party.lua).
        local okParty, errParty = pcall(function() Ext.Require("Party.lua").Start(report.RunId) end)
        -- Save sync: a WoW snapshot id in every save, a restore request on load.
        local okWs, errWs = pcall(function() Ext.Require("WowSave.lua").Start(report.RunId) end)
        if not okWs then Ext.Log.Print("WOW-BG3 wowsave unavailable: " .. tostring(errWs)) end
        if not okParty then Ext.Log.Print("WOW-BG3 party: " .. tostring(errParty)) end
        -- /wowbg3 learn, unlearn: a BG3 spell for the hero (test command, files of its own).
        local okLearn, errLearn = pcall(function() Ext.Require("Learn.lua").Start(report.RunId) end)
        if not okLearn then Ext.Log.Print("WOW-BG3 learn: " .. tostring(errLearn)) end
        -- WoW portraits (BG3's custom-icon component) and WoW levels (display only) on stand-ins.
        local okPortraits, errPortraits = pcall(function() Ext.Require("Portraits.lua").Start(report.RunId, "server") end)
        if not okPortraits then Ext.Log.Print("WOW-BG3 portraits: " .. tostring(errPortraits)) end
        -- WoW race names on stand-ins while BG3's examine or trade screen shows them.
        local okPr, errPr = pcall(function() Ext.Require("PreviewRaces.lua").Start(report.RunId) end)
        if not okPr then Ext.Log.Print("WOW-BG3 preview races: " .. tostring(errPr)) end
        pcall(function() Ext.Require("Abilities.lua").Start(report.RunId) end)
        local okInv, errInv = pcall(function() Ext.Require("Inventory.lua").Start(report.RunId) end)
        if not okInv then Ext.Log.Print("WOW-BG3 inventory: " .. tostring(errInv)) end
        local okSp, errSp = pcall(function() Ext.Require("WowSpells.lua").Start("server") end)
        if not okSp then Ext.Log.Print("WOW-BG3 spells: " .. tostring(errSp)) end
        Ext.Require("Arena.lua").Start(report.RunId)
        -- WoW's light as BG3's gameplay light round the party: cones, sight, stealth.
        local okWl, errWl = pcall(function() Ext.Require("WorldLight.lua").Start(report.RunId) end)
        if not okWl then Ext.Log.Print("WOW-BG3 worldlight: " .. tostring(errWl)) end
    end
    save("bootstrap")
    Ext.Events.GameStateChanged:Subscribe(function() save("state-change") end)
    Ext.Events.SessionLoaded:Subscribe(function() save("session-loaded") end)
end
return Probe
