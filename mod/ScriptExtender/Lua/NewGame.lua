-- New game straight into world mode (start a New Game, make or pick a
-- character in BG3's character creation, e.g. the origin Gale, and land in world mode at the WoW
-- character's spot without the nautiloid intro). On for every New Game started from BG3's own menu
-- with WOW-BG3 enabled (the release has no launch flag; the old
-- WOW_BG3_NEW_GAME=1 of sandbox.py --new-game is no longer read). A session is a new game once the
-- story's CharacterCreationStarted fires in it (M.Enabled()); a loaded save never sees that event, so
-- the default flow (Continue, Load) never runs any of this.
--
-- What is skipped, all with the story's own data (BG3's goals: Gustav.pak
-- Mods/Gustav/Story/RawFiles/Goals, SharedDev Z_Shared_CharacterCreation.txt, ZZZ_LastGoal.txt):
--   1. The opening cinematic: at the end of character creation the story moves the party to the
--      start level with TeleportPartiesToLevelWithMovie(level, "", movie), the movie taken from
--      DB_CharacterCreationTransitionInfo("TUT_Avernus_C", "GUS_CGI01_Part2") (Gustav Start.txt;
--      "can be empty string" says Z_Shared_CharacterCreation.txt). The server sets that movie to ""
--      while character creation runs (and once more right before CharacterCreationFinished), so the
--      story's own transition runs without the movie.
--   2. The pod awakening (TUT_Start.txt): on the level's first load, PROC_LevelLoadedOnce
--      ("TUT_Avernus_C") assigns every player a pod and starts the pod's intro dialog (the
--      level-starting cinematic dialog, TUT_Start_OriginIntroduction_*), guarded per user by
--      QRY_OnlyOncePerUser(player, "TUT_Start_IntroCinematic"); PROC_Tutorial_Start plays the
--      intro's mute sound (QRY_OnlyOnce("TUT_Start_MuteSound")). Before the story sees LevelLoaded
--      ("TUT_Avernus_C") the server marks both as done (DB_OnlyOnce_PerUser(profile,
--      "TUT_Start_IntroCinematic"), DB_OnlyOnce("TUT_Start_MuteSound")), as the story itself does
--      when it reloads. So no pod, no intro dialog, no mute, and nothing that hangs off the intro's
--      end (DialogEnded: the get-up timer with the journal's "escape the nautiloid" start, the
--      narrator's start bark TUT_Start_PAD_Start).
--   3. Any dialog that still runs on the hero once the level plays is stopped with BG3's own
--      DialogRequestStop (none in tests; logged).
-- Kept as BG3 has it: the tutorial's goals (they wait for the player at the pods, the transform
-- chamber, the helm: 1 km east of the void), Lae'zel and the alt guide off stage (the story does it
-- when the avatar is not Lae'zel), Shadowheart in her pod (the transform chamber), the tutorial's
-- level-up and party rules. Status: wow-bg3/newgame-status.json. The world bridge starts world mode
-- once Hero.lua reports the hero Ready in wow-bg3/hero.json (in the level, no dialog, gameplay on).
--
-- Client (harness only): a research hook that runs in any game state (the main menu and character
-- creation too; WorldMode's Research runs only in a level): wow-bg3/newgame-command.json {RunId,
-- Sequence, Action = "Research"} runs wow-bg3/newgame-research.lua (a chunk returning a function)
-- and writes wow-bg3/newgame-research-result.json. NewGameDrive.lua drives the menu and character
-- creation for the headless test.
local M = {}
local Harness = Ext.Require("Harness.lua")
local LEVEL = "TUT_Avernus_C"
-- The host level: with wow-bg3/host.json naming another level (Hero.lua), character
-- creation sends the party straight there (the transition's level is the story's own data too).
local HOST = Ext.Require("Hero.lua").REGION
local INTRO_ONCE = "TUT_Start_IntroCinematic"
local MUTE_ONCE = "TUT_Start_MuteSound"

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function readJson(name)
    local raw = Ext.IO.LoadFile(name)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end
