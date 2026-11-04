use serde_json::json;
use wow_bg3_link::{
    Error,
    bridge::{Combatant, CreatureStats, Fight},
    coords::{Basis, Point},
    handoff::{Handoff, ServerHandoff},
    mirror::{KILL_RETRY_MS, Mapping, Mirror, SETTLE_MS, bg3_forward, parse_mobs, parse_status},
};

const MOBS: &str = "seq 7\n\
hero 0000000000000001 -8920 53 132 0.5 52 52 1\n\
mob f130000000012b01 299 1 1 -8915 53 132 3.14159 100 100 0 Young Wolf\n\
mob f130000000012b02 299 2 1 -8920 63 132 0 50 100 3 Young Wolf\n\
mob f130000000012b03 6 1 0 -8920 43 132.5 1.0 100 100 0 Kobold Vermin\n\
mob f130000000012b04 299 1 1 -8850 53 132 0 100 100 0 Young Wolf\n\
mob f130000000012b05 299 1 1 -8915 53 132 0 0 100 0 Dead Wolf\n\
mob xyz 299 1 1 -8915 53 132 0 100 100 0 Bad Guid\n";

fn stats() -> CreatureStats {
    CreatureStats::from_value(json!({
        "entries": {"299": {"name": "Young Wolf", "level_min": 1, "level_max": 2, "type": 1,
            "family": 1, "rank": 0, "class": 1, "health_mult": 1.0, "damage_mult": 1.0,
            "variance": 0.02, "attack_ms": 2000, "faction": 32}},
        "levels": {"1": {"1": [1.57945, 42], "2": [2.31493, 55]}}}))
}
fn mirror() -> Mirror {
    let mut m = Mirror::new("s1");
    m.mobs = parse_mobs(MOBS);
    m.mapping = Mapping::from_world(&json!({"Active": true, "WowOrigin": [-8922.0, 43.0, 131.0],
        "Centre": [-1400.0, 120.0, -350.0]}));
    m
}

#[test]
fn mobs_file_parses_the_hero_and_living_mobs() {
    let m = parse_mobs(MOBS);
    assert_eq!(m.seq, 7);
    let h = m.hero.unwrap();
    assert_eq!(
        (h.max_hp, h.level, h.pos),
        (52, 1, Point(-8920., 53., 132.))
    );
    assert_eq!(m.mobs.len(), 4, "dead and bad lines skipped");
    assert_eq!(m.mobs[0].name, "Young Wolf");
    assert!(m.mobs[0].hostile && !m.mobs[2].hostile);
    assert_eq!((m.mobs[1].hp, m.mobs[1].flags), (50, 3));
}

#[test]
fn hero_state_reaches_the_plan() {
    // benilla 0025 ends the hero line with its state; older lines read as alive.
    assert_eq!(parse_mobs(MOBS).hero.unwrap().state, 0);
    for (line, state, name) in [
        ("1", 1, "dead"),
        ("2", 2, "ghost"),
        ("0", 0, "alive"),
        ("9", 0, "alive"),
    ] {
        let text = MOBS.replace("52 52 1\n", &format!("0 52 1 {line}\n"));
        let mut m = mirror();
        m.mobs = parse_mobs(&text);
        assert_eq!(m.hero().unwrap().state, state, "{line}");
        let plan = m.plan(1, "s1", &[], None, "wolf-t", None, "neutral-f", 0);
        assert_eq!(plan["HeroState"], name);
        assert_eq!(plan["HeroWowHp"], 0);
        assert_eq!(plan["HeroWowMaxHp"], 52);
    }
}

#[test]
fn mapping_and_facing_follow_world_mode() {
    let m = mirror();
    let map = m.mapping.unwrap();
    let p = map.to_bg3(Point(-8920., 53., 132.));
    let yd = 0.9144;
    assert!((p[0] - (-1400. + 10. * yd)).abs() < 1e-9);
    assert!((p[1] - (120. + 1. * yd)).abs() < 1e-9);
    assert!((p[2] - (-350. - 2. * yd)).abs() < 1e-9);
    // Facing WoW +x (orientation 0) is BG3 -z; facing WoW +y (pi/2) is BG3 +x.
    let f = bg3_forward(0.);
    assert!(f[0].abs() < 1e-9 && (f[1] + 1.).abs() < 1e-9);
    let f = bg3_forward(std::f64::consts::FRAC_PI_2);
    assert!((f[0] - 1.).abs() < 1e-9 && f[1].abs() < 1e-9);
}

