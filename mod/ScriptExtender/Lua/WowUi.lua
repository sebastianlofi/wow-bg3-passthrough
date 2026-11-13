-- BG3's GUI in world mode (client context).
--
-- XAML overrides (WoW portraits in BG3's turn order; feasibility only): BG3's own XAML is
-- read through the engine (Ext.IO.LoadFile in the data context), changed as text, saved under
-- the mod's IO folder (Mods/WOW-BG3/data/io) gui/ and put in place of the game's file with Ext.IO.AddPathOverride
-- (bg3le honours it in ls::FileReader's constructor; both paths are made absolute against the Data
-- root, /game/Data in the sandbox, so ours is reached with ../). Nothing of the game's is shipped
-- or written into its data. It must be in before the GUI loads the file, so it runs at bootstrap
-- (bg3le clears overrides at every Lua reset; this adds them again). Harness result: the turn order
-- loads our TurnOrderLib.xaml (a magenta band over every portrait showed), but no image of ours
-- loads: DDS files (DX10 RGBA8 and legacy BGRA8 headers) under new GUI paths and in place of
-- existing GUI textures (Assets and AssetsLowRes) never showed; BG3's GUI textures do not come
-- through that override. So the turn-order probe stays off unless the harness asks for it
-- (the mod's IO folder (Mods/WOW-BG3/data/io) gui/turnorder-probe.txt present); see the world-fights notes.
-- Report: wow-bg3/wowui-status.json.
--
-- The map: BG3's map panel (JournalMap) is black in
-- the void. While it is open in world mode, its map control (WorldMap, an ls.LSWorldMap) is made
-- invisible and click-through and the page's background is cleared, so the compositor shows
-- WoW's frame there, and link/bg3ui.bin ("BUI1", seq, mode, x, y, w, h as fractions of the window,
-- seq again) tells benilla where BG3's map area is: benilla runs the WoW layout addon's
-- WowBg3Layout_Map, which draws WoW's world map there, and writes WoW's depth there as the near
-- plane so BG3's scene never covers it. BG3's banner, legend and waypoint panel stay on top.
-- The minimap: see "WoW's minimap inside BG3's minimap" below.
local M = {}
M.Status = {}
M.Map = {}

local SE_DIR = "Larian Studios/Baldur's Gate 3/Script Extender"

-- The engine's Data root, as Ext.IO.AddPathOverride sees it (it makes both paths absolute against it).
local function dataRoot()
    Ext.IO.AddPathOverride("WowBg3/root-probe", "x")
    local abs = Ext.IO.GetPathOverride("WowBg3/root-probe")
    if type(abs) ~= "string" or abs:sub(-2) ~= "/x" then return nil end
    return abs:sub(1, -3)
end

-- A path relative to the Data root that reaches the absolute path `abs` (a file of ours).
local function fromRoot(root, abs)
    local depth = 0
    for _ in root:gmatch("[^/]+") do depth = depth + 1 end
    return string.rep("../", depth) .. abs:gsub("^/+", "")
end

-- The mod's IO folder as an absolute path (Ext.IO's "wow-bg3/"): Mods/WOW-BG3/data/io, which the launcher plugin
-- maps it to (bg3le patch 0012) and names in WOWBG3_IO; a bg3le without that patch keeps it in its Script Extender folder.
local function ioRoot()
    local io = Ext._Internal.Getenv("WOWBG3_IO")
    if type(io) == "string" and io ~= "" then return io end
    local xdg = Ext._Internal.Getenv("XDG_DATA_HOME")
    if type(xdg) ~= "string" or xdg == "" then
        local home = Ext._Internal.Getenv("HOME")
        if type(home) ~= "string" or home == "" then return nil end
        xdg = home .. "/.local/share"
    end
    return xdg .. "/" .. SE_DIR .. "/wow-bg3"
end

