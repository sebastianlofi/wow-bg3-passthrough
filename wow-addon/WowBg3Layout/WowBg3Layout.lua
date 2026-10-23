-- WOW-BG3 world mode: BG3's HUD (portraits on the left, turn order at the top, minimap at
-- the top right, hotbar at the bottom) is the game's UI; WoW keeps its chat, menu and bag buttons,
-- moved clear of it, and its dialogs (gossip, quests, loot, vendors) as they are. WoW's unit frames
-- (player, party, target, target of target, combo points) stay hidden, and so does WoW's minimap
-- now: BG3's minimap shows WoW's own minimap art (benilla 0028 writes
-- it, BG3's WowUi.lua puts it under BG3's markers); the mail notice moves next to the bag buttons.
-- Installed by link/tools/play-world.sh for world mode only and parked again when it stops.
-- Chat on and off (for recording): type "/wowbg3 chat" (Enter, the line, Enter
-- from the BG3 window) to hide or show WoW's chat windows, tabs and menu button; "/wowbg3 chat off"
-- or "on" to set it. Typing still works while it is hidden (the edit box is its own frame).
-- play-world.sh WOW_BG3_CHAT=off starts with it hidden (Settings.lua, written there each start);
-- nothing else hides it, and WowBg3Layout_ChatLeft and _ChatBottom (Settings.lua) move it.
-- Other words ("/wowbg3 help", party, tele...) are WOW-BG3 commands: benilla 0053 and its helper answer.

local HIDE = {
    "MainMenuBar", "MultiBarBottomLeft", "MultiBarBottomRight", "MultiBarLeft", "MultiBarRight",
    "PlayerFrame", "PetFrame", "PartyMemberFrame1", "PartyMemberFrame2", "PartyMemberFrame3",
    "PartyMemberFrame4", "CastingBarFrame", "ShapeshiftBarFrame", "PetActionBarFrame",
    "BonusActionBarFrame", "MainMenuExpBar", "ReputationWatchBar", "TutorialFrameAlertButton",
    "TutorialFrame", -- WoW's new-player tips (also turned off for good below).
    -- BG3 shows the target.
    "TargetFrame", "TargetofTargetFrame", "ComboFrame",
    "MinimapCluster", -- BG3's minimap shows WoW's ground.
    "DurabilityFrame", -- the armor figure for damaged gear (hide it).
}
local MICRO = {
    "CharacterMicroButton", "SpellbookMicroButton", "TalentMicroButton", "QuestLogMicroButton",
    "SocialsMicroButton", "WorldMapMicroButton", "MainMenuMicroButton", "HelpMicroButton",
}
local BAGS = {
    "MainMenuBarBackpackButton", "CharacterBag0Slot", "CharacterBag1Slot", "CharacterBag2Slot",
    "CharacterBag3Slot",
}

-- Frames parented to a hidden frame never draw, whoever calls Show on them later.
local hider = CreateFrame("Frame", "WowBg3LayoutHider", UIParent)
hider:Hide()

local function keepHidden(name)
    local f = getglobal(name)
    if not f then return end
    if f:GetParent() ~= hider then f:SetParent(hider) end
    f:Hide()
end

-- Chat on and off (see the header). Hidden chat frames sit under the hider like HIDE's; shown again,
-- they go back to UIParent as they were (a docked window that was not the selected one stays hidden).
local CHAT = {"ChatFrameMenuButton"}
for i = 1, 7 do
    CHAT[table.getn(CHAT) + 1] = "ChatFrame" .. i
    CHAT[table.getn(CHAT) + 1] = "ChatFrame" .. i .. "Tab"
end
local chatHidden, chatWasShown = false, {}
local function setChat(hidden)
    if hidden == chatHidden then return end
    chatHidden = hidden
    for _, name in ipairs(CHAT) do
        local c = getglobal(name)
        if c then
            if hidden then
                chatWasShown[name] = c:IsShown()
                keepHidden(name)
            else
                c:SetParent(UIParent)
                if chatWasShown[name] then c:Show() end
            end
        end
    end
end

