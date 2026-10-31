# Item effects: on-equip, on-use, random suffixes

On by default. Off, `items.json` holds nothing of this mapping and nothing below happens. What has run in the
game and what has not: "Proven" and "Not proven".

## The switch

It works when the item table is built. Any one of these turns it on:

- `wow_items.py build --effects`
- `WOW_BG3_ITEM_EFFECTS=1` in the environment of the first start
- `"enabled": true` under `"effects"` in `wow_items_rules.json`

The first start builds the table only if `items.json` is missing, so an existing install needs a rebuild
by hand. Off again: rebuild without it; a member's added boosts leave at the next sync.

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
- A consumable with an instant WoW heal can pick its BG3 potion by WoW's amount (`heal_tiers`). Empty for
  now, see call 10.

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
| +ranged attack power | 305 | 305 | pool `rap`: RollBonus(RangedWeaponDamage,{n}) per 32, cap 3 |
| +attack power | 291 | 291 | pool `ap`: RollBonus(MeleeWeaponDamage,{n}) per 32, cap 3 |
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
| stat buff (scrolls, elixirs) | 104 | 93 | not mapped |
| open a lock | 104 | 104 | not mapped |
| script | 99 | 98 | not mapped |
| trigger a spell | 84 | 61 | not mapped |
| script event | 79 | 79 | not mapped |
| periodic trigger (well fed and others) | 75 | 75 | not mapped |
| summon a pet | 70 | 70 | not mapped |
| mechanic immunity | 57 | 21 | not mapped |
| activate an object | 56 | 52 | not mapped |
| instant heal | 53 | 53 | not mapped: `heal_tiers` is empty until a use in BG3 reaches WoW |
| temporary weapon enchantment | 52 | 52 | not mapped |
| school damage | 47 | 45 | not mapped |
| restore mana or rage | 45 | 45 | not mapped: BG3 has no mana |
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

## Balance calls still open

Every number below is a proposal in `wow_items_rules.json` (`effects`), and the game runs on them until
The maintainer chooses. "Conservative" marks a number taken low on purpose: when in doubt, gear gives less. The
boosts work in the game; whether the numbers feel right has not been played.

| # | Call | Proposed | Reasoning, and what it does |
|---|---|---|---|
| 1 | Attack power per +1 weapon damage (melee and ranged separately) | 32, cap +3 | From the existing stat rule: 8 strength = +1 STR = +0.5 damage and 2 attack power per strength, so 32 AP = 16 strength = +1 damage. 106 items reach a step alone. |
| 2 | Crit % per natural crit threshold step | 5, cap 2 (crit on 18). **Conservative** | One d20 step is 5%. **It barely bites:** full Bloodfang (epic rogue set) has 4% and gets nothing. 3 would give it a step. Spell crit shares this pool. |
| 3 | Hit % per +1 to hit | 5, cap 2. **Conservative** | As 2. Full Bloodfang has 3% and gets nothing. Spell hit is its own pool on the same numbers. |
| 4 | Spell damage per +1 spell save DC | 40, cap 2 | Budget comparison with the stat rule, not a measurement. Full Netherwind (204) sits at the cap, so 2 is reached well before end game. Only DC, because spell attacks are what spell hit gives. |
| 5 | One-school spell damage counts as any school | yes | "of Fiery Wrath" also raises the DC of frost spells. The other answer is not mapping them (198 effects). |
| 6 | Dodge, parry, block % per +1 AC | 5, cap 2, one pool | Same d20 reasoning. Defense and bonus armour join the pool: 25 defense or 100 armour count as 5%. The 25 and the 100 are estimates. |
| 7 | Run speed | WoW's % as a movement multiplier, cap 15. **Conservative** | Direct, no scale. Only BG3's turn movement. WoW's own sum could be used up to 30. |
| 8 | Resistance from gear in total | 30 points = Resistant, once per school. **Conservative** | Resistant halves a whole school, so the sum takes twice the 15 a single item needs under the fixed-stat rule. |
| 9 | Suffix abilities | 8 points per +1, cap +2 per ability over **all** worn gear | Existing rule and numbers. The cap is the question: a full set "of the Bear" gets the same +2 as one level 60 piece (+29). It comes on top of the fixed stats' +2 per item. |
| 10 | Potion tiers by WoW's heal | Off: `"heal_tiers": []`. **Conservative** | **Stronger potions would be free:** a use in BG3 comes back at the next sync until an `inv use` verb exists. The tiers to turn on (under 300 Healing, 300 Greater, 600 Superior, 1000 Supreme; 32 items change base) are written beside the setting. |

## Not mapped, on purpose

- BG3 has no such thing: healing power, mana and mana regeneration, attack speed, block value.
- Needs a hand-made passive or spell, per item or per family: procs and chance on hit, thorns, health
  regeneration, bonus against a creature type, on-use effects of equipment (trinkets).
- WoW's own systems: class spell tweaks, recipes, enchantments, mounts, pets, keys. These need WoW to run
  them, so the same `inv use` verb.
- Consumable buffs (elixirs, scrolls): BG3's elixirs do different things, so a rule would misdescribe them.

## Proven

In the game (release build, a fresh `items.json`), driving a level 60 Barbarian's items from WoW's side:

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

## Not proven

- In a fight: nothing here has been rolled on, so a raised crit range or DC is only proven to be on the sheet.
- Run speed, hit, attack power and resistances from worn gear: their strings are proven, their items were not
  worn (the test character cannot wear plate or those weapons).
- A companion's worn gear, and a suffix item bought or looted rather than added.
- `OBJ_Potion_Healing_Greater`, `_Superior`, `_Supreme`, if call 10 is turned on, are from memory of BG3's
  data. A missing base makes that item fail to appear in BG3.
- An item that exists when the switch goes on keeps its old BG3 base and text until it is made again.

## Known limits

- The tooltip shows the BG3 base item's text (a bone for "Worn in WoW" pieces), not WoW's text, so a suffix's
  numbers show nowhere in BG3. The code that sets the description is older than this mapping.
- An existing install keeps its `items.json`: the first start builds one only if it is missing.
- Gauntlets on the base `ARM_Gloves_Metal` count as Medium Armour, so a Barbarian's Unarmoured Defence drops
  (AC 13 to 11 in the test). That base rule is older than this mapping.
