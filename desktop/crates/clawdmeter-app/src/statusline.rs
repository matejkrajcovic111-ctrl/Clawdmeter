//! clawdmeter2: `clawdmeter statusline`, Claude Code's status line command on
//! this PC. Claude Code pipes it the session's state after each response,
//! including the account's `rate_limits`; it saves those as
//! `live-<n>.json` for the daemon ([`clawdmeter_core::live`]) and prints a
//! short line: `5h 23% · 7d 41%`.
//!
//! A status line you already had keeps showing: set `statusline_command` in
//! the config and it gets the same input; its output is printed instead.

use std::io::{IsTerminal as _, Read as _, Write as _};
use std::process::{Command, Stdio};
use std::time::SystemTime;

use clawdmeter_core::{live, ConfigFile, Paths, Settings};
use clawdmeter_relay::LiveUsage;
use serde_json::Value;

/// The line printed without a `statusline_command`.
pub fn line(l: Option<&LiveUsage>) -> String {
    let Some(l) = l else { return String::new() };
    let part = |name: &str, w: Option<live::LiveWindow>| w.map(|w| format!("{name} {:.0}%", w.pct));
    [part("5h", l.five_hour), part("7d", l.seven_day)].into_iter().flatten().collect::<Vec<_>>().join(" · ")
}

/// Save the reading under the email logged in to the folder it came from.
/// A single folder takes an unattributed reading; with several, it must
/// name one. A folder with no email yet writes nothing.
pub fn record(
    input: &Value,
    settings: &Settings,
    state_dir: &std::path::Path,
    home: &std::path::Path,
    now: u64,
) -> Option<LiveUsage> {
    let l = clawdmeter_relay::live_usage(input, &settings.config_dirs, now)?;
    let account = l.account.or((settings.config_dirs.len() == 1).then_some(1));
    let dir = account.and_then(|a| settings.config_dirs.get(usize::from(a).wrapping_sub(1)));
    if let Some(email) = dir.and_then(|d| clawdmeter_core::identity::logged_in_email(d, home)) {
        let _ = live::write_email(state_dir, &email, &l);
    }
    Some(l)
}

/// Entry point: read stdin, record, print, exit 0 whatever happens.
pub fn main() -> i32 {
    let mut input = String::new();
    let stdin = std::io::stdin();
    if !stdin.is_terminal() {
        let _ = stdin.lock().read_to_string(&mut input);
    }
    let paths = Paths::for_user();
    let cfg = std::fs::read_to_string(&paths.config_file)
        .map(|t| ConfigFile::parse(&t))
        .unwrap_or_else(|_| ConfigFile::parse(""));
    let settings = Settings::from_config(&cfg, &paths, None);
    let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let v: Value = serde_json::from_str(&input).unwrap_or(Value::Null);
    let l = record(&v, &settings, &paths.state_dir, &paths.home, now);
    let out = match cfg.get("statusline_command").filter(|c| !c.trim().is_empty()) {
        Some(c) => run_shell(c, &input).unwrap_or_default(),
        None => line(l.as_ref()),
    };
    print!("{out}");
    let _ = std::io::stdout().flush();
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

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn records_by_account() {
        let d = std::env::temp_dir().join(format!("cm-statusline-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let paths = Paths::under(&d);
        let s = Settings::from_config(&ConfigFile::parse("config_dirs = ~/.claude, ~/.claude-2\n"), &paths, None);
        let tp = paths.home.join(".claude-2").join("projects").join("p").join("s.jsonl");
        let input = json!({
            "transcript_path": tp.to_string_lossy(),
            "rate_limits": {"five_hour": {"used_percentage": 12.0, "resets_at": 99}}
        });
        std::fs::create_dir_all(paths.home.join(".claude-2")).unwrap();
        std::fs::write(
            paths.home.join(".claude-2").join(".claude.json"),
            r#"{"oauthAccount":{"emailAddress":"l@x.com"}}"#,
        )
        .unwrap();
        let l = record(&input, &s, &d, &paths.home, 5).unwrap();
        assert_eq!(l.account, Some(2));
        assert!(d.join("live-e-l@x.com.json").exists() || d.join("live-e-l_x.com.json").exists());
        assert_eq!(line(Some(&l)), "5h 12%");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn statusline_needs_email() {
        let d = std::env::temp_dir().join(format!("cm-statusline-ne-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let paths = Paths::under(&d);
        let s = Settings::from_config(&ConfigFile::parse("config_dirs = ~/.claude-2\n"), &paths, None);
        let tp = paths.home.join(".claude-2").join("projects").join("p").join("s.jsonl");
        let input = json!({
            "transcript_path": tp.to_string_lossy(),
            "rate_limits": {"five_hour": {"used_percentage": 12.0, "resets_at": 99}}
        });
        assert!(record(&input, &s, &d, &paths.home, 5).is_some(), "still printed");
        let wrote = std::fs::read_dir(&d)
            .map(|r| r.flatten().any(|e| e.file_name().to_string_lossy().starts_with("live-")))
            .unwrap_or(false);
        assert!(!wrote);
        let _ = std::fs::remove_dir_all(&d);
    }
}
