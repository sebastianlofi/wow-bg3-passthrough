//! Scripted fixtures, not a BG3 rules engine. No sockets, game processes or game data.
use crate::{
    coords::{Basis, Point},
    wire::{Actor, Error, Message, Outcome, Rejection, Role},
};
use std::collections::{HashSet, VecDeque};

pub struct Fake {
    pub role: Role,
    pub encounter: String,
    pub queue: VecDeque<Message>,
    pub phase: &'static str,
    pub frozen: bool,
    pub credits: usize,
    pub paths: usize,
    pub effects: usize,
    pub rejections: usize,
    credited: HashSet<String>,
}
fn check(ok: bool) -> Result<(), Error> {
    if ok {
        Ok(())
    } else {
        Err("fake rejected invalid encounter, turn, action or phase".into())
    }
}
impl Fake {
    pub fn new(role: Role, encounter: &str) -> Self {
        Self {
            role,
            encounter: encounter.into(),
            queue: VecDeque::new(),
            phase: "idle",
            frozen: false,
            credits: 0,
            paths: 0,
            effects: 0,
            rejections: 0,
            credited: HashSet::new(),
        }
    }
    pub fn begin(&mut self) -> Result<(), Error> {
        check(
            self.role == Role::Wow
                && self.phase == "idle"
                && !self.credited.contains(&self.encounter),
        )?;
        self.frozen = true; // Fake server acknowledges freezing before Begin is queued.
        self.phase = "begin";
        self.queue.push_back(Message::Begin {
            encounter: self.encounter.clone(),
            freeze_receipt: "fake-server-frozen-1".into(),
            map: 0,
            basis: Basis {
                wow_origin: Point(-8949.95, -132.493, 83.5312),
                bridge_origin: Point(0., 0., 0.),
                yaw: 0.,
            },
            actors: vec![
                Actor {
                    id: "hero".into(),
                    wow_guid: "0000000000000001".into(),
                    position: Point(0., 0., 0.),
                    hp: 100,
                    max_hp: 100,
                },
                Actor {
                    id: "wolf".into(),
                    wow_guid: "f130000000000002".into(),
                    position: Point(4., 0., 0.),
                    hp: 12,
                    max_hp: 12,
                },
            ],
        });
        Ok(())
    }
    /// Fake server reconciliation, explicitly called by the harness after a lost link.
    /// The real server must reconcile its authoritative encounter journal before unfreezing.
    pub fn reconcile_disconnect(&mut self) {
        self.queue.clear();
        self.frozen = false;
        self.phase = "aborted";
    }
    pub fn apply(&mut self, m: &Message) -> Result<(), Error> {
        m.validate(self.role.other())?;
        check(m.encounter() == self.encounter && self.queue.len() < 8)?;
        let e = self.encounter.clone();
        if let Message::Abort { .. } = m {
            self.reconcile_disconnect();
            return Ok(());
        }
        match (self.role, m) {
            (Role::Bg3, Message::Begin { actors, .. }) => {
                check(
                    self.phase == "idle"
                        && actors.len() == 2
                        && actors[0].id == "hero"
                        && actors[1].id == "wolf",
                )?;
                self.phase = "move";
                self.queue.push_back(Message::Ready {
                    encounter: e.clone(),
                });
                self.queue.push_back(Message::Turn {
                    encounter: e,
                    turn: 1,
                    actor: "hero".into(),
                });
            }
            (Role::Wow, Message::Ready { .. }) => {
                check(self.phase == "begin" && self.frozen)?;
                self.phase = "ready";
            }
            (Role::Wow, Message::Turn { turn, actor, .. }) => {
                check(self.phase == "ready" && *turn == 1 && actor == "hero")?;
                // First an out-of-reach move, which the BG3 side must refuse with a typed reason.
                self.phase = "move0";
                self.queue.push_back(Message::Move {
                    encounter: e,
                    turn: 1,
                    action: "move-0".into(),
                    actor: "hero".into(),
                    target: Point(40., 0., 0.),
                });
            }
            (
                Role::Wow,
                Message::Rejected {
                    turn,
                    action,
                    reason,
                    ..
                },
            ) => {
                check(
                    self.phase == "move0"
                        && *turn == 1
                        && action == "move-0"
                        && *reason == Rejection::NoPath,
                )?;
                self.rejections += 1;
                self.phase = "move";
                self.queue.push_back(Message::Move {
                    encounter: e,
                    turn: 1,
                    action: "move-1".into(),
                    actor: "hero".into(),
                    target: Point(2., 0., 0.),
                });
            }
            (
                Role::Bg3,
                Message::Move {
                    turn,
                    action,
                    actor,
                    target,
                    ..
                },
            ) if action == "move-0" => {
                check(
                    self.phase == "move"
                        && *turn == 1
                        && actor == "hero"
                        && *target == Point(40., 0., 0.),
                )?;
                self.queue.push_back(Message::Rejected {
                    encounter: e,
                    turn: 1,
                    action: action.clone(),
                    reason: Rejection::NoPath,
                    detail: "fixture: beyond the movement budget".into(),
                });
            }
            (
                Role::Bg3,
                Message::Move {
                    turn,
                    action,
                    actor,
                    target,
                    ..
                },
            ) => {
                check(
                    self.phase == "move"
                        && *turn == 1
                        && action == "move-1"
                        && actor == "hero"
                        && *target == Point(2., 0., 0.),
                )?;
                self.phase = "cast";
                self.queue.push_back(Message::Path {
                    encounter: e,
                    turn: 1,
                    action: action.clone(),
                    actor: actor.clone(),
                    points: vec![Point(0., 0., 0.), Point(1., 0.2, 0.), *target],
                });
            }
            (
                Role::Wow,
                Message::Path {
                    turn,
                    action,
                    actor,
                    points,
                    ..
                },
            ) => {
                check(
                    self.phase == "move"
                        && *turn == 1
                        && action == "move-1"
                        && actor == "hero"
                        && points.first() == Some(&Point(0., 0., 0.))
                        && points.last() == Some(&Point(2., 0., 0.)),
                )?;
                self.paths += 1;
                self.phase = "cast";
                self.queue.push_back(Message::Cast {
                    encounter: e,
                    turn: 1,
                    action: "cast-1".into(),
                    actor: "hero".into(),
                    target: "wolf".into(),
                    spell: "Fireball".into(),
                });
            }
            (
                Role::Bg3,
                Message::Cast {
                    turn,
                    action,
                    actor,
                    target,
                    spell,
                    ..
                },
            ) => {
                check(
                    self.phase == "cast"
                        && *turn == 1
                        && action == "cast-1"
                        && actor == "hero"
                        && target == "wolf"
                        && spell == "Fireball",
                )?;
                self.phase = "end_turn";
                self.queue.push_back(Message::Effect {
                    encounter: e,
                    turn: 1,
                    action: action.clone(),
                    target: target.clone(),
                    hp: 0,
                    max_hp: 12,
                    animation: "human-cast-fixture".into(),
                });
            }
            (
                Role::Wow,
                Message::Effect {
                    turn,
                    action,
                    target,
                    hp,
                    max_hp,
                    ..
                },
            ) => {
                check(
                    self.phase == "cast"
                        && *turn == 1
                        && action == "cast-1"
                        && target == "wolf"
                        && *hp == 0
                        && *max_hp == 12,
                )?;
                self.effects += 1;
                self.phase = "finish";
                self.queue.push_back(Message::EndTurn {
                    encounter: e,
                    turn: 1,
                });
            }
            (Role::Bg3, Message::EndTurn { turn, .. }) => {
                check(self.phase == "end_turn" && *turn == 1)?;
                self.phase = "cleanup";
                self.queue.push_back(Message::Finished {
                    encounter: e,
                    outcome: Outcome::Victory,
                });
            }
            (Role::Wow, Message::Finished { outcome, .. }) => {
                check(
                    self.phase == "finish"
                        && *outcome == Outcome::Victory
                        && !self.credited.contains(&e),
                )?;
                self.credited.insert(e.clone());
                self.credits += 1;
                self.phase = "cleanup";
                self.queue.push_back(Message::Cleanup { encounter: e });
            }
            (Role::Bg3, Message::Cleanup { .. }) => {
                check(self.phase == "cleanup")?;
                self.phase = "done";
                self.queue.push_back(Message::Cleaned { encounter: e });
            }
            (Role::Wow, Message::Cleaned { .. }) => {
                check(self.phase == "cleanup")?;
                self.frozen = false;
                self.phase = "done";
            }
            _ => return Err("fake rejected unexpected message".into()),
        }
        Ok(())
    }
}