#[test]
fn plan_lists_near_mobs_fight_members_and_wow_numbers() {
    let mut m = mirror();
    let s = stats();
    let far = "f130000000012b04".to_string();
    let plan = m.plan(
        3,
        "s1",
        std::slice::from_ref(&far),
        Some(&s),
        "wolf-t",
        Some("hostile-f"),
        "neutral-f",
        0,
    );
    assert_eq!(plan["Active"], true);
    assert_eq!(plan["HeroWowMaxHp"], 52);
    let mobs = plan["Mobs"].as_array().unwrap();
    let guids: Vec<&str> = mobs.iter().map(|v| v["Guid"].as_str().unwrap()).collect();
    // The fight's mob first, then the others.
    assert_eq!(guids[0], "f130000000012b04");
    assert_eq!(guids.len(), 4);
    let wolf = mobs
        .iter()
        .find(|v| v["Guid"] == "f130000000012b02")
        .unwrap();
    assert_eq!(wolf["Template"], "wolf-t");
    assert_eq!(wolf["Hostile"], true);
    assert_eq!(wolf["WowMaxHp"], 55, "level 2 template health");
    assert_eq!(wolf["WowHp"], 28, "half of it, from the client's percent");
    let kobold = mobs
        .iter()
        .find(|v| v["Guid"] == "f130000000012b03")
        .unwrap();
    assert_eq!(
        (kobold["Kind"].as_str(), kobold["Hostile"].as_bool()),
        (Some("goblin"), Some(false))
    );
    assert_eq!(plan["Fight"][0], "f130000000012b04");
    // Out of a fight the wolf 70 yd off is mirrored too: WoW has it loaded (no range of ours).
    let plan = m.plan(4, "s1", &[], Some(&s), "wolf-t", None, "neutral-f", 0);
    let guids: Vec<&str> = plan["Mobs"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v["Guid"].as_str().unwrap())
        .collect();
    assert_eq!(guids.len(), 4);
    assert_eq!(guids[3], "f130000000012b04", "nearest first");
    // Settled mobs (an ended fight's) are not mirrored for a while.
    m.settle(["f130000000012b01".to_string()], 0);
    let plan = m.plan(4, "s1", &[], Some(&s), "wolf-t", None, "neutral-f", 1000);
    assert_eq!(plan["Mobs"].as_array().unwrap().len(), 3);
    let plan = m.plan(5, "s1", &[], Some(&s), "wolf-t", None, "neutral-f", 61_000);
    assert_eq!(plan["Mobs"].as_array().unwrap().len(), 4);
    // Without world mode nothing is mirrored.
    m.mapping = None;
    assert_eq!(
        m.plan(6, "s1", &[], None, "w", None, "n", 0)["Active"],
        false
    );
}

#[test]
fn bg3_combat_with_mirrors_makes_a_pull() {
    let mut m = mirror();
    m.status = parse_status(
        &json!({"Seq": 3, "Tav": {"Hp": 14, "MaxHp": 14, "InCombat": true},
        "Mobs": {"f130000000012b02": {"Uuid": "u2", "Hp": 6, "MaxHp": 11, "InCombat": true},
                 "f130000000012b01": {"Uuid": "u1", "Hp": 11, "MaxHp": 11, "InCombat": false, "Searching": true},
                 "F130000000012B03": {"Uuid": "u3", "Hp": 0, "MaxHp": 11, "InCombat": true, "Dead": true}}}),
    );
    assert_eq!(m.in_bg3_combat(0), vec!["f130000000012b02".to_string()]);
    let pull = m.pull(&m.in_bg3_combat(0), 41).unwrap();
    assert_eq!(pull["seq"], 41);
    let enemies = wow_bg3_link::bridge::units(&pull);
    assert_eq!(enemies.len(), 1);
    assert_eq!(
        (enemies[0].entry, enemies[0].hp, enemies[0].max_hp),
        (299, 50, 100)
    );
    assert_eq!(pull["hero"]["max_hp"], 52);
    let text = m.mirrored_text();
    assert!(text.contains("m f130000000012b01\n") && !text.contains("m f130000000012b03"));
    // A dead mob's stand-in corpse (guids read lower case): BG3's to click (its loot window), not WoW's.
    assert!(text.contains("c f130000000012b03\n"));
    // A robbed NPC searching: benilla holds its model where BG3 walks the stand-in.
    assert!(text.contains("s f130000000012b01\n") && !text.contains("s f130000000012b02"));
    // A mob that joined the fight with no mirror has a stand-in all the same (portrait, clicks).
    m.in_fight = vec!["f130000044000001".into(), "f130000000012b01".into()];
    let text = m.mirrored_text();
    assert!(text.contains("m f130000044000001\n"));
    assert_eq!(text.matches("m f130000000012b01\n").count(), 1);
    m.status.tav_combat = false;
    assert!(m.in_bg3_combat(0).is_empty());
}

struct Record(Vec<Handoff>);
impl ServerHandoff for Record {
    fn begin(&mut self, h: &Handoff) -> Result<String, Error> {
        self.0.push(h.clone());
        Ok("r".into())
    }
    fn send(&mut self, h: &Handoff) -> Result<(), Error> {
        self.0.push(h.clone());
        Ok(())
    }
}

#[test]
fn a_one_blow_kill_outside_a_fight_still_reaches_the_server() {
    let mut m = mirror();
    m.status = parse_status(&json!({"Tav": {"Hp": 14, "MaxHp": 14},
        "Mobs": {"f130000000012b01": {"Uuid": "u1", "Hp": 0, "MaxHp": 11, "Dead": true, "HitByTav": true},
                 "f130000000012b02": {"Uuid": "u2", "Hp": 0, "MaxHp": 11, "Dead": true}}}));
    let mut rec = Record(Vec::new());
    wow_bg3_link::bridge::kills_outside_fights(&mut m, &mut rec).unwrap();
    // Only the one Tav hurt (a stand-in that fell gives no credit); begin, hp 0, end.
    assert_eq!(rec.0.len(), 3);
    let Handoff::Begin { encounter, mobs } = &rec.0[0] else {
        panic!()
    };
    assert_eq!(mobs, &vec!["f130000000012b01".to_string()]);
    assert_eq!(
        rec.0[1],
        Handoff::Hp {
            encounter: encounter.clone(),
            mob: "f130000000012b01".into(),
            hp: 0,
            max_hp: 11
        }
    );
    assert!(matches!(&rec.0[2], Handoff::End { outcome, .. } if outcome == "victory"));
    // Waiting for the server's answer: not sent twice.
    wow_bg3_link::bridge::kills_outside_fights(&mut m, &mut rec).unwrap();
    assert_eq!(rec.0.len(), 3);
}

