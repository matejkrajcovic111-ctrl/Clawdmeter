//! Live usage from Claude Code's status line (clawdmeter2).
//!
//! Since 2.1.80 Claude Code hands its status line command the account's
//! `rate_limits` after each response. `clawdmeter statusline` (this PC) and
//! the relay's listener (a server's, see `clawdmeter_relay`) write the latest
//! per account to `<state>/live-<n>.json`; the daemon reads them with a
//! [`LiveInbox`] and feeds them to the engine as a fresh reading, so the
//! device follows usage within seconds and without a single extra API call.
//!
//! A window Claude Code leaves out has reset (it drops one once `resets_at`
//! passes). The 5-hour one is then "not started"; a missing weekly one keeps
//! the last known reading unless its reset has passed, so no data is never
//! shown as a zero.

use std::collections::HashMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use chrono::{DateTime, TimeZone, Utc};
pub use clawdmeter_relay::{LiveUsage, LiveWindow};

use crate::usage::{UsageReading, Window};

/// Live files older than this are ignored (a session that went quiet).
pub const MAX_AGE_SECS: u64 = 15 * 60;
/// At most one live reading per account this often (the device's refresh).
pub const MIN_INTERVAL_SECS: u64 = 10;

fn file(state_dir: &Path, account: u8) -> PathBuf {
    state_dir.join(format!("live-{account}.json"))
}

/// Save the latest live usage for `account` (1-based), atomically.
pub fn write(state_dir: &Path, account: u8, l: &LiveUsage) -> io::Result<()> {
    fs::create_dir_all(state_dir)?;
    let tmp = state_dir.join(format!("live-{account}.json.{}", std::process::id()));
    fs::write(&tmp, serde_json::to_string(l).map_err(io::Error::other)?)?;
    fs::rename(&tmp, file(state_dir, account)).inspect_err(|_| {
        let _ = fs::remove_file(&tmp);
    })
}

fn email_file(state_dir: &Path, email: &str) -> PathBuf {
    let safe: String = email
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '-') { c } else { '_' })
        .collect();
    state_dir.join(format!("live-e-{safe}.json"))
}

/// Save the latest live usage for the account logged in as `email`, atomically.
pub fn write_email(state_dir: &Path, email: &str, l: &LiveUsage) -> io::Result<()> {
    fs::create_dir_all(state_dir)?;
    let dest = email_file(state_dir, email);
    let tmp = dest.with_extension(format!("json.{}", std::process::id()));
    fs::write(&tmp, serde_json::to_string(l).map_err(io::Error::other)?)?;
    fs::rename(&tmp, dest).inspect_err(|_| {
        let _ = fs::remove_file(&tmp);
    })
}

fn window(w: &LiveWindow) -> Window {
    Window { pct: w.pct.round() as u32, resets_at: Utc.timestamp_opt(w.resets_at, 0).single() }
}

/// The reading a live update gives, on top of the account's last one.
pub fn merge(prev: Option<&UsageReading>, l: &LiveUsage, now: DateTime<Utc>) -> UsageReading {
    let mut r = prev.cloned().unwrap_or_default();
    r.session = l.five_hour.as_ref().map(window);
    r.week = match &l.seven_day {
        Some(w) => Some(window(w)),
        None => r.week.filter(|w| w.resets_at.is_none_or(|t| t > now)),
    };
    r.spend = None;
    r
}

/// Follows the `live-<n>.json` files.
#[derive(Debug)]
pub struct LiveInbox {
    dir: PathBuf,
    /// Per account: the `ts` of the last reading taken, and when (our clock).
    seen: HashMap<u8, (u64, u64)>,
    /// The same, per email.
    seen_email: HashMap<String, (u64, u64)>,
}

impl LiveInbox {
    /// Watch `state_dir`.
    pub fn new(state_dir: &Path) -> Self {
        LiveInbox { dir: state_dir.to_owned(), seen: HashMap::new(), seen_email: HashMap::new() }
    }

    /// New readings for accounts 1..=`accounts`: not taken before, younger
    /// than [`MAX_AGE_SECS`], and at most one per [`MIN_INTERVAL_SECS`] of
    /// our own clock. The newest file stays pending while throttled, so the
    /// last reading of a burst is taken once the interval has passed.
    pub fn take(&mut self, accounts: usize, now_unix: u64) -> Vec<(u8, LiveUsage)> {
        let mut out = Vec::new();
        for i in 1..=accounts.min(9) as u8 {
            let Ok(text) = fs::read_to_string(file(&self.dir, i)) else { continue };
            let Ok(l) = serde_json::from_str::<LiveUsage>(&text) else { continue };
            let seen = self.seen.get(&i).copied();
            if l.ts + MAX_AGE_SECS < now_unix || seen.is_some_and(|(ts, _)| l.ts <= ts) {
                continue;
            }
            if seen.is_some_and(|(_, at)| now_unix < at + MIN_INTERVAL_SECS) {
                continue;
            }
            self.seen.insert(i, (l.ts, now_unix));
            out.push((i, l));
        }
        out
    }

