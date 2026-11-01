# Item effects: on-equip, on-use, random suffixes

On by default. Off, `items.json` holds nothing of this mapping and nothing below happens. What has run in the
game and what has not: "Proven" and "Not proven".

## The switch

It works when the item table is built. Any one of these turns it on:

- `wow_items.py build --effects`
- `WOW_BG3_ITEM_EFFECTS=1` in the environment of the first start
- `"enabled": true` under `"effects"` in `wow_items_rules.json`

The first start builds the table if `items.json` is missing or of an older schema, so a changed rule needs a
new `ITEMS_SCHEMA`, or a rebuild by hand. Off again: rebuild without it; a member's added boosts leave at the next sync.

## How it works

- A WoW item's on-equip spell becomes points of a kind (`ef` on the row), a random suffix likewise
  (`Props`). The points of everything a member wears in WoW add up, and every `per` points is one step of
  the BG3 boost, at most `cap`. Summing over the worn gear is what lets WoW's small numbers count: most
  single items are below one step.
- The boosts go on the member, through the same call and source as the "Worn in WoW" pieces. So nothing
  here needs a stats entry per suffix, and only WoW's worn list decides.
- A suffix shows in the item's name. It reaches BG3 once the item is in a WoW inventory, because the server
  rolls it then: a vendor ware or corpse item shows the plain name until it is bought or taken. The tooltip
  does not show its numbers (see "Not proven").
- Every Equip, Use and Chance on hit line, mapped or not, is added to the item's text as WoW words it.
- A consumable takes a BG3 potion, elixir or food as its base by what its WoW spell does: see "Consumables".

## Rule table

Generated: `wow_items.py effects`. Count is spell effects on items, Items is distinct items, from the
server's `item_template` (17,707 items, highest patch row) and `spell_template`, and the client's
`ItemRandomProperties.dbc` and `SpellItemEnchantment.dbc`. Test and deprecated items count too.

### On-equip effects (spell trigger 1)

| WoW effect | Count | Items | BG3 |
|---|---:|---:|---|
| +healing | 1019 | 1017 | not mapped: BG3 has no healing power (spells that also give spell damage count through +spell damage) |
| +spell damage | 937 | 928 | pool `sp`: SpellSaveDC({n}) per 40, cap 2, single-school spell damage counts like any school (balance call) |
| +crit % | 384 | 384 | pool `crit`: ReduceCriticalAttackThreshold({n}) per 5, cap 2 |
| mana per 5 s | 344 | 344 | not mapped: BG3 has spell slots, no mana |
| +ranged attack power | 305 | 305 | pool `rap`: the same boost as `ap`, the larger total counts |
| +attack power | 291 | 291 | pool `ap`: CharacterWeaponDamage({n}) per 32, cap 3, every weapon attack |
| +skill (defense, weapon) | 216 | 206 | only defense; weapon skills not mapped |
| +spell crit % | 215 | 215 | pool `crit`: ReduceCriticalAttackThreshold({n}) per 5, cap 2, shares melee crit's pool |
| +hit % | 192 | 192 | pool `hit`: RollBonus(MeleeWeaponAttack,{n});RollBonus(RangedWeaponAttack,{n}) per 5, cap 2 |
| +dodge % | 89 | 89 | pool `avoid`: AC({n}) per 5, cap 2 |
| +spell hit % | 76 | 76 | pool `spellhit`: RollBonus(MeleeSpellAttack,{n});RollBonus(RangedSpellAttack,{n}) per 5, cap 2 |
| +resistance or armour | 74 | 74 | armour into `avoid`, schools into `res_*`, from 30 points (fixed resistances keep their own 15) |
| +run speed % | 62 | 62 | pool `speed`: ActionResourceMultiplier(Movement,{p},0) per 1, cap 15 |
| proc on an event | 54 | 52 | not mapped: hand-made |
| class spell tweak (flat) | 51 | 50 | not mapped: WoW class spells |
| +attack power vs a creature type | 40 | 40 | not mapped: hand-made |
| +ranged attack power vs a creature type | 39 | 39 | not mapped: hand-made |
| health per 5 s | 38 | 38 | not mapped: BG3 has no regeneration boost; needs a hand-made passive |
| spell penetration | 30 | 30 | not mapped |
| +parry % | 28 | 28 | pool `avoid`: AC({n}) per 5, cap 2 |
| +block % | 24 | 24 | pool `avoid`: AC({n}) per 5, cap 2 |
| +ranged haste (quivers) | 21 | 21 | not mapped: BG3 has no attack speed |
| +block value | 21 | 21 | not mapped: BG3 has no block |
| class spell tweak (%) | 16 | 16 | not mapped: WoW class spells |
| thorns (damage shield) | 10 | 10 | not mapped: hand-made |
| everything else | 107 | 99 | not mapped |

