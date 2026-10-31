use std::os::unix::fs::PermissionsExt;
use std::{
    env, fs,
    io::Read,
    path::{Path, PathBuf},
    process::{Command, ExitCode},
    thread,
    time::{Duration, Instant},
};
use wow_bg3_link::{
    Endpoint, Error, Role, bridge,
    coords::{Basis, Point},
    fake::Fake,
    mailbox::Mailbox,
    wire::{Actor, Frame, Message, Outcome, Rejection, VERSION, decode, encode},
};

fn peer(root: &Path, session: &str, role: Role) -> Result<(), Error> {
    let mailbox = Mailbox::new(root, role)?;
    let mut link = Endpoint::new(session, role, 0, 3000)?;
    let mut game = Fake::new(role, "fixture-1");
    if role == Role::Wow {
        game.begin()?;
    }
    let start = Instant::now();
    let mut done_at = None;
    loop {
        let now = start.elapsed().as_millis() as u64;
        if now > 30_000 {
            return Err("fake fight exceeded 30 seconds".into());
        }
        link.tick(now)
            .inspect_err(|_| game.reconcile_disconnect())?;
        // A non-atomic Script Extender write may be caught midway. Never acknowledge it.
        if let Some(raw) = mailbox.read()?
            && decode(&raw).is_ok()
        {
            link.receive(&raw, now, |m| game.apply(m))?;
        }
        if !link.busy()
            && let Some(m) = game.queue.pop_front()
        {
            link.offer(m)?;
        }
        mailbox.write(&link.frame()?)?;
        if game.phase == "done" && game.queue.is_empty() && !link.busy() {
            let at = *done_at.get_or_insert(now);
            // Keep ack/heartbeats alive briefly so the other side sees final cleanup delivery.
            if now - at > 300 {
                break;
            }
        }
        thread::sleep(Duration::from_millis(20));
    }
    if role == Role::Wow
        && (game.frozen
            || game.credits != 1
            || game.paths != 1
            || game.effects != 1
            || game.rejections != 1)
    {
        return Err("fake final-state assertions failed".into());
    }
    println!(
        "{role:?}: PASS phase={} frozen={} paths={} effects={} rejections={} credits={}",
        game.phase, game.frozen, game.paths, game.effects, game.rejections, game.credits
    );
    Ok(())
}
/// Runs fake WoW against a BG3 peer. `lua_peer` selects the Lua adapter instead of the Rust fake.
fn harness(root: &Path, lua_peer: Option<(&str, &str)>) -> Result<(), Error> {
    fs::create_dir(root).map_err(|e| e.to_string())?;
    fs::set_permissions(root, fs::Permissions::from_mode(0o700)).map_err(|e| e.to_string())?;
    let mut random = [0u8; 16];
    fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut random))
        .map_err(|e| e.to_string())?;
    let session = random
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>();
    let exe = env::current_exe().map_err(|e| e.to_string())?;
    let mut bg3 = match lua_peer {
        Some((script, moddir)) => Command::new("lua")
            .args([script, moddir])
            .arg(root)
            .args([&session, "bg3"])
            .spawn(),
        None => Command::new(&exe)
            .arg("peer")
            .arg(root)
            .args([&session, "bg3"])
            .spawn(),
    }
    .map_err(|e| e.to_string())?;
    let wow = Command::new(&exe)
        .arg("peer")
        .arg(root)
        .args([&session, "wow"])
        .status()
        .map_err(|e| e.to_string())?;
    let bg3 = bg3.wait().map_err(|e| e.to_string())?;
    if !wow.success() || !bg3.success() {
        return Err("fake peer failed".into());
    }
    let side = if lua_peer.is_some() {
        "Lua BG3 adapter"
    } else {
        "Rust fake"
    };
    println!("PASS: two-process file transport ({side}), scripted fight, single credit, cleanup");
    Ok(())
}
fn random_session() -> Result<String, Error> {
    let mut random = [0u8; 16];
    fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut random))
        .map_err(|e| e.to_string())?;
    Ok(random.iter().map(|b| format!("{b:02x}")).collect())
}
/// One step of a live scenario. Actions are built when sent, with the current turn number.
enum Step {
    /// Send a move or cast and wait for Path, Effect or Rejected.
    Act(Box<dyn Fn(u32) -> Message>),
    /// Send EndTurn, then wait until the hero's next Turn (the round rolled over).
    EndTurn,
    /// Send Cleanup and wait for Cleaned.
    Cleanup,
}
/// Scripted WoW peer against the live private BG3 (link test, LinkLive.lua). It records
/// what the engine answered and applies no game rules. Scenarios:
/// `ftb`: host in Force Turn-Based; an out-of-reach move, a short move, Dash twice, Cleanup.
/// `combat`: real fight; Fire Bolt twice at the first mapped enemy (the second must be refused
/// once the action is spent), EndTurn, wait for the hero's next turn, Cleanup.
/// Actors are `id=UUID` pairs mapping protocol ids to BG3 characters (not used by `ftb`).
fn live_wow(dir: &Path, run_id: &str, scenario: &str, actor_args: &[String]) -> Result<(), Error> {
    if !(run_id.len() == 32 && run_id.bytes().all(|b| b.is_ascii_hexdigit())) {
        return Err("run id must be 32 hex characters".into());
    }
    let mut mapping = serde_json::Map::new();
    for a in actor_args {
        let (id, uuid) = a.split_once('=').ok_or("actor must be id=UUID")?;
        if !wow_bg3_link::wire::id_ok(id) || id == "hero" || uuid.len() != 36 {
            return Err(format!("invalid actor mapping {a}"));
        }
        mapping.insert(id.into(), serde_json::Value::String(uuid.into()));
    }
    let enemy = mapping.keys().next().cloned();
    if !dir.exists() {
        fs::create_dir(dir).map_err(|e| e.to_string())?;
    }
    fs::set_permissions(dir, fs::Permissions::from_mode(0o700)).map_err(|e| e.to_string())?;
    let mailbox = Mailbox::new(dir, Role::Wow)?;
    let session = random_session()?;
    let mut link = Endpoint::new(&session, Role::Wow, 0, 15_000)?;
    let e = || "live-1".to_string();
    let mv = |action: &'static str, target: Point| -> Step {
        Step::Act(Box::new(move |turn| Message::Move {
            encounter: "live-1".into(),
            turn,
            action: action.into(),
            actor: "hero".into(),
            target,
        }))
    };
    let cast = |action: &'static str, target: String, spell: &'static str| -> Step {
        Step::Act(Box::new(move |turn| Message::Cast {
            encounter: "live-1".into(),
            turn,
            action: action.into(),
            actor: "hero".into(),
            target: target.clone(),
            spell: spell.into(),
        }))
    };
    let mut steps: std::collections::VecDeque<Step> = match scenario {
        "ftb" => vec![
            mv("move-0", Point(40., 0., 0.)),
            mv("move-1", Point(-3., 0., -1.)),
            cast("cast-1", "hero".into(), "Dash"),
            cast("cast-2", "hero".into(), "Dash"),
            Step::Cleanup,
        ],
        "combat" => {
            let target = enemy
                .clone()
                .ok_or("combat needs at least one enemy actor")?;
            vec![
                cast("cast-1", target.clone(), "FireBolt"),
                cast("cast-2", target, "FireBolt"),
                Step::EndTurn,
                Step::Cleanup,
            ]
        }
        _ => return Err("scenario must be ftb or combat".into()),
    }
    .into();
    let mut actors = vec![Actor {
        id: "hero".into(),
        wow_guid: "0000000000000001".into(),
        position: Point(0., 0., 0.),
        hp: 1,
        max_hp: 1,
    }];
    // Positions and HP are placeholders: this test maps actors to existing BG3 characters.
    for (i, id) in mapping.keys().enumerate() {
        actors.push(Actor {
            id: id.clone(),
            wow_guid: format!("{:016x}", 0xf130_0000_0000_0000u64 + i as u64 + 2),
            position: Point(i as f64 + 1., 0., 0.),
            hp: 1,
            max_hp: 1,
        });
    }
    let mut first = Some(Message::Begin {
        encounter: e(),
        freeze_receipt: "harness-no-server".into(),
        map: 0,
        basis: Basis {
            wow_origin: Point(0., 0., 0.),
            bridge_origin: Point(0., 0., 0.),
            yaw: 0.,
        },
        actors,
    });
    #[derive(PartialEq)]
    enum Wait {
        HeroTurn,
        Answer,
        Cleaned,
        Nothing,
    }
    let mut wait = Wait::HeroTurn;
    let mut turn = 0u32;
    let mut hero_turns = 0usize;
    let mut log = Vec::new();
    let tmp = dir.join("start.tmp");
    fs::write(
        &tmp,
        serde_json::json!({"RunId": run_id, "Session": session, "Actors": mapping}).to_string(),
    )
    .map_err(|e| e.to_string())?;
    fs::rename(&tmp, dir.join("start.json")).map_err(|e| e.to_string())?;
    let start = Instant::now();
    let mut done_at = None;
    loop {
        let now = start.elapsed().as_millis() as u64;
        if now > 300_000 {
            return Err(format!("live test exceeded 5 minutes; log: {log:?}"));
        }
        link.tick(now)?;
        if let Some(raw) = mailbox.read()?
            && let Ok(frame) = decode(&raw)
            && frame.session == session
        {
            link.receive(&raw, now, |m| {
                let seen = serde_json::to_value(m).map_err(|e| e.to_string())?;
                println!("{now:>7} ms <- {seen}");
                log.push(seen);
                match m {
                    Message::Turn { turn: t, actor, .. } => {
                        turn = *t;
                        if actor == "hero" {
                            hero_turns += 1;
                            if wait == Wait::HeroTurn {
                                wait = Wait::Nothing;
                            }
                        }
                    }
                    Message::Path { .. } | Message::Effect { .. } | Message::Rejected { .. }
                        if wait == Wait::Answer =>
                    {
                        wait = Wait::Nothing
                    }
                    Message::Cleaned { .. } if wait == Wait::Cleaned => wait = Wait::Nothing,
                    Message::Abort { reason, .. } => println!("BG3 aborted: {reason}"),
                    _ => {}
                }
                Ok(())
            })?;
        }
        if !link.busy() {
            if let Some(m) = first.take() {
                link.offer(m)?;
            } else if wait == Wait::Nothing {
                match steps.pop_front() {
                    Some(step) => {
                        let (m, next) = match step {
                            Step::Act(build) => (build(turn), Wait::Answer),
                            Step::EndTurn => (
                                Message::EndTurn {
                                    encounter: e(),
                                    turn,
                                },
                                Wait::HeroTurn,
                            ),
                            Step::Cleanup => (Message::Cleanup { encounter: e() }, Wait::Cleaned),
                        };
                        println!(
                            "{now:>7} ms -> {}",
                            serde_json::to_string(&m).map_err(|e| e.to_string())?
                        );
                        link.offer(m)?;
                        wait = next;
                    }
                    None => {
                        let at = *done_at.get_or_insert(now);
                        if now - at > 1500 {
                            break;
                        }
                    }
                }
            }
        }
        mailbox.write(&link.frame()?)?;
        thread::sleep(Duration::from_millis(50));
    }
    let summary = serde_json::json!({"session": session, "scenario": scenario,
        "hero_turns": hero_turns, "received": log});
    fs::write(
        dir.join(format!("live-wow-{session}.json")),
        serde_json::to_string_pretty(&summary).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    println!(
        "PASS: live link session {session} ({scenario}) completed with Cleaned; hero turns {hero_turns}"
    );
    Ok(())
}
/// Valid frames covering every message type, for the Lua cross-test corpus.
fn vectors(dir: &Path) -> Result<(), Error> {
    fs::create_dir(dir).map_err(|e| e.to_string())?;
    let mut f = Fake::new(Role::Wow, "enc-1");
    f.begin()?;
    let begin = f.queue.pop_front().ok_or("no begin")?;
    let e = || "enc-1".to_string();
    let p = |x, y, z| Point(x, y, z);
    let mut big = Vec::new();
    for i in 0..64 {
        big.push(Actor {
            id: format!("actor-{i}"),
            wow_guid: format!("{:016X}", 0xf130_0000_0000_0000u64 + i),
            position: p(i as f64 * 1.25, -0.0, 999_999.999),
            hp: u32::MAX - 1,
            max_hp: u32::MAX,
        });
    }
    let messages = vec![
        (Role::Wow, None),
        (Role::Wow, Some(begin)),
        (
            Role::Wow,
            Some(Message::Begin {
                encounter: "big-encounter_1".into(),
                freeze_receipt: "r".into(),
                map: u32::MAX,
                basis: Basis {
                    wow_origin: p(1e-7, -123456.789, 0.1 + 0.2),
                    bridge_origin: p(-0.0, 5e-324, 1_000_000.0),
                    yaw: -std::f64::consts::PI,
                },
                actors: big,
            }),
        ),
        (Role::Bg3, Some(Message::Ready { encounter: e() })),
        (
            Role::Bg3,
            Some(Message::Turn {
                encounter: e(),
                turn: 1,
                actor: "hero".into(),
            }),
        ),
        (
            Role::Wow,
            Some(Message::Move {
                encounter: e(),
                turn: 1,
                action: "move-1".into(),
                actor: "hero".into(),
                target: p(2., 0., 0.),
            }),
        ),
        (
            Role::Bg3,
            Some(Message::Path {
                encounter: e(),
                turn: 1,
                action: "move-1".into(),
                actor: "hero".into(),
                points: (0..256)
                    .map(|i| p(i as f64 / 3.0, (i as f64).sin(), -(i as f64) * 0.9144))
                    .collect(),
            }),
        ),
        (
            Role::Wow,
            Some(Message::Cast {
                encounter: e(),
                turn: 1,
                action: "cast-1".into(),
                actor: "hero".into(),
                target: "wolf".into(),
                spell: "Fireball".into(),
            }),
        ),
        (
            Role::Bg3,
            Some(Message::Effect {
                encounter: e(),
                turn: u32::MAX,
                action: "cast-1".into(),
                target: "wolf".into(),
                hp: 0,
                max_hp: 12,
                animation: "human-cast-fixture".into(),
            }),
        ),
        (
            Role::Wow,
            Some(Message::EndTurn {
                encounter: e(),
                turn: 1,
            }),
        ),
        (
            Role::Bg3,
            Some(Message::Finished {
                encounter: e(),
                outcome: Outcome::Defeat,
            }),
        ),
        (Role::Wow, Some(Message::Cleanup { encounter: e() })),
        (Role::Bg3, Some(Message::Cleaned { encounter: e() })),
        (
            Role::Bg3,
            Some(Message::Abort {
                encounter: e(),
                reason: "quote \" backslash \\ newline \n tab \t nul \u{0} ünïcode ✓ 𝄞".into(),
            }),
        ),
        (
            Role::Bg3,
            Some(Message::Rejected {
                encounter: e(),
                turn: 1,
                action: "move-0".into(),
                reason: Rejection::NoPath,
                detail: String::new(),
            }),
        ),
        (
            Role::Bg3,
            Some(Message::Rejected {
                encounter: e(),
                turn: u32::MAX,
                action: "cast-0".into(),
                reason: Rejection::NotYourTurn,
                detail: "é".repeat(128),
            }),
        ),
        (
            Role::Bg3,
            Some(Message::Acted {
                encounter: e(),
                turn: 2,
                action: "wolf-2-1".into(),
                actor: "wolf".into(),
                spell: "Target_Bite".into(),
                target: "hero".into(),
                points: vec![p(-1.5, 0.25, 3.0), p(-0.0, 1e-9, 2.0)],
            }),
        ),
        (
            Role::Bg3,
            Some(Message::Acted {
                encounter: e(),
                turn: 3,
                action: "wolf-3-1".into(),
                actor: "wolf".into(),
                spell: "none".into(),
                target: "none".into(),
                points: Vec::new(),
            }),
        ),
        (
            Role::Wow,
            Some(Message::Join {
                encounter: e(),
                actors: vec![Actor {
                    id: "e2".into(),
                    wow_guid: "f130000000012b02".into(),
                    position: p(-3.5, 0.25, 6.0),
                    hp: 42,
                    max_hp: 42,
                }],
            }),
        ),
        (
            Role::Bg3,
            Some(Message::Roster {
                encounter: e(),
                joins: 1,
                actors: vec![
                    Actor {
                        id: "hero".into(),
                        wow_guid: "0000000000000001".into(),
                        position: p(0.0, 0.0, 0.0),
                        hp: 9,
                        max_hp: 14,
                    },
                    Actor {
                        id: "e2".into(),
                        wow_guid: "F130000000012B02".into(),
                        position: p(-1e-9, 1.5, 999_999.5),
                        hp: 0,
                        max_hp: 10,
                    },
                ],
            }),
        ),
    ];
    for (i, (sender, message)) in messages.into_iter().enumerate() {
        let frame = Frame {
            version: VERSION,
            session: "0123456789abcdef0123456789ABCDEF".into(),
            sender,
            heartbeat: (1 << 53) - 1 - i as u64,
            seq: if message.is_some() { 1 + i as u64 } else { 0 },
            ack: (1 << 53) - 1,
            message,
        };
        fs::write(dir.join(format!("valid-{i:02}.json")), encode(&frame)?)
            .map_err(|e| e.to_string())?;
    }
    Ok(())
}
/// One JSON line per input: canonical body if accepted; optional re-encoded copy.
fn decode_files(out: &Path, files: &[String]) -> Result<(), Error> {
    for file in files {
        let bytes = fs::read(file).map_err(|e| e.to_string())?;
        let name = Path::new(file)
            .file_name()
            .and_then(|n| n.to_str())
            .ok_or("bad file name")?;
        let line = match decode(&bytes) {
            Ok(frame) => {
                fs::write(out.join(name), encode(&frame)?).map_err(|e| e.to_string())?;
                serde_json::json!({"file": name, "ok": true,
                    "canonical": serde_json::to_string(&frame).map_err(|e| e.to_string())?})
            }
            Err(e) => serde_json::json!({"file": name, "ok": false, "error": e}),
        };
        println!("{line}");
    }
    Ok(())
}
/// `bridge`: the WoW side of the fights (see `wow_bg3_link::bridge`).
fn bridge_args(args: &[String]) -> Result<(), Error> {
    let project = env::current_dir().map_err(|e| e.to_string())?;
    let mut o = bridge::Options {
        link: Path::new(&args[0]).into(),
        io: Path::new(&args[1]).into(),
        project: project.parent().ok_or("run from link/")?.into(),
        template: String::new(),
        faction: None,
        neutral_faction: "Neutral NPC_cfb709b3-220f-9682-bcfb-6f0d8837462e".into(),
        mirrors: true,
        centre: Point(-136., 10., -444.),
        size: 40.,
        follow: true,
        script: false,
        pull_at: None,
        script_enemies: 1,
        script_adds: 0,
        stats: PathBuf::new(),
        max_seconds: 14400, // hand-played fights of four run long; only a stuck bridge should reach this
    };
    o.stats = o.project.join("tmp/creature-stats.json");
    let num = |s: Option<&String>| -> Result<f64, Error> {
        s.ok_or("missing number")?
            .parse::<f64>()
            .map_err(|e| e.to_string())
    };
    let mut i = 2;
    while i < args.len() {
        let a = &args[i];
        match a.as_str() {
            "--template" => {
                o.template = args.get(i + 1).ok_or("--template needs a value")?.clone();
                i += 2
            }
            "--faction" => {
                o.faction = Some(args.get(i + 1).ok_or("--faction needs a value")?.clone());
                i += 2
            }
            "--neutral-faction" => {
                o.neutral_faction = args
                    .get(i + 1)
                    .ok_or("--neutral-faction needs a value")?
                    .clone();
                i += 2
            }
            "--no-mirrors" => {
                o.mirrors = false;
                i += 1
            }
            "--centre" => {
                o.centre = Point(
                    num(args.get(i + 1))?,
                    num(args.get(i + 2))?,
                    num(args.get(i + 3))?,
                );
                i += 4
            }
            "--size" => {
                o.size = num(args.get(i + 1))?;
                i += 2
            }
            "--no-follow" => {
                o.follow = false;
                i += 1
            }
            "--script" => {
                o.script = true;
                o.pull_at = Some((
                    num(args.get(i + 1))?,
                    num(args.get(i + 2))?,
                    num(args.get(i + 3))?,
                ));
                i += 4
            }
            "--script-enemies" => {
                o.script_enemies = num(args.get(i + 1))?.clamp(1., 8.) as u32;
                i += 2
            }
            "--script-adds" => {
                o.script_adds = num(args.get(i + 1))?.clamp(0., 7.) as u32;
                i += 2
            }
            "--stats" => {
                o.stats = args.get(i + 1).ok_or("--stats needs a path")?.into();
                i += 2
            }
            "--max-seconds" => {
                o.max_seconds = num(args.get(i + 1))? as u64;
                i += 2
            }
            _ => return Err(format!("unknown bridge option {a}")),
        }
    }
    if o.template.is_empty() || o.template.len() > 128 {
        return Err("--template ROOT_TEMPLATE is required".into());
    }
    if !(2.0..=120.0).contains(&o.size) || !o.centre.valid() {
        return Err("invalid --size or --centre".into());
    }
    bridge::run(&o)
}
fn run() -> Result<(), Error> {
    let args: Vec<_> = env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("peer") if args.len() == 5 => {
            let role = match args[4].as_str() {
                "wow" => Role::Wow,
                "bg3" => Role::Bg3,
                _ => return Err("invalid role".into()),
            };
            peer(Path::new(&args[2]), &args[3], role)
        }
        Some("harness") if args.len() == 3 => harness(Path::new(&args[2]), None),
        Some("harness-lua") if args.len() == 5 => {
            harness(Path::new(&args[2]), Some((&args[3], &args[4])))
        }
        Some("vectors") if args.len() == 3 => vectors(Path::new(&args[2])),
        Some("live-wow") if args.len() >= 5 => {
            live_wow(Path::new(&args[2]), &args[3], &args[4], &args[5..])
        }
        Some("decode") if args.len() >= 4 => decode_files(Path::new(&args[2]), &args[3..]),
        Some("bridge") if args.len() >= 4 => bridge_args(&args[2..]),
        _ => Err("usage: wow-bg3-link harness NEW_PRIVATE_DIRECTORY | harness-lua NEW_PRIVATE_DIRECTORY LUA_PEER MOD_LUA_DIR | live-wow LINK_DIRECTORY RUN_ID ftb|combat [ID=UUID...] | vectors NEW_DIRECTORY | decode OUT_DIRECTORY FRAME... | bridge LINK_DIRECTORY IO_DIRECTORY --template ROOT_TEMPLATE [--faction ID] [--neutral-faction ID] [--no-mirrors] [--centre X Y Z] [--size M] [--no-follow] [--script X Y Z] [--script-enemies N] [--script-adds N] [--stats CREATURE_STATS_JSON] [--max-seconds N]".into()),
    }
}
fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("FAIL: {e}");
            ExitCode::FAILURE
        }
    }
}
