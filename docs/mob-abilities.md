# Changing mob abilities

Which BG3 abilities a WoW mob gets comes from rules in `WOW-BG3/tools/link/tools/wow_abilities_rules.json`.
To give or take away a mob's spells (a boss fight of your own, a mod pack's), put a file of your own in
`Mods/WOW-BG3/data/abilities-rules.json` (or several in `Mods/WOW-BG3/data/abilities-rules.d/*.json`,
read in name order). It is merged over the shipped rules and survives updates; BG3's next start uses it.
Mobs are keyed by creature entry and spells by WoW spell id (look them up in any vanilla database site);
names are only comments (`_name`). A value is a BG3 spell, `null` (the mob does not get it) or `true` (the
shipped rules decide); a spell the mob does not cast in WoW is given to it. `spells` outside `mobs` applies to
every mob.

```json
{
  "overrides": {
    "mobs": {
      "448": {"_name": "Hogger", "spells": {"6016": null, "8599": {"b": "Shout_Rage", "every": "fight"}}}
    },
    "spells": {"9532": "Projectile_FireBolt"}
  }
}
```

That takes Pierce Armor (6016) from Hogger, gives him BG3's Rage once a fight, named after WoW's Enrage (8599), and
makes every mob's Lightning Bolt (9532) a Fire Bolt. What came of it is in `logs/abilities.log` and
`Mods/WOW-BG3/data/io/link/abilities-status.json`.

