//! Turn-based mode outside combat, the WoW side (was
//! `link/tools/turnhold.py`, which sent the verbs through the mangosd console: 3.5 to 4.2 s from
//! BG3's switch to the server's hold, so creatures finished their walk).
//!
//! `TurnHold.lua` (BG3 side) writes `link/turnhold.json`: `{Schema 1, RunId, Seq, Tick, Hold, Radius
//! (WoW yards), Members: [{Name, Wow: [x, y, z]}]}` while any party member is in BG3's turn-based mode
//! outside combat. The fight bridge turns it into the server's hold-area verbs (patch 0010,
//! m5-server-handoff.md "Hold area") and sends them through the hero's own client like every
//! other `.wowbg3` verb (benilla's server channel, about 0.1 s to the server):
//! - Hold true: `hold HID R X,Y,Z ...` on every new Seq (members walked, joined, left) and again every
//!   [`REPEAT_MS`] (the same HID adds creatures that came near since);
//! - Hold false, a new BG3 session (RunId), a file older than [`STALE_MS`] or a heartbeat (Tick)
//!   unchanged that long while held (BG3 stopped): `unhold HID` once; creatures go on where they
//!   stood. The first step of a bridge run sends `unhold all` (holds of an earlier run).
use crate::handoff::Handoff;
use serde_json::Value;
use std::{fs, path::Path, time::SystemTime};

/// Milliseconds between repeats of a running hold (newcomers near the party are held too).
pub const REPEAT_MS: u64 = 4_000;
/// Milliseconds without a heartbeat (or file write) after which the hold lets go.
pub const STALE_MS: u64 = 15_000;
/// Milliseconds between reads of turnhold.json.
pub const READ_MS: u64 = 100;
const MAX_SPOTS: usize = 8;
const MAX_RADIUS: i64 = 200;

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
}

fn party(line: String) -> Handoff {
    Handoff::Party { line }
}

/// The members' WoW points as `X,Y,Z` (at most 8, the server's limit).
pub fn spots(d: &Value) -> Vec<String> {
    let mut out = Vec::new();
    for m in d
        .get("Members")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        let Some(w) = m.get("Wow").and_then(Value::as_array) else {
            continue;
        };
        let v: Vec<f64> = w.iter().filter_map(Value::as_f64).collect();
        if v.len() == 3 && w.len() == 3 && v.iter().all(|x| x.is_finite() && x.abs() < 100_000.0) {
            out.push(format!("{:.1},{:.1},{:.1}", v[0], v[1], v[2]));
        }
        if out.len() == MAX_SPOTS {
            break;
        }
    }
    out
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
        let points = if want { spots(&d) } else { Vec::new() };
        let want = want && !points.is_empty();
        let new_run = self.run.is_some() && run != self.run;
        let stale = self.held.is_some() && now.saturating_sub(self.tick_at) > STALE_MS;
        if let Some(h) = self.held.clone()
            && (!want || new_run || stale)
        {
            out.push(party(format!("unhold {h}")));
            self.held = None;
        }
        if want
            && !stale
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
            let r = d
                .get("Radius")
                .and_then(Value::as_i64)
                .unwrap_or(50)
                .clamp(1, MAX_RADIUS);
            out.push(party(format!("hold {h} {r} {}", points.join(" "))));
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
    fn holds_at_once_repeats_and_lets_go() {
        let dir = std::env::temp_dir().join(format!("turnhold-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let mut t = TurnHold::default();
        assert_eq!(lines(t.step(&dir, 0)), vec![".wowbg3 unhold all"]);
        let on = |seq: i64, tick: i64| {
            json!({"Schema": 1, "RunId": "5ba0566e-2e4a", "Seq": seq, "Tick": tick, "Hold": true, "Radius": 50,
                   "Members": [{"Name": "Gale", "Wow": [-8987.4, -141.6, 83.1]}]})
        };
        write(&dir, on(1, 6));
        assert_eq!(
            lines(t.step(&dir, 200)),
            vec![".wowbg3 hold tb-5ba0566e-1 50 -8987.4,-141.6,83.1"]
        );
        assert!(lines(t.step(&dir, 400)).is_empty());
        assert_eq!(lines(t.step(&dir, 200 + REPEAT_MS)).len(), 1);
        write(&dir, on(2, 12));
        assert_eq!(lines(t.step(&dir, 200 + REPEAT_MS + 200)).len(), 1);
        write(
            &dir,
            json!({"Schema": 1, "RunId": "5ba0566e-2e4a", "Seq": 3, "Tick": 18, "Hold": false, "Members": []}),
        );
        assert_eq!(
            lines(t.step(&dir, 20_000)),
            vec![".wowbg3 unhold tb-5ba0566e-1"]
        );
        assert!(lines(t.step(&dir, 20_200)).is_empty());
        // A heartbeat that stops while held lets go.
        write(&dir, on(4, 24));
        assert_eq!(
            lines(t.step(&dir, 21_000)),
            vec![".wowbg3 hold tb-5ba0566e-2 50 -8987.4,-141.6,83.1"]
        );
        let late = 21_000 + STALE_MS + 1;
        assert_eq!(
            lines(t.step(&dir, late)),
            vec![".wowbg3 unhold tb-5ba0566e-2"]
        );
        let _ = fs::remove_dir_all(&dir);
    }
}