fn kill_encounter(h: &Handoff) -> String {
    match h {
        Handoff::Begin { encounter, .. } => encounter.clone(),
        other => panic!("{other:?}"),
    }
}

#[test]
fn a_kill_refused_because_the_player_is_dead_waits_for_the_hero_and_is_not_settled() {
    const GUID: &str = "f130000000012b01";
    let mut m = mirror();
    m.status = parse_status(&json!({"Tav": {"Hp": 0, "MaxHp": 14},
        "Mobs": {GUID: {"Uuid": "u1", "Hp": 0, "MaxHp": 11, "Dead": true, "HitByTav": true}}}));
    let dead_hero = "seq 8\nhero 0000000000000001 -8920 53 132 0.5 0 52 1 1\n\
        mob f130000000012b01 299 1 1 -8915 53 132 3.14159 100 100 0 Young Wolf\n";
    m.mobs = parse_mobs(dead_hero);
    let mut rec = Record(Vec::new());
    wow_bg3_link::bridge::kills_outside_fights(&mut m, &mut rec).unwrap();
    assert_eq!(rec.0.len(), 3);
    let first = kill_encounter(&rec.0[0]);
    m.kill_event(&format!("WOWBG3 begin {first} refused dead -"));
    m.kill_event(&format!("WOWBG3 hp {first} F130000000012B01 refused unknown"));
    m.kill_event(&format!("WOWBG3 end {first} refused unknown"));
    // Refused: still dead, nothing sent, and the mob stays listed for the mod.
    wow_bg3_link::bridge::kills_outside_fights(&mut m, &mut rec).unwrap();
    assert_eq!(rec.0.len(), 3);
    assert!(m.wanted(&[], m.now()).iter().any(|u| u.guid == GUID));
    // The hero lives: at once, as a new encounter.
    m.mobs = parse_mobs(MOBS);
    wow_bg3_link::bridge::kills_outside_fights(&mut m, &mut rec).unwrap();
    assert_eq!(rec.0.len(), 6);
    let second = kill_encounter(&rec.0[3]);
    assert_ne!(first, second);
    // Landed: settled, not sent again.
    m.kill_event(&format!("WOWBG3 hp {second} F130000000012B01 killed 0 42"));
    wow_bg3_link::bridge::kills_outside_fights(&mut m, &mut rec).unwrap();
    assert_eq!(rec.0.len(), 6);
    assert!(!m.wanted(&[], m.now()).iter().any(|u| u.guid == GUID));
}

#[test]
fn a_kill_refused_for_another_reason_is_asked_again_later() {
    const GUID: &str = "f130000000012b01";
    let mut m = mirror();
    m.status = parse_status(&json!({"Tav": {"Hp": 14, "MaxHp": 14},
        "Mobs": {GUID: {"Uuid": "u1", "Hp": 0, "MaxHp": 11, "Dead": true, "HitByTav": true}}}));
    m.kill_sent(GUID, "kill-1", 1_000);
    m.kill_event("WOWBG3 begin kill-1 refused far F130000000012B01");
    assert!(m.kills_due(1_000 + KILL_RETRY_MS - 1).is_empty());
    assert_eq!(m.kills_due(1_000 + KILL_RETRY_MS), vec![GUID.to_string()]);
    // The mob itself dead in WoW: nothing more to send.
    m.kill_event("WOWBG3 begin kill-1 refused dead F130000000012B01");
    assert!(m.settled.contains_key(GUID));
    assert!(m.kills_due(m.now()).is_empty());
}

#[test]
fn a_dead_stand_ins_mob_counts_as_respawned_only_after_a_confirmed_kill_and_leaving_the_list() {
    const GUID: &str = "f130000000012b01";
    let dead = json!({"Tav": {"Hp": 14, "MaxHp": 14},
        "Mobs": {GUID: {"Uuid": "u1", "Hp": 0, "MaxHp": 11, "Dead": true}}});
    let without = "seq 9\nhero 0000000000000001 -8920 53 132 0.5 52 52 1\n";
    let mut m = mirror();
    m.status = parse_status(&dead);
    m.note_respawns();
    assert!(m.respawned().is_empty(), "the kill has not landed yet");
    // Confirmed, and benilla has not caught up: still no respawn.
    m.kill_event("WOWBG3 hp pull-1 F130000000012B01 killed 0 42");
    m.note_respawns();
    assert!(m.respawned().is_empty());
    // Gone from mobs.txt (a corpse), then back: WoW respawned it.
    m.mobs = parse_mobs(without);
    m.note_respawns();
    assert!(m.respawned().is_empty());
    m.mobs = parse_mobs(MOBS);
    m.note_respawns();
    assert_eq!(m.respawned(), vec![GUID.to_string()]);
    assert_eq!(
        m.plan(1, "s1", &[], None, "t", None, "n", 0)["WowRespawned"],
        json!([GUID])
    );
    // Its stand-in is remade: done with.
    m.status = parse_status(&json!({"Tav": {"Hp": 14, "MaxHp": 14},
        "Mobs": {GUID: {"Uuid": "u2", "Hp": 11, "MaxHp": 11}}}));
    m.note_respawns();
    assert!(m.respawned().is_empty());
}

