//! Who is logged in to each Claude Code config folder, and the fixed account
//! number of each email.

use crate::usage::{UsageReading, Window};
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Most emails that get an account number.
const MAX_ACCOUNTS: usize = 9;

/// Where Claude Code keeps `.claude.json` for a config folder: `~/.claude`
/// uses `~/.claude.json`, any other folder holds its own.
pub fn state_file(dir: &Path, home: &Path) -> PathBuf {
    if dir == home.join(".claude") {
        home.join(".claude.json")
    } else {
        dir.join(".claude.json")
    }
}

/// `oauthAccount.emailAddress` from `.claude.json` text. Duplicate keys
/// are fine (the last wins); anything unreadable is `None`.
pub fn email_from(text: &str) -> Option<String> {
    let v: serde_json::Value = serde_json::from_str(text).ok()?;
    let e = v.get("oauthAccount")?.get("emailAddress")?.as_str()?.trim();
    (!e.is_empty()).then(|| e.to_owned())
}

/// The email logged in to a config folder, if any.
pub fn logged_in_email(dir: &Path, home: &Path) -> Option<String> {
    email_from(&std::fs::read_to_string(state_file(dir, home)).ok()?)
}

/// The last usage read for an account, kept so it can be shown while the
/// account is offline. Times are unix seconds.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SavedReading {
    /// Session percent used (0 when the window had not started).
    pub session_pct: u32,
    /// When the session window resets.
    pub session_resets_at: Option<i64>,
    /// Week percent used; `None` when there was no week line.
    pub week_pct: Option<u32>,
    /// When the week window resets.
    pub week_resets_at: Option<i64>,
    /// When it was read.
    pub read_at: i64,
}

impl SavedReading {
    /// Keep the session and week windows of a reading.
    pub fn from_reading(r: &UsageReading, read_at: i64) -> Self {
        Self {
            session_pct: r.session.as_ref().map_or(0, |w| w.pct),
            session_resets_at: r.session.as_ref().and_then(|w| w.resets_at).map(|t| t.timestamp()),
            week_pct: r.week.as_ref().map(|w| w.pct),
            week_resets_at: r.week.as_ref().and_then(|w| w.resets_at).map(|t| t.timestamp()),
            read_at,
        }
    }

    /// The reading as it stands at `now`: a window that has reset is shown
    /// as not started (no session, week at 0 %).
    pub fn to_reading(&self, now: i64) -> UsageReading {
        let at = |t: Option<i64>| t.and_then(|s| DateTime::<Utc>::from_timestamp(s, 0));
        let expired = |t: Option<i64>| t.is_some_and(|s| s <= now);
        let session = if expired(self.session_resets_at) || (self.session_pct == 0 && self.session_resets_at.is_none())
        {
            None
        } else {
            Some(Window { pct: self.session_pct, resets_at: at(self.session_resets_at) })
        };
        let week = self.week_pct.map(|pct| {
            if expired(self.week_resets_at) {
                Window { pct: 0, resets_at: None }
            } else {
                Window { pct, resets_at: at(self.week_resets_at) }
            }
        });
        UsageReading { session, week, ..Default::default() }
    }
}

/// What `AccountBook::observe` saw.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Observed {
    /// The folder already showed this email.
    Same,
    /// The folder's login changed.
    Switched {
        /// The email before.
        from: Option<String>,
        /// The email now.
        to: Option<String>,
    },
}

/// Fixed account numbers by email, which folder is logged in to which email
/// (now and over time), and each email's last saved reading.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AccountBook {
    emails: Vec<String>,
    /// `(folder, email, since)`, appended only when a folder's email changes.
    timeline: Vec<(PathBuf, Option<String>, i64)>,
    saved: BTreeMap<String, SavedReading>,
}

impl AccountBook {
    /// A book with these emails numbered 1.. in order (capped at 9).
    pub fn new(emails: Vec<String>) -> Self {
        let mut b = Self { emails, ..Self::default() };
        b.tidy();
        b
    }

    /// Drop duplicate emails (first wins) and cap at 9.
    fn tidy(&mut self) {
        let mut seen = Vec::new();
        self.emails.retain(|e| {
            !seen.contains(e) && {
                seen.push(e.clone());
                true
            }
        });
        self.emails.truncate(MAX_ACCOUNTS);
    }

