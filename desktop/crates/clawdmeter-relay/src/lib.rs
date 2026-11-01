//! The relay: Claude Code running on a machine without the Clawdmeter app
//! (a Proxmox server, a VM) reports to the app on another machine.
//!
//! On the server, `clawdmeter-relay hook …` runs as the Claude Code hook in
//! place of `clawdmeter hook …` and `clawdmeter-relay tokens` runs from a
//! timer. Each sends one small JSON message by HTTP POST to every target in
//! its config; whichever PC runs the app (and is reachable) takes it. The
//! app's listener ([`to_local_hook_input`]) replays a hook message through
//! its own hook code, so the daemon sees it exactly like a local hook.
//!
//! Only the event name, session id, alert kind and account number travel:
//! never the prompt, the tool call or the transcript. Plain HTTP: run it
//! over a private network (Tailscale); every request carries the shared key
//! in `X-Clawdmeter-Key`.

pub mod tokens;

use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

/// Wire format version.
pub const VERSION: u32 = 1;
/// The app's default listening port.
pub const DEFAULT_PORT: u16 = 8790;
/// The one path the app answers.
pub const PATH: &str = "/clawdmeter/v1";
/// Largest request body the app reads.
pub const MAX_BODY: usize = 64 * 1024;
/// Header carrying the shared key.
pub const KEY_HEADER: &str = "x-clawdmeter-key";

/// One hook invocation, reduced to what the device needs.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct HookEvent {
    /// The hook's arguments, e.g. `["alert", "event"]`.
    pub args: Vec<String>,
    /// `hook_event_name`.
    pub event: String,
    /// `session_id`.
    pub session: String,
    /// 1-based account (place in the relay's `accounts` list); `None` when
    /// the transcript's config dir isn't listed.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub account: Option<u8>,
    /// `notification_type`, for the alert kind.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub notification_type: Option<String>,
    /// Unix seconds on the sender.
    pub ts: u64,
}

/// Billed tokens per local day for one account.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AccountDays {
    /// 1-based account.
    pub i: u8,
    /// `(YYYY-MM-DD, billed tokens)`, oldest first.
    pub days: Vec<(String, u64)>,
}

/// One rate-limit window as Claude Code's status line reports it.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct LiveWindow {
    /// `used_percentage`, 0-100.
    pub pct: f64,
    /// `resets_at`, unix seconds.
    pub resets_at: i64,
}

/// The usage numbers Claude Code hands its status line after each response
/// (`rate_limits`, Claude Code 2.1.80+, Pro/Max only): live, for free.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LiveUsage {
    /// 1-based account, as in [`HookEvent::account`].
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub account: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub five_hour: Option<LiveWindow>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub seven_day: Option<LiveWindow>,
    /// Unix seconds on the sender when it was read.
    pub ts: u64,
}

/// [`LiveUsage`] from a status line's stdin JSON; `None` when it carries no
/// `rate_limits` window (before the first response, or not Pro/Max).
pub fn live_usage(input: &Value, accounts: &[PathBuf], ts: u64) -> Option<LiveUsage> {
    let rl = input.get("rate_limits")?;
    let window = |k: &str| {
        let w = rl.get(k)?;
        Some(LiveWindow {
            pct: w.get("used_percentage")?.as_f64()?.clamp(0.0, 100.0),
            resets_at: w.get("resets_at")?.as_i64()?,
        })
    };
    let (five_hour, seven_day) = (window("five_hour"), window("seven_day"));
    if five_hour.is_none() && seven_day.is_none() {
        return None;
    }
    let account = input
        .get("transcript_path")
        .and_then(Value::as_str)
        .and_then(config_dir_of)
        .and_then(|d| accounts.iter().position(|a| a == d))
        .map(|i| (i + 1).min(9) as u8);
    Some(LiveUsage { account, five_hour, seven_day, ts })
}

/// What a message carries.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Body {
    Hook(HookEvent),
    Tokens(Vec<AccountDays>),
    Live(LiveUsage),
    /// `clawdmeter-relay test`: checks the route and the key, changes nothing.
    Ping,
}

/// One request body.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Envelope {
    pub v: u32,
    /// The sender's name, e.g. "proxmox" (`[A-Za-z0-9_-]`, see [`machine_name`]).
    pub machine: String,
    pub body: Body,
}