    /// New readings per email: from `live-e-<email>.json` for each of
    /// `emails`, and from the numbered `live-<n>.json` files (the relay's)
    /// mapped through `legacy[n-1]`. Same freshness and interval rules as
    /// [`LiveInbox::take`].
    pub fn take_emails(&mut self, emails: &[String], legacy: &[String], now_unix: u64) -> Vec<(String, LiveUsage)> {
        let mut candidates: Vec<(String, PathBuf)> =
            emails.iter().map(|e| (e.clone(), email_file(&self.dir, e))).collect();
        for (i, e) in legacy.iter().enumerate().take(9) {
            candidates.push((e.clone(), file(&self.dir, i as u8 + 1)));
        }
        let mut out = Vec::new();
        for (email, path) in candidates {
            let Ok(text) = fs::read_to_string(path) else { continue };
            let Ok(l) = serde_json::from_str::<LiveUsage>(&text) else { continue };
            let seen = self.seen_email.get(&email).copied();
            if l.ts + MAX_AGE_SECS < now_unix || seen.is_some_and(|(ts, _)| l.ts <= ts) {
                continue;
            }
            if seen.is_some_and(|(_, at)| now_unix < at + MIN_INTERVAL_SECS) {
                continue;
            }
            self.seen_email.insert(email.clone(), (l.ts, now_unix));
            out.push((email, l));
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const NOW: i64 = 1_760_000_000;

    fn live(five: Option<f64>, seven: Option<f64>, ts: u64) -> LiveUsage {
        LiveUsage {
            account: Some(1),
            five_hour: five.map(|pct| LiveWindow { pct, resets_at: NOW + 3600 }),
            seven_day: seven.map(|pct| LiveWindow { pct, resets_at: NOW + 86_400 }),
            ts,
        }
    }

    fn now() -> DateTime<Utc> {
        Utc.timestamp_opt(NOW, 0).unwrap()
    }

    #[test]
    fn merge_rules() {
        let r = merge(None, &live(Some(23.6), Some(41.2), 0), now());
        assert_eq!(r.session.as_ref().unwrap().pct, 24);
        assert_eq!(r.week.as_ref().unwrap().pct, 41);
        // The 5-hour window reset: not started. The weekly one went missing
        // but hasn't reset: kept, never a zero.
        let r2 = merge(Some(&r), &live(None, None, 0), now());
        assert!(r2.session.is_none());
        assert_eq!(r2.week.as_ref().unwrap().pct, 41);
        // ...unless its reset has passed.
        let later = Utc.timestamp_opt(NOW + 90_000, 0).unwrap();
        assert!(merge(Some(&r), &live(None, None, 0), later).week.is_none());
    }

    #[test]
    fn last_reading_of_a_burst_is_taken_later() {
        let d = std::env::temp_dir().join(format!("cm-live-burst-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        let mut inbox = LiveInbox::new(&d);
        let t = NOW as u64;
        write(&d, 1, &live(Some(10.0), None, t)).unwrap();
        assert_eq!(inbox.take(1, t).len(), 1);
        write(&d, 1, &live(Some(16.0), None, t + 6)).unwrap();
        assert!(inbox.take(1, t + 6).is_empty(), "throttled, but stays pending");
        assert!(inbox.take(1, t + 9).is_empty());
        assert_eq!(inbox.take(1, t + 10)[0].1.five_hour.unwrap().pct, 16.0);
        assert!(inbox.take(1, t + 30).is_empty());
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn live_email_roundtrip() {
        let d = std::env::temp_dir().join(format!("cm-live-e-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        let mut inbox = LiveInbox::new(&d);
        let t = NOW as u64;
        write_email(&d, "a+b@x.com", &live(Some(10.0), None, t)).unwrap();
        let emails = vec!["a+b@x.com".to_owned(), "other@x".to_owned()];
        let got = inbox.take_emails(&emails, &[], t);
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].0, "a+b@x.com");
        assert!(inbox.take_emails(&emails, &[], t + 20).is_empty(), "once");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn live_legacy_numbered_maps_to_email() {
        let d = std::env::temp_dir().join(format!("cm-live-leg-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        let mut inbox = LiveInbox::new(&d);
        let t = NOW as u64;
        write(&d, 2, &live(Some(10.0), None, t)).unwrap();
        let legacy = vec!["m@x".to_owned(), "l@x".to_owned()];
        let got = inbox.take_emails(&legacy, &legacy, t);
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].0, "l@x");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn inbox_takes_new_fresh_files_only() {
        let d = std::env::temp_dir().join(format!("cm-live-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        let mut inbox = LiveInbox::new(&d);
        let t = NOW as u64;
        write(&d, 2, &live(Some(10.0), None, t)).unwrap();
        assert!(inbox.take(1, t).is_empty(), "account 2 isn't configured");
        assert_eq!(inbox.take(2, t).len(), 1);
        assert!(inbox.take(2, t + 1).is_empty(), "already taken");
        write(&d, 2, &live(Some(11.0), None, t + 5)).unwrap();
        assert!(inbox.take(2, t + 5).is_empty(), "within the 10 s interval");
        write(&d, 2, &live(Some(12.0), None, t + 12)).unwrap();
        assert_eq!(inbox.take(2, t + 12)[0].1.five_hour.unwrap().pct, 12.0);
        assert!(inbox.take(2, t + 40).is_empty(), "the same file is never taken twice");
        write(&d, 1, &live(Some(1.0), None, t - MAX_AGE_SECS - 1)).unwrap();
        assert!(inbox.take(2, t + 20).is_empty(), "too old");
        let _ = fs::remove_dir_all(&d);
    }
}
