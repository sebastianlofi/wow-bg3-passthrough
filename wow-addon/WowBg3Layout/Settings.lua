-- WOW-BG3 world layout settings. link/tools/play-world.sh writes this file at each start (in its
-- copy of the addon): WOW_BG3_CHAT=off sets WowBg3Layout_ChatHidden, which hides WoW's chat at login.
WowBg3Layout_ChatHidden = false

-- WoW's chat window on the left: its left and bottom edge as fractions of the screen's height
-- (BG3's HUD scales with the height, so the same numbers fit 16:9 and 21:9). BG3's party portraits
-- end at 0.081 of the height; 0.16 leaves two portrait widths of room. Uncomment to change them.
-- WowBg3Layout_ChatLeft = 0.16
-- WowBg3Layout_ChatBottom = 0.223