/// The relay's config (`key = value` lines, `#` comments):
///
/// ```text
/// targets  = workstation:8790, laptop:8790
/// key      = <the same secret as the app's relay_key>
/// machine  = proxmox
/// accounts = /root/.claude, /root/.claude-acc2
/// ```
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RelayConfig {
    pub targets: Vec<String>,
    pub key: String,
    pub machine: String,
    /// Claude Code config dirs, in the same order as the app's `config_dirs`.
    pub accounts: Vec<PathBuf>,
}

impl RelayConfig {
    pub fn parse(text: &str) -> Self {
        let mut c = RelayConfig::default();
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some((k, v)) = line.split_once('=') else { continue };
            let list = || v.split(',').map(str::trim).filter(|s| !s.is_empty()).map(str::to_owned);
            match k.trim() {
                "targets" => c.targets = list().map(|t| with_port(&t)).collect(),
                "key" => c.key = v.trim().to_owned(),
                "machine" => c.machine = machine_name(v.trim()),
                "accounts" => c.accounts = list().map(|a| expand_home(&a)).collect(),
                _ => {}
            }
        }
        c
    }

    /// What's missing for the relay to send anything.
    pub fn problems(&self) -> Vec<&'static str> {
        let mut p = Vec::new();
        if self.targets.is_empty() {
            p.push("no targets");
        }
        if self.key.len() < 16 {
            p.push("key missing or shorter than 16 characters");
        }
        if self.machine.is_empty() {
            p.push("no machine name");
        }
        p
    }
}

/// `~/x` → `$HOME/x` (the app's `config_dirs` allow it, so the relay does too).
fn expand_home(p: &str) -> PathBuf {
    let home = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE"));
    match (p.strip_prefix("~/").or_else(|| p.strip_prefix("~\\")).or((p == "~").then_some("")), home) {
        (Some(rest), Some(h)) => PathBuf::from(h).join(rest),
        _ => PathBuf::from(p),
    }
}

/// `host` → `host:8790`; `host:port` and `[v6]:port` stay.
fn with_port(t: &str) -> String {
    let has_port =
        t.rsplit_once(':').is_some_and(|(h, p)| !h.is_empty() && !h.ends_with(':') && p.parse::<u16>().is_ok());
    if has_port && (!t.starts_with('[') || t.contains("]:")) {
        t.to_owned()
    } else {
        format!("{t}:{DEFAULT_PORT}")
    }
}

