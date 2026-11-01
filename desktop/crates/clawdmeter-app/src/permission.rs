//! clawdmeter2: answer Claude Code's permission prompts from the device.
//!
//! `clawdmeter hook permission` runs as Claude Code's `PermissionRequest`
//! hook. When the daemon has the device connected it writes a request file
//! `perm/req-<id>.json` and waits up to [`WAIT_SECS`] for `perm/ans-<id>`
//! ("allow" / "deny"), which the daemon writes when you tap the device. The
//! answer goes to stdout as the hook's decision; no answer prints nothing,
//! so Claude Code asks in the terminal as usual.
//!
//! The daemon ([`Bridge`]) shows each new request on the device
//! (`{"pq":1,"id":..,"tool":..,"t":..}`) and withdraws it (`{"pq":0,"id":..}`)
//! once its file is gone or the session moved on (answered in the terminal).

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

/// How long the hook waits for a tap on the device.
pub const WAIT_SECS: u64 = 45;
/// Longest request text sent to the device, in bytes.
pub const TEXT_MAX: usize = 120;
/// A request file older than this is stale (its hook died).
const STALE_SECS: i64 = WAIT_SECS as i64 + 15;

/// The folder holding the request / answer files.
pub fn dir(state_dir: &Path) -> PathBuf {
    state_dir.join("perm")
}

fn req_file(state_dir: &Path, id: u32) -> PathBuf {
    dir(state_dir).join(format!("req-{id}.json"))
}

fn ans_file(state_dir: &Path, id: u32) -> PathBuf {
    dir(state_dir).join(format!("ans-{id}"))
}

/// Cut `s` to at most `max` bytes on a char boundary, one line.
fn clip(s: &str, max: usize) -> String {
    let one: String = s.chars().map(|c| if c.is_control() { ' ' } else { c }).collect();
    let one = one.trim();
    if one.len() <= max {
        return one.to_owned();
    }
    let mut end = max.saturating_sub(3);
    while !one.is_char_boundary(end) {
        end -= 1;
    }
    format!("{}...", &one[..end])
}

/// Tools whose whole request is one shell command: the only ones the device
/// may answer. Everything else (edits, plans, questions, MCP calls) has more
/// to it than the card can show, so the terminal decides.
const DEVICE_TOOLS: [&str; 2] = ["Bash", "PowerShell"];

/// What the device shows for a tool call, whole: `None` when the device
/// must not be asked (not a shell command, too long for the card, or text the
/// device font can't show faithfully, e.g. lookalike / invisible characters).
/// A cut or garbled command could hide what it really does.
pub fn full_summary(tool: &str, input: &Value) -> Option<String> {
    if !DEVICE_TOOLS.contains(&tool) {
        return None;
    }
    let text = input.get("command").and_then(Value::as_str)?.trim();
    // Checked raw, nothing tidied first: a line break starts a second
    // command, and shown as a space it would read as an argument.
    let plain = text.bytes().all(|b| (0x20..0x7f).contains(&b));
    (!text.is_empty() && plain && text.len() <= TEXT_MAX).then(|| text.to_owned())
}

