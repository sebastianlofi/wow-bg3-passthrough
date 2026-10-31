-- WoW portraits and WoW levels on BG3 stand-ins (server context). Both go through
-- BG3's own data, so BG3's UI shows them itself (no overlay, nothing drawn over BG3's HUD):
--
-- Portraits: a mirrored stand-in (Mirrors.lua, WOWBG3MirrorOwned) whose WoW mob benilla has baked a
-- portrait for gets an eoc::CustomIconComponent, the component that carries Tav's own portrait (a
-- WebP image; Tav's is 300 x 300), and its eoc::IconComponent names "CustomIconSet", as Tav's does.
-- benilla writes link/portraits/<display>.webp (the creature's model through WoW's portrait camera,
-- as WoW's target frame shows it) and link/portraits/guids.txt ("<guid hex> <display id>" lines); the
-- wowbg3 plugin (op 40) fills the component's buffer from that file, and both components are
-- replicated: the client decodes the image into the stand-in's ecl::CustomIconTextureComponent and
-- BG3's turn order (and anything else that shows the character's portrait) binds that texture. A mob
-- that joins a fight with no mirror yet (a guard WoW just summoned, one out of mirror reach) gets a
-- stand-in from the fight instead (LinkLive.lua, StandIns.lua Spawn, WOWBG3FightSpawned), treated the
-- same. Tav, and anything that is not such a stand-in, is never touched.
--
-- Levels (display only): BG3's hover popup ("Lv."), tooltips and turn order show the level the
-- client holds in its copy of eoc::LevelComponent, which the server replicates. Each stand-in's
-- server entity goes into the plugin's level table with its WoW mob's level (link/mirror.json
-- Mobs[].Level; op 41) and its eoc::LevelComponent is replicated again: the plugin's hook on the
-- server's replication serializer sends the WoW level for those entities, so the client and BG3's UI
-- see it, while the server's LevelComponent, which BG3's rules read (proficiency, attack bonus,
-- saves), keeps the template's level. eoc::exp::AvailableLevelComponent is never sent or rewritten:
-- BG3's client plays its level-up (VFX_UI_LevelUp_01, "Level Up!", the level-up sounds) on any
-- character whose AvailableLevel rises.
--
-- Status: wow-bg3/link/portraits-status.json. Research (harness only): wow-bg3/research-server.lua
-- runs on the server when wow-bg3/research-server-cmd.json carries this run's RunId and a new
-- Sequence; the result goes to wow-bg3/research-server-result.json (the client's research is
-- WorldMode.lua's).
local M = {}
local Harness = Ext.Require("Harness.lua")
local N = Ext.Require("WowNative.lua")
local DIR = "wow-bg3/link/"
local OP_ICON, OP_LEVEL = 40, 41
local EVERY = 15 -- ticks between passes
local RETRY = 150 -- ticks before a refused stand-in is tried again
local ICON_NAME = "CustomIconSet" -- what BG3's eoc::IconComponent holds for a character with a custom icon (Tav)
M.Portraits, M.Levels = true, true -- knobs (research)

local function readJson(path)
    local raw = Ext.IO.LoadFile(path)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

local function handleOf(e)
    local h = e and Ext._Internal.EntityProxyHandle(e)
    return math.type(h) == "integer" and h or nil
end

local function call(op, ...)
    local out = N.Call(op, ...)
    if out.Err ~= 0 then error(string.format("plugin %d: %s", out.Err, tostring(out.Why)), 0) end
    return out
end

local function research(runId, state)
    if not Harness.On then return end
    local cmd = readJson("wow-bg3/research-server-cmd.json")
    if not cmd or cmd.RunId ~= runId or math.type(cmd.Sequence) ~= "integer" or cmd.Sequence <= state.seq then return end
    state.seq = cmd.Sequence
    local ok, value = pcall(function()
        local src = assert(Ext.IO.LoadFile("wow-bg3/research-server.lua"), "no research-server.lua")
        local chunk = assert(load(src))
        return chunk()(M)
    end)
    Ext.IO.SaveFile("wow-bg3/research-server-result.json", Ext.Json.Stringify({RunId = runId, Sequence = cmd.Sequence,
        OK = ok, Value = ok and value or tostring(value)}))
end

function M.Start(runId, context)
    if context ~= "server" then return end
    local ticks, rstate = 0, {seq = 0}
    local icons = {}  -- stand-in uuid -> {Display, Stage ("filled", "sent"), Tick, Error, Guid}
    local levels = {} -- stand-in uuid -> {Handle, Level, Error, Tick, Guid}
    local displays, lastIndex = {}, nil
    local stats = {Filled = 0, Sent = 0, Refused = 0, Levels = 0, LevelRefused = 0}
    M.State = {Icons = icons, Levels = levels, Stats = stats}

    local function readIndex()
        local raw = Ext.IO.LoadFile(DIR .. "portraits/guids.txt")
        if not raw or raw == lastIndex then return end
        lastIndex = raw
        local map = {}
        for guid, display in raw:gmatch("(%x+)%s+(%d+)") do map[guid:lower()] = tonumber(display) end
        displays = map
    end

    -- Fills the Icon ScratchBuffer at `field` with the portrait for `display` (plugin op 40).
    local function fill(field, display)
        local a1, a2, a3 = N.Pieces(field)
        return call(OP_ICON, a1, a2, a3, display, 1, 0)
    end

    local function send(e)
        e:Replicate("CustomIcon")
        if e.Icon and e.Icon.Icon ~= ICON_NAME then e.Icon.Icon = ICON_NAME end
        if e.Icon then e:Replicate("Icon") end
    end

    local function portrait(uuid, guid, display)
        local d = icons[uuid]
        if d and d.Display == display and d.Stage == "sent" then return end
        if d and d.Error and ticks - d.Tick < RETRY then return end
        local e = Ext.Entity.Get(uuid)
        if not e then return end
        local ok, err = pcall(function()
            if d and d.Display == display and d.Stage == "filled" then
                -- Made in the last pass (its buffer filled in the command buffer): now on the entity.
                assert(e.CustomIcon, "the CustomIcon component did not arrive")
                send(e)
                d.Stage, d.Tick = "sent", ticks
                stats.Sent = stats.Sent + 1
                return
            end
            if e.CustomIcon then
                -- One of ours from an earlier pass (op 40 refuses any other): the new picture.
                local field = assert(Ext._Internal.FieldAddress(handleOf(e), "CustomIcon", "Icon"), "no Icon field")
                fill(field, display)
                send(e)
                icons[uuid] = {Display = display, Stage = "sent", Tick = ticks, Guid = guid}
                stats.Sent = stats.Sent + 1
                return
            end
            local addr, cls = Ext._Internal.EntityComponentAdd(assert(handleOf(e), "no handle"), "CustomIcon", false)
            assert(addr, "CreateComponent CustomIcon refused")
            local field = assert(Ext._Internal.ObjectFieldAddress(addr, cls, "Icon"), "no Icon field")
            fill(field, display)
            icons[uuid] = {Display = display, Stage = "filled", Tick = ticks, Guid = guid}
            stats.Filled = stats.Filled + 1
        end)
        if not ok then
            icons[uuid] = {Display = display, Error = tostring(err), Tick = ticks, Guid = guid}
            stats.Refused = stats.Refused + 1
        end
    end

    local function handlePieces(h) return h & 0xffff, (h >> 16) & 0xffff, (h >> 32) & 0xffff, (h >> 48) & 0xffff end

    local function level(uuid, guid, wow)
        local d = levels[uuid]
        if d and d.Level == wow and not d.Error then return end
        if d and d.Error and ticks - d.Tick < RETRY then return end
        local e = Ext.Entity.Get(uuid)
        if not e or not e.EocLevel then return end
        local ok, err = pcall(function()
            local h = assert(handleOf(e), "no handle")
            local a1, a2, a3, b1 = handlePieces(h)
            call(OP_LEVEL, a1, a2, a3, b1, wow, 0)
            -- Send the level component again: the hook puts the WoW level in what goes out. Not
            -- AvailableLevel: a rise there is BG3's level-up (effect, text, sound) on the client.
            e:Replicate("EocLevel")
            levels[uuid] = {Handle = h, Level = wow, Bg3 = e.EocLevel.Level, Tick = ticks, Guid = guid}
            stats.Levels = stats.Levels + 1
        end)
        if not ok then
            levels[uuid] = {Level = wow, Error = tostring(err), Tick = ticks, Guid = guid}
            stats.LevelRefused = stats.LevelRefused + 1
        end
    end

    local function forgetLevel(uuid)
        local d = levels[uuid]
        levels[uuid] = nil
        if d and d.Handle then
            local a1, a2, a3, b1 = handlePieces(d.Handle)
            pcall(call, OP_LEVEL, a1, a2, a3, b1, 0, 0)
        end
    end

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % EVERY ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        pcall(research, runId, rstate)
        local ok, err = pcall(function()
            local owned = {}
            for uuid, guid in pairs(WOWBG3MirrorOwned or {}) do owned[uuid] = guid end
            for uuid, guid in pairs(WOWBG3FightSpawned or {}) do owned[uuid] = guid end
            if M.Portraits then
                readIndex()
                for uuid, guid in pairs(owned) do
                    local display = displays[tostring(guid):lower()]
                    if display then portrait(uuid, guid, display) end
                end
            end
            if M.Levels then
                local mirror = readJson(DIR .. "mirror.json")
                local wowLevel = {}
                for _, m in ipairs(mirror and type(mirror.Mobs) == "table" and mirror.Mobs or {}) do
                    local lv = type(m) == "table" and math.tointeger(m.Level)
                    if lv and lv >= 1 and lv <= 99 and type(m.Guid) == "string" then wowLevel[m.Guid:lower()] = lv end
                end
                for uuid, guid in pairs(owned) do
                    local wow = wowLevel[tostring(guid):lower()]
                    if wow then level(uuid, guid, wow) end
                end
            end
            for uuid in pairs(icons) do if not owned[uuid] then icons[uuid] = nil end end
            for uuid in pairs(levels) do if not owned[uuid] then forgetLevel(uuid) end end
        end)
        if ticks % (EVERY * 4) == 0 then
            Ext.IO.SaveFile(DIR .. "portraits-status.json", Ext.Json.Stringify({RunId = runId, Tick = ticks,
                Error = not ok and tostring(err) or nil, Stats = stats, Icons = icons, Levels = levels}))
        end
    end)
end

return M
