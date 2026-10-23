//! Mirrored WoW mobs (world fights). In world mode every attackable WoW creature near
//! the hero has a BG3 stand-in all the time (`Mirrors.lua`), at the mob's position and facing on
//! the treadmill ground, so BG3's own targeting, attacks, throws, spells, sight cones, stealth and
//! turn-based mode work on WoW's mobs. WoW keeps drawing the mobs (the capture layer's `void` keep
//! list leaves the stand-ins out of the composite).
//!
//! Files in the link directory:
//! - `mobs.txt` (benilla, every 12 frames): `seq N`, `hero GUID X Y Z O HP MAXHP LEVEL [STATE]`
//!   (STATE since benilla 0025: 0 alive, 1 dead, 2 a ghost), then one
//!   `mob GUID ENTRY LEVEL REACTION X Y Z O HP MAXHP FLAGS NAME...` per living creature within
//!   40 yd (WoW yards, O the WoW orientation in radians, HP as the 1.12 client sees it, REACTION 1
//!   for a mob that aggroes by itself, 0 for a neutral one, 2 (benilla 0027) for a
//!   friendly NPC the player may not attack in WoW: vendors, quest givers, guards, villagers;
//!   FLAGS 1 in combat, 2 its target is you).
//!
//! Friendly NPCs: they get stand-ins too, after the attackable
//! mobs (never in their place), within [`FRIENDLY_RANGE`], at most [`MAX_FRIENDLY`] of their own.
//! Attacking one is BG3's own flow: BG3's Assault crime makes the stand-in temporarily hostile
//! (`Mirrors.lua`) and the BG3 combat that follows is the fight; the server then turns that NPC on
//! the player (`begin`/`join` with `!GUID`, server patch 0006). A hit alone (BG3 started no combat: a 0-damage assault is only a warning there) pulls none.
//! - `mirror.json` (bridge, for `Mirrors.lua`; every 250 ms while world mode runs): the mobs to
//!   mirror with their stand-in kind and BG3 position and forward direction (world mode's mapping,
//!   `world.json`), the WoW numbers (the HP fraction a stand-in keeps of its template's BG3 HP, the
//!   damage scaling), and the mobs in the running fight (never
//!   moved or removed by the mod while it runs). Also the WoW hero's HP and
//!   state (`HeroWowHp`, `HeroState` alive, dead or ghost): `Mirrors.lua` resurrects Tav with BG3's
//!   own resurrection when the WoW character comes back to life, and lets her die when it dies in
//!   WoW. Positions of idle mobs reach BG3 every frame through benilla's `wowmobs.bin`
//!   `Pos`/`Dir` here are the fallback and the spawn point.
//! - `mirror-status.json` (`Mirrors.lua`): per mirrored guid its stand-in uuid, BG3 HP and whether
//!   it is in Tav's BG3 combat; Tav's HP, combat and turn-based state.
//! - `mirrored.txt` (bridge, for benilla): `m GUID` per mob with a live stand-in (BG3 takes clicks
//!   on it) and `c GUID` per dead one whose stand-in's corpse is still there (BG3 takes clicks on
//!   that corpse too: BG3's own loot window, filled with the WoW loot by `Inventory.lua`, not WoW's),
//!   and `s GUID` per robbed NPC searching for the thief (BG3 walks its stand-in; benilla 0067).
//!   (`h GUID` lines for the old turn-based freeze are gone: TurnHold.lua and the server's
//!   hold verbs replace it.)
//!
//! Per party member (party-multiplayer-design.md): mobs are mirrored
//! round every member, not only the hero. `party-wow.json` (`world_bridge.py`, Format 1) gives each
//! member's WoW position; a mob within `MIRROR_RANGE` of any member is wanted, and one already
//! mirrored stays until it is `KEEP_MARGIN` further out (no re-creating a stand-in that walks along
//! the edge). Where the creatures come from is one function, [`read_feed`]: benilla's view of the
//! hero (`mobs.txt`) plus any per-member feed files `feed/<member>.txt` in the same `mob ...` line
//! format (the server's per-member creature feed, lands there).
//!
//! The server's per-member feed itself (patch 0010): [`PartyLink`] sends, through the
//! hero's own client like every `.wowbg3` verb (`server-cmd.txt`, benilla): `move NAME X Y Z MAP` so
//! each member's bot stands where its BG3 member stands, `feed NAME on` once per bot, and `view
//! NAME|self` for Far Sight (`farsight.json`). The feed's lines come back in `server-feed.txt`
//! (benilla 0046; `WOWBG3 cr|crgone|tick MEMBER ...`, [`Mirror::feed_event`], read by
//! `crate::bridge::server_feed_step`) and their creatures join benilla's
//! in `mobs` (a member's feed counts while its last `tick` is under 5 s old).
use crate::{
    Error,
    bridge::{CreatureStats, spec},
    coords::{METRES_PER_YARD, Point},
    handoff::Handoff,
};
use serde_json::{Value, json};
use std::sync::atomic::{AtomicBool, Ordering};
use std::{
    collections::{HashMap, HashSet},
    fs,
    io::Read,
    path::Path,
    time::Instant,
};