#[test]
fn a_respawned_mob_is_not_killed_by_its_old_corpse() {
    const GUID: &str = "f130000000012b01";
    let corpse = json!({"Tav": {"Hp": 14, "MaxHp": 14},
        "Mobs": {GUID: {"Uuid": "u1", "Hp": 0, "MaxHp": 11, "Dead": true, "HitByTav": true}}});
    let without = "seq 9\nhero 0000000000000001 -8920 53 132 0.5 52 52 1\n";
    let mut m = mirror();
    // The fight's last blow: WoW confirms it, the fight's end settles the mob, it leaves the list.
    m.status = parse_status(&corpse);
    m.kill_event("WOWBG3 hp pull-1 F130000000012B01 killed 0 42");
    m.settle([GUID.to_string()], 1_000);
    m.mobs = parse_mobs(without);
    m.note_respawns();
    assert!(m.kills_due(1_000 + SETTLE_MS).is_empty());
    // WoW respawns it past the settling time. The poll that first lists it still reads the corpse's
    // status (the mod removes it only after this plan): the corpse goes, the kill does not.
    let after = 180_000;
    m.mobs = parse_mobs(MOBS);
    m.note_respawns();
    assert_eq!(m.respawned(), vec![GUID.to_string()]);
    assert!(m.dead_unfought(after).is_empty());
    assert!(m.kills_due(after).is_empty());
    let mut rec = Record(Vec::new());
    wow_bg3_link::bridge::kills_outside_fights(&mut m, &mut rec).unwrap();
    assert!(rec.0.is_empty(), "free kill: {:?}", rec.0);
    // Its new stand-in lives; that life's own one-blow kill is owed again.
    m.status = parse_status(&json!({"Tav": {"Hp": 14, "MaxHp": 14},
        "Mobs": {GUID: {"Uuid": "u2", "Hp": 11, "MaxHp": 11}}}));
    m.note_respawns();
    assert!(m.kills_due(after + 1_000).is_empty());
    m.status = parse_status(&json!({"Tav": {"Hp": 14, "MaxHp": 14},
        "Mobs": {GUID: {"Uuid": "u2", "Hp": 0, "MaxHp": 11, "Dead": true, "HitByTav": true}}}));
    m.note_respawns();
    assert_eq!(m.kills_due(after + 2_000), vec![GUID.to_string()]);
}

#[test]
fn world_mode_arms_sight_mode_until_the_server_confirms_it() {
    let mut m = Mirror::new("s1");
    m.mobs = parse_mobs(MOBS);
    // Before world mode: one plain arm (it keeps the server's sight mode), however it answers.
    assert_eq!(m.arm_due(0), Some(Handoff::Arm { sight: None }));
    m.arm_event("WOWBG3 armed 1 sight");
    assert_eq!(m.arm_due(20_000), None);
    m.arm_event("WOWBG3 armed 1");
    assert_eq!(m.arm_due(40_000), None);
    // World mode: sight mode, again every 15 s until confirmed.
    m.mapping = Mapping::from_world(
        &json!({"Active": true, "WowOrigin": [0.0, 0.0, 0.0], "Centre": [0.0, 0.0, 0.0]}),
    );
    assert_eq!(m.arm_due(42_000), Some(Handoff::Arm { sight: Some(true) }));
    assert_eq!(m.arm_due(50_000), None);
    assert_eq!(m.arm_due(57_001), Some(Handoff::Arm { sight: Some(true) }));
    assert_eq!(
        Handoff::Arm { sight: Some(true) }.command().unwrap(),
        ".wowbg3 arm sight"
    );
    assert_eq!(
        Handoff::Arm { sight: Some(false) }.command().unwrap(),
        ".wowbg3 arm nosight"
    );
    m.arm_event("WOWBG3 armed 1 sight");
    assert_eq!(m.arm_due(60_000), None);
    // A BG3 or world restart (no mapping for a while) keeps sight mode: no plain arm.
    m.mapping = None;
    assert_eq!(m.arm_due(80_000), None);
    // A death ends sight mode on the server: asked again at once, at most every 2 s.
    m.arm_event("WOWBG3 sight 0 died");
    assert_eq!(m.arm_due(80_500), Some(Handoff::Arm { sight: Some(true) }));
    m.arm_event("WOWBG3 sight 0 died");
    assert_eq!(m.arm_due(81_500), None);
    assert_eq!(m.arm_due(82_501), Some(Handoff::Arm { sight: Some(true) }));
    // Without a new death, a missing confirmation waits 15 s again.
    assert_eq!(m.arm_due(90_000), None);
    assert_eq!(m.arm_due(97_502), Some(Handoff::Arm { sight: Some(true) }));
}

#[test]
fn a_victory_kills_enemies_whose_last_blow_never_reached_wow() {
    use wow_bg3_link::wire::{Message, Outcome};
    let basis = Basis {
        wow_origin: Point(0., 0., 0.),
        bridge_origin: Point(0., 0., 0.),
        yaw: 0.,
    };
    let hero = Combatant::new("hero", "0000000000000001", Point(0., 0., 0.), 52, 52);
    let wolf = Combatant::new("enemy", "f13000020d013722", Point(5., 0., 0.), 27, 27);
    let mut f = Fight::new("pull-1", basis, hero, vec![wolf]);
    f.handoff.clear();
    f.state = "active";
    // BG3 knew the wolf at 7/27 last.
    f.apply(&Message::Roster {
        encounter: "pull-1".into(),
        joins: 0,
        actors: vec![wow_bg3_link::wire::Actor {
            id: "e1".into(),
            wow_guid: "f13000020d013722".into(),
            position: Point(5., 0., 0.),
            hp: 27,
            max_hp: 27,
        }],
    })
    .unwrap();
    f.apply(&Message::Roster {
        encounter: "pull-1".into(),
        joins: 0,
        actors: vec![wow_bg3_link::wire::Actor {
            id: "e1".into(),
            wow_guid: "f13000020d013722".into(),
            position: Point(5., 0., 0.),
            hp: 7,
            max_hp: 27,
        }],
    })
    .unwrap();
    f.handoff.clear();
    f.apply(&Message::Finished {
        encounter: "pull-1".into(),
        outcome: Outcome::Victory,
    })
    .unwrap();
    let h: Vec<_> = f.handoff.iter().cloned().collect();
    assert!(matches!(&h[0], Handoff::Pos { mob, .. } if mob == "f13000020d013722"));
    assert_eq!(
        h[1],
        Handoff::Hp {
            encounter: "pull-1".into(),
            mob: "f13000020d013722".into(),
            hp: 0,
            max_hp: 27
        }
    );
    assert!(matches!(&h[2], Handoff::End { outcome, .. } if outcome == "victory"));
}

