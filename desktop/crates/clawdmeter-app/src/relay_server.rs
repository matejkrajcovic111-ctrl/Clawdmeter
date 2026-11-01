//! clawdmeter2: the app end of `clawdmeter-relay` (a server running Claude
//! Code without this app, see that crate). Listens on `relay_listen` and,
//! for each request carrying `relay_key`:
//!
//! * a hook message is replayed through [`crate::hooks::run`], so it lands in
//!   events.log / the attention files exactly like a local hook (the session
//!   id prefixed with the machine name, the account mapped to this PC's
//!   `config_dirs` by its number);
//! * live usage goes to `live-<n>.json` ([`clawdmeter_core::live`]);
//! * token totals go to `relay-tokens-<machine>.json` for the charts.
//!
//! One thread, one short request at a time: the relay sends a few small
//! messages a minute.

use std::net::{TcpListener, TcpStream};
use std::path::Path;
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime};

use clawdmeter_core::{live, Paths, Settings};
use clawdmeter_relay::{keys_match, read_request, respond, Body, Envelope, PATH, VERSION};

/// A whole request must arrive within this, counted from accept.
const REQUEST_DEADLINE: Duration = Duration::from_secs(5);
/// At most one wrong-key warning per this.
const WARN_EVERY: Duration = Duration::from_secs(60);
/// A reset time further out than this is nonsense (the longest window is 7 days).
const MAX_RESET_AHEAD: u64 = 8 * 24 * 3600;

/// True when the wrong-key warning may be logged now (and notes that it was).
fn warn_due() -> bool {
    static LAST: Mutex<Option<Instant>> = Mutex::new(None);
    let Ok(mut last) = LAST.lock() else { return false };
    let due = last.is_none_or(|t| t.elapsed() >= WARN_EVERY);
    if due {
        *last = Some(Instant::now());
    }
    due
}

/// Start listening on a background thread. Logs and gives up when the
/// address can't be bound (another instance, a typo).
pub fn spawn(listen: String, paths: Paths) {
    let listener = match TcpListener::bind(&listen) {
        Ok(l) => l,
        Err(e) => {
            log::warn!("relay: can't listen on {listen}: {e}");
            return;
        }
    };
    log::info!("relay: listening on {listen}");
    let _ = std::thread::Builder::new().name("clawdmeter-relay".into()).spawn(move || {
        for stream in listener.incoming().flatten() {
            let _ = stream.set_read_timeout(Some(Duration::from_secs(1)));
            let _ = stream.set_write_timeout(Some(Duration::from_secs(3)));
            if let Err(e) = handle(&stream, &paths) {
                log::debug!("relay: {e}");
            }
        }
    });
}

fn handle(stream: &TcpStream, paths: &Paths) -> std::io::Result<()> {
    let req = match read_request(stream, Instant::now() + REQUEST_DEADLINE) {
        Ok(r) => r,
        Err(e) => return respond(stream, 400, "Bad Request").and(Err(e)),
    };
    if req.method != "POST" || req.path != PATH {
        return respond(stream, 404, "Not Found");
    }
    // The key and the accounts come from the config as it is now.
    let Ok(settings) = Settings::load(paths) else { return respond(stream, 503, "Unavailable") };
    let Some(key) = settings.relay_key.as_deref() else { return respond(stream, 503, "Unavailable") };
    if !req.key.as_deref().is_some_and(|k| keys_match(k, key)) {
        if warn_due() {
            log::warn!(
                "relay: refused a message with a wrong key from {:?} (logged once a minute)",
                stream.peer_addr().ok()
            );
        }
        return respond(stream, 403, "Forbidden");
    }
    let Ok(env) = serde_json::from_str::<Envelope>(&req.body) else { return respond(stream, 400, "Bad Request") };
    if env.v != VERSION || env.machine.is_empty() || clawdmeter_relay::machine_name(&env.machine) != env.machine {
        return respond(stream, 400, "Bad Request");
    }
    accept(&env, &settings, &paths.state_dir);
    respond(stream, 204, "No Content")
}

