//! Server handoff plug points for world fights. The bridge raises one
//! [`Handoff`] at each fixed point of an encounter and a [`ServerHandoff`] carries it to the
//! server. The server patch (patches/vmangos) takes them as
//! `.wowbg3` chat commands from the player's own session, which benilla owns, so [`Ledger`], the
//! default, appends them as `SEQ .wowbg3 ...` lines to `server-cmd.txt` in the link folder
//! (benilla sends new lines as SAY chat when `WOW_BG3_M5=1`) and every request to
//! `handoff.jsonl`. Without the server patch nothing is held: the real mobs keep their WoW AI during a BG3
//! fight, and a BG3 kill gives no WoW credit.
use crate::Error;
use serde_json::json;
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

/// Mob guids per `begin`/`join` chat line (16 hex digits and a comma each, under WoW's 255).
pub const GUIDS_PER_LINE: usize = 12;

/// One request to the server, in encounter order.
#[derive(Clone, Debug, PartialEq)]
pub enum Handoff {
    /// Bridge ready: hold mobs the moment they enter combat with the player (`arm`). `sight`
    /// (world mode, server patch 0002): hostile mobs never notice the player by proximity or line
    /// of sight; BG3's stand-ins decide, and the bridge sends `begin` when one spots Tav
    /// (`arm sight`); `nosight` ends sight mode (server p3); None: plain `arm`, which leaves it as
    /// it is.
    Arm { sight: Option<bool> },
    /// The pull: hold (freeze) these mobs for the BG3 encounter. GUIDs as 16 hex digits.
    Begin {
        encounter: String,
        mobs: Vec<String>,
    },
    /// Adds joining the running encounter: hold them too.
    Join {
        encounter: String,
        mobs: Vec<String>,
    },
    /// A mob's hit points as BG3 has them (its stand-in's, a level, not a delta): the server sets
    /// the same fraction of its WoW HP, attributed to the player. 0 is the kill, with normal XP,
    /// loot and quest credit once (the server dedupes repeats).
    Hp {
        encounter: String,
        mob: String,
        hp: u32,
        max_hp: u32,
    },
    /// Where a mob stands in WoW yards as BG3's fight left it (server patch 0004, sent right before
    /// its kill): the server moves the held mob there, so its corpse lies where the WoW model fell
    /// and the player can loot it ("corpse too far away").
    Pos {
        encounter: String,
        mob: String,
        x: f64,
        y: f64,
        z: f64,
    },
    /// Where a dead mob's body came to rest in BG3, in WoW yards (server patch 0029): a stand-in
    /// that dies while a spell pushes, pulls or throws it lands somewhere else than where it
    /// died, and WoW's corpse goes there. No encounter: the body may land after the fight's end.
    Corpse { mob: String, x: f64, y: f64, z: f64 },
    /// Someone in BG3 carries this dead mob's body (server patch 0031): WoW keeps the corpse (no decay, no
    /// respawn) until it is put down. Said again while it is carried.
    CorpseHold { mob: String },
    /// Tav's hit points as BG3 has them (the WoW character's HP follows
    /// Tav's as a fraction; Tav dying kills it). Shape `hero ENC TAVHP TAVMAX`, agreed with the server patch.
    Hero {
        encounter: String,
        hp: u32,
        max_hp: u32,
    },
    /// Heartbeat while the encounter runs (the server ends it after 120 s without a command).
    Keep { encounter: String },
    /// Server patch 0009 (link/tools/faction.sh): the player counts as `side`
    /// (horde, alliance or own) until logout.
    Faction { side: String },
    /// A mob that could not join BG3's fight. The server patch has no verb for it: `end` releases it.
    Release { encounter: String, mob: String },
    /// The encounter is over: `victory`, `defeat`, `fled` or `abort`; living mobs go home.
    End { encounter: String, outcome: String },
    /// A party member verb of server patch 0010: `move NAME X Y Z MAP`,
    /// `feed NAME on RANGE MS`, `view NAME|self` (`crate::mirror::PartyLink`).
    Party { line: String },
}

