use wow_bg3_link::{
    Endpoint,
    coords::{Basis, METRES_PER_YARD, Point},
    fake::Fake,
    wire::{self, Frame, MAX_FRAME, Message, Rejection, Role, VERSION},
};
const SESSION: &str = "0123456789abcdef0123456789abcdef";
fn endpoint(role: Role) -> Endpoint {
    Endpoint::new(SESSION, role, 0, 1000).unwrap()
}
fn begin() -> Message {
    let mut f = Fake::new(Role::Wow, "enc-1");
    f.begin().unwrap();
    f.queue.pop_front().unwrap()
}
fn pump(
    a: &mut Endpoint,
    ga: &mut Fake,
    b: &mut Endpoint,
    gb: &mut Fake,
    t: u64,
    duplicates: bool,
) {
    if !a.busy()
        && let Some(m) = ga.queue.pop_front()
    {
        a.offer(m).unwrap();
    }
    let bytes = a.frame().unwrap();
    b.receive(&bytes, t, |m| gb.apply(m)).unwrap();
    if duplicates {
        // A retransmit may have a fresh heartbeat but must not apply the message twice.
        b.receive(&a.frame().unwrap(), t, |m| gb.apply(m)).unwrap();
        b.receive(&bytes, t, |_| panic!("old frame was delivered"))
            .unwrap();
    }
}
#[test]
fn fight_crosses_two_endpoints_exactly_once_with_retries() {
    let (mut a, mut b) = (endpoint(Role::Wow), endpoint(Role::Bg3));
    let (mut wow, mut bg3) = (Fake::new(Role::Wow, "enc-1"), Fake::new(Role::Bg3, "enc-1"));
    wow.begin().unwrap();
    for i in 1..100 {
        pump(&mut a, &mut wow, &mut b, &mut bg3, i, true);
        pump(&mut b, &mut bg3, &mut a, &mut wow, i, true);
        if wow.phase == "done" && bg3.phase == "done" && !a.busy() && !b.busy() {
            break;
        }
    }
    assert_eq!((wow.phase, bg3.phase), ("done", "done"));
    assert_eq!(
        (wow.paths, wow.effects, wow.rejections, wow.credits),
        (1, 1, 1, 1)
    );
    assert!(!wow.frozen);
}
#[test]
fn typed_rejections_are_bounded_directional_and_only_answer_the_pending_action() {
    let r = |action: &str, detail: String| Message::Rejected {
        encounter: "enc-1".into(),
        turn: 1,
        action: action.into(),
        reason: Rejection::NoPath,
        detail,
    };
    assert!(r("move-0", String::new()).validate(Role::Bg3).is_ok());
    assert!(r("move-0", "é".repeat(128)).validate(Role::Bg3).is_ok());
    assert!(r("move-0", "é".repeat(129)).validate(Role::Bg3).is_err());
    assert!(r("move 0", String::new()).validate(Role::Bg3).is_err());
    assert!(r("move-0", String::new()).validate(Role::Wow).is_err());
    let zero = Message::Rejected {
        encounter: "enc-1".into(),
        turn: 0,
        action: "move-0".into(),
        reason: Rejection::Unavailable,
        detail: String::new(),
    };
    assert!(zero.validate(Role::Bg3).is_err());
    let json = serde_json::to_string(&r("move-0", "x".into())).unwrap();
    assert_eq!(
        json,
        r#"{"type":"rejected","encounter":"enc-1","turn":1,"action":"move-0","reason":"no_path","detail":"x"}"#
    );
    assert!(serde_json::from_str::<Message>(&json.replace("no_path", "NoPath")).is_err());
    // WoW accepts a rejection only for its outstanding action, and only once.
    let mut wow = Fake::new(Role::Wow, "enc-1");
    wow.begin().unwrap();
    wow.queue.clear();
    assert!(wow.apply(&r("move-0", String::new())).is_err());
    wow.apply(&Message::Ready {
        encounter: "enc-1".into(),
    })
    .unwrap();
    wow.apply(&Message::Turn {
        encounter: "enc-1".into(),
        turn: 1,
        actor: "hero".into(),
    })
    .unwrap();
    assert!(wow.apply(&r("move-1", String::new())).is_err());
    wow.apply(&r("move-0", String::new())).unwrap();
    assert!(wow.apply(&r("move-0", String::new())).is_err());
    assert_eq!(wow.rejections, 1);
}
#[test]
fn every_partial_write_is_rejected_without_consuming_sequence() {
    let (mut a, mut b) = (endpoint(Role::Wow), endpoint(Role::Bg3));
    a.offer(begin()).unwrap();
    let bytes = a.frame().unwrap();
    for cut in 0..bytes.len() {
        assert!(
            b.receive(&bytes[..cut], 1, |_| panic!("partial delivered"))
                .is_err()
        );
    }
    let mut calls = 0;
    assert!(
        b.receive(&bytes, 2, |_| {
            calls += 1;
            Ok(())
        })
        .unwrap()
    );
    assert_eq!(calls, 1);
}
#[test]
fn corrupt_length_checksum_and_version_fail_closed() {
    let mut a = endpoint(Role::Wow);
    a.offer(begin()).unwrap();
    let bytes = a.frame().unwrap();
    let mut v: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
    v["bytes"] = 0.into();
    assert!(wire::decode(&serde_json::to_vec(&v).unwrap()).is_err());
    v = serde_json::from_slice(&bytes).unwrap();
    v["checksum"] = 0.into();
    assert!(wire::decode(&serde_json::to_vec(&v).unwrap()).is_err());
    let mut frame = wire::decode(&bytes).unwrap();
    frame.version = 2;
    assert!(wire::decode(&wire::encode(&frame).unwrap()).is_err());
    assert!(wire::decode(&vec![b' '; MAX_FRAME + 1]).is_err());
}
#[test]
fn application_refusal_is_not_acknowledged() {
    let (mut a, mut b) = (endpoint(Role::Wow), endpoint(Role::Bg3));
    a.offer(begin()).unwrap();
    let raw = a.frame().unwrap();
    assert!(
        b.receive(&raw, 1, |_| Err("bad freeze receipt".into()))
            .is_err()
    );
    assert_eq!(wire::decode(&b.frame().unwrap()).unwrap().ack, 0);
    assert!(b.receive(&raw, 2, |_| Ok(())).unwrap());
}
#[test]
fn gaps_conflicting_retries_and_future_acks_fail_closed() {
    let (mut a, mut b) = (endpoint(Role::Wow), endpoint(Role::Bg3));
    a.offer(begin()).unwrap();
    let raw = a.frame().unwrap();
    let mut f = wire::decode(&raw).unwrap();
    f.seq = 2;
    assert!(
        b.receive(&wire::encode(&f).unwrap(), 1, |_| panic!("gap delivered"))
            .is_err()
    );
    b.receive(&raw, 2, |_| Ok(())).unwrap();
    f.seq = 1;
    f.heartbeat += 1;
    if let Some(Message::Begin { map, .. }) = &mut f.message {
        *map = 1;
    }
    assert!(
        b.receive(&wire::encode(&f).unwrap(), 3, |_| panic!(
            "conflict delivered"
        ))
        .is_err()
    );
    f.message = None;
    f.seq = 0;
    f.ack = 10;
    assert!(
        b.receive(&wire::encode(&f).unwrap(), 4, |_| Ok(()))
            .is_err()
    );
}
#[test]
fn stale_file_does_not_keep_dead_peer_alive_and_old_session_cannot_resume() {
    let (mut a, mut b) = (endpoint(Role::Wow), endpoint(Role::Bg3));
    let raw = a.frame().unwrap();
    b.receive(&raw, 1, |_| Ok(())).unwrap();
    for t in [200, 500, 999] {
        assert!(!b.receive(&raw, t, |_| Ok(())).unwrap());
    }
    assert!(b.tick(1002).is_err());
    assert!(b.receive(&a.frame().unwrap(), 1003, |_| Ok(())).is_err());
    let mut fresh =
        Endpoint::new("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", Role::Bg3, 1003, 1000).unwrap();
    assert!(
        fresh
            .receive(&raw, 1004, |_| panic!("old session delivered"))
            .is_err()
    );
}
#[test]
fn lost_ack_retries_do_not_apply_twice() {
    let (mut a, mut b) = (endpoint(Role::Wow), endpoint(Role::Bg3));
    a.offer(begin()).unwrap();
    let mut n = 0;
    b.receive(&a.frame().unwrap(), 1, |_| {
        n += 1;
        Ok(())
    })
    .unwrap();
    let _lost_ack = b.frame().unwrap();
    b.receive(&a.frame().unwrap(), 2, |_| {
        n += 1;
        Ok(())
    })
    .unwrap();
    assert_eq!(n, 1);
    a.receive(&b.frame().unwrap(), 3, |_| Ok(())).unwrap();
    assert!(!a.busy());
}
#[test]
fn disconnect_reconciliation_drops_uncommitted_encounter() {
    let mut wow = Fake::new(Role::Wow, "enc-1");
    wow.begin().unwrap();
    assert!(wow.frozen);
    wow.reconcile_disconnect();
    assert!(!wow.frozen);
    assert!(wow.queue.is_empty());
    assert_eq!(wow.credits, 0);
    let stale = Message::Finished {
        encounter: "enc-1".into(),
        outcome: wire::Outcome::Victory,
    };
    assert!(wow.apply(&stale).is_err());
    assert_eq!(wow.credits, 0);
    // A fresh session/encounter can run from the start; the old endpoint cannot be revived.
    let mut next = Fake::new(Role::Wow, "enc-2");
    assert!(next.begin().is_ok());
}
#[test]
fn wrong_encounter_turn_actor_and_direction_are_rejected() {
    let mut wow = Fake::new(Role::Wow, "enc-1");
    wow.begin().unwrap();
    wow.queue.clear();
    assert!(
        wow.apply(&Message::Ready {
            encounter: "other".into()
        })
        .is_err()
    );
    wow.apply(&Message::Ready {
        encounter: "enc-1".into(),
    })
    .unwrap();
    assert!(
        wow.apply(&Message::Turn {
            encounter: "enc-1".into(),
            turn: 2,
            actor: "hero".into()
        })
        .is_err()
    );
    assert!(
        wow.apply(&Message::Turn {
            encounter: "enc-1".into(),
            turn: 1,
            actor: "stranger".into()
        })
        .is_err()
    );
    assert!(begin().validate(Role::Bg3).is_err());
}
#[test]
fn invalid_paths_guids_and_nonfinite_coordinates_are_rejected() {
    let mut m = begin();
    if let Message::Begin { actors, .. } = &mut m {
        actors[0].position = Point(f64::NAN, 0., 0.);
    }
    assert!(m.validate(Role::Wow).is_err());
    m = begin();
    if let Message::Begin { actors, .. } = &mut m {
        actors[0].wow_guid = "123".into();
    }
    assert!(m.validate(Role::Wow).is_err());
    let p = Message::Path {
        encounter: "enc-1".into(),
        turn: 1,
        action: "a".into(),
        actor: "hero".into(),
        points: vec![Point(0., 0., 0.); 257],
    };
    assert!(p.validate(Role::Bg3).is_err());
}
#[test]
fn coordinate_axes_units_and_roundtrip_include_slopes_and_rotations() {
    let b = Basis {
        wow_origin: Point(0., 0., 0.),
        bridge_origin: Point(0., 0., 0.),
        yaw: 0.,
    };
    assert_eq!(
        b.to_bridge(Point(1., 0., 0.)),
        Point(0., 0., -METRES_PER_YARD)
    );
    assert_eq!(
        b.to_bridge(Point(0., 1., 0.)),
        Point(-METRES_PER_YARD, 0., 0.)
    );
    assert_eq!(
        b.to_bridge(Point(0., 0., 1.)),
        Point(0., METRES_PER_YARD, 0.)
    );
    for yaw in [0., 0.3, 1.57, std::f64::consts::PI, -2.8] {
        let b = Basis {
            wow_origin: Point(-8949., -132., 83.),
            bridge_origin: Point(40., -2., 120.),
            yaw,
        };
        for p in [Point(-8940., -140., 88.), Point(-9000., -99., 70.)] {
            let q = b.to_wow(b.to_bridge(p));
            assert!(
                (q.0 - p.0).abs() < 1e-9 && (q.1 - p.1).abs() < 1e-9 && (q.2 - p.2).abs() < 1e-9
            );
        }
    }
}
#[test]
fn json_counters_and_unknown_fields_are_bounded() {
    let f = Frame {
        version: VERSION,
        session: SESSION.into(),
        sender: Role::Wow,
        heartbeat: 1,
        seq: 0,
        ack: wire::MAX_SEQUENCE + 1,
        message: None,
    };
    assert!(wire::decode(&wire::encode(&f).unwrap()).is_err());
    let mut e = endpoint(Role::Wow);
    let raw = e.frame().unwrap();
    let mut v: serde_json::Value = serde_json::from_slice(&raw).unwrap();
    v["extra"] = 1.into();
    assert!(wire::decode(&serde_json::to_vec(&v).unwrap()).is_err());
}
#[test]
fn coordinates_round_trip_bit_exactly() {
    // serde_json's default parser can be one ULP off; float_roundtrip must stay enabled
    // so Rust and the Lua codec agree on every coordinate.
    let points: Vec<Point> = (0..256)
        .map(|i| {
            let x = f64::from(i);
            Point(x / 3.0, x.sin(), -x * METRES_PER_YARD)
        })
        .collect();
    let f = Frame {
        version: VERSION,
        session: SESSION.into(),
        sender: Role::Bg3,
        heartbeat: 1,
        seq: 1,
        ack: 0,
        message: Some(Message::Path {
            encounter: "enc-1".into(),
            turn: 1,
            action: "move-1".into(),
            actor: "hero".into(),
            points: points.clone(),
        }),
    };
    let Some(Message::Path { points: back, .. }) =
        wire::decode(&wire::encode(&f).unwrap()).unwrap().message
    else {
        panic!("path did not decode");
    };
    for (a, b) in points.iter().zip(&back) {
        assert_eq!(
            [a.0.to_bits(), a.1.to_bits(), a.2.to_bits()],
            [b.0.to_bits(), b.1.to_bits(), b.2.to_bits()]
        );
    }
}
#[test]
fn combatant_actions_are_bg3_only_bounded_and_may_stand_still() {
    let acted = |points: Vec<Point>, spell: &str| Message::Acted {
        encounter: "enc-1".into(),
        turn: 2,
        action: "wolf-2-1".into(),
        actor: "wolf".into(),
        spell: spell.into(),
        target: "hero".into(),
        points,
    };
    assert!(
        acted(vec![Point(1., 0., 2.)], "Target_Bite")
            .validate(Role::Bg3)
            .is_ok()
    );
    assert!(acted(Vec::new(), "none").validate(Role::Bg3).is_ok());
    assert!(acted(Vec::new(), "none").validate(Role::Wow).is_err());
    assert!(acted(Vec::new(), "").validate(Role::Bg3).is_err());
    assert!(
        acted(vec![Point(0., 0., 0.); 257], "none")
            .validate(Role::Bg3)
            .is_err()
    );
    assert!(
        acted(vec![Point(0., f64::INFINITY, 0.)], "none")
            .validate(Role::Bg3)
            .is_err()
    );
    let mut wow = endpoint(Role::Wow);
    let mut bg3 = endpoint(Role::Bg3);
    bg3.offer(acted(vec![Point(1., 0., 2.)], "Target_Bite"))
        .unwrap();
    let bytes = bg3.frame().unwrap();
    let mut got = None;
    assert!(
        wow.receive(&bytes, 1, |m| {
            got = Some(m.clone());
            Ok(())
        })
        .unwrap()
    );
    assert_eq!(got, Some(acted(vec![Point(1., 0., 2.)], "Target_Bite")));
}
#[test]
fn joins_go_to_bg3_and_rosters_come_back_with_valid_actor_lists() {
    let a = |id: &str, hp: u32, max_hp: u32| wire::Actor {
        id: id.into(),
        wow_guid: "f130000000012b02".into(),
        position: Point(1., 0., 2.),
        hp,
        max_hp,
    };
    let join = |actors| Message::Join {
        encounter: "enc-1".into(),
        actors,
    };
    let roster = |actors| Message::Roster {
        encounter: "enc-1".into(),
        joins: 3,
        actors,
    };
    assert!(join(vec![a("e2", 42, 42)]).validate(Role::Wow).is_ok());
    assert!(join(vec![a("e2", 42, 42)]).validate(Role::Bg3).is_err());
    assert!(join(Vec::new()).validate(Role::Wow).is_err());
    assert!(
        join(vec![a("e2", 42, 42), a("e2", 42, 42)])
            .validate(Role::Wow)
            .is_err()
    );
    assert!(join(vec![a("e 2", 42, 42)]).validate(Role::Wow).is_err());
    assert!(roster(vec![a("e2", 0, 12)]).validate(Role::Bg3).is_ok());
    assert!(roster(vec![a("e2", 0, 12)]).validate(Role::Wow).is_err());
    assert!(roster(vec![a("e2", 13, 12)]).validate(Role::Bg3).is_err());
    assert!(roster(vec![a("e2", 0, 0)]).validate(Role::Bg3).is_err());
    let mut wow = endpoint(Role::Wow);
    let mut bg3 = endpoint(Role::Bg3);
    wow.offer(join(vec![a("e2", 42, 42)])).unwrap();
    let mut got = None;
    bg3.receive(&wow.frame().unwrap(), 1, |m| {
        got = Some(m.clone());
        Ok(())
    })
    .unwrap();
    assert_eq!(got, Some(join(vec![a("e2", 42, 42)])));
}
