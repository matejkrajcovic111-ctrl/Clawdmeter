//! The daemon's decisions, with no I/O: what to poll, when, and what to write
//! to the device. The runtime (app crate) feeds it events, poll results and
//! the clock, and performs the writes it asks for.
//!
//! Folders (`config_dirs`) are the unit of polling: probe, backoff, rate
//! limits and working/turn tracking are per folder. Accounts are the emails
//! logged in to them ([`AccountBook`]); everything the device gets per
//! account comes from per-email views, numbered by the account table.

use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use chrono::{DateTime, TimeZone, Utc};

use crate::attention::{Alert, AttentionState};
use crate::backoff::SavedBlock;
use crate::cadence::{heartbeat_due, AccountClock, Cadence, PollReason, Scheduler};
use crate::config::Settings;
use crate::events::{machines_line, Activity, HookEvent, HookKind, Signal};
use crate::identity::{AccountBook, Observed};

/// clawdmeter2: a changed prompt token count goes out at most this often.
const TURN_TOKENS_EVERY: i64 = 3;
use crate::payload::{clock_fields, OtherAccount, Payload};
use crate::probe::{self, Plan as ProbePlan, Source};
use crate::rate::{Mood, RateTracker};
use crate::selector::PlanSelector;
use crate::usage::{AccountState, UsageError, UsageReading};

/// `a` changes are sent at most this often (s).
pub const ACTIVITY_MIN_INTERVAL: i64 = 5;
/// `e`/`es` ride along for this long after the event (s), so a device that
/// reconnects or reboots later doesn't react to an old event.
pub const EVENT_FRESH_SECS: i64 = 60;

/// Why a write happens.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WriteKind {
    /// Fresh `/usage` numbers.
    Poll,
    /// Attention alert or clear.
    Attention,
    /// A hook event / activity / session-count change.
    Event,
    /// Keep-alive replay.
    Heartbeat,
    /// Every account is logged out: `{"ok":false}` (with the offline
    /// accounts' last readings in `o`, if any).
    NoData,
}

/// A write the runtime should perform.
#[derive(Debug, Clone, PartialEq)]
pub struct Write {
    /// What to send.
    pub payload: Payload,
    /// Why.
    pub kind: WriteKind,
}

/// Host facts the payload needs at write time.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct HostClock {
    /// Local UTC offset (s), for `t`.
    pub utc_offset_secs: i64,
    /// `clock = auto` resolves to 12h.
    pub prefers_12h: bool,
}

/// A `/usage` poll to run now.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PollPlan {
    /// Why (the earliest-due account's reason).
    pub reason: PollReason,
    /// The folders due now; the rest wait (cadence, gap or backoff).
    pub accounts: Vec<PathBuf>,
    /// How to read each of `accounts` (same order): the probe, `/usage`, or
    /// a reading known without a call ([`crate::probe::choose_source`]).
    pub plans: Vec<ProbePlan>,
    /// Who was logged in to each of `accounts` (same order) when the poll
    /// was planned: its result is saved for that email.
    pub emails: Vec<String>,
}

/// One account's last poll error, for the settings window.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AccountError {
    /// The account (config dir).
    pub dir: PathBuf,
    /// What to tell the user.
    pub message: String,
    /// The app is retrying on its own (rate limit, timeout, odd output);
    /// otherwise the user has to act.
    pub retrying: bool,
}

/// Snapshot for the tray / status output.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct EngineStatus {
    /// The folder whose numbers are shown on the device.
    pub active_account: Option<PathBuf>,
    /// Its latest numbers.
    pub reading: Option<UsageReading>,
    /// Seconds since those numbers were read.
    pub age_secs: Option<i64>,
    /// Three misses in a row on the shown account.
    pub stale: bool,
    /// Every account failed with a login problem.
    pub logged_out: bool,
    /// Per-account last error, for the settings window.
    pub errors: Vec<AccountError>,
    /// An attention alert is up.
    pub attention: bool,
    /// Hook activity level 0-3.
    pub activity: u8,
    /// Sessions active in the last 5 min.
    pub sessions: u32,
    /// Hooks have been seen (fast clock available).
    pub hooks: bool,
    /// Mood from the rate math.
    pub mood: Option<Mood>,
    /// Unix seconds of the next scheduled poll.
    pub next_poll_at: Option<i64>,
    /// The earliest a refresh of the shown account can poll it (the
    /// per-account gap and backoff); `None`: right away.
    pub refresh_at: Option<i64>,
    /// The usage endpoint has blocked the shown account (a 429) until then:
    /// nothing asks it before.
    pub paused_until: Option<i64>,
    /// The shown account is being polled.
    pub polling: bool,
}

/// One numbered account (an email) as the device sees it.
#[derive(Debug, Clone)]
struct AccountView {
    /// 1-based account number (place in the account table).
    number: u8,
    email: String,
    /// Online: the newest folder reading (none yet right after a login);
    /// offline: the saved one, as it stands now.
    reading: Option<UsageReading>,
    /// Some watched folder is logged in to it now.
    online: bool,
    /// The watched folders logged in to it now.
    dirs: Vec<PathBuf>,
}

/// Per-account flags for one write, by view (account number - 1).
struct Flags {
    wk: Vec<bool>,
    th: Vec<bool>,
    wt: Vec<Option<u64>>,
    /// Unix seconds of the account's latest prompt.
    since: Vec<Option<i64>>,
    /// The tool code it is running now (only while working).
    tl: Vec<Option<&'static str>>,
}

/// The daemon state machine.
#[derive(Debug)]
pub struct Engine {
    settings: Settings,
    scheduler: Scheduler,
    activity: Activity,
    attention: AttentionState,
    /// Who is logged in where; account numbers; saved readings.
    book: AccountBook,
    /// Keyed by email; fed only online accounts.
    selector: PlanSelector<String>,
    /// Per watched folder (the unit of polling).
    accounts: BTreeMap<PathBuf, AccountState>,
    in_flight: Vec<PathBuf>,
    /// The email each folder had when its poll was planned.
    planned: BTreeMap<PathBuf, String>,
    /// The shown account's email (always an online one).
    active: Option<String>,
    /// A folder's login changed since the last write.
    logins_changed: bool,
    rate: RateTracker,
    hooks_installed: bool,
    fresh_poll: bool,
    logged_out: bool,
    last_write: Option<i64>,
    sent_seq: u32,
    sent_a: Option<u8>,
    sent_a_at: i64,
    sent_n: Option<u32>,
    sent_sm: Option<Option<String>>,
    /// The liveliness the device last got (`lv`).
    sent_lv: Option<u8>,
    /// The attention animation the device last got (`aa`).
    sent_aa: Option<u16>,
    /// The pace marker the device last got (`pm`).
    sent_pm: Option<u8>,
    last_event_ts: Option<i64>,
    /// Latest prompt or tool activity that names no configured account.
    unattributed_activity: Option<i64>,
    /// A `claude` binary was found, so the probe can run.
    claude_found: bool,
    /// A new alert should chime on this computer (`desktop_chime`).
    desktop_chime_due: bool,
    /// clawdmeter2: which folders Claude is working in (`wk`).
    working: crate::working::Working,
    /// The `wk` flags (by account number) the device last got, and the
    /// ones the write in flight carries.
    sent_wk: Option<Vec<bool>>,
    pending_wk: Option<Vec<bool>>,
    /// The `th` (thinking) flags, like `wk`.
    sent_th: Option<Vec<bool>>,
    pending_th: Option<Vec<bool>>,
    /// clawdmeter2: the tool codes last sent (`tl`) and the ones in flight.
    sent_tl: Option<Vec<Option<&'static str>>>,
    pending_tl: Option<Vec<Option<&'static str>>>,
    /// clawdmeter2: tokens of each folder's current prompt (`wt`), and the
    /// per-account ones the device last got / the write in flight carries.
    turns: crate::turn::Turns,
    sent_wt: Option<Vec<Option<u64>>>,
    pending_wt: Option<Vec<Option<u64>>>,
}

fn utc(now: i64) -> DateTime<Utc> {
    Utc.timestamp_opt(now, 0).single().unwrap_or_default()
}

impl Engine {
    /// A fresh engine; the first `poll_due` fires immediately. The account
    /// table starts from `settings.accounts`.
    pub fn new(settings: Settings) -> Self {
        let cadence = Cadence::from_settings(settings.poll_interval, settings.poll_interval_explicit);
        Engine {
            book: AccountBook::new(settings.accounts.clone()),
            settings,
            scheduler: Scheduler::new(cadence),
            activity: Activity::default(),
            attention: AttentionState::default(),
            selector: PlanSelector::default(),
            accounts: BTreeMap::new(),
            in_flight: Vec::new(),
            planned: BTreeMap::new(),
            active: None,
            logins_changed: false,
            rate: RateTracker::default(),
            hooks_installed: false,
            fresh_poll: false,
            logged_out: false,
            last_write: None,
            sent_seq: 0,
            sent_a: None,
            sent_a_at: i64::MIN / 2,
            sent_n: None,
            sent_sm: None,
            sent_lv: None,
            sent_aa: None,
            sent_pm: None,
            last_event_ts: None,
            unattributed_activity: None,
            claude_found: true,
            desktop_chime_due: false,
            working: Default::default(),
            sent_wk: None,
            pending_wk: None,
            sent_th: None,
            pending_th: None,
            sent_tl: None,
            pending_tl: None,
            turns: Default::default(),
            sent_wt: None,
            pending_wt: None,
        }
    }

    /// Current settings.
    pub fn settings(&self) -> &Settings {
        &self.settings
    }

    /// The account table and who is logged in where.
    pub fn book(&self) -> &AccountBook {
        &self.book
    }

    /// The account table, to load a saved one or forget an email.
    pub fn book_mut(&mut self) -> &mut AccountBook {
        &mut self.book
    }