/// Yards from the hero within which mobs are mirrored (45, was 26, so they
/// pop in farther away). The treadmill keeps at least 24 m (26 yd) of WoW ground around Tav: a
/// stand-in past that can stand beyond BG3's ground until she comes closer.
pub const MIRROR_RANGE: f64 = 45.0;
/// Stand-ins mirrored at once (the nearest), besides the running fight's, with the hero alone.
pub const MAX_MIRRORS: usize = 24;
/// More stand-ins for each other party member with mobs round it, up to [`MAX_MIRRORS_PARTY`]
/// (`Mirrors.lua` keeps at most 60, fights included).
pub const MIRRORS_PER_MEMBER: usize = 8;
pub const MAX_MIRRORS_PARTY: usize = 48;
/// Yards past `MIRROR_RANGE` a mirrored mob keeps its stand-in (stand-ins at the edge
/// were re-created often).
pub const KEEP_MARGIN: f64 = 8.0;
/// Yards: friendly NPCs are mirrored this close (benilla's `mobs.txt` reaches 40 yd: every NPC the
/// player sees round the party), so BG3's right click and Talk reach them from where the player
/// sees them (was 18, BG3's reach for a melee attack, and a quest giver further off had no stand-in).
pub const FRIENDLY_RANGE: f64 = 40.0;
/// Friendly NPCs mirrored at once, the nearest, besides the attackable mobs (their own budget, so a
/// field of wolves does not take the quest giver's stand-in; was 8 within what [`MAX_MIRRORS`] left).
pub const MAX_FRIENDLY: usize = 16;
/// The server holds at most this many mobs per encounter.
pub const MAX_HELD: usize = 50;
pub const FLAG_COMBAT: u32 = 1;
pub const FLAG_TARGETS_ME: u32 = 2;

/// A WoW unit as benilla sees it (`mobs.txt`).
#[derive(Clone, Debug, PartialEq)]
pub struct WowUnit {
    pub guid: String,
    pub entry: u32,
    pub level: u32,
    pub hostile: bool,
    /// A friendly NPC (REACTION 2): WoW does not let the player attack it.
    pub friendly: bool,
    pub pos: Point,
    pub orient: f64,
    pub hp: u32,
    pub max_hp: u32,
    pub flags: u32,
    pub name: String,
    /// The hero's state (benilla 0025): 0 alive, 1 dead (a corpse), 2 a ghost; 0 for mobs and
    /// for an older benilla.
    pub state: u32,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Mobs {
    pub seq: u64,
    pub hero: Option<WowUnit>,
    pub mobs: Vec<WowUnit>,
}

/// A party member other than the hero, where it stands in WoW (`party-wow.json`).
#[derive(Clone, Debug, PartialEq)]
pub struct Member {
    pub uuid: String,
    pub name: String,
    pub pos: Point,
}

/// `party-wow.json` (Format 1): the members, the hero's own row (`Hero` true) and dead ones left out
/// (the hero's WoW position is the feed's `hero`).
pub fn parse_members(v: &Value) -> Vec<Member> {
    if v.get("Format").and_then(Value::as_u64) != Some(1) {
        return Vec::new();
    }
    let mut out = Vec::new();
    for m in v
        .get("Members")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .take(16)
    {
        let Some(uuid) = m.get("Uuid").and_then(Value::as_str) else {
            continue;
        };
        if m.get("Hero").and_then(Value::as_bool) == Some(true)
            || m.get("Dead").and_then(Value::as_bool) == Some(true)
        {
            continue;
        }
        let p: Vec<f64> = m
            .get("Wow")
            .and_then(Value::as_array)
            .map(|a| a.iter().filter_map(Value::as_f64).collect())
            .unwrap_or_default();
        if p.len() != 3 || !p.iter().all(|x| x.is_finite()) {
            continue;
        }
        let pos = Point(p[0], p[1], p[2]);
        if !pos.valid() {
            continue;
        }
        out.push(Member {
            uuid: uuid.chars().take(64).collect(),
            name: m
                .get("Name")
                .and_then(Value::as_str)
                .unwrap_or("")
                .chars()
                .take(64)
                .collect(),
            pos,
        });
    }
    out
}

/// A patch 0010 `cr` line (`WOWBG3 cr MEMBER GUID ENTRY LEVEL HP MAXHP REACT FACTION GROUP X Y Z O
/// DISPLAY NPCFLAGS FLAGS NAME...`) as (member, unit); None for a dead, player-owned or bad one.
/// REACT below 3 is hostile; a creature the member may not attack (no `a` flag) is a friendly NPC.
pub fn parse_cr(line: &str) -> Option<(String, Option<WowUnit>)> {
    let w: Vec<&str> = line.split_whitespace().collect();
    if w.first() != Some(&"WOWBG3") || w.get(1) != Some(&"cr") {
        return None;
    }
    let member = w.get(2)?.to_string();
    let guid = guid_ok(w.get(3)?)?;
    let f = |i: usize| {
        w.get(i)
            .and_then(|v| v.parse::<f64>().ok())
            .filter(|v| v.is_finite())
    };
    let u = |i: usize| w.get(i).and_then(|v| v.parse::<u32>().ok());
    let flags = *w.get(17)?;
    let (hp, react) = (u(6)?, u(8)?);
    if flags.contains('d') || flags.contains('p') || hp == 0 {
        return Some((member, None));
    }
    let attackable = flags.contains('a');
    let unit = WowUnit {
        guid,
        entry: u(4)?,
        level: u(5)?,
        hostile: attackable && react < 3,
        friendly: !attackable,
        pos: Point(f(11)?, f(12)?, f(13)?),
        orient: f(14)?,
        hp,
        max_hp: u(7)?.max(1),
        flags: if flags.contains('c') { FLAG_COMBAT } else { 0 },
        name: w
            .get(18..)
            .map(|n| n.join(" "))
            .unwrap_or_default()
            .chars()
            .filter(|c| !c.is_control())
            .take(64)
            .collect(),
        state: 0,
    };
    Some((member, unit.pos.valid().then_some(unit)))
}

/// The party's verbs to the server (patch 0010) and what they last sent.
#[derive(Debug, Default)]
pub struct PartyLink {
    /// bot name -> (ms, WoW position) of the last `move`.
    moved: HashMap<String, (u64, Point)>,
    /// bot name -> ms of the last `feed NAME on`.
    feeds: HashMap<String, u64>,
    /// The Far Sight target last asked for ("self" the hero).
    view: String,
    /// Server patch 0015's `walk` (the bots teleported
    /// after their members): the server answered a walk, so it has the verb.
    walk_ok: bool,
    /// bot name -> ms of a walk not answered yet.
    walk_pending: HashMap<String, u64>,
    /// Until this ms `move` is used alone: a walk went unanswered on a server without the verb.
    walk_off_until: u64,
    /// Bots whose last walk was refused (`far`, `busy`, ...): their next verb is a `move`.
    walk_refused: HashSet<String>,
}

/// Yards a bot's member moves before the bot follows, and how often at most (ms): the teleport
/// (`move`), for a member farther than [`BOT_WALK_REACH_YD`], on another map, or with no `walk`.
pub const BOT_MOVE_YD: f64 = 3.0;
pub const BOT_MOVE_MS: u64 = 1_000;
/// The walk (patch 0015 `walk NAME X Y Z MAP SECONDS`: a server spline at the member's own pace,
/// within 40 yd of the bot): after 1 yd, at most every 500 ms, the time the member took (0.3 to 2 s).
pub const BOT_WALK_YD: f64 = 1.0;
pub const BOT_WALK_MS: u64 = 500;
/// Inside the server's 40 yd, which it measures from where the bot is (still on its way).
pub const BOT_WALK_REACH_YD: f64 = 35.0;
/// No answer to a walk in this long and none ever came: a server without `walk`; `move` for a minute.
pub const BOT_WALK_WAIT_MS: u64 = 5_000;

impl PartyLink {
    /// The server's answer to a walk (`WOWBG3 walk NAME ok MAP X Y Z SPEED`, `WOWBG3 walk NAME refused
    /// WHY [...]`), from `server-events.txt`.
    pub fn event(&mut self, line: &str) {
        let mut w = line.split_whitespace();
        if w.next() != Some("WOWBG3") || w.next() != Some("walk") {
            return;
        }
        let Some(name) = w.next() else { return };
        self.walk_pending.remove(name);
        match w.next() {
            Some("ok") => {
                self.walk_ok = true;
                self.walk_refused.remove(name);
            }
            Some("refused") => {
                self.walk_ok = true;
                self.walk_refused.insert(name.to_string());
            }
            _ => {}
        }
    }

