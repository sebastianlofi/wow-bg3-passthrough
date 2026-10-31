//! WoW-side fight bridge (first Young Wolf pulls, then every world fight). It turns
//! benilla's pull, join and input files into protocol v1 messages for the BG3 mod
//! (`LinkLive.lua`, void fights) and BG3's answers into one state file benilla plays on its
//! models. It applies no game rules: BG3 decides turns, paths, hits and HP.
//!
//! Files in the link directory (BG3's private `wow-bg3/link`, where benilla also streams
//! `camera.bin`):
//! - `pull.json` (benilla): `{"seq", "map", "hero": UNIT, "enemies": [UNIT, ...]}` (older benilla:
//!   one `"enemy"`), UNIT = `{guid, entry, name, level, pos, hp, max_hp}`, WoW yards; a new `seq`
//!   starts a fight with every mob in it (the one attacked or aggroing and the rest of its pack).
//! - `join.json` (benilla): `{"seq", "enemies": [UNIT, ...]}`: mobs that entered the fight in WoW
//!   while it runs (adds that aggro, or a mob attacked); a new `seq` sends them to BG3 as a Join.
//! - `input.json` (benilla): `{"seq", "kind": "move"|"cast"|"end_turn"|"leave", "target": [x, y, z],
//!   "spell": "FireBolt"|"Attack", "target_guid": "16 hex"}`; read on the hero's turn only.
//! - `standins.json` (bridge, read by the mod at Begin and Join): the stand-in of every enemy id
//!   and the WoW numbers it is scaled from (`StandIn`, `spec`).
//! - `fight.json`, `fight.txt` (bridge): the fight as WoW should show it (positions in WoW yards,
//!   BG3 HP, paths, enemy attacks with hit or miss), rewritten after every change.
//! - `server-cmd.txt`, `handoff.jsonl` (bridge): server handoff requests (`crate::handoff`).
//! - `server-events.txt` (benilla): the server's `WOWBG3 ...` system lines as `N LINE`; an end by
//!   timeout or player death ends the BG3 side, refusals and releases are logged.
//! - `server-feed.txt` (benilla 0046): the per-member creature feed's `WOWBG3 cr|crgone|tick|near`
//!   lines, same `N LINE` form (N counts on across both files), kept apart so the feed's traffic
//!   never pushes a fight's lines out of `server-events.txt` (`Mirror::feed_event`).
//! - `mobs.txt`, `mirror.json`, `mirror-status.json`, `mirrored.txt`: the mirrored mobs
//!   (`crate::mirror`). In world mode every attackable mob near the hero has a BG3 stand-in all the
//!   time; BG3 combat with one starts a fight here (as a pull does) and BG3 pulling more of them in
//!   makes them join. Turn-based mode outside a fight is TurnHold.lua's (the server's hold verbs);
//!   the bridge's old turn-based freeze (`begin tb-...`) is gone.
//!
//! The arena comes from `link/tools/wow_arena.py` around the hero and goes into the BG3 IO
//! directory; `start.json` tells the mod where it lies.
use crate::{
    Endpoint, Error, Role,
    coords::{Basis, Point},
    handoff::{Handoff, Ledger, ServerHandoff},
    mailbox::Mailbox,
    mirror::{Mirror, WowUnit},
    wire::{Actor, Message, Outcome, decode, id_ok},
};
use serde_json::{Value, json};
use std::{
    collections::{HashSet, VecDeque},
    fs,
    io::Read,
    os::unix::fs::PermissionsExt,
    path::{Path, PathBuf},
    process::Command,
    thread,
    time::{Duration, Instant},
};

/// Enemies one encounter takes (pull plus joins); the mod's stand-in limit matches.
pub const MAX_ENEMIES: usize = 50;
const MAX_ANIMS: usize = 24;
const MAX_LOG: usize = 12;

#[derive(Clone, Debug)]
pub struct Options {
    pub link: PathBuf,
    pub io: PathBuf,
    pub project: PathBuf,
    /// The wolf stand-in's root template (`--template`; BG3's plain "Wolf" by default).
    pub template: String,
    pub faction: Option<String>,
    /// The faction of neutral mobs' stand-ins (attackable, never aggressive).
    pub neutral_faction: String,
    /// World mode: mirror the mobs near the hero as BG3 stand-ins (`crate::mirror`).
    pub mirrors: bool,
    pub centre: Point,
    pub size: f64,
    pub follow: bool,
    /// Play the hero from a fixed script instead of benilla input (headless tests).
    pub script: bool,
    /// Script mode: pull at this WoW point with the enemies a few yards east.
    pub pull_at: Option<(f64, f64, f64)>,
    /// Script mode: enemies in the pull, and adds that join on the hero's second turn.
    pub script_enemies: u32,
    pub script_adds: u32,
    /// WoW creature numbers (`link/tools/creature_stats.py`); without them no damage scaling.
    pub stats: PathBuf,
    pub max_seconds: u64,
}

/// A BG3 stand-in kind (world fights): a BG3 root template with attacks like the WoW
/// mob's, and the average damage of its basic attack from BG3's stats. The mod adds
/// `DamageBonus(WoW average hit x ratio - base_damage)`, so a BG3 hit does WoW's damage, scaled.
/// Its health is the template's own BG3 max HP (no HP scaling), at the WoW
/// mob's HP fraction (`WowHp` / `WowMaxHp` in its [`spec`]).
/// Verified live: the wolf (Target_Bite_Wolf 2d4 + Dex 15, bites of 7 in early tests). The others come
/// from BG3's stats and 5e values, not yet seen in a fight.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StandIn {
    pub kind: &'static str,
    pub template: &'static str,
    pub base_damage: f64,
}
pub const STANDINS: &[StandIn] = &[
    // Wolf: Target_Bite_Wolf only (plus BG3's common actions).
    StandIn {
        kind: "wolf",
        template: "41da0eff-b533-43cc-a014-218f7d23fa11",
        base_damage: 7.0,
    },
    // BOAR_A: Target_Tusk_Boar and Rush (WoW boars charge too).
    StandIn {
        kind: "boar",
        template: "b9186958-8507-4076-ade5-0a3b43763997",
        base_damage: 4.5,
    },
    // Bear (Bear_Black): claws and bite.
    StandIn {
        kind: "bear",
        template: "b2754d31-198e-4ae6-87ec-5399a3669639",
        base_damage: 7.0,
    },
    // SPIDER_Giant_B: poisonous bite 1d8 + Dex 16, jump.
    StandIn {
        kind: "spider",
        template: "8ed12d53-90aa-4df0-a027-a7b4a022714c",
        base_damage: 7.5,
    },
    // RAT_A: critters.
    StandIn {
        kind: "rat",
        template: "aa60d129-8d11-4178-a22b-836062e86010",
        base_damage: 1.0,
    },
    // BASE_Undead_Zombie: Target_Slam_Zombie 1d6 + Str 16.
    StandIn {
        kind: "zombie",
        template: "a42a8493-b701-4d71-bdc8-0857e718c247",
        base_damage: 6.5,
    },
    // Goblins_Male_Melee: small humanoids (kobolds, murlocs, troggs). Goblin Brawler with
    // WPN_Goblin_Scimitar 1d6 + Dex 13 (finesse) = 4.5 (read in game; 5.5 put the
    // damage floor too low and kobolds never attacked).
    StandIn {
        kind: "goblin",
        template: "2db928f2-f1c5-4b5a-8751-168f9f292249",
        base_damage: 4.5,
    },
    // Humans_Male_Guard_Melee: other humanoids and anything unmatched (longsword 1d8 + Str 16).
    StandIn {
        kind: "human",
        template: "d3b7cade-c52e-47dc-816d-067c00d08f37",
        base_damage: 7.5,
    },
];

/// The stand-in kind for a WoW creature: vmangos `type` (1 beast, 6 undead, 7 humanoid,
/// 8 critter) and `pet_family` (1 wolf, 3 spider, 4 bear, 5 boar), then name words.
pub fn standin_kind(ty: u32, family: u32, name: &str) -> &'static str {
    let n = name.to_ascii_lowercase();
    if ["kobold", "murloc", "trogg", "goblin"]
        .iter()
        .any(|w| n.contains(w))
    {
        return "goblin";
    }
    match (ty, family) {
        (1, 3) => "spider",
        (1, 4) => "bear",
        (1, 5) => "boar",
        (1, _) => "wolf",
        (8, _) => "rat",
        (6, _) => "zombie",
        (0, _) if n.contains("wolf") => "wolf",
        (0, _) if n.contains("spider") => "spider",
        (0, _) if n.contains("boar") => "boar",
        (0, _) if n.contains("bear") => "bear",
        _ => "human",
    }
}