-- WoW's main chat window is never hidden unless this session asked for it ("/wowbg3 chat off" or
-- Settings.lua): whatever else hid it (a chat state saved by an earlier session, another addon, a
-- cinematic) loses on the next pass. Only window 1, its tab and the menu button are forced back;
-- another chat window the player closed himself stays closed.
local function showMainChat()
    for _, name in ipairs({"ChatFrame1", "ChatFrame1Tab", "ChatFrameMenuButton"}) do
        local c = getglobal(name)
        if c then
            if c:GetParent() ~= UIParent then c:SetParent(UIParent) end
            if not c:IsShown() then c:Show() end
        end
    end
end

-- Chat's place: its left and bottom edge as fractions of the screen's height. BG3's HUD scales with
-- the height, so the same two numbers hold at 16:9 and at 21:9. Settings.lua may set them.
-- BG3's portrait column ends at 0.081 of the height (58 px of 720, 116 px of 1440, interaction.md);
-- 0.16 puts the chat's left edge two portrait widths in, 115 px at 1280x720 and 230 px at 3440x1440.
-- 0.223 is where the chat has always sat vertically, above BG3's hotbar.
if not WowBg3Layout_ChatLeft then WowBg3Layout_ChatLeft = 0.16 end
if not WowBg3Layout_ChatBottom then WowBg3Layout_ChatBottom = 0.223 end