impl Handoff {
    /// The server chat command, or None for a request the server has no verb for.
    pub fn command(&self) -> Option<String> {
        Some(match self {
            Self::Arm { sight: Some(true) } => ".wowbg3 arm sight".into(),
            Self::Arm { sight: Some(false) } => ".wowbg3 arm nosight".into(),
            Self::Arm { sight: None } => ".wowbg3 arm".into(),
            Self::Begin { encounter, mobs } => {
                format!(".wowbg3 begin {encounter} {}", mobs.join(","))
            }
            Self::Join { encounter, mobs } => {
                format!(".wowbg3 join {encounter} {}", mobs.join(","))
            }
            Self::Hp {
                encounter,
                mob,
                hp,
                max_hp,
            } => format!(".wowbg3 hp {encounter} {mob} {hp} {max_hp}"),
            Self::Pos {
                encounter,
                mob,
                x,
                y,
                z,
            } => format!(".wowbg3 pos {encounter} {mob} {x:.2} {y:.2} {z:.2}"),
            Self::Corpse { mob, x, y, z } => format!(".wowbg3 corpse {mob} {x:.2} {y:.2} {z:.2}"),
            Self::CorpseHold { mob } => format!(".wowbg3 corpse {mob} hold"),
            Self::Hero {
                encounter,
                hp,
                max_hp,
            } => format!(".wowbg3 hero {encounter} {hp} {max_hp}"),
            Self::Keep { encounter } => format!(".wowbg3 keep {encounter}"),
            Self::Faction { side } => format!(".wowbg3 faction {side}"),
            Self::Release { .. } => return None,
            Self::Party { line } => format!(".wowbg3 {line}"),
            Self::End { encounter, outcome } => format!(".wowbg3 end {encounter} {outcome}"),
        })
    }
    /// Every chat line for this request: a Begin or Join of more mobs than one 255-character chat
    /// line holds goes out as `begin` with the first [`GUIDS_PER_LINE`] and `join` lines for the
    /// rest (encounters take up to 50 mobs).
    pub fn commands(&self) -> Vec<String> {
        // A join line goes for one mob each: the server refuses a whole line if one of its mobs is in the fight
        // already (a pack mate it took by itself, 0015), and the others then stay free in WoW.
        let joins = |encounter: &str, mobs: &[String]| -> Vec<String> {
            mobs.iter().map(|m| format!(".wowbg3 join {encounter} {m}")).collect()
        };
        match self {
            Self::Begin { encounter, mobs } if mobs.len() > GUIDS_PER_LINE => {
                let (first, rest) = mobs.split_at(GUIDS_PER_LINE);
                let mut lines = vec![format!(".wowbg3 begin {encounter} {}", first.join(","))];
                lines.extend(joins(encounter, rest));
                lines
            }
            Self::Join { encounter, mobs } => joins(encounter, mobs),
            _ => self.command().into_iter().collect(),
        }
    }
    /// The request for the log, also when it has no server command.
    pub fn describe(&self) -> String {
        match self {
            Self::Release { encounter, mob } => format!("(release {encounter} {mob})"),
            _ => self.command().unwrap_or_default(),
        }
    }
}

/// Where handoff requests go. `begin` returns the freeze receipt the Begin message carries.
pub trait ServerHandoff {
    fn begin(&mut self, h: &Handoff) -> Result<String, Error>;
    fn send(&mut self, h: &Handoff) -> Result<(), Error>;
}

/// Default handoff: `server-cmd.txt` for benilla (the last 64 commands, `SEQ COMMAND`, SEQ rising
/// across bridge runs) and `handoff.jsonl` for review. The receipt says no server hold exists.
pub struct Ledger {
    dir: PathBuf,
    seq: u64,
    lines: Vec<String>,
}

impl Ledger {
    pub fn new(dir: &Path) -> Self {
        let lines: Vec<String> = fs::read_to_string(dir.join("server-cmd.txt"))
            .unwrap_or_default()
            .lines()
            .map(str::to_string)
            .collect();
        let seq = lines
            .iter()
            .filter_map(|l| l.split_once(' ')?.0.parse::<u64>().ok())
            .max()
            .unwrap_or(0);
        Self {
            dir: dir.into(),
            seq,
            lines,
        }
    }
    fn record(&mut self, h: &Handoff) -> Result<(), Error> {
        let commands = h.commands();
        for command in &commands {
            self.seq += 1;
            self.lines.push(format!("{} {command}", self.seq));
            if self.lines.len() > 64 {
                self.lines.remove(0);
            }
        }
        if !commands.is_empty() {
            let tmp = self.dir.join("server-cmd.tmp");
            fs::write(&tmp, self.lines.join("\n") + "\n").map_err(|e| e.to_string())?;
            fs::rename(&tmp, self.dir.join("server-cmd.txt")).map_err(|e| e.to_string())?;
        }
        let ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| d.as_millis() as u64);
        let mut log = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(self.dir.join("handoff.jsonl"))
            .map_err(|e| e.to_string())?;
        writeln!(log, "{}", json!({"ms": ms, "request": h.describe()})).map_err(|e| e.to_string())
    }
}

impl ServerHandoff for Ledger {
    fn begin(&mut self, h: &Handoff) -> Result<String, Error> {
        self.record(h)?;
        Ok("no-server-freeze-m4".into())
    }
    fn send(&mut self, h: &Handoff) -> Result<(), Error> {
        self.record(h)
    }
}