/// WoW creature numbers from `creature_stats.py` (vmangos world database, read-only export).
pub struct CreatureStats(Value);
#[derive(Clone, Debug, PartialEq)]
pub struct MobStats {
    pub name: String,
    pub ty: u32,
    pub family: u32,
    /// Average melee hit: melee_damage(class, level) x damage_multiplier.
    pub damage: f64,
    /// health(class, level) x health_multiplier.
    pub health: u32,
    /// A guard (vmangos `flags_extra` CREATURE_FLAG_EXTRA_GUARD, exported by
    /// `creature_stats.py`): its friendly stand-in reacts to an attack as BG3's guards do.
    pub guard: bool,
}
impl CreatureStats {
    pub fn load(path: &Path) -> Option<Self> {
        let s = fs::read_to_string(path).ok()?;
        serde_json::from_str(&s).ok().map(Self)
    }
    pub fn from_value(v: Value) -> Self {
        Self(v)
    }
    pub fn mob(&self, entry: u32, level: u32) -> Option<MobStats> {
        let e = self.0.get("entries")?.get(entry.to_string())?;
        let num = |k: &str| e.get(k).and_then(Value::as_f64);
        let (lo, hi) = (num("level_min")? as u32, num("level_max")? as u32);
        let level = if level == 0 { lo } else { level.clamp(lo, hi) };
        let class = num("class")? as u32;
        let row = self
            .0
            .get("levels")?
            .get(class.to_string())?
            .get(level.to_string())?;
        let (dmg, hp) = (row.get(0)?.as_f64()?, row.get(1)?.as_f64()?);
        Some(MobStats {
            name: e.get("name")?.as_str()?.to_string(),
            ty: num("type")? as u32,
            family: num("family")? as u32,
            damage: dmg * num("damage_mult")?,
            health: (hp * num("health_mult")?).round().max(1.) as u32,
            guard: num("flags_extra").is_some_and(|f| (f as u64) & 1024 != 0),
        })
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Combatant {
    pub id: String,
    pub guid: String,
    pub pos: Point,
    /// BG3's hit points once BG3 has reported them (Roster or Effect), WoW's before.
    pub hp: u32,
    pub max_hp: u32,
    /// WoW creature entry, level and name (0, 0, "" for the hero).
    pub entry: u32,
    pub level: u32,
    pub name: String,
    /// WoW hit points: at the pull, then less the damage handed to the server.
    pub wow_hp: u32,
    pub wow_max_hp: u32,
    /// Enemies: "joining" (Join sent, not yet in a Roster), "in", "refused" or "dead".
    pub status: &'static str,
    /// The Join that brought it (0: Begin).
    pub join: u32,
    /// BG3 has reported its hit points.
    pub bg3: bool,
    /// A friendly NPC in WoW: the server turns it on the player for this fight
    /// (`begin`/`join` with `!GUID`, server patch 0006).
    pub provoke: bool,
}
impl Combatant {
    pub fn new(id: &str, guid: &str, pos: Point, hp: u32, max_hp: u32) -> Self {
        Self {
            id: id.into(),
            guid: guid.into(),
            pos,
            hp,
            max_hp,
            entry: 0,
            level: 0,
            name: String::new(),
            wow_hp: hp,
            wow_max_hp: max_hp,
            status: "in",
            join: 0,
            bg3: false,
            provoke: false,
        }
    }
    /// Its guid as the server's `begin`/`join` take it (`!GUID` for a friendly NPC).
    pub fn server_guid(&self) -> String {
        if self.provoke {
            format!("!{}", self.guid)
        } else {
            self.guid.clone()
        }
    }
    fn alive(&self) -> bool {
        self.status == "in" && self.hp > 0
    }
    fn label(&self) -> String {
        if self.name.is_empty() {
            self.id.clone()
        } else {
            format!("{} ({})", self.name, self.id)
        }
    }
}

/// The attack of a BG3-run combatant, between its Acted and its Effect.
#[derive(Debug)]
struct Strike {
    action: String,
    actor: String,
    target: String,
    spell: String,
    before: u32,
}

/// What WoW knows about the fight. Pure state: tested without files or BG3.
#[derive(Debug)]
pub struct Fight {
    pub encounter: String,
    /// The server's encounter id (`.wowbg3` verbs).
    pub server_enc: String,
    pub basis: Basis,
    pub receipt: String,
    pub state: &'static str,
    pub turn: u32,
    pub turn_actor: Option<String>,
    pub outstanding: Option<String>,
    pub actions: u32,
    pub hero: Combatant,
    pub enemies: Vec<Combatant>,
    pub anims: Vec<Value>,
    pub anim_seq: u64,
    pub log: Vec<String>,
    pub outcome: Option<Outcome>,
    pub hero_turns: u32,
    pub seq: u64,
    /// Lines ever logged; benilla prints the ones it has not seen.
    pub log_total: u64,
    pub joins_sent: u32,
    pub joins_done: u32,
    /// Server requests raised and not yet taken by the loop, in order.
    pub handoff: VecDeque<Handoff>,
    ended: bool,
    strike: Option<Strike>,
    next_enemy: u32,
    /// Mobs already told they stay out (full encounter): said once each, not on every poll.
    crowded: HashSet<String>,
}

fn wow(basis: &Basis, p: &[f64; 3]) -> Point {
    basis.to_wow(Point(p[0], p[1], p[2]))
}

impl Fight {
    /// `enemies` get ids e1, e2, ... in order.
    pub fn new(
        encounter: &str,
        basis: Basis,
        mut hero: Combatant,
        enemies: Vec<Combatant>,
    ) -> Self {
        hero.id = "hero".into();
        let mut f = Self {
            encounter: encounter.into(),
            server_enc: encounter.into(),
            basis,
            receipt: "no-server-freeze-m4".into(),
            state: "starting",
            turn: 0,
            turn_actor: None,
            outstanding: None,
            actions: 0,
            hero,
            enemies: Vec::new(),
            anims: Vec::new(),
            anim_seq: 0,
            log: Vec::new(),
            outcome: None,
            hero_turns: 0,
            seq: 1,
            log_total: 0,
            joins_sent: 0,
            joins_done: 0,
            handoff: VecDeque::new(),
            ended: false,
            strike: None,
            crowded: HashSet::new(),
            next_enemy: 0,
        };
        for mut c in enemies.into_iter().take(MAX_ENEMIES) {
            f.next_enemy += 1;
            c.id = format!("e{}", f.next_enemy);
            c.status = "in";
            c.join = 0;
            f.enemies.push(c);
        }
        let mobs = f.enemies.iter().map(Combatant::server_guid).collect();
        f.handoff.push_back(Handoff::Begin {
            encounter: f.server_enc.clone(),
            mobs,
        });
        f
    }
    fn actor(&self, c: &Combatant) -> Actor {
        Actor {
            id: c.id.clone(),
            wow_guid: c.guid.clone(),
            position: self.basis.to_bridge(c.pos),
            hp: c.hp.min(c.max_hp),
            max_hp: c.max_hp.max(1),
        }
    }
    pub fn begin(&self) -> Message {
        let mut actors = vec![self.actor(&self.hero)];
        actors.extend(self.enemies.iter().map(|c| self.actor(c)));
        Message::Begin {
            encounter: self.encounter.clone(),
            freeze_receipt: self.receipt.clone(),
            map: 0,
            basis: self.basis,
            actors,
        }
    }
    /// The guids of the fight's enemies (all but refused ones), for the mirrors.
    /// World mode (WoW models lagged far behind BG3 or never moved): each enemy's
    /// position follows its stand-in live (`mirror-status.json` `Pos`, about 3 times a second), not
    /// only the paths BG3 reports once an action is over. True when one moved.
    pub fn follow_standins(&mut self, live: &[(String, Point)]) -> bool {
        let mut moved = false;
        for c in &mut self.enemies {
            if !matches!(c.status, "in" | "joining") {
                continue;
            }
            let Some((_, p)) = live.iter().find(|(g, _)| g.eq_ignore_ascii_case(&c.guid)) else {
                continue;
            };
            let d = ((p.0 - c.pos.0).powi(2) + (p.1 - c.pos.1).powi(2) + (p.2 - c.pos.2).powi(2))
                .sqrt();
            if d > 0.25 {
                c.pos = *p;
                moved = true;
            }
        }
        moved
    }
    pub fn guids(&self) -> Vec<String> {
        self.enemies
            .iter()
            .filter(|c| c.status != "refused")
            .map(|c| c.guid.clone())
            .collect()
    }
    pub fn enemy(&self, id: &str) -> Option<&Combatant> {
        self.enemies.iter().find(|c| c.id == id)
    }
    /// Mobs that entered the fight in WoW: a Join for the ones not in it yet (up to
    /// `MAX_ENEMIES` in all), or None. Only while the fight runs.
    pub fn join(&mut self, new: Vec<Combatant>) -> Option<Message> {
        if !matches!(self.state, "active") {
            return None;
        }
        let mut joined = Vec::new();
        for mut c in new {
            if self.enemies.iter().any(|e| e.guid == c.guid) || c.guid == self.hero.guid {
                continue;
            }
            let live = self
                .enemies
                .iter()
                .filter(|e| matches!(e.status, "in" | "joining"))
                .count();
            if live >= MAX_ENEMIES {
                if self.crowded.insert(c.guid.clone()) {
                    self.say(format!(
                        "{} stays out of the BG3 fight: {MAX_ENEMIES} enemies already",
                        c.name
                    ));
                }
                continue;
            }
            self.next_enemy += 1;
            c.id = format!("e{}", self.next_enemy);
            c.status = "joining";
            c.join = self.joins_sent + 1;
            self.say(format!("{} joins the fight", c.label()));
            joined.push(c.clone());
            self.enemies.push(c);
        }
        if joined.is_empty() {
            return None;
        }
        self.joins_sent += 1;
        self.seq += 1;
        self.handoff.push_back(Handoff::Join {
            encounter: self.server_enc.clone(),
            mobs: joined.iter().map(Combatant::server_guid).collect(),
        });
        Some(Message::Join {
            encounter: self.encounter.clone(),
            actors: joined.iter().map(|c| self.actor(c)).collect(),
        })
    }
    fn say(&mut self, line: String) {
        self.log_total += 1;
        self.log.push(line);
        if self.log.len() > MAX_LOG {
            self.log.remove(0);
        }
    }
    fn anim(&mut self, actor: &str, kind: &str, path: Vec<Point>, spell: &str, target: &str) {
        self.anim_seq += 1;
        let path: Vec<[f64; 3]> = path.iter().map(|p| [p.0, p.1, p.2]).collect();
        self.anims.push(
            json!({"seq": self.anim_seq, "actor": actor, "kind": kind, "path": path,
            "spell": spell, "target": target}),
        );
        if self.anims.len() > MAX_ANIMS {
            self.anims.remove(0);
        }
    }
    /// A BG3-run attack's outcome: benilla plays the WoW mob's own attack (hit or miss).
    fn strike_done(&mut self, hp_after: Option<u32>) {
        let Some(s) = self.strike.take() else {
            return;
        };
        let damage = hp_after.map_or(0, |hp| s.before.saturating_sub(hp));
        let result = if damage > 0 { "hit" } else { "miss" };
        self.anim(&s.actor, "strike", Vec::new(), &s.spell, &s.target);
        if let Some(a) = self.anims.last_mut() {
            a["result"] = json!(result);
            a["damage"] = json!(damage);
        }
    }
    /// The encounter ends for the server (once): survivors are released.
    pub fn end(&mut self, outcome: &str) {
        if self.ended {
            return;
        }
        self.ended = true;
        self.handoff.push_back(Handoff::End {
            encounter: self.server_enc.clone(),
            outcome: outcome.into(),
        });
    }
    /// BG3's hit points for an actor. Every change on an enemy after BG3's first numbers goes to
    /// the server as its new BG3 level (the server keeps the same fraction of the WoW mob's HP);
    /// 0 is the kill, sent once.
    fn set_hp(&mut self, id: &str, hp: u32, max_hp: u32, pos: Option<Point>) {
        if id == self.hero.id {
            // The WoW character's HP follows Tav's, from BG3's first
            // numbers on.
            if !self.hero.bg3 || (hp, max_hp) != (self.hero.hp, self.hero.max_hp) {
                self.handoff.push_back(Handoff::Hero {
                    encounter: self.server_enc.clone(),
                    hp,
                    max_hp,
                });
            }
            self.hero.hp = hp;
            self.hero.max_hp = max_hp;
            self.hero.bg3 = true;
            return;
        }
        let encounter = self.server_enc.clone();
        let Some(i) = self.enemies.iter().position(|c| c.id == id) else {
            return;
        };
        let c = &mut self.enemies[i];
        let (before, known) = (c.hp, c.bg3);
        c.hp = hp;
        c.max_hp = max_hp;
        c.bg3 = true;
        if let Some(p) = pos {
            c.pos = p;
        }
        let mut lines = Vec::new();
        if !known {
            lines.push(format!(
                "{}: WoW {}/{} HP, BG3 stand-in {hp}/{max_hp} HP",
                c.label(),
                c.wow_hp,
                c.wow_max_hp
            ));
            // A mirrored mob may already be hurt when the fight starts (Tav's opening attack out of
            // combat started it): the server gets that level too. A stand-in only at the WoW mob's
            // own fraction (a mob already hurt in WoW) is no damage.
            if hp > 0 && below_wow_fraction(hp, max_hp, c.wow_hp, c.wow_max_hp) {
                c.wow_hp =
                    ((hp as f64 / max_hp.max(1) as f64) * c.wow_max_hp as f64).round() as u32;
                self.handoff.push_back(Handoff::Hp {
                    encounter,
                    mob: c.guid.clone(),
                    hp,
                    max_hp,
                });
            }
        } else if hp != before && c.status != "dead" {
            c.wow_hp = ((hp as f64 / max_hp.max(1) as f64) * c.wow_max_hp as f64).round() as u32;
            if hp == 0 {
                c.status = "dead";
                lines.push(format!("{} dies", c.label()));
                // The corpse lies where BG3's fight left the mob (server patch 0004).
                self.handoff.push_back(Handoff::Pos {
                    encounter: encounter.clone(),
                    mob: c.guid.clone(),
                    x: c.pos.0,
                    y: c.pos.1,
                    z: c.pos.2,
                });
            }
            self.handoff.push_back(Handoff::Hp {
                encounter,
                mob: c.guid.clone(),
                hp,
                max_hp,
            });
        }
        for l in lines {
            self.say(l);
        }
    }
    /// One `WOWBG3 ...` line from the server about this encounter: an end by timeout or by
    /// the player (dead, offline, away) ends the BG3 side too (Cleanup, returned); refusals and
    /// releases are logged.
    pub fn server_event(&mut self, line: &str) -> Option<Message> {
        let w: Vec<&str> = line.split_whitespace().collect();
        if w.first() != Some(&"WOWBG3") || w.get(2) != Some(&self.server_enc.as_str()) {
            return None;
        }
        let who = |g: &str| {
            let g = g.to_ascii_lowercase();
            self.enemies
                .iter()
                .find(|c| c.guid == g)
                .map_or(g, Combatant::label)
        };
        match (w.get(1).copied(), w.get(3).copied()) {
            (Some("end"), Some(why @ ("timeout" | "player"))) => {
                self.say(format!("The server ended the fight ({why})"));
                self.ended = true;
                if matches!(self.state, "active" | "starting") {
                    self.state = "cleaning";
                    self.turn_actor = None;
                    return Some(Message::Cleanup {
                        encounter: self.encounter.clone(),
                    });
                }
            }
            (Some("release"), Some(guid)) => {
                let line = format!(
                    "The server let {} go ({})",
                    who(guid),
                    w.get(4).unwrap_or(&"?")
                );
                self.say(line);
            }
            (Some(verb @ ("begin" | "join" | "hp")), Some(_)) if w.contains(&"refused") => {
                let reason = w
                    .iter()
                    .skip_while(|x| **x != "refused")
                    .nth(1)
                    .unwrap_or(&"?");
                self.say(format!("The server refused {verb}: {reason}"));
            }
            _ => {}
        }
        self.seq += 1;
        None
    }
    pub fn your_turn(&self) -> bool {
        self.state == "active"
            && self.turn_actor.as_deref() == Some(self.hero.id.as_str())
            && self.outstanding.is_none()
    }
    /// A BG3 message, already validated by the endpoint. Returns the message to send back, if any.
    pub fn apply(&mut self, m: &Message) -> Result<Option<Message>, Error> {
        if m.encounter() != self.encounter {
            return Err("message for another encounter".into());
        }
        self.seq += 1;
        if !matches!(m, Message::Effect { .. }) {
            self.strike_done(None);
        }
        let mut reply = None;
        match m {
            Message::Ready { .. } => {
                self.state = "active";
                self.say("BG3: the fight is set up".into());
            }
            Message::Roster { joins, actors, .. } => {
                for a in actors {
                    let pos = self.basis.to_wow(a.position);
                    if let Some(c) = self.enemies.iter_mut().find(|c| c.id == a.id)
                        && c.status == "joining"
                    {
                        c.status = "in";
                    }
                    self.set_hp(&a.id, a.hp, a.max_hp, (a.id != "hero").then_some(pos));
                }
                self.joins_done = self.joins_done.max(*joins);
                let refused: Vec<usize> = (0..self.enemies.len())
                    .filter(|&i| {
                        let c = &self.enemies[i];
                        c.status == "joining" && c.join <= self.joins_done
                    })
                    .collect();
                for i in refused {
                    self.enemies[i].status = "refused";
                    let (label, guid) = (self.enemies[i].label(), self.enemies[i].guid.clone());
                    self.say(format!("{label} could not join the BG3 fight"));
                    self.handoff.push_back(Handoff::Release {
                        encounter: self.server_enc.clone(),
                        mob: guid,
                    });
                }
            }
            Message::Turn { turn, actor, .. } => {
                self.turn = *turn;
                self.turn_actor = Some(actor.clone());
                if *actor == self.hero.id {
                    self.hero_turns += 1;
                    self.say(format!("Turn {turn}: your turn"));
                } else {
                    let who = self.enemy(actor).map_or(actor.clone(), Combatant::label);
                    self.say(format!("Turn {turn}: {who}"));
                }
            }
            Message::Path { action, points, .. } => {
                let path: Vec<Point> = points
                    .iter()
                    .map(|p| wow(&self.basis, &[p.0, p.1, p.2]))
                    .collect();
                if let Some(last) = path.last() {
                    self.hero.pos = *last;
                }
                let id = self.hero.id.clone();
                self.anim(&id, "move", path, "none", "none");
                if self.outstanding.as_deref() == Some(action) {
                    self.outstanding = None;
                }
            }
            Message::Effect {
                action,
                target,
                hp,
                max_hp,
                animation,
                ..
            } => {
                let (hp, max_hp) = (*hp, *max_hp);
                if self.hero.id != *target && self.enemy(target).is_none() {
                    return Err("effect on an unknown actor".into());
                }
                let caster = if self.outstanding.as_deref() == Some(action) {
                    self.outstanding = None;
                    let id = self.hero.id.clone();
                    self.anim(&id, "cast", Vec::new(), animation, target);
                    id
                } else {
                    self.turn_actor.clone().unwrap_or_default()
                };
                if self
                    .strike
                    .as_ref()
                    .is_some_and(|s| s.action == *action && s.target == *target)
                {
                    self.strike_done(Some(hp));
                }
                self.set_hp(target, hp, max_hp, None);
                self.say(format!(
                    "{caster}: {animation} on {target}, {hp}/{max_hp} HP"
                ));
            }
            Message::Acted {
                action,
                actor,
                spell,
                target,
                points,
                ..
            } => {
                let path: Vec<Point> = points
                    .iter()
                    .map(|p| wow(&self.basis, &[p.0, p.1, p.2]))
                    .collect();
                let before = if *target == self.hero.id {
                    Some(self.hero.hp)
                } else {
                    self.enemy(target).map(|c| c.hp)
                };
                let c = if *actor == self.hero.id {
                    &mut self.hero
                } else {
                    self.enemies
                        .iter_mut()
                        .find(|c| c.id == *actor)
                        .ok_or("action by an unknown actor")?
                };
                if let Some(last) = path.last() {
                    c.pos = *last;
                }
                if !path.is_empty() {
                    self.anim(actor, "move", path, "none", "none");
                }
                if spell != "none" {
                    let who = self.enemy(actor).map_or(actor.clone(), Combatant::label);
                    self.say(format!("{who}: {spell} at {target}"));
                    if let Some(before) = before {
                        self.strike = Some(Strike {
                            action: action.clone(),
                            actor: actor.clone(),
                            target: target.clone(),
                            spell: spell.clone(),
                            before,
                        });
                    }
                }
            }
            Message::Rejected {
                action,
                reason,
                detail,
                ..
            } => {
                if self.outstanding.as_deref() == Some(action) {
                    self.outstanding = None;
                }
                self.say(format!("BG3 refused {action}: {reason:?} {detail}"));
            }
            Message::Finished { outcome, .. } => {
                // BG3's victory means every enemy is down: one whose last HP never reached WoW (a
                // direct BG3 blow just before the end) still dies on the server, with its credit.
                if matches!(outcome, Outcome::Victory) {
                    let alive: Vec<(String, u32)> = self
                        .enemies
                        .iter()
                        .filter(|c| c.status == "in" && c.hp > 0)
                        .map(|c| (c.id.clone(), c.max_hp))
                        .collect();
                    for (id, max_hp) in alive {
                        self.set_hp(&id, 0, max_hp, None);
                    }
                }
                self.outcome = Some(outcome.clone());
                self.turn_actor = None;
                self.say(format!("Fight over: {outcome:?}"));
                self.end(&format!("{outcome:?}").to_lowercase());
                reply = Some(Message::Cleanup {
                    encounter: self.encounter.clone(),
                });
                self.state = "cleaning";
            }
            Message::Cleaned { .. } => {
                self.state = "done";
                self.end("fled");
                self.say("BG3 cleaned up".into());
            }
            Message::Abort { reason, .. } => {
                self.state = "failed";
                self.end("abort");
                self.say(format!("BG3 aborted: {reason}"));
            }
            _ => return Err("unexpected message from BG3".into()),
        }
        Ok(reply)
    }
    /// A player request (benilla input or the script) on the hero's turn. A cast goes at
    /// `target_id` when that enemy is alive in the fight, else at the nearest living enemy.
    pub fn request(
        &mut self,
        kind: &str,
        target: Option<Point>,
        spell: &str,
        target_id: Option<&str>,
    ) -> Result<Message, Error> {
        if kind == "leave" {
            if self.state != "active" {
                return Err("no fight to leave".into());
            }
            self.state = "cleaning";
            self.turn_actor = None;
            self.say("Leaving the fight".into());
            return Ok(Message::Cleanup {
                encounter: self.encounter.clone(),
            });
        }
        if !self.your_turn() {
            return Err("not your turn".into());
        }
        let e = self.encounter.clone();
        let turn = self.turn;
        let actor = self.hero.id.clone();
        let m = match kind {
            "move" => {
                let t = target.ok_or("move needs a target")?;
                self.actions += 1;
                let action = format!("move-{}", self.actions);
                self.outstanding = Some(action.clone());
                Message::Move {
                    encounter: e,
                    turn,
                    action,
                    actor,
                    target: self.basis.to_bridge(t),
                }
            }
            "cast" => {
                if !id_ok(spell) {
                    return Err("invalid spell".into());
                }
                let named = target_id
                    .and_then(|id| self.enemy(id))
                    .filter(|c| c.alive());
                let h = self.hero.pos;
                let d = |c: &Combatant| (c.pos.0 - h.0).powi(2) + (c.pos.1 - h.1).powi(2);
                let aim = named
                    .or_else(|| {
                        self.enemies
                            .iter()
                            .filter(|c| c.alive())
                            .min_by(|a, b| d(a).total_cmp(&d(b)))
                    })
                    .ok_or("no living enemy to aim at")?
                    .id
                    .clone();
                self.actions += 1;
                let action = format!("cast-{}", self.actions);
                self.outstanding = Some(action.clone());
                Message::Cast {
                    encounter: e,
                    turn,
                    action,
                    actor,
                    target: aim,
                    spell: spell.into(),
                }
            }
            "end_turn" => {
                self.turn_actor = None;
                Message::EndTurn { encounter: e, turn }
            }
            _ => return Err("unknown input kind".into()),
        };
        self.seq += 1;
        Ok(m)
    }
    /// The same state as `snapshot`, as plain lines for benilla (no JSON parser there):
    /// `seq N`, `state S`, `encounter ID`, `turn N`, `your_turn 0|1`, `actor ID HP MAX X Y Z` (hero and enemies),
    /// `enemy ID GUID ENTRY STATUS`, `anim SEQ ACTOR KIND SPELL TARGET X Y Z X Y Z ...` (path in
    /// WoW yards), `strike SEQ ACTOR TARGET hit|miss DAMAGE SPELL` (a BG3-run attack, in the
    /// same sequence as anims), `log N TEXT` (N counts every line ever logged), `outcome O`.
    pub fn text(&self) -> String {
        let mut out = format!(
            "seq {}\nstate {}\nencounter {}\nturn {}\nyour_turn {}\n",
            self.seq,
            self.state,
            self.encounter,
            self.turn,
            self.your_turn() as u8
        );
        for c in std::iter::once(&self.hero).chain(self.enemies.iter()) {
            out += &format!(
                "actor {} {} {} {} {} {}\n",
                c.id, c.hp, c.max_hp, c.pos.0, c.pos.1, c.pos.2
            );
        }
        for c in &self.enemies {
            out += &format!("enemy {} {} {} {}\n", c.id, c.guid, c.entry, c.status);
        }
        for a in &self.anims {
            let s = |k: &str| {
                a.get(k)
                    .and_then(Value::as_str)
                    .unwrap_or("none")
                    .replace(' ', "_")
            };
            let seq = a.get("seq").and_then(Value::as_u64).unwrap_or(0);
            if s("kind") == "strike" {
                out += &format!(
                    "strike {seq} {} {} {} {} {}\n",
                    s("actor"),
                    s("target"),
                    s("result"),
                    a.get("damage").and_then(Value::as_u64).unwrap_or(0),
                    s("spell")
                );
                continue;
            }
            out += &format!(
                "anim {seq} {} {} {} {}",
                s("actor"),
                s("kind"),
                s("spell"),
                s("target")
            );
            for p in a
                .get("path")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
            {
                for v in p.as_array().into_iter().flatten() {
                    out += &format!(" {}", v.as_f64().unwrap_or(0.));
                }
            }
            out += "\n";
        }
        let first = self.log_total + 1 - self.log.len() as u64;
        for (i, line) in self.log.iter().enumerate() {
            out += &format!("log {} {}\n", first + i as u64, line.replace('\n', " "));
        }
        if let Some(o) = &self.outcome {
            out += &format!("outcome {}\n", format!("{o:?}").to_lowercase());
        }
        out
    }
    pub fn snapshot(&self) -> Value {
        let c = |c: &Combatant| {
            json!({"guid": c.guid, "pos": [c.pos.0, c.pos.1, c.pos.2], "hp": c.hp,
                "max_hp": c.max_hp, "entry": c.entry, "name": c.name, "status": c.status,
                "wow_hp": c.wow_hp, "wow_max_hp": c.wow_max_hp})
        };
        let mut actors = serde_json::Map::new();
        for x in std::iter::once(&self.hero).chain(self.enemies.iter()) {
            actors.insert(x.id.clone(), c(x));
        }
        json!({"seq": self.seq, "state": self.state, "encounter": self.encounter, "turn": self.turn,
            "turn_actor": self.turn_actor, "your_turn": self.your_turn(), "outstanding": self.outstanding,
            "actors": actors, "anims": self.anims, "log": self.log,
            "joins_sent": self.joins_sent, "joins_done": self.joins_done,
            "outcome": self.outcome.as_ref().map(|o| format!("{o:?}").to_lowercase())})
    }
}

/// BG3's first hit points for a stand-in are damage only below the WoW mob's own fraction of the
/// BG3 max rounded down (the least BG3 can show for it): a stand-in keeps its template's HP
/// (a Young Wolf 11), which cannot show every WoW percent. For an unhurt mob
/// that is any HP under the max, as before.
pub fn below_wow_fraction(hp: u32, max_hp: u32, wow_hp: u32, wow_max_hp: u32) -> bool {
    let frac = wow_hp.min(wow_max_hp) as f64 / wow_max_hp.max(1) as f64;
    hp as f64 + 1.0 <= frac * max_hp as f64 + 1e-9
}

/// WoW numbers from the creature stats: the 1.12 client sees a mob's health as a percentage
/// (max 100), so its max HP comes from the template (health at its level) and its HP from the
/// client's fraction; an empty name takes the template's.
pub fn with_stats(c: &mut Combatant, stats: Option<&CreatureStats>) {
    let Some(m) = stats.and_then(|s| s.mob(c.entry, c.level)) else {
        return;
    };
    if c.name.is_empty() {
        c.name = m.name.clone();
    }
    let frac = c.wow_hp as f64 / c.wow_max_hp.max(1) as f64;
    c.wow_max_hp = m.health.max(1);
    c.wow_hp = ((frac * m.health as f64).round() as u32).min(c.wow_max_hp);
    c.hp = c.wow_hp;
    c.max_hp = c.wow_max_hp;
}

/// The stand-in record the mod sets up from (`standins.json` per enemy id): the BG3 template and
/// its base damage, the WoW mob's hit points (their fraction of the template's BG3 max HP is the
/// stand-in's HP) and average hit (the damage scaling; null without creature stats).
pub fn spec(c: &Combatant, stats: Option<&CreatureStats>, wolf_template: &str) -> Value {
    let mob = stats.and_then(|s| s.mob(c.entry, c.level));
    let kind = mob.as_ref().map_or_else(
        || standin_kind(0, 0, &c.name),
        |m| standin_kind(m.ty, m.family, &m.name),
    );
    let s = STANDINS
        .iter()
        .find(|s| s.kind == kind)
        .copied()
        .unwrap_or(STANDINS[0]);
    let template = if kind == "wolf" {
        wolf_template
    } else {
        s.template
    };
    json!({"Kind": kind, "Template": template, "BaseDamage": s.base_damage,
        "WowDamage": mob.as_ref().map(|m| (m.damage * 100.).round() / 100.),
        "WowHp": c.wow_hp, "WowMaxHp": c.wow_max_hp, "Name": c.name, "Entry": c.entry,
        "Level": c.level})
}

fn read_json(path: &Path) -> Option<Value> {
    let mut s = String::new();
    fs::File::open(path)
        .ok()?
        .take(65_536)
        .read_to_string(&mut s)
        .ok()?;
    serde_json::from_str(&s).ok()
}
fn write_atomic(path: &Path, bytes: &[u8]) -> Result<(), Error> {
    let tmp = path.with_extension("tmp");
    fs::write(&tmp, bytes).map_err(|e| e.to_string())?;
    fs::rename(&tmp, path).map_err(|e| e.to_string())
}
fn point(v: &Value) -> Option<Point> {
    let a = v.as_array()?;
    if a.len() != 3 {
        return None;
    }
    let p = Point(a[0].as_f64()?, a[1].as_f64()?, a[2].as_f64()?);
    p.valid().then_some(p)
}
/// One UNIT of pull.json or join.json (benilla's view of a WoW unit).
pub fn combatant(id: &str, v: &Value) -> Option<Combatant> {
    let guid = v.get("guid")?.as_str()?;
    let max_hp = v.get("max_hp")?.as_u64()?.clamp(1, u32::MAX as u64) as u32;
    let hp = (v.get("hp")?.as_u64()?.min(max_hp as u64)) as u32;
    let guid = (guid.len() == 16 && guid.bytes().all(|b| b.is_ascii_hexdigit()))
        .then(|| guid.to_ascii_lowercase())?;
    let mut c = Combatant::new(id, &guid, point(v.get("pos")?)?, hp, max_hp);
    let num = |k: &str| v.get(k).and_then(Value::as_u64).unwrap_or(0) as u32;
    c.entry = num("entry");
    c.level = num("level");
    c.provoke = v.get("provoke").and_then(Value::as_bool) == Some(true);
    c.name = v
        .get("name")
        .and_then(Value::as_str)
        .unwrap_or("")
        .chars()
        .filter(|ch| !ch.is_control())
        .take(64)
        .collect();
    Some(c)
}
/// The enemies of a pull or join file: `enemies` (world fights) or the older single `enemy`.
pub fn units(v: &Value) -> Vec<Combatant> {
    let list: Vec<&Value> = match v.get("enemies").and_then(Value::as_array) {
        Some(a) => a.iter().collect(),
        None => v.get("enemy").into_iter().collect(),
    };
    list.into_iter()
        .filter_map(|u| combatant("enemy", u))
        .take(MAX_ENEMIES)
        .collect()
}
fn random_hex() -> Result<String, Error> {
    let mut random = [0u8; 16];
    fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut random))
        .map_err(|e| e.to_string())?;
    Ok(random.iter().map(|b| format!("{b:02x}")).collect())
}

