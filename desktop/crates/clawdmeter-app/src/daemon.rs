//! The daemon runtime: drives [`clawdmeter_core::Engine`] with real I/O
//! (the `claude` CLI, events.log, attention files, the BLE link).
//!
//! It runs on its own thread with a current-thread tokio runtime, so the GUI
//! event loop never shares a reactor with it. The UI talks to it through a
//! [`DaemonHandle`]: commands in, a [`Status`] snapshot out.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use clawdmeter_ble::{
    should_wake, AddressCache, Bluetooth, DeviceEvent, DeviceEvents, DeviceLink, LinkError, LinkOptions, Reconnect,
    Sighting, MAX_WRITE,
};
use clawdmeter_core::attention::AttentionInbox;
use clawdmeter_core::backoff;
use clawdmeter_core::engine::PollPlan;
use clawdmeter_core::events::{EventTail, HookKind};
use clawdmeter_core::identity::{logged_in_email, state_file, AccountBook};
use clawdmeter_core::probe;
use clawdmeter_core::usage;
use clawdmeter_core::{unix_now, Engine, EngineStatus, HostClock, Paths, Settings, WriteKind};
use tokio::sync::{mpsc, watch};
use tokio::task::JoinHandle;

/// Loop tick: events reach the device within about this long.
const TICK: Duration = Duration::from_millis(500);
/// Config re-read interval (s).
const SETTINGS_EVERY: i64 = 5;
/// Connection check interval (s).
const LINK_CHECK_EVERY: i64 = 5;
/// clawdmeter2: how often transcripts are rescanned for the history screens.
const HISTORY_EVERY: i64 = 600;

/// Modification times of the two files that say who is logged in to a folder.
type FolderStamp = (Option<SystemTime>, Option<SystemTime>);

fn folder_stamp(dir: &Path, home: &Path) -> FolderStamp {
    let m = |p: PathBuf| std::fs::metadata(p).and_then(|m| m.modified()).ok();
    (m(state_file(dir, home)), m(dir.join(".credentials.json")))
}

/// Emails in `known` that `now` no longer lists: the user forgot them.
fn forgotten(known: &[String], now: &[String]) -> Vec<String> {
    known.iter().filter(|e| !now.contains(e)).cloned().collect()
}

/// Requests from the UI.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    /// Poll `/usage` now.
    RefreshNow,
    /// Drop the link and look for the device again (e.g. after pairing).
    Reconnect,
    /// Forget the cached device address, then look again (after swapping boards).
    Forget,
    /// Mute the alert chimes (device and this computer) for this run,
    /// whatever the config says.
    SetChimesMuted(bool),
    /// Stop the daemon thread.
    Shutdown,
}

/// The BLE side, for the tray's status dot.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub enum LinkState {
    /// Not tried yet.
    #[default]
    Starting,
    /// No adapter, or Bluetooth is off.
    NoAdapter(String),
    /// No paired device found; retrying.
    Searching,
    /// Connecting to a known device.
    Connecting,
    /// Connected.
    Connected {
        /// Cache key (address).
        key: String,
        /// Device name.
        name: Option<String>,
    },
}

/// Everything the tray needs, updated every tick.
#[derive(Debug, Clone, Default)]
pub struct Status {
    /// BLE link.
    pub link: LinkState,
    /// Usage / hooks / attention.
    pub engine: EngineStatus,
    /// Unix seconds of the last successful device write.
    pub last_write_at: Option<i64>,
    /// `claude` CLI path in use (None = not found).
    pub claude: Option<PathBuf>,
}

