//! Pure logic for the Clawdmeter desktop daemon.
//!
//! Nothing in here talks to Bluetooth or draws a window. The modules:
//!
//! * [`paths`], [`config`]: the shared state dir (`~/.config/clawdmeter`) and its files.
//! * [`usage`]: run and parse `claude --output-format json -p /usage`.
//! * [`oauth`]: usage from Claude's usage endpoint with Claude Code's stored sign-in.
//! * [`probe`]: usage from one message's rate-limit headers, and when that is allowed.
//! * [`events`]: the hook events log (fast clock) and the activity tracker.
//! * [`attention`]: the attention hook's alert / clear files (written by
//!   `clawdmeter hook`; the installer lives in the app crate).
//! * [`cadence`]: when to poll, when to heartbeat.
//! * [`backoff`]: how long an account waits after a failed poll.
//! * [`selector`]: which account to show.
//! * [`payload`]: the JSON the firmware parses.
//! * [`rate`]: the slow-clock mood.
//! * [`engine`]: all of the above as one I/O-free state machine.
//! * [`autostart`], [`launch`], [`platform`]: start-at-login, the path to
//!   launch the app by, and host queries.

#![warn(missing_docs)]

pub mod attention;
pub mod autostart;
pub mod backoff;
pub mod burn;
pub mod cadence;
pub mod config;
pub mod engine;
pub mod events;
pub mod history;
pub mod identity;
pub mod launch;
pub mod live;
pub mod oauth;
pub mod paths;
pub mod payload;
pub mod platform;
pub mod probe;
pub mod rate;
pub mod selector;
pub mod toolinfo;
pub mod turn;
pub mod usage;
pub mod working;

/// Claude Code token totals per day from the session transcripts (shared
/// with the relay, which sends a server's totals).
pub use clawdmeter_relay::tokens;

pub use config::{ConfigFile, Settings};
pub use engine::{AccountError, Engine, EngineStatus, HostClock, PollPlan, Write, WriteKind};
pub use paths::Paths;
pub use payload::Payload;
pub use usage::{UsageError, UsageReading, Window};

/// The device's advertised name.
pub const DEVICE_NAME: &str = "Clawdmeter2";

/// Unix seconds now.
pub fn unix_now() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}
