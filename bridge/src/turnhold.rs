//! Turn-based mode outside combat, the WoW side (was
//! `link/tools/turnhold.py`, which sent the verbs through the mangosd console: 3.5 to 4.2 s from
//! BG3's switch to the server's hold, so creatures finished their walk).
//!
//! `TurnHold.lua` (BG3 side) writes `link/turnhold.json`: `{Schema 1, RunId, Seq, Tick, Hold, Guids:
//! [WoW guid, ...], Members: [{Name, Wow: [x, y, z]}]}` while any party member is in BG3's turn-based
//! mode outside combat; Guids are the creatures to stop: those WoW has loaded and those whose
//! stand-ins BG3 froze (`TurnHold.lua` says why). The fight bridge turns it into the server's hold verbs (patch 0010's
//! hold, by guid since patch 0026) and sends them through the hero's own client like every
//! other `.wowbg3` verb (benilla's server channel, about 0.1 s to the server):
//! - Hold true: `hold HID guids G1,G2,...` ([`GUIDS_PER_LINE`] a line, a chat line's 255 characters)
//!   for every guid on every new Seq and again every [`REPEAT_MS`] (a held one stays held; the repeat
//!   renews the hold), and `hold HID drop G1,...` for those sent before that BG3 no longer froze;
//! - Hold false, a new BG3 session (RunId), a file older than [`STALE_MS`] or a heartbeat (Tick)
//!   unchanged that long while held (BG3 stopped): `unhold HID` once; creatures go on where they
//!   stood. The first step of a bridge run sends `unhold all` (holds of an earlier run).
use crate::handoff::{GUIDS_PER_LINE, Handoff};
use serde_json::Value;
use std::{collections::BTreeSet, fs, path::Path, time::SystemTime};

/// Milliseconds between repeats of a running hold (newcomers near the party are held too).
pub const REPEAT_MS: u64 = 4_000;
/// Milliseconds without a heartbeat (or file write) after which the hold lets go.
pub const STALE_MS: u64 = 15_000;
/// Milliseconds between reads of turnhold.json.
pub const READ_MS: u64 = 100;

#[derive(Debug, Default)]
pub struct TurnHold {
    started: bool,
    /// The hold id while the server holds.
    held: Option<String>,
    run: Option<String>,
    seq: Option<i64>,
    tick: Option<i64>,
    tick_at: u64,
    sent_at: u64,
    read_at: Option<u64>,
    n: u32,
    /// The guids held under `held` (sent in `hold HID guids` lines, not dropped since).
    guids: BTreeSet<String>,
}

fn party(line: String) -> Handoff {
    Handoff::Party { line }
}

/// The creatures to stop: `Guids`, WoW creature guids as 16 hex digits (lower case).
pub fn frozen(d: &Value) -> BTreeSet<String> {
    d.get("Guids")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .filter(|g| g.len() == 16 && g.chars().all(|c| c.is_ascii_hexdigit()))
        .map(str::to_ascii_lowercase)
        .collect()
}

/// `hold HID VERB G1,G2,...` lines, [`GUIDS_PER_LINE`] guids each.
fn guid_lines<'a>(h: &str, verb: &str, guids: impl Iterator<Item = &'a String>) -> Vec<Handoff> {
    let all: Vec<&String> = guids.collect();
    all.chunks(GUIDS_PER_LINE)
        .map(|c| {
            let list: Vec<&str> = c.iter().map(|g| g.as_str()).collect();
            party(format!("hold {h} {verb} {}", list.join(",")))
        })
        .collect()
}