-- Text edits of TurnOrderLib.xaml. Feasibility, first run: a magenta band over every
-- turn-order portrait showed (the override works); our probe image did not. Second run: four
-- images, new and existing GUI paths, DX10 and legacy DDS headers, under a thin band.
local PROBES = {
    {"a", "Assets/WowBg3/probe.png", 12, 19},
    {"b", "Assets/Combatants/WorldTurnPortrait_big.png", 44, 19},
    {"c", "Assets/WowBg3/probe2.png", 76, 19},
    {"d", "Assets/Combatants/btn_small_round_disabled.png", 12, 62},
}
local function turnOrder(xaml)
    local icon = '(<Rectangle x:Name="IconContainer"[^\n]-/>)'
    local extra = '%1\r\n                <Rectangle x:Name="WowBg3Probe" Fill="#C0FF00FF" Width="108" Height="6" Margin="12,13,0,0" HorizontalAlignment="Left" VerticalAlignment="Top" IsHitTestVisible="False"/>'
    for _, pr in ipairs(PROBES) do
        extra = extra .. string.format('\r\n                <Image x:Name="WowBg3Probe%s" Source="pack://application:,,,/Core;component/%s" Stretch="Fill" Width="30" Height="40" Margin="%d,%d,0,0" HorizontalAlignment="Left" VerticalAlignment="Top" IsHitTestVisible="False"/>', pr[1], pr[2], pr[3], pr[4])
    end
    local out, n = xaml:gsub(icon, extra, 1)
    return n == 1 and out or nil
end

local function override(root, se, gamePath, name, text)
    Ext.IO.SaveFile("wow-bg3/gui/" .. name, text)
    local abs = se .. "/gui/" .. name
    local rel = fromRoot(root, abs)
    Ext.IO.AddPathOverride(gamePath, rel)
    return {Game = gamePath, Override = Ext.IO.GetPathOverride(gamePath), Bytes = #text}
end

-- BG3's XP bar on the character sheet: its tooltip speaks of BG3's XP, while the bar shows the way to the next WoW
-- level (Progression.lua, ShowWowProgress). BG3's description goes, these words come in its place. The UI keeps its
-- own copy of the string, so Ext.Loca does not reach the tooltip.
local SHEET = "Public/Game/GUI/Library/CharacterSheetTemplates.xaml"
local function xpTooltip(xaml, text)
    local src = xaml:find("Stats.Level.Description", 1, true)
    if not src then return nil, "the XP bar's description not found in " .. SHEET end
    local open = xaml:sub(1, src):match(".*()<b:Interaction.Behaviors>")
    local _, close = xaml:find("</b:Interaction.Behaviors>", src, true)
    local tag = open and xaml:sub(1, open):match(".*()<TextBlock ")
    if not tag or not close then return nil, "the XP bar's text not found" end
    -- As an attribute: text between the tags came out as an empty tooltip.
    return xaml:sub(1, tag - 1) .. "<TextBlock Text=\"" .. text .. "\" " .. xaml:sub(tag + 11, open - 1) .. xaml:sub(close + 1)
end

-- BG3's map page: the JournalMap widget under the UI root (ContentRoot's children), or nil.
local function journalMap()
    local function name(n) local ok, v = pcall(function() return n.Name end); return ok and v or nil end
    local found
    local function visit(n, depth)
        if not n or depth > 5 or found then return end
        if name(n) == "JournalMap" then found = n; return end
        local okc, cnt = pcall(function() return n.VisualChildrenCount end)
        for i = 1, math.min(okc and cnt or 0, 64) do
            local okv, ch = pcall(function() return n:VisualChild(i) end)
            if okv and ch then visit(ch, depth + 1) end
            if found then return end
        end
    end
    visit(Ext.UI.GetRoot(), 0)
    return found
end
local function prop(n, k) local ok, v = pcall(function() return n:GetProperty(k) end); return ok and v or nil end

