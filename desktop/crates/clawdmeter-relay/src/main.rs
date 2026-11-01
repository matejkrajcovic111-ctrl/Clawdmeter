//! `clawdmeter-relay`: run Claude Code on a server, see it on the Clawdmeter.
//!
//! ```text
//! clawdmeter-relay hook <alert|clear|event> [event]   Claude Code hook (stdin: hook JSON)
//! clawdmeter-relay statusline                         Claude Code status line (stdin: status JSON)
//! clawdmeter-relay tokens                             send the last 35 days' token totals (run from a timer)
//! clawdmeter-relay test                               check every target and the key
//! clawdmeter-relay install                            wire the hooks and status line into each account
//! ```
//!
//! Config: `$CLAWDMETER_RELAY_CONFIG`, else `~/.config/clawdmeter-relay/config`
//! (see [`clawdmeter_relay::RelayConfig`]; `statusline_command` keeps a status
//! line you already had: it gets the same input and its output is shown).
//! The hook and status line modes never fail and never print anything but
//! the status line itself, so Claude Code is never disturbed.

use std::io::{IsTerminal as _, Read as _, Write as _};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant, SystemTime};

use clawdmeter_relay::{
    hook_event, live_usage, post, tokens, AccountDays, Body, Envelope, LiveUsage, RelayConfig, VERSION,
};
use serde_json::{json, Value};

/// How long one send may take. Hooks block Claude Code, so keep it short.
const SEND_TIMEOUT: Duration = Duration::from_millis(1500);
/// Days of token totals sent (the device's chart shows 30).
const TOKEN_DAYS: i64 = 35;

fn config_path() -> PathBuf {
    if let Some(p) = std::env::var_os("CLAWDMETER_RELAY_CONFIG") {
        return PathBuf::from(p);
    }
    let home = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")).unwrap_or_default();
    PathBuf::from(home).join(".config/clawdmeter-relay/config")
}

fn load() -> (RelayConfig, String) {
    let text = std::fs::read_to_string(config_path()).unwrap_or_default();
    (RelayConfig::parse(&text), text)
}

fn now_unix() -> u64 {
    SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map_or(0, |d| d.as_secs())
}

fn read_stdin() -> String {
    let mut s = String::new();
    let stdin = std::io::stdin();
    if !stdin.is_terminal() {
        let _ = stdin.lock().read_to_string(&mut s);
    }
    s
}

/// Send to every target at once; results in target order. Never takes much
/// longer than [`SEND_TIMEOUT`]: a target that hasn't answered by then is
/// reported as timed out and its thread left behind.
fn send_all(cfg: &RelayConfig, body: Body) -> Vec<(String, Result<u16, String>)> {
    let env = Envelope { v: VERSION, machine: cfg.machine.clone(), body };
    let Ok(text) = serde_json::to_string(&env) else { return Vec::new() };
    let (tx, rx) = std::sync::mpsc::channel();
    for (i, t) in cfg.targets.iter().enumerate() {
        let (t, key, text, tx) = (t.clone(), cfg.key.clone(), text.clone(), tx.clone());
        std::thread::spawn(move || {
            let _ = tx.send((i, post(&t, &key, &text, SEND_TIMEOUT).map_err(|e| e.to_string())));
        });
    }
    drop(tx);
    let mut results: Vec<Option<Result<u16, String>>> = vec![None; cfg.targets.len()];
    let deadline = Instant::now() + SEND_TIMEOUT + Duration::from_millis(300);
    while results.iter().any(Option::is_none) {
        match rx.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
            Ok((i, r)) => results[i] = Some(r),
            Err(_) => break,
        }
    }
    cfg.targets.iter().zip(results).map(|(t, r)| (t.clone(), r.unwrap_or_else(|| Err("timed out".into())))).collect()
}

fn usable(cfg: &RelayConfig) -> bool {
    cfg.problems().is_empty()
}

fn cmd_hook(args: &[String]) -> i32 {
    let input = read_stdin();
    let (cfg, _) = load();
    if !usable(&cfg) {
        return 0;
    }
    let Ok(v) = serde_json::from_str::<Value>(&input) else { return 0 };
    if let Some(ev) = hook_event(args, &v, &cfg.accounts, now_unix()) {
        send_all(&cfg, Body::Hook(ev));
    }
    0
}