/// A machine name reduced to `[A-Za-z0-9_-]`, at most 24 characters.
pub fn machine_name(s: &str) -> String {
    s.chars().filter(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-')).take(24).collect()
}

/// The config dir a transcript path belongs to: the parent of its `projects`.
fn config_dir_of(transcript: &str) -> Option<&Path> {
    Path::new(transcript).ancestors().find(|a| a.file_name().is_some_and(|n| n == "projects"))?.parent()
}

/// Reduce a hook's stdin JSON to a [`HookEvent`]. `None` for input that
/// isn't an object.
pub fn hook_event(args: &[String], input: &Value, accounts: &[PathBuf], ts: u64) -> Option<HookEvent> {
    let obj = input.as_object()?;
    let text = |k: &str| obj.get(k).and_then(Value::as_str).map(str::to_owned);
    let account = text("transcript_path")
        .as_deref()
        .and_then(config_dir_of)
        .and_then(|d| accounts.iter().position(|a| a == d))
        .map(|i| (i + 1).min(9) as u8);
    Some(HookEvent {
        args: args.iter().take(2).cloned().collect(),
        event: text("hook_event_name").unwrap_or_else(|| "unknown".into()),
        session: text("session_id").unwrap_or_default(),
        account,
        notification_type: text("notification_type"),
        ts,
    })
}

/// The app side: the hook input its own hook code would have read, with the
/// session id prefixed by the machine (`proxmox.<sid>`, so sessions count
/// per machine and never collide) and a transcript path under the matching
/// local account, so the daemon attributes the event to that account.
pub fn to_local_hook_input(ev: &HookEvent, machine: &str, config_dirs: &[PathBuf]) -> Value {
    // It came over the network and goes into a path: only [A-Za-z0-9_-].
    let session: String = ev
        .session
        .chars()
        .take(128)
        .map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' { c } else { '_' })
        .collect();
    let sid = format!("{machine}.{session}");
    let mut v = json!({ "hook_event_name": ev.event, "session_id": sid });
    if let Some(dir) = ev.account.and_then(|i| config_dirs.get(usize::from(i).checked_sub(1)?)) {
        let path = dir.join("projects").join(format!("relay-{machine}")).join(format!("{session}.jsonl"));
        v["transcript_path"] = Value::String(path.to_string_lossy().into_owned());
    }
    if let Some(t) = &ev.notification_type {
        v["notification_type"] = Value::String(t.clone());
    }
    // The real alert text stays on the server; the device says where to look.
    if ev.event == "Notification" || ev.args.iter().any(|a| a == "alert") {
        let what = match ev.notification_type.as_deref() {
            Some("idle_prompt") => "Claude is waiting",
            _ => "Claude needs you",
        };
        v["message"] = Value::String(format!("{machine}: {what}"));
    }
    v
}

/// Compare keys without an early exit on the first differing byte.
pub fn keys_match(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    if a.len() != b.len() || a.is_empty() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

// ─── HTTP, the minimum both sides need ──────────────────────────────────────

/// Resolve `target` (DNS can block for tens of seconds) in a worker thread
/// and give up after `timeout`.
fn resolve(target: &str, timeout: Duration) -> io::Result<std::net::SocketAddr> {
    let (tx, rx) = mpsc::channel();
    let t = target.to_owned();
    std::thread::spawn(move || {
        let _ = tx.send(t.to_socket_addrs().map(|mut a| a.next()));
    });
    match rx.recv_timeout(timeout) {
        Ok(Ok(Some(a))) => Ok(a),
        Ok(Ok(None)) => Err(io::Error::new(io::ErrorKind::NotFound, "no address")),
        Ok(Err(e)) => Err(e),
        Err(_) => Err(io::Error::new(io::ErrorKind::TimedOut, "name lookup timed out")),
    }
}

/// POST `body` to `target` (`host:port`) and wait for the status line.
/// Returns the HTTP status code. `timeout` is the budget for the whole
/// exchange (lookup, connect, send, answer), not per step.
pub fn post(target: &str, key: &str, body: &str, timeout: Duration) -> io::Result<u16> {
    let deadline = Instant::now() + timeout;
    let left = || {
        let d = deadline.saturating_duration_since(Instant::now());
        if d.is_zero() {
            Err(io::Error::new(io::ErrorKind::TimedOut, "timed out"))
        } else {
            Ok(d)
        }
    };
    let addr = resolve(target, left()?)?;
    let mut s = TcpStream::connect_timeout(&addr, left()?)?;
    s.set_write_timeout(Some(left()?))?;
    let host = target.rsplit_once(':').map_or(target, |(h, _)| h);
    let req = format!(
        "POST {PATH} HTTP/1.1\r\nHost: {host}\r\nContent-Type: application/json\r\n{KEY_HEADER}: {key}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    s.write_all(req.as_bytes())?;
    s.set_read_timeout(Some(left()?))?;
    let mut status = String::new();
    BufReader::new(s).read_line(&mut status)?;
    status
        .split_whitespace()
        .nth(1)
        .and_then(|c| c.parse().ok())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "no HTTP status"))
}

/// A request the app accepted at the HTTP level.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Request {
    pub method: String,
    pub path: String,
    pub key: Option<String>,
    pub body: String,
}

/// A reader that fails with `TimedOut` once `deadline` has passed, checked
/// before every read (a blocked read still ends by the stream's own timeout).
struct Deadline<R> {
    inner: R,
    deadline: Instant,
}

impl<R: Read> Read for Deadline<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if Instant::now() >= self.deadline {
            return Err(io::Error::new(io::ErrorKind::TimedOut, "request took too long"));
        }
        self.inner.read(buf)
    }
}