-- The page's map control and the template root whose background is black.
local function mapParts(jm)
    local root = jm:VisualChild(1)
    if not root then return nil end
    local okc, cnt = pcall(function() return root.VisualChildrenCount end)
    for i = 1, math.min(okc and cnt or 0, 64) do
        local ch = root:VisualChild(i)
        local ok, nm = pcall(function() return ch.Name end)
        if ok and nm == "WorldMap" then return root, ch end
    end
    return root, nil
end

local function startMap(runId)
    local World = Ext.Require("WorldMode.lua")
    local win = Ext.Json.Parse(Ext.IO.LoadFile("wow-bg3/window.json") or "{}") or {}
    local W, H = tonumber(win.Width) or 1280, tonumber(win.Height) or 720
    local seq, ticks, last, lastAt = 0, 0, nil, 0
    local function write(mode, r)
        seq = seq + 1
        Ext.IO.SaveFile("wow-bg3/link/bg3ui.bin", string.pack("<c4I4I4ffffI4", "BUI1", seq, mode, r[1], r[2], r[3], r[4], seq))
        last, lastAt = mode, ticks
        M.Map = {Mode = mode, Rect = r, Seq = seq}
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 10 ~= 3 then return end
        local ok, err = pcall(function()
            local jm = World.IsActive and World.IsActive() and journalMap() or nil
            local open = jm ~= nil and tostring(prop(jm, "Visibility")) == "Visible"
            if not open then
                if last and last ~= 0 then write(0, {0, 0, 0, 0}) end
                return
            end
            local root, wm = mapParts(jm)
            if not wm then return end
            -- BG3's own map: invisible and click-through; the page background (black) cleared.
            if prop(wm, "Opacity") ~= 0.0 then wm:SetProperty("Opacity", 0.0) end
            if prop(wm, "IsHitTestVisible") ~= false then wm:SetProperty("IsHitTestVisible", false) end
            if prop(root, "Background") ~= nil then root:SetProperty("Background", nil) end
            -- The map area: the control's rows (it ends at the page's bottom), less the waypoint panel
            -- on the right (714 wide, 12 from the edge, in the page's 3840-wide design units).
            local aw, ah = prop(jm, "ActualWidth") or 3840, prop(jm, "ActualHeight") or 2160
            local mh = prop(wm, "ActualHeight") or ah
            local sc = math.min(W / aw, H / ah)
            local ox, oy = (W - aw * sc) / 2, (H - ah * sc) / 2
            local r = {ox / W, (oy + (ah - mh) * sc) / H, (aw - 740) * sc / W, mh * sc / H}
            if last ~= 1 or ticks - lastAt > 60 then write(1, r) end
        end)
        if not ok then M.Map.Error = tostring(err) end
    end)
end