### Chance on hit (spell trigger 2)

| WoW effect | Count | Items | BG3 |
|---|---:|---:|---|
| any proc | 233 | 230 | not mapped: hand-made; shown as WoW's text |

### On-use effects (spell trigger 0)

| WoW effect | Count | Items | BG3 |
|---|---:|---:|---|
| teach a recipe or spell | 1676 | 1673 | not mapped |
| create an item | 247 | 247 | not mapped |
| food (health over time) | 194 | 194 | already BG3 food by the existing rule (name, spell category) |
| permanent item enchantment | 134 | 134 | not mapped |
| mount speed | 129 | 129 | not mapped |
| mount | 125 | 125 | not mapped |
| stat buff (scrolls, elixirs) | 104 | 93 | consumables only: see Consumables |
| open a lock | 104 | 104 | not mapped |
| script | 99 | 98 | not mapped |
| trigger a spell | 84 | 61 | not mapped |
| script event | 79 | 79 | not mapped |
| periodic trigger (well fed and others) | 75 | 75 | consumables only, through what it triggers: see Consumables |
| summon a pet | 70 | 70 | not mapped |
| mechanic immunity | 57 | 21 | not mapped |
| activate an object | 56 | 52 | not mapped |
| instant heal | 53 | 53 | `heal_tiers`, see Consumables |
| temporary weapon enchantment | 52 | 52 | not mapped |
| school damage | 47 | 45 | not mapped |
| restore mana or rage | 45 | 45 | see Consumables: mana gives spell slots |
| summon | 41 | 40 | not mapped |
| everything else | 761 | 635 | not mapped |

### Random suffixes

1706 items can roll one, from 780 pools, 1315 properties under 40 names.

| Suffix | Properties | BG3 pool kinds |
|---|---:|---|
| of the Boar | 57 | Strength, Wisdom |
| of the Monkey | 56 | Constitution, Dexterity |
| of the Tiger | 56 | Dexterity, Strength |
| of the Bear | 55 | Constitution, Strength |
| of the Eagle | 55 | Constitution, Intelligence |
| of the Falcon | 55 | Dexterity, Intelligence |
| of the Gorilla | 55 | Intelligence, Strength |
| of the Owl | 55 | Intelligence, Wisdom |
| of the Whale | 55 | Constitution, Wisdom |
| of the Wolf | 55 | Dexterity, Wisdom |
| of Eluding | 38 | Dexterity, avoid |
| of Regeneration | 37 | name and text only |
| of Blocking | 35 | Strength, avoid |
| of Healing | 31 | name and text only |
| of Fiery Wrath | 30 | sp |
| of Intellect | 30 | Intelligence |
| of Nature's Wrath | 30 | sp |
| of Stamina | 30 | Constitution |
| of Agility | 29 | Dexterity |
| of Arcane Wrath | 29 | sp |
| of Concentration | 29 | name and text only |
| of Frost Resistance | 29 | res_cold |
| of Frozen Wrath | 29 | sp |
| of Nature Resistance | 29 | res_poison |
| of Power | 29 | ap, rap |
| of Shadow Resistance | 29 | res_necrotic |
| of Shadow Wrath | 29 | sp |
| of Spirit | 29 | Wisdom |
| of Strength | 29 | Strength |
| of Arcane Resistance | 28 | res_force |
| of Defense | 28 | avoid |
| of Fire Resistance | 28 | res_fire |
| of Holy Wrath | 28 | sp |
| of Marksmanship | 26 | rap |
| of Toughness | 14 | avoid |
| of Beast Slaying | 7 | name and text only |
| of Restoration | 7 | Constitution |
| of Sorcery | 7 | Constitution, Intelligence, sp |
| of Striking | 7 | Constitution, Dexterity, Strength |
| of Retaliation | 1 | name and text only |

