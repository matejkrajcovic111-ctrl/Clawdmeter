//! clawdmeter2: is Claude working in an account right now? Drives the
//! device's recording light (`wk`). Per Claude Code session, the last hook
//! event says it: a prompt or a finished tool means it's working; a Stop, or
//! a question or notification waiting on you, means it isn't. A session that
//! goes quiet for [`WORKING_FOR`] counts as stopped (a crash, a closed window).

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use crate::events::HookKind;

/// A working session with no hook event for this long is taken as stopped.
pub const WORKING_FOR: i64 = 10 * 60;

/// The last state of each (account, session).
#[derive(Debug, Default, Clone)]
pub struct Working {
    sessions: HashMap<(PathBuf, String), (i64, bool)>,
}

impl Working {
    /// One hook event of `session` in `account`.
    pub fn note(&mut self, account: &Path, session: &str, kind: &HookKind, ts: i64) {
        let busy = match kind {
            HookKind::UserPromptSubmit | HookKind::PostToolUse => true,
            // PreToolUse only fires for AskUserQuestion: waiting on you.
            HookKind::Stop | HookKind::Notification | HookKind::PreToolUse => false,
            HookKind::Other(_) => return,
        };
        let key = (account.to_path_buf(), session.to_owned());
        if self.sessions.get(&key).is_some_and(|(t, _)| *t > ts) {
            return; // out of order: keep the newer state
        }
        self.sessions.insert(key, (ts, busy));
    }

    /// Some session of `account` is working at `now`.
    pub fn is_working(&self, account: &Path, now: i64) -> bool {
        self.sessions.iter().any(|((a, _), (ts, busy))| *busy && a == account && now - ts < WORKING_FOR)
    }

    /// Forget every session of `account` (its login changed).
    pub fn forget(&mut self, account: &Path) {
        self.sessions.retain(|(a, _), _| a != account);
    }

    /// Forget sessions that can no longer count.
    pub fn prune(&mut self, now: i64) {
        self.sessions.retain(|_, (ts, _)| now - *ts < WORKING_FOR);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn prompt_works_until_stop() {
        let a = Path::new("/h/.claude");
        let mut w = Working::default();
        assert!(!w.is_working(a, 0));
        w.note(a, "s1", &HookKind::UserPromptSubmit, 100);
        assert!(w.is_working(a, 101));
        w.note(a, "s1", &HookKind::PostToolUse, 200);
        assert!(w.is_working(a, 201));
        w.note(a, "s1", &HookKind::Stop, 300);
        assert!(!w.is_working(a, 301));
    }

    #[test]
    fn any_session_counts_and_accounts_are_separate() {
        let (a, b) = (Path::new("/h/.claude"), Path::new("/h/.claude-2"));
        let mut w = Working::default();
        w.note(a, "s1", &HookKind::UserPromptSubmit, 100);
        w.note(a, "s2", &HookKind::Stop, 110);
        assert!(w.is_working(a, 120));
        assert!(!w.is_working(b, 120));
    }

    #[test]
    fn waiting_on_you_and_silence_are_not_working() {
        let a = Path::new("/h/.claude");
        let mut w = Working::default();
        w.note(a, "s1", &HookKind::UserPromptSubmit, 100);
        w.note(a, "s1", &HookKind::Notification, 150);
        assert!(!w.is_working(a, 151));
        w.note(a, "s1", &HookKind::PostToolUse, 200);
        assert!(!w.is_working(a, 200 + WORKING_FOR), "quiet too long");
        w.note(a, "s1", &HookKind::Stop, 190);
        assert!(w.is_working(a, 201), "an older event doesn't override");
        w.prune(200 + WORKING_FOR);
        assert!(!w.is_working(a, 201));
    }
}