local function apply()
    for _, name in ipairs(HIDE) do keepHidden(name) end
    if chatHidden then
        for _, name in ipairs(CHAT) do keepHidden(name) end
    end

    -- Menu and bag buttons: top left (WoW's minimap is hidden), reparented off the hidden bar.
    local prev
    for _, name in ipairs(MICRO) do
        local b = getglobal(name)
        if b then
            b:SetParent(UIParent)
            b:ClearAllPoints()
            if prev then
                b:SetPoint("BOTTOMLEFT", prev, "BOTTOMRIGHT", -3, 0)
            else
                b:SetPoint("TOPLEFT", UIParent, "TOPLEFT", 6, -14)
            end
            b:Show()
            prev = b
        end
    end
    prev = nil
    for _, name in ipairs(BAGS) do
        local b = getglobal(name)
        if b then
            b:SetParent(UIParent)
            b:ClearAllPoints()
            if prev then
                b:SetPoint("RIGHT", prev, "LEFT", -2, 0)
            else
                b:SetPoint("TOPLEFT", UIParent, "TOPLEFT", 186, -62)
            end
            b:Show()
            prev = b
        end
    end
    -- The mail notice lived on the minimap: right of the bag buttons.
    if MiniMapMailFrame then
        if MiniMapMailFrame:GetParent() ~= UIParent then MiniMapMailFrame:SetParent(UIParent) end
        MiniMapMailFrame:ClearAllPoints()
        MiniMapMailFrame:SetPoint("TOPLEFT", UIParent, "TOPLEFT", 226, -64)
    end

    -- Buffs: right of the menu buttons (their default corner is under BG3's map).
    if BuffFrame then
        BuffFrame:ClearAllPoints()
        BuffFrame:SetPoint("TOPLEFT", UIParent, "TOPLEFT", 240, -14)
    end
    if TemporaryEnchantFrame then
        TemporaryEnchantFrame:ClearAllPoints()
        TemporaryEnchantFrame:SetPoint("TOPLEFT", UIParent, "TOPLEFT", 240, -60)
    end

    -- Chat: left side, clear of BG3's party portraits and above its hotbar. An anchor offset is in
    -- ChatFrame1's own units, which the client multiplies by the frame's effective scale (it hangs
    -- off UIParent, whose scale is the uiScale CVar, 0.9 by default) to reach the screen root, and
    -- the root is 768 units high at every resolution. A fixed offset is therefore not a fixed part
    -- of the screen, which is what BG3's HUD is: the fractions above are turned into units here.
    -- The old 74 units came to 0.087 of the height once the 0.9 was in, 9 px clear of BG3's
    -- portraits at 3440x1440 and 4 px at 1280x720, which read as sitting on them.
    if ChatFrame1 then
        local s = ChatFrame1.GetEffectiveScale and ChatFrame1:GetEffectiveScale() or 1
        if not s or s <= 0 then s = 1 end
        ChatFrame1:ClearAllPoints()
        ChatFrame1:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT",
            WowBg3Layout_ChatLeft * 768 / s, WowBg3Layout_ChatBottom * 768 / s)
        ChatFrame1:SetWidth(380)
        ChatFrame1:SetHeight(130)
    end
end

-- WoW's world map inside BG3's map frame. benilla calls this when BG3's
-- map panel opens, moves or closes (link/bg3ui.bin, written by BG3's WowUi.lua): mode 0 closed,
-- 1 inside the rect, 2 the whole screen; x, y, w, h are fractions of the screen from the top left.
-- BG3 hides its own (black) map control and the page's background there, and the compositor
-- shows WoW's pixels in the rect, under BG3's banner, legend and waypoint panel. WorldMapFrame
-- (normally anchored to the whole screen; its border art hangs off the frame, the zone map off
-- the 1024 x 768 positioning guide at its centre) is anchored to a black backdrop of our own that
-- covers the rect (WoW's chat and minimap too) and scaled so the guide fits: its size as benilla
-- lays it out gives the scale (harness: computing it from effective scales came out 7% too big;
-- benilla's WorldMapFrame is not a child of UIParent). WorldMapFrame:Show() rather than
-- ShowUIPanel, which would hide UIParent for a fullscreen panel.
local mapBackdrop, mapMeasure = nil, 0
-- The scale at which the 1024 x 768 guide fits the frame, from the frame's size at scale 1 once
-- benilla has laid it out against the backdrop (the frame after the anchoring): measured once per
-- opening, as GetWidth there does not shrink with the frame's own scale.
local function fitMap()
    if mapMeasure <= 0 then return end
    mapMeasure = mapMeasure - 1
    local w, h = WorldMapFrame:GetWidth() or 0, WorldMapFrame:GetHeight() or 0
    if mapMeasure > 0 and not (w > 1 and h > 1) then return end
    mapMeasure = 0
    if w > 1 and h > 1 then WorldMapFrame:SetScale(math.min(w / 1024, h / 768)) end
end
-- The screen in UIParent's units: benilla reports UIParent at 768 high whatever its scale (0.9 in
-- the harness), and draws it scaled, so the screen is WorldFrame's size over UIParent's scale.
local function screenSize()
    if WorldFrame and WorldFrame.GetEffectiveScale then
        local k = WorldFrame:GetEffectiveScale() / UIParent:GetEffectiveScale()
        return WorldFrame:GetWidth() * k, WorldFrame:GetHeight() * k
    end
    return UIParent:GetWidth(), UIParent:GetHeight()
end
function WowBg3Layout_Map(mode, x, y, w, h)
    if not WorldMapFrame then return end
    if mode == 0 then
        if WorldMapFrame:IsShown() then WorldMapFrame:Hide() end
        if mapBackdrop then mapBackdrop:Hide() end
        mapMeasure = 0
        WorldMapFrame:SetScale(1)
        WorldMapFrame:ClearAllPoints()
        WorldMapFrame:SetPoint("TOPLEFT", UIParent, "TOPLEFT", 0, 0)
        WorldMapFrame:SetPoint("BOTTOMRIGHT", UIParent, "BOTTOMRIGHT", 0, 0)
        return
    end
    if mode == 2 then x, y, w, h = 0, 0, 1, 1 end
    local sw, sh = screenSize()
    local rx, ry, rw, rh = x * sw, y * sh, w * sw, h * sh
    if not mapBackdrop then
        mapBackdrop = CreateFrame("Frame", "WowBg3MapBackdrop", UIParent)
        mapBackdrop:SetFrameStrata("HIGH")
        local t = mapBackdrop:CreateTexture(nil, "BACKGROUND")
        t:SetAllPoints(mapBackdrop)
        t:SetTexture(0, 0, 0, 1)
        mapBackdrop:SetScript("OnUpdate", fitMap)
    end
    mapBackdrop:ClearAllPoints()
    mapBackdrop:SetPoint("TOPLEFT", UIParent, "TOPLEFT", rx, -ry)
    mapBackdrop:SetWidth(rw)
    mapBackdrop:SetHeight(rh)
    mapBackdrop:Show()
    WorldMapFrame:SetScale(1)
    WorldMapFrame:ClearAllPoints()
    WorldMapFrame:SetPoint("TOPLEFT", mapBackdrop, "TOPLEFT", 0, 0)
    WorldMapFrame:SetPoint("BOTTOMRIGHT", mapBackdrop, "BOTTOMRIGHT", 0, 0)
    if not WorldMapFrame:IsShown() then WorldMapFrame:Show() end
    mapMeasure = 3
end

-- WoW's panels clear of BG3's party portraits. Gossip, quests, the
-- quest log, vendors, trainers and every other UIPanelWindows panel sit in WoW's left slots
-- (x 0 and 384 from UIParent's left edge, FrameXML UIParent.lua SetLeftFrame, SetCenterFrame,
-- SetDoublewideFrame, MovePanelToLeft, MovePanelToCenter), right on top of BG3's portraits. Each of
-- those functions is wrapped so the panel it placed moves right by WowBg3Layout_PanelOffset
-- (UIParent units: 768 is the screen's height at any size, so the offset follows BG3's HUD, which
-- also scales with the height, at 3440x1440 and 16:9 alike; Settings.lua may set it).
if not WowBg3Layout_PanelOffset then WowBg3Layout_PanelOffset = 96 end
local BLIZZARD_X = {[0] = true, [384] = true}
local function shiftPanel(frame)
    if not frame or not frame.GetPoint then return end
    local point, rel, relPoint, x, y = frame:GetPoint(1)
    if point ~= "TOPLEFT" or not x then return end
    x = math.floor(x + 0.5)
    if not BLIZZARD_X[x] then return end -- already ours (or placed by someone else)
    frame:ClearAllPoints()
    frame:SetPoint("TOPLEFT", "UIParent", "TOPLEFT", x + WowBg3Layout_PanelOffset, y)
end
local function shiftPanels()
    shiftPanel(UIParent.left)
    shiftPanel(UIParent.center)
    shiftPanel(UIParent.doublewide)
end
for _, name in ipairs({"SetLeftFrame", "SetCenterFrame", "SetDoublewideFrame", "MovePanelToLeft", "MovePanelToCenter"}) do
    local orig = getglobal(name)
    if orig then
        setglobal(name, function(a1, a2, a3)
            local r1, r2 = orig(a1, a2, a3)
            shiftPanels()
            return r1, r2
        end)
    end
end

-- WoW's quest tracker (the watched quests) and quest timers at the centre right of the screen
-- instead of their default place. FrameXML hangs them under WoW's minimap
-- (UIParent_ManageFramePositions: TOPRIGHT of MinimapCluster's BOTTOMRIGHT), which BG3's minimap
-- covers in world mode. The tracker's middle sits on the screen's right edge, half way down (it grows
-- both ways), WowBg3Layout_TrackerOffset UIParent units in (768 is the screen's height); the quest
-- timers sit right above it. Placed again after every UIParent_ManageFramePositions.
if not WowBg3Layout_TrackerOffset then WowBg3Layout_TrackerOffset = 40 end
local function placeTracker()
    if QuestWatchFrame then
        QuestWatchFrame:ClearAllPoints()
        QuestWatchFrame:SetPoint("RIGHT", "UIParent", "RIGHT", -WowBg3Layout_TrackerOffset, 0)
    end
    if QuestTimerFrame and QuestWatchFrame then
        QuestTimerFrame:ClearAllPoints()
        QuestTimerFrame:SetPoint("BOTTOMRIGHT", "QuestWatchFrame", "TOPRIGHT", 0, 10)
    end
end
if UIParent_ManageFramePositions then
    local origManage = UIParent_ManageFramePositions
    UIParent_ManageFramePositions = function(a1, a2, a3)
        origManage(a1, a2, a3)
        placeTracker()
    end
end

-- Death the BG3 way (party-multiplayer-design.md "Death"): a WoW
-- character never releases its spirit or walks back from a graveyard; BG3 revives it in place.
-- WoW's death popup (DEATH, "Release Spirit"), the corpse and spirit healer popups that only follow
-- a release, and the spirit healer's XP-loss confirmations never show: StaticPopup_Show refuses
-- them, and one already up (shown before this addon loaded) is hidden at once. Resurrection offers
-- (RESURRECT...) stay: a revive from another player still needs WoW's accept.
local NO_POPUP = {DEATH = true, RECOVER_CORPSE = true, RECOVER_CORPSE_INSTANCE = true,
    AREA_SPIRIT_HEAL = true, XP_LOSS = true, XP_LOSS_NO_SICKNESS = true}
WowBg3Layout_PopupsRefused = 0
if StaticPopup_Show then
    local origShow = StaticPopup_Show
    StaticPopup_Show = function(which, a1, a2, data)
        if which and NO_POPUP[which] then
            WowBg3Layout_PopupsRefused = WowBg3Layout_PopupsRefused + 1
            return nil
        end
        return origShow(which, a1, a2, data)
    end
end
local function hideDeathPopups()
    if not (StaticPopup_Visible and StaticPopup_Hide) then return end
    for which in pairs(NO_POPUP) do
        if StaticPopup_Visible(which) then StaticPopup_Hide(which) end
    end
end

SLASH_WOWBG3LAYOUT1 = "/wowbg3"
SlashCmdList["WOWBG3LAYOUT"] = function(msg)
    local word = string.gsub(string.lower(msg or ""), "^%s+", "")
    word = string.gsub(word, "%s+$", "")
    if word == "chat" then
        setChat(not chatHidden)
    elseif word == "chat off" or word == "chat hide" then
        setChat(true)
    elseif word == "chat on" or word == "chat show" then
        setChat(false)
    else
        -- Every other line goes to benilla (patch 0053), which passes it to the WOW-BG3 command
        -- helper and prints its answer here: list, add, remove, pool, levelup, catchup, respec, tp,
        -- where, find, faction, refill, level, xprate, help.
        if word == "" or word == "help" then
            DEFAULT_CHAT_FRAME:AddMessage("WOW-BG3: /wowbg3 chat on | off - hide or show WoW's chat (\"/wowbg3 chat\" alone toggles)")
        end
        local line = string.gsub(string.gsub(msg or "", "^%s+", ""), "%s+$", "")
        if WowBg3_ChatCommand then
            WowBg3_ChatCommand(line == "" and "help" or line)
        elseif word ~= "" and word ~= "help" then
            DEFAULT_CHAT_FRAME:AddMessage("WOW-BG3: this WoW client has no WOW-BG3 commands (only /wowbg3 chat)")
        end
    end
end

-- Blizzard's code (and saved chat positions) put frames back after login: apply on the events
-- and every two seconds after.
local f = CreateFrame("Frame", "WowBg3LayoutFrame")
local tick = 0
f:RegisterEvent("PLAYER_ENTERING_WORLD")
f:RegisterEvent("UPDATE_CHAT_WINDOWS")
f:RegisterEvent("PLAYER_DEAD")
f:RegisterEvent("CORPSE_IN_RANGE")
local started = false
f:SetScript("OnEvent", function()
    if not started then
        started = true
        if WowBg3Layout_ChatHidden then setChat(true) end
        -- WoW's new-player tips, off for good (a playtest report): the option WoW's interface
        -- settings set (Config.wtf keeps it), and the tips already queued marked as seen.
        if SetCVar then SetCVar("showTutorials", "0") end
        if ClearTutorials then ClearTutorials() end
    end
    if not chatHidden then showMainChat() end
    apply()
    hideDeathPopups()
    placeTracker()
end)
f:SetScript("OnUpdate", function()
    tick = tick + arg1
    if tick >= 2 then
        tick = 0
        if not chatHidden then showMainChat() end
        apply()
        hideDeathPopups()
        shiftPanels()
        placeTracker()
    end
end)