    /// The verbs due now. `link`: `party-wow.json` (member positions, world_bridge.py) and
    /// `farsight.json` (the controlled member); `io`: `party-bots.json` (bots online) and
    /// `wowchars.json` (member uuid -> WoW name), both link/tools/lifecycle.py's. `feed_seen`:
    /// bot name -> ms of its feed's last tick.
    pub fn step(
        &mut self,
        link: &Path,
        io: &Path,
        map: u32,
        now: u64,
        feed_seen: &HashMap<String, u64>,
    ) -> Vec<Handoff> {
        let mut out = Vec::new();
        let bots: HashSet<String> = read_value(&io.join("party-bots.json"))
            .and_then(|v| v.get("Bots")?.as_array().cloned())
            .unwrap_or_default()
            .iter()
            .filter_map(|b| b.as_str().map(String::from))
            .filter(|b| b.len() <= 12 && b.chars().all(|c| c.is_ascii_alphabetic()))
            .collect();
        if bots.is_empty() {
            return out;
        }
        let chars = read_value(&io.join("wowchars.json"))
            .and_then(|v| v.get("Chars").cloned())
            .unwrap_or(Value::Null);
        let name_of = |uuid: &str| {
            chars
                .get(uuid)
                .and_then(Value::as_str)
                .filter(|n| bots.contains(*n))
                .map(String::from)
        };
        let party = read_value(&link.join("party-wow.json")).unwrap_or(Value::Null);
        for m in party
            .get("Members")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            let Some(name) = m.get("Uuid").and_then(Value::as_str).and_then(name_of) else {
                continue;
            };
            let p: Vec<f64> = m
                .get("Wow")
                .and_then(Value::as_array)
                .map(|a| a.iter().filter_map(Value::as_f64).collect())
                .unwrap_or_default();
            if p.len() == 3
                && p.iter().all(|v| v.is_finite())
                && m.get("Dead").and_then(Value::as_bool) != Some(true)
            {
                let at = Point(p[0], p[1], p[2]);
                // A walk with no answer on a server that never answered one: no `walk` there.
                if let Some(t) = self.walk_pending.get(&name).copied()
                    && now >= t + BOT_WALK_WAIT_MS
                {
                    self.walk_pending.remove(&name);
                    if !self.walk_ok {
                        self.walk_off_until = now + 60_000;
                    }
                }
                let refused = self.walk_refused.contains(&name);
                // None: nothing due; Some(Some(seconds)): a walk; Some(None): a move.
                let verb = match self.moved.get(&name) {
                    None => Some(None),
                    Some((t, q)) => {
                        let d =
                            ((q.0 - at.0).powi(2) + (q.1 - at.1).powi(2) + (q.2 - at.2).powi(2))
                                .sqrt();
                        if now >= self.walk_off_until && !refused && d <= BOT_WALK_REACH_YD {
                            (now >= t + BOT_WALK_MS && d >= BOT_WALK_YD)
                                .then(|| Some(((now - t) as f64 / 1000.0).clamp(0.3, 2.0)))
                        } else {
                            (refused
                                || (now >= t + BOT_MOVE_MS
                                    && hdist(*q, at).max((q.2 - at.2).abs()) >= BOT_MOVE_YD))
                                .then_some(None)
                        }
                    }
                };
                match verb {
                    Some(Some(secs)) => {
                        self.moved.insert(name.clone(), (now, at));
                        self.walk_pending.entry(name.clone()).or_insert(now);
                        out.push(Handoff::Party {
                            line: format!(
                                "walk {name} {:.2} {:.2} {:.2} {map} {secs:.2}",
                                at.0, at.1, at.2
                            ),
                        });
                    }
                    Some(None) => {
                        self.moved.insert(name.clone(), (now, at));
                        self.walk_refused.remove(&name);
                        out.push(Handoff::Party {
                            line: format!("move {name} {:.2} {:.2} {:.2} {map}", at.0, at.1, at.2),
                        });
                    }
                    None => {}
                }
            }
            // The feed: on once, again when its ticks stopped for 30 s.
            let ticking = feed_seen.get(&name).is_some_and(|t| now < t + 30_000);
            if !ticking && self.feeds.get(&name).is_none_or(|t| now >= t + 30_000) {
                self.feeds.insert(name.clone(), now);
                out.push(Handoff::Party {
                    line: format!("feed {name} on 100 500"),
                });
            }
        }
        let fs = read_value(&link.join("farsight.json")).unwrap_or(Value::Null);
        let want = (fs.get("On").and_then(Value::as_bool) == Some(true))
            .then(|| fs.get("Uuid").and_then(Value::as_str).and_then(name_of))
            .flatten()
            .unwrap_or_else(|| "self".into());
        if self.view.is_empty() {
            self.view = "self".into();
        }
        if want != self.view {
            self.view = want.clone();
            out.push(Handoff::Party {
                line: format!("view {want}"),
            });
        }
        out
    }
}