#[test]
fn a_mob_tav_attacked_outside_bg3_combat_is_pulled() {
    let mut m = mirror();
    // A missed attack on a neutral stand-in: BG3 starts no combat, WoW would.
    m.status = parse_status(&json!({"Tav": {"Hp": 14, "MaxHp": 14, "InCombat": false},
        "Mobs": {"f130000000012b03": {"Uuid": "u3", "Hp": 11, "MaxHp": 11, "HitByTav": true},
                 "f130000000012b01": {"Uuid": "u1", "Hp": 11, "MaxHp": 11},
                 "f130000000012b02": {"Uuid": "u2", "Hp": 0, "MaxHp": 11, "Dead": true, "HitByTav": true}}}));
    assert!(m.in_bg3_combat(0).is_empty());
    assert_eq!(m.provoked(0), vec!["f130000000012b03".to_string()]);
    let pull = m.pull(&m.provoked(0), 9).unwrap();
    assert_eq!(
        wow_bg3_link::bridge::units(&pull)[0].guid,
        "f130000000012b03"
    );
    // Already in BG3's combat: that path pulls it instead.
    m.status = parse_status(&json!({"Tav": {"Hp": 14, "MaxHp": 14, "InCombat": true},
        "Mobs": {"f130000000012b03": {"Uuid": "u3", "Hp": 3, "MaxHp": 11, "HitByTav": true, "InCombat": true}}}));
    assert!(m.provoked(0).is_empty());
}

#[test]
fn to_wow_inverts_to_bg3() {
    let map = mirror().mapping.unwrap();
    let p = Point(-8915.5, 47.25, 133.0);
    let back = map.to_wow(map.to_bg3(p));
    assert!(
        (back.0 - p.0).abs() < 1e-9 && (back.1 - p.1).abs() < 1e-9 && (back.2 - p.2).abs() < 1e-9
    );
}

#[test]
fn fight_enemies_follow_their_stand_ins() {
    let status = parse_status(&json!({"Mobs": {"f130000000012b01": {"Uuid": "u1", "Hp": 9,
        "MaxHp": 9, "Pos": [-1390.0, 121.0, -352.0]}}}));
    let map = mirror().mapping.unwrap();
    let wow = map.to_wow(status.mobs["f130000000012b01"].pos.unwrap());
    let basis = Basis {
        wow_origin: Point(-8920., 53., 132.),
        bridge_origin: Point(0., 0., 0.),
        yaw: 0.,
    };
    let hero = Combatant::new("hero", "0000000000000001", Point(-8920., 53., 132.), 52, 52);
    let c = Combatant::new("e1", "f130000000012b01", Point(-8915., 53., 132.), 42, 42);
    let mut f = Fight::new("pull-5", basis, hero, vec![c]);
    f.enemies[0].status = "in";
    assert!(f.follow_standins(&[("f130000000012b01".into(), wow)]));
    assert_eq!(f.enemies[0].pos, wow);
    assert!(!f.follow_standins(&[("f130000000012b01".into(), wow)]));
}

/// Friendly NPCs, REACTION 2 in benilla 0027's `mobs.txt`.
const FRIENDLY: &str = "mob f130000000013c01 1213 5 2 -8918 53 132 0 100 100 0 Godric Rothgar\n\
mob f130000000013c02 1642 55 2 -8920 73 132 0 100 100 0 Northshire Guard\n\
mob f130000000013c03 1642 55 2 -8920 103 132 0 100 100 0 Northshire Guard\n";

#[test]
fn every_loaded_creature_is_mirrored_nearest_first() {
    let mut m = mirror();
    m.mobs = parse_mobs(&format!("{MOBS}{FRIENDLY}"));
    let g = m.unit("f130000000013c01").unwrap();
    assert!(g.friendly && !g.hostile);
    assert!(m.unit("f130000000012b01").is_some_and(|u| !u.friendly));
    // Every friendly NPC WoW lists has one, the guard 50 yd away too; all nearest first, so the
    // quest giver 2 yd off comes before the wolf 5 yd off and the wolf 70 yd off after the guard.
    let wanted: Vec<String> = m.wanted(&[], 0).iter().map(|u| u.guid.clone()).collect();
    let at = |g: &str| wanted.iter().position(|w| w == g).unwrap();
    assert_eq!(at("f130000000013c01"), 0);
    assert!(at("f130000000013c01") < at("f130000000012b01"));
    assert!(at("f130000000013c02") < at("f130000000013c03"));
    assert!(at("f130000000013c03") < at("f130000000012b04"));
    // No count of ours: a field of 80 wolves leaves no creature WoW has without a stand-in.
    let mut text = String::from(MOBS);
    for i in 0..80 {
        text += &format!("mob f1300000000140{i:02x} 299 1 1 -8919 53 132 0 100 100 0 Young Wolf\n");
    }
    text += FRIENDLY;
    m.mobs = parse_mobs(&text);
    let wanted = m.wanted(&[], 0);
    assert_eq!(wanted.len(), m.mobs.mobs.len());
    assert_eq!(wanted.iter().filter(|u| u.friendly).count(), 3);
}