/// The hook's stdout for an answer.
pub fn decision_json(allow: bool) -> String {
    let decision = if allow {
        json!({"behavior": "allow"})
    } else {
        json!({"behavior": "deny", "message": "Denied on the Clawdmeter2 device."})
    };
    json!({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": decision}}).to_string()
}

fn new_id() -> u32 {
    let nanos =
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0);
    let mix = nanos ^ (u64::from(std::process::id()) << 20);
    ((mix ^ (mix >> 31)) as u32 & 0x7fff_ffff).max(1)
}

/// The device is connected right now (per the daemon's status file).
fn device_connected(state_dir: &Path, now: i64) -> bool {
    let Ok(text) = fs::read_to_string(state_dir.join("app-status.json")) else { return false };
    let Ok(v) = serde_json::from_str::<Value>(&text) else { return false };
    let fresh = v.get("updated_at").and_then(Value::as_i64).is_some_and(|t| now - t < 15);
    fresh && v.get("link").and_then(Value::as_str) == Some("connected")
}

/// The hook: returns the stdout to print ("" = no decision).
pub fn run_hook(input: &str, state_dir: &Path, now: i64) -> String {
    let Ok(v) = serde_json::from_str::<Value>(input) else { return String::new() };
    if !device_connected(state_dir, now) {
        return String::new();
    }
    let tool = v.get("tool_name").and_then(Value::as_str).unwrap_or("");
    let input = v.get("tool_input").cloned().unwrap_or(Value::Null);
    let session = v.get("session_id").and_then(Value::as_str).unwrap_or("");
    // Only what the card can show in full goes to the device.
    let Some(text) = full_summary(tool, &input) else { return String::new() };
    let id = new_id();
    let req = json!({"id": id, "tool": clip(tool, 31), "t": text, "ts": now, "s": session});
    let _ = fs::remove_file(ans_file(state_dir, id)); // never an old answer
    if fs::create_dir_all(dir(state_dir)).is_err() || fs::write(req_file(state_dir, id), req.to_string()).is_err() {
        return String::new();
    }
    let started = Instant::now();
    let mut last_link_check = Instant::now();
    let mut out = String::new();
    while started.elapsed() < Duration::from_secs(WAIT_SECS) {
        // Only an exact answer counts (the daemon writes it in one rename).
        match fs::read_to_string(ans_file(state_dir, id)).as_deref().map(str::trim) {
            Ok("allow") => {
                out = decision_json(true);
                break;
            }
            Ok("deny") => {
                out = decision_json(false);
                break;
            }
            _ => {}
        }
        // The daemon withdrew it (answered in the terminal, the session moved on).
        if !req_file(state_dir, id).exists() {
            break;
        }
        // The device went away: hand back to the terminal now, not after 45 s.
        if last_link_check.elapsed() >= Duration::from_secs(2) {
            last_link_check = Instant::now();
            if !device_connected(state_dir, now + started.elapsed().as_secs() as i64) {
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(150));
    }
    let _ = fs::remove_file(req_file(state_dir, id));
    let _ = fs::remove_file(ans_file(state_dir, id));
    out
}

/// One pending request, as the daemon sees it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Request {
    /// Its id.
    pub id: u32,
    /// Unix seconds it was asked.
    pub ts: i64,
    /// Session id.
    pub session: String,
    /// The device message that shows it.
    pub show: String,
}

/// Read every request file (dropping stale ones).
pub fn pending(state_dir: &Path, now: i64) -> Vec<Request> {
    let Ok(rd) = fs::read_dir(dir(state_dir)) else { return Vec::new() };
    let mut out = Vec::new();
    for e in rd.flatten() {
        let p = e.path();
        let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if !(name.starts_with("req-") && name.ends_with(".json")) {
            continue;
        }
        let Some(v) = fs::read_to_string(&p).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()) else {
            continue;
        };
        let (Some(id), Some(ts)) = (v.get("id").and_then(Value::as_u64), v.get("ts").and_then(Value::as_i64)) else {
            continue;
        };
        if now - ts > STALE_SECS {
            let _ = fs::remove_file(&p);
            continue;
        }
        let s = |k: &str| v.get(k).and_then(Value::as_str).unwrap_or("").to_owned();
        let show = json!({"pq": 1, "id": id, "tool": s("tool"), "t": s("t")}).to_string();
        let Ok(id) = u32::try_from(id) else { continue };
        out.push(Request { id, ts, session: s("s"), show });
    }
    out.sort_by_key(|r| r.ts);
    out
}

/// Delete answer files whose request is gone (the hook was killed, or the
/// answer came after it gave up).
fn sweep_answers(state_dir: &Path, pending: &[Request]) {
    let Ok(rd) = fs::read_dir(dir(state_dir)) else { return };
    for e in rd.flatten() {
        let p = e.path();
        let Some(name) = p.file_name().and_then(|n| n.to_str()) else { continue };
        let Some(rest) = name.strip_prefix("ans-") else { continue };
        let id = rest.trim_end_matches(".tmp").parse::<u32>().ok();
        if id.is_none_or(|id| !pending.iter().any(|r| r.id == id)) {
            let _ = fs::remove_file(&p);
        }
    }
}

/// The daemon's side: which requests the device shows.
#[derive(Debug, Default)]
pub struct Bridge {
    shown: HashMap<u32, Request>,
}