/// The creatures round the party: the one place their source is chosen. Today benilla's view of
/// the hero (`mobs.txt`), plus per-member feeds `feed/<member>.txt` in the same `mob ...` line
/// format (a mob in both keeps benilla's line, which is fresher; files older than 5 s are left out).
/// The server's per-member feed
/// replaces or fills these files; nothing else changes.
pub fn read_feed(link: &Path) -> Option<Mobs> {
    let mut out = parse_mobs(&read_text(&link.join("mobs.txt")).ok()?);
    if let Ok(dir) = fs::read_dir(link.join("feed")) {
        let mut names: Vec<_> = dir
            .filter_map(Result::ok)
            .map(|e| e.path())
            .filter(|p| p.extension().is_some_and(|x| x == "txt"))
            // A feed older than 5 s is a member gone (or a feed that stopped): left out.
            .filter(|p| {
                fs::metadata(p)
                    .and_then(|m| m.modified())
                    .ok()
                    .and_then(|t| t.elapsed().ok())
                    .is_some_and(|age| age.as_secs_f64() < 5.0)
            })
            .collect();
        names.sort();
        for path in names.into_iter().take(16) {
            let Ok(text) = read_text(&path) else {
                continue;
            };
            for m in parse_mobs(&text).mobs {
                if !out.mobs.iter().any(|o| o.guid == m.guid) && out.mobs.len() < 512 {
                    out.mobs.push(m);
                }
            }
        }
    }
    Some(out)
}

fn guid_ok(g: &str) -> Option<String> {
    (g.len() == 16 && g.bytes().all(|b| b.is_ascii_hexdigit())).then(|| g.to_ascii_lowercase())
}

/// Parses `mobs.txt`; bad lines are skipped.
pub fn parse_mobs(text: &str) -> Mobs {
    let mut out = Mobs::default();
    for line in text.lines() {
        let w: Vec<&str> = line.split_whitespace().collect();
        let f = |i: usize| {
            w.get(i)
                .and_then(|v| v.parse::<f64>().ok())
                .filter(|v| v.is_finite())
        };
        let u = |i: usize| w.get(i).and_then(|v| v.parse::<u32>().ok());
        match w.first().copied() {
            Some("seq") => out.seq = w.get(1).and_then(|v| v.parse().ok()).unwrap_or(0),
            Some("hero") => {
                let parsed = (|| {
                    Some(WowUnit {
                        guid: guid_ok(w.get(1)?)?,
                        entry: 0,
                        level: u(8).unwrap_or(0),
                        hostile: false,
                        friendly: false,
                        pos: Point(f(2)?, f(3)?, f(4)?),
                        orient: f(5)?,
                        hp: u(6)?,
                        max_hp: u(7)?.max(1),
                        flags: 0,
                        name: String::new(),
                        state: u(9).filter(|s| *s <= 2).unwrap_or(0),
                    })
                })();
                if let Some(h) = parsed.filter(|h| h.pos.valid()) {
                    out.hero = Some(h);
                }
            }
            Some("mob") => {
                let parsed = (|| {
                    Some(WowUnit {
                        guid: guid_ok(w.get(1)?)?,
                        entry: u(2)?,
                        level: u(3)?,
                        hostile: u(4)? == 1,
                        friendly: u(4)? == 2,
                        pos: Point(f(5)?, f(6)?, f(7)?),
                        orient: f(8)?,
                        hp: u(9)?,
                        max_hp: u(10)?.max(1),
                        flags: u(11)?,
                        name: w
                            .get(12..)
                            .map(|n| n.join(" "))
                            .unwrap_or_default()
                            .chars()
                            .filter(|c| !c.is_control())
                            .take(64)
                            .collect(),
                        state: 0,
                    })
                })();
                if let Some(m) = parsed.filter(|m| m.pos.valid() && m.hp > 0) {
                    out.mobs.push(m);
                }
            }
            _ => {}
        }
    }
    out
}

/// World mode's mapping (`world.json`, `world_bridge.py`): BG3 = Centre + (dy, dz, -dx) x 0.9144
/// for a WoW delta from WowOrigin.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Mapping {
    pub wow_origin: [f64; 3],
    pub centre: [f64; 3],
}
impl Mapping {
    pub fn from_world(world: &Value) -> Option<Self> {
        if world.get("Active").and_then(Value::as_bool) != Some(true) {
            return None;
        }
        let v = |k: &str, i: usize| world.get(k)?.get(i)?.as_f64().filter(|x| x.is_finite());
        Some(Self {
            wow_origin: [v("WowOrigin", 0)?, v("WowOrigin", 1)?, v("WowOrigin", 2)?],
            centre: [v("Centre", 0)?, v("Centre", 1)?, v("Centre", 2)?],
        })
    }
    pub fn to_bg3(&self, p: Point) -> [f64; 3] {
        let (w, c) = (self.wow_origin, self.centre);
        [
            c[0] + (p.1 - w[1]) * METRES_PER_YARD,
            c[1] + (p.2 - w[2]) * METRES_PER_YARD,
            c[2] - (p.0 - w[0]) * METRES_PER_YARD,
        ]
    }
    /// The inverse of [`Mapping::to_bg3`]: a BG3 point in WoW yards.
    pub fn to_wow(&self, b: [f64; 3]) -> Point {
        let (w, c) = (self.wow_origin, self.centre);
        Point(
            w[0] - (b[2] - c[2]) / METRES_PER_YARD,
            w[1] + (b[0] - c[0]) / METRES_PER_YARD,
            w[2] + (b[1] - c[1]) / METRES_PER_YARD,
        )
    }
}
/// The hero's state as `mirror.json` says it (`HeroState`).
pub fn hero_state_name(state: u32) -> &'static str {
    match state {
        1 => "dead",
        2 => "ghost",
        _ => "alive",
    }
}
/// BG3's horizontal forward direction (x, z) for a WoW orientation: WoW faces (cos o, sin o) in
/// (x, y), which the mapping turns into (sin o, -cos o) in BG3's (x, z).
pub fn bg3_forward(orient: f64) -> [f64; 2] {
    [orient.sin(), -orient.cos()]
}