/// Read one request (headers, then a `Content-Length` body of at most
/// [`MAX_BODY`]), all of it before `deadline`. Errors on anything else.
pub fn read_request<R: Read>(r: R, deadline: Instant) -> io::Result<Request> {
    let bad = |m: &str| io::Error::new(io::ErrorKind::InvalidData, m.to_owned());
    let mut r = BufReader::new(Deadline { inner: r, deadline }.take((MAX_BODY + 8192) as u64));
    let mut line = String::new();
    r.read_line(&mut line)?;
    let mut parts = line.split_whitespace();
    let (method, path) = (parts.next().ok_or_else(|| bad("empty"))?.to_owned(), parts.next().unwrap_or("").to_owned());
    let (mut len, mut key) = (0usize, None);
    for _ in 0..64 {
        line.clear();
        if r.read_line(&mut line)? == 0 {
            return Err(bad("headers cut off"));
        }
        let l = line.trim_end();
        if l.is_empty() {
            break;
        }
        if let Some((name, value)) = l.split_once(':') {
            match name.trim().to_ascii_lowercase().as_str() {
                "content-length" => len = value.trim().parse().map_err(|_| bad("length"))?,
                KEY_HEADER => key = Some(value.trim().to_owned()),
                _ => {}
            }
        }
    }
    if len > MAX_BODY {
        return Err(bad("body too large"));
    }
    let mut body = vec![0; len];
    r.read_exact(&mut body)?;
    Ok(Request { method, path, key, body: String::from_utf8(body).map_err(|_| bad("utf-8"))? })
}

