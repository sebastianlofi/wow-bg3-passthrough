use wow_bg3_link::{
    bridge::{Combatant, CreatureStats, Fight, below_wow_fraction, spec, standin_kind, units},
    coords::{Basis, Point},
    handoff::Handoff,
    wire::{Actor, Message, Outcome, Rejection, Role},
};
fn wolf(n: u32, pos: Point) -> Combatant {
    let mut c = Combatant::new("enemy", &format!("f130000000012b0{n}"), pos, 42, 42);
    c.entry = 299;
    c.level = 1;
    c.name = "Young Wolf".into();
    c
}
fn fight_with(enemies: Vec<Combatant>) -> Fight {
    let basis = Basis {
        wow_origin: Point(-8949.95, -132.49, 83.53),
        bridge_origin: Point(0., 0., 0.),
        yaw: 0.,
    };
    let hero = Combatant::new(
        "hero",
        "0000000000000001",
        Point(-8949.95, -132.49, 83.53),
        100,
        100,
    );
    Fight::new("pull-1", basis, hero, enemies)
}
fn fight() -> Fight {
    fight_with(vec![wolf(1, Point(-8942.95, -132.49, 83.53))])
}
fn e() -> String {
    "pull-1".into()
}
fn actor(id: &str, guid: &str, hp: u32, max_hp: u32) -> Actor {
    Actor {
        id: id.into(),
        wow_guid: guid.into(),
        position: Point(0., 0., -3.),
        hp,
        max_hp,
    }
}
fn ready_hero_turn(f: &mut Fight, turn: u32) {
    f.apply(&Message::Turn {
        encounter: e(),
        turn,
        actor: "hero".into(),
    })
    .unwrap();
}
#[test]
fn begin_is_valid_and_puts_the_hero_on_the_bridge_origin() {
    let f = fight_with(vec![
        wolf(1, Point(-8942.95, -132.49, 83.53)),
        wolf(2, Point(-8942.95, -129.49, 83.53)),
    ]);
    let m = f.begin();
    m.validate(Role::Wow).unwrap();
    let Message::Begin { actors, .. } = m else {
        panic!("not a begin");
    };
    assert_eq!(actors[0].position, Point(0., 0., 0.));
    // 7 yards along WoW +x is bridge -z (coords.rs), 6.4 m.
    assert!((actors[1].position.2 + 7. * 0.9144).abs() < 1e-9);
    let ids: Vec<&str> = actors.iter().map(|a| a.id.as_str()).collect();
    assert_eq!(ids, ["hero", "e1", "e2"]);
    assert_eq!(
        f.handoff.front(),
        Some(&Handoff::Begin {
            encounter: e(),
            mobs: vec!["f130000000012b01".into(), "f130000000012b02".into()]
        })
    );
}
#[test]
fn hero_turn_actions_results_and_enemy_turns_update_the_wow_view() {
    let mut f = fight();
    assert!(
        f.request("move", Some(Point(0., 0., 0.)), "", None)
            .is_err(),
        "acted before the fight"
    );
    f.apply(&Message::Ready { encounter: e() }).unwrap();
    ready_hero_turn(&mut f, 1);
    assert!(f.your_turn());
    let target = Point(-8946.95, -132.49, 83.53);
    let m = f.request("move", Some(target), "", None).unwrap();
    m.validate(Role::Wow).unwrap();
    assert!(!f.your_turn(), "second action while one is outstanding");
    let Message::Move {
        action, target: bt, ..
    } = m
    else {
        panic!()
    };
    f.apply(&Message::Path {
        encounter: e(),
        turn: 1,
        action: action.clone(),
        actor: "hero".into(),
        points: vec![Point(0., 0., 0.), bt],
    })
    .unwrap();
    let p = f.hero.pos;
    assert!(
        (p.0 - target.0).abs() < 1e-9 && (p.1 - target.1).abs() < 1e-9,
        "hero ends where BG3 walked"
    );
    let Message::Cast { action, target, .. } = f.request("cast", None, "FireBolt", None).unwrap()
    else {
        panic!()
    };
    assert_eq!(target, "e1");
    f.apply(&Message::Effect {
        encounter: e(),
        turn: 1,
        action,
        target: "e1".into(),
        hp: 4,
        max_hp: 11,
        animation: "FireBolt".into(),
    })
    .unwrap();
    assert_eq!((f.enemies[0].hp, f.enemies[0].max_hp), (4, 11));
    let Message::Cast { action, .. } = f.request("cast", None, "FireBolt", None).unwrap() else {
        panic!()
    };
    f.apply(&Message::Rejected {
        encounter: e(),
        turn: 1,
        action,
        reason: Rejection::Unavailable,
        detail: String::new(),
    })
    .unwrap();
    assert!(f.your_turn(), "a refusal keeps the turn");
    assert!(matches!(
        f.request("end_turn", None, "", None).unwrap(),
        Message::EndTurn { turn: 1, .. }
    ));
    assert!(!f.your_turn());
    f.apply(&Message::Turn {
        encounter: e(),
        turn: 2,
        actor: "e1".into(),
    })
    .unwrap();
    assert!(
        f.request("cast", None, "FireBolt", None).is_err(),
        "acted on the wolf's turn"
    );
    f.apply(&Message::Acted {
        encounter: e(),
        turn: 2,
        action: "e1-2".into(),
        actor: "e1".into(),
        spell: "Target_Bite_Wolf".into(),
        target: "hero".into(),
        points: vec![Point(0., 0., -3.), Point(0., 0., -1.)],
    })
    .unwrap();
    f.apply(&Message::Effect {
        encounter: e(),
        turn: 2,
        action: "e1-2".into(),
        target: "hero".into(),
        hp: 9,
        max_hp: 14,
        animation: "Target_Bite_Wolf".into(),
    })
    .unwrap();
    assert_eq!(f.hero.hp, 9);
    assert!(
        (f.enemies[0].pos.0 - (-8949.95 + 1. / 0.9144)).abs() < 1e-6,
        "wolf moved next to the hero"
    );
    let text = f.text();
    assert!(
        text.contains("strike 4 e1 hero hit 91 Target_Bite_Wolf"),
        "the bite plays as a WoW hit: {text}"
    );
    assert!(text.contains("enemy e1 f130000000012b01 299 in"));
    let snap = f.snapshot();
    assert_eq!(snap["anims"].as_array().unwrap().len(), 4);
    assert_eq!(snap["actors"]["hero"]["hp"], 9);
    let reply = f
        .apply(&Message::Finished {
            encounter: e(),
            outcome: Outcome::Victory,
        })
        .unwrap();
    assert!(matches!(reply, Some(Message::Cleanup { .. })));
    f.apply(&Message::Cleaned { encounter: e() }).unwrap();
    assert_eq!(f.state, "done");
    assert_eq!(
        f.handoff.back(),
        Some(&Handoff::End {
            encounter: e(),
            outcome: "victory".into()
        }),
        "one end, with the outcome"
    );
    assert!(
        f.apply(&Message::Ready {
            encounter: "other".into()
        })
        .is_err()
    );
}
#[test]
fn an_attack_without_an_effect_plays_as_a_miss() {
    let mut f = fight();
    f.apply(&Message::Ready { encounter: e() }).unwrap();
    f.apply(&Message::Turn {
        encounter: e(),
        turn: 1,
        actor: "e1".into(),
    })
    .unwrap();
    f.apply(&Message::Acted {
        encounter: e(),
        turn: 1,
        action: "e1-1".into(),
        actor: "e1".into(),
        spell: "Target_Bite_Wolf".into(),
        target: "hero".into(),
        points: Vec::new(),
    })
    .unwrap();
    ready_hero_turn(&mut f, 2);
    assert!(
        f.text()
            .contains("strike 1 e1 hero miss 0 Target_Bite_Wolf")
    );
}
#[test]
fn rosters_show_template_hp_and_joins_add_or_refuse_mobs() {
    let mut f = fight();
    // A Young Wolf's stand-in keeps BG3's wolf template's 11 HP.
    f.apply(&Message::Roster {
        encounter: e(),
        joins: 0,
        actors: vec![
            actor("hero", "0000000000000001", 14, 14),
            actor("e1", "f130000000012b01", 11, 11),
        ],
    })
    .unwrap();
    assert_eq!((f.enemies[0].hp, f.enemies[0].max_hp), (11, 11));
    assert!(
        f.log
            .iter()
            .any(|l| l == "Young Wolf (e1): WoW 42/42 HP, BG3 stand-in 11/11 HP")
    );
    assert!(
        f.join(vec![wolf(2, Point(-8950., -128., 83.5))]).is_none(),
        "not before Ready"
    );
    f.apply(&Message::Ready { encounter: e() }).unwrap();
    let m = f
        .join(vec![
            wolf(1, Point(-8950., -128., 83.5)),
            wolf(2, Point(-8950., -128., 83.5)),
            wolf(3, Point(-8950., -125., 83.5)),
        ])
        .unwrap();
    m.validate(Role::Wow).unwrap();
    let Message::Join { actors, .. } = m else {
        panic!()
    };
    assert_eq!(actors.len(), 2, "e1 is already in the fight");
    assert_eq!((actors[0].id.as_str(), actors[1].id.as_str()), ("e2", "e3"));
    assert_eq!(f.joins_sent, 1);
    // BG3 set up e2 only: e3 could not join.
    f.apply(&Message::Roster {
        encounter: e(),
        joins: 1,
        actors: vec![
            actor("hero", "0000000000000001", 14, 14),
            actor("e1", "f130000000012b01", 11, 11),
            actor("e2", "f130000000012b02", 11, 11),
        ],
    })
    .unwrap();
    assert_eq!(f.enemy("e2").unwrap().status, "in");
    assert_eq!(f.enemy("e3").unwrap().status, "refused");
    let handoff: Vec<String> = f.handoff.iter().map(Handoff::describe).collect();
    assert_eq!(
        handoff,
        [
            ".wowbg3 begin pull-1 f130000000012b01",
            ".wowbg3 hero pull-1 14 14",
            ".wowbg3 join pull-1 f130000000012b02,f130000000012b03",
            "(release pull-1 f130000000012b03)"
        ]
    );
    // Its join failed but BG3 had its stand-in in the fight and killed it (the mod lists it late):
    // it is in, and the kill goes to WoW (the server still holds it: the release is only a note).
    f.handoff.clear();
    f.apply(&Message::Roster {
        encounter: e(),
        joins: 1,
        actors: vec![
            actor("hero", "0000000000000001", 14, 14),
            actor("e1", "f130000000012b01", 11, 11),
            actor("e2", "f130000000012b02", 11, 11),
            actor("e3", "f130000000012b03", 0, 11),
        ],
    })
    .unwrap();
    assert_eq!(f.enemy("e3").unwrap().status, "dead");
    assert!(f.guids().contains(&"f130000000012b03".to_string()));
    let handoff: Vec<String> = f.handoff.iter().filter_map(Handoff::command).collect();
    assert_eq!(handoff.len(), 2, "{handoff:?}");
    assert!(handoff[0].starts_with(".wowbg3 pos pull-1 f130000000012b03 "));
    assert_eq!(handoff[1], ".wowbg3 hp pull-1 f130000000012b03 0 11");
    assert!(
        f.log
            .iter()
            .any(|l| l == "Young Wolf (e3) is in the BG3 fight after all")
    );
}
#[test]
fn bg3_hit_points_go_to_the_server_as_levels_and_a_kill_once() {
    let mut f = fight();
    f.handoff.clear();
    let roster = |hp| Message::Roster {
        encounter: e(),
        joins: 0,
        actors: vec![actor("e1", "f130000000012b01", hp, 11)],
    };
    f.apply(&roster(11)).unwrap();
    assert!(f.handoff.is_empty(), "the first BG3 numbers are no damage");
    f.apply(&roster(7)).unwrap();
    f.apply(&roster(7)).unwrap();
    assert_eq!(
        f.enemies[0].wow_hp, 27,
        "7 of 11 BG3 HP is 26.7 of the wolf's 42 WoW HP"
    );
    f.apply(&roster(0)).unwrap();
    f.apply(&roster(0)).unwrap();
    let handoff: Vec<String> = f.handoff.iter().filter_map(Handoff::command).collect();
    assert_eq!(handoff.len(), 3, "{handoff:?}");
    assert_eq!(handoff[0], ".wowbg3 hp pull-1 f130000000012b01 7 11");
    assert!(
        handoff[1].starts_with(".wowbg3 pos pull-1 f130000000012b01 "),
        "the corpse's place right before the kill: {}",
        handoff[1]
    );
    assert_eq!(
        handoff[2], ".wowbg3 hp pull-1 f130000000012b01 0 11",
        "levels, not deltas; the kill once"
    );
    assert_eq!(f.enemies[0].status, "dead");
    assert!(f.log.iter().any(|l| l == "Young Wolf (e1) dies"));
}
#[test]
fn a_member_missing_from_the_roster_or_first_seen_at_0_hp_is_a_kill() {
    // The roster drops a member BG3 reported before: hp 0, and the kill before Tav's own 0.
    let mut f = fight();
    let e1 = |hp| actor("e1", "f130000000012b01", hp, 11);
    f.apply(&Message::Roster {
        encounter: e(),
        joins: 0,
        actors: vec![actor("hero", "0000000000000001", 14, 14), e1(11)],
    })
    .unwrap();
    f.handoff.clear();
    let without = Message::Roster {
        encounter: e(),
        joins: 0,
        actors: vec![actor("hero", "0000000000000001", 0, 14)],
    };
    f.apply(&without).unwrap();
    f.apply(&without).unwrap();
    assert_eq!(f.enemies[0].status, "dead");
    let h: Vec<String> = f.handoff.iter().filter_map(Handoff::command).collect();
    assert_eq!(h.len(), 3, "the kill once: {h:?}");
    assert!(h[0].starts_with(".wowbg3 pos pull-1 f130000000012b01 "));
    assert_eq!(h[1], ".wowbg3 hp pull-1 f130000000012b01 0 11");
    assert_eq!(h[2], ".wowbg3 hero pull-1 0 14", "the mob's kill goes first");
    // Never reported: not BG3's to lose (the first roster may name only part of a fight).
    let mut f = fight();
    f.handoff.clear();
    f.apply(&Message::Roster {
        encounter: e(),
        joins: 0,
        actors: vec![actor("hero", "0000000000000001", 14, 14)],
    })
    .unwrap();
    assert_eq!(f.enemies[0].status, "in");
    // BG3's first numbers say 0: the kill, not a template's HP.
    let mut f = fight();
    f.handoff.clear();
    f.apply(&Message::Roster {
        encounter: e(),
        joins: 0,
        actors: vec![e1(0)],
    })
    .unwrap();
    assert_eq!(f.enemies[0].status, "dead");
    let h: Vec<String> = f.handoff.iter().filter_map(Handoff::command).collect();
    assert_eq!(h.last().unwrap(), ".wowbg3 hp pull-1 f130000000012b01 0 11");
}
#[test]
fn a_stand_in_at_the_wow_mobs_own_fraction_is_no_damage_at_the_start() {
    // A wolf WoW has at 21/42 HP; its stand-in keeps the template's 11 HP at that
    // fraction (5.5, which BG3 shows as 5 or 6). Only HP under that (Tav's opening hit) is damage.
    let hurt = || {
        let mut w = wolf(1, Point(-8942.95, -132.49, 83.53));
        w.wow_hp = 21;
        w.hp = 21;
        fight_with(vec![w])
    };
    let roster = |hp| Message::Roster {
        encounter: e(),
        joins: 0,
        actors: vec![actor("e1", "f130000000012b01", hp, 11)],
    };
    for start in [5, 6] {
        let mut f = hurt();
        f.handoff.clear();
        f.apply(&roster(start)).unwrap();
        assert!(f.handoff.is_empty(), "{start}/11 is the WoW fraction");
        assert_eq!(f.enemies[0].wow_hp, 21, "WoW keeps its own HP");
        f.apply(&roster(3)).unwrap();
        let h: Vec<String> = f.handoff.iter().filter_map(Handoff::command).collect();
        assert_eq!(h, [".wowbg3 hp pull-1 f130000000012b01 3 11"]);
        assert_eq!(f.enemies[0].wow_hp, 11, "3 of 11 is 11.5 of 42");
    }
    let mut f = hurt();
    f.handoff.clear();
    f.apply(&roster(4)).unwrap();
    let h: Vec<String> = f.handoff.iter().filter_map(Handoff::command).collect();
    assert_eq!(
        h,
        [".wowbg3 hp pull-1 f130000000012b01 4 11"],
        "an opening hit"
    );
    // An unhurt mob: any HP under the max is damage, as before.
    assert!(below_wow_fraction(10, 11, 42, 42));
    assert!(!below_wow_fraction(11, 11, 42, 42));
    assert!(
        !below_wow_fraction(10, 11, 41, 42),
        "10.7 shows as 11 or 10"
    );
    assert!(below_wow_fraction(9, 11, 41, 42));
}
#[test]
fn tav_hit_points_go_to_the_server_and_server_ends_end_the_fight() {
    let mut f = fight();
    f.handoff.clear();
    let hero = |hp| Message::Roster {
        encounter: e(),
        joins: 0,
        actors: vec![actor("hero", "0000000000000001", hp, 14)],
    };
    f.apply(&hero(14)).unwrap();
    f.apply(&hero(14)).unwrap();
    f.apply(&hero(5)).unwrap();
    let handoff: Vec<String> = f.handoff.iter().filter_map(Handoff::command).collect();
    assert_eq!(
        handoff,
        [".wowbg3 hero pull-1 14 14", ".wowbg3 hero pull-1 5 14"],
        "Tav's first numbers, then each change"
    );
    f.apply(&Message::Ready { encounter: e() }).unwrap();
    assert!(
        f.server_event("WOWBG3 release pull-1 F130000000012B01 immune")
            .is_none()
    );
    assert!(
        f.log
            .iter()
            .any(|l| l == "The server let Young Wolf (e1) go (immune)")
    );
    assert!(
        f.server_event("WOWBG3 end pull-9 timeout ok 0 1").is_none(),
        "another encounter"
    );
    assert!(
        f.server_event("WOWBG3 end pull-1 victory ok 1 0").is_none(),
        "our own end's answer"
    );
    assert_eq!(f.state, "active");
    assert!(matches!(
        f.server_event("WOWBG3 end pull-1 timeout ok 0 1"),
        Some(Message::Cleanup { .. })
    ));
    assert_eq!(f.state, "cleaning");
    f.apply(&Message::Cleaned { encounter: e() }).unwrap();
    assert!(
        !f.handoff.iter().any(|h| matches!(h, Handoff::End { .. })),
        "the server already ended it"
    );
}
#[test]
fn a_refused_begin_ends_the_fight_on_bg3s_side() {
    // Refused before BG3 is set up: BG3 takes the Cleanup only at Ready.
    let mut f = fight();
    f.handoff.clear();
    assert!(
        f.server_event("WOWBG3 begin pull-9 refused dead -").is_none(),
        "another encounter"
    );
    assert!(!f.refused);
    let seq = f.seq;
    assert!(f.server_event("WOWBG3 begin pull-1 refused dead -").is_none());
    assert!(f.seq > seq, "the player's line reaches fight.txt");
    assert_eq!(f.state, "starting");
    assert!(
        f.log
            .iter()
            .any(|l| l == "The server refused this fight: your WoW character is dead")
    );
    assert!(matches!(
        f.apply(&Message::Ready { encounter: e() }).unwrap(),
        Some(Message::Cleanup { .. })
    ));
    assert_eq!(f.state, "cleaning");
    f.apply(&Message::Cleaned { encounter: e() }).unwrap();
    assert_eq!(f.state, "done");
    assert!(
        !f.handoff.iter().any(|h| matches!(h, Handoff::End { .. })),
        "the server holds no encounter to end"
    );

    // Refused once it runs: Cleanup at once, whatever the reason.
    let mut f = fight();
    f.apply(&Message::Ready { encounter: e() }).unwrap();
    assert_eq!(f.state, "active");
    assert!(matches!(
        f.server_event("WOWBG3 begin pull-1 refused busy pull-0"),
        Some(Message::Cleanup { .. })
    ));
    assert_eq!(f.state, "cleaning");

    // An accepted begin and a refused join change nothing.
    let mut f = fight();
    f.apply(&Message::Ready { encounter: e() }).unwrap();
    assert!(
        f.server_event("WOWBG3 begin pull-1 ok 1 76 76").is_none()
            && f.server_event("WOWBG3 join pull-1 refused dead -")
                .is_none()
    );
    assert_eq!(f.state, "active");
    assert!(!f.refused);
}
#[test]
fn casts_aim_at_the_named_or_nearest_living_enemy() {
    let mut f = fight_with(vec![
        wolf(1, Point(-8930., -132.49, 83.53)),
        wolf(2, Point(-8945., -132.49, 83.53)),
    ]);
    f.apply(&Message::Ready { encounter: e() }).unwrap();
    ready_hero_turn(&mut f, 1);
    let Message::Cast { target, .. } = f.request("cast", None, "Attack", None).unwrap() else {
        panic!()
    };
    assert_eq!(target, "e2", "nearest");
    f.outstanding = None;
    let Message::Cast { target, .. } = f.request("cast", None, "Attack", Some("e1")).unwrap()
    else {
        panic!()
    };
    assert_eq!(target, "e1", "named");
}
#[test]
fn leaving_sends_cleanup_and_unknown_input_is_refused() {
    let mut f = fight();
    f.apply(&Message::Ready { encounter: e() }).unwrap();
    ready_hero_turn(&mut f, 1);
    assert!(f.request("dance", None, "", None).is_err());
    assert!(f.request("cast", None, "bad spell", None).is_err());
    assert!(matches!(
        f.request("leave", None, "", None).unwrap(),
        Message::Cleanup { .. }
    ));
    assert_eq!(f.state, "cleaning");
}
#[test]
fn pulls_read_packs_and_older_single_enemy_files() {
    let unit = |g: &str| {
        serde_json::json!({"guid": g, "entry": 299, "name": "Young\nWolf", "level": 1,
            "pos": [1.0, 2.0, 3.0], "hp": 40, "max_hp": 42})
    };
    let pack = serde_json::json!({"enemies": [unit("F130000000012B01"), unit("bad"), unit("f130000000012b02")]});
    let got = units(&pack);
    assert_eq!(got.len(), 2);
    assert_eq!(got[0].guid, "f130000000012b01");
    assert_eq!((got[0].hp, got[0].wow_max_hp, got[0].level), (40, 42, 1));
    assert_eq!(got[0].name, "YoungWolf");
    let old = serde_json::json!({"enemy": unit("f130000000012b01")});
    assert_eq!(units(&old).len(), 1);
}
#[test]
fn stand_ins_follow_wow_type_family_and_name_with_wow_numbers() {
    assert_eq!(standin_kind(1, 1, "Young Wolf"), "wolf");
    assert_eq!(standin_kind(1, 3, "Mine Spider"), "spider");
    assert_eq!(standin_kind(1, 5, "Stonetusk Boar"), "boar");
    assert_eq!(standin_kind(7, 0, "Kobold Vermin"), "goblin");
    assert_eq!(standin_kind(7, 0, "Defias Thug"), "human");
    assert_eq!(standin_kind(8, 0, "Rabbit"), "rat");
    assert_eq!(standin_kind(6, 0, "Rattlecage Skeleton"), "zombie");
    let stats = CreatureStats::from_value(serde_json::json!({
        "entries": {"299": {"name": "Young Wolf", "level_min": 1, "level_max": 1, "type": 1,
            "family": 1, "rank": 0, "class": 1, "health_mult": 1.0, "damage_mult": 1.0,
            "variance": 0.02, "attack_ms": 2000, "faction": 32}},
        "levels": {"1": {"1": [1.57945, 42], "2": [2.31493, 55]}}}));
    let mob = stats.mob(299, 5).unwrap();
    assert_eq!(
        (mob.health, mob.ty, mob.family),
        (42, 1, 1),
        "level clamped to the template's"
    );
    assert!((mob.damage - 1.57945).abs() < 1e-9);
    let s = spec(&wolf(1, Point(0., 0., 0.)), Some(&stats), "wolf-template");
    assert_eq!(s["Kind"], "wolf");
    assert_eq!(s["Template"], "wolf-template");
    assert_eq!(s["WowDamage"], 1.58);
    assert_eq!(s["WowMaxHp"], 42);
    // The 1.12 client sees mob health in percent: max HP from the template, HP from the fraction.
    let mut seen = Combatant::new("enemy", "f130000000012b05", Point(0., 0., 0.), 50, 100);
    seen.entry = 299;
    wow_bg3_link::bridge::with_stats(&mut seen, Some(&stats));
    assert_eq!((seen.wow_hp, seen.wow_max_hp, seen.hp), (21, 42, 21));
    assert_eq!(seen.name, "Young Wolf");
    let mut thug = wolf(9, Point(0., 0., 0.));
    thug.entry = 38;
    thug.name = "Defias Thug".into();
    let s = spec(&thug, Some(&stats), "wolf-template");
    assert_eq!(
        (s["Kind"].as_str(), s["WowDamage"].is_null()),
        (Some("human"), true)
    );
}