#[test]
fn friendly_npcs_fight_only_through_bg3_combat_and_as_provoked_for_the_server() {
    let mut m = mirror();
    m.mobs = parse_mobs(&format!(
        "{MOBS}mob f130000000013c03 1642 55 2 -8921 53 132 0 100 100 0 Northshire Guard\n"
    ));
    let stats = CreatureStats::from_value(json!({
        "entries": {"1642": {"name": "Northshire Guard", "level_min": 55, "level_max": 55,
            "type": 7, "family": 0, "rank": 0, "class": 1, "health_mult": 2.0, "damage_mult": 1.0,
            "variance": 0.0, "attack_ms": 2000, "faction": 11, "flags_extra": 1024},
            "299": {"name": "Young Wolf", "level_min": 1, "level_max": 2, "type": 1, "family": 1,
            "rank": 0, "class": 1, "health_mult": 1.0, "damage_mult": 1.0, "variance": 0.02,
            "attack_ms": 2000, "faction": 32}},
        "levels": {"1": {"1": [1.57945, 42], "2": [2.31493, 55], "55": [80.0, 3000]}}}));
    let plan = m.plan(1, "s1", &[], Some(&stats), "wolf-t", None, "neutral-f", 0);
    let mob = |g: &str| {
        plan["Mobs"]
            .as_array()
            .unwrap()
            .iter()
            .find(|v| v["Guid"] == g)
            .cloned()
            .unwrap()
    };
    assert_eq!(
        (
            mob("f130000000013c03")["Friendly"].clone(),
            mob("f130000000013c03")["Guard"].clone()
        ),
        (json!(true), json!(true))
    );
    assert_eq!(
        (
            mob("f130000000012b01")["Friendly"].clone(),
            mob("f130000000012b01")["Guard"].clone()
        ),
        (json!(false), json!(false))
    );
    // A hit alone pulls no friendly NPC (BG3's crime flow decides).
    m.status = parse_status(
        &json!({"Tav": {"Hp": 14, "MaxHp": 14, "InCombat": false, "Ftb": true},
        "Mobs": {"f130000000013c03": {"Uuid": "u9", "Hp": 20, "MaxHp": 20, "HitByTav": true}}}),
    );
    assert!(m.provoked(0).is_empty());
    // BG3's combat with it (after its crime reaction) does, as `!GUID` for the server.
    m.status = parse_status(&json!({"Tav": {"Hp": 14, "MaxHp": 14, "InCombat": true},
        "Mobs": {"f130000000013c03": {"Uuid": "u9", "Hp": 20, "MaxHp": 20, "HitByTav": true,
            "InCombat": true}}}));
    assert_eq!(m.in_bg3_combat(0), vec!["f130000000013c03".to_string()]);
    let pull = m.pull(&m.in_bg3_combat(0), 5).unwrap();
    let units = wow_bg3_link::bridge::units(&pull);
    assert!(units[0].provoke);
    assert_eq!(units[0].server_guid(), "!f130000000013c03");
    assert_eq!(m.server_guid("f130000000013c03"), "!f130000000013c03");
    assert_eq!(m.server_guid("f130000000012b01"), "f130000000012b01");
    assert_eq!(
        Handoff::Begin {
            encounter: "e1".into(),
            mobs: vec![units[0].server_guid(), "f130000000012b01".into()]
        }
        .command()
        .unwrap(),
        ".wowbg3 begin e1 !f130000000013c03,f130000000012b01"
    );
    // A friendly NPC killed with one blow outside a fight goes to the server provoked too.
    m.status = parse_status(&json!({"Tav": {"Hp": 14, "MaxHp": 14},
        "Mobs": {"f130000000013c03": {"Uuid": "u9", "Hp": 0, "MaxHp": 20, "Dead": true, "HitByTav": true}}}));
    let mut rec = Record(Vec::new());
    wow_bg3_link::bridge::kills_outside_fights(&mut m, &mut rec).unwrap();
    let Handoff::Begin { mobs, .. } = &rec.0[0] else {
        panic!("{:?}", rec.0)
    };
    assert_eq!(mobs, &vec!["!f130000000013c03".to_string()]);
}

#[test]
fn mobs_round_every_party_member_are_mirrored_at_any_distance_wow_lists() {
    use wow_bg3_link::mirror::parse_members;
    let mut m = mirror();
    let hero = m.hero().unwrap().pos;
    // A companion 600 yd north of the hero and 40 yd higher (no member stands at that height yet),
    // with a mob 10 yd from it and one 90 yd from it (WoW's visibility reaches 100 yd).
    let far = (hero.0 + 600.0, hero.1, hero.2 + 40.0);
    let text = format!(
        "{MOBS}mob f130000000099001 299 5 1 {} {} {} 0.0 50 50 0 Far Wolf\nmob f130000000099002 299 5 1 {} {} {} 0.0 50 50 0 Edge Wolf\n",
        far.0 + 10.0,
        far.1,
        far.2,
        far.0 + 90.0,
        far.1,
        far.2
    );
    m.mobs = parse_mobs(&text);
    let wanted = |m: &Mirror| {
        m.wanted(&[], 0)
            .iter()
            .map(|u| u.guid.clone())
            .collect::<Vec<_>>()
    };
    assert!(
        !wanted(&m).contains(&"f130000000099001".to_string()),
        "no member there yet"
    );
    m.members = parse_members(&serde_json::json!({"Format": 1, "Members": [
        {"Uuid": "hero-uuid", "Hero": true, "Wow": [hero.0, hero.1, hero.2]},
        {"Uuid": "astarion", "Name": "Astarion", "Wow": [far.0, far.1, far.2]},
        {"Uuid": "dead-one", "Dead": true, "Wow": [0.0, 0.0, 0.0]}]}));
    assert_eq!(m.members.len(), 1);
    let w = wanted(&m);
    assert!(
        w.contains(&"f130000000099001".to_string()),
        "the mob near the companion is mirrored"
    );
    assert!(
        w.contains(&"f130000000099002".to_string()),
        "and the one 90 yd from it: WoW lists it, so no range of ours leaves it out"
    );
}