/// A minimal response.
pub fn respond<W: Write>(mut w: W, code: u16, text: &str) -> io::Result<()> {
    write!(w, "HTTP/1.1 {code} {text}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_parses() {
        let c = RelayConfig::parse(
            "# server\ntargets = workstation, laptop:9000 , [fd7a::1]:8790\nkey = 0123456789abcdef0\nmachine = Prox mox!\naccounts = /root/.claude,/root/.claude-2\n",
        );
        assert_eq!(c.targets, vec!["workstation:8790", "laptop:9000", "[fd7a::1]:8790"]);
        assert_eq!(c.machine, "Proxmox");
        assert_eq!(c.accounts, vec![PathBuf::from("/root/.claude"), PathBuf::from("/root/.claude-2")]);
        assert!(c.problems().is_empty());
        assert_eq!(RelayConfig::parse("key = short").problems().len(), 3);
        if let Some(home) = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")) {
            let c = RelayConfig::parse("accounts = ~/.claude-2");
            assert_eq!(c.accounts, vec![PathBuf::from(home).join(".claude-2")]);
        }
    }

    #[test]
    fn hook_event_keeps_only_the_safe_fields() {
        let input = json!({
            "hook_event_name": "Notification", "session_id": "abc", "notification_type": "permission_prompt",
            "transcript_path": "/root/.claude-2/projects/-root-app/abc.jsonl",
            "prompt": "secret", "message": "Claude needs your permission to use Bash", "tool_input": {"command": "rm -rf /tmp/x"}
        });
        let accounts = [PathBuf::from("/root/.claude"), PathBuf::from("/root/.claude-2")];
        let ev = hook_event(&["alert".into(), "event".into()], &input, &accounts, 7).unwrap();
        assert_eq!(ev.account, Some(2));
        let wire =
            serde_json::to_string(&Envelope { v: VERSION, machine: "proxmox".into(), body: Body::Hook(ev.clone()) })
                .unwrap();
        for leak in ["secret", "rm -rf", "Bash", "-root-app"] {
            assert!(!wire.contains(leak), "{leak} must not travel: {wire}");
        }
        let back: Envelope = serde_json::from_str(&wire).unwrap();
        assert_eq!(back.body, Body::Hook(ev));
        assert!(hook_event(&[], &json!([1]), &accounts, 0).is_none());
    }

    #[test]
    fn replays_under_the_local_account() {
        let ev = HookEvent {
            args: vec!["event".into()],
            event: "UserPromptSubmit".into(),
            session: "abc".into(),
            account: Some(2),
            notification_type: None,
            ts: 1,
        };
        let dirs = [PathBuf::from("/home/u/.claude"), PathBuf::from("/home/u/.claude-2")];
        let v = to_local_hook_input(&ev, "proxmox", &dirs);
        assert_eq!(v["session_id"], "proxmox.abc");
        let tp = PathBuf::from(v["transcript_path"].as_str().unwrap());
        assert!(tp.starts_with("/home/u/.claude-2/projects"));
        assert!(v.get("message").is_none()); // a prompt isn't an alert
        let alert = HookEvent { args: vec!["alert".into()], event: "Notification".into(), ..ev.clone() };
        assert_eq!(to_local_hook_input(&alert, "proxmox", &dirs)["message"], "proxmox: Claude needs you");
        let idle = HookEvent { notification_type: Some("idle_prompt".into()), ..alert };
        assert_eq!(to_local_hook_input(&idle, "proxmox", &dirs)["message"], "proxmox: Claude is waiting");
        let v2 = to_local_hook_input(&HookEvent { session: "../../x/y".into(), ..ev.clone() }, "proxmox", &dirs);
        assert_eq!(v2["session_id"], "proxmox.______x_y");
        assert!(!v2["transcript_path"].as_str().unwrap().contains(".."));
        // Unknown account: no transcript path, so the event is unattributed.
        let v = to_local_hook_input(&HookEvent { account: Some(5), ..ev }, "proxmox", &dirs);
        assert!(v.get("transcript_path").is_none());
    }

    #[test]
    fn live_usage_from_the_status_line() {
        let accounts = [PathBuf::from("/root/.claude")];
        let input = json!({
            "transcript_path": "/root/.claude/projects/p/s.jsonl",
            "rate_limits": {
                "five_hour": {"used_percentage": 23.5, "resets_at": 1738425600},
                "seven_day": {"used_percentage": 41.2, "resets_at": 1738857600}
            }
        });
        let l = live_usage(&input, &accounts, 9).unwrap();
        assert_eq!(l.account, Some(1));
        assert_eq!(l.five_hour, Some(LiveWindow { pct: 23.5, resets_at: 1738425600 }));
        assert_eq!(l.seven_day.unwrap().pct, 41.2);
        // Only one window: still a reading. None at all: nothing.
        assert!(live_usage(&json!({"rate_limits": {"seven_day": {"used_percentage": 1, "resets_at": 2}}}), &[], 0)
            .is_some());
        assert!(live_usage(&json!({"model": {}}), &accounts, 0).is_none());
    }

    #[test]
    fn keys() {
        assert!(keys_match("abcdefabcdefabcd", "abcdefabcdefabcd"));
        assert!(!keys_match("abcdefabcdefabcd", "abcdefabcdefabce"));
        assert!(!keys_match("", ""));
        assert!(!keys_match("abc", "abcd"));
    }

    fn far() -> Instant {
        Instant::now() + Duration::from_secs(60)
    }

    #[test]
    fn slow_request_is_dropped_at_the_deadline() {
        struct Drip;
        impl Read for Drip {
            fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
                std::thread::sleep(Duration::from_millis(20));
                buf[0] = b'a';
                Ok(1)
            }
        }
        let t = Instant::now();
        let e = read_request(Drip, Instant::now() + Duration::from_millis(200)).unwrap_err();
        assert_eq!(e.kind(), io::ErrorKind::TimedOut);
        assert!(t.elapsed() < Duration::from_secs(2));
    }

    #[test]
    fn request_round_trip() {
        let body = r#"{"v":1}"#;
        let raw = format!(
            "POST {PATH} HTTP/1.1\r\nHost: x\r\nX-Clawdmeter-Key: k1\r\nContent-Length: {}\r\n\r\n{body}",
            body.len()
        );
        let r = read_request(raw.as_bytes(), far()).unwrap();
        assert_eq!(
            (r.method.as_str(), r.path.as_str(), r.key.as_deref(), r.body.as_str()),
            ("POST", PATH, Some("k1"), body)
        );
        let huge = format!("POST / HTTP/1.1\r\nContent-Length: {}\r\n\r\n", MAX_BODY + 1);
        assert!(read_request(huge.as_bytes(), far()).is_err());
        assert!(read_request("POST / HTTP/1.1\r\nContent-Length: 5\r\n".as_bytes(), far()).is_err());
    }

    #[test]
    fn post_reaches_a_listener() {
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let target = l.local_addr().unwrap().to_string();
        let server = std::thread::spawn(move || {
            let (s, _) = l.accept().unwrap();
            let r = read_request(&s, far()).unwrap();
            respond(&s, 204, "No Content").unwrap();
            r
        });
        assert_eq!(post(&target, "k", "{}", Duration::from_secs(2)).unwrap(), 204);
        assert_eq!(server.join().unwrap().key.as_deref(), Some("k"));
    }
}