/// Handle held by the UI.
#[derive(Debug)]
pub struct DaemonHandle {
    /// Send commands.
    pub commands: mpsc::UnboundedSender<Command>,
    /// Watch status.
    pub status: watch::Receiver<Status>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl DaemonHandle {
    /// Ask the daemon to stop and wait for it.
    pub fn shutdown(mut self) {
        let _ = self.commands.send(Command::Shutdown);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

/// Start the daemon on a background thread.
pub fn spawn(paths: Paths) -> DaemonHandle {
    let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
    let (st_tx, st_rx) = watch::channel(Status::default());
    let thread = std::thread::Builder::new()
        .name("clawdmeter-daemon".into())
        .spawn(move || {
            let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio runtime");
            rt.block_on(run(paths, cmd_rx, st_tx));
        })
        .expect("spawn daemon thread");
    DaemonHandle { commands: cmd_tx, status: st_rx, thread: Some(thread) }
}

type PollResults = Vec<(PathBuf, probe::Polled)>;
type ConnectResult = Result<(DeviceLink, DeviceEvents), LinkError>;

/// Read each account of a poll plan: the probe, `/usage`, or a reading known
/// without a call. The probe runs from `cwd` (the state dir).
fn poll_all(claude: Option<PathBuf>, plan: PollPlan, home: PathBuf, cwd: PathBuf) -> PollResults {
    let now = chrono::Utc::now();
    plan.accounts
        .into_iter()
        .zip(plan.plans)
        .map(|(d, p)| {
            let r = probe::poll(claude.as_deref(), &d, &home, &cwd, &p, now);
            (d, r)
        })
        .collect()
}

/// The daemon loop. Returns on [`Command::Shutdown`] or when every command
/// sender is dropped.
pub async fn run(paths: Paths, mut commands: mpsc::UnboundedReceiver<Command>, status: watch::Sender<Status>) {
    let settings = Settings::load(&paths).unwrap_or_else(|e| {
        log::warn!("config unreadable ({e}); using defaults");
        Settings::from_config(&Default::default(), &paths, None)
    });
    log::info!(
        "watching: {}",
        settings.config_dirs.iter().map(|d| d.display().to_string()).collect::<Vec<_>>().join(", ")
    );
    let mut engine = Engine::new(settings);
    // The persisted book brings back logged-out accounts and their last readings.
    match AccountBook::load(&paths.accounts_file()) {
        Ok(b) => *engine.book_mut() = b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => log::warn!("accounts.json unreadable ({e}); starting from the config"),
    }
    // Emails of the table last written to / read from the config.
    let mut synced_accounts: Vec<String> = engine.book().emails().to_vec();
    let mut saved_book = serde_json::to_vec(engine.book()).unwrap_or_default();
    let mut folder_stamps: HashMap<PathBuf, FolderStamp> = HashMap::new();
    // Blocks the usage endpoint set survive restarts (a call during one
    // lengthens it).
    engine.restore_blocks(&backoff::load_blocks(&paths.usage_blocks_file()), unix_now());
    let mut saved_blocks = engine.blocks(unix_now());
    let host = HostClock { utc_offset_secs: 0, prefers_12h: clawdmeter_core::platform::prefers_12h() };
    let inbox = AttentionInbox::new(&paths.state_dir);
    // clawdmeter2: live usage from status lines (this PC's and relayed),
    // and the relay listener when it's configured (a restart applies changes).
    let mut live_inbox = clawdmeter_core::live::LiveInbox::new(&paths.state_dir);
    if let (Some(listen), Some(_)) = (&engine.settings().relay_listen, &engine.settings().relay_key) {
        crate::relay_server::spawn(listen.clone(), paths.clone());
    }
    let (mut tail, seed) = EventTail::open(paths.events_log());
    engine.seed_events(&seed);
    engine.set_hooks_installed(tail.exists());
    let mut turns_read_at = 0i64;
    let cache = AddressCache::new(paths.ble_address_file());
    let link_opts = LinkOptions::default();

    let mut claude = usage::find_claude(engine.settings().claude_path.as_deref(), &paths.home);
    match &claude {
        Some(c) => log::info!("claude CLI: {}", c.display()),
        None => log::warn!(
            "claude CLI not found (set claude_path in {}); usage comes from the usage endpoint every 12 min",
            paths.config_file.display()
        ),
    }
    engine.set_claude_found(claude.is_some());

    let mut link: Option<DeviceLink> = None;
    let mut device_events: Option<DeviceEvents> = None;
    let mut connecting: Option<JoinHandle<ConnectResult>> = None;
    let mut polling: Option<JoinHandle<PollResults>> = None;
    let mut link_state = LinkState::Starting;
    let mut reconnect = Reconnect::default();
    let bt = Bluetooth::default();
    let mut wakeups = clawdmeter_ble::spawn_watcher(bt.clone());
    let mut last_settings = unix_now();
    let mut last_link_check = unix_now();
    let mut last_write_at = None;
    let mut chimes_muted = false;
    // clawdmeter2: the 30-day chart and the week's tokens, per account. A
    // transcript scan is slow, so it runs off the loop every HISTORY_EVERY
    // (and on connect); only accounts whose numbers changed are re-sent, one
    // message per tick.
    let mut history_scan: Option<JoinHandle<Vec<clawdmeter_core::history::AccountHistory>>> = None;
    let mut last_history: i64 = 0;
    let mut history_queue: Vec<String> = Vec::new();
    let mut history_sent: std::collections::HashMap<u8, u64> = std::collections::HashMap::new();
    // clawdmeter2: permission requests answered on the device.
    let mut perm_bridge = crate::permission::Bridge::default();
    // Only the device paired in Windows may answer them (not a look-alike).
    let mut perm_trusted = false;
    let mut ticker = tokio::time::interval(TICK);
    ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    loop {
        ticker.tick().await;
        let now = unix_now();

        // ---- commands ----
        loop {
            match commands.try_recv() {
                Ok(Command::RefreshNow) => engine.request_refresh(now),
                Ok(cmd @ (Command::Reconnect | Command::Forget)) => {
                    if let Some(l) = link.take() {
                        l.close().await;
                    }
                    if cmd == Command::Forget {
                        if let Some(key) = cache.load() {
                            cache.invalidate(&key);
                        }
                    }
                    device_events = None;
                    link_state = LinkState::Searching;
                    reconnect.retry_in(now, 0);
                }
                Ok(Command::SetChimesMuted(m)) => {
                    chimes_muted = m;
                    let mut s = engine.settings().clone();
                    if m {
                        s.alert_chime = false;
                        s.desktop_chime = false;
                    } else if let Ok(fresh) = Settings::load(&paths) {
                        s = fresh;
                    }
                    engine.update_settings(s);
                }
                Ok(Command::Shutdown) | Err(mpsc::error::TryRecvError::Disconnected) => {
                    if let Some(l) = link.take() {
                        l.close().await;
                    }
                    log::info!("daemon stopped");
                    return;
                }
                Err(mpsc::error::TryRecvError::Empty) => break,
            }
        }

        // ---- config (re-read every SETTINGS_EVERY) ----
        if now - last_settings >= SETTINGS_EVERY {
            last_settings = now;
            if let Ok(mut s) = Settings::load(&paths) {
                if chimes_muted {
                    s.alert_chime = false;
                    s.desktop_chime = false;
                }
                if s.claude_path != engine.settings().claude_path || claude.is_none() {
                    claude = usage::find_claude(s.claude_path.as_deref(), &paths.home);
                }
                // Forget (settings page) removes an email from the list.
                if s.accounts_explicit {
                    for e in forgotten(&synced_accounts, &s.accounts) {
                        log::info!("account forgotten: {e}");
                        engine.book_mut().forget(&e);
                    }
                    synced_accounts.retain(|e| s.accounts.contains(e));
                }
                engine.update_settings(s);
                engine.set_claude_found(claude.is_some());
            }
        }

        // ---- who is logged in where ----
        // A folder is looked at when its login files change (and on the first tick).
        let mut changed_book = false;
        for d in engine.settings().config_dirs.clone() {
            let stamp = folder_stamp(&d, &paths.home);
            if folder_stamps.get(&d) == Some(&stamp) {
                continue;
            }
            folder_stamps.insert(d.clone(), stamp);
            let email = logged_in_email(&d, &paths.home);
            let before = engine.book().email_of(&d).map(str::to_owned);
            engine.on_folder_email(&d, email.as_deref(), now);
            if before != email {
                log::info!("{}: now {}", d.display(), email.as_deref().unwrap_or("logged out"));
                changed_book = true;
            }
        }
        // A newly seen email joined the table: write it to the config too.
        if changed_book && engine.book().emails() != synced_accounts.as_slice() {
            let added: Vec<String> =
                engine.book().emails().iter().filter(|e| !synced_accounts.contains(e)).cloned().collect();
            let res = crate::ui::settings::write_settings(&paths, |s, _| {
                for e in &added {
                    if !s.accounts.contains(e) {
                        s.accounts.push(e.clone());
                    }
                }
            });
            match res {
                Ok(()) => synced_accounts = engine.book().emails().to_vec(),
                Err(e) => log::warn!("couldn't add {} to the config: {e}", added.join(", ")),
            }
        }
        persist_book(engine.book(), &paths, &mut saved_book); // cheap compare; writes only on change

        // ---- fast clock ----
        for ev in tail.poll() {
            log::debug!("hook {:?} {}", ev.kind, ev.session);
            // The turn ended or you typed again: the request was answered there.
            // (Not PostToolUse: with parallel tool calls another tool finishing
            // says nothing about this one.)
            if matches!(ev.kind, HookKind::Stop | HookKind::UserPromptSubmit) {
                perm_bridge.session_moved_on(&paths.state_dir, &ev.session, ev.ts);
            }
            engine.on_hook_event(&ev, now);
        }
        engine.set_hooks_installed(tail.exists());
        let alert = inbox.take_alert(SystemTime::now());
        let clears = inbox.take_clears();
        if alert.is_some() || !clears.is_empty() {
            engine.on_attention(alert, &clears);
        }
        if engine.take_desktop_chime() {
            crate::chime::play();
        }
        if engine.expire_attention(SystemTime::now()) {
            log::info!("attention alert expired");
        }
        // Numbered files (the relay's) keep the table's numbering until the
        // remote end sends emails.
        let emails = engine.book().emails().to_vec();
        for (email, l) in live_inbox.take_emails(&emails, &emails, now as u64) {
            log::debug!("live usage received");
            engine.on_live(&email, &l, now);
        }

        // ---- slow clock ----
        if let Some(rx) = device_events.as_mut() {
            while let Ok(ev) = rx.try_recv() {
                match ev {
                    DeviceEvent::Refresh => {
                        log::info!("refresh requested by device");
                        engine.request_refresh(now);
                        // It asks after every (re)boot, and keeps history in RAM
                        // only: rescan and resend it now, not at the next scan.
                        history_sent.clear();
                        last_history = 0;
                    }
                    DeviceEvent::Dismissed => {
                        log::info!("attention alert dismissed on the device");
                        engine.on_device_dismissed();
                    }
                    DeviceEvent::Permission { id, allow } => {
                        let ok = perm_trusted && perm_bridge.answer(&paths.state_dir, id, allow);
                        log::info!(
                            "permission {id} {} on the device{}",
                            if allow { "allowed" } else { "denied" },
                            if ok { "" } else { " (too late)" }
                        );
                    }
                }
            }
        }
        if polling.as_ref().is_some_and(JoinHandle::is_finished) {
            if let Ok(results) = polling.take().expect("checked").await {
                for (d, r) in &results {
                    if let Err(e) = &r.result {
                        log::warn!("{}: {e}", d.display());
                    }
                }
                engine.on_poll_results(unix_now(), results);
                persist_book(engine.book(), &paths, &mut saved_book);
                let blocks = engine.blocks(unix_now());
                if blocks != saved_blocks {
                    if let Err(e) = backoff::save_blocks(&paths.usage_blocks_file(), &blocks) {
                        log::warn!("couldn't save usage blocks: {e}");
                    }
                    saved_blocks = blocks;
                }
            }
        }
        if polling.is_none() {
            if let Some(plan) = engine.begin_poll(now) {
                log::debug!(
                    "polling ({:?}): {:?}",
                    plan.reason,
                    plan.accounts.iter().zip(&plan.plans).collect::<Vec<_>>()
                );
                let (c, home, cwd) = (claude.clone(), paths.home.clone(), paths.state_dir.clone());
                polling = Some(tokio::task::spawn_blocking(move || poll_all(c, plan, home, cwd)));
            }
        }

        // ---- history (clawdmeter2) ----
        if history_scan.as_ref().is_some_and(JoinHandle::is_finished) {
            if let Ok(all) = history_scan.take().expect("checked").await {
                let pick = (now as u64) / 86_400; // a new joke each day
                for h in &all {
                    // Resent every scan, changed or not: the board keeps history in RAM only, so a
                    // reflash or reboot the app didn't notice would otherwise leave it empty.
                    let sig = clawdmeter_core::history::signature(h);
                    {
                        history_sent.insert(h.account, sig);
                        history_queue.retain(|m| !m.contains(&format!("\"i\":{},", h.account)));
                        history_queue.push(clawdmeter_core::history::message(h, pick + u64::from(h.account)));
                    }
                }
            }
        }
        if history_scan.is_none() && link.is_some() && now - last_history >= HISTORY_EVERY {
            last_history = now;
            let dirs = engine.settings().config_dirs.clone();
            let book = engine.book().clone();
            let state_dir = paths.state_dir.clone();
            history_scan = Some(tokio::task::spawn_blocking(move || {
                use clawdmeter_core::history;
                let today = chrono::Local::now().date_naive();
                let mut by = history::scan_local_by(&dirs, today, &|d, ts| book.email_at(d, ts).map(str::to_owned));
                // One entry per table email, number = position.
                let local: Vec<_> = book.emails().iter().map(|e| by.remove(e).unwrap_or_default()).collect();
                let slots: Vec<std::path::PathBuf> = book.emails().iter().map(std::path::PathBuf::from).collect();
                history::build(&slots, &local, &history::remote_reports(&state_dir), today)
            }));
        }

        // ---- link ----
        if connecting.as_ref().is_some_and(JoinHandle::is_finished) {
            match connecting.take().expect("checked").await {
                Ok(Ok((l, rx))) => {
                    log::info!("connected to {} ({})", l.name().unwrap_or("device"), l.key());
                    link_state = LinkState::Connected { key: l.key().to_owned(), name: l.name().map(str::to_owned) };
                    let key = l.key().to_owned();
                    perm_trusted =
                        tokio::task::spawn_blocking(move || clawdmeter_ble::os_paired(&key)).await.unwrap_or(false);
                    if !perm_trusted {
                        log::warn!("{} is not the device paired in Windows: no permission prompts on it", l.key());
                    }
                    link = Some(l);
                    device_events = Some(rx);
                    reconnect.connected();
                    inbox.reset();
                    engine.on_connected();
                    // A (re)connected device may have rebooted: resend history soon.
                    history_sent.clear();
                    last_history = 0;
                }
                Ok(Err(e)) => {
                    link_state = match &e {
                        LinkError::NoAdapter(s) => LinkState::NoAdapter(s.clone()),
                        _ => LinkState::Searching,
                    };
                    let wait = reconnect.failed(now);
                    if !matches!(e, LinkError::NotFound) || wait == 1 {
                        log::info!("{e}; retrying in {wait}s");
                    }
                }
                Err(e) => log::error!("connect task failed: {e}"),
            }
        }
        // The OS saw the device come back (BlueZ reconnected the bonded
        // device, or a scan saw it advertise): don't sit out the wait.
        while let Ok(w) = wakeups.try_recv() {
            if link.is_some() || !should_wake(&w, cache.load().as_deref()) {
                continue;
            }
            if connecting.is_none() && !reconnect.due(now) {
                let how = match w.sighting {
                    Sighting::Connected => "connected at the OS level",
                    Sighting::Advertising => "advertising",
                };
                log::info!("device {} {how}; reconnecting now", w.key);
            }
            reconnect.wake(now, connecting.is_some());
        }
        if link.is_none() && connecting.is_none() && reconnect.due(now) {
            if link_state == LinkState::Starting {
                link_state = LinkState::Connecting;
            }
            let (bt, cache, opts) = (bt.clone(), cache.clone(), link_opts.clone());
            connecting = Some(tokio::spawn(async move {
                let adapter = bt.adapter().await?;
                DeviceLink::open(&adapter, &cache, &opts).await
            }));
        }
        if let Some(l) = &link {
            if now - last_link_check >= LINK_CHECK_EVERY {
                last_link_check = now;
                if !l.is_connected().await {
                    log::info!("device disconnected, reconnecting...");
                    link = None;
                    device_events = None;
                    link_state = LinkState::Searching;
                    reconnect.retry_in(now, 2);
                }
            }
        }

        // ---- write ----
        // clawdmeter2: the current prompts' tokens from the transcripts.
        if now - turns_read_at >= 2 {
            turns_read_at = now;
            engine.refresh_turns(now);
        }
        if link.is_none() {
            perm_bridge.forget();
            perm_trusted = false;
        }
        if let Some(l) = &link {
            // Permission cards first: they are waiting on you.
            let cards = if perm_trusted {
                perm_bridge.sync(&paths.state_dir, now)
            } else {
                perm_bridge.decline_all(&paths.state_dir, now);
                Vec::new()
            };
            for m in cards {
                log::info!("Permission: {m}");
                if let Err(e) = l.write(m.as_bytes()).await {
                    log::warn!("permission write failed: {e}");
                }
            }
            if let Some(w) = engine
                .next_write(now, HostClock { utc_offset_secs: clawdmeter_core::platform::utc_offset_secs(), ..host })
            {
                let bytes = w.payload.encode(MAX_WRITE);
                let label = match w.kind {
                    WriteKind::Poll => "Sending",
                    WriteKind::Heartbeat => "Heartbeat",
                    WriteKind::Attention => "Attention",
                    WriteKind::Event => "Event",
                    WriteKind::NoData => "No data",
                };
                log::info!("{label}: {}", String::from_utf8_lossy(&bytes));
                match l.write(&bytes).await {
                    Ok(()) => {
                        engine.mark_written(now, &w);
                        last_write_at = Some(now);
                    }
                    Err(e) => {
                        log::warn!("write failed: {e}");
                        if !l.is_connected().await {
                            link = None;
                            device_events = None;
                            link_state = LinkState::Searching;
                            reconnect.retry_in(now, 2);
                        }
                    }
                }
            } else if !history_queue.is_empty() {
                // Only on a tick with nothing else to say: usage comes first.
                let m = history_queue.remove(0);
                log::info!("History: {m}");
                if let Err(e) = l.write(m.as_bytes()).await {
                    log::warn!("history write failed: {e}");
                    history_sent.clear(); // try them all again next scan
                }
            }
        }

        let _ = status.send(Status {
            link: link_state.clone(),
            engine: engine.status(now),
            last_write_at,
            claude: claude.clone(),
        });
    }
}

/// Write the book to `accounts.json` when it differs from what was last written.
fn persist_book(book: &AccountBook, paths: &Paths, last: &mut Vec<u8>) {
    let Ok(now) = serde_json::to_vec(book) else { return };
    if now == *last {
        return;
    }
    match book.save(&paths.accounts_file()) {
        Ok(()) => *last = now,
        Err(e) => log::warn!("couldn't save accounts.json: {e}"),
    }
}

/// Tell the engine who is logged in to each watched folder (`--once`; the
/// daemon watches the folders itself).
fn observe_logins(engine: &mut Engine, home: &std::path::Path, now: i64) {
    for d in engine.settings().config_dirs.clone() {
        let email = clawdmeter_core::identity::logged_in_email(&d, home);
        engine.on_folder_email(&d, email.as_deref(), now);
    }
}

/// One poll of every account, printed as the payload the device would get
/// (`clawdmeter --once`). No BLE.
pub fn once(paths: &Paths) -> i32 {
    let settings = match Settings::load(paths) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("config: {e}");
            return 1;
        }
    };
    let claude = usage::find_claude(settings.claude_path.as_deref(), &paths.home);
    let now = unix_now();
    let mut engine = Engine::new(settings.clone());
    observe_logins(&mut engine, &paths.home, now);
    engine.restore_blocks(&backoff::load_blocks(&paths.usage_blocks_file()), now);
    // A blocked account isn't asked: a call during a block lengthens it.
    let blocked = engine.blocks(now);
    for (d, b) in &blocked {
        println!("{}: {}", d.display(), usage::paused_message(b.until, clawdmeter_core::platform::utc_offset_secs()));
    }
    // Nothing is known yet, so the idle source for every account (no message).
    // Only folders someone known is logged in to.
    let (accounts, emails): (Vec<PathBuf>, Vec<String>) = settings
        .config_dirs
        .iter()
        .filter(|d| !blocked.contains_key(*d))
        .filter_map(|d| Some((d.clone(), engine.book().email_of(d)?.to_owned())))
        .unzip();
    let plan = PollPlan {
        reason: clawdmeter_core::cadence::PollReason::Startup,
        plans: vec![probe::Plan::Usage; accounts.len()],
        accounts,
        emails,
    };
    let results = poll_all(claude, plan, paths.home.clone(), paths.state_dir.clone());
    let mut ok = false;
    for (d, r) in &results {
        match &r.result {
            Ok(r) if r.is_spend_only() => {
                ok = true;
                println!("{}: spend limit {}% used", d.display(), r.session_pct());
            }
            Ok(r) => {
                ok = true;
                println!(
                    "{}: session {}%{}, week {}%",
                    d.display(),
                    r.session_pct(),
                    if r.window_started() { "" } else { " (window not started)" },
                    r.week.as_ref().map_or(0, |w| w.pct)
                );
            }
            Err(e) => println!("{}: {e}", d.display()),
        }
    }
    engine.on_poll_results(now, results);
    if let Err(e) = backoff::save_blocks(&paths.usage_blocks_file(), &engine.blocks(unix_now())) {
        eprintln!("couldn't save usage blocks: {e}");
    }
    let host = HostClock {
        utc_offset_secs: clawdmeter_core::platform::utc_offset_secs(),
        prefers_12h: clawdmeter_core::platform::prefers_12h(),
    };
    if let Some(w) = engine.next_write(now, host) {
        println!("{}", String::from_utf8_lossy(&w.payload.encode(MAX_WRITE)));
    }
    if ok {
        0
    } else {
        1
    }
}