/// A new request in `link/faction.txt` (`SEQ SIDE`, SIDE horde, alliance or own), once per SEQ.
fn faction_due(link: &Path, last: &mut u64) -> Option<Handoff> {
    let text = fs::read_to_string(link.join("faction.txt")).ok()?;
    let (seq, side) = text.trim().split_once(' ')?;
    let seq: u64 = seq.parse().ok()?;
    let side = side.trim();
    if seq <= *last || !matches!(side, "horde" | "alliance" | "own") {
        return None;
    }
    *last = seq;
    Some(Handoff::Faction { side: side.into() })
}

/// World mode (`link/tools/world_bridge.py`): the session's mapping in `world.json`
/// (`Active`, `WowOrigin`, `Centre`). A fight then cuts its arena where that mapping puts the hero,
/// so the fight's positions and the world's agree and Tav stays where she stands.
pub fn world_centre(world: &Value, hero: Point) -> Option<Point> {
    if world.get("Active").and_then(Value::as_bool) != Some(true) {
        return None;
    }
    let v = |k: &str, i: usize| world.get(k)?.get(i)?.as_f64();
    let (w, c) = (
        [v("WowOrigin", 0)?, v("WowOrigin", 1)?, v("WowOrigin", 2)?],
        [v("Centre", 0)?, v("Centre", 1)?, v("Centre", 2)?],
    );
    const YD: f64 = 0.9144;
    Some(Point(
        c[0] + (hero.1 - w[1]) * YD,
        c[1] + (hero.2 - w[2]) * YD,
        c[2] - (hero.0 - w[0]) * YD,
    ))
}