    /// Apply re-read settings (the config is re-read every few seconds).
    pub fn update_settings(&mut self, settings: Settings) {
        if settings == self.settings {
            return;
        }
        self.scheduler.cadence = Cadence::from_settings(settings.poll_interval, settings.poll_interval_explicit);
        // A newly added account has never been polled, so it is due at once.
        if settings.config_dirs != self.settings.config_dirs {
            self.accounts.retain(|k, _| settings.config_dirs.contains(k));
        }
        self.settings = settings;
    }

    /// The email logged in to a watched folder now (`None`: logged out, or
    /// logged in but not yet known who). On a change the folder's own state
    /// is dropped, so its old numbers, a poll in flight, its working light
    /// and its prompt never count for the new login.
    pub fn on_folder_email(&mut self, dir: &Path, email: Option<&str>, now: i64) {
        if self.book.observe(dir, email, now) == Observed::Same {
            return;
        }
        if let Some(st) = self.accounts.get_mut(dir) {
            // A 429 block stays with the folder; everything else starts over.
            let blocked = st.last_error.as_ref().is_some_and(UsageError::is_throttle)
                && st.backoff.until().is_some_and(|u| u > now);
            let keep = blocked.then(|| (st.backoff, st.last_error.clone(), st.last_attempt_at));
            *st = AccountState::default();
            if let Some((backoff, err, attempt)) = keep {
                st.backoff = backoff;
                st.last_error = err;
                st.last_attempt_at = attempt;
            }
        }
        self.in_flight.retain(|d| d != dir);
        self.working.forget(dir);
        self.turns.forget(dir);
        self.logins_changed = true;
        self.logged_out = self.all_auth();
        self.reselect();
    }

    /// Whether events.log exists.
    pub fn set_hooks_installed(&mut self, installed: bool) {
        self.hooks_installed = installed;
    }

    /// History from events.log at start-up (no `e` is emitted for it).
    pub fn seed_events(&mut self, events: &[HookEvent]) {
        self.activity.seed(events);
        for ev in events {
            self.note_working(ev);
        }
    }

    /// The watched folder an event belongs to: the one it names, or the only
    /// one there is; `None` when that folder has no known login (its events
    /// count for nobody).
    fn event_dir(&self, ev: &HookEvent) -> Option<PathBuf> {
        let dirs = &self.settings.config_dirs;
        let d = match &ev.account {
            Some(a) => dirs.iter().find(|d| *d == a).cloned(),
            None if dirs.len() == 1 => dirs.first().cloned(),
            None => None,
        }?;
        self.book.email_of(&d).is_some().then_some(d)
    }

    fn note_working(&mut self, ev: &HookEvent) {
        if let Some(d) = self.event_dir(ev) {
            self.working.note(&d, &ev.session, &ev.kind, ev.ts);
            match ev.kind {
                HookKind::UserPromptSubmit => self.turns.prompt(&d, &ev.session, ev.ts),
                HookKind::Stop => self.turns.stop(&d, &ev.session, ev.ts),
                _ => {}
            }
        }
    }

    /// Read the transcripts' new lines for the current prompts' tokens
    /// (file I/O: the daemon calls this every few seconds).
    pub fn refresh_turns(&mut self, now: i64) {
        self.turns.refresh(now);
    }

    /// The watched folders logged in to `email` now.
    fn dirs_of(&self, email: &str) -> Vec<PathBuf> {
        self.book.dirs_of(email).into_iter().filter(|d| self.settings.config_dirs.contains(d)).collect()
    }