In numbers the table merges:

- +skill: defense 162, into `avoid` (25 defense = 5 points); weapon skills 54, not mapped.
- +spell damage: every school 738, one school 198, physical only 1 (not mapped).
- 27 of the 1,706 items that can roll a suffix also have an on-equip spell, 12 also have fixed stats.
  Those fixed stats keep their own per-item rule; they do not add to the suffix's points.
- Of 2,012 properties in the client, 1,315 are reachable on this server. Alone, 669 of those reach no step, 105 of them because they have no mapped kind at all.

## Consumables

Generated: `wow_items.py consumables`, rules in `wow_items_rules.json` (`effects.consumables`). The first rule that
matches wins, on the item's use spell and the spells it triggers (Well Fed hangs behind Food's trigger). 221 of 994
consumables match, deprecated and test items counted. The rest have no BG3 equivalent or need WoW itself: they stay
plain items with WoW's text: weapon oils and poisons, enchantments, recipes, fireworks, grenades, costumes, soulstones,
reflection, +healing.

| WoW effect | Items | Examples | BG3 base (from WoW's points) |
|---|---:|---|---|
| restore mana (with health too) | 21 | Mana Potion, Major Rejuvenation Potion | Elixir of Arcane Cultivation: `ALCH_Solution_Elixir_Meditation` level 1 slot, `_Greater` level 2 from 500, `_Superior` level 3 from 1000, `_Supreme` level 4 from 1500 |
| restore rage | 3 | Rage Potion, Mighty Rage Potion | `ALCH_Solution_Elixir_Bloodlust` |
| restore energy | 1 | Thistle Tea | `ALCH_Solution_Elixir_Alertness` |
| immunity | 4 | Light of Elune, Flask of Petrification | `OBJ_Potion_Of_Invulnerability` |
| dreamless sleep | 2 | Dreamless Sleep Potion | `ALCH_Solution_Potion_Rest_Lesser`, `_Greater` from 500 |
| +armour | 13 | Elixir of Defense, Greater Stoneshield Potion | `ALCH_Solution_Elixir_Barkskin` |
| +defense | 2 | Buttermilk Delight | `ALCH_Solution_Elixir_Barkskin` |
| resist all magic | 5 | Magic Resistance Potion, Flask of Chromatic Resistance | `OBJ_Potion_Of_Force_Resistance`, `OBJ_Potion_Of_Invulnerability` from 50 |
| resist or absorb one school | 13 | Greater Fire Protection Potion | `OBJ_Potion_Of_*_Resistance`: holy Radiant, fire Fire, nature Poison, frost Cold, shadow Necrotic, arcane Force |
| free action | 2 | Free Action Potion | `ALCH_Solution_Elixir_Freedom` |
| invisibility | 2 | Lesser Invisibility Potion | `OBJ_Potion_Of_Invisibility` |
| +strength | 15 | Elixir of Giants, Smoked Desert Dumplings | `ALCH_Solution_Elixir_Alertness`, `ALCH_Solution_Elixir_Enlarge` from 8, `OBJ_Potion_Of_Hill_Giant_Strength` from 25 |
| +stamina or max health | 37 | Elixir of Fortitude, Monster Omelet | `ALCH_Solution_Elixir_Alertness`, `OBJ_Potion_Of_Heroism` from 8 |
| +agility | 13 | Elixir of Agility, Grilled Squid | `ALCH_Solution_Elixir_Alertness`, `ALCH_Solution_Elixir_Criticals` from 8 |
| +intellect | 10 | Elixir of Greater Intellect | `ALCH_Solution_Elixir_Alertness`, `ALCH_Solution_Elixir_ArcaneAcuity` from 8 |
| +spirit | 8 | Gizzard Gum, Hot Smoked Bass | `ALCH_Solution_Elixir_Alertness`, `ALCH_Solution_Elixir_Concentration` from 8 |
| +spell damage | 9 | Arcane Elixir, Flask of Supreme Power | `ALCH_Solution_Elixir_ArcaneAcuity` |
| +attack power | 2 | Winterfall Firewater | `ALCH_Solution_Elixir_Bloodlust` |
| +run or attack speed | 7 | Swiftness Potion, Fishliver Oil | `OBJ_Potion_Of_Speed` |
| +swim speed | 1 | Swim Speed Potion | `ALCH_Solution_Potion_Jump` |
| water breathing | 2 | Elixir of Water Breathing | `ALCH_Solution_Elixir_Darkvision` |
| detect | 4 | Catseye Elixir | `ALCH_Solution_Elixir_SeeInvisible` |
| dispel | 10 | Jungle Remedy, Purification Potion | `ALCH_Solution_Potion_Remedy` |
| fire proc | 1 | Dragonbreath Chili | `OBJ_Potion_Of_Fire_Breath` |
| heal over time | 30 | Linen Bandage, Troll's Blood | `OBJ_Potion_Healing` |
| mana over time | 4 | Mageblood Potion | `ALCH_Solution_Elixir_Meditation` |
| instant heal (`heal_tiers`, not a rule) | | Healing Potion, Whipper Root Tuber | `OBJ_Potion_Healing`, `_Greater` from 300, `_Superior` from 600, `_Supreme` from 1000 |

How it behaves:

- **Mana is BG3's own potion.** Its status `ALCH_ELIXIR_MEDITATION*` is `ActionResource(SpellSlot,1,N)`, and its template
  applies it with duration -1, so the slot lasts until long rest. Read from BG3's stats and root templates.
- **One elixir at a time.** Most of these share BG3's stack `ALCH_ELIXIR`, Heroism and Hill Giant Strength too: a new one
  replaces the old, also a spent mana slot's status. WoW stacks a food buff with elixirs; BG3 does not.
- **Food and drink with a camp supply** keep their supply and price, and follow a rule only with `food_from`, so from 8
  points of a stat (one step of `ability_per`), and the chili. Below that, and for food that regenerates health or mana,
  they stay BG3 food. A buffed food does not heal when eaten: its base is the potion.
- **Water and alcohol keep their rules**, a drink that makes WoW drunk too (Rumsey Rum). An elixir named for water is no
  water.
- **What WoW makes for free stays at the first tier**: healthstones and mana gems, so a mage's gem is never a level 4 slot.
- A stat elixir below 8 points is `Alertness` (+5 initiative): too small for a stat, still something.
- **Spending:** every usable consumable that is no camp supply has `"us": 1` on its row, and `Inventory.lua` spends a use
  in WoW as it does a camp supply. During a long rest and 30 s after it, such a stack's drop is no spending: it is put back
  from WoW's list (see "Known limits").

## Balance calls

Each is taken as proposed, as the recommended answer, and is in `wow_items_rules.json` (`effects`); the maintainer can
overrule any. "Conservative" marks a number taken low on purpose: when in doubt, gear gives less. Whether the
numbers feel right has not been played.

| # | Call | Proposed | Reasoning, and what it does |
|---|---|---|---|
| 1 | Attack power per +1 weapon damage | 32, cap +3, melee and ranged one boost | From the existing stat rule: 8 strength = +1 STR = +0.5 damage and 2 attack power per strength, so 32 AP = 16 strength = +1 damage. 106 items reach a step alone. BG3 cannot split melee from ranged here (see "Proven"), so ranged-only attack power also raises melee. |
| 2 | Crit % per natural crit threshold step | 5, cap 2 (crit on 18). **Conservative** | One d20 step is 5%. **It barely bites:** full Bloodfang (epic rogue set) has 4% and gets nothing. 3 would give it a step. Spell crit shares this pool. |
| 3 | Hit % per +1 to hit | 5, cap 2. **Conservative** | As 2. Full Bloodfang has 3% and gets nothing. Spell hit is its own pool on the same numbers. |
| 4 | Spell damage per +1 spell save DC | 40, cap 2 | Budget comparison with the stat rule, not a measurement. Full Netherwind (204) sits at the cap, so 2 is reached well before end game. Only DC, because spell attacks are what spell hit gives. |
| 5 | One-school spell damage counts as any school | yes | "of Fiery Wrath" also raises the DC of frost spells. The other answer is not mapping them (198 effects). |
| 6 | Dodge, parry, block % per +1 AC | 5, cap 2, one pool | Same d20 reasoning. Defense and bonus armour join the pool: 25 defense or 100 armour count as 5%. The 25 and the 100 are estimates. |
| 7 | Run speed | WoW's % as a movement multiplier, cap 15. **Conservative** | Direct, no scale. Only BG3's turn movement. WoW's own sum could be used up to 30. |
| 8 | Resistance from gear in total | 30 points = Resistant, once per school. **Conservative** | Resistant halves a whole school, so the sum takes twice the 15 a single item needs under the fixed-stat rule. |
| 9 | Suffix abilities | 8 points per +1, cap +2 per ability over **all** worn gear | Existing rule and numbers. The cap is the question: a full set "of the Bear" gets the same +2 as one level 60 piece (+29). It comes on top of the fixed stats' +2 per item. |
| 10 | Potion tiers by WoW's heal | On: under 300 Healing, 300 Greater, 600 Superior, 1000 Supreme | Bases read from BG3's stats. |
| 11 | Mana per spell slot level | 0 level 1, 500 level 2, 1000 level 3, 1500 level 4 | Puts Minor and Lesser at 1, Mana and Greater at 2, Superior at 3, Major at 4. A low level character has no use for a slot above its own. Rejuvenation counts as mana; its health is lost. |
| 12 | Well Fed | From 8 points a stat elixir's effect, below that plain food | **Heroism on food may be strong**: 21 foods, 10 temporary HP and 1d4 on attacks and saves for the price of a meal. The other answer is raising `food_from`. |
| 13 | Rage, energy | Bloodlust (a kill gives temporary HP and an action), Alertness. **Conservative for energy** | BG3 has no potion for a class's resource. Haste would fit energy better, and is too much for a 15th level recipe. |
| 14 | Swiftness | `OBJ_Potion_Of_Speed` (Haste) | **Strong for its price**: WoW's +50% run speed at item level 15 becomes BG3's Haste, because nothing else in BG3's potions is speed. |
| 15 | Giant Strength | From 25 points | Strength 21 outright, so it means much at low level. Elixir of Giants, R.O.I.D.S. |

## Not mapped, on purpose

- BG3 has no such thing: healing power, mana and mana regeneration, attack speed, block value.
- Needs a hand-made passive or spell, per item or per family: procs and chance on hit, thorns, health
  regeneration, bonus against a creature type, on-use effects of equipment (trinkets).
- WoW's own systems: class spell tweaks, recipes, enchantments, mounts, pets, keys. These need WoW to run
  them, so the same `inv use` verb.
- Consumables without a BG3 equivalent: see "Consumables". A chosen elixir does what BG3's elixir does, not what WoW's
  text says: the item's text still shows WoW's words.

## Proven

In the game (release build, a fresh `items.json`), driving a level 60 Barbarian's items from WoW's side.

In a fight, axe 1d6+3:

- Worn attack power of 160 gives `CharacterWeaponDamage(3)`. Without it hits did 6, 6, 7, 6; with it 9, 9, 8, 10, 10, 10.
  Gear off, the boost leaves.
- What was tried first did nothing: `RollBonus(MeleeWeaponDamage,3)` showed on the character but a hit still did 4.
  A condition cannot separate melee from ranged: with `CharacterWeaponDamage(40)`, `IF(IsMeleeWeaponAttack())`,
  `IF(IsMeleeAttack())` and `IF(IsAttackType(AttackType.MeleeWeaponAttack))` hit for 8, 9 and 5, no condition for 46, and
  `IF(not IsRangedWeaponAttack())` for 46. So through `Osi.AddBoosts` an attack condition reads false.

On the character, from WoW's worn list:

- Attack power and hit from worn trinkets, rings and a cloak; +1 to hit from 6% summed over four items.
- Run speed: 15% takes movement from 9.0 to 10.35.
- Resistance: 32 frost points over three items give Resistant to Cold, 22 do not.
- Armour: AC up by 2 from an item of 10 points; spell hit +1 from 5% over three items.

A suffix item from a kill: a boar with the item added to its loot table dropped it, the server rolled it (`Rand` 757),
WoW's loot verb took it, BG3 shows it worn as "Pioneer Tunic of the Owl". No vendor on this server sells an item that can
roll a suffix, so buying one cannot happen.

Earlier runs:

- Every boost string the rules can make, 21 in all, once on the hero: BG3 took each and took each off again.
- On equip and off again from WoW: `ReduceCriticalAttackThreshold(2)`, `SpellSaveDC(2)`, and Dexterity and
  Constitution +2 from a suffix. The character sheet shows the abilities and AC follow.
- Three effect items together keep all their boosts, and a swap between suffixes changes only those boosts.
- 7 of 7 server rolls on one item show the right suffix name, in the tooltip too. All 67,679 suffix names and
  17,707 names are there after a load.
- Save, reload: same items, no duplicates, names and boosts back, each boost once.

Three bugs came out of this, all fixed: a boost removed and added in one tick was lost, a load lost the boosts
while their variable stayed, and strings made by client and server at the same moment were lost (a second pass
at the start of play makes them).

Consumables, on a level 1 Wizard with 2 level 1 slots, each used from BG3 and read back from BG3 and from WoW's list:

| Item | BG3 got | BG3 stack | WoW's count |
|---|---|---|---|
| Minor Mana Potion | `ALCH_ELIXIR_MEDITATION`, level 1 slots 2 to 3 | 2 to 1 | 2 to 1 |
| Mana Potion | `_GREATER`, level 2 slots 0 to 1 | 2 to 1 | 2 to 1 |
| Major Mana Potion | `_SUPREME`, level 4 slots 0 to 1 | 2 to 1 | 2 to 1 |
| Monster Omelet (Well Fed) | `POTION_OF_HEROISM` and its temporary hit points | 2 to 1 | 2 to 1 |
| Grilled Squid (Well Fed) | `ALCH_ELIXIR_CRITICALS` | 2 to 1 | 2 to 1 |
| Elixir of Giants | `POTION_OF_STRENGTH_HILL_GIANT` | 2 to 1 | 2 to 1 |
| Elixir of Greater Agility | `ALCH_ELIXIR_CRITICALS` | 2 to 1 | 2 to 1 |
| Greater Fire Protection Potion | `POTION_OF_RESISTANCE_FIRE` | 2 to 1 | 2 to 1 |
| Rage Potion | `ALCH_ELIXIR_BLOODLUST` | 2 to 1 | 2 to 1 |
| Thistle Tea | `ALCH_ELIXIR_ALERTNESS` | 2 to 1 | 2 to 1 |

- A long rest ends the mana slot: level 2 slots 1 to 0, the status gone. It stays through the trip to camp until then.
- A real click on the hotbar's Major Mana Potion, then on the portrait, did the same as a use by script: slot, stack, WoW.
- A new elixir replaces the old and takes its slot with it: level 4 slots 1 to 0 on eating the Omelet.
- Rests spend food only, also buffed food: one Omelet of two, 2 Squid and 2 bread.
- The guard: with all 9 potion stacks of a member deleted as the rest finished, all were back within 3 s, WoW's counts
  unchanged, no `del` sent. A drink before the rest and one 90 s after it were spent as usual.

## Not proven

- Consumables: the tooltip; a consumable in a fight; the other rules of the table (only the kinds above ran).
- In a fight only weapon damage was measured. Hit, crit range, DC, AC and resistance from gear are on the character, not
  measured on a roll.
- Attack power on a ranged weapon: the test character has none. BG3's own Sharpshooter uses the same boost.
- A companion's worn gear.
- An item that exists when the switch goes on keeps its old BG3 base and text until it is made again.

## Known limits

- **A member's stacks can vanish at the end of a long rest.** Seen in 2 of 9 rests, about 2 s after `LongRestFinished`: all of one
  member's food and potion stacks at once, another member's kept, and with them WoW's copies. Cause not caught: no rest with the
  call log on showed it, and no setup brings it on at will. Potions and elixirs now come back (`restGuard`, `Inventory.lua`).
  **Food in such a stack is still lost**, since a rest spends food and the two look alike.

- The tooltip shows the BG3 base item's text (a bone for "Worn in WoW" pieces), not WoW's text, so a suffix's
  numbers show nowhere in BG3. The code that sets the description is older than this mapping.
- Gauntlets on the base `ARM_Gloves_Metal` count as Medium Armour, so a Barbarian's Unarmoured Defence drops
  (AC 13 to 11 in the test). That base rule is older than this mapping.