/// Cut the arena around the hero and put it where the mod reads it. Returns arena.json.
/// In world mode (`in_world`) the ground is already world mode's treadmill (`world_bridge.py`
/// owns `arena-tiles.bin` and `arena-triangles.bin`), so only `arena.json` is written: a fight's
/// 40 m cut must not replace the treadmill's triangles before its next swap.
/// `map`: the WoW continent (0 Eastern Kingdoms, 1 Kalimdor; world.json `Map`).
fn arena(
    o: &Options,
    hero: Point,
    centre: Point,
    in_world: bool,
    map: u64,
) -> Result<Value, Error> {
    let out = o
        .project
        .join("tmp")
        .join(format!("bridge-arena-{}", random_hex()?));
    let status = Command::new("python3")
        .arg(o.project.join("link/tools/wow_arena.py"))
        .arg(&out)
        .args(["--wow", &hero.0.to_string(), &hero.1.to_string()])
        .args([
            "--bg3",
            &centre.0.to_string(),
            &centre.1.to_string(),
            &centre.2.to_string(),
        ])
        .args(["--size", &o.size.to_string()])
        .args(["--map", &map.to_string()])
        .stdout(std::process::Stdio::null())
        .status()
        .map_err(|e| e.to_string())?;
    if !status.success() {
        return Err("wow_arena.py failed".into());
    }
    for (from, to) in [
        ("tiles.bin", "arena-tiles.bin"),
        ("triangles.bin", "arena-triangles.bin"),
        ("arena.json", "arena.json"),
    ] {
        if in_world && to != "arena.json" {
            continue;
        }
        let bytes = fs::read(out.join(from)).map_err(|e| e.to_string())?;
        write_atomic(&o.io.join(to), &bytes)?;
    }
    read_json(&out.join("arena.json")).ok_or_else(|| "unreadable arena.json".into())
}

