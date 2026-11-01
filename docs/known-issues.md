# Known issues in 0.1.0

This is the first public version. What we know is missing or rough:

## Platform

- Linux and the native Linux BG3 from Steam only. No Windows version yet.
- Tested on AMD graphics (Mesa RADV) only. NVIDIA and Intel are untested.
- The local server uses two fixed ports on the loopback interface (3744 and 8105). Another program
  on those ports stops the WoW side from starting.

## First start

- The first New Game after installing waits about three minutes behind the loading screen while
  the server data and the first movement maps are built from your WoW files.
- WoW item icons show from the second BG3 start (BG3 reads its icon atlases only at start; the items
  themselves work at once).

## Picture

- Enemy vision cones show on WoW's ground while you sneak (BG3's own overlay, composited), but their
  edges are ragged: BG3 tests each 0.5 m tile's height, and where WoW's ground is more than about half
  a metre off that height the cone stops. What enemies can see follows BG3's own rules either way.
- Ground surfaces (fire, water, grease) can look matte next to BG3's own levels.
- Some ground is walkable in BG3 where WoW would not let you walk (some steep hills and slopes).
- Some spells' visual effects are missing or only partly drawn on WoW's picture; the spells themselves
  work by BG3's rules.
- WoW's minimap markers are not shown on BG3's minimap, and the world map page has no WoW icons yet.
- BG3's cloth simulation is not carried onto WoW's picture: a garment with its own cloth mesh
  (Gale's robe, Karlach's armour) trails behind the character and can clip. Karlach's body mesh is
  also missing from the capture, so she looks wrong without armour.

## Game

- Camp and real long rests are on (`camp = 0` in `settings.conf` turns them off: Go to Camp is greyed out and
  `/wowbg3 refill` stands in for a long rest). The camp is BG3's Act 3 farm whatever the zone, BG3's exploration
  music stays off there, and the WoW side shows nothing while you are at camp. Eating and drinking from the
  bag, alcohol's Drunk, and a successful pickpocket of food have not been tried in the game yet.
- The camera ignores all collision for now. Walls, roofs, tent cloth, trees, thick posts and the ground open
  in a see-through circle; fences, lamps and other small things do not. Expect odd views, mostly in caves.
- A save made during a fight cannot resume that fight: after the load, BG3's fight from the save is
  ended where everyone stands (no WoW fight runs behind it any more).
- Horde start zones, Teldrassil and Mulgore have had less testing than Northshire and Shadowglen.
- WoW's sound (music, ambience, spells) is on by default but has had little testing; turn it off in
  `settings.conf` (`wow_sound = 0`) if it misbehaves.
- Quitting through BG3's own menu stops the WoW side as BG3 closes; check that
  `~/.local/share/wow-bg3/logs/server.log` ends with `stopped` if you see a problem on the next start.
- Cities have raised floors now, up to four levels a tile, but they are new: heading back across Orgrimmar's
  upper deck can jump, rooms behind tall entrance steps cannot be reached on foot, and fights on a raised
  floor have had little testing.
- Stairs: from a hall, a click on anything at or above your ceiling goes through it to what is underneath, so
  click lower on the stairs. Razor Hill's watchtower top has no walking route: moving up or back teleports. Stairs
  have been walked in only a few towns.
- Fading: ribbons and point lights of a faded prop stay visible.

- Camp: walking far off the camp area counts as leaving: the party goes back to where it left. Tested with
  two party members.
- Camp food: a save from before camp can hold stray single copies of food, and a rest can then take the whole
  stack.

## Levels and progression

- Your WoW levels 1 to 4 each open a party slot; every WoW level from 5 on earns one BG3 level-up (two at
  every 5th), which BG3's own Level Up button offers on every party member with room: level whichever one you
  want, so members sit at different BG3 levels on purpose. BG3's own XP is off; `pool` shows what
  your WoW levels earned and what is left.
- Dismissing a companion does not return its level-ups to the pool. It keeps its BG3 levels and
  rejoins at them, and `pool` still counts those level-ups as given (a dismissed member is listed
  under "not in the party"). Only Withers' respec, `respec NAME`, puts them back.
- BG3's own XP table ends at level 12, so no Level Up is offered past 12 without a level-cap mod.
  WoW 60 is worth 68 level-ups (a party of four at level 18, the cap per character), so without such a
  mod the rest of the pool stays unspent, and a popup says which mods unlock more. No level-cap mod is bundled
  or tested, and we strongly recommend one (README, Levelling); ours needs only a larger `MaxXPLevel` and
  the levels above 12 in BG3's XP table. 5e Spells needs Script Extender; on bg3le that is unknown.
- Companions earn no WoW XP of their own: the pool comes from your hero's WoW level alone.

## Items

- Every WoW item is mapped to a BG3 item by rule, not by hand: a weapon keeps its BG3 base's damage
  die and gains +1 to +3 from its WoW damage per second, body armour gets BG3's own AC for the
  armour type its WoW armour class and item level pick, small pieces give AC(1) from item level 45
  at rare quality, and WoW stats become +1 ability per 8 points (at most +2 an item), with
  resistances from 15 points. A WoW item's BG3 numbers are a translation, not WoW's numbers.
- WoW slots BG3 has none for (shoulders, legs, waist, wrists, shirt, tabard, trinkets, off-hand
  held, relic, ammo) are carried as plain items whose text says "Worn in WoW"; they can only be
  equipped on the WoW side.
- Every item looks like its BG3 base item, not like the WoW item.
- Not mapped yet: random suffixes ("of the Bear"), on-equip effects (attack power, crit), item
  durability and repair, vendor buyback, and bags as BG3 containers. A mapping for suffixes and part of
  the on-equip and on-use effects exists behind a switch, on by default:
  [item-effects.md](item-effects.md).
- A WoW consumable uses BG3's own effect, so the use is not spent in WoW and the item can come back
  on the next sync. Dropping or destroying an item from BG3 has nowhere to go in WoW 1.12, which has
  no ground drop.

## Fights and numbers

- Your party is pure BG3: BG3 health, spells, saves and proficiency. WoW gear only adds AC and
  ability bonuses.
- A WoW mob fights as the nearest BG3 creature and keeps that creature's own BG3 health, at the WoW
  mob's health fraction; only its damage is scaled, by your BG3 health against your WoW character's.
  The weakest mobs therefore hit a little harder than WoW's exact ratio.
- The two level curves are not tuned against each other yet: a BG3 level 1 character out-damages a
  level 4 WoW human.
- Swimming and flying NPCs have no handling of their own, and neither has been tested. BG3 cannot swim:
  deep water is cut as a walkable surface at water level, and a mob's stand-in is placed with a ground
  snap. So a mob under the water or in the air probably stands on the surface or the ground below it,
  away from its WoW model until a fight starts, and clicks or attacks may land on the wrong spot.

## Download size

- The zip is about 270 MB, most of it the WoW engine, the server's world database and Python.

## Licences

- bg3le's Script Extender part is under the MIT licence with the Commons Clause: WOW-BG3 releases may
  not be sold, including paid hosting, support or early access.
- Every release must keep its `-sources.tar.gz` next to the zip for as long as the zip is offered.