    /// Of these folders, the one with the latest prompt (the first on a tie).
    fn busiest<'a>(&self, dirs: &'a [PathBuf]) -> Option<&'a PathBuf> {
        dirs.iter().rev().max_by_key(|d| self.turns.since(d))
    }

    /// The folder of `email` with the newest completed reading.
    fn newest_dir(&self, email: &str) -> Option<PathBuf> {
        self.dirs_of(email)
            .into_iter()
            .filter_map(|d| {
                let st = self.accounts.get(&d)?;
                st.last.as_ref()?;
                Some((st.last_ok_at, d))
            })
            .max_by_key(|(at, _)| *at)
            .map(|(_, d)| d)
    }

    /// Every numbered account, in number order.
    fn views(&self, now: i64) -> Vec<AccountView> {
        self.book
            .emails()
            .iter()
            .enumerate()
            .map(|(i, email)| {
                let dirs = self.dirs_of(email);
                let online = !dirs.is_empty();
                let reading = if online {
                    self.newest_dir(email).and_then(|d| self.accounts.get(&d)?.last.clone())
                } else {
                    self.book.saved(email).map(|s| s.to_reading(now))
                };
                AccountView { number: (i + 1) as u8, email: email.clone(), reading, online, dirs }
            })
            .collect()
    }

    /// Per account: working (any of its folders), thinking (working and the
    /// transcript's guess), and its latest prompt's tokens and start.
    fn flags(&self, views: &[AccountView], now: i64) -> Flags {
        let working = |d: &PathBuf| self.working.is_working(d, now);
        let latest: Vec<Option<&PathBuf>> = views.iter().map(|v| self.busiest(&v.dirs)).collect();
        Flags {
            wk: views.iter().map(|v| v.dirs.iter().any(working)).collect(),
            th: views
                .iter()
                .map(|v| v.dirs.iter().any(|d| working(d) && self.turns.thinking(d) == Some(true)))
                .collect(),
            wt: latest.iter().map(|d| d.and_then(|d| self.turns.tokens(d))).collect(),
            since: latest.iter().map(|d| d.and_then(|d| self.turns.since(d))).collect(),
            tl: views
                .iter()
                .zip(&latest)
                .map(|(v, d)| {
                    let d = (*d).filter(|d| v.dirs.iter().any(|x| x == *d) && working(d))?;
                    self.turns.tool(d).map(crate::toolinfo::tool_code)
                })
                .collect(),
        }
    }

    /// One live hook event.
    pub fn on_hook_event(&mut self, ev: &HookEvent, now: i64) {
        // A watched folder with no known login counts for nobody: not for
        // the activity signal either (a prompt there must not start a poll).
        if let Some(d) = ev.account.as_ref().and_then(|a| self.settings.config_dirs.iter().find(|d| *d == a)) {
            if self.book.email_of(d).is_none() {
                return;
            }
        }
        self.note_working(ev);
        if ev.kind.sends_messages() {
            match ev.account.as_ref().and_then(|a| self.settings.config_dirs.iter().find(|d| *d == a)) {
                // A folder with no known login: its activity counts for nobody.
                Some(d) if self.book.email_of(d).is_none() => {}
                Some(d) => self.accounts.entry(d.clone()).or_default().note_activity(ev.ts),
                None => self.unattributed_activity = self.unattributed_activity.max(Some(ev.ts)),
            }
        }
        match self.activity.ingest(ev) {
            Signal::Prompt { .. } => {
                self.scheduler.on_prompt(now);
                self.last_event_ts = Some(now);
                self.follow_prompt(ev);
            }
            Signal::Stop => {
                self.scheduler.on_stop(now);
                self.last_event_ts = Some(now);
            }
            Signal::Tool | Signal::Other => {}
        }
    }

    /// clawdmeter2: a prompt in one of several accounts makes it the shown
    /// one right away, without waiting for its usage to move in a poll.
    fn follow_prompt(&mut self, ev: &HookEvent) {
        let Some(email) = ev
            .account
            .as_ref()
            .filter(|a| self.settings.config_dirs.contains(a))
            .and_then(|a| self.book.email_of(a))
            .map(str::to_owned)
        else {
            return;
        };
        if self.newest_dir(&email).is_some() {
            self.selector.touch(&email);
            self.active = Some(email);
        }
    }

    /// One tick's attention files.
    /// Alerts of a kind the user turned off (`alert_on`) are dropped here,
    /// so neither the device nor the tray sees them.
    pub fn on_attention(&mut self, alert: Option<Alert>, clears: &HashMap<String, SystemTime>) {
        let alert = alert.filter(|a| self.settings.alert_on.allows(a.kind));
        if self.attention.ingest(alert, clears) && self.settings.desktop_chime {
            self.desktop_chime_due = true;
        }
    }

    /// A new alert went up since the last call and `desktop_chime` is on:
    /// play the chime on this computer.
    pub fn take_desktop_chime(&mut self) -> bool {
        std::mem::take(&mut self.desktop_chime_due)
    }

    /// End an alert past its time to live (see [`AttentionState::expire`]).
    pub fn expire_attention(&mut self, now: SystemTime) -> bool {
        self.attention.expire(now)
    }

    /// The device says the user dismissed the alert there (or it timed out).
    pub fn on_device_dismissed(&mut self) {
        self.attention.dismissed();
    }

    /// The device (REQ notify) or the user asked for fresh numbers. Accounts
    /// that are backing off, or were polled in the last 2 minutes, still
    /// wait for that to pass.
    pub fn request_refresh(&mut self, now: i64) {
        self.scheduler.request(now);
    }

    /// Some account is offline with a saved reading to show.
    fn has_offline_reading(&self) -> bool {
        self.book.emails().iter().any(|e| self.book.saved(e).is_some() && self.dirs_of(e).is_empty())
    }

    /// A (re)connection: resend everything the device needs.
    pub fn on_connected(&mut self) {
        self.attention.reset();
        self.sent_a = None;
        self.sent_n = None;
        self.sent_sm = None;
        self.sent_lv = None;
        self.sent_aa = None;
        self.sent_pm = None;
        if self.active.is_some() || self.logged_out || self.has_offline_reading() {
            self.fresh_poll = true;
        }
    }

    /// Whether a `claude` binary was found. Without one the probe can't run,
    /// so the idle source (the usage endpoint) answers on its slower cadence
    /// even while a window runs.
    pub fn set_claude_found(&mut self, found: bool) {
        self.claude_found = found;
    }

    /// The accounts whose usage endpoint is blocked at `now` by a 429, to
    /// save across restarts. Other misses back off in memory only.
    pub fn blocks(&self, now: i64) -> BTreeMap<PathBuf, SavedBlock> {
        self.accounts
            .iter()
            .filter(|(_, a)| a.last_error.as_ref().is_some_and(UsageError::is_throttle))
            .filter_map(|(d, a)| {
                let until = a.backoff.until().filter(|u| *u > now)?;
                Some((d.clone(), SavedBlock { strikes: a.backoff.strikes(), until }))
            })
            .collect()
    }

    /// Restore saved blocks (at start-up). Expired ones are dropped; the
    /// rest keep the endpoint and `claude -p /usage` away from that account
    /// until they pass, whatever asks.
    pub fn restore_blocks(&mut self, blocks: &BTreeMap<PathBuf, SavedBlock>, now: i64) {
        for (d, b) in blocks.iter().filter(|(d, b)| b.until > now && self.settings.config_dirs.contains(d)) {
            let st = self.accounts.entry(d.clone()).or_default();
            st.backoff = crate::backoff::Backoff::restore(b.strikes, b.until);
            st.last_error.get_or_insert(UsageError::Throttled { retry_after_secs: None });
        }
    }

    /// Where this account's next reading would come from at `now`.
    fn source(&self, dir: &PathBuf, now: i64) -> Source {
        let empty = AccountState::default();
        match probe::choose_source(self.accounts.get(dir).unwrap_or(&empty), self.unattributed_activity, now) {
            Source::Probe if !self.claude_found => Source::Usage,
            s => s,
        }
    }

    fn clock(&self, dir: &PathBuf, now: i64) -> AccountClock {
        let st = self.accounts.get(dir);
        let source = self.source(dir, now);
        AccountClock {
            shown: self.active.as_deref().is_none_or(|a| self.book.email_of(dir) == Some(a)),
            last_attempt: st.and_then(|a| a.last_attempt_at),
            last_failed: st.is_some_and(|a| a.last_error.is_some()),
            // The backoff is `/usage`'s (its endpoint's rate limit).
            blocked_until: st.and_then(|a| a.backoff.until()).filter(|_| source == Source::Usage),
            usage_only: source == Source::Usage,
            wake_at: st
                .and_then(|a| a.last.as_ref())
                .and_then(|r| r.session.as_ref())
                .and_then(|w| w.resets_at)
                .map(|t| t.timestamp()),
        }
    }

    fn account_due(&self, dir: &PathBuf, now: i64) -> (i64, PollReason) {
        self.scheduler.account_due(&self.clock(dir, now), &self.activity, self.hooks_installed, now)
    }

    fn refresh_at(&self, dir: &PathBuf, now: i64) -> Option<i64> {
        self.scheduler.earliest_poll(&self.clock(dir, now))
    }

    /// The watched folders that can be polled: someone known is logged in.
    fn pollable(&self) -> impl Iterator<Item = &PathBuf> {
        self.settings.config_dirs.iter().filter(|d| self.book.email_of(d).is_some())
    }

    /// The earliest account's next poll: (unix seconds, reason).
    fn next_due(&self, now: i64) -> Option<(i64, PollReason)> {
        self.pollable().map(|d| self.account_due(d, now)).min_by_key(|(t, _)| *t)
    }

    /// `Some(reason)` when some account's `/usage` poll is due now.
    pub fn poll_due(&self, now: i64) -> Option<PollReason> {
        self.next_due(now).filter(|(t, _)| *t <= now).map(|(_, why)| why)
    }

    /// Start a poll: the folders due now (one per account: of several
    /// logged in to one email, the one with the latest prompt), stamped as
    /// attempted. `None` when nothing is due. Hand the results to
    /// [`Engine::on_poll_results`].
    pub fn begin_poll(&mut self, now: i64) -> Option<PollPlan> {
        let reason = self.poll_due(now)?;
        let mut accounts: Vec<PathBuf> = Vec::new();
        let mut emails: Vec<String> = Vec::new();
        let mut siblings: Vec<PathBuf> = Vec::new();
        let due: Vec<(PathBuf, String)> = self
            .pollable()
            .filter(|d| self.account_due(d, now).0 <= now)
            .filter_map(|d| Some((d.clone(), self.book.email_of(d)?.to_owned())))
            .collect();
        for (_, email) in &due {
            if emails.contains(email) {
                continue;
            }
            let dirs: Vec<PathBuf> = due.iter().filter(|(_, e)| e == email).map(|(d, _)| d.clone()).collect();
            let pick = self.busiest(&dirs).cloned().expect("at least one");
            siblings.extend(dirs.into_iter().filter(|d| *d != pick));
            accounts.push(pick);
            emails.push(email.clone());
        }
        let plans = accounts
            .iter()
            .map(|d| match self.source(d, now) {
                Source::Probe => ProbePlan::Probe {
                    usage_fallback: self.accounts.get(d).is_none_or(|a| a.backoff.remaining(now).is_none()),
                },
                Source::Usage => ProbePlan::Usage,
                Source::NotStarted(r) => ProbePlan::Known(r),
            })
            .collect();
        // The other folders of an account wait for the next cycle too.
        for d in accounts.iter().chain(&siblings) {
            self.accounts.entry(d.clone()).or_default().last_attempt_at = Some(now);
        }
        self.in_flight = accounts.clone();
        self.planned = accounts.iter().cloned().zip(emails.iter().cloned()).collect();
        Some(PollPlan { reason, accounts, plans, emails })
    }

    /// Every watched folder with a known login answered with a login problem.
    fn all_auth(&self) -> bool {
        let mut dirs = self.pollable().peekable();
        dirs.peek().is_some()
            && dirs.all(|d| self.accounts.get(d).and_then(|a| a.last_error.as_ref()).is_some_and(UsageError::is_auth))
    }

    /// Pick the shown account among the online ones with a reading.
    /// Accounts that missed (or weren't polled) keep their last values,
    /// which don't count as a rise, so a miss, a rate limit or a backoff
    /// never flips the shown account. Offline accounts are never shown.
    fn reselect(&mut self) {
        let sessions: Vec<(String, u32)> = self
            .book
            .emails()
            .iter()
            .filter_map(|e| {
                let d = self.newest_dir(e)?;
                Some((e.clone(), self.accounts.get(&d)?.last.as_ref()?.session_pct()))
            })
            .collect();
        let prev_active = self.active.clone();
        self.active = self.selector.choose(&sessions);
        if self.active != prev_active && sessions.len() > 1 {
            if let Some(a) = &self.active {
                log::info!("active plan: #{}", self.book.number_of(a).unwrap_or(0));
            }
        }
    }

    /// Results of one poll (the folders of a [`PollPlan`]).
    /// A plain `Result` counts as the idle source's. A good reading is saved
    /// for the email the folder had when the poll was planned; when the
    /// folder's login changed since, the folder itself keeps nothing of it.
    pub fn on_poll_results<R: Into<probe::Polled>>(&mut self, now: i64, results: Vec<(PathBuf, R)>) {
        let results: Vec<(PathBuf, probe::Polled)> = results.into_iter().map(|(d, r)| (d, r.into())).collect();
        let polled = !results.is_empty();
        let mut ok_dirs: Vec<PathBuf> = Vec::new();
        for (dir, r) in results {
            let current = self.book.email_of(&dir).map(str::to_owned);
            // Results without a `begin_poll` (`--once`, live readings) are
            // for whoever is logged in now.
            let email = self.planned.remove(&dir).or_else(|| current.clone());
            if let (Some(e), Ok(reading)) = (&email, &r.result) {
                self.book.save_reading(e, reading, now);
            }
            if email != current {
                continue; // a late answer for the previous login
            }
            if r.result.is_ok() {
                ok_dirs.push(dir.clone());
            }
            let st = self.accounts.entry(dir.clone()).or_default();
            // Results without a `begin_poll` (`--once`) count as attempted now.
            if !self.in_flight.contains(&dir) {
                st.last_attempt_at = Some(now);
            }
            st.record_polled(now, r);
            if let (Some(left), Some(e)) = (st.backoff.remaining(now), &st.last_error) {
                log::info!("{}: {e}; not polling it for {left}s", dir.display());
            }
        }
        let all_auth = self.all_auth();
        self.reselect();
        // Only numbers read in this cycle for the shown account are a real
        // poll. When it missed (another account answered), its last values
        // must not enter the rate math, here or on the device: no `Poll`
        // write, so they would only ride along on a replay.
        let active_fresh = self.active_dir().is_some_and(|a| ok_dirs.contains(&a));
        if active_fresh {
            if let Some(r) = self.active_reading() {
                let pct = r.session_pct();
                self.rate.sample(now, pct, false);
            }
        }
        self.in_flight.clear();
        self.planned.clear();
        self.logged_out = all_auth;
        self.fresh_poll = active_fresh || (all_auth && polled);
    }

    /// clawdmeter2: a live reading from Claude Code's status line for the
    /// account `email` (see [`crate::live`]). It counts as a fresh poll of
    /// the busiest folder logged in to it, without calling anything; ignored
    /// when no watched folder is.
    pub fn on_live(&mut self, email: &str, l: &crate::live::LiveUsage, now: i64) {
        let dirs = self.dirs_of(email);
        let Some(dir) = self.busiest(&dirs).cloned() else {
            return;
        };
        let prev = self.newest_dir(email).and_then(|d| self.accounts.get(&d)?.last.clone());
        let reading = crate::live::merge(prev.as_ref(), l, utc(now));
        // A live update lands between polls: don't disturb one in flight.
        let in_flight = std::mem::take(&mut self.in_flight);
        let planned = std::mem::take(&mut self.planned);
        self.on_poll_results(now, vec![(dir, probe::Polled { result: Ok(reading), idle: false, probe_missed: false })]);
        self.in_flight = in_flight;
        self.planned = planned;
    }

    /// The folder whose numbers are shown: the shown account's newest.
    fn active_dir(&self) -> Option<PathBuf> {
        self.active.as_deref().and_then(|a| self.newest_dir(a))
    }

    fn active_state(&self) -> Option<&AccountState> {
        self.active_dir().and_then(|d| self.accounts.get(&d))
    }

    fn active_reading(&self) -> Option<&UsageReading> {
        self.active_state().and_then(|a| a.last.as_ref())
    }

    /// The shown account has missed three polls in a row.
    pub fn stale(&self, now: i64) -> bool {
        self.active_state().is_some_and(|a| a.is_stale(now))
    }

    /// The next write, if one is due. Call [`Engine::mark_written`] after a
    /// successful write; on failure just call this again next tick.
    pub fn next_write(&mut self, now: i64, host: HostClock) -> Option<Write> {
        self.activity.prune(now);
        let level = self.activity.level(now);
        let sessions = self.activity.sessions_active(now);
        let hooks = self.activity.hooks_seen;
        let event_fresh = self.last_event_ts.is_some_and(|t| now - t < EVENT_FRESH_SECS);

        let new_event = event_fresh && self.activity.seq != self.sent_seq;
        let a_changed = hooks && self.sent_a != Some(level) && now - self.sent_a_at >= ACTIVITY_MIN_INTERVAL;
        let sm = machines_line(&self.activity.sessions_by_machine(now));
        let n_changed = hooks && (self.sent_n != Some(sessions) || self.sent_sm.as_ref() != Some(&sm));
        // A changed device setting goes out right away, like an event.
        let aa = self.settings.attention_anim.seconds();
        let pm = u8::from(self.settings.pace_marker);
        let prefs_changed =
            self.sent_lv != Some(self.settings.liveliness) || self.sent_aa != Some(aa) || self.sent_pm != Some(pm);
        // The recording light goes on or off right away, like an event.
        self.working.prune(now);
        let views = self.views(now);
        let flags = self.flags(&views, now);
        let wk_changed = hooks
            && (self.sent_wk.as_ref() != Some(&flags.wk)
                || self.sent_th.as_ref() != Some(&flags.th)
                || self.sent_tl.as_ref() != Some(&flags.tl));
        // The prompt's token count, at most every TURN_TOKENS_EVERY seconds.
        let wt_changed = hooks
            && self.sent_wt.as_ref() != Some(&flags.wt)
            && self.last_write.is_none_or(|t| now - t >= TURN_TOKENS_EVERY);
        let live = self.active_reading().cloned();
        let offline = views.iter().any(|v| !v.online && v.reading.is_some());

        let (mut payload, kind) = if self.logged_out || (live.is_none() && offline) {
            // Nobody live to show: "no data", with the offline accounts'
            // last numbers (dimmed on the device) for the swipe.
            let due = self.fresh_poll
                || self.attention.pending()
                || (offline
                    && !self.logged_out
                    && (self.logins_changed
                        || self.last_write.is_none()
                        || heartbeat_due(self.last_write, self.settings.heartbeat_interval, now)));
            if !due {
                return None;
            }
            let mut p = Payload::no_data();
            if offline {
                self.add_accounts(&mut p, now, &views, &flags);
            }
            (p, WriteKind::NoData)
        } else {
            // Nothing to show until the first good poll.
            let reading = live?;
            let kind = if self.fresh_poll {
                WriteKind::Poll
            } else if self.attention.pending() {
                WriteKind::Attention
            } else if (new_event
                || a_changed
                || n_changed
                || prefs_changed
                || wk_changed
                || wt_changed
                || self.logins_changed)
                && !self.stale(now)
            {
                // Any ok payload refreshes the device's DATA_FRESH_MS, so
                // while the numbers are stale only alerts and polls go out
                // and the device can show its stale state.
                WriteKind::Event
            } else if !self.stale(now) && heartbeat_due(self.last_write, self.settings.heartbeat_interval, now) {
                WriteKind::Heartbeat
            } else {
                return None;
            };
            let mut p = Payload::from_reading(&reading, utc(now));
            if self.settings.reset_chime {
                p.c = Some(1);
            }
            if let Some(cf) = clock_fields(self.settings.clock, utc(now), host.utc_offset_secs, host.prefers_12h) {
                p.t = Some(cf.local_epoch);
                p.tf = Some(cf.hour_format);
            }
            if kind != WriteKind::Poll {
                p.rp = Some(1);
            }
            p.lv = Some(self.settings.liveliness);
            p.aa = Some(aa);
            p.pm = Some(pm);
            self.add_accounts(&mut p, now, &views, &flags);
            (p, kind)
        };

        let att = self.attention.fields(self.settings.alert_chime);
        if att.beep {
            payload.b = Some(1);
        }
        payload.m = att.message;
        if att.clear {
            payload.x = Some(1);
        }
        if hooks {
            payload.a = Some(level);
            payload.n = Some(sessions);
            payload.sm = sm;
            self.pending_wk = Some(flags.wk);
            self.pending_th = Some(flags.th);
            self.pending_tl = Some(flags.tl);
            self.pending_wt = Some(flags.wt);
            if event_fresh {
                if let Some(code) = self.activity.latest {
                    payload.e = Some(code.as_str().to_owned());
                    payload.es = Some(self.activity.seq);
                }
            }
        }
        Some(Write { payload, kind })
    }

    /// clawdmeter2: tag the usage fields with the shown account's number
    /// (`ai`, from the account table) and add the other accounts' readings
    /// (`o`) so the device can swipe between them; offline ones are marked
    /// `off` and go first, so the 3-entry cap never drops them. `ac` counts
    /// the table's accounts with a reading (live or saved). With one account
    /// numbered #1 none of it is sent, only its flags. Flags (`wk`/`th`/`wt`/
    /// `wd`) are sent once hooks have been seen: the shown account's at the
    /// top, the others' in their `o` entries.
    fn add_accounts(&self, p: &mut Payload, now: i64, views: &[AccountView], f: &Flags) {
        let hooks = self.activity.hooks_seen;
        let flag = |i: usize| hooks.then(|| u8::from(f.wk.get(i).copied().unwrap_or(false)));
        // Thinking only ever goes with working.
        let think = |i: usize| {
            hooks.then(|| u8::from(f.wk.get(i).copied().unwrap_or(false) && f.th.get(i).copied().unwrap_or(false)))
        };
        let toks = |i: usize| {
            if !hooks {
                return None;
            }
            f.wt.get(i).copied().flatten().map(|n| n.min(u32::MAX as u64) as u32)
        };
        // Seconds since that account's latest prompt, so the device's timer
        // matches Claude Code's whatever it missed (a reboot, a swipe).
        let since = |i: usize| {
            hooks
                .then(|| f.since.get(i).copied().flatten())
                .flatten()
                .map(|s| (now - s).clamp(0, u32::MAX as i64) as u32)
        };
        let active = self.active.as_ref().and_then(|a| views.iter().position(|v| &v.email == a));
        let ac = views.iter().filter(|v| v.reading.is_some() || self.book.saved(&v.email).is_some()).count();
        if let Some(i) = active {
            p.wk = flag(i);
            p.th = think(i);
            p.wt = toks(i);
            p.wd = since(i);
            p.tl = hooks.then(|| f.tl.get(i).copied().flatten()).flatten().map(str::to_owned);
            if ac <= 1 && i == 0 {
                return;
            }
            p.ai = Some(views[i].number);
        }
        p.ac = Some(ac.min(9) as u8);
        let mut others: Vec<(usize, &AccountView)> =
            views.iter().enumerate().filter(|(i, v)| Some(*i) != active && v.reading.is_some()).collect();
        others.sort_by_key(|(_, v)| v.online);
        let others: Vec<OtherAccount> = others
            .into_iter()
            .take(3)
            .filter_map(|(i, v)| {
                let mut o = OtherAccount::from_reading(v.number, v.reading.as_ref()?, utc(now));
                o.wk = flag(i);
                o.th = think(i);
                o.wt = toks(i);
                o.wd = since(i);
                o.off = (!v.online).then_some(1);
                Some(o)
            })
            .collect();
        if !others.is_empty() {
            p.o = Some(others);
        }
    }

    /// The write from [`Engine::next_write`] reached the device.
    pub fn mark_written(&mut self, now: i64, w: &Write) {
        self.last_write = Some(now);
        self.fresh_poll = false;
        self.logins_changed = false;
        if w.payload.b.is_some() || w.payload.m.is_some() || w.payload.x.is_some() || self.attention.pending() {
            self.attention.mark_sent();
        }
        if let Some(es) = w.payload.es {
            self.sent_seq = es;
        }
        if let Some(a) = w.payload.a {
            if self.sent_a != Some(a) {
                self.sent_a_at = now;
            }
            self.sent_a = Some(a);
        }
        if let Some(n) = w.payload.n {
            self.sent_n = Some(n);
            self.sent_sm = Some(w.payload.sm.clone());
        }
        if let Some(lv) = w.payload.lv {
            self.sent_lv = Some(lv);
        }
        if let Some(aa) = w.payload.aa {
            self.sent_aa = Some(aa);
        }
        if let Some(wk) = self.pending_wk.take() {
            self.sent_wk = Some(wk);
        }
        if let Some(th) = self.pending_th.take() {
            self.sent_th = Some(th);
        }
        if let Some(tl) = self.pending_tl.take() {
            self.sent_tl = Some(tl);
        }
        if let Some(wt) = self.pending_wt.take() {
            self.sent_wt = Some(wt);
        }
        if let Some(pm) = w.payload.pm {
            self.sent_pm = Some(pm);
        }
    }

    /// Snapshot for the UI.
    pub fn status(&self, now: i64) -> EngineStatus {
        let st = self.active_state();
        let active_dir = self.active_dir();
        let active_dirs = self.active.as_deref().map(|a| self.dirs_of(a)).unwrap_or_default();
        EngineStatus {
            active_account: active_dir.clone(),
            reading: st.and_then(|a| a.last.clone()),
            age_secs: st.and_then(|a| a.age(now)),
            stale: self.stale(now),
            logged_out: self.logged_out,
            errors: self
                .accounts
                .iter()
                .filter_map(|(k, a)| {
                    a.last_error.as_ref().map(|e| AccountError {
                        dir: k.clone(),
                        message: match a.backoff.until().filter(|u| *u > now) {
                            Some(until) if e.is_throttle() => {
                                crate::usage::paused_message(until, crate::platform::utc_offset_secs())
                            }
                            _ => e.user_message(a.backoff.remaining(now)),
                        },
                        retrying: e.is_transient(),
                    })
                })
                .collect(),
            attention: self.attention.active(),
            activity: self.activity.level(now),
            sessions: self.activity.sessions_active(now),
            hooks: self.activity.hooks_seen,
            mood: st.and_then(|a| a.last.as_ref()).map(|_| self.rate.mood(self.activity.level(now))),
            next_poll_at: self.next_due(now).map(|(t, _)| t),
            refresh_at: active_dir.as_ref().and_then(|a| self.refresh_at(a, now)),
            // The shown folder, or the first one while nothing is shown yet.
            paused_until: active_dir
                .as_ref()
                .or(self.settings.config_dirs.first())
                .and_then(|d| self.accounts.get(d))
                .filter(|a| a.last_error.as_ref().is_some_and(UsageError::is_throttle))
                .and_then(|a| a.backoff.until())
                .filter(|u| *u > now),
            polling: active_dirs.iter().any(|d| self.in_flight.contains(d)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::attention::AlertKind;
    use crate::config::{ConfigFile, Settings};
    use crate::events::HookKind;
    use crate::paths::Paths;
    use crate::usage::Window;
    use std::path::Path;
    use std::time::Duration;

    const T0: i64 = 1_760_000_000;

    fn settings(text: &str) -> Settings {
        Settings::from_config(&ConfigFile::parse(text), &Paths::under(Path::new("/h")), None)
    }

    fn reading(s: u32) -> UsageReading {
        UsageReading {
            session: Some(Window { pct: s, resets_at: Some(utc(T0 + 3600)) }),
            week: Some(Window { pct: 10, resets_at: Some(utc(T0 + 86_400)) }),
            ..Default::default()
        }
    }

    fn ev(ts: i64, kind: &str) -> HookEvent {
        HookEvent { ts, kind: HookKind::parse(kind), session: "s1".into(), account: None }
    }

    fn write(e: &mut Engine, now: i64) -> Option<Write> {
        let w = e.next_write(now, HostClock::default())?;
        e.mark_written(now, &w);
        Some(w)
    }

    /// Log every watched folder without an email in as one named after it
    /// (`/h/.claude` -> `.claude@x`), in `config_dirs` order, so the numbers
    /// match the folders' places.
    fn name_dirs(e: &mut Engine) {
        for d in e.settings().config_dirs.clone() {
            if e.book().email_of(&d).is_none() {
                let email = format!("{}@x", d.file_name().unwrap().to_string_lossy());
                e.on_folder_email(&d, Some(&email), 0);
            }
        }
    }

    /// An engine whose folders are logged in (see [`name_dirs`]).
    fn engine(text: &str) -> Engine {
        let mut e = Engine::new(settings(text));
        name_dirs(&mut e);
        e
    }

    // A `/login` switch while a poll is out: the result belongs to whoever
    // was logged in when it started.
    #[test]
    fn late_poll_after_switch_keeps_old_email() {
        let mut e = Engine::new(settings(""));
        let a = PathBuf::from("/h/.claude");
        e.on_folder_email(&a, Some("m@x"), T0);
        let plan = e.begin_poll(T0).unwrap();
        assert_eq!((plan.accounts.clone(), plan.emails.clone()), (vec![a.clone()], vec!["m@x".to_owned()]));
        e.on_folder_email(&a, Some("l@x"), T0 + 5);
        e.on_poll_results(T0 + 10, vec![(a.clone(), Ok(reading(40)))]);
        assert_eq!(e.book().saved("m@x").map(|s| s.session_pct), Some(40));
        assert_eq!(e.book().saved("l@x"), None);
        assert!(e.accounts.get(&a).is_none_or(|s| s.last.is_none()), "not relabelled");
        // The new login has nothing to show yet.
        assert!(e.next_write(T0 + 10, HostClock::default()).is_none_or(|w| w.payload.ai.is_none()));
    }

    // Two folders logged in to one email are one account; the newest
    // reading wins.
    #[test]
    fn same_email_two_folders_is_one_account() {
        let mut e = Engine::new(settings("config_dirs = ~/.claude, ~/.claude-2\n"));
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-2"));
        e.on_folder_email(&a, Some("m@x"), T0);
        e.on_folder_email(&b, Some("m@x"), T0);
        let plan = e.begin_poll(T0).unwrap();
        assert_eq!(plan.accounts.len(), 1, "one probe per account per cycle");
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(30)))]);
        e.on_poll_results(T0 + 10, vec![(b.clone(), Ok(reading(35)))]);
        let w = write(&mut e, T0 + 10).unwrap();
        assert_eq!(w.payload.s, Some(35));
        assert!(w.payload.ac.is_none_or(|n| n == 1));
        assert_eq!(w.payload.o, None);
    }

    // After a restart with nobody logged in anywhere: nothing is shown as
    // live; both accounts ride in `o`, dimmed, with a passed reset as not
    // started.
    #[test]
    fn all_offline_after_restart() {
        let mut book = crate::identity::AccountBook::new(vec!["m@x".into(), "l@x".into()]);
        book.save_reading("m@x", &reading(40), T0 - 100);
        let expired = UsageReading {
            session: Some(Window { pct: 70, resets_at: Some(utc(T0 - 10)) }),
            week: Some(Window { pct: 20, resets_at: Some(utc(T0 + 86_400)) }),
            ..Default::default()
        };
        book.save_reading("l@x", &expired, T0 - 5000);
        let mut e = Engine::new(settings("accounts = m@x, l@x\n"));
        *e.book_mut() = book;
        assert!(e.begin_poll(T0).is_none(), "no folder has a login");
        let w = write(&mut e, T0).unwrap();
        assert!(!w.payload.ok);
        assert_eq!((w.payload.ai, w.payload.s, w.payload.ac), (None, None, Some(2)));
        let o = w.payload.o.unwrap();
        assert_eq!(o.len(), 2);
        assert!(o.iter().all(|x| x.off == Some(1)));
        let m = o.iter().find(|x| x.i == 1).unwrap();
        assert_eq!((m.s, m.sr, m.w), (40, 60, 10));
        let l = o.iter().find(|x| x.i == 2).unwrap();
        assert_eq!((l.s, l.sr, l.w), (0, -1, 20), "session reset passed: not started");
    }

    // Credentials but no email yet (right after /login): not polled, its
    // events count for nobody.
    #[test]
    fn unknown_who_is_not_polled() {
        let mut e = Engine::new(settings("config_dirs = ~/.claude, ~/.claude-2\n"));
        e.set_hooks_installed(true);
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-2"));
        e.on_folder_email(&a, Some("m@x"), T0);
        e.on_folder_email(&b, None, T0);
        assert_eq!(e.begin_poll(T0).unwrap().accounts, vec![a.clone()]);
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(20)))]);
        write(&mut e, T0).unwrap();
        e.on_hook_event(&HookEvent { account: Some(b.clone()), ..ev(T0 + 5, "UserPromptSubmit") }, T0 + 5);
        // Dropped before the activity signal: nothing to tell the device.
        assert!(write(&mut e, T0 + 5).is_none());
        assert!(e.next_due(T0 + 5).is_some_and(|(t, _)| t > T0 + 5), "the unknown folder is never due");
        // Nor does it reach the activity signal (no prompt, no "last event").
        assert_eq!(e.last_event_ts, None);
    }

    // Numbers come from the account table, not the folder's place.
    #[test]
    fn second_account_by_email() {
        let mut e = Engine::new(settings("accounts = m@x, l@x\n"));
        let a = PathBuf::from("/h/.claude");
        e.on_folder_email(&a, Some("l@x"), T0);
        e.on_poll_results(T0, vec![(a, Ok(reading(20)))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!((w.payload.ai, w.payload.s), (Some(2), Some(20)));
    }

    #[test]
    fn nothing_before_first_poll_then_poll_write() {
        let mut e = engine("");
        assert_eq!(e.poll_due(T0), Some(PollReason::Startup));
        assert!(write(&mut e, T0).is_none());
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!(w.kind, WriteKind::Poll);
        assert_eq!(
            w.payload.to_json(),
            r#"{"s":15,"sr":60,"w":10,"wr":1440,"st":"allowed","acct":"pro","ok":true,"lv":3,"aa":120,"pm":0}"#
        );
        assert!(write(&mut e, T0 + 1).is_none());
    }

    // Heartbeat replays (bash parity + rp marker), and none once stale.
    #[test]
    fn heartbeat_replays_with_aged_countdowns() {
        let mut e = engine("reset_chime = on\n");
        let dir = PathBuf::from("/h/.claude");
        e.on_poll_results(T0, vec![(dir.clone(), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        assert!(write(&mut e, T0 + 59).is_none());
        let hb = write(&mut e, T0 + 300).unwrap();
        assert_eq!(hb.kind, WriteKind::Heartbeat);
        assert_eq!(
            hb.payload.to_json(),
            r#"{"s":15,"sr":55,"w":10,"wr":1435,"st":"allowed","acct":"pro","c":1,"ok":true,"rp":1,"lv":3,"aa":120,"pm":0}"#
        );
        // Three misses: stale, heartbeats stop so the device's freshness lapses.
        for i in 1..=3 {
            e.on_poll_results(T0 + 300 + i, vec![(dir.clone(), Err(UsageError::Timeout))]);
        }
        assert!(e.stale(T0 + 304));
        assert!(write(&mut e, T0 + 1000).is_none());
        assert_eq!(e.status(T0 + 1000).reading, Some(reading(15)), "last values kept");
    }

    // clawdmeter2: with two accounts every write names the active one and
    // carries the other's numbers for the device's swipe.
    #[test]
    fn two_accounts_ride_along_for_the_swipe() {
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        let a = PathBuf::from("/h/.claude");
        let b = PathBuf::from("/h/.claude-work");
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(10))), (b.clone(), Ok(reading(30)))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!((w.payload.s, w.payload.ai, w.payload.ac), (Some(30), Some(2), Some(2)));
        assert_eq!(
            w.payload.o,
            Some(vec![OtherAccount {
                i: 1,
                s: 10,
                sr: 60,
                w: 10,
                wr: 1440,
                wk: None,
                th: None,
                wt: None,
                wd: None,
                off: None
            }])
        );
        // One account: none of it.
        let mut one = engine("");
        one.on_poll_results(T0, vec![(a, Ok(reading(10)))]);
        let w = write(&mut one, T0).unwrap();
        assert_eq!((w.payload.ai, w.payload.ac, w.payload.o), (None, None, None));
    }

    // clawdmeter2: a prompt in the other account shows it at once, before
    // its usage moves; one with no reading yet, or no account, changes nothing.
    #[test]
    fn a_prompt_shows_its_account_right_away() {
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        let a = PathBuf::from("/h/.claude");
        let b = PathBuf::from("/h/.claude-work");
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(30))), (b.clone(), Ok(reading(10)))]);
        assert_eq!(e.status(T0).active_account, Some(a.clone()));
        e.on_hook_event(&ev(T0 + 5, "UserPromptSubmit"), T0 + 5);
        assert_eq!(e.status(T0 + 5).active_account, Some(a.clone()), "no account named");
        e.on_hook_event(&HookEvent { account: Some(b.clone()), ..ev(T0 + 10, "UserPromptSubmit") }, T0 + 10);
        assert_eq!(e.status(T0 + 10).active_account, Some(b.clone()));
        let w = write(&mut e, T0 + 10).unwrap();
        assert_eq!((w.payload.ai, w.payload.s), (Some(2), Some(10)));
        // The next poll keeps it while nothing else moves.
        e.on_poll_results(T0 + 200, vec![(a.clone(), Ok(reading(30))), (b.clone(), Ok(reading(10)))]);
        assert_eq!(e.status(T0 + 200).active_account, Some(b));
    }

    // clawdmeter2: the recording light per account, on and off right away.
    #[test]
    fn working_light_per_account() {
        use crate::working::WORKING_FOR;
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        let a = PathBuf::from("/h/.claude");
        let b = PathBuf::from("/h/.claude-work");
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(30))), (b.clone(), Ok(reading(10)))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!((w.payload.wk, w.payload.th), (None, None), "no hooks seen: no light");
        e.on_hook_event(&HookEvent { account: Some(b.clone()), ..ev(T0 + 10, "UserPromptSubmit") }, T0 + 10);
        let w = write(&mut e, T0 + 10).unwrap();
        assert_eq!((w.payload.ai, w.payload.wk), (Some(2), Some(1)));
        assert_eq!(w.payload.o.as_ref().unwrap()[0].wk, Some(0));
        // No transcript here: a prompt means thinking, and never with wk 0.
        assert_eq!((w.payload.th, w.payload.o.as_ref().unwrap()[0].th), (Some(1), Some(0)));
        e.on_hook_event(&HookEvent { account: Some(b.clone()), ..ev(T0 + 20, "Stop") }, T0 + 20);
        let w = write(&mut e, T0 + 20).unwrap();
        assert_eq!((w.payload.wk, w.payload.th), (Some(0), Some(0)));
        // A prompt in #1, then quiet: the light goes out by itself.
        e.on_hook_event(&HookEvent { account: Some(a.clone()), ..ev(T0 + 30, "UserPromptSubmit") }, T0 + 30);
        assert_eq!(write(&mut e, T0 + 30).unwrap().payload.wk, Some(1));
        let w = write(&mut e, T0 + 30 + WORKING_FOR).unwrap();
        assert_eq!((w.kind, w.payload.wk), (WriteKind::Event, Some(0)));
    }

    #[test]
    fn live_status_line_readings_are_fresh_polls() {
        use crate::live::{LiveUsage, LiveWindow};
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        let a = PathBuf::from("/h/.claude");
        let b = PathBuf::from("/h/.claude-work");
        e.on_poll_results(T0, vec![(a, Ok(reading(10))), (b.clone(), Ok(reading(30)))]);
        write(&mut e, T0).unwrap();
        let live = LiveUsage {
            account: Some(2),
            five_hour: Some(LiveWindow { pct: 34.4, resets_at: T0 + 1800 }),
            seven_day: None,
            ts: T0 as u64,
        };
        e.on_live(".claude-work@x", &live, T0 + 20);
        let w = write(&mut e, T0 + 20).unwrap();
        assert_eq!(w.kind, WriteKind::Poll);
        assert_eq!((w.payload.s, w.payload.sr, w.payload.w), (Some(34), Some(30), Some(10)), "week kept");
        e.on_live("nobody@x", &live, T0 + 40); // no such account: ignored
        assert!(write(&mut e, T0 + 40).is_none());
    }

    #[test]
    fn multi_account_selection_and_glitch_keeps_choice() {
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        let a = PathBuf::from("/h/.claude");
        let b = PathBuf::from("/h/.claude-work");
        assert_eq!(e.begin_poll(T0).unwrap().accounts, vec![a.clone(), b.clone()]);
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(10))), (b.clone(), Ok(reading(30)))]);
        assert_eq!(e.status(T0).active_account, Some(b.clone()));
        e.on_poll_results(T0 + 60, vec![(a.clone(), Ok(reading(20))), (b.clone(), Ok(reading(30)))]);
        assert_eq!(e.status(T0).active_account, Some(a.clone()));
        // A misses: still shown with its last values.
        e.on_poll_results(T0 + 120, vec![(a.clone(), Err(UsageError::Timeout)), (b.clone(), Ok(reading(30)))]);
        assert_eq!(e.status(T0).active_account, Some(a.clone()));
        assert_eq!(e.status(T0).reading.unwrap().session_pct(), 20);
        // B tokenless/logged out doesn't matter while A works.
        e.on_poll_results(T0 + 180, vec![(a.clone(), Ok(reading(20))), (b, Err(UsageError::NotLoggedIn("x".into())))]);
        assert!(!e.status(T0 + 180).logged_out);
    }

    // A miss on the shown account is never a poll, even when another
    // account answered: its old values must not reach the rate math.
    #[test]
    fn miss_on_shown_account_is_not_a_poll() {
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        let a = PathBuf::from("/h/.claude");
        let b = PathBuf::from("/h/.claude-work");
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(20))), (b.clone(), Ok(reading(5)))]);
        assert_eq!(write(&mut e, T0).unwrap().kind, WriteKind::Poll);
        e.on_poll_results(T0 + 30, vec![(a.clone(), Err(UsageError::Timeout)), (b, Ok(reading(5)))]);
        assert_eq!(e.status(T0 + 30).active_account, Some(a));
        assert!(write(&mut e, T0 + 30).is_none(), "no poll write for the shown account's old values");
        // The keep-alive still carries them, marked as a replay.
        assert_eq!(write(&mut e, T0 + 60).unwrap().payload.rp, Some(1));
    }

    // Stale: hook events stop refreshing the device, so its freshness lapses.
    #[test]
    fn stale_suppresses_event_writes() {
        let mut e = engine("");
        let dir = PathBuf::from("/h/.claude");
        e.on_poll_results(T0, vec![(dir.clone(), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        for i in 1..=3 {
            e.on_poll_results(T0 + i, vec![(dir.clone(), Err(UsageError::Timeout))]);
        }
        e.on_hook_event(&ev(T0 + 10, "UserPromptSubmit"), T0 + 10);
        assert!(write(&mut e, T0 + 10).is_none());
    }

    #[test]
    fn all_logged_out_sends_no_data() {
        let mut e = engine("");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Err(UsageError::NotLoggedIn("x".into())))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!((w.kind, w.payload.to_json().as_str()), (WriteKind::NoData, r#"{"ok":false}"#));
        assert!(write(&mut e, T0 + 100).is_none());
    }

    // Ported from test_windows_reconnect.py::test_transient_poll_failure_does_not_set_error
    // and test_freeride.py: only a login problem on every account is "no
    // data"; a glitch (timeout, bad output, CLI missing) stays silent.
    #[test]
    fn transient_failures_are_not_no_data() {
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-work"));
        e.on_poll_results(
            T0,
            vec![(a.clone(), Err(UsageError::Timeout)), (b.clone(), Err(UsageError::Spawn("x".into())))],
        );
        assert!(!e.status(T0).logged_out);
        assert!(write(&mut e, T0).is_none());
        // One logged out, one glitching: still not "no data".
        e.on_poll_results(
            T0 + 30,
            vec![(a.clone(), Err(UsageError::NotLoggedIn("x".into()))), (b.clone(), Err(UsageError::Timeout))],
        );
        assert!(write(&mut e, T0 + 30).is_none());
        // Both need a login: no data, once.
        e.on_poll_results(
            T0 + 60,
            vec![(a, Err(UsageError::NotSubscription)), (b, Err(UsageError::NotLoggedIn("x".into())))],
        );
        assert_eq!(write(&mut e, T0 + 60).unwrap().kind, WriteKind::NoData);
    }

    // A spend-capped account drives the device's enterprise layout and the
    // multi-account choice by its spend %.
    #[test]
    fn spend_only_account() {
        use crate::usage::Spend;
        let mut e = engine("");
        let r = UsageReading {
            spend: Some(Spend { pct: 41, resets_at: Some(utc(T0 + 86_400)), period: None }),
            ..Default::default()
        };
        assert_eq!(r.session_pct(), 41);
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(r))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!((w.payload.acct.as_deref(), w.payload.s, w.payload.pd), (Some("ent"), Some(41), Some(30)));
    }

    #[test]
    fn attention_rides_a_replay() {
        let mut e = engine("alert_chime = on\n");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        let alert = Alert {
            message: "Claude needs you".into(),
            session: "abc".into(),
            kind: AlertKind::Permission,
            at: SystemTime::now(),
        };
        e.on_attention(Some(alert.clone()), &HashMap::new());
        let w = write(&mut e, T0 + 2).unwrap();
        assert_eq!(w.kind, WriteKind::Attention);
        assert_eq!((w.payload.b, w.payload.m.as_deref(), w.payload.rp), (Some(1), Some("Permission needed!"), Some(1)));
        assert!(write(&mut e, T0 + 3).is_none());
        let clears: HashMap<_, _> = [("abc".to_owned(), alert.at + Duration::from_secs(1))].into();
        e.on_attention(None, &clears);
        let w = write(&mut e, T0 + 4).unwrap();
        assert_eq!((w.payload.x, w.payload.m.clone()), (Some(1), None));
    }

    #[test]
    fn stuck_alerts_end_by_expiry_or_device_dismissal() {
        let mut e = engine("");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        let at = SystemTime::now();
        let alert = |s: &str| Alert { message: "m".into(), session: s.into(), kind: AlertKind::Permission, at };

        // A session killed before its clear: expires with an x=1.
        e.on_attention(Some(alert("gone")), &HashMap::new());
        write(&mut e, T0 + 2).unwrap();
        assert!(!e.expire_attention(at + Duration::from_secs(60)));
        assert!(e.expire_attention(at + crate::attention::ALERT_TTL));
        assert!(!e.status(T0).attention);
        assert_eq!(write(&mut e, T0 + 3).unwrap().payload.x, Some(1));

        // Tapped away on the device: gone here too, nothing sent back.
        e.on_attention(Some(alert(crate::attention::TEST_SESSION)), &HashMap::new());
        write(&mut e, T0 + 4).unwrap();
        e.on_device_dismissed();
        assert!(!e.status(T0).attention);
        assert!(write(&mut e, T0 + 5).is_none());
    }

    #[test]
    fn the_desktop_chime_follows_new_alerts() {
        let at = SystemTime::now();
        let alert = |s: &str, kind| Alert { message: "m".into(), session: s.into(), kind, at };
        let mut e = engine("");
        e.on_attention(Some(alert("a", AlertKind::Permission)), &HashMap::new());
        assert!(!e.take_desktop_chime(), "off by default");
        let mut e = engine("desktop_chime = on\n");
        e.on_attention(Some(alert("a", AlertKind::Permission)), &HashMap::new());
        assert!(e.take_desktop_chime());
        assert!(!e.take_desktop_chime(), "once per alert");
        // The same session again (AskUserQuestion notifies twice): no repeat.
        e.on_attention(Some(alert("a", AlertKind::Question)), &HashMap::new());
        assert!(!e.take_desktop_chime());
        // A kind that's turned off never chimes.
        e.on_attention(Some(alert("b", AlertKind::Idle)), &HashMap::new());
        assert!(!e.take_desktop_chime());
        // Cleared in the same tick it arrived: nothing to announce.
        let clears: HashMap<_, _> = [("c".to_owned(), at + Duration::from_secs(1))].into();
        e.on_attention(Some(alert("c", AlertKind::Permission)), &clears);
        assert!(!e.take_desktop_chime());
        // The test alert chimes, so the setting can be tried.
        e.on_attention(Some(alert(crate::attention::TEST_SESSION, AlertKind::Permission)), &HashMap::new());
        assert!(e.take_desktop_chime());
    }

    #[test]
    fn disabled_alert_kinds_never_alert() {
        let mut e = engine("alert_chime = on\n");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        let idle =
            Alert { message: "waiting".into(), session: "s".into(), kind: AlertKind::Idle, at: SystemTime::now() };
        e.on_attention(Some(idle.clone()), &HashMap::new());
        assert!(!e.status(T0).attention, "idle is off by default");
        assert!(write(&mut e, T0 + 2).is_none());
        e.update_settings(settings("alert_chime = on\nalert_on = idle\n"));
        e.on_attention(Some(idle), &HashMap::new());
        assert!(e.status(T0).attention);
    }

    #[test]
    fn hook_events_reach_the_device() {
        let mut e = engine("");
        e.set_hooks_installed(true);
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!(w.payload.a, None, "no hooks seen yet");

        e.on_hook_event(&ev(T0 + 10, "UserPromptSubmit"), T0 + 10);
        // The prompt's poll waits out the 2-minute gap since the last one.
        assert_eq!(e.poll_due(T0 + 10), None);
        assert_eq!(e.poll_due(T0 + 120), Some(PollReason::Prompt));
        let w = write(&mut e, T0 + 10).unwrap();
        assert_eq!(w.kind, WriteKind::Event, "the event goes out before the poll lands");
        assert_eq!(
            (w.payload.e.as_deref(), w.payload.es, w.payload.n, w.payload.a),
            (Some("p"), Some(1), Some(1), Some(0))
        );
        assert_eq!(w.payload.rp, Some(1));
        e.on_poll_results(T0 + 11, vec![(PathBuf::from("/h/.claude"), Ok(reading(16)))]);
        let w = write(&mut e, T0 + 11).unwrap();
        assert_eq!((w.kind, w.payload.rp, w.payload.s), (WriteKind::Poll, None, Some(16)));

        // Tool calls raise `a`, sent at most every 5 s.
        for i in 0..5 {
            e.on_hook_event(&ev(T0 + 11 + i, "PostToolUse"), T0 + 11 + i);
        }
        assert!(write(&mut e, T0 + 14).is_none());
        let w = write(&mut e, T0 + 15).unwrap();
        assert_eq!(w.payload.a, Some(2));
        // `e` keeps riding along but doesn't trigger writes by itself.
        assert_eq!(w.payload.es, Some(1));

        e.on_hook_event(&ev(T0 + 30, "Stop"), T0 + 30);
        let w = write(&mut e, T0 + 30).unwrap();
        assert_eq!((w.payload.e.as_deref(), w.payload.es), (Some("s"), Some(2)));
        assert_eq!(e.poll_due(T0 + 40), None, "polled at T0+11: gap until T0+131");
        assert_eq!(e.poll_due(T0 + 131), Some(PollReason::AfterStop));

        // Old events stop riding along after a minute.
        let w = e.next_write(T0 + 200, HostClock::default()).unwrap();
        assert_eq!((w.payload.e, w.payload.es), (None, None));
    }

    #[test]
    fn reconnect_resends() {
        let mut e = engine("");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        e.on_connected();
        assert_eq!(write(&mut e, T0 + 1).unwrap().kind, WriteKind::Poll);
    }

    #[test]
    fn settings_change_triggers_poll() {
        let mut e = engine("");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        assert_eq!(e.poll_due(T0 + 1), None);
        e.update_settings(settings("config_dirs = ~/.claude, ~/.w\n"));
        name_dirs(&mut e);
        assert_eq!(e.poll_due(T0 + 1), Some(PollReason::Startup));
        assert_eq!(e.begin_poll(T0 + 1).unwrap().accounts, vec![PathBuf::from("/h/.w")], "only the new account");
    }

    fn poll(e: &mut Engine, now: i64, answer: impl Fn(&Path) -> Result<UsageReading, UsageError>) -> Vec<PathBuf> {
        let Some(plan) = e.begin_poll(now) else { return Vec::new() };
        let results = plan.accounts.iter().map(|d| (d.clone(), answer(d))).collect();
        e.on_poll_results(now, results);
        plan.accounts
    }

    fn no_lines() -> Result<UsageReading, UsageError> {
        Err(UsageError::NoUsageLines("You are currently using your subscription".into()))
    }

    /// A reading with no known window (a session line without a reset), so
    /// `/usage` stays the source.
    fn usage_reading(s: u32) -> UsageReading {
        UsageReading { session: Some(Window { pct: s, resets_at: None }), ..reading(s) }
    }

    // The owner's case: the shown account's /usage comes back without its
    // lines (429). It keeps being shown with its last values, isn't polled
    // again for 10 minutes whatever asks, and the other account keeps its
    // own rhythm (12 min while no window is known).
    #[test]
    fn rate_limited_shown_account_backs_off_and_stays() {
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        e.set_hooks_installed(true);
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-work"));
        let pct = |d: &Path| if d == a { 40 } else { 10 };
        assert_eq!(poll(&mut e, T0, |d| Ok(usage_reading(pct(d)))), vec![a.clone(), b.clone()]);
        assert_eq!(e.status(T0).active_account, Some(a.clone()));
        e.request_refresh(T0 + 200);
        assert_eq!(poll(&mut e, T0 + 200, |d| if d == a { no_lines() } else { Ok(usage_reading(10)) }).len(), 2);
        let st = e.status(T0 + 200);
        assert_eq!(st.active_account, Some(a.clone()), "a rate limit doesn't flip the shown account");
        assert_eq!(st.reading.map(|r| r.session_pct()), Some(40));
        assert_eq!(
            st.errors,
            vec![AccountError {
                dir: a.clone(),
                message: "No usage data was returned. Trying again in about 10 minutes.".into(),
                retrying: true,
            }]
        );
        // A refresh request polls only the account that isn't backing off.
        e.request_refresh(T0 + 400);
        assert_eq!(poll(&mut e, T0 + 400, |_| Ok(usage_reading(10))), vec![b.clone()]);
        assert_eq!(e.status(T0 + 400).active_account, Some(a.clone()));
        e.on_hook_event(&ev(T0 + 450, "UserPromptSubmit"), T0 + 450);
        for t in [T0 + 450, T0 + 599, T0 + 799] {
            assert!(e.begin_poll(t).is_none(), "nothing due at {t}");
        }
        assert_eq!(e.status(T0 + 700).next_poll_at, Some(T0 + 800));
        assert_eq!(e.status(T0 + 700).refresh_at, Some(T0 + 800), "a refresh waits for the backoff too");
        // Second miss: 20 minutes.
        assert_eq!(poll(&mut e, T0 + 800, |_| Err(UsageError::RateLimited { retry_after_secs: 400 })), vec![a.clone()]);
        assert!(e.status(T0 + 800).errors[0].message.starts_with("Usage paused by Anthropic until "));
        assert_eq!(poll(&mut e, T0 + 1120, |_| Ok(usage_reading(10))), vec![b.clone()], "B every 12 minutes");
        assert!(e.begin_poll(T0 + 1999).is_none_or(|p| !p.accounts.contains(&a)));
        // Back: errors clear, and the device gets a poll write.
        let polled = poll(&mut e, T0 + 2000, |d| Ok(usage_reading(pct(d) + 5)));
        assert!(polled.contains(&a));
        assert!(e.status(T0 + 2000).errors.is_empty());
        assert_eq!(write(&mut e, T0 + 2000).unwrap().kind, WriteKind::Poll);
    }

    // Without a claude binary there's no probe: the usage endpoint answers
    // every 12 minutes, window or not.
    #[test]
    fn without_claude_the_endpoint_answers_on_its_cadence() {
        let mut e = engine("");
        e.set_hooks_installed(true);
        e.set_claude_found(false);
        let a = PathBuf::from("/h/.claude");
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(15)))]);
        let mine = HookEvent { account: Some(a.clone()), ..ev(T0 + 200, "UserPromptSubmit") };
        e.on_hook_event(&mine, T0 + 200);
        assert!(e.begin_poll(T0 + 200).is_none(), "a prompt doesn't speed it up");
        assert_eq!(plans(&mut e, T0 + 720), vec![ProbePlan::Usage]);
    }

    // A block survives a restart: nothing calls the endpoint or /usage for
    // that account until it passes, not even a refresh request.
    #[test]
    fn a_saved_block_holds_after_a_restart() {
        let a = PathBuf::from("/h/.claude");
        let mut e = engine("");
        e.on_poll_results(T0, vec![(a.clone(), Err(UsageError::Throttled { retry_after_secs: Some(3600) }))]);
        let saved = e.blocks(T0 + 10);
        assert_eq!(saved.get(&a).map(|b| b.until), Some(T0 + 3600));
        let mut fresh = engine("");
        fresh.restore_blocks(&saved, T0 + 100);
        assert!(fresh.begin_poll(T0 + 100).is_none(), "start-up waits for the block");
        fresh.request_refresh(T0 + 200);
        assert!(fresh.begin_poll(T0 + 200).is_none(), "so does Refresh now");
        assert!(fresh.status(T0 + 200).errors[0].message.starts_with("Usage paused by Anthropic until "));
        assert_eq!(fresh.status(T0 + 200).paused_until, Some(T0 + 3600));
        assert_eq!(fresh.begin_poll(T0 + 3600).map(|p| p.plans), Some(vec![ProbePlan::Usage]));
        // Expired blocks aren't restored.
        let mut later = engine("");
        later.restore_blocks(&saved, T0 + 4000);
        assert!(later.blocks(T0 + 4000).is_empty());
    }

    fn plans(e: &mut Engine, now: i64) -> Vec<ProbePlan> {
        e.begin_poll(now).map(|p| p.plans).unwrap_or_default()
    }

    // The probe runs only while a window is known to be running, isn't held
    // up by /usage's backoff, and a window that ends quietly needs no call.
    #[test]
    fn the_probe_never_starts_a_window() {
        let mut e = engine("");
        e.set_hooks_installed(true);
        let a = PathBuf::from("/h/.claude");
        // Unknown at first: /usage.
        assert_eq!(plans(&mut e, T0), vec![ProbePlan::Usage]);
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(15)))]); // resets at T0 + 3600
                                                                   // Known window: the probe, on the working cadence after a prompt.
        let mine = |ts| HookEvent { account: Some(a.clone()), ..ev(ts, "UserPromptSubmit") };
        e.on_hook_event(&mine(T0 + 200), T0 + 200);
        assert_eq!(plans(&mut e, T0 + 200), vec![ProbePlan::Probe { usage_fallback: true }]);
        // A probe miss with /usage backing off: the probe still runs, alone.
        e.on_poll_results(T0 + 200, vec![(a.clone(), no_lines())]);
        e.on_hook_event(&mine(T0 + 400), T0 + 400);
        assert_eq!(plans(&mut e, T0 + 400), vec![ProbePlan::Probe { usage_fallback: false }]);
        e.on_poll_results(T0 + 400, vec![(a.clone(), Ok(reading(20)))]);
        // Inside the margin before the reset: /usage, which can't start one.
        assert_eq!(plans(&mut e, T0 + 3400), vec![ProbePlan::Usage]);
        e.on_poll_results(T0 + 3400, vec![(a.clone(), Ok(reading(20)))]);
        // At the reset, with nothing since: over, no call.
        assert_eq!(e.status(T0 + 3500).next_poll_at, Some(T0 + 3600), "the reset wakes it");
        let Some(PollPlan { reason, plans: due, .. }) = e.begin_poll(T0 + 3600) else { panic!() };
        assert_eq!(reason, PollReason::WindowEnd);
        let [ProbePlan::Known(r)] = due.as_slice() else { panic!("{due:?}") };
        assert_eq!(r.session, None);
        assert_eq!(r.week, reading(20).week, "weekly kept until its own reset");
        e.on_poll_results(T0 + 3600, vec![(a.clone(), Ok(r.clone()))]);
        assert_eq!(e.status(T0 + 3600).reading.map(|r| r.session_pct()), Some(0));
        // Now /usage keeps watch every 12 min; another account's (or an
        // unknown) prompt doesn't open the probe.
        e.on_hook_event(&ev(T0 + 3700, "UserPromptSubmit"), T0 + 3700);
        assert!(e.begin_poll(T0 + 3700).is_none());
        assert_eq!(plans(&mut e, T0 + 3600 + 720), vec![ProbePlan::Usage]);
        e.on_poll_results(T0 + 4320, vec![(a.clone(), Ok(usage_reading(0)))]);
        // The user's own prompt starts a window: the probe, at once.
        e.on_hook_event(&mine(T0 + 4500), T0 + 4500);
        assert_eq!(plans(&mut e, T0 + 4500), vec![ProbePlan::Probe { usage_fallback: true }]);
    }

    // Only a 429 is saved as a block (and called a pause by Anthropic); a
    // timeout or odd output backs off in memory only.
    #[test]
    fn only_throttles_are_saved_as_blocks() {
        let mut e = engine("config_dirs = ~/.claude, ~/.w\n");
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.w"));
        poll(&mut e, T0, |d| {
            if d == a {
                Err(UsageError::Throttled { retry_after_secs: Some(900) })
            } else {
                Err(UsageError::Timeout)
            }
        });
        let blocks = e.blocks(T0 + 1);
        assert_eq!(blocks.keys().collect::<Vec<_>>(), vec![&a]);
        let errors = e.status(T0 + 1).errors;
        let msg = |d: &PathBuf| errors.iter().find(|x| &x.dir == d).unwrap().message.clone();
        assert!(msg(&a).starts_with("Usage paused by Anthropic until "));
        assert!(!msg(&b).contains("paused"), "{}", msg(&b));
    }

    // An account with no reading at all can't be shown; the other one is.
    #[test]
    fn account_without_data_is_not_shown() {
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-work"));
        poll(&mut e, T0, |d| if d == a { no_lines() } else { Ok(reading(5)) });
        assert_eq!(e.status(T0).active_account, Some(b));
        assert!(!e.status(T0).logged_out);
        let _ = a;
    }

    // Logged out is about every account's latest answer, not just the ones
    // polled this time.
    #[test]
    fn logged_out_needs_every_account() {
        let mut e = engine("config_dirs = ~/.claude, ~/.claude-work\n");
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-work"));
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(5))), (b.clone(), Ok(reading(3)))]);
        e.on_poll_results(T0 + 600, vec![(b.clone(), Err(UsageError::NotLoggedIn("x".into())))]);
        assert!(!e.status(T0 + 600).logged_out);
        let errs = e.status(T0 + 600).errors;
        assert_eq!((errs.len(), errs[0].retrying), (1, false));
        e.on_poll_results(T0 + 700, vec![(a, Err(UsageError::NotLoggedIn("x".into())))]);
        assert!(e.status(T0 + 700).logged_out);
    }

    #[test]
    fn liveliness_goes_out_with_the_data_and_when_it_changes() {
        let mut e = engine("");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!(w.payload.lv, Some(3), "every data payload carries it");
        assert!(write(&mut e, T0 + 1).is_none(), "nothing new");
        e.update_settings(settings("liveliness = 1\n"));
        let w = write(&mut e, T0 + 2).expect("a new liveliness is sent right away");
        assert_eq!((w.kind, w.payload.lv, w.payload.rp), (WriteKind::Event, Some(1), Some(1)));
        assert!(write(&mut e, T0 + 3).is_none());
        e.on_connected();
        assert_eq!(write(&mut e, T0 + 4).unwrap().payload.lv, Some(1), "resent on a reconnect");
    }

    #[test]
    fn attention_animation_goes_out_with_the_data_and_when_it_changes() {
        let mut e = engine("");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        assert_eq!(write(&mut e, T0).unwrap().payload.aa, Some(120), "every data payload carries it");
        assert!(write(&mut e, T0 + 1).is_none(), "nothing new");
        e.update_settings(settings("attention_anim = off\n"));
        let w = write(&mut e, T0 + 2).expect("a new setting is sent right away");
        assert_eq!((w.kind, w.payload.aa, w.payload.rp), (WriteKind::Event, Some(0), Some(1)));
        assert!(write(&mut e, T0 + 3).is_none());
        e.on_connected();
        assert_eq!(write(&mut e, T0 + 4).unwrap().payload.aa, Some(0), "resent on a reconnect");
    }

    #[test]
    fn pace_marker_goes_out_with_the_data_and_when_it_changes() {
        let mut e = engine("");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        assert_eq!(write(&mut e, T0).unwrap().payload.pm, Some(0), "every data payload carries it");
        assert!(write(&mut e, T0 + 1).is_none(), "nothing new");
        e.update_settings(settings("pace_marker = on\n"));
        let w = write(&mut e, T0 + 2).expect("a new setting is sent right away");
        assert_eq!((w.kind, w.payload.pm, w.payload.rp), (WriteKind::Event, Some(1), Some(1)));
        assert!(write(&mut e, T0 + 3).is_none());
        e.on_connected();
        assert_eq!(write(&mut e, T0 + 4).unwrap().payload.pm, Some(1), "resent on a reconnect");
    }

    #[test]
    fn clock_fields_included() {
        let mut e = engine("clock = 12\n");
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        let w = e.next_write(T0, HostClock { utc_offset_secs: -3600, prefers_12h: false }).unwrap();
        assert_eq!((w.payload.t, w.payload.tf), (Some(T0 - 3600), Some(12)));
    }
}