/// A scripted unit (headless tests): Young Wolf numbers, a fake creature GUID.
fn script_unit(n: u32, at: (f64, f64, f64)) -> Value {
    json!({"guid": format!("f1300000000{:05x}", 0x12b01 + n), "entry": 299, "name": "Young Wolf",
        "level": 1, "pos": [at.0, at.1, at.2], "hp": 42, "max_hp": 42})
}
/// Script mode: the hero at `at` with the enemies 7 yards east, 3 yards apart.
fn script_pull(at: (f64, f64, f64), enemies: u32) -> Value {
    let list: Vec<Value> = (0..enemies.max(1))
        .map(|i| script_unit(i, (at.0 + 7.0, at.1 + 3.0 * i as f64, at.2)))
        .collect();
    json!({"seq": 1, "map": 0,
        "hero": {"guid": "0000000000000001", "pos": [at.0, at.1, at.2], "hp": 60, "max_hp": 60},
        "enemies": list})
}

/// `standins.json` for the mod: every enemy's stand-in (Begin and each Join rewrite it).
fn write_standins(
    o: &Options,
    f: &Fight,
    session: &str,
    stats: Option<&CreatureStats>,
) -> Result<(), Error> {
    let mut map = serde_json::Map::new();
    for c in &f.enemies {
        map.insert(c.id.clone(), spec(c, stats, &o.template));
    }
    let v = json!({"Session": session, "Encounter": f.encounter, "HeroWowMaxHp": f.hero.wow_max_hp,
        "StandIns": map});
    write_atomic(&o.link.join("standins.json"), v.to_string().as_bytes())
}