fn hdist(a: Point, b: Point) -> f64 {
    ((a.0 - b.0).powi(2) + (a.1 - b.1).powi(2)).sqrt()
}

/// One stand-in as `Mirrors.lua` reports it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MirrorState {
    pub uuid: String,
    pub hp: u32,
    pub max_hp: u32,
    /// In Tav's BG3 combat.
    pub in_combat: bool,
    pub dead: bool,
    /// Tav has hurt it (BG3's AttackedBy).
    pub hit_by_tav: bool,
    /// The stand-in's BG3 position (`Pos`), when reported.
    pub pos: Option<[f64; 3]>,
    /// A robbed NPC searching for the thief (`Searching`, Mirrors.lua): BG3 walks its stand-in.
    pub searching: bool,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Status {
    pub seq: u64,
    pub tav_hp: u32,
    pub tav_max_hp: u32,
    pub tav_combat: bool,
    pub tav_ftb: bool,
    pub tav_sneaking: bool,
    pub mobs: HashMap<String, MirrorState>,
}
pub fn parse_status(v: &Value) -> Status {
    let num = |v: &Value, k: &str| v.get(k).and_then(Value::as_u64).unwrap_or(0) as u32;
    let yes = |v: &Value, k: &str| v.get(k).and_then(Value::as_bool) == Some(true);
    let tav = v.get("Tav").cloned().unwrap_or(Value::Null);
    let mut s = Status {
        seq: v.get("Seq").and_then(Value::as_u64).unwrap_or(0),
        tav_hp: num(&tav, "Hp"),
        tav_max_hp: num(&tav, "MaxHp"),
        tav_combat: yes(&tav, "InCombat"),
        tav_ftb: yes(&tav, "Ftb"),
        tav_sneaking: yes(&tav, "Sneaking"),
        mobs: HashMap::new(),
    };
    if let Some(map) = v.get("Mobs").and_then(Value::as_object) {
        for (g, m) in map {
            let (Some(guid), Some(uuid)) = (guid_ok(g), m.get("Uuid").and_then(Value::as_str))
            else {
                continue;
            };
            s.mobs.insert(
                guid,
                MirrorState {
                    uuid: uuid.chars().take(64).collect(),
                    hp: num(m, "Hp"),
                    max_hp: num(m, "MaxHp"),
                    in_combat: yes(m, "InCombat"),
                    dead: yes(m, "Dead"),
                    hit_by_tav: yes(m, "HitByTav"),
                    searching: yes(m, "Searching"),
                    pos: m.get("Pos").and_then(Value::as_array).and_then(|a| {
                        let v: Vec<f64> = a.iter().filter_map(Value::as_f64).collect();
                        (v.len() == 3 && v.iter().all(|x| x.is_finite()))
                            .then(|| [v[0], v[1], v[2]])
                    }),
                },
            );
        }
    }
    s
}

/// What the bridge knows about the mirrors. Pure apart from `poll` and the clock.
#[derive(Debug, Default)]
pub struct Mirror {
    /// This bridge run (`mirror.json` Session): a new one makes the mod start over.
    pub session: String,
    epoch: Option<Instant>,
    pub mobs: Mobs,
    /// The other party members (`party-wow.json`): mobs round each are mirrored too.
    pub members: Vec<Member>,
    /// The server's per-member feeds (patch 0010): bot name -> guid -> creature, and each feed's
    /// last tick (ms).
    pub feeds: HashMap<String, HashMap<String, WowUnit>>,
    pub feed_seen: HashMap<String, u64>,
    /// The party verbs to the server.
    pub party: PartyLink,
    /// Turn-based mode outside combat: the server's hold around the party.
    pub turnhold: crate::turnhold::TurnHold,
    pub mapping: Option<Mapping>,
    /// The WoW map world mode runs on (`world.json` Map).
    pub map: u32,
    pub status: Status,
    /// Mobs of an ended fight: not mirrored or pulled again until this time (ms).
    pub settled: HashMap<String, u64>,
    /// What the server last said about arming: Some(true) armed in sight mode, Some(false) armed
    /// without it (or disarmed, or sight mode ended by a death).
    pub sight: Option<bool>,
    sight_sent: Option<u64>,
    /// World mode was seen in this bridge run; `sight 0 died` asks for a quick re-arm; the plain
    /// arm went out.
    world_seen: bool,
    resend: bool,
    plain_sent: bool,
    seq: u64,
    written_at: Option<u64>,
    mirrored_text: String,
}

/// Set by SIGTERM or SIGINT once [`catch_stop`] ran: the bridge's loops end at the next
/// [`Mirror::stop_if_asked`].
pub static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_stop(_signal: i32) {
    STOP.store(true, Ordering::Relaxed);
}

/// SIGTERM (systemd's stop) and SIGINT set [`STOP`] instead of ending the process at once.
pub fn catch_stop() {
    unsafe extern "C" {
        fn signal(signum: i32, handler: extern "C" fn(i32)) -> usize;
    }
    // SAFETY: libc's signal(2) with a handler that only stores to an atomic.
    unsafe {
        signal(15, on_stop);
        signal(2, on_stop);
    }
}