/// `5h 23% · 7d 41%`, or nothing before the first response.
fn default_line(l: Option<&LiveUsage>) -> String {
    let Some(l) = l else { return String::new() };
    let part = |name: &str, w: Option<clawdmeter_relay::LiveWindow>| w.map(|w| format!("{name} {:.0}%", w.pct));
    [part("5h", l.five_hour), part("7d", l.seven_day)].into_iter().flatten().collect::<Vec<_>>().join(" · ")
}

fn cmd_statusline() -> i32 {
    let input = read_stdin();
    let (cfg, raw) = load();
    let v: Value = serde_json::from_str(&input).unwrap_or(Value::Null);
    let live = live_usage(&v, &cfg.accounts, now_unix());
    // Send in the background while the line is drawn.
    let sender = (usable(&cfg) && live.is_some()).then(|| {
        let (cfg, live) = (cfg.clone(), live.clone().unwrap());
        std::thread::spawn(move || send_all(&cfg, Body::Live(live)))
    });
    let passthrough = raw
        .lines()
        .filter_map(|l| l.split_once('='))
        .find(|(k, _)| k.trim() == "statusline_command")
        .map(|(_, v)| v.trim().to_owned())
        .filter(|c| !c.is_empty());
    let line = match passthrough {
        Some(c) => run_shell(&c, &input).unwrap_or_default(),
        None => default_line(live.as_ref()),
    };
    print!("{line}");
    let _ = std::io::stdout().flush();
    if let Some(h) = sender {
        let _ = h.join();
    }
    0
}

fn run_shell(command: &str, input: &str) -> Option<String> {
    let mut child = if cfg!(windows) {
        Command::new("cmd").args(["/C", command]).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().ok()?
    } else {
        Command::new("sh").args(["-c", command]).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().ok()?
    };
    let _ = child.stdin.take()?.write_all(input.as_bytes());
    let out = child.wait_with_output().ok()?;
    Some(String::from_utf8_lossy(&out.stdout).into_owned())
}

fn cmd_tokens() -> i32 {
    let (cfg, _) = load();
    if !usable(&cfg) {
        eprintln!("config {}: {}", config_path().display(), cfg.problems().join(", "));
        return 1;
    }
    let today = chrono::Local::now().date_naive();
    let since = today - chrono::Duration::days(TOKEN_DAYS - 1);
    let accounts: Vec<AccountDays> = cfg
        .accounts
        .iter()
        .enumerate()
        .take(9)
        .map(|(i, dir)| AccountDays {
            i: (i + 1) as u8,
            days: tokens::scan_dir(dir, since, &chrono::Local)
                .iter()
                .map(|d| (d.date.format("%Y-%m-%d").to_string(), d.billed()))
                .collect(),
        })
        .collect();
    report(send_all(&cfg, Body::Tokens(accounts)))
}

fn cmd_test() -> i32 {
    let (cfg, _) = load();
    println!("config: {}", config_path().display());
    if !usable(&cfg) {
        println!("not ready: {}", cfg.problems().join(", "));
        return 1;
    }
    println!("machine: {}, accounts: {}", cfg.machine, cfg.accounts.len());
    report(send_all(&cfg, Body::Ping))
}

/// Print one line per target; success when any target took it.
fn report(results: Vec<(String, Result<u16, String>)>) -> i32 {
    let mut any = false;
    for (t, r) in &results {
        match r {
            Ok(204) => {
                any = true;
                println!("{t}: ok");
            }
            Ok(403) => println!("{t}: refused, the key doesn't match the app's relay_key"),
            Ok(code) => println!("{t}: HTTP {code}"),
            Err(e) => println!("{t}: unreachable ({e})"),
        }
    }
    if any {
        0
    } else {
        1
    }
}

/// The hooks Clawdmeter needs, as in the app's own installer.
const HOOKS: &[(&str, Option<&str>, &str)] = &[
    ("UserPromptSubmit", None, "clear event"),
    ("PostToolUse", None, "clear event"),
    ("Stop", None, "event"),
    ("Notification", None, "alert event"),
    ("PreToolUse", Some("AskUserQuestion"), "alert event"),
];