#[test]
fn world_mode_fights_use_the_session_mapping() {
    use wow_bg3_link::bridge::world_centre;
    let world = serde_json::json!({"Active": true, "WowOrigin": [-8922.0, 43.0, 131.0],
        "Centre": [-136.0, 10.0, -444.0]});
    let c = world_centre(&world, Point(-8920.0, 53.0, 132.0)).unwrap();
    let yd = 0.9144;
    assert!((c.0 - (-136.0 + 10.0 * yd)).abs() < 1e-9);
    assert!((c.1 - (10.0 + 1.0 * yd)).abs() < 1e-9);
    assert!((c.2 - (-444.0 - 2.0 * yd)).abs() < 1e-9);
    let off = serde_json::json!({"Active": false, "WowOrigin": [0.0, 0.0, 0.0], "Centre": [0.0, 0.0, 0.0]});
    assert!(world_centre(&off, Point(0.0, 0.0, 0.0)).is_none());
}

#[test]
fn big_pulls_go_out_as_begin_and_join_lines_that_fit_wow_chat() {
    let mobs: Vec<String> = (0..30).map(|i| format!("f1300000000{i:05x}")).collect();
    let lines = Handoff::Begin {
        encounter: "pull-1".into(),
        mobs: mobs.clone(),
    }
    .commands();
    assert!(lines[0].starts_with(".wowbg3 begin pull-1 "));
    assert!(lines[1..].iter().all(|l| l.starts_with(".wowbg3 join pull-1 ") && !l.contains(',')), "{lines:?}");
    assert!(lines.iter().all(|l| l.len() < 255), "{lines:?}");
    let all: Vec<&str> = lines
        .iter()
        .flat_map(|l| l.rsplit(' ').next().unwrap().split(','))
        .collect();
    assert_eq!(all, mobs.iter().map(String::as_str).collect::<Vec<_>>());
    // One mob a line: a mob the server has already refuses only itself.
    let small = Handoff::Join {
        encounter: "pull-1".into(),
        mobs: mobs[..2].to_vec(),
    };
    assert_eq!(
        small.commands(),
        vec![format!(".wowbg3 join pull-1 {}", mobs[0]), format!(".wowbg3 join pull-1 {}", mobs[1])]
    );
}