/// Milliseconds a mob of an ended fight stays unmirrored (its stand-in is respawned fresh, out of
/// BG3's combat, afterwards).
pub const SETTLE_MS: u64 = 60_000;

impl Mirror {
    pub fn new(session: &str) -> Self {
        Self {
            session: session.into(),
            epoch: Some(Instant::now()),
            ..Self::default()
        }
    }
    /// Milliseconds since the bridge started (the clock of `settled`).
    pub fn now(&self) -> u64 {
        self.epoch.map_or(0, |e| e.elapsed().as_millis() as u64)
    }
    /// A server feed line (patch 0010): `WOWBG3 cr MEMBER ...`, `crgone MEMBER GUID`, `tick MEMBER
    /// SEQ N ...`, `feed MEMBER off ...`.
    pub fn feed_event(&mut self, line: &str) {
        let now = self.now();
        if let Some((member, unit)) = parse_cr(line) {
            let set = self.feeds.entry(member).or_default();
            match unit {
                Some(u) if set.len() < 256 || set.contains_key(&u.guid) => {
                    set.insert(u.guid.clone(), u);
                }
                Some(_) => {}
                None => {
                    if let Some(g) = line.split_whitespace().nth(3).and_then(guid_ok) {
                        set.remove(&g);
                    }
                }
            }
            return;
        }
        let w: Vec<&str> = line.split_whitespace().collect();
        match (w.first(), w.get(1), w.get(2)) {
            (Some(&"WOWBG3"), Some(&"crgone"), Some(m)) => {
                if let (Some(set), Some(g)) =
                    (self.feeds.get_mut(*m), w.get(3).and_then(|g| guid_ok(g)))
                {
                    set.remove(&g);
                }
            }
            (Some(&"WOWBG3"), Some(&"tick"), Some(m)) => {
                self.feed_seen.insert((*m).to_string(), now);
            }
            (Some(&"WOWBG3"), Some(&"feed"), Some(m)) if w.get(3) == Some(&"off") => {
                self.feeds.remove(*m);
                self.feed_seen.remove(*m);
            }
            _ => {}
        }
    }
    /// The live feeds' creatures that benilla's own view lacks, added to `mobs`.
    fn merge_feeds(&mut self) {
        let now = self.now();
        for (member, set) in &self.feeds {
            if !self.feed_seen.get(member).is_some_and(|t| now < t + 5_000) {
                continue;
            }
            for u in set.values() {
                if self.mobs.mobs.len() < 512 && !self.mobs.mobs.iter().any(|m| m.guid == u.guid) {
                    self.mobs.mobs.push(u.clone());
                }
            }
        }
    }
    /// A server line about arming: `WOWBG3 armed 1 sight`, `armed 1`, `armed 0`, `sight 0 ...`.
    pub fn arm_event(&mut self, line: &str) {
        let w: Vec<&str> = line.split_whitespace().collect();
        match (w.first(), w.get(1), w.get(2), w.get(3)) {
            (Some(&"WOWBG3"), Some(&"armed"), Some(&"1"), Some(&"sight")) => {
                self.sight = Some(true)
            }
            // A death ends sight mode (`sight 0 died`): asked again at once (server p3 keeps a
            // request made while dead and applies it at resurrection).
            (Some(&"WOWBG3"), Some(&"sight"), _, _) => {
                self.sight = Some(false);
                self.resend = true;
            }
            (Some(&"WOWBG3"), Some(&"armed"), _, _) => self.sight = Some(false),
            _ => {}
        }
    }
    /// The arm request due now. Before world mode was seen: one plain `arm` (it keeps whatever
    /// sight mode the server has). Once world mode was seen in this bridge run (sticky, so a BG3
    /// or world restart never drops it): `arm sight` (BG3's stand-ins decide who notices Tav) until
    /// the server confirms it, again every 15 s, and at once after `sight 0 died` (at most every
    /// 2 s). `arm nosight` is benilla's, when it enters the world outside world mode.
    pub fn arm_due(&mut self, now: u64) -> Option<Handoff> {
        if self.mapping.is_some() && self.hero().is_some() {
            self.world_seen = true;
        }
        if !self.world_seen {
            if self.plain_sent {
                return None;
            }
            self.plain_sent = true;
            return Some(Handoff::Arm { sight: None });
        }
        let gap = if self.resend { 2_000 } else { 15_000 };
        if self.sight == Some(true) || self.sight_sent.is_some_and(|t| now < t + gap) {
            return None;
        }
        self.resend = false;
        self.sight_sent = Some(now);
        Some(Handoff::Arm { sight: Some(true) })
    }
    /// Leave a mob alone for 10 s (a refused hold).
    pub fn skip(&mut self, guid: &str, now: u64) {
        if !guid.is_empty() {
            self.settled.insert(guid.to_string(), now + 10_000);
        }
    }
    pub fn hero(&self) -> Option<&WowUnit> {
        self.mobs.hero.as_ref()
    }
    pub fn unit(&self, guid: &str) -> Option<&WowUnit> {
        self.mobs.mobs.iter().find(|m| m.guid == guid)
    }
    fn settled_now(&self, guid: &str, now: u64) -> bool {
        self.settled.get(guid).is_some_and(|&until| now < until)
    }
    pub fn settle(&mut self, guids: impl IntoIterator<Item = String>, now: u64) {
        for g in guids {
            self.settled.insert(g, now + SETTLE_MS);
        }
        self.settled.retain(|_, until| now < *until);
    }
    /// Where the party stands in WoW: the hero first, then the other members.
    pub fn anchors(&self) -> Vec<Point> {
        let mut out: Vec<Point> = self.hero().map(|h| h.pos).into_iter().collect();
        if !out.is_empty() {
            out.extend(self.members.iter().map(|m| m.pos));
        }
        out
    }
    /// A mob's horizontal distance to the nearest party member within 20 yd of its height (none:
    /// infinite).
    fn party_dist(&self, anchors: &[Point], p: Point) -> f64 {
        anchors
            .iter()
            .filter(|a| (p.2 - a.2).abs() < 20.)
            .map(|a| hdist(p, *a))
            .fold(f64::INFINITY, f64::min)
    }
    /// The mobs to mirror: the running fight's, then the nearest attackable others within
    /// `MIRROR_RANGE` of any party member (one already mirrored within `MIRROR_RANGE + KEEP_MARGIN`),
    /// at most `MAX_MIRRORS` (more with members: `MIRRORS_PER_MEMBER` each, up to
    /// `MAX_MIRRORS_PARTY`), then the nearest friendly NPCs within `FRIENDLY_RANGE` (at most
    /// `MAX_FRIENDLY`, a budget of their own): friendlies never take an attackable mob's stand-in.
    pub fn wanted(&self, fight: &[String], now: u64) -> Vec<&WowUnit> {
        let anchors = self.anchors();
        if anchors.is_empty() {
            return Vec::new();
        }
        let near_of = |friendly: bool, range: f64| {
            let mut v: Vec<(f64, &WowUnit)> = self
                .mobs
                .mobs
                .iter()
                .filter(|m| m.friendly == friendly)
                .filter(|m| !fight.contains(&m.guid) && !self.settled_now(&m.guid, now))
                .map(|m| (self.party_dist(&anchors, m.pos), m))
                .filter(|(d, m)| {
                    *d <= range
                        || (*d <= range + KEEP_MARGIN && self.status.mobs.contains_key(&m.guid))
                })
                .collect();
            v.sort_by(|a, b| a.0.total_cmp(&b.0));
            v.into_iter().map(|(_, m)| m).collect::<Vec<_>>()
        };
        let cap = (MAX_MIRRORS + MIRRORS_PER_MEMBER * (anchors.len() - 1)).min(MAX_MIRRORS_PARTY);
        let mut near = near_of(false, MIRROR_RANGE);
        near.truncate(cap);
        near.extend(near_of(true, FRIENDLY_RANGE).into_iter().take(MAX_FRIENDLY));
        let mut out: Vec<&WowUnit> = self
            .mobs
            .mobs
            .iter()
            .filter(|m| fight.contains(&m.guid))
            .collect();
        out.extend(near);
        out
    }
    /// `mirror.json` for the mod.
    #[allow(clippy::too_many_arguments)]
    pub fn plan(
        &self,
        seq: u64,
        session: &str,
        fight: &[String],
        stats: Option<&CreatureStats>,
        wolf_template: &str,
        hostile_faction: Option<&str>,
        neutral_faction: &str,
        now: u64,
    ) -> Value {
        let Some(map) = self.mapping.filter(|_| self.hero().is_some()) else {
            return json!({"Seq": seq, "Session": session, "Active": false});
        };
        let mobs: Vec<Value> = self
            .wanted(fight, now)
            .into_iter()
            .map(|m| {
                let mut c = crate::bridge::Combatant::new("mirror", &m.guid, m.pos, m.hp, m.max_hp);
                c.entry = m.entry;
                c.level = m.level;
                c.name = m.name.clone();
                crate::bridge::with_stats(&mut c, stats);
                let mut v = spec(&c, stats, wolf_template);
                let p = map.to_bg3(m.pos);
                v["Guid"] = json!(m.guid);
                v["Hostile"] = json!(m.hostile);
                // Friendly NPCs: BG3's crime reaction for a guard (it calls for help and fights)
                // or for anyone else (the narrator's line, then the fight); see Mirrors.lua.
                v["Friendly"] = json!(m.friendly);
                v["Guard"] = json!(
                    m.friendly
                        && stats
                            .and_then(|s| s.mob(m.entry, m.level))
                            .is_some_and(|s| s.guard)
                );
                v["Pos"] = json!(p);
                v["Dir"] = json!(bg3_forward(m.orient));
                v["Wow"] = json!([m.pos.0, m.pos.1, m.pos.2, m.orient]);
                v
            })
            .collect();
        let hero = self.hero();
        json!({"Seq": seq, "Session": session, "Active": true,
            "HeroWowMaxHp": hero.map(|h| h.max_hp), "Hero": hero.map(|h| map.to_bg3(h.pos)),
            "HeroWowHp": hero.map(|h| h.hp), "HeroState": hero.map(|h| hero_state_name(h.state)),
            "Hostile": hostile_faction, "Neutral": neutral_faction,
            "Mobs": mobs, "Fight": fight})
    }
    /// Mirrored mobs in Tav's BG3 combat (alive, not settled), nearest first.
    pub fn in_bg3_combat(&self, now: u64) -> Vec<String> {
        if !self.status.tav_combat {
            return Vec::new();
        }
        let mut out: Vec<&WowUnit> = self
            .mobs
            .mobs
            .iter()
            .filter(|m| !self.settled_now(&m.guid, now))
            .filter(|m| {
                self.status
                    .mobs
                    .get(&m.guid)
                    .is_some_and(|s| s.in_combat && !s.dead)
            })
            .collect();
        if let Some(h) = self.hero() {
            out.sort_by(|a, b| hdist(a.pos, h.pos).total_cmp(&hdist(b.pos, h.pos)));
        }
        out.into_iter().map(|m| m.guid.clone()).collect()
    }
    /// Mirrored mobs Tav attacked that BG3 left out of its combat (alive, not settled), nearest
    /// first: a miss on a neutral stand-in starts nothing in BG3, but in WoW any attack pulls.
    /// Not friendly NPCs: BG3's own crime flow decides whether a hit on one starts a fight (a
    /// 0-damage assault is only a warning there); its combat brings it in (`in_bg3_combat`).
    pub fn provoked(&self, now: u64) -> Vec<String> {
        let mut out: Vec<&WowUnit> = self
            .mobs
            .mobs
            .iter()
            .filter(|m| !m.friendly && !self.settled_now(&m.guid, now))
            .filter(|m| {
                self.status
                    .mobs
                    .get(&m.guid)
                    .is_some_and(|s| s.hit_by_tav && !s.in_combat && !s.dead)
            })
            .collect();
        if let Some(h) = self.hero() {
            out.sort_by(|a, b| hdist(a.pos, h.pos).total_cmp(&hdist(b.pos, h.pos)));
        }
        out.into_iter().map(|m| m.guid.clone()).collect()
    }
    /// Mirrored mobs whose stand-in BG3 killed outside any fight (one blow out of combat) while
    /// the WoW mob still lives: the kill still goes to the server.
    pub fn dead_unfought(&self, now: u64) -> Vec<String> {
        self.mobs
            .mobs
            .iter()
            .filter(|m| !self.settled_now(&m.guid, now))
            .filter(|m| {
                self.status
                    .mobs
                    .get(&m.guid)
                    .is_some_and(|s| s.dead && s.hit_by_tav)
            })
            .map(|m| m.guid.clone())
            .collect()
    }
    /// A pull (benilla's `pull.json` shape) for these mobs, from what benilla last reported.
    pub fn pull(&self, guids: &[String], seq: u64) -> Option<Value> {
        let h = self.hero()?;
        let unit = |m: &WowUnit| {
            json!({"guid": m.guid, "entry": m.entry, "name": m.name, "level": m.level,
                "pos": [m.pos.0, m.pos.1, m.pos.2], "hp": m.hp, "max_hp": m.max_hp,
                "provoke": m.friendly})
        };
        let enemies: Vec<Value> = guids
            .iter()
            .filter_map(|g| self.unit(g))
            .take(MAX_HELD)
            .map(unit)
            .collect();
        if enemies.is_empty() {
            return None;
        }
        Some(json!({"seq": seq, "map": 0, "hero": unit(h), "enemies": enemies, "from": "bg3"}))
    }
    /// A mob's guid as `begin`/`join` take it: `!GUID` for a friendly NPC, which the server turns
    /// on the player for the fight (server patch 0006).
    pub fn server_guid(&self, guid: &str) -> String {
        match self.unit(guid) {
            Some(u) if u.friendly => format!("!{guid}"),
            _ => guid.to_string(),
        }
    }
    /// `mirrored.txt` for benilla (an `s` line too for one searching: benilla holds its model where
    /// BG3 walks the stand-in).
    pub fn mirrored_text(&self) -> String {
        let mut lines: Vec<String> = self
            .status
            .mobs
            .iter()
            .flat_map(|(g, s)| {
                let first = if s.dead { format!("c {g}") } else { format!("m {g}") };
                let search = (s.searching && !s.dead).then(|| format!("s {g}"));
                std::iter::once(first).chain(search)
            })
            .collect();
        lines.sort();
        lines.join("\n") + "\n"
    }

