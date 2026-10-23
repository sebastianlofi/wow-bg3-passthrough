-- Client-side harness channel for a disposable save, separate from MenuProbe.
-- Read-only: Explore (bounded UI/view-model property reads).
-- Mutating: Button, limited to normal HotBar turn controls through their own UI command
-- (CanExecute first, exactly what a click invokes); UseSlot, the hotbar's own UseSlotCommand
-- on one allowlisted self-targeted spell slot of the disposable character; ConfirmCast and
-- CancelCast, one left or right click at the screen centre through the real pointer (runtime
-- patch 0002), only while that allowlisted spell is in its targeting preview; MoveClick, one
-- left click at a bounded offset from the screen centre, only on the host's own turn in
-- turn-based mode with no spell preview open. Clicks may hit world objects; private save only. No Osiris calls, cast flags or rule edits.
-- TargetCombatant: the turn-order bar's TargetGameobjectCommand during a targeted preview.
-- SelectHost: the portrait SelectCharacter command (disposable character, or another character
-- this player controls, so its turn can be ended through the normal HotBar).
-- World picking: Camera (read-only projection and picking state), Hover
-- (pointer motion only) and ClickWorld. ClickWorld projects a world point or character
-- through the active camera, moves the real pointer there, then clicks only if the game's
-- own picking helper agrees: the intended character for Target, plain ground otherwise.
-- Modes: Walk (real time, out of turn-based mode, bounded distance), Move (own turn),
-- OutOfTurn (turn-based, not the host's turn: probes the engine's refusal), Target
-- (an allowlisted targeted spell is in its preview) and Ground (an allowlisted point spell such
-- as Jump is in its preview; plain ground within the walk limit).
-- World fights: BG3's common actions are allowlisted too (Hide, Shove, Help, Throw,
-- Improvised Weapon, Jump) for the basic-actions evidence in a mirrored fight.
-- Mutations require Running, the authorized host/region per the server monitor, a fresh
-- monotonic sequence and are never retried.
local M = {}
local Harness = Ext.Require("Harness.lua")
-- The hero: the character the player controls (Hero.lua), was Tav's fixed UUID.
local Hero = Ext.Require("Hero.lua")
local EXPECTED_REGION = Ext.Require("Hero.lua").REGION -- host level (Hero.lua, host.json)
local BUTTONS = {HotBar = {EnterFTBButton = true, ExitFTBButton = true, EndTurnButton = true}}
-- Self-targeted actions confirm with a centre click; targeted ones need ClickWorld Target.
local SLOT_SPELLS = {Shout_Rage = true, Shout_Dash = true, Shout_Disengage = true, Shout_Hide = true}
local TARGET_SPELLS = {Projectile_FireBolt = true, Target_MainHandAttack = true, Target_Shove = true,
    Target_Help = true, Throw_Throw = true, Throw_ImprovisedWeapon = true,
    -- Lacerate (the greataxe's weapon action).
    Target_Slash_New = true}
-- Point spells confirm with ClickWorld Ground.
local POINT_SPELLS = {Projectile_Jump = true}
local function allowed(spell) return SLOT_SPELLS[spell] or TARGET_SPELLS[spell] or POINT_SPELLS[spell] end
local WALK_LIMIT = 25.0
-- Keep clicks off the bottom HUD and screen edges. SAFE is the area for the old 1280x720 private
-- window; safeArea scales it to the real screen (at 3440x1440 the fixed numbers put the
-- centred target of every WoW-side targeted cast "off the safe screen area").
local SAFE = {x0 = 40, x1 = 1240, y0 = 60, y1 = 580}
local function safeArea(w, h)
    local sx, sy = w / 1280, h / 720
    return {x0 = SAFE.x0 * sx, x1 = SAFE.x1 * sx, y0 = SAFE.y0 * sy, y1 = SAFE.y1 * sy}
end

local function vec(v) return {v[1], v[2], v[3]} end
local function uuidOf(entity)
    if entity == nil then return nil end
    local ok, u = pcall(function() return tostring(entity.Uuid.EntityUuid) end)
    if ok then return u end
    return "<entity>"
end
-- Matrices arrive as 16 column-major numbers or as four columns.
local function flat(m)
    local out = {}
    local first = m[1]
    if type(first) == "number" then
        for i = 1, 16 do out[i] = m[i] end
    else
        for c = 1, 4 do for r = 1, 4 do out[(c - 1) * 4 + r] = m[c][r] end end
    end
    for i = 1, 16 do assert(type(out[i]) == "number", "Matrix element missing") end
    return out
end
local function mul(m, v)
    local o = {}
    for r = 1, 4 do o[r] = m[r] * v[1] + m[4 + r] * v[2] + m[8 + r] * v[3] + m[12 + r] * v[4] end
    return o
end
local function cameras()
    local rows = {}
    for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("Camera")) do
        local ok, c = pcall(function() return e.Camera end)
        if ok and c and c.Active == true then rows[#rows + 1] = c end
        if #rows >= 8 then break end
    end
    return rows
end
local function activeController()
    local list = cameras()
    assert(#list > 0, "No active camera")
    for _, c in ipairs(list) do if c.AcceptsInput == true then return c.Controller, #list end end
    return list[1].Controller, #list
end
-- BG3's window size in pixels (the space of WindowCursorPos and of injected mouse events).
-- The HotBar's ScreenResolution holds floats ({3440.0, 1440.0}); the old integer
-- test refused them and fell back to 1280x720 at every size, so at 3440x1440 every projected click
-- (harness ClickWorld, LinkLive's aim and preview cancel) landed at the wrong pixel. Integral
-- numbers of either kind are taken; the UI root's size is the next source (Noesis lays out at the
-- window's pixels).
local function screenSize(dc)
    local function size(w, h)
        w, h = tonumber(w), tonumber(h)
        if w and h and w >= 1 and h >= 1 and w == math.floor(w) and h == math.floor(h) then
            return math.tointeger(w), math.tointeger(h)
        end
    end
    local ok, res = pcall(function() return dc.Data.ScreenResolution end)
    if ok and type(res) == "table" then
        local w, h = size(res[1], res[2])
        if w then return w, h end
    end
    local okr, w, h = pcall(function()
        local root = Ext.UI.GetRoot()
        return size(root.ActualWidth, root.ActualHeight)
    end)
    if okr and w then return w, h end
    return 1280, 720
end
-- In this game build the rf::Camera matrices sit 16 bytes before bg3le's declared offsets
-- (observed: the View read ends in its translation column followed by
-- InvView's first column). Detect that shift and rebuild the true matrices from the
-- neighbouring reads: View's first column is the cross product of the next two (checked
-- against InvView's first element), Projection's first column is the tail of the
-- InvView read. Aligned layouts pass through unchanged.
local function near(a, b) return math.abs(a - b) < 1e-3 end
function M.Matrices(ctl)
    local cam = ctl.Camera
    local v, iv, p = flat(cam.ViewMatrix), flat(cam.InvViewMatrix), flat(cam.ProjectionMatrix)
    if near(v[16], 1) and near(v[4], 0) and near(v[8], 0) and near(v[12], 0) then
        return v, p, "aligned"
    end
    assert(near(v[12], 1) and near(v[4], 0) and near(v[8], 0), "Unrecognised camera matrix layout")
    local c1, c2 = {v[1], v[2], v[3]}, {v[5], v[6], v[7]}
    local c0 = {c1[2] * c2[3] - c1[3] * c2[2], c1[3] * c2[1] - c1[1] * c2[3], c1[1] * c2[2] - c1[2] * c2[1]}
    -- v[13] is InvView[1][1] = R00; it fixes the handedness of the rebuilt column.
    if math.abs(c0[1] + v[13]) < math.abs(c0[1] - v[13]) then c0 = {-c0[1], -c0[2], -c0[3]} end
    assert(near(c0[1], v[13]), "Rebuilt view column disagrees with InvView")
    local view = {c0[1], c0[2], c0[3], 0}
    for i = 1, 12 do view[4 + i] = v[i] end
    local proj = {iv[13], iv[14], iv[15], iv[16]}
    for i = 1, 12 do proj[4 + i] = p[i] end
    return view, proj, "shifted16"
end
function M.Project(world, w, h)
    local ctl = activeController()
    local view, proj = M.Matrices(ctl)
    local clip = mul(proj, mul(view, {world[1], world[2], world[3], 1.0}))
    assert(clip[4] > 1e-4, "Point is behind the camera")
    local nx, ny = clip[1] / clip[4], clip[2] / clip[4]
    return {X = (nx * 0.5 + 0.5) * w, Y = (0.5 - ny * 0.5) * h, YFlipped = (ny * 0.5 + 0.5) * h,
        Ndc = {nx, ny, clip[3] / clip[4]}}
end
local function pickInfo()
    local p = Ext.UI.GetPickingHelper(1)
    assert(p, "No picking helper")
    local out = {}
    local function grab(key, fn) local ok, v = pcall(fn); out[key] = ok and v or nil end
    grab("Cursor", function() local c = p.WindowCursorPos; return {c[1], c[2]} end)
    grab("World", function() return vec(p.Inner.WorldPosition) end)
    grab("Position", function() return vec(p.Inner.Position) end)
    grab("Scenery", function() return vec(p.Inner.SceneryPosition) end)
    for i = 1, 2 do
        grab("Character" .. i, function() return uuidOf(p.Inner.Inner[i].Character) end)
        grab("Item" .. i, function() return uuidOf(p.Inner.Inner[i].Item) end)
    end
    return out
end
local function hostPosition()
    local e = assert(Hero.Entity(), "Hero entity unavailable on client")
    return vec(e.Transform.Transform.Translate)
end
local function worldPoint(command)
    if type(command.Target) == "string" and command.Target ~= "" then
        local e = assert(Ext.Entity.Get(command.Target), "Target entity unavailable on client")
        local p = vec(e.Transform.Transform.Translate)
        p[2] = p[2] + (tonumber(command.Lift) or 0.0) -- aim above the feet when targeting a body
        return p
    end
    local w = command.World
    assert(type(w) == "table" and #w == 3, "World point or Target required")
    for i = 1, 3 do assert(type(w[i]) == "number" and w[i] == w[i] and math.abs(w[i]) < 1e5, "Invalid world point") end
    return {w[1], w[2], w[3]}
end

local function findUnique(widget, name)
    local found, count, seen, visited = nil, 0, {}, 0
    local function walk(node, owner, depth)
        if not node or depth > 24 or visited >= 4096 or seen[tostring(node)] then return end
        seen[tostring(node)] = true
        visited = visited + 1
        local ok, nodeName = pcall(function() return node.Name end)
        if not ok then nodeName = nil end
        if node.Type == "ls.UIWidget" then owner = nodeName end
        if owner == widget and nodeName == name then found, count = node, count + 1 end
        for i = 1, math.min(node.VisualChildrenCount or 0, 128) do walk(node:VisualChild(i), owner, depth + 1) end
    end
    walk(Ext.UI.GetRoot(), nil, 0)
    assert(count == 1, "Target must identify exactly one current node")
    return found
end

local function scalar(v)
    local t = type(v)
    if t == "string" or t == "number" or t == "boolean" or t == "nil" then return v end
    local ok, s = pcall(function() return v:ToString() end)
    return ok and ("<" .. tostring(s) .. ">") or tostring(v)
end
local function describe(obj, items)
    if type(obj) ~= "table" then return {Value = scalar(obj)} end
    local out = {Text = scalar(obj)}
    local ok, props = pcall(function() return obj:GetAllProperties() end)
    if ok and type(props) == "table" then
        out.Properties = {}
        local n = 0
        for k, v in pairs(props) do
            n = n + 1
            if n > 256 then break end
            out.Properties[tostring(k)] = scalar(v)
        end
    end
    -- Collections index from 1 and return nil past the end; non-collections raise.
    if (items or 0) > 0 then
        for i = 1, items do
            local iok, item = pcall(function() return obj[i] end)
            if not iok or item == nil then break end
            out.Items = out.Items or {}
            out.Items[i] = describe(item, 0)
        end
    end
    return out
end
-- Path segments are property names or 1-based collection indices.
local function resolve(start, path)
    local obj = start
    for seg in string.gmatch(path or "", "[^%.]+") do
        local index = math.tointeger(tonumber(seg))
        obj = obj[index or seg]
        assert(obj ~= nil, "Path segment not found: " .. seg)
    end
    return obj
end

local function activeAllowlistedSlot(dc)
    local character = dc.CurrentPlayer.SelectedCharacter
    assert(character.EntityUUID == Hero.Uuid(), "Selected character is not the hero")
    local props = character.PlayerCharacterProperties
    assert(props.IsCasting == true, "No spell is in its targeting preview")
    local found
    for _, bar in ipairs({"CommonHotBar", "ClassHotBar"}) do
        local list = props[bar].SlotList
        for i = 1, 64 do
            local slot = list[i]
            if slot == nil then break end
            if slot.IsActive == true then
                local spell = slot.Content and slot.Content.PrototypeID
                assert(found == nil, "More than one active slot")
                assert(allowed(spell), "Active slot spell not allowlisted: " .. tostring(spell))
                found = spell
            end
        end
    end
    assert(found, "No active allowlisted slot")
    return found
end

local function authorized(runId)
    local raw = Ext.IO.LoadFile("wow-bg3/m3-actions.json")
    assert(raw, "Server monitor report unavailable")
    local s = Ext.Json.Parse(raw)
    assert(s.RunId == runId, "Server monitor report is from another run")
    -- The hero (the character the player controls, Hero.lua) must be present in the expected
    -- region (the private profile's tutorial level), and only it is ever driven.
    assert(type(s.Hero) == "string" and s.Hero ~= "" and s.Region == EXPECTED_REGION, "No hero in the authorized level")
end

-- Shared with LinkLive.lua (same client context). Never use both channels at once.
M.PickInfo, M.ScreenSize, M.HostPosition, M.FindUnique = pickInfo, screenSize, hostPosition, findUnique
M.SAFE, M.Safe, M.EXPECTED_REGION, M.Hero = SAFE, safeArea, EXPECTED_REGION, Hero

function M.Start(runId)
    local sequence, ticks = 0, 0
    local pending = {} -- {due tick, function}; pointer steps spread over frames
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        while pending[1] and pending[1][1] <= ticks do
            local step = table.remove(pending, 1)
            local sok, serr = pcall(step[2])
            if not sok then
                pending = {} -- abandon the rest of the sequence; never click blind
                Ext.IO.SaveFile("wow-bg3/action-followup.json", Ext.Json.Stringify({RunId = runId,
                    Sequence = step[3] or 0, OK = false, Value = tostring(serr)}))
            end
        end
        if ticks % 15 ~= 0 then return end
        local raw = Ext.IO.LoadFile("wow-bg3/action-command.json")
        if not raw or #raw > 8192 then return end
        local parsed, command = pcall(Ext.Json.Parse, raw)
        if not parsed or type(command) ~= "table" or command.RunId ~= runId then return end
        if math.type(command.Sequence) ~= "integer" or command.Sequence <= sequence or command.Sequence > 4096 then return end
        sequence = command.Sequence -- never retried automatically
        local result = {RunId = runId, Sequence = sequence, Action = command.Action}
        local ok, value = pcall(function()
            assert(Ext.Utils.GetGameState() == "Running", "Harness actions require a running world")
            if command.Action == "Research" then
                -- Test setups only (Harness.lua): wow-bg3/action-research.lua, a chunk returning a
                -- function (M, command, later) -> value, where later(ticks, fn) runs fn that many ticks on
                -- (pointer steps spread over frames, as the actions below do).
                assert(Harness.On, "harness actions are off (WOW_BG3_HARNESS=1)")
                local src = assert(Ext.IO.LoadFile("wow-bg3/action-research.lua"), "no action-research.lua")
                local fn = assert(load(src))()
                local seq = sequence
                return fn(M, command, function(d, f) pending[#pending + 1] = {ticks + d, f, seq} end)
            end
            local target = findUnique(command.Widget, command.Name)
            if command.Action == "Explore" then
                return describe(resolve(target, command.Path), 24)
            end
            if command.Action == "Button" then
                authorized(runId)
                assert(BUTTONS[command.Widget] and BUTTONS[command.Widget][command.Name], "Button not allowlisted")
                assert(target.Type == "ls.LSButton", "Only a normal button may activate")
                -- A click on a disabled button does nothing, even when its command would accept.
                -- Unset IsEnabled reads as nil: Noesis's default for that property is enabled.
                assert(target.IsEnabled ~= false and target.IsVisible == true, "Button is disabled or hidden")
                local buttonCommand = target.Command
                local parameter = target.CommandParameter
                assert(buttonCommand and buttonCommand:CanExecute(parameter), "Button command unavailable or disabled")
                buttonCommand:Execute(parameter)
                return "Command executed; verify engine events and resources"
            end
            if command.Action == "UseSlot" then
                authorized(runId)
                assert(command.Widget == "HotBar" and command.Name == "HotBar", "UseSlot targets the HotBar widget")
                assert(type(command.Path) == "string" and command.Path:match("^%a+HotBar%.SlotList%.%d+$"), "Invalid slot path")
                local dc = target.DataContext
                local character = dc.CurrentPlayer.SelectedCharacter
                assert(character.EntityUUID == Hero.Uuid(), "Selected character is not the hero")
                local slot = resolve(character.PlayerCharacterProperties, command.Path)
                local spell = slot.Content and slot.Content.PrototypeID
                assert(allowed(spell), "Slot spell not allowlisted: " .. tostring(spell))
                -- CanUse is what enables the slot button for a player.
                if slot.CanUse ~= true then return {Spell = spell, CanUse = false, Executed = false} end
                local useSlot = dc.UseSlotCommand
                local can = useSlot:CanExecute(slot)
                if not can then return {Spell = spell, CanExecute = false, Executed = false} end
                useSlot:Execute(slot)
                return {Spell = spell, CanExecute = true, Executed = true}
            end
            if command.Action == "MoveClick" then
                authorized(runId)
                assert(#pending == 0, "Pointer sequence already in progress")
                assert(command.Widget == "HotBar" and command.Name == "HotBar", "Pointer actions target the HotBar widget")
                local dx, dy = math.tointeger(command.Dx), math.tointeger(command.Dy)
                assert(dx and dy and math.abs(dx) <= 480 and math.abs(dy) <= 300, "Offset out of bounds")
                local character = target.DataContext.CurrentPlayer.SelectedCharacter
                assert(character.EntityUUID == Hero.Uuid(), "Selected character is not the hero")
                assert(character.InTurnBasedMode == true and character.IsCurrentTurn == true, "Not the host's turn")
                assert(character.PlayerCharacterProperties.IsCasting ~= true, "A spell preview is open")
                local x, y = 640 + dx, 360 + dy
                Ext._Internal.InjectMouse(0, 0, false, x, y)
                pending[#pending + 1] = {ticks + 10, function() Ext._Internal.InjectMouse(1, 1, true, x, y) end}
                pending[#pending + 1] = {ticks + 13, function() Ext._Internal.InjectMouse(1, 1, false, x, y) end}
                return {X = x, Y = y, Note = "Click queued; verify position and Movement"}
            end
            if command.Action == "ConfirmCast" or command.Action == "CancelCast" then
                authorized(runId)
                assert(#pending == 0, "Pointer sequence already in progress")
                assert(command.Widget == "HotBar" and command.Name == "HotBar", "Pointer actions target the HotBar widget")
                local spell = activeAllowlistedSlot(target.DataContext)
                -- A centre click only confirms self-targeted spells; targeted ones use ClickWorld.
                assert(command.Action == "CancelCast" or SLOT_SPELLS[spell], "Targeted spells confirm through ClickWorld Target")
                local w, h = screenSize(target.DataContext)
                local x, y = w // 2, h // 2
                local button = command.Action == "ConfirmCast" and 1 or 3
                Ext._Internal.InjectMouse(0, 0, false, x, y)
                pending[#pending + 1] = {ticks + 10, function() Ext._Internal.InjectMouse(1, button, true, x, y) end}
                pending[#pending + 1] = {ticks + 13, function() Ext._Internal.InjectMouse(1, button, false, x, y) end}
                return {Spell = spell, X = x, Y = y, Button = button, Note = "Click queued; verify engine events and resources"}
            end
            if command.Action == "TargetCombatant" then
                -- During an allowlisted targeted spell's preview, the turn-order bar's own
                -- TargetGameobjectCommand with the target's combatant entry (a portrait click).
                -- Needed for stand-ins hidden with SetVisible, which world picking cannot hit.
                authorized(runId)
                assert(command.Widget == "CombatantsOverlay" and command.Name == "CombatantsOverlay", "TargetCombatant targets CombatantsOverlay")
                assert(type(command.Target) == "string" and #command.Target == 36, "Target UUID required")
                local spell = activeAllowlistedSlot(findUnique("HotBar", "HotBar").DataContext)
                assert(TARGET_SPELLS[spell], "Active spell is not a targeted test spell")
                local dc = target.DataContext
                local entry
                for i = 1, 64 do
                    local row = dc.Combatants[i]
                    if row == nil then break end
                    if row.CurrentCombatant and row.CurrentCombatant.EntityUUID == command.Target then entry = row end
                end
                assert(entry, "Target is not in the turn order")
                -- A real portrait hover runs ShowGameobjectInTarget first; the click then runs
                -- TargetGameobjectCommand. Keep that order, a few frames apart.
                assert(#pending == 0, "Pointer sequence already in progress")
                local parameter = entry.CurrentCombatant
                local show, confirm = dc.ShowGameobjectInTarget, dc.TargetGameobjectCommand
                local canShow = show:CanExecute(parameter)
                if canShow then show:Execute(parameter) end
                local seq = sequence
                pending[#pending + 1] = {ticks + 10, function()
                    local follow = {RunId = runId, Sequence = seq, Action = command.Action, OK = true,
                        Picking = pickInfo(), ShowExecuted = canShow}
                    follow.ConfirmCanExecute = confirm:CanExecute(parameter)
                    if follow.ConfirmCanExecute then confirm:Execute(parameter) end
                    pcall(function() dc.HideGameobjectInTarget:Execute(parameter) end)
                    Ext.IO.SaveFile("wow-bg3/action-followup.json", Ext.Json.Stringify(follow))
                end, seq}
                return {Spell = spell, ShowExecuted = canShow, Note = "Follow-up confirms through TargetGameobjectCommand"}
            end
            if command.Action == "CommandState" then
                -- Read-only: what the game's own commands report for a HotBar turn button.
                assert(command.Widget == "HotBar" and BUTTONS.HotBar[command.Name], "CommandState is for HotBar turn buttons")
                local dc = findUnique("HotBar", "HotBar").DataContext
                local out = {}
                local function grab(key, fn) local gok, v = pcall(fn); out[key] = gok and v or ("<" .. tostring(v) .. ">") end
                grab("IsEnabled", function() return target.IsEnabled end)
                grab("ButtonCanExecute", function() return target.Command:CanExecute(target.CommandParameter) end)
                grab("SameAsViewModelEndTurn", function() return target.Command == dc.EndTurn end)
                grab("EndTurnCanExecute", function() return dc.EndTurn:CanExecute(nil) end)
                grab("PlayerCanEndTurn", function() return dc.CurrentPlayer.CanEndTurn end)
                grab("CharacterCanEndTurn", function() return dc.CurrentPlayer.SelectedCharacter.PlayerCharacterProperties.CanEndTurn end)
                return out
            end
            if command.Action == "EndTurnCommand" then
                -- The HotBar view model's own EndTurn command (what the End Turn key invokes), used
                -- when the button's cached IsEnabled disagrees with the game's own CanEndTurn flags.
                authorized(runId)
                assert(command.Widget == "HotBar" and command.Name == "HotBar", "EndTurnCommand targets the HotBar widget")
                local dc = target.DataContext
                assert(dc.CurrentPlayer.IsCurrentTurn == true and dc.CurrentPlayer.CanEndTurn == true, "Game reports the turn cannot end")
                assert(dc.CurrentPlayer.SelectedCharacter.PlayerCharacterProperties.CanEndTurn == true, "Selected character cannot end its turn")
                assert(dc.EndTurn:CanExecute(nil), "EndTurn command unavailable")
                dc.EndTurn:Execute(nil)
                return "EndTurn executed; verify TurnEnded and the next TurnStarted"
            end
            if command.Action == "SelectHost" then
                -- The portrait's own SelectCharacter command, with the disposable character's
                -- entry from the player's assigned characters (what a portrait click passes).
                authorized(runId)
                assert(command.Widget == "PlayerPortraits" and command.Name == "PlayerPortraits", "SelectHost targets PlayerPortraits")
                local dc = target.DataContext
                -- Default: the disposable character. --target may name another character the
                -- player controls in this save (for example Lae'zel, to end her turn normally).
                local want = (type(command.Target) == "string" and #command.Target == 36) and command.Target or Hero.Uuid()
                local found
                for i = 1, 16 do
                    local c = dc.CurrentPlayer.AssignedCharacters[i]
                    if c == nil then break end
                    if c.EntityUUID == want then found = c end
                end
                assert(found, "Not a character assigned to this player")
                local cmd = dc.SelectCharacter
                assert(cmd and cmd:CanExecute(found), "SelectCharacter unavailable")
                cmd:Execute(found)
                return "Select command executed; verify the HotBar's SelectedCharacter"
            end
            if command.Action == "Camera" then
                local ctl, count = activeController()
                local w, h = screenSize(target.DataContext)
                local out = {ActiveCameras = count, Screen = {w, h}, Picking = pickInfo()}
                local function grab(key, fn) local gok, v = pcall(fn); out[key] = gok and v or ("<" .. tostring(v) .. ">") end
                grab("Fov", function() return ctl.FOV end)
                grab("Near", function() return ctl.NearPlane end)
                grab("Far", function() return ctl.FarPlane end)
                grab("Aspect", function() return ctl.AspectRatio end)
                grab("Orthographic", function() return ctl.IsOrthographic end)
                grab("LookAt", function() return vec(ctl.LookAt) end)
                grab("Eye", function() return vec(ctl.WorldTransform.Translate) end)
                grab("RawView", function() return flat(ctl.Camera.ViewMatrix) end)
                grab("RawInvView", function() return flat(ctl.Camera.InvViewMatrix) end)
                grab("RawProjection", function() return flat(ctl.Camera.ProjectionMatrix) end)
                grab("Matrices", function() local mv, mp, layout = M.Matrices(ctl); return {View = mv, Projection = mp, Layout = layout} end)
                grab("Host", function() local p = hostPosition(); return {World = p, Screen = M.Project(p, w, h)} end)
                if command.Target or command.World then
                    grab("Point", function() local p = worldPoint(command); return {World = p, Screen = M.Project(p, w, h)} end)
                end
                return out
            end
            if command.Action == "Hover" or command.Action == "ClickWorld" then
                authorized(runId)
                assert(#pending == 0, "Pointer sequence already in progress")
                assert(command.Widget == "HotBar" and command.Name == "HotBar", "Pointer actions target the HotBar widget")
                local dc = target.DataContext
                local character = dc.CurrentPlayer.SelectedCharacter
                assert(character.EntityUUID == Hero.Uuid(), "Selected character is not the hero")
                local props = character.PlayerCharacterProperties
                local point = worldPoint(command)
                local w, h = screenSize(dc)
                local s = M.Project(point, w, h)
                local x, y = math.floor(s.X + 0.5), math.floor(s.Y + 0.5)
                local safe = safeArea(w, h)
                assert(x >= safe.x0 and x <= safe.x1 and y >= safe.y0 and y <= safe.y1, "Projected point is off the safe screen area")
                local mode = command.Mode
                local wantTarget
                if command.Action == "ClickWorld" then
                    if mode == "Walk" then
                        assert(character.InTurnBasedMode ~= true, "Walk is for real time; use Move in turn-based mode")
                        local hp = hostPosition()
                        local dx, dz = point[1] - hp[1], point[3] - hp[3]
                        assert(math.sqrt(dx * dx + dz * dz) <= WALK_LIMIT, "Walk target too far")
                        assert(props.IsCasting ~= true, "A spell preview is open")
                    elseif mode == "Move" then
                        assert(character.InTurnBasedMode == true and character.IsCurrentTurn == true, "Not the host's turn")
                        assert(props.IsCasting ~= true, "A spell preview is open")
                    elseif mode == "OutOfTurn" then
                        assert(character.InTurnBasedMode == true and character.IsCurrentTurn ~= true, "OutOfTurn needs another combatant's turn")
                        assert(props.IsCasting ~= true, "A spell preview is open")
                    elseif mode == "Target" then
                        local spell = activeAllowlistedSlot(dc)
                        assert(TARGET_SPELLS[spell], "Active spell is not a targeted test spell")
                        assert(type(command.Target) == "string" and #command.Target == 36, "Target mode needs a character UUID")
                        wantTarget = command.Target
                    elseif mode == "Ground" then
                        local spell = activeAllowlistedSlot(dc)
                        assert(POINT_SPELLS[spell], "Active spell is not a point test spell")
                        local hp = hostPosition()
                        local dx, dz = point[1] - hp[1], point[3] - hp[3]
                        assert(math.sqrt(dx * dx + dz * dz) <= WALK_LIMIT, "Ground point too far")
                    else
                        error("Unknown ClickWorld mode")
                    end
                end
                local seq = sequence
                Ext._Internal.InjectMouse(0, 0, false, x, y)
                pending[#pending + 1] = {ticks + 10, function()
                    local pick = pickInfo()
                    local follow = {RunId = runId, Sequence = seq, Action = command.Action, Mode = mode, OK = true,
                        Screen = {x, y}, World = point, Picking = pick, Clicked = false}
                    if command.Action == "ClickWorld" then
                        local hovered = pick.Character1 or pick.Character2
                        local agree
                        if wantTarget then
                            agree = hovered == wantTarget
                        else
                            agree = hovered == nil and pick.Item1 == nil and pick.Item2 == nil
                        end
                        follow.PickAgrees = agree
                        if agree then
                            follow.Clicked = true
                            Ext._Internal.InjectMouse(1, 1, true, x, y)
                            pending[#pending + 1] = {ticks + 3, function() Ext._Internal.InjectMouse(1, 1, false, x, y) end, seq}
                        end
                    end
                    Ext.IO.SaveFile("wow-bg3/action-followup.json", Ext.Json.Stringify(follow))
                end, seq}
                return {Mode = mode, Screen = {x, y}, Projected = s, World = point,
                    Note = "Pointer moved; follow-up reports picking and whether it clicked"}
            end
            error("Unknown harness action")
        end)
        result.OK, result.Value = ok, ok and value or tostring(value)
        Ext.IO.SaveFile("wow-bg3/action-result.json", Ext.Json.Stringify(result))
    end)
end
return M