/// Hand the fight's server requests to the handoff, in order.
fn drain(f: &mut Fight, server: &mut dyn ServerHandoff) -> Result<(), Error> {
    while let Some(h) = f.handoff.pop_front() {
        println!("handoff: {}", h.describe());
        server.send(&h)?;
    }
    Ok(())
}

/// One fight from a pull to Cleaned (or failure). Returns the final fight state.
fn fight(
    o: &Options,
    pull: &Value,
    run_id: &str,
    stats: Option<&CreatureStats>,
    server: &mut dyn ServerHandoff,
    mirror: &mut Mirror,
) -> Result<Fight, Error> {
    let hero = combatant("hero", pull.get("hero").ok_or("pull without hero")?)
        .ok_or("invalid hero in pull")?;
    let mut enemies = units(pull);
    for c in &mut enemies {
        with_stats(c, stats);
    }
    if enemies.is_empty() {
        return Err("pull without enemies".into());
    }
    let world = read_json(&o.link.join("world.json"));
    let world_at = world.as_ref().and_then(|w| world_centre(w, hero.pos));
    let centre = world_at.unwrap_or(o.centre);
    let map = world
        .as_ref()
        .filter(|_| world_at.is_some())
        .and_then(|w| w.get("Map"))
        .and_then(Value::as_u64)
        .unwrap_or(0);
    let meta = arena(o, hero.pos, centre, world_at.is_some(), map)?;
    let num = |k: &str, i: usize| {
        meta.get(k)
            .and_then(|v| v.get(i))
            .and_then(Value::as_f64)
            .ok_or("arena.json field")
    };
    let wow_origin = Point(
        num("wow_centre", 0)?,
        num("wow_centre", 1)?,
        num("wow_centre", 2)?,
    );
    let centre = [
        num("bg3_centre", 0)?,
        num("bg3_centre", 1)?,
        num("bg3_centre", 2)?,
    ];
    let min_corner = [num("min_corner_bg3", 0)?, num("min_corner_bg3", 1)?];
    let basis = Basis {
        wow_origin,
        bridge_origin: Point(0., 0., 0.),
        yaw: 0.,
    };
    let encounter = format!("pull-{}", &random_hex()?[..12]);
    let mut f = Fight::new(&encounter, basis, hero, enemies);
    if let Some(Handoff::Begin { .. }) = f.handoff.front() {
        let h = f.handoff.pop_front().ok_or("no begin handoff")?;
        println!("handoff: {}", h.describe());
        f.receipt = server.begin(&h)?;
    }
    let session = random_hex()?;
    let result = drive(
        o, &mut f, &session, centre, min_corner, run_id, stats, server, mirror,
    );
    if result.is_err() {
        f.end("abort");
    }
    drain(&mut f, server)?;
    result.map(|_| f)
}

