# vmangos patches

WOW-BG3's patches to the [vmangos](https://github.com/vmangos/core) server core at commit
`53d24d5c748bef5e0cde7b1155bcb3d979af614c`, applied in name order (checked against `SHA256SUMS`)
by `server/build-portable.sh`. They add the `.wowbg3` console verbs and the server side of BG3
fights, party members and the merged inventory.

| Patch | What it does |
|---|---|
| 0001 fight-handoff | `.wowbg3` verbs; mobs held for a BG3 fight before their first swing; kills credited once; the hero's HP follows BG3's |
| 0002 sight-mode | BG3 decides proximity aggro (for BG3 stealth) |
| 0003 sight-keeps | sight mode kept across arms and resurrection |
| 0004 corpse-pos | a held mob is moved to where BG3's fight left it, so its corpse is lootable |
| 0005 fifty-mobs | up to 50 held mobs per encounter |
| 0006 friendly-attack | a friendly NPC attacked in BG3 turns hostile to that player for the fight |
| 0007 silent-hold, 0008 hidden-stun | the hold without WoW's cast and stun visuals |
| 0009 faction | `.wowbg3 faction horde, alliance or own`: the player counts as that side until logout |
| 0010 party-members | party members as WoW characters (bots): add, remove, move, per-member creature feed, Far Sight, BG3 death rules, movement tiles on demand |
| 0011 inventory | inventory, equipment, loot and vendor verbs |
| 0012 party-fixes | items given between party members; a new bot skips its race intro |
| 0013 inventory-shape | one reply line per inventory verb, for `tools/inventory_bridge.py` |
| 0014 bots-no-xp | party bots take no share of kill XP |
| 0015 packs-and-walk | packs join a fight together; a party bot walks a short server path with its BG3 member |
| 0016 hero-no-fight | `.wowbg3 hero - HP MAX`: the hero's HP follows Tav's outside a fight too, and Tav dying with no fight running kills the hero in place |
| 0017 thaw-walks-on | a creature let go after BG3's turn-based mode walks its patrol or wander on at once (it stood 30 s) |
| 0018 vendor-list-from-afar | `vendor list` answers for a vendor within 100 yd, so BG3 holds its wares before the trade screen opens |
| 0019 any-proficiency | an equip from BG3 first learns the weapon or armour proficiency the item needs, so any item can be worn (for now) |
| 0020 forced-reaction-interact | a forced reaction counts when talking to an NPC, so `faction alliance` or `faction horde` opens that side's merchants |
| 0021 vendor-stock-from-bg3 | `vendor list` adds a ware's maxcount and the vendor's level; `vendor buy` clears WoW's timed counts, since BG3's save holds what was bought since the long rest |
| 0022 join-idempotent | a `join` of mobs already in the encounter drops them and answers `ok`, as `begin`'s resend does |
| 0023 owner-between-maps | the hero between maps (a worldport to another continent) keeps its party bots, their feeds and Far Sight; before, every bot was logged out at the first such move |

Upstream `Unit.cpp` uses CRLF line endings, so some hunks are CRLF: never convert line endings in
these files.

## Licence

vmangos is GPL-2.0 with vMaNGOS' OpenSSL exception. These patches are distributed under the same
licence. Their source and the exact upstream tree they were built against are in every release's
`-sources.tar.gz`.