impl TurnHold {
    /// Reads turnhold.json (at most every [`READ_MS`]) and returns the verbs to send. `now` is the
    /// bridge clock in ms.
    pub fn step(&mut self, link: &Path, now: u64) -> Vec<Handoff> {
        let mut out = Vec::new();
        if !self.started {
            self.started = true;
            out.push(party("unhold all".into()));
        }
        if self.read_at.is_some_and(|t| now < t + READ_MS) {
            return out;
        }
        self.read_at = Some(now);
        let path = link.join("turnhold.json");
        let age_ms = fs::metadata(&path)
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| SystemTime::now().duration_since(t).ok())
            .map_or(u64::MAX, |d| d.as_millis() as u64);
        let d = fs::read_to_string(&path)
            .ok()
            .and_then(|t| serde_json::from_str::<Value>(&t).ok())
            .filter(|v| v.get("Schema").and_then(Value::as_i64) == Some(1));
        let Some(d) = d else {
            return out;
        };
        let tick = d.get("Tick").and_then(Value::as_i64);
        if tick != self.tick {
            self.tick = tick;
            self.tick_at = now;
        }
        let run = d.get("RunId").and_then(Value::as_str).map(str::to_string);
        let seq = d.get("Seq").and_then(Value::as_i64);
        let want = d.get("Hold") == Some(&Value::Bool(true)) && age_ms < STALE_MS;
        let guids = if want { frozen(&d) } else { BTreeSet::new() };
        let new_run = self.run.is_some() && run != self.run;
        let stale = self.held.is_some() && now.saturating_sub(self.tick_at) > STALE_MS;
        if let Some(h) = self.held.clone()
            && (!want || new_run || stale)
        {
            out.push(party(format!("unhold {h}")));
            self.held = None;
            self.guids.clear();
        }
        // BG3 let these go: the server too (only what this hold holds).
        if let Some(h) = self.held.clone()
            && want
            && !stale
        {
            let gone: Vec<String> = self.guids.difference(&guids).cloned().collect();
            out.extend(guid_lines(&h, "drop", gone.iter()));
            for g in &gone {
                self.guids.remove(g);
            }
        }
        if want
            && !stale
            && !guids.is_empty()
            && (self.held.is_none() || seq != self.seq || now >= self.sent_at + REPEAT_MS)
        {
            let h = match &self.held {
                Some(h) => h.clone(),
                None => {
                    self.n += 1;
                    let id: String = run
                        .as_deref()
                        .unwrap_or("0")
                        .chars()
                        .filter(char::is_ascii_alphanumeric)
                        .take(8)
                        .collect();
                    let h = format!("tb-{id}-{}", self.n);
                    self.held = Some(h.clone());
                    h
                }
            };
            out.extend(guid_lines(&h, "guids", guids.iter()));
            self.guids = guids;
            self.sent_at = now;
        }
        self.run = run;
        self.seq = seq;
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn write(dir: &Path, v: Value) {
        fs::write(dir.join("turnhold.json"), v.to_string()).unwrap();
    }
    fn lines(v: Vec<Handoff>) -> Vec<String> {
        v.iter().filter_map(Handoff::command).collect()
    }

    #[test]
    fn holds_what_bg3_froze_repeats_drops_and_lets_go() {
        let dir = std::env::temp_dir().join(format!("turnhold-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let mut t = TurnHold::default();
        assert_eq!(lines(t.step(&dir, 0)), vec![".wowbg3 unhold all"]);
        let on = |seq: i64, tick: i64, guids: Value| {
            json!({"Schema": 1, "RunId": "5ba0566e-2e4a", "Seq": seq, "Tick": tick, "Hold": true, "Guids": guids,
                   "Members": [{"Name": "Gale", "Wow": [-8987.4, -141.6, 83.1]}]})
        };
        // Nothing frozen yet: nothing to hold.
        write(&dir, on(1, 6, json!([])));
        assert!(lines(t.step(&dir, 200)).is_empty());
        write(
            &dir,
            on(2, 6, json!(["F130000045013888", "f13000012b01389b", "bad"])),
        );
        assert_eq!(
            lines(t.step(&dir, 400)),
            vec![".wowbg3 hold tb-5ba0566e-1 guids f130000045013888,f13000012b01389b"]
        );
        assert!(lines(t.step(&dir, 600)).is_empty());
        assert_eq!(lines(t.step(&dir, 400 + REPEAT_MS)).len(), 1);
        // BG3 let one go: dropped, the other held again.
        write(&dir, on(3, 12, json!(["f130000045013888"])));
        assert_eq!(
            lines(t.step(&dir, 400 + REPEAT_MS + 200)),
            vec![
                ".wowbg3 hold tb-5ba0566e-1 drop f13000012b01389b",
                ".wowbg3 hold tb-5ba0566e-1 guids f130000045013888"
            ]
        );
        // Many guids go out GUIDS_PER_LINE a line, each line within a chat line.
        let many: Vec<String> = (0..30).map(|i| format!("f1300000450138{i:02x}")).collect();
        write(&dir, on(4, 18, json!(many)));
        let l = lines(t.step(&dir, 10_000));
        assert_eq!(l[0], ".wowbg3 hold tb-5ba0566e-1 drop f130000045013888");
        assert_eq!(l.len() - 1, 30usize.div_ceil(GUIDS_PER_LINE));
        assert!(l.iter().all(|x| x.len() <= 255));
        write(
            &dir,
            json!({"Schema": 1, "RunId": "5ba0566e-2e4a", "Seq": 5, "Tick": 18, "Hold": false, "Guids": [], "Members": []}),
        );
        assert_eq!(
            lines(t.step(&dir, 20_000)),
            vec![".wowbg3 unhold tb-5ba0566e-1"]
        );
        assert!(lines(t.step(&dir, 20_200)).is_empty());
        // A heartbeat that stops while held lets go.
        write(&dir, on(6, 24, json!(["f130000045013888"])));
        assert_eq!(
            lines(t.step(&dir, 21_000)),
            vec![".wowbg3 hold tb-5ba0566e-2 guids f130000045013888"]
        );
        let late = 21_000 + STALE_MS + 1;
        assert_eq!(
            lines(t.step(&dir, late)),
            vec![".wowbg3 unhold tb-5ba0566e-2"]
        );
        let _ = fs::remove_dir_all(&dir);
    }
}