/// What `merge_settings` did about the status line.
#[derive(Debug, PartialEq, Eq)]
enum StatusLine {
    /// Ours is in (new, or already was); nothing of another's replaced.
    Ours,
    /// A status line of someone else's was replaced; this is its command.
    Replaced(String),
    /// Someone else's status line, different from `kept`, was left alone.
    Conflict(String),
}

/// Add our hooks and status line to one settings.json value. `kept` is the
/// foreign status line command an earlier account already gave up: a
/// different one here is left untouched, as only one can be passed through.
/// Errors when the file isn't the shape Claude Code writes (don't write `v`).
fn merge_settings(v: &mut Value, exe: &str, kept: Option<&str>) -> Result<StatusLine, String> {
    let Some(root) = v.as_object_mut() else { return Err("not a JSON object".into()) };
    if root.get("hooks").is_some_and(|h| !h.is_object()) {
        return Err("`hooks` is not an object".into());
    }
    let ours = |args: &str| format!("{exe} hook {args}");
    for (event, matcher, args) in HOOKS {
        let hooks = root.entry("hooks").or_insert_with(|| json!({}));
        let list = match hooks.get(*event) {
            None => Vec::new(),
            Some(Value::Array(a)) => a.clone(),
            Some(_) => return Err(format!("`hooks.{event}` is not a list")),
        };
        let present = list.iter().any(|g| {
            g["hooks"].as_array().is_some_and(|hs| {
                hs.iter().any(|h| h["command"].as_str().is_some_and(|c| c.contains("clawdmeter-relay hook")))
            })
        });
        if present {
            continue;
        }
        let mut group = json!({ "hooks": [{ "type": "command", "command": ours(args) }] });
        if let Some(m) = matcher {
            group["matcher"] = json!(m);
        }
        let mut list = list;
        list.push(group);
        hooks[*event] = Value::Array(list);
    }
    let old = v["statusLine"]["command"].as_str().map(str::to_owned);
    let foreign = old.filter(|c| !c.contains("clawdmeter-relay"));
    if let Some(f) = &foreign {
        if kept.is_some_and(|k| k != f) {
            return Ok(StatusLine::Conflict(f.clone()));
        }
    }
    v["statusLine"] = json!({ "type": "command", "command": format!("{exe} statusline") });
    Ok(foreign.map_or(StatusLine::Ours, StatusLine::Replaced))
}

fn cmd_install() -> i32 {
    let (cfg, raw) = load();
    if cfg.accounts.is_empty() {
        eprintln!("set `accounts` in {} first", config_path().display());
        return 1;
    }
    let exe = std::env::current_exe().map(|p| p.display().to_string()).unwrap_or_else(|_| "clawdmeter-relay".into());
    // Check and merge every account before writing any: an unreadable
    // settings.json is never overwritten.
    let mut kept: Option<String> = None;
    let mut done = Vec::new();
    for dir in &cfg.accounts {
        let path = dir.join("settings.json");
        let mut v = match std::fs::read_to_string(&path) {
            Ok(t) => match serde_json::from_str::<Value>(&t) {
                Ok(v) => v,
                Err(e) => {
                    eprintln!(
                        "{}: not valid JSON ({e}); nothing was changed. Fix or move it, then run install again",
                        path.display()
                    );
                    return 1;
                }
            },
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => json!({}),
            Err(e) => {
                eprintln!("{}: {e}; nothing was changed", path.display());
                return 1;
            }
        };
        let note = match merge_settings(&mut v, &exe, kept.as_deref()) {
            Ok(StatusLine::Ours) => String::new(),
            Ok(StatusLine::Replaced(old)) => {
                kept.get_or_insert(old);
                String::new()
            }
            Ok(StatusLine::Conflict(old)) => format!(
                " (its own status line `{old}` differs from the one already kept, so it was left as it is; the relay won't report live usage for it)"
            ),
            Err(e) => {
                eprintln!("{}: {e}; nothing was changed. Fix it, then run install again", path.display());
                return 1;
            }
        };
        done.push((path, v, note));
    }
    for (path, v, note) in &done {
        if let Err(e) = write_json(path, v) {
            eprintln!("{}: {e}", path.display());
            return 1;
        }
        println!("{}: hooks and status line installed{note}", path.display());
    }
    if let Some(old) = kept {
        if !raw.lines().any(|l| l.trim_start().starts_with("statusline_command")) {
            let text = format!("{}\nstatusline_command = {old}\n", raw.trim_end());
            let _ = std::fs::write(config_path(), text);
            println!("your previous status line still shows (statusline_command in the config)");
        }
    }
    0
}