    /// Record the email a folder shows at `now`. A new email gets the next
    /// number (up to 9; beyond that it is tracked but has no number).
    pub fn observe(&mut self, dir: &Path, email: Option<&str>, now: i64) -> Observed {
        if let Some(e) = email {
            if !self.emails.iter().any(|x| x == e) && self.emails.len() < MAX_ACCOUNTS {
                self.emails.push(e.to_owned());
            }
        }
        let from = self.email_of(dir).map(str::to_owned);
        if from.as_deref() == email {
            return Observed::Same;
        }
        let to = email.map(str::to_owned);
        self.timeline.push((dir.to_path_buf(), to.clone(), now));
        Observed::Switched { from, to }
    }

    /// The numbered emails; position + 1 is the number.
    pub fn emails(&self) -> &[String] {
        &self.emails
    }

    /// The account number (1-based) of an email.
    pub fn number_of(&self, email: &str) -> Option<u8> {
        self.emails.iter().position(|e| e == email).map(|i| i as u8 + 1)
    }

    /// The email a folder shows now.
    pub fn email_of(&self, dir: &Path) -> Option<&str> {
        self.timeline.iter().rev().find(|(d, ..)| d == dir)?.1.as_deref()
    }

    /// The email a folder showed at `ts`. Before its first stint: the first
    /// email seen there.
    pub fn email_at(&self, dir: &Path, ts: i64) -> Option<&str> {
        let mut stints = self.timeline.iter().filter(|(d, ..)| d == dir);
        let mut cur = stints.next()?;
        for s in stints {
            if s.2 <= ts {
                cur = s;
            }
        }
        cur.1.as_deref()
    }

    /// Folders logged in to this email now.
    pub fn dirs_of(&self, email: &str) -> Vec<PathBuf> {
        let mut out: Vec<PathBuf> = Vec::new();
        for (d, ..) in &self.timeline {
            if !out.contains(d) && self.email_of(d) == Some(email) {
                out.push(d.clone());
            }
        }
        out
    }

    /// Whether any folder is logged in to this email now.
    pub fn online(&self, email: &str) -> bool {
        !self.dirs_of(email).is_empty()
    }

    /// Drop an email from the table (and its saved reading); later numbers
    /// shift down.
    pub fn forget(&mut self, email: &str) {
        self.emails.retain(|e| e != email);
        self.saved.remove(email);
    }

    /// Keep the last reading of an account.
    pub fn save_reading(&mut self, email: &str, r: &UsageReading, at: i64) {
        self.saved.insert(email.to_owned(), SavedReading::from_reading(r, at));
    }

    /// The last saved reading of an account.
    pub fn saved(&self, email: &str) -> Option<&SavedReading> {
        self.saved.get(email)
    }

    /// Read a book from a JSON file.
    pub fn load(path: &Path) -> std::io::Result<Self> {
        let mut b: Self = serde_json::from_slice(&std::fs::read(path)?).map_err(std::io::Error::other)?;
        b.tidy();
        Ok(b)
    }

    /// Write the book as JSON: to a temp file, then renamed into place.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let mut tmp = path.as_os_str().to_owned();
        tmp.push(".tmp");
        let tmp = PathBuf::from(tmp);
        std::fs::write(&tmp, serde_json::to_vec_pretty(self).map_err(std::io::Error::other)?)?;
        std::fs::rename(&tmp, path)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn d(s: &str) -> PathBuf {
        PathBuf::from(s)
    }

    #[test]
    fn observe_switch() {
        let mut b = AccountBook::default();
        let dir = d("/h/.claude");
        assert_eq!(b.observe(&dir, Some("a@x"), 10), Observed::Switched { from: None, to: Some("a@x".into()) });
        assert_eq!(b.observe(&dir, Some("a@x"), 15), Observed::Same);
        assert_eq!(
            b.observe(&dir, Some("b@x"), 20),
            Observed::Switched { from: Some("a@x".into()), to: Some("b@x".into()) }
        );
        assert_eq!(b.email_of(&dir), Some("b@x"));
        assert_eq!(b.email_at(&dir, 19), Some("a@x"));
        assert_eq!(b.email_at(&dir, 20), Some("b@x"));
        assert_eq!(b.email_at(&dir, 1), Some("a@x"));
        assert_eq!(b.email_at(&d("/other"), 20), None);
    }