    /// The bridge was asked to stop (SIGTERM or SIGINT, [`catch_stop`]): `mirror.json` says
    /// `Active false` and the process ends (so that a
    /// leftover `Active true` with the hero dead killed the hero after a load).
    pub fn stop_if_asked(&mut self, link: &Path) {
        if !STOP.load(Ordering::Relaxed) {
            return;
        }
        self.seq += 1;
        let plan = json!({"Seq": self.seq, "Session": self.session, "Active": false});
        let _ = write_atomic(&link.join("mirror.json"), plan.to_string().as_bytes());
        println!("bridge: stopped (mirror.json Active false)");
        std::process::exit(0);
    }
    /// Read `mobs.txt`, `world.json` and `mirror-status.json`; write `mirror.json` (every 250 ms)
    /// and `mirrored.txt` (on change).
    pub fn poll(
        &mut self,
        link: &Path,
        fight: &[String],
        stats: Option<&CreatureStats>,
        wolf_template: &str,
        hostile_faction: Option<&str>,
        neutral_faction: &str,
    ) -> Result<(), Error> {
        self.stop_if_asked(link);
        let now = self.now();
        if self.written_at.is_some_and(|t| now < t + 250) {
            return Ok(());
        }
        self.written_at = Some(now);
        if let Some(feed) = read_feed(link) {
            self.mobs = feed;
        }
        self.merge_feeds();
        self.members = read_value(&link.join("party-wow.json"))
            .map(|v| parse_members(&v))
            .unwrap_or_default();
        let world = read_value(&link.join("world.json"));
        self.map = world
            .as_ref()
            .and_then(|w| w.get("Map")?.as_u64())
            .unwrap_or(0) as u32;
        self.mapping = world.and_then(|w| Mapping::from_world(&w));
        if let Some(v) = read_value(&link.join("mirror-status.json")) {
            self.status = parse_status(&v);
        }
        self.seq += 1;
        let plan = self.plan(
            self.seq,
            &self.session,
            fight,
            stats,
            wolf_template,
            hostile_faction,
            neutral_faction,
            now,
        );
        write_atomic(&link.join("mirror.json"), plan.to_string().as_bytes())?;
        let text = self.mirrored_text();
        if text != self.mirrored_text {
            write_atomic(&link.join("mirrored.txt"), text.as_bytes())?;
            self.mirrored_text = text;
        }
        Ok(())
    }
}

fn read_text(path: &Path) -> std::io::Result<String> {
    let mut s = String::new();
    fs::File::open(path)?.take(262_144).read_to_string(&mut s)?;
    Ok(s)
}
fn read_value(path: &Path) -> Option<Value> {
    serde_json::from_str(&read_text(path).ok()?).ok()
}
fn write_atomic(path: &Path, bytes: &[u8]) -> Result<(), Error> {
    let tmp = path.with_extension("tmp");
    fs::write(&tmp, bytes).map_err(|e| e.to_string())?;
    fs::rename(&tmp, path).map_err(|e| e.to_string())
}

/// The guids of a list, as a set (helper for callers).
pub fn set(list: &[String]) -> HashSet<String> {
    list.iter().cloned().collect()
}