#[allow(clippy::too_many_arguments)]
fn drive(
    o: &Options,
    f: &mut Fight,
    session: &str,
    centre: [f64; 3],
    min_corner: [f64; 2],
    run_id: &str,
    stats: Option<&CreatureStats>,
    server: &mut dyn ServerHandoff,
    mirror: &mut Mirror,
) -> Result<(), Error> {
    let mailbox = Mailbox::new(&o.link, Role::Wow)?;
    let mut link = Endpoint::new(session, Role::Wow, 0, 20_000)?;
    write_standins(o, f, session, stats)?;
    let templates: serde_json::Map<String, Value> = f
        .enemies
        .iter()
        .map(|c| {
            (
                c.id.clone(),
                spec(c, stats, &o.template)["Template"].clone(),
            )
        })
        .collect();
    let start = json!({"RunId": run_id, "Session": session, "Actors": {},
        "Void": {"Centre": centre, "MinCorner": min_corner, "Layer": -15.262,
            "Templates": templates, "Faction": o.faction, "Follow": o.follow}});
    write_atomic(&o.link.join("start.json"), start.to_string().as_bytes())?;
    let fight_file = o.link.join("fight.json");
    let mut first = Some(f.begin());
    let mut queue: VecDeque<Message> = Default::default();
    let seq_of = |name: &str| {
        read_json(&o.link.join(name))
            .and_then(|v| v.get("seq").and_then(Value::as_u64))
            .unwrap_or(0)
    };
    let (mut input_seq, mut join_seq) = (seq_of("input.json"), seq_of("join.json"));
    let mut pull_seq = seq_of("pull.json");
    let (mut script_step, mut script_turn, mut script_joined) = (0usize, 0u32, false);
    let mut written = 0u64;
    let t0 = Instant::now();
    let (mut done_at, mut kept) = (None, 0u64);
    let events_file = o.link.join("server-events.txt");
    let mut event_seq = last_line(&events_file);
    let feed_file = o.link.join("server-feed.txt");
    let mut feed_seq = last_line(&feed_file);
    let mut polled = 0u64;
    loop {
        let now = t0.elapsed().as_millis() as u64;
        if now > o.max_seconds * 1000 {
            return Err(format!(
                "fight exceeded {} s in state {}",
                o.max_seconds, f.state
            ));
        }
        link.tick(now)?;
        if let Some(raw) = mailbox.read()?
            && let Ok(frame) = decode(&raw)
            && frame.session == session
        {
            link.receive(&raw, now, |m| {
                println!(
                    "{now:>7} ms <- {}",
                    serde_json::to_string(m).unwrap_or_default()
                );
                if let Some(r) = f.apply(m)? {
                    queue.push_back(r);
                }
                Ok(())
            })?;
        }
        // Mobs that entered the fight in WoW (benilla), or the script's adds.
        let mut joining = Vec::new();
        if f.state == "active" {
            if o.script {
                if f.hero_turns >= 2 && !script_joined && o.script_adds > 0 {
                    script_joined = true;
                    let h = f.hero.pos;
                    joining = (0..o.script_adds)
                        .filter_map(|i| {
                            let u =
                                script_unit(100 + i, (h.0 - 6.0, h.1 + 4.0 + 3.0 * i as f64, h.2));
                            combatant("enemy", &u)
                        })
                        .collect();
                }
            } else if let Some(v) = read_json(&o.link.join("join.json"))
                && let Some(seq) = v.get("seq").and_then(Value::as_u64)
                && seq > join_seq
            {
                join_seq = seq;
                joining = units(&v);
            }
            // A pull benilla wrote meanwhile: its mobs join this fight.
            if !o.script
                && let Some(v) = read_json(&o.link.join("pull.json"))
                && let Some(seq) = v.get("seq").and_then(Value::as_u64)
                && seq > pull_seq
            {
                pull_seq = seq;
                joining.extend(units(&v));
            }
        }
        // The mirrors: kept in sync beside the fight; mirrored mobs BG3 pulled into its combat join.
        if o.mirrors && !o.script {
            mirror.in_fight = f
                .enemies
                .iter()
                .filter(|c| c.status == "in")
                .map(|c| c.guid.clone())
                .collect();
            mirror.poll(
                &o.link,
                &f.guids(),
                stats,
                &o.template,
                o.faction.as_deref(),
                &o.neutral_faction,
            )?;
            if matches!(f.state, "starting" | "active")
                && let Some(map) = mirror.mapping
            {
                let live: Vec<(String, Point)> = mirror
                    .status
                    .mobs
                    .iter()
                    .filter_map(|(g, s)| Some((g.clone(), map.to_wow(s.pos?))))
                    .collect();
                if f.follow_standins(&live) {
                    f.seq += 1;
                }
            }
            party_step(o, mirror, server)?;
            if f.state == "active" {
                let have = f.guids();
                let now_ms = mirror.now();
                let mut wanted = mirror.in_bg3_combat(now_ms);
                wanted.extend(mirror.provoked(now_ms));
                for g in wanted {
                    if !have.contains(&g)
                        && !joining.iter().any(|c| c.guid == g)
                        && let Some(u) = mirror.unit(&g)
                    {
                        joining.push(unit_combatant(u));
                    }
                }
            }
        }
        for c in &mut joining {
            with_stats(c, stats);
        }
        // The server's answers (through benilla).
        if now - polled >= 400 {
            polled = now;
            for (n, line) in event_lines(&events_file, event_seq) {
                event_seq = event_seq.max(n);
                mirror.arm_event(&line);
                mirror.feed_event(&line);
                // A mob the server held as an add of this encounter (its pack joined, 0015, or it
                // aggroed a party bot): it joins the BG3 fight even where benilla cannot see it
                // (a far member's fight under Far Sight elsewhere).
                if let Some(c) = server_add(&line, &f.server_enc, mirror)
                    && !f.guids().contains(&c.guid)
                    && !joining.iter().any(|j| j.guid == c.guid)
                {
                    joining.push(c);
                }
                if let Some(m) = f.server_event(&line) {
                    queue.push_back(m);
                }
            }
            server_feed_step(mirror, &feed_file, &mut feed_seq);
        }
        // The server ends an encounter after 120 s without a command.
        if now - kept > 30_000 && matches!(f.state, "active" | "starting") {
            kept = now;
            f.handoff.push_back(Handoff::Keep {
                encounter: f.server_enc.clone(),
            });
        }
        if !joining.is_empty()
            && let Some(m) = f.join(joining)
        {
            write_standins(o, f, session, stats)?;
            println!(
                "{now:>7} ms -> {}",
                serde_json::to_string(&m).unwrap_or_default()
            );
            queue.push_back(m);
        }
        drain(f, server)?;
        if !link.busy() {
            if let Some(m) = first.take() {
                link.offer(m)?;
            } else if let Some(m) = queue.pop_front() {
                if !matches!(m, Message::Join { .. }) {
                    println!(
                        "{now:>7} ms -> {}",
                        serde_json::to_string(&m).unwrap_or_default()
                    );
                }
                link.offer(m)?;
            } else if o.script && f.state == "active" && f.your_turn() {
                // Script: the first hero turn steps 3 yards toward the nearest enemy; every
                // hero turn casts Fire Bolt at it and ends; after six hero turns it leaves.
                if script_turn != f.hero_turns {
                    script_turn = f.hero_turns;
                    script_step = 0;
                }
                let plan: &[&str] = if f.hero_turns == 1 {
                    &["move", "cast", "end_turn"]
                } else {
                    &["cast", "end_turn"]
                };
                let kind = if f.hero_turns > 6 {
                    "leave"
                } else {
                    plan.get(script_step).copied().unwrap_or("end_turn")
                };
                script_step += 1;
                let h = f.hero.pos;
                let e = f
                    .enemies
                    .iter()
                    .filter(|c| c.alive())
                    .map(|c| c.pos)
                    .min_by(|a, b| {
                        let d = |p: &Point| (p.0 - h.0).powi(2) + (p.1 - h.1).powi(2);
                        d(a).total_cmp(&d(b))
                    })
                    .unwrap_or(h);
                let d = ((e.0 - h.0).powi(2) + (e.1 - h.1).powi(2)).sqrt().max(0.1);
                let target = Some(Point(
                    h.0 + 3. * (e.0 - h.0) / d,
                    h.1 + 3. * (e.1 - h.1) / d,
                    h.2,
                ));
                match f.request(kind, target, "FireBolt", None) {
                    Ok(m) => {
                        println!(
                            "{now:>7} ms -> {}",
                            serde_json::to_string(&m).unwrap_or_default()
                        );
                        link.offer(m)?;
                    }
                    Err(e) => {
                        f.say(format!("Script input ignored: {e}"));
                        if let Ok(m) = f.request("end_turn", None, "", None) {
                            link.offer(m)?;
                        }
                    }
                }
            } else if !o.script
                && let Some(input) = read_json(&o.link.join("input.json"))
                && let Some(seq) = input.get("seq").and_then(Value::as_u64)
                && seq > input_seq
            {
                input_seq = seq;
                let kind = input.get("kind").and_then(Value::as_str).unwrap_or("");
                let target = input.get("target").and_then(point);
                let spell = input
                    .get("spell")
                    .and_then(Value::as_str)
                    .unwrap_or("FireBolt");
                let aim = input
                    .get("target_guid")
                    .and_then(Value::as_str)
                    .map(str::to_ascii_lowercase)
                    .and_then(|g| f.enemies.iter().find(|c| c.guid == g).map(|c| c.id.clone()));
                match f.request(kind, target, spell, aim.as_deref()) {
                    Ok(m) => {
                        println!(
                            "{now:>7} ms -> {}",
                            serde_json::to_string(&m).unwrap_or_default()
                        );
                        link.offer(m)?;
                    }
                    Err(e) => f.say(format!("Input ignored: {e}")),
                }
                f.seq += 1;
            }
        }
        drain(f, server)?;
        mailbox.write(&link.frame()?)?;
        if f.seq != written {
            written = f.seq;
            write_atomic(&fight_file, f.snapshot().to_string().as_bytes())?;
            write_atomic(&o.link.join("fight.txt"), f.text().as_bytes())?;
        }
        if f.state == "done" || f.state == "failed" {
            let at = *done_at.get_or_insert(now);
            if now - at > 1500 {
                break;
            }
        }
        thread::sleep(Duration::from_millis(40));
    }
    Ok(())
}

fn current_run(o: &Options) -> Result<String, Error> {
    let client = read_json(&o.io.join("m3-client.json")).ok_or("no BG3 client report")?;
    let run_id = client
        .get("RunId")
        .and_then(Value::as_str)
        .ok_or("no BG3 run id")?
        .to_string();
    if !(run_id.len() == 32 && run_id.bytes().all(|b| b.is_ascii_hexdigit())) {
        return Err("invalid BG3 run id".into());
    }
    if client.get("GameState").and_then(Value::as_str) != Some("Running") {
        return Err("BG3 is not in a running level".into());
    }
    Ok(run_id)
}

/// A mirrored mob as a fight combatant (WoW yards, the client's HP).
fn unit_combatant(u: &WowUnit) -> Combatant {
    let mut c = Combatant::new("enemy", &u.guid, u.pos, u.hp.min(u.max_hp), u.max_hp);
    c.entry = u.entry;
    c.level = u.level;
    c.name = u.name.clone();
    c.provoke = u.friendly;
    c
}

/// `WOWBG3 mob ENC GUID ENTRY LEVEL HP MAXHP MINDMG MAXDMG ATTACK_MS X Y Z aggro` for this encounter:
/// the mob as a combatant (the mirror's unit when it has one).
pub fn server_add(line: &str, enc: &str, mirror: &Mirror) -> Option<Combatant> {
    let w: Vec<&str> = line.split_whitespace().collect();
    if w.len() != 15 || w[0] != "WOWBG3" || w[1] != "mob" || w[2] != enc || w[14] != "aggro" {
        return None;
    }
    let guid = w[3].to_ascii_lowercase();
    if guid.len() != 16 || !guid.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    if let Some(u) = mirror.unit(&guid) {
        return Some(unit_combatant(u));
    }
    let num = |i: usize| w[i].parse::<u32>().ok();
    let pos = |i: usize| w[i].parse::<f64>().ok().filter(|v| v.is_finite());
    let (entry, level, hp, max_hp) = (num(4)?, num(5)?, num(6)?, num(7)?.max(1));
    let mut c = Combatant::new(
        "enemy",
        &guid,
        Point(pos(11)?, pos(12)?, pos(13)?),
        hp.min(max_hp),
        max_hp,
    );
    c.entry = entry;
    c.level = level;
    c.name = format!("creature {entry}");
    Some(c)
}

/// A stand-in BG3 killed with one blow out of combat (no fight started): the server gets the kill
/// as a one-mob encounter (XP, loot and quest credit once).
pub fn kills_outside_fights(
    mirror: &mut Mirror,
    server: &mut dyn ServerHandoff,
) -> Result<(), Error> {
    let now = mirror.now();
    for g in mirror.dead_unfought(now) {
        let max_hp = mirror.status.mobs.get(&g).map_or(1, |s| s.max_hp.max(1));
        // A friendly NPC killed with one blow is turned on the player first (server patch 0006).
        let server_guid = mirror.server_guid(&g);
        let encounter = format!("kill-{}", &random_hex()?[..12]);
        let list = vec![
            Handoff::Begin {
                encounter: encounter.clone(),
                mobs: vec![server_guid.clone()],
            },
            Handoff::Hp {
                encounter: encounter.clone(),
                mob: g.clone(),
                hp: 0,
                max_hp,
            },
            Handoff::End {
                encounter,
                outcome: "victory".into(),
            },
        ];
        println!("bridge: BG3 killed mirrored mob {g} outside a fight");
        for h in list {
            println!("handoff: {}", h.describe());
            server.send(&h)?;
        }
        mirror.settle([g], now);
    }
    Ok(())
}