    #[test]
    fn numbers_fixed_and_appended() {
        let mut b = AccountBook::new(vec!["m@x".into(), "l@x".into()]);
        b.observe(&d("/a"), Some("l@x"), 1);
        assert_eq!(b.number_of("l@x"), Some(2));
        b.observe(&d("/b"), Some("z@x"), 2);
        assert_eq!(b.number_of("z@x"), Some(3));
        for i in 4..=10 {
            b.observe(&d(&format!("/d{i}")), Some(&format!("e{i}@x")), i);
        }
        assert_eq!(b.number_of("e9@x"), Some(9));
        assert_eq!(b.number_of("e10@x"), None);
        assert_eq!(b.emails().len(), 9);
        assert_eq!(b.email_of(&d("/d10")), Some("e10@x"));
    }

    #[test]
    fn forget_shifts_numbers() {
        let mut b = AccountBook::new(vec!["m@x".into(), "l@x".into()]);
        b.forget("m@x");
        assert_eq!(b.number_of("l@x"), Some(1));
    }

    #[test]
    fn forgotten_email_comes_back_on_observe() {
        let mut b = AccountBook::default();
        b.observe(&d("/a"), Some("m@x"), 1);
        b.forget("m@x");
        assert_eq!(b.number_of("m@x"), None);
        assert_eq!(b.observe(&d("/a"), Some("m@x"), 2), Observed::Same);
        assert_eq!(b.number_of("m@x"), Some(1));
    }

    #[test]
    fn new_dedupes() {
        let b = AccountBook::new(vec!["a@x".into(), "b@x".into(), "a@x".into()]);
        assert_eq!(b.emails(), ["a@x".to_owned(), "b@x".to_owned()]);
    }

    #[test]
    fn same_email_two_dirs() {
        let mut b = AccountBook::default();
        b.observe(&d("/a"), Some("m@x"), 1);
        b.observe(&d("/b"), Some("m@x"), 2);
        assert_eq!(b.dirs_of("m@x").len(), 2);
        assert!(b.online("m@x"));
        b.observe(&d("/a"), None, 3);
        assert!(b.online("m@x"));
        b.observe(&d("/b"), None, 4);
        assert!(!b.online("m@x"));
    }

    fn win(pct: u32, at: i64) -> Window {
        Window { pct, resets_at: DateTime::<Utc>::from_timestamp(at, 0) }
    }

    #[test]
    fn saved_reading_expires() {
        let r = UsageReading { session: Some(win(40, 100)), week: Some(win(70, 1000)), ..Default::default() };
        let s = SavedReading::from_reading(&r, 5);
        assert_eq!(s.to_reading(50), r);
        let late = s.to_reading(150);
        assert_eq!(late.session, None);
        assert_eq!(late.week, Some(win(70, 1000)));
        let later = s.to_reading(2000);
        assert_eq!(later.session, None);
        assert_eq!(later.week, Some(Window { pct: 0, resets_at: None }));
        assert!(later.models.is_empty() && later.spend.is_none() && !later.overage);
    }

    #[test]
    fn persist_roundtrip() {
        let mut b = AccountBook::new(vec!["m@x".into()]);
        b.observe(&d("/a"), Some("m@x"), 1);
        let r = UsageReading { session: Some(win(40, 100)), ..Default::default() };
        b.save_reading("m@x", &r, 9);
        let path = std::env::temp_dir().join(format!("clawd-book-{}.json", std::process::id()));
        b.save(&path).unwrap();
        assert!(!path.with_extension("json.tmp").exists());
        assert_eq!(AccountBook::load(&path).unwrap(), b);
        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn state_file_choice() {
        let home = Path::new("/h");
        assert_eq!(state_file(&home.join(".claude"), home), home.join(".claude.json"));
        assert_eq!(state_file(&home.join(".claude-2"), home), home.join(".claude-2").join(".claude.json"));
    }

    #[test]
    fn email_lenient() {
        let dup = r#"{"projects":{"E:/Website":{"a":1},"E:/website":{"a":2}},
            "oauthAccount":{"emailAddress":" me@x.com "}}"#;
        assert_eq!(email_from(dup).as_deref(), Some("me@x.com"));
        assert_eq!(email_from("{}"), None);
        assert_eq!(email_from("not json"), None);
        assert_eq!(email_from(r#"{"oauthAccount":{"emailAddress":""}}"#), None);
    }

    #[test]
    fn email_missing_file() {
        assert_eq!(logged_in_email(Path::new("/nope/.claude-9"), Path::new("/nope")), None);
    }
}
