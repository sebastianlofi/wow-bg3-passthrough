use crate::coords::{Basis, Point};
use serde::{Deserialize, Serialize};

pub const VERSION: u16 = 1;
pub const MAX_FRAME: usize = 65_536;
pub const MAX_ACTORS: usize = 64;
pub const MAX_PATH: usize = 256;
pub const MAX_SEQUENCE: u64 = (1 << 53) - 1;
pub type Error = String;
fn require(ok: bool, why: &str) -> Result<(), Error> {
    if ok { Ok(()) } else { Err(why.into()) }
}
pub fn id_ok(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 64
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"_-".contains(&b))
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Role {
    Wow,
    Bg3,
}
impl Role {
    pub fn other(self) -> Self {
        match self {
            Self::Wow => Self::Bg3,
            Self::Bg3 => Self::Wow,
        }
    }
    pub fn file(self) -> &'static str {
        match self {
            Self::Wow => "wow.json",
            Self::Bg3 => "bg3.json",
        }
    }
}
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Actor {
    pub id: String,
    /// Hex string keeps the 64-bit WoW GUID exact across JSON/Lua.
    pub wow_guid: String,
    pub position: Point,
    pub hp: u32,
    pub max_hp: u32,
}
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum Message {
    Begin {
        encounter: String,
        freeze_receipt: String,
        map: u32,
        basis: Basis,
        actors: Vec<Actor>,
    },
    Ready {
        encounter: String,
    },
    Turn {
        encounter: String,
        turn: u32,
        actor: String,
    },
    Move {
        encounter: String,
        turn: u32,
        action: String,
        actor: String,
        target: Point,
    },
    Path {
        encounter: String,
        turn: u32,
        action: String,
        actor: String,
        points: Vec<Point>,
    },
    Cast {
        encounter: String,
        turn: u32,
        action: String,
        actor: String,
        target: String,
        spell: String,
    },
    Effect {
        encounter: String,
        turn: u32,
        action: String,
        target: String,
        hp: u32,
        max_hp: u32,
        animation: String,
    },
    EndTurn {
        encounter: String,
        turn: u32,
    },
    Finished {
        encounter: String,
        outcome: Outcome,
    },
    Cleanup {
        encounter: String,
    },
    Cleaned {
        encounter: String,
    },
    /// BG3 accepted the request for an attempt, then its normal engine path refused it.
    /// The action id is spent; the turn continues and WoW may send a new action.
    Rejected {
        encounter: String,
        turn: u32,
        action: String,
        reason: Rejection,
        detail: String,
    },
    Abort {
        encounter: String,
        reason: String,
    },
    /// BG3 to WoW: one action a combatant BG3 controls (an enemy stand-in, later a BG3-run
    /// party member) took during its own turn, so WoW can play it on that combatant's model.
    /// `points` is the walked path in bridge coordinates (empty when it did not move), `spell`
    /// the BG3 spell or attack ("none" when it only moved) and `target` the aimed actor
    /// ("none" without one). Damage follows as Effect messages carrying the same action id.
    Acted {
        encounter: String,
        turn: u32,
        action: String,
        actor: String,
        spell: String,
        target: String,
        points: Vec<Point>,
    },
    /// WoW to BG3 (world fights): more enemies join the running encounter (adds that
    /// arrive, or a mob provoked mid-fight). Same actor rules as Begin; ids are new to the
    /// encounter. BG3 sets their stand-ins up and answers with a Roster whose `joins` counts it.
    Join {
        encounter: String,
        actors: Vec<Actor>,
    },
    /// BG3 to WoW: the combatants in BG3's fight as BG3 has them (stand-ins at their template's HP),
    /// positions in bridge coordinates. Sent once setup is done (before Ready), after each Join
    /// (`joins` = Join messages handled so far; a joined actor missing from that Roster could
    /// not join) and when hit points changed without an Effect (a direct BG3 action).
    Roster {
        encounter: String,
        joins: u32,
        actors: Vec<Actor>,
    },
}
/// Why the engine refused a requested action. Categories come from what the game reports
/// through its normal player path (tested in game), never from a homegrown rules check.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Rejection {
    /// The engine reports it is not this actor's turn.
    NotYourTurn,
    /// The engine disables the action (exhausted action, resource or other UI refusal).
    Unavailable,
    /// The engine found no path to the requested point.
    NoPath,
    /// The requested target or point could not be picked on screen.
    CannotAim,
    /// The request was issued but the engine reported no cast, movement or effect.
    NoEffect,
}
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Outcome {
    Victory,
    Defeat,
    Fled,
}
impl Message {
    pub fn encounter(&self) -> &str {
        match self {
            Self::Begin { encounter, .. }
            | Self::Ready { encounter }
            | Self::Turn { encounter, .. }
            | Self::Move { encounter, .. }
            | Self::Path { encounter, .. }
            | Self::Cast { encounter, .. }
            | Self::Effect { encounter, .. }
            | Self::EndTurn { encounter, .. }
            | Self::Finished { encounter, .. }
            | Self::Cleanup { encounter }
            | Self::Cleaned { encounter }
            | Self::Rejected { encounter, .. }
            | Self::Abort { encounter, .. }
            | Self::Acted { encounter, .. }
            | Self::Join { encounter, .. }
            | Self::Roster { encounter, .. } => encounter,
        }
    }
    pub fn validate(&self, sender: Role) -> Result<(), Error> {
        require(id_ok(self.encounter()), "invalid encounter")?;
        let direction = match self {
            Self::Begin { .. }
            | Self::Move { .. }
            | Self::Cast { .. }
            | Self::EndTurn { .. }
            | Self::Cleanup { .. }
            | Self::Join { .. } => Role::Wow,
            Self::Ready { .. }
            | Self::Turn { .. }
            | Self::Path { .. }
            | Self::Effect { .. }
            | Self::Finished { .. }
            | Self::Cleaned { .. }
            | Self::Rejected { .. }
            | Self::Acted { .. }
            | Self::Roster { .. } => Role::Bg3,
            Self::Abort { reason, .. } => {
                require(
                    !reason.is_empty() && reason.len() <= 256,
                    "invalid abort reason",
                )?;
                sender
            }
        };
        require(direction == sender, "wrong message direction")?;
        match self {
            Self::Begin {
                freeze_receipt,
                basis,
                actors,
                ..
            } => {
                require(
                    id_ok(freeze_receipt) && basis.valid(),
                    "invalid freeze receipt or coordinate basis",
                )?;
                actors_ok(actors)?;
            }
            Self::Join { actors, .. } | Self::Roster { actors, .. } => actors_ok(actors)?,
            Self::Turn { turn, actor, .. } => require(*turn > 0 && id_ok(actor), "invalid turn")?,
            Self::Move {
                turn,
                action,
                actor,
                target,
                ..
            } => require(
                *turn > 0 && id_ok(action) && id_ok(actor) && target.valid(),
                "invalid move",
            )?,
            Self::Path {
                turn,
                action,
                actor,
                points,
                ..
            } => require(
                *turn > 0
                    && id_ok(action)
                    && id_ok(actor)
                    && !points.is_empty()
                    && points.len() <= MAX_PATH
                    && points.iter().all(|p| p.valid()),
                "invalid path",
            )?,
            Self::Cast {
                turn,
                action,
                actor,
                target,
                spell,
                ..
            } => require(
                *turn > 0 && [action, actor, target, spell].iter().all(|s| id_ok(s)),
                "invalid cast",
            )?,
            Self::Effect {
                turn,
                action,
                target,
                hp,
                max_hp,
                animation,
                ..
            } => require(
                *turn > 0
                    && [action, target, animation].iter().all(|s| id_ok(s))
                    && *max_hp > 0
                    && hp <= max_hp,
                "invalid effect",
            )?,
            Self::EndTurn { turn, .. } => require(*turn > 0, "invalid turn")?,
            Self::Acted {
                turn,
                action,
                actor,
                spell,
                target,
                points,
                ..
            } => require(
                *turn > 0
                    && [action, actor, spell, target].iter().all(|s| id_ok(s))
                    && points.len() <= MAX_PATH
                    && points.iter().all(|p| p.valid()),
                "invalid combatant action",
            )?,
            Self::Rejected {
                turn,
                action,
                detail,
                ..
            } => require(
                *turn > 0 && id_ok(action) && detail.len() <= 256,
                "invalid rejection",
            )?,
            _ => {}
        }
        Ok(())
    }
}
/// Actor lists (Begin, Join, Roster): 1 to 64 unique ids, exact GUIDs, valid state.
fn actors_ok(actors: &[Actor]) -> Result<(), Error> {
    require(
        !actors.is_empty() && actors.len() <= MAX_ACTORS,
        "invalid actor count",
    )?;
    let mut seen = std::collections::HashSet::new();
    for a in actors {
        require(
            id_ok(&a.id) && seen.insert(&a.id),
            "invalid or duplicate actor",
        )?;
        require(
            a.wow_guid.len() == 16 && a.wow_guid.bytes().all(|b| b.is_ascii_hexdigit()),
            "invalid WoW GUID",
        )?;
        require(
            a.position.valid() && a.max_hp > 0 && a.hp <= a.max_hp,
            "invalid actor state",
        )?;
    }
    Ok(())
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Frame {
    pub version: u16,
    pub session: String,
    pub sender: Role,
    pub heartbeat: u64,
    pub seq: u64,
    pub ack: u64,
    pub message: Option<Message>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Envelope {
    body: String,
    bytes: usize,
    checksum: u32,
}
/// FNV-1a detects torn/mixed file writes. It is not authentication.
fn checksum(bytes: &[u8]) -> u32 {
    bytes.iter().fold(2_166_136_261, |hash, b| {
        (hash ^ u32::from(*b)).wrapping_mul(16_777_619)
    })
}
pub fn encode(frame: &Frame) -> Result<Vec<u8>, Error> {
    let body = serde_json::to_string(frame).map_err(|e| e.to_string())?;
    let envelope = Envelope {
        bytes: body.len(),
        checksum: checksum(body.as_bytes()),
        body,
    };
    let bytes = serde_json::to_vec(&envelope).map_err(|e| e.to_string())?;
    require(bytes.len() <= MAX_FRAME, "frame too large")?;
    Ok(bytes)
}
pub fn decode(bytes: &[u8]) -> Result<Frame, Error> {
    require(bytes.len() <= MAX_FRAME, "frame too large")?;
    let envelope: Envelope = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
    require(
        envelope.bytes == envelope.body.len()
            && envelope.checksum == checksum(envelope.body.as_bytes()),
        "incomplete or corrupt frame",
    )?;
    let f: Frame = serde_json::from_str(&envelope.body).map_err(|e| e.to_string())?;
    require(f.version == VERSION, "unsupported version")?;
    require(
        f.heartbeat > 0
            && f.heartbeat <= MAX_SEQUENCE
            && f.seq <= MAX_SEQUENCE
            && f.ack <= MAX_SEQUENCE,
        "counter out of range",
    )?;
    require(
        (f.seq > 0) == f.message.is_some(),
        "sequence/message mismatch",
    )?;
    if let Some(m) = &f.message {
        m.validate(f.sender)?;
    }
    Ok(f)
}
/// One outstanding message per direction. Application validation precedes acknowledgement.
/// Restart both endpoints with a fresh session after timeout; never resume ambiguous effects.
pub struct Endpoint {
    session: String,
    role: Role,
    sent: u64,
    received: u64,
    heartbeat: u64,
    peer_heartbeat: u64,
    last_seen: u64,
    timeout_ms: u64,
    pending: Option<Message>,
    last_message: Option<Message>,
    failed: bool,
}
impl Endpoint {
    pub fn new(session: &str, role: Role, now: u64, timeout_ms: u64) -> Result<Self, Error> {
        require(
            session.len() == 32 && session.bytes().all(|b| b.is_ascii_hexdigit()),
            "session must be 128-bit hex",
        )?;
        require(timeout_ms > 0, "timeout must be positive")?;
        Ok(Self {
            session: session.into(),
            role,
            sent: 0,
            received: 0,
            heartbeat: 0,
            peer_heartbeat: 0,
            last_seen: now,
            timeout_ms,
            pending: None,
            last_message: None,
            failed: false,
        })
    }
    pub fn busy(&self) -> bool {
        self.pending.is_some()
    }
    pub fn tick(&mut self, now: u64) -> Result<(), Error> {
        if now.saturating_sub(self.last_seen) > self.timeout_ms {
            self.failed = true;
        }
        require(
            !self.failed,
            "link expired: reconcile encounter and use a fresh session",
        )
    }
    pub fn offer(&mut self, message: Message) -> Result<(), Error> {
        require(
            !self.failed && !self.busy() && self.sent < MAX_SEQUENCE,
            "endpoint busy, expired or exhausted",
        )?;
        message.validate(self.role)?;
        let probe = Frame {
            version: VERSION,
            session: self.session.clone(),
            sender: self.role,
            heartbeat: 1,
            seq: self.sent + 1,
            ack: self.received,
            message: Some(message.clone()),
        };
        encode(&probe)?;
        self.sent += 1;
        self.pending = Some(message);
        Ok(())
    }
    pub fn frame(&mut self) -> Result<Vec<u8>, Error> {
        require(
            !self.failed && self.heartbeat < MAX_SEQUENCE,
            "endpoint expired or exhausted",
        )?;
        self.heartbeat += 1;
        encode(&Frame {
            version: VERSION,
            session: self.session.clone(),
            sender: self.role,
            heartbeat: self.heartbeat,
            seq: if self.busy() { self.sent } else { 0 },
            ack: self.received,
            message: self.pending.clone(),
        })
    }
    pub fn receive(
        &mut self,
        bytes: &[u8],
        now: u64,
        mut apply: impl FnMut(&Message) -> Result<(), Error>,
    ) -> Result<bool, Error> {
        self.tick(now)?;
        let f = decode(bytes)?;
        require(
            f.session == self.session && f.sender == self.role.other(),
            "stale session or wrong peer",
        )?;
        require(f.ack <= self.sent, "acknowledgement from the future")?;
        if f.heartbeat <= self.peer_heartbeat {
            return Ok(false);
        }
        let mut delivered = false;
        if let Some(m) = &f.message {
            if f.seq == self.received + 1 {
                apply(m)?;
                self.received = f.seq;
                self.last_message = Some(m.clone());
                delivered = true;
            } else {
                require(
                    f.seq == self.received && self.last_message.as_ref() == Some(m),
                    "sequence gap or conflicting retry",
                )?;
            }
        }
        if f.ack == self.sent {
            self.pending = None;
        }
        self.peer_heartbeat = f.heartbeat;
        self.last_seen = now;
        Ok(delivered)
    }
}