-- WoW's minimap inside BG3's minimap (design: keep BG3's
-- minimap, showing WoW's ground; hide WoW's). BG3's minimap is an ls:LSWorldMap (Minimap.xaml) whose
-- template (DefaultShared.Styles.xaml) draws the level's map (MapImageVT, a virtual texture named by
-- the view model's MapImageUri, 6800 px over the level's 540 m) inside MapHolder, which the control
-- moves, turns and zooms (MapPos, MapRotation, MapScale) so the camera's look-at point sits in the
-- middle; markers (Tav, party, enemies, pings) sit on PART_MarkerHolder from world positions. In the
-- void the level's map is 1.3 km away, so the minimap was black. We add one element to that
-- template, through the same XAML override as the turn order: a layer in MapHolder, above the
-- level's map and below the markers, with one Image whose source is an image benilla writes
-- (link/minimap/mm-<slot>.dds: WoW's own minimap tiles round the player, north up;
-- link/minimap.json says where; the URI carries benilla's run and sequence number after "?") and
-- BG3's GUI loads through the wowbg3 plugin (op 110: BG3's texture provider answers files in that
-- folder; nothing else loads files of ours). WowUi.lua places it in
-- MapHolder's own pixels from the view model (WorldCenter, WorldSize: MapHolder's middle is
-- WorldCenter, one metre is MapImage's width over WorldSize, +x right, +z up) and world mode's
-- mapping, so BG3 turns, zooms and clips it and draws its markers on it with its own code. The layer
-- is in every LSWorldMap (the map page too), invisible until set; only the minimap's is set.
local MAP_STYLES = "Public/Game/GUI/Theme/DefaultShared.Styles.xaml"
-- The image is turned half round in place (a template's transforms are frozen: Lua cannot change
-- them, so the turn is fixed here; world mode's mapping always needs it, see minimapRect).
local MAP_LAYER = table.concat({
    "\r\n",
    "                            <!-- WOW-BG3 (WowUi.lua): WoW's minimap art, placed in MapHolder's own pixels. -->\r\n",
    "                            <Grid x:Name=\"WowMinimapLayer\" IsHitTestVisible=\"False\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\" Width=\"{Binding ElementName=MapImage, Path=ActualWidth}\" Height=\"{Binding ElementName=MapImage, Path=ActualHeight}\">\r\n",
    "                                <TextBlock x:Name=\"WowMinimapSource\" Visibility=\"Collapsed\"/>\r\n",
    "                                <Image x:Name=\"WowMinimapImage\" Source=\"{Binding Text, ElementName=WowMinimapSource}\" Stretch=\"Fill\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\" Width=\"1\" Height=\"1\" Opacity=\"0\" RenderTransformOrigin=\"0.5,0.5\" noesis:Element.PPAAMode=\"Disabled\">\r\n",
    "                                    <Image.RenderTransform>\r\n",
    "                                        <ScaleTransform ScaleX=\"-1\" ScaleY=\"-1\"/>\r\n",
    "                                    </Image.RenderTransform>\r\n",
    "                                </Image>\r\n",
    "                            </Grid>"})

-- The template with our layer right after the level's map (MapImageVT), or nil and why not.
local function mapLayer(xaml)
    local s, e = xaml:find("<Rectangle x:Name=\"MapImageVT\"", 1, true)
    if not s then return nil, "MapImageVT not found in " .. MAP_STYLES end
    local _, ce = xaml:find("</Rectangle>", e, true)
    if not ce then return nil, "end of MapImageVT not found" end
    return xaml:sub(1, ce) .. MAP_LAYER .. xaml:sub(ce + 1)
end

M.Minimap = {}
local YD = 0.9144 -- metres per WoW yard (world mode's mapping, world_bridge.py)
local OP_UI_TEXTURES = 110

local function readJsonFile(path)
    local raw = Ext.IO.LoadFile(path)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

local function nodeName(n) local ok, v = pcall(function() return n.Name end); return ok and v or nil end
local function findNamed(root, want, maxd)
    local found
    local function visit(n, d)
        if not n or d > maxd or found then return end
        if nodeName(n) == want then found = n; return end
        local okc, cnt = pcall(function() return n.VisualChildrenCount end)
        for i = 1, math.min(okc and cnt or 0, 200) do
            local okv, ch = pcall(function() return n:VisualChild(i) end)
            if okv and ch then visit(ch, d + 1) end
            if found then return end
        end
    end
    visit(root, 0)
    return found
end

-- The minimap's control and our layer's parts in it, found fresh (the HUD can be rebuilt).
local function minimapParts()
    local mm = findNamed(Ext.UI.GetRoot(), "Minimap", 8)
    if not mm then return nil end
    local img = findNamed(mm, "WowMinimapImage", 40)
    local src = img and findNamed(mm, "WowMinimapSource", 40)
    local map = img and findNamed(mm, "MapImage", 40)
    if not (img and src and map) then return nil end
    return {Widget = mm, Image = img, Source = src, MapImage = map}
end

-- The image's rectangle in MapHolder's pixels. link/minimap.json: X0..X1 south to north, Y0..Y1
-- east to west (WoW yards); the image's first row is north, its first column west. World mode:
-- BG3 = Centre + (dy, dz, -dx) x 0.9144 from WowOrigin, so WoW's west is BG3's +x and WoW's north
-- BG3's -z: on BG3's map (+x right, +z up) the image shows turned half round (the layer's fixed
-- ScaleTransform -1, -1 about the image's middle). At camp `shift` (BG3 metres x, z) lays it round the
-- hero there: she stands on the spot she left (the WoW body waits there), and it does not move under her.
local function minimapRect(mmj, wj, centre, size, w, h, shift)
    local O, C = wj.WowOrigin, wj.Centre
    local sx, sz = shift and shift[1] or 0, shift and shift[2] or 0
    local xEast = C[1] + sx + (mmj.Y0 - O[2]) * YD
    local xWest = C[1] + sx + (mmj.Y1 - O[2]) * YD
    local zNorth = C[3] + sz - (mmj.X1 - O[1]) * YD
    local zSouth = C[3] + sz - (mmj.X0 - O[1]) * YD
    local kx, kz = w / size[1], h / size[2]
    return {w / 2 + (xEast - centre[1]) * kx, h / 2 - (zSouth - centre[2]) * kz,
        (xWest - xEast) * kx, (zSouth - zNorth) * kz}
end

-- World mode turns BG3's map half round against WoW's (WoW's north is BG3's -z, WoW's west BG3's
-- +x), so BG3's compass letters would name WoW's opposite directions: while world mode runs, the
-- minimap's N and S letters swap images, and E and W (each letter keeps its place and turn, so all
-- still read from outside the ring). BG3's coordinate readout under the map shows the void's
-- coordinates, meaningless in WoW: hidden while world mode runs. Both go back when it stops.
local DIRS = {N = "dir_north", S = "dir_south", E = "dir_east", W = "dir_west"}
local SWAP = {N = "S", S = "N", E = "W", W = "E"}
-- A BitmapImage's URI (GetProperty does not read it; GetAllProperties does). SE's GUI objects are
-- not Lua userdata to type(), so no type test.
local function uriOf(src)
    if src == nil then return nil end
    local ok, all = pcall(function() return src:GetAllProperties() end)
    return ok and type(all) == "table" and type(all.UriSource) == "string" and all.UriSource or nil
end
local function minimapExtras(mm)
    local found = {Dirs = {}}
    local function visit(n, d)
        if not n or d > 40 then return end
        local t = tostring(n)
        if t:find("^Image ") then
            local src = prop(n, "Source")
            local uri = uriOf(src)
            -- A compass letter (any of the four images): which one by its place on the ring, since
            -- its image is swapped while world mode runs.
            if type(uri) == "string" and uri:find("Minimap/dir_", 1, true) then
                local va, ha = tostring(prop(n, "VerticalAlignment")), tostring(prop(n, "HorizontalAlignment"))
                local k = (va == "Top" and "N") or (va == "Bottom" and "S") or (ha == "Right" and "E") or (ha == "Left" and "W")
                if k and not found.Dirs[k] then found.Dirs[k] = {Image = n, Source = src} end
            end
        elseif t:find("^TextBlock ") and not found.Coords then
            -- The readout's text is a MultiBinding (unreadable here): Minimap.xaml's only TextBlock at
            -- opacity 0.8, bottom centre.
            local op = prop(n, "Opacity")
            if type(op) == "number" and math.abs(op - 0.8) < 0.01 and tostring(prop(n, "VerticalAlignment")) == "Bottom"
                and tostring(prop(n, "HorizontalAlignment")) == "Center" then
                found.Coords = n
            end
        end
        local okc, cnt = pcall(function() return n.VisualChildrenCount end)
        for i = 1, math.min(okc and cnt or 0, 200) do
            local okv, ch = pcall(function() return n:VisualChild(i) end)
            if okv and ch then visit(ch, d + 1) end
        end
    end
    visit(mm, 0)
    return found
end
-- WoW's minimap art is about 2 yd a pixel: BG3's own zoom (25 m round Tav by default, 13 to 67 m)
-- would show a few dozen of them. In world mode the minimap's zoom range is WoW's instead (about
-- 30 to 210 m round Tav, 100 m to start; BG3's +/- buttons and wheel step through it as before).
-- MapScale (the holder's zoom) is Scale x 0.3818 here: 10.69 for Scale 28, the radius 270 m / MapScale.
local ZOOM_WORLD = {Scale = 7.0, MinScale = 3.4, MaxScale = 24.0}
local ZOOM_BG3 = {Scale = 28.0, MinScale = 8.0, MaxScale = 40.0}
-- The starting zoom (Scale) is set only when the range changes (world mode on or off, a rebuilt
-- minimap): this runs every check, and setting Scale each time put the player's own zoom back a second
-- after every +/- or wheel step (a playtest report).
local function setZoom(map, z)
    if not map then return end
    local function near(k) local v = tonumber(prop(map, k)); return v ~= nil and math.abs(v - z[k]) < 1e-3 end
    if near("MinScale") and near("MaxScale") then return end
    for _, k in ipairs({"MinScale", "MaxScale", "Scale"}) do
        if not near(k) then map:SetProperty(k, z[k]) end
    end
end
local function worldExtras(mm, on)
    local x = minimapExtras(mm)
    -- Which image each letter has now (from this tree only: a rebuilt HUD has new objects).
    local srcFor = {}
    for _, d in pairs(x.Dirs) do
        local uri = uriOf(d.Source)
        for letter, stem in pairs(DIRS) do
            if type(uri) == "string" and uri:find(stem, 1, true) then srcFor[letter] = d.Source end
        end
    end
    for k, d in pairs(x.Dirs) do
        local want = srcFor[on and SWAP[k] or k]
        local uri = uriOf(d.Source)
        if want and not (type(uri) == "string" and uri:find(DIRS[on and SWAP[k] or k], 1, true)) then
            d.Image:SetProperty("Source", want)
        end
    end
    if x.Coords then
        local vis = on and "Collapsed" or "Visible"
        if prop(x.Coords, "Visibility") ~= vis then x.Coords:SetProperty("Visibility", vis) end
    end
    setZoom(findNamed(mm, "WorldMap", 30), on and ZOOM_WORLD or ZOOM_BG3)
    return x
end

local function startMinimap(runId, s)
    local World = Ext.Require("WorldMode.lua")
    local N = Ext.Require("WowNative.lua")
    local se = ioRoot()
    local MM = M.Minimap
    MM.RunId = runId
    local ticks, shown, lastCheck = 0, nil, -1000
    local function hide(parts)
        if parts and prop(parts.Image, "Opacity") ~= 0.0 then parts.Image:SetProperty("Opacity", 0.0) end
        if parts then worldExtras(parts.Widget, false) end
        shown, MM.Shown = nil, false
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 6 ~= 5 then return end
        local ok, err = pcall(function()
            -- World mode, at camp too: WoW's minimap stays.
            local active = World.Running and World.Running()
            if not active then
                if MM.Shown then hide(minimapParts()) end
                return
            end
            local shift = nil
            if World.InCamp and World.InCamp() then
                if not MM.CampShift then
                    local cs = readJsonFile("wow-bg3/camp-state.json")
                    local from = cs and cs.From
                    local okp, p = pcall(Ext.Require("ActionProbe.lua").HostPosition)
                    if not (type(from) == "table" and okp and type(p) == "table") then return end
                    MM.CampShift = {p[1] - from[1], p[3] - from[3]}
                    shown = nil
                end
                shift = MM.CampShift
            elseif MM.CampShift then
                MM.CampShift, shown = nil, nil
            end
            local mmj = readJsonFile("wow-bg3/link/minimap.json")
            local wj = readJsonFile("wow-bg3/link/world.json")
            if not (mmj and wj and wj.Active and type(mmj.File) == "string" and wj.WowOrigin and wj.Centre) then return end
            local key = tostring(mmj.Run) .. ":" .. tostring(mmj.Seq) .. ":" .. tostring(wj.Time) .. ":" .. mmj.File
            -- A new image or mapping is placed at once; the rest is checked every 5 s (the HUD can be
            -- rebuilt, BG3 can change the level's map, BG3 can install a new texture provider).
            if key == shown and ticks - lastCheck < 150 then return end
            lastCheck = ticks
            local tex = N.Call(OP_UI_TEXTURES, 1)
            MM.Textures = {Err = tex.Err, Installed = tex.R[1], Made = tex.R[2], Sized = tex.R[3], Failed = tex.R[4],
                LastMs = tex.F[2], MaxMs = tex.F[3]}
            if tex.Err ~= 0 then MM.Error = "plugin op 110: " .. tostring(tex.Why); return end
            local parts = minimapParts()
            if not parts then MM.Error = "minimap layer not found"; return end
            local vm = parts.MapImage.DataContext
            local centre, size = vm and prop(vm, "WorldCenter"), vm and prop(vm, "WorldSize")
            local w, h = prop(parts.MapImage, "ActualWidth"), prop(parts.MapImage, "ActualHeight")
            if type(centre) ~= "table" or type(size) ~= "table" or not ((size[1] or 0) > 0) or not ((size[2] or 0) > 0)
                or not ((w or 0) > 0) or not ((h or 0) > 0) then
                MM.Error = "no map frame yet"
                return
            end
            local r = minimapRect(mmj, wj, centre, size, w, h, shift)
            local path = se .. "/link/minimap/" .. mmj.File .. "?" .. tostring(mmj.Run) .. "-" .. tostring(mmj.Seq)
            if prop(parts.Source, "Text") ~= path then parts.Source:SetProperty("Text", path) end
            parts.Image:SetProperty("Margin", {r[1], r[2], 0.0, 0.0})
            parts.Image:SetProperty("Width", r[3])
            parts.Image:SetProperty("Height", r[4])
            if prop(parts.Image, "Opacity") ~= 1.0 then parts.Image:SetProperty("Opacity", 1.0) end
            local x = worldExtras(parts.Widget, true)
            MM.Compass, MM.Coords = 0, x.Coords ~= nil
            for _ in pairs(x.Dirs) do MM.Compass = MM.Compass + 1 end
            shown, MM.Shown = key, true
            MM.Seq, MM.File, MM.Rect, MM.Error = mmj.Seq, mmj.File, r, nil
        end)
        if not ok then MM.Error = tostring(err) end
        if ticks % 150 == 5 then
            pcall(function() Ext.IO.SaveFile("wow-bg3/minimap-status.json", Ext.Json.Stringify(MM)) end)
        end
    end)
end

function M.Start(runId)
    local s = {RunId = runId, State = tostring(Ext.Utils.GetGameState())}
    s.TurnOrderProbe = Ext.IO.LoadFile("wow-bg3/gui/turnorder-probe.txt") ~= nil
    local ok, err = true, nil
    if s.TurnOrderProbe then ok, err = pcall(function()
        local root = assert(dataRoot(), "no Data root (path overrides off?)")
        local se = assert(ioRoot(), "no WOW-BG3 IO folder")
        s.Root, s.SeRoot = root, se
        local game = "Public/Game/GUI/Library/TurnOrderLib.xaml"
        local xaml = assert(Ext.IO.LoadFile(game, "data"), "cannot read " .. game)
        s.TurnOrderBytes = #xaml
        local changed = assert(turnOrder(xaml), "IconContainer not found in " .. game)
        s.TurnOrder = override(root, se, game, "TurnOrderLib.xaml", changed)
        -- Our own images (DDS files in the mod's IO folder (Mods/WOW-BG3/data/io) gui/) where the GUI looks for them.
        local files = {a = "probe.DDS", b = "probe.DDS", c = "probe-legacy.DDS", d = "probe-legacy.DDS"}
        s.Probes = {}
        for _, pr in ipairs(PROBES) do
            local target = fromRoot(root, se .. "/gui/" .. files[pr[1]])
            -- Second run: the existing textures still showed BG3's own. Below 1080p BG3 reads its
            -- GUI art from AssetsLowRes, so both trees get the override.
            for _, tree in ipairs({"Assets/", "AssetsLowRes/"}) do
                local stem = "Public/Game/GUI/" .. tree .. pr[2]:gsub("^Assets/", ""):gsub("%.png$", "")
                Ext.IO.AddPathOverride(stem .. ".DDS", target)
                Ext.IO.AddPathOverride(stem .. ".png", target)
            end
            s.Probes[pr[1]] = Ext.IO.GetPathOverride("Public/Game/GUI/AssetsLowRes/" .. pr[2]:gsub("^Assets/", ""):gsub("%.png$", ".DDS"))
        end
    end) end
    s.OK = ok
    if not ok then s.Error = tostring(err) end
    M.Status = s
    local okm, errm = pcall(startMap, runId)
    if not okm then s.MapError = tostring(errm) end
    -- WoW's minimap in BG3's minimap: the layer goes into BG3's map template before the GUI loads it.
    s.MinimapOff = Ext.IO.LoadFile("wow-bg3/gui/minimap-off.txt") ~= nil
    if not s.MinimapOff then
        local okl, errl = pcall(function()
            local root = assert(dataRoot(), "no Data root (path overrides off?)")
            local se = assert(ioRoot(), "no WOW-BG3 IO folder")
            local xaml = assert(Ext.IO.LoadFile(MAP_STYLES, "data"), "cannot read " .. MAP_STYLES)
            local changed, why = mapLayer(xaml)
            s.MinimapLayer = override(root, se, MAP_STYLES, "DefaultShared.Styles.xaml", assert(changed, why))
        end)
        if not okl then s.MinimapLayerError = tostring(errl) end
        local okmm, errmm = pcall(startMinimap, runId, s)
        if not okmm then s.MinimapError = tostring(errmm) end
    end
    local okx, errx = pcall(function()
        local root = assert(dataRoot(), "no Data root (path overrides off?)")
        local se = assert(ioRoot(), "no WOW-BG3 IO folder")
        local xaml = assert(Ext.IO.LoadFile(SHEET, "data"), "cannot read " .. SHEET)
        local P = Ext.Require("Progression.lua")
        local changed, why = xpTooltip(xaml, string.format("This bar fills toward your next WoW level. WoW levels run to %d. "
            .. "From level %d, each WoW level levels up one party member, and every %dth levels up two. "
            .. "At level %d, a party of four is BG3 level %d.", P.WOW_MAX, P.FIRST_EARN, P.CURVE.Every, P.WOW_MAX, P.CURVE.Cap))
        s.XpTooltip = override(root, se, SHEET, "CharacterSheetTemplates.xaml", assert(changed, why))
    end)
    if not okx then s.XpTooltipError = tostring(errx) end
    Ext.Log.Print("WOW-BG3 ui: XP bar tooltip " .. (okx and "in" or ("failed: " .. tostring(errx))))
    Ext.IO.SaveFile("wow-bg3/wowui-status.json", Ext.Json.Stringify(s))
    Ext.Log.Print("WOW-BG3 ui: " .. (s.TurnOrderProbe and ("turn-order probe " .. (ok and "in" or ("failed: " .. tostring(err)))) or "map link on"))
    Ext.Log.Print("WOW-BG3 ui: minimap layer " .. (s.MinimapOff and "off" or (s.MinimapLayer and "in" or ("failed: " .. tostring(s.MinimapLayerError)))))
end
return M