local function uuidOf(s)
    return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil
end

-- This Lua session began with BG3's character creation (server: the story's CharacterCreationStarted;
-- bg3le builds new Lua states at every load, so a loaded save starts false). The level change from
-- character creation to the host level keeps the session (no UnloadSession), so it stays true there.
M.IsNewGame = false
function M.Enabled() return M.IsNewGame end
local LAUNCHER = "wowbg3-launcher"

local function startServer(runId)
    local status = {RunId = runId, Enabled = false, Movie = nil, Intro = nil, Dialogs = {}, Stopped = {},
        Events = {}}
    local function save() Ext.IO.SaveFile("wow-bg3/newgame-status.json", Ext.Json.Stringify(status)) end
    local function note(what)
        status.Events[#status.Events + 1] = {Time = Ext.Utils.MonotonicTime(), What = what}
        if #status.Events > 64 then table.remove(status.Events, 1) end
        Ext.Log.Print("WOW-BG3 new game: " .. what)
    end
    save()

    -- 1. The opening cinematic: the transition movie off (the story's own config, empty allowed).
    local function noMovie(why)
        local rows = q(function() return Osi.DB_CharacterCreationTransitionInfo:Get(nil, nil) end) or {}
        for _, row in ipairs(rows) do
            local level, movie = row[1], row[2]
            if type(movie) == "string" and movie ~= "" then
                Osi.DB_CharacterCreationTransitionInfo:Delete(level, movie)
                Osi.DB_CharacterCreationTransitionInfo(level, "")
                status.Movie = {Level = level, Was = movie, Now = "", Why = why}
                note("transition movie " .. movie .. " to " .. tostring(level) .. " off (" .. why .. ")")
                save()
            end
        end
    end
    -- Act 1 as host level: its first-load story. The crash-site dialog and the wake-up
    -- dialogs after it (Act1_CRA_Escape_WakeUp) start on the level's first load with the hero in them.
    -- The story's own skip switch is DB_CRA_SkipCrashCinematic(1) (GLO_StartupParameters: the game
    -- option skipCrashCinematic; Larian's act-skip restore sets it too): with it the goal's INIT
    -- completes the goal, so no crash dialog, no wake-up dialogs and nothing left waiting on them.
    -- Set once character creation ends (before Act 1 loads, so before that goal's INIT) and again
    -- before the story sees Act 1's LevelLoaded; DB_CRA_Crash_Started(1), the dialog's own guard, stays
    -- as the second line. Everything else of Act 1's first load runs as BG3 has it (the origins wait
    -- at the crash site for the player; the hero is in the void, far from every trigger).
    local function noCrash(level, why)
        if HOST ~= "WLD_Main_A" or (level and level ~= "WLD_Main_A") then return end
        local did = {}
        if #(q(function() return Osi.DB_CRA_SkipCrashCinematic:Get(1) end) or {1}) == 0 then
            Osi.DB_CRA_SkipCrashCinematic(1)
            did[#did + 1] = "crash cinematic skipped (DB_CRA_SkipCrashCinematic)"
        end
        if level and #(q(function() return Osi.DB_CRA_Crash_Started:Get(1) end) or {1}) == 0 then
            Osi.DB_CRA_Crash_Started(1)
            did[#did + 1] = "crash-site dialog marked as started"
        end
        if #did > 0 then
            note("Act 1 (" .. why .. "): " .. table.concat(did, ", "))
            save()
        end
    end
    -- 2. The pod awakening: the story's once-guards of the intro, set before it sees the level load.
    local function noIntro(level)
        if level ~= LEVEL or status.Intro then return end
        if q(function() return #Osi.DB_LevelLoadedOnce:Get(LEVEL) end) ~= 0 then return end -- not a first load
        local intro = {Players = {}}
        for _, row in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
            local who = row[1]
            local user = q(Osi.GetReservedUserID, who)
            local profile = user and q(Osi.GetUserProfileID, user)
            if profile then
                Osi.DB_OnlyOnce_PerUser(profile, INTRO_ONCE)
                intro.Players[#intro.Players + 1] = {Character = who, User = user, Profile = profile}
            else
                intro.Players[#intro.Players + 1] = {Character = who, User = user, Error = "no user profile"}
            end
        end
        if #(q(function() return Osi.DB_OnlyOnce:Get(MUTE_ONCE) end) or {1}) == 0 then Osi.DB_OnlyOnce(MUTE_ONCE) end
        intro.Mute = true
        status.Intro = intro
        note("pod intro and its mute marked as played for " .. #intro.Players .. " player(s)")
        save()
    end
    local function listen(name, arity, when, fn)
        local ok, err = pcall(Ext.Osiris.RegisterListener, name, arity, when, function(...)
            local okf, errf = pcall(fn, ...)
            if not okf then note(name .. " handler: " .. tostring(errf)) end
        end)
        if not ok then note("listener " .. name .. ": " .. tostring(err)) end
    end
    listen("CharacterCreationStarted", 0, "after", function()
        M.IsNewGame = true
        status.Enabled = true
        -- A fresh id per new game: the client presses Escape on the opening movie only when it changes.
        status.Creation = string.format("%d-%d", math.floor(Ext.Utils.MonotonicTime()), math.random(0, 0xffffff))
        -- Character creation needs no WoW: the launcher's loading-screen hold lets it in (the hold is for
        -- the host level, where WoW must be up first; the launcher resets "ready" at the next unload).
        pcall(function()
            for _, p in ipairs(Ext.Plugins.List() or {}) do
                if p.Name == LAUNCHER and p.Loaded then Ext.Plugins.Set(LAUNCHER, "ready", 1) end
            end
        end)
        note("new game with WOW-BG3 (character creation started)")
        noMovie("character creation started")
    end)
    listen("CharacterCreationFinished", 0, "before", function()
        if not M.IsNewGame then return end
        noMovie("character creation finished")
        -- Another host level (host.json): the story moves the party only to the campaign's startup
        -- level (Z_Shared_CharacterCreation: GetActiveModStartupLevel joined with the transition DB),
        -- so its own switch DB_CharacterCreationNoTeleport stops that move and we send the party to the
        -- host level ourselves, as the story would (DB_TeleportedToStartCharacters, no movie).
        if HOST ~= LEVEL then Osi.DB_CharacterCreationNoTeleport(1) end
        noCrash(nil, "character creation finished")
    end)
    listen("CharacterCreationFinished", 0, "after", function()
        if not M.IsNewGame or HOST == LEVEL then return end
        for _, row in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
            Osi.DB_TeleportedToStartCharacters(row[1])
        end
        Osi.TeleportPartiesToLevelWithMovie(HOST, "", "")
        status.Host = HOST
        note("party sent straight to the host level " .. HOST)
        save()
    end)
    listen("LevelLoaded", 1, "before", function(level)
        if M.IsNewGame then noIntro(level); noCrash(level, "first load") end
    end)
    listen("LevelGameplayStarted", 2, "after", function(level)
        if not M.IsNewGame then return end
        status.Level = level
        M.LevelStarted = level
        note("gameplay started in " .. tostring(level))
        save()
    end)
    for name, arity in pairs({DialogStarted = 2, AutomatedDialogStarted = 2}) do
        listen(name, arity, "after", function(dialog, id)
            if not M.IsNewGame then return end
            status.Dialogs[#status.Dialogs + 1] = {Event = name, Dialog = tostring(dialog), Instance = id,
                Region = q(function() local h = Osi.GetHostCharacter(); return h and Osi.GetRegion(h) end)}
            if #status.Dialogs > 32 then table.remove(status.Dialogs, 1) end
            save()
        end)
    end

    -- 3. A dialog still running on the hero once the level plays: BG3's own stop (none expected).
    local ticks = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if not M.IsNewGame or ticks % 30 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        -- While character creation runs: keep the movie off (the listener above is the second line).
        if not M.LevelStarted and #(q(function() return Osi.DB_CharacterCreationStarted:Get(1) end) or {}) == 1 then
            pcall(noMovie, "character creation")
        end
        if M.LevelStarted ~= HOST or ticks % 150 ~= 0 then return end
        local hero = uuidOf(q(Osi.GetHostCharacter))
        for _, row in ipairs(q(function() return Osi.DB_DialogSpeakers:Get(nil, nil, nil) end) or {}) do
            if hero and tostring(row[2]):find(hero, 1, true) and #status.Stopped < 8 then
                local ok, err = pcall(Osi.DialogRequestStop, hero)
                status.Stopped[#status.Stopped + 1] = {Instance = row[1], OK = ok, Error = (not ok) and tostring(err) or nil}
                note("a dialog ran on the hero (instance " .. tostring(row[1]) .. "): DialogRequestStop " .. (ok and "sent" or tostring(err)))
                save()
                break
            end
        end
    end)
end

local function startClient(runId)
    local sequence, ticks = 0, 0
    -- 4. The opening movie before character creation (GUS_CGI01_Part1, ~2 min, the engine's own,
    -- played while character creation loads): skipped the way a player skips it, Escape (harness
    -- With one Escape the client left PrepareRunning for character creation at once).
    -- Only for a new game: the first PrepareRunning after the main menu whose server started character
    -- creation (newgame-status.json Creation, a new id since the menu; no
    -- launch flag), pressed 2 s in and again every 3 s while still in it, at most 4 times. A load from
    -- the menu writes no new Creation, so nothing is pressed over its loading screen.
    do
        local function creation()
            local s = readJson("wow-bg3/newgame-status.json")
            return s and s.RunId == runId and s.Creation or nil
        end
        local fromMenu, since, presses, last, seen, t = false, nil, 0, nil, nil, 0
        Ext.Events.Tick:Subscribe(function()
            t = t + 1
            local state = tostring(Ext.Utils.GetGameState())
            if state == "Menu" then
                if not fromMenu or t % 15 == 0 then seen = creation() end
                fromMenu, since, presses, last = true, nil, 0, nil
                return
            end
            if not fromMenu or presses >= 4 then return end
            if state ~= "PrepareRunning" then
                if since then fromMenu = false end -- the movie (and character creation's load) is over
                return
            end
            if not since and (t % 5 ~= 0 or creation() == seen) then return end -- not a new game (yet)
            local now = Ext.Utils.MonotonicTime()
            since = since or now
            if now - since >= 2000 and (not last or now - last >= 3000) then
                pcall(Ext.Input.InjectKeyPress, "ESCAPE")
                presses, last = presses + 1, now
                Ext.Log.Print("WOW-BG3 new game: opening movie skipped (Escape " .. presses .. ")")
            end
        end)
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 15 ~= 0 then return end
        local c = readJson("wow-bg3/newgame-command.json")
        if not c or c.RunId ~= runId or math.type(c.Sequence) ~= "integer" or c.Sequence <= sequence then return end
        sequence = c.Sequence
        local ok, value = pcall(function()
            if c.Action == "Research" then
                assert(Harness.On, "harness actions are off (WOW_BG3_HARNESS=1)")
                local src = assert(Ext.IO.LoadFile("wow-bg3/newgame-research.lua"), "no newgame-research.lua")
                local chunk = assert(load(src))
                return chunk()(c)
            end
            error("unknown action")
        end)
        Ext.IO.SaveFile("wow-bg3/newgame-research-result.json", Ext.Json.Stringify({RunId = runId, Sequence = sequence,
            State = tostring(Ext.Utils.GetGameState()), OK = ok, Value = ok and value or tostring(value)}))
    end)
end

function M.Start(runId, context)
    if context == "server" then startServer(runId) else startClient(runId) end
end
return M