fn write_json(path: &Path, v: &Value) -> std::io::Result<()> {
    if let Some(p) = path.parent() {
        std::fs::create_dir_all(p)?;
    }
    let tmp = path.with_extension("json.clawdmeter-tmp");
    std::fs::write(&tmp, serde_json::to_string_pretty(v).unwrap_or_default() + "\n")?;
    std::fs::rename(&tmp, path)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let code = match args.first().map(String::as_str) {
        Some("hook") => cmd_hook(&args[1..]),
        Some("statusline") => cmd_statusline(),
        Some("tokens") => cmd_tokens(),
        Some("test") => cmd_test(),
        Some("install") => cmd_install(),
        _ => {
            eprintln!("usage: clawdmeter-relay <hook ARGS|statusline|tokens|test|install>");
            2
        }
    };
    std::process::exit(code);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn install_merges_and_keeps_what_was_there() {
        let mut v = json!({
            "statusLine": {"type": "command", "command": "~/my-line.sh"},
            "hooks": {"Stop": [{"hooks": [{"type": "command", "command": "echo done"}]}]}
        });
        let exe = "/usr/local/bin/clawdmeter-relay";
        assert_eq!(merge_settings(&mut v, exe, None), Ok(StatusLine::Replaced("~/my-line.sh".into())));
        assert_eq!(v["hooks"]["Stop"].as_array().unwrap().len(), 2);
        assert_eq!(v["hooks"]["PreToolUse"][0]["matcher"], "AskUserQuestion");
        assert_eq!(v["statusLine"]["command"], "/usr/local/bin/clawdmeter-relay statusline");
        // Twice: nothing doubles, and our own status line isn't "kept".
        assert_eq!(merge_settings(&mut v, exe, None), Ok(StatusLine::Ours));
        assert_eq!(v["hooks"]["Stop"].as_array().unwrap().len(), 2);
    }

    #[test]
    fn second_foreign_status_line_is_left_alone() {
        let exe = "/bin/clawdmeter-relay";
        let mut a = json!({"statusLine": {"type": "command", "command": "a.sh"}});
        let mut b = json!({"statusLine": {"type": "command", "command": "b.sh"}});
        let mut c = json!({"statusLine": {"type": "command", "command": "a.sh"}});
        assert_eq!(merge_settings(&mut a, exe, None), Ok(StatusLine::Replaced("a.sh".into())));
        assert_eq!(merge_settings(&mut b, exe, Some("a.sh")), Ok(StatusLine::Conflict("b.sh".into())));
        assert_eq!(b["statusLine"]["command"], "b.sh");
        assert!(b["hooks"]["Stop"].is_array(), "hooks still go in");
        // The same command twice is fine: one pass-through serves both.
        assert_eq!(merge_settings(&mut c, exe, Some("a.sh")), Ok(StatusLine::Replaced("a.sh".into())));
    }

    #[test]
    fn odd_settings_shapes_are_refused() {
        let exe = "/bin/clawdmeter-relay";
        assert!(merge_settings(&mut json!([1]), exe, None).is_err());
        assert!(merge_settings(&mut json!({"hooks": []}), exe, None).is_err());
        assert!(merge_settings(&mut json!({"hooks": {"Stop": {}}}), exe, None).is_err());
    }

    #[test]
    fn default_status_line() {
        let l = LiveUsage {
            account: None,
            five_hour: Some(clawdmeter_relay::LiveWindow { pct: 23.4, resets_at: 0 }),
            seven_day: None,
            ts: 0,
        };
        assert_eq!(default_line(Some(&l)), "5h 23%");
        assert_eq!(default_line(None), "");
    }
}
