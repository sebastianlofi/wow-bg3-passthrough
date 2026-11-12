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
- WoW item icons, the item table and the quest cursors are made in the background of that first session, a
  minute or two in; until then a WoW item shows its BG3 base's icon. No restart is needed.

- BG3 keeps its list of enabled mods in Steam Cloud, one list for every machine of a Steam account, and it
  turns a mod off by itself when a save made without it is loaded (Continue included). So WOW-BG3 can be off
  in a BG3 you start although you never turned it off. In such a BG3 the mod does nothing: New Game is BG3's
  own, and a WOW-BG3 save asks in BG3's Mod Verification box; tick WoW-BG3 Passthrough there and it loads as
  usual. To have it on for a New Game, enable it in BG3's Mod Manager and start BG3 again.
  When the mod is on, the last line under BG3's main menu reads "WOW-BG3 0.1.0 (build ...)"; without that line
  it is off.

## Picture

- WoW's world is shown in WoW's own colours. BG3's spell glow no longer spills onto WoW ground that the effect
  itself does not cover.
- BG3's characters are shaded as WoW shades its own: as a whole, when the ground they stand on is in a tree's or a
  building's baked shadow. A shadow's edge does not cross a body.
- BG3's characters are lit by WoW's light at WoW's time of day, the same at every camera distance: BG3's own
  close-up character light (the key and rim light it adds when the camera comes near) is off in WoW's world. In a
  dark room a character keeps a small neutral light of its own, so it stays readable; the room itself does not.

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

- Dungeons and raids are entered through WoW's own portals and played inside the same hidden BG3 level as the outdoor
  world: each continent and each instance has a place of its own in it (the Eastern Kingdoms, Kalimdor a floor above
  them, everything else on a third floor). Only the Deadmines has been entered through its portal so far (in and
  out, a fight, a companion, a defeat and a load). The Stockade, one of the dungeons that are buildings only (most
  dungeons, Molten Core, Onyxia's Lair, the Deeprun Tram), has been stood in: its entrance walked and a fight begun.
  Every other instance goes the same way but is untested, and no boss has any hand-made rules. Inside, BG3 offers
  no camp and no Long Rest. A raid needs no raid group; WoW's level minimums, keys and attunements still apply.

- Lifts and the Deeprun Tram carry the party on BG3's own moving platforms, one under each of WoW's cars near the
  party. Such a car keeps BG3's time: in turn-based mode, in a fight or not, it stands while a turn is open and
  moves on the environment's turn (six seconds' worth a round), in WoW's picture as well; a car far from the party
  keeps WoW's own clock. Ridden in the harness: Undercity's lifts (up and down, with a companion, through
  turn-based mode) and the Deeprun Tram (both ways, with a companion, a hold in the tunnel). Limits: a platform's
  floor is round and a little wider than the car's, so at a tram car's ends a character can stand a step beside
  WoW's deck; where a car's floor lies above the ground beside it (the tram's deck, 0.7 m over the station floor)
  the last two metres of ground slope up to it, also while the car is away; the other lifts of the world (Thousand
  Needles, Gnomeregan, Blackrock and so on) use the same code and are untested; a real fight aboard is untested
  (turn-based mode without one is; no hostile creature comes within reach of a lift in the test world). Open: BG3 crashed twice as Undercity's car left (a fight aboard with the old
  catch-up orders; a chained companion aboard), in BG3's own behaviour scripts, cause not found; it has not come
  back in more than twenty later rounds with a companion. A companion added (`add`) while the hero stands on a
  car lands beside the platform's hold and may not be carried. Zeppelins and boats are not ridden: a ship turns,
  a BG3 platform never does, also on a curved path (measured in stock); the guardian at the dock takes the party
  across. A rider on a moving car takes no move order and casts nothing until it stops, and turn-based mode can be
  turned on mid-ride: both are BG3's own rules for its platforms (the same in stock).

- Camp and real long rests are on (`camp = 0` in `settings.conf` turns them off: Go to Camp is greyed out and
  `/wowbg3 refill` stands in for a long rest). The camp is a BG3 level of its own, the mountain pass
  camp (Wyrm's Lookout) whatever the zone: going there and coming back are loading screens (about 6 s there, about
  20 s back while the WoW side comes up again), and the WoW side is down while you are at camp. Food has no use from the bag (as in
  BG3): it is eaten at camp, by a long rest.
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

- Ships and zeppelins are drawn by WoW and cannot be boarded: your dream guardian stands at each dock and takes the
  party to the dock at the other end (a BG3 dialog: "Travel to <place>?"). Crossed by real clicks, one way each, with the guardian standing at the far dock for the way back: the ships
  Menethil Harbor to Auberdine and to Theramore, Auberdine to Rut'theran Village, Ratchet to Booty Bay, and the
  zeppelins Durotar to Tirisfal Glades, Tirisfal Glades to Grom'gol and Grom'gol to Durotar. The ferry between
  Feathermoon Stronghold and the Forgotten Coast is not offered: no place to stand is found at the Forgotten Coast
  end. The party arrives on the guardian's own spot and BG3 moves the two a step apart. The place is named by the
  server's area at the pier, which for some piers is the water beside the town ("The Long Wash, Darkshore" is
  Auberdine's pier). The dialog has no staged camera: the two stand as they stood. The guardian is drawn like a party
  member, so it takes a few seconds to appear the first time. `/wowbg3 cross` is a test command now.

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
- A WoW consumable uses BG3's own effect, not WoW's spell. One elixir at a time, as in BG3: eating a Well
  Fed food can replace a mana potion's elixir and take its spell slot. Dropping or destroying an item from
  BG3 has nowhere to go in WoW 1.12, which has no ground drop.

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

- The zip is about 235 MB, most of it the WoW engine, Python and the Script Extender. The server's world
  database is not in it: the first start downloads it (about 48 MB) and needs the internet that once.
- The first time WOW-BG3 runs, BG3 stays on its loading screen for about two minutes before the main
  menu: it reads the mobs' abilities once a start, and waits while they are made from the database the
  first start downloads (bg3le patch 0008, at most 20 minutes).

## Licences

- bg3le's Script Extender part is under the MIT licence with the Commons Clause: WOW-BG3 releases may
  not be sold, including paid hosting, support or early access.
- Every release must keep its `-sources.tar.gz` next to the zip for as long as the zip is offered.