/// The last line number of an `N LINE` file (0 when none): where a new reader starts.
fn last_line(path: &Path) -> u64 {
    event_lines(path, 0).iter().map(|e| e.0).max().unwrap_or(0)
}

/// `server-events.txt` (or `server-feed.txt`) lines after `after` (`N LINE`).
fn event_lines(path: &Path, after: u64) -> Vec<(u64, String)> {
    fs::read_to_string(path)
        .unwrap_or_default()
        .lines()
        .filter_map(|l| {
            let (n, rest) = l.split_once(' ')?;
            Some((n.parse::<u64>().ok()?, rest.to_string()))
        })
        .filter(|(n, _)| *n > after)
        .collect()
}

/// The server's `WOWBG3 ...` lines since the last call: arming and sight mode (`Mirror::arm_event`).
/// The turn-based freeze that also lived here (`begin tb-...` while Tav was in turn-based mode, `end
/// ... abort` after) is gone: TurnHold.lua and the server's hold verbs
/// (`hold`/`unhold`, patch 0010) hold every creature near the party in place and leave it there.
pub fn server_events_step(mirror: &mut Mirror, events: &Path, event_seq: &mut u64) {
    for (n, line) in event_lines(events, *event_seq) {
        *event_seq = (*event_seq).max(n);
        mirror.arm_event(&line);
        mirror.feed_event(&line);
        // Patch 0015's walk answers.
        mirror.party.event(&line);
    }
}

/// The server's creature feed since the last call (benilla 0046 `server-feed.txt`: `WOWBG3 cr|crgone|
/// tick|near MEMBER ...`, its own line numbers; `Mirror::feed_event`).
pub fn server_feed_step(mirror: &mut Mirror, feed: &Path, feed_seq: &mut u64) {
    for (n, line) in event_lines(feed, *feed_seq) {
        *feed_seq = (*feed_seq).max(n);
        mirror.feed_event(&line);
    }
}

/// The party's verbs (patch 0010: bots where their members stand, their creature feeds, Far Sight;
/// `crate::mirror::PartyLink`), while world mode runs.
pub fn party_step(
    o: &Options,
    mirror: &mut Mirror,
    server: &mut dyn ServerHandoff,
) -> Result<(), Error> {
    mirror.stop_if_asked(&o.link);
    // Turn-based mode outside combat holds the creatures round the party (through
    // the hero's client, not the console; crate::turnhold).
    let now = mirror.now();
    for h in mirror.turnhold.step(&o.link, now) {
        println!("handoff: {}", h.describe());
        server.send(&h)?;
    }
    if mirror.mapping.is_none() {
        return Ok(());
    }
    let (map, seen) = (mirror.map, mirror.feed_seen.clone());
    for h in mirror.party.step(&o.link, &o.io, map, now, &seen) {
        println!("handoff: {}", h.describe());
        server.send(&h)?;
    }
    Ok(())
}

pub fn run(o: &Options) -> Result<(), Error> {
    fs::set_permissions(&o.link, fs::Permissions::from_mode(0o700)).map_err(|e| e.to_string())?;
    crate::mirror::catch_stop();
    let run_id = current_run(o)?;
    let stats = CreatureStats::load(&o.stats);
    if stats.is_none() {
        println!(
            "bridge: no creature stats at {:?}: stand-ins keep BG3's damage",
            o.stats
        );
    }
    let mut server = Ledger::new(&o.link);
    // Hold mobs as they aggro the player; the pull or aggro claims them with `begin`. With
    // mirrors the loop arms (in sight mode once world mode runs: BG3's stand-ins decide who
    // notices Tav).
    if !o.mirrors {
        server.send(&Handoff::Arm { sight: None })?;
    }
    let mut mirror = Mirror::new(&random_hex()?);
    let pull_seq = || {
        read_json(&o.link.join("pull.json"))
            .and_then(|v| v.get("seq").and_then(Value::as_u64))
            .unwrap_or(0)
    };
    let mut last_pull = pull_seq();
    let idle = |state: &str, why: &str| -> Result<(), Error> {
        let v = json!({"seq": 0, "state": state, "log": [why]});
        write_atomic(&o.link.join("fight.json"), v.to_string().as_bytes())?;
        let text = format!(
            "seq 0\nstate {state}\nyour_turn 0\nlog 1 {}\n",
            why.replace('\n', " ")
        );
        write_atomic(&o.link.join("fight.txt"), text.as_bytes())
    };
    idle("idle", "BG3 bridge ready")?;
    let events = o.link.join("server-events.txt");
    let mut event_seq = last_line(&events);
    let feed = o.link.join("server-feed.txt");
    let mut feed_seq = last_line(&feed);
    println!("bridge: BG3 run {run_id}; waiting for a pull");
    // link/faction.txt (`SEQ SIDE`, link/tools/faction.sh): the WoW character's side.
    // Sent once per new SEQ; at bridge start the file's side is sent again (the server forgets
    // it at logout, and each world-mode start is a new login).
    let mut faction_seq = 0u64;
    loop {
        if let Some(h) = faction_due(&o.link, &mut faction_seq) {
            println!("handoff: {}", h.describe());
            server.send(&h)?;
        }
        let pull = if o.script {
            script_pull(
                o.pull_at.ok_or("script mode needs --pull X Y Z")?,
                o.script_enemies,
            )
        } else {
            let mut found = read_json(&o.link.join("pull.json"))
                .filter(|p| p.get("seq").and_then(Value::as_u64).unwrap_or(0) > last_pull);
            if found.is_none() && o.mirrors {
                mirror.in_fight.clear();
                mirror.poll(
                    &o.link,
                    &[],
                    stats.as_ref(),
                    &o.template,
                    o.faction.as_deref(),
                    &o.neutral_faction,
                )?;
                if let Some(h) = mirror.arm_due(mirror.now()) {
                    println!("handoff: {}", h.describe());
                    server.send(&h)?;
                }
                kills_outside_fights(&mut mirror, &mut server)?;
                party_step(o, &mut mirror, &mut server)?;
                // BG3 combat with a mirrored mob (Tav attacked it, or its stand-in saw her), or a
                // mob Tav attacked that BG3 left out of combat (a miss on a neutral one).
                let mut combat = mirror.in_bg3_combat(mirror.now());
                let mut why = "BG3 combat with";
                if combat.is_empty() {
                    combat = mirror.provoked(mirror.now());
                    why = "Tav attacked";
                }
                if !combat.is_empty() {
                    found = mirror.pull(&combat, last_pull);
                    if found.is_some() {
                        println!("bridge: {why} mirrored mobs {combat:?}");
                    }
                }
                if found.is_none() {
                    server_events_step(&mut mirror, &events, &mut event_seq);
                    server_feed_step(&mut mirror, &feed, &mut feed_seq);
                }
            }
            match found {
                Some(p) => p,
                None => {
                    thread::sleep(Duration::from_millis(100));
                    continue;
                }
            }
        };
        last_pull = pull
            .get("seq")
            .and_then(Value::as_u64)
            .unwrap_or(last_pull + 1)
            .max(last_pull);
        println!("bridge: pull {}", pull);
        let pulled: Vec<String> = units(&pull).into_iter().map(|c| c.guid).collect();
        // BG3 may have been restarted since the bridge started: take its current run.
        let run_id = match current_run(o) {
            Ok(r) => r,
            Err(e) => {
                idle("failed", &format!("BG3 is not ready: {e}"))?;
                mirror.settle(pulled, mirror.now());
                continue;
            }
        };
        match fight(o, &pull, &run_id, stats.as_ref(), &mut server, &mut mirror) {
            Ok(f) => {
                println!(
                    "bridge: fight {} ended: {} {:?} after {} hero turns",
                    f.encounter, f.state, f.outcome, f.hero_turns
                );
                for c in &f.enemies {
                    println!(
                        "bridge:   {} {} {}: BG3 {}/{} HP, WoW {}/{} HP",
                        c.id, c.guid, c.status, c.hp, c.max_hp, c.wow_hp, c.wow_max_hp
                    );
                }
                // Its mobs' stand-ins are respawned fresh (out of BG3's combat) after a while.
                let guids: Vec<String> = f.enemies.iter().map(|c| c.guid.clone()).collect();
                mirror.settle(guids, mirror.now());
                if o.script {
                    return if f.state == "done" {
                        Ok(())
                    } else {
                        Err(format!("fight ended {}", f.state))
                    };
                }
            }
            Err(e) => {
                println!("bridge: fight failed: {e}");
                idle("failed", &format!("BG3 fight failed: {e}"))?;
                mirror.settle(pulled, mirror.now());
                if o.script {
                    return Err(e);
                }
            }
        }
        // Pulls benilla wrote during the fight joined it (or are old news).
        last_pull = last_pull.max(pull_seq());
        event_seq = event_seq.max(
            event_lines(&events, event_seq)
                .iter()
                .map(|e| e.0)
                .max()
                .unwrap_or(0),
        );
    }
}