impl Bridge {
    /// Compare the request files with what the device shows; returns the
    /// messages to send (shows for new ones, withdrawals for gone ones).
    pub fn sync(&mut self, state_dir: &Path, now: i64) -> Vec<String> {
        let now_pending = pending(state_dir, now);
        sweep_answers(state_dir, &now_pending);
        let mut out = Vec::new();
        let gone: Vec<u32> = self.shown.keys().copied().filter(|id| !now_pending.iter().any(|r| r.id == *id)).collect();
        for id in gone {
            self.shown.remove(&id);
            out.push(json!({"pq": 0, "id": id}).to_string());
        }
        for r in now_pending {
            if let std::collections::hash_map::Entry::Vacant(e) = self.shown.entry(r.id) {
                out.push(r.show.clone());
                e.insert(r);
            }
        }
        out
    }

    /// The device answered `id` (only one it was shown). Written to a temp
    /// file and renamed, so the waiting hook never reads half an answer.
    pub fn answer(&mut self, state_dir: &Path, id: u32, allow: bool) -> bool {
        if !self.shown.contains_key(&id) || !req_file(state_dir, id).exists() {
            return false;
        }
        let tmp = dir(state_dir).join(format!("ans-{id}.tmp"));
        fs::write(&tmp, if allow { "allow" } else { "deny" }).is_ok()
            && fs::rename(&tmp, ans_file(state_dir, id)).is_ok()
    }

    /// A hook event from `session` at `ts`: a request of that session asked
    /// before it was answered in the terminal (or the turn moved on), so
    /// withdraw it; the next [`Bridge::sync`] tells the device.
    pub fn session_moved_on(&mut self, state_dir: &Path, session: &str, ts: i64) {
        if session.is_empty() {
            return;
        }
        for r in pending(state_dir, ts) {
            if r.session == session && ts > r.ts {
                let _ = fs::remove_file(req_file(state_dir, r.id));
            }
        }
    }

    /// The device can't show `id` whole: hand it back to the terminal.
    pub fn withdraw(&mut self, state_dir: &Path, id: u32) {
        if self.shown.contains_key(&id) {
            let _ = fs::remove_file(req_file(state_dir, id));
        }
    }

    /// The connected device may not answer: hand every waiting request back
    /// to the terminal at once (the hooks see their file gone).
    pub fn decline_all(&mut self, state_dir: &Path, now: i64) {
        for r in pending(state_dir, now) {
            let _ = fs::remove_file(req_file(state_dir, r.id));
        }
    }