/// Act on one authenticated message.
pub fn accept(env: &Envelope, settings: &Settings, state_dir: &Path) {
    let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    match &env.body {
        Body::Hook(ev) => {
            let input = clawdmeter_relay::to_local_hook_input(ev, &env.machine, &settings.config_dirs);
            let mode = crate::hooks::Mode::parse(&ev.args);
            log::debug!("relay: {} {} from {}", ev.event, ev.session, env.machine);
            crate::hooks::run(mode, &input.to_string(), state_dir, now);
        }
        Body::Live(l) => {
            // A server with one account may not name it; with several, it must.
            let account = l.account.or((settings.config_dirs.len() == 1).then_some(1));
            if let Some(a) = account.filter(|a| (1..=settings.config_dirs.len()).contains(&usize::from(*a))) {
                // Stamp with our clock: the inbox compares it with ours. The
                // numbers came over the network: keep them sane.
                let window = |w: Option<clawdmeter_relay::LiveWindow>| {
                    let far = i64::try_from(now + MAX_RESET_AHEAD).unwrap_or(i64::MAX);
                    // NaN or a reset time days away: the window is dropped.
                    let w = w.filter(|w| w.pct.is_finite() && w.resets_at <= far)?;
                    Some(clawdmeter_relay::LiveWindow { pct: w.pct.clamp(0.0, 100.0), ..w })
                };
                let stamped = live::LiveUsage {
                    ts: now,
                    five_hour: window(l.five_hour),
                    seven_day: window(l.seven_day),
                    ..l.clone()
                };
                if let Err(e) = live::write(state_dir, a, &stamped) {
                    log::warn!("relay: live usage: {e}");
                }
            }
        }
        Body::Tokens(days) => {
            let file = state_dir.join(format!("relay-tokens-{}.json", env.machine));
            if let Err(e) =
                serde_json::to_string(days).map_err(std::io::Error::other).and_then(|t| std::fs::write(&file, t))
            {
                log::warn!("relay: {}: {e}", file.display());
            } else {
                log::info!("relay: token totals from {}", env.machine);
            }
        }
        Body::Ping => log::info!("relay: test message from {}", env.machine),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use clawdmeter_core::ConfigFile;
    use clawdmeter_relay::{HookEvent, LiveUsage, LiveWindow};

    fn dir(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("cm-relay-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn settings(home: &Path) -> Settings {
        let cfg = ConfigFile::parse("config_dirs = ~/.claude, ~/.claude-2\nrelay_key = 0123456789abcdef\n");
        Settings::from_config(&cfg, &Paths::under(home), None)
    }

    #[test]
    fn hook_lands_in_events_log_under_the_local_account() {
        let d = dir("hook");
        let s = settings(&d);
        let env = Envelope {
            v: VERSION,
            machine: "proxmox".into(),
            body: Body::Hook(HookEvent {
                args: vec!["clear".into(), "event".into()],
                event: "UserPromptSubmit".into(),
                session: "abc".into(),
                account: Some(2),
                notification_type: None,
                ts: 1,
            }),
        };
        accept(&env, &s, &d);
        let log = std::fs::read_to_string(d.join("events.log")).unwrap();
        assert!(log.contains("UserPromptSubmit proxmox.abc"), "{log}");
        assert!(log.trim_end().ends_with(".claude-2"), "attributed to account 2: {log}");
        assert!(d.join("clear-proxmox.abc").exists());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn live_and_tokens_are_saved() {
        let d = dir("live");
        let s = settings(&d);
        let live = LiveUsage {
            account: Some(1),
            five_hour: Some(LiveWindow { pct: 5.0, resets_at: 9 }),
            seven_day: None,
            ts: 0,
        };
        accept(&Envelope { v: VERSION, machine: "proxmox".into(), body: Body::Live(live) }, &s, &d);
        assert!(d.join("live-1.json").exists());
        let unnamed = LiveUsage { account: None, five_hour: None, seven_day: None, ts: 0 };
        accept(&Envelope { v: VERSION, machine: "proxmox".into(), body: Body::Live(unnamed) }, &s, &d);
        assert!(!d.join("live-2.json").exists(), "two accounts: an unnamed reading is dropped");
        accept(&Envelope { v: VERSION, machine: "proxmox".into(), body: Body::Tokens(vec![]) }, &s, &d);
        assert!(d.join("relay-tokens-proxmox.json").exists());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn live_values_are_sanitised() {
        let d = dir("sane");
        let s = settings(&d);
        let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap().as_secs() as i64;
        let send = |account, five, seven| {
            let live = LiveUsage { account, five_hour: five, seven_day: seven, ts: 0 };
            accept(&Envelope { v: VERSION, machine: "p".into(), body: Body::Live(live) }, &s, &d);
        };
        send(Some(0), Some(LiveWindow { pct: 5.0, resets_at: now }), None);
        assert!(!d.join("live-0.json").exists(), "account 0 is rejected");
        send(Some(3), Some(LiveWindow { pct: 5.0, resets_at: now }), None);
        assert!(!d.join("live-3.json").exists(), "beyond config_dirs");
        send(
            Some(1),
            Some(LiveWindow { pct: 250.0, resets_at: now + 100 }),
            Some(LiveWindow { pct: 1.0, resets_at: now + 30 * 86_400 }),
        );
        let l: LiveUsage = serde_json::from_str(&std::fs::read_to_string(d.join("live-1.json")).unwrap()).unwrap();
        assert_eq!(l.five_hour.unwrap().pct, 100.0);
        assert!(l.seven_day.is_none(), "reset a month away is dropped");
        send(Some(2), Some(LiveWindow { pct: f64::NAN, resets_at: now }), None);
        assert!(std::fs::read_to_string(d.join("live-2.json")).unwrap().contains("\"ts\""));
        let l: LiveUsage = serde_json::from_str(&std::fs::read_to_string(d.join("live-2.json")).unwrap()).unwrap();
        assert!(l.five_hour.is_none());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn wrong_key_is_refused_over_tcp() {
        let d = dir("tcp");
        let paths = Paths::under(&d);
        std::fs::create_dir_all(paths.config_file.parent().unwrap()).unwrap();
        std::fs::write(&paths.config_file, "relay_key = 0123456789abcdef\n").unwrap();
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = l.local_addr().unwrap().to_string();
        let p2 = paths.clone();
        let server = std::thread::spawn(move || {
            for _ in 0..2 {
                let (s, _) = l.accept().unwrap();
                let _ = handle(&s, &p2);
            }
        });
        let ping =
            serde_json::to_string(&Envelope { v: VERSION, machine: "proxmox".into(), body: Body::Ping }).unwrap();
        let t = Duration::from_secs(2);
        assert_eq!(clawdmeter_relay::post(&addr, "wrong-key-wrong-key", &ping, t).unwrap(), 403);
        assert_eq!(clawdmeter_relay::post(&addr, "0123456789abcdef", &ping, t).unwrap(), 204);
        server.join().unwrap();
        let _ = std::fs::remove_dir_all(&d);
    }
}