#[test]
fn feed_files_add_mobs_round_far_members() {
    use wow_bg3_link::mirror::read_feed;
    let dir = std::env::temp_dir().join(format!("wowbg3-feed-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(dir.join("feed")).unwrap();
    std::fs::write(dir.join("mobs.txt"), MOBS).unwrap();
    std::fs::write(
        dir.join("feed/astarion.txt"),
        "mob f130000000099003 299 5 1 100.0 200.0 30.0 0.5 40 50 0 Feed Wolf\n",
    )
    .unwrap();
    let feed = read_feed(&dir).unwrap();
    assert!(
        feed.mobs
            .iter()
            .any(|u| u.guid == "f130000000099003" && u.hp == 40)
    );
    assert_eq!(feed.hero, parse_mobs(MOBS).hero);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_servers_feed_adds_creatures_and_the_party_link_moves_bots() {
    use std::collections::HashMap;
    use wow_bg3_link::mirror::{PartyLink, parse_cr};
    let mut m = mirror();
    // A feed line for Astarion's bot: a hostile wolf 600 yd from the hero.
    let line = "WOWBG3 cr Astarion F13000012B099001 299 2 30 55 1 32 8 -8320.0 53.0 132.0 1.5 447 0 ma Young Wolf";
    let (member, unit) = parse_cr(line).unwrap();
    let unit = unit.unwrap();
    assert_eq!(
        (
            member.as_str(),
            unit.guid.as_str(),
            unit.hostile,
            unit.friendly
        ),
        ("Astarion", "f13000012b099001", true, false)
    );
    assert!(
        parse_cr("WOWBG3 cr Astarion F13000012B099002 299 2 0 55 1 32 8 1 2 3 0 447 0 dl Dead")
            .unwrap()
            .1
            .is_none()
    );
    m.feed_event(line);
    m.feed_event("WOWBG3 tick Astarion 4 1 0 -8330.0 53.0 132.0");
    let dir = std::env::temp_dir().join(format!("wowbg3-party-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("mobs.txt"), MOBS).unwrap();
    std::fs::write(dir.join("world.json"), json!({"Active": true, "WowOrigin": [-8922.0, 43.0, 131.0], "Centre": [-1400.0, 120.0, -350.0], "Map": 0}).to_string()).unwrap();
    std::fs::write(
        dir.join("party-wow.json"),
        json!({"Format": 1, "Members": [
        {"Uuid": "hero-u", "Hero": true, "Wow": [-8920.0, 53.0, 132.0]},
        {"Uuid": "ast-u", "Name": "Astarion", "Wow": [-8330.0, 53.0, 132.0]}]})
        .to_string(),
    )
    .unwrap();
    std::fs::write(
        dir.join("party-bots.json"),
        json!({"Bots": ["Astarion"], "Hero": "Test"}).to_string(),
    )
    .unwrap();
    std::fs::write(
        dir.join("wowchars.json"),
        json!({"Chars": {"hero-u": "Test", "ast-u": "Astarion"}}).to_string(),
    )
    .unwrap();
    m.poll(&dir, &[], None, "wolf", None, "neutral").unwrap();
    assert!(
        m.mobs.mobs.iter().any(|u| u.guid == "f13000012b099001"),
        "the feed's wolf joins benilla's mobs"
    );
    assert!(
        m.wanted(&[], 0)
            .iter()
            .any(|u| u.guid == "f13000012b099001"),
        "and is mirrored by Astarion"
    );
    m.feed_event("WOWBG3 crgone Astarion F13000012B099001");
    assert!(m.feeds["Astarion"].is_empty());
    // The party link: a move and the feed on, nothing again at once, a move after 3 yd and 1 s.
    let mut p = PartyLink::default();
    let lines = |v: Vec<Handoff>| v.iter().map(|h| h.command().unwrap()).collect::<Vec<_>>();
    let none: HashMap<String, u64> = HashMap::new();
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 1000, &none)),
        vec![
            ".wowbg3 move Astarion -8330.00 53.00 132.00 0",
            ".wowbg3 feed Astarion on 250 500"
        ]
    );
    assert!(p.step(&dir, &dir, 0, 1500, &none).is_empty());
    std::fs::write(
        dir.join("party-wow.json"),
        json!({"Format": 1, "Members": [
        {"Uuid": "ast-u", "Wow": [-8320.0, 53.0, 132.0]}]})
        .to_string(),
    )
    .unwrap();
    // Within 35 yd the bot walks (patch 0015), at the member's pace.
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 2100, &none)),
        vec![".wowbg3 walk Astarion -8320.00 53.00 132.00 0 1.10"]
    );
    let at = |x: f64| {
        std::fs::write(
            dir.join("party-wow.json"),
            json!({"Format": 1, "Members": [{"Uuid": "ast-u", "Wow": [x, 53.0, 132.0]}]})
                .to_string(),
        )
        .unwrap()
    };
    // No answer in 5 s from a server that never answered a walk: `move` instead, for a minute.
    at(-8318.0);
    assert!(
        p.step(&dir, &dir, 0, 2400, &none).is_empty(),
        "under 500 ms"
    );
    at(-8316.0);
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 7200, &none)),
        vec![".wowbg3 move Astarion -8316.00 53.00 132.00 0"]
    );
    at(-8314.0);
    assert!(
        p.step(&dir, &dir, 0, 8300, &none).is_empty(),
        "a move needs 3 yd"
    );
    // A server with the verb: ok answers keep the walk; `refused far` sends one move.
    let mut p = PartyLink::default();
    p.step(&dir, &dir, 0, 1000, &none);
    at(-8312.0);
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 1600, &none)),
        vec![".wowbg3 walk Astarion -8312.00 53.00 132.00 0 0.60"]
    );
    p.event("WOWBG3 walk Astarion ok 0 -8312.00 53.00 132.00 3.33");
    at(-8310.0);
    p.event("WOWBG3 walk Astarion refused far 41.20");
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 2200, &none)),
        vec![".wowbg3 move Astarion -8310.00 53.00 132.00 0"]
    );
    at(-8270.0);
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 3300, &none)),
        vec![".wowbg3 move Astarion -8270.00 53.00 132.00 0"],
        "beyond 35 yd: the teleport"
    );
    at(-8268.0);
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 3900, &none)),
        vec![".wowbg3 walk Astarion -8268.00 53.00 132.00 0 0.60"]
    );
    // Far Sight onto Astarion's bot, then back.
    std::fs::write(
        dir.join("farsight.json"),
        json!({"Format": 1, "On": true, "Uuid": "ast-u"}).to_string(),
    )
    .unwrap();
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 2200, &none)),
        vec![".wowbg3 view Astarion"]
    );
    std::fs::write(
        dir.join("farsight.json"),
        json!({"Format": 1, "On": false}).to_string(),
    )
    .unwrap();
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 2300, &none)),
        vec![".wowbg3 view self"]
    );
    // A refused Far Sight (t58: the bot offline) is asked again 2 s after the last ask, while the
    // player still controls that member; a refusal of another name, or an ok, changes nothing.
    std::fs::write(
        dir.join("farsight.json"),
        json!({"Format": 1, "On": true, "Uuid": "ast-u"}).to_string(),
    )
    .unwrap();
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 10_000, &none)),
        vec![".wowbg3 view Astarion"]
    );
    p.event("WOWBG3 view Karlach refused offline");
    assert!(p.step(&dir, &dir, 0, 13_000, &none).is_empty());
    p.event("WOWBG3 view Astarion refused offline");
    assert!(
        p.step(&dir, &dir, 0, 11_000, &none).is_empty(),
        "not within 2 s of the ask"
    );
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 13_100, &none)),
        vec![".wowbg3 view Astarion"]
    );
    assert!(p.step(&dir, &dir, 0, 16_000, &none).is_empty(), "asked once");
    p.event("WOWBG3 view Astarion ok 0000000000000010");
    assert!(p.step(&dir, &dir, 0, 20_000, &none).is_empty());
    // The server ended it by itself (the member changed map): asked again.
    p.event("WOWBG3 view self reset");
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 20_100, &none)),
        vec![".wowbg3 view Astarion"]
    );
    // Back on the hero: a reset then is the hero's own and nothing is asked.
    std::fs::write(
        dir.join("farsight.json"),
        json!({"Format": 1, "On": false}).to_string(),
    )
    .unwrap();
    assert_eq!(
        lines(p.step(&dir, &dir, 0, 20_200, &none)),
        vec![".wowbg3 view self"]
    );
    p.event("WOWBG3 view self reset");
    assert!(p.step(&dir, &dir, 0, 30_000, &none).is_empty());
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn feed_lines_come_from_the_feed_file() {
    // benilla 0046: the feed's `cr|crgone|tick|near` lines are in `server-feed.txt` (`N LINE`, N shared
    // with server-events.txt); the bridge reads them from there (testing).
    use wow_bg3_link::bridge::server_feed_step;
    let dir = std::env::temp_dir().join(format!("wowbg3-feedfile-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let feed = dir.join("server-feed.txt");
    std::fs::write(
        &feed,
        "3 WOWBG3 cr Astarion F13000012B099003 299 2 30 55 1 32 8 -8321.0 53.0 132.0 1.5 447 0 ma Old Wolf\n\
         7 WOWBG3 cr Astarion F13000012B099001 299 2 30 55 1 32 8 -8320.0 53.0 132.0 1.5 447 0 ma Young Wolf\n\
         9 WOWBG3 tick Astarion 4 1 0 -8330.0 53.0 132.0\n",
    )
    .unwrap();
    let mut m = mirror();
    let mut seq = 5;
    server_feed_step(&mut m, &feed, &mut seq);
    assert_eq!(seq, 9);
    let set = &m.feeds["Astarion"];
    assert!(set.contains_key("f13000012b099001"), "line 7 read");
    assert!(!set.contains_key("f13000012b099003"), "line 3 was history");
    std::fs::write(
        &feed,
        "9 WOWBG3 tick Astarion 4 1 0 -8330.0 53.0 132.0\n12 WOWBG3 crgone Astarion F13000012B099001\n",
    )
    .unwrap();
    server_feed_step(&mut m, &feed, &mut seq);
    assert_eq!(seq, 12);
    assert!(m.feeds["Astarion"].is_empty(), "crgone from the feed file");
    let _ = std::fs::remove_dir_all(&dir);
}