    /// The link dropped: the device shows nothing any more.
    pub fn forget(&mut self) {
        self.shown.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("cm-perm-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn only_plain_shell_commands_reach_the_device() {
        assert_eq!(full_summary("Bash", &json!({"command": " git push origin "})).as_deref(), Some("git push origin"));
        assert_eq!(full_summary("Bash", &json!({"command": "echo hi\nrm -rf x"})), None, "two commands");
        assert_eq!(full_summary("Bash", &json!({"command": "ls\t-la"})), None);
        assert_eq!(full_summary("PowerShell", &json!({"command": "ls"})).as_deref(), Some("ls"));
        assert_eq!(full_summary("Write", &json!({"file_path": "C:/a.txt", "content": "x"})), None);
        assert_eq!(full_summary("ExitPlanMode", &json!({"plan": "x"})), None);
        assert_eq!(full_summary("AskUserQuestion", &json!({"questions": []})), None);
        assert_eq!(full_summary("Bash", &json!({"command": "rm\u{200b} -rf x"})), None, "invisible char");
        assert_eq!(full_summary("Bash", &json!({"command": "é"})), None);
        assert_eq!(full_summary("Bash", &json!({})), None);
    }

    #[test]
    fn half_written_answer_is_not_a_deny() {
        let d = tmp("half");
        fs::write(d.join("app-status.json"), r#"{"updated_at":100,"link":"connected"}"#).unwrap();
        let hook_dir = d.clone();
        let h = std::thread::spawn(move || {
            run_hook(r#"{"tool_name":"Bash","tool_input":{"command":"ls"},"session_id":"s1"}"#, &hook_dir, 100)
        });
        let mut b = Bridge::default();
        let mut id = 0;
        for _ in 0..50 {
            if let Some(m) = b.sync(&d, 100).first() {
                id = serde_json::from_str::<Value>(m).unwrap()["id"].as_u64().unwrap() as u32;
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        fs::write(ans_file(&d, id), "").unwrap(); // an empty (half-written) answer
        std::thread::sleep(Duration::from_millis(400));
        assert!(b.answer(&d, id, true));
        assert!(h.join().unwrap().contains("\"allow\""));
    }

    #[test]
    fn device_gone_hands_back_quickly() {
        let d = tmp("gone");
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs() as i64;
        fs::write(d.join("app-status.json"), format!(r#"{{"updated_at":{now},"link":"connected"}}"#)).unwrap();
        let hook_dir = d.clone();
        let t = Instant::now();
        let h = std::thread::spawn(move || {
            run_hook(r#"{"tool_name":"Bash","tool_input":{"command":"ls"},"session_id":"s1"}"#, &hook_dir, now)
        });
        std::thread::sleep(Duration::from_millis(300));
        fs::write(d.join("app-status.json"), format!(r#"{{"updated_at":{now},"link":"scanning"}}"#)).unwrap();
        assert_eq!(h.join().unwrap(), "");
        assert!(t.elapsed() < Duration::from_secs(5), "{:?}", t.elapsed());
    }

    #[test]
    fn long_commands_stay_in_the_terminal() {
        let long = format!("ls {}; rm -rf /", "a ".repeat(70));
        assert_eq!(full_summary("Bash", &json!({"command": long})), None);
        let d = tmp("long");
        fs::write(d.join("app-status.json"), r#"{"updated_at":100,"link":"connected"}"#).unwrap();
        let t = Instant::now();
        let input = json!({"tool_name": "Bash", "tool_input": {"command": long}}).to_string();
        assert_eq!(run_hook(&input, &d, 100), "");
        assert!(t.elapsed() < Duration::from_secs(1), "no wait: the device isn't asked");
    }

    #[test]
    fn decisions() {
        let a: Value = serde_json::from_str(&decision_json(true)).unwrap();
        assert_eq!(a["hookSpecificOutput"]["hookEventName"], "PermissionRequest");
        assert_eq!(a["hookSpecificOutput"]["decision"]["behavior"], "allow");
        let d: Value = serde_json::from_str(&decision_json(false)).unwrap();
        assert_eq!(d["hookSpecificOutput"]["decision"]["behavior"], "deny");
    }

    #[test]
    fn no_device_no_wait() {
        let d = tmp("nodev");
        let t = Instant::now();
        assert_eq!(run_hook(r#"{"tool_name":"Bash","tool_input":{"command":"ls"}}"#, &d, 100), "");
        assert!(t.elapsed() < Duration::from_secs(1));
    }

    #[test]
    fn bridge_round_trip() {
        let d = tmp("bridge");
        fs::write(d.join("app-status.json"), r#"{"updated_at":100,"link":"connected"}"#).unwrap();
        let hook_dir = d.clone();
        let h = std::thread::spawn(move || {
            run_hook(r#"{"tool_name":"Bash","tool_input":{"command":"ls"},"session_id":"s1"}"#, &hook_dir, 100)
        });
        let mut b = Bridge::default();
        let mut shown = Vec::new();
        for _ in 0..50 {
            shown = b.sync(&d, 100);
            if !shown.is_empty() {
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        assert_eq!(shown.len(), 1);
        let v: Value = serde_json::from_str(&shown[0]).unwrap();
        assert_eq!(v["pq"], 1);
        assert_eq!(v["t"], "ls");
        let id = v["id"].as_u64().unwrap() as u32;
        assert!(b.answer(&d, id, true));
        let out = h.join().unwrap();
        assert!(out.contains("\"allow\""), "{out}");
        // The hook cleaned up: the device gets the withdrawal.
        assert_eq!(b.sync(&d, 101), vec![json!({"pq": 0, "id": id}).to_string()]);
    }

    #[test]
    fn terminal_answer_withdraws() {
        let d = tmp("moved");
        fs::write(d.join("app-status.json"), r#"{"updated_at":100,"link":"connected"}"#).unwrap();
        let hook_dir = d.clone();
        let h = std::thread::spawn(move || {
            run_hook(r#"{"tool_name":"Bash","tool_input":{"command":"ls"},"session_id":"s1"}"#, &hook_dir, 100)
        });
        let mut b = Bridge::default();
        for _ in 0..50 {
            if !b.sync(&d, 100).is_empty() {
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        b.session_moved_on(&d, "other", 105);
        assert_eq!(pending(&d, 105).len(), 1, "another session doesn't count");
        b.session_moved_on(&d, "s1", 105);
        assert_eq!(h.join().unwrap(), "", "no decision: the terminal had it");
    }
}
