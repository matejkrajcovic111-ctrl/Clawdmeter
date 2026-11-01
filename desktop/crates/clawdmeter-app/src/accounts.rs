//! Claude Code account folders: who is logged in, and the login console.

use std::path::{Path, PathBuf};

use clawdmeter_core::identity::AccountBook;
pub use clawdmeter_core::identity::{email_from, logged_in_email, state_file};

/// The folder to open or log in for `email`: a folder logged into it now,
/// else the first watched folder nobody is logged into, else a new
/// `~/.claude-<k>` (lowest free k from 2; `true` = it must be created and
/// watched).
pub fn folder_for(email: &str, book: &AccountBook, watched: &[PathBuf], home: &Path) -> (PathBuf, bool) {
    if let Some(d) = book.dirs_of(email).into_iter().next() {
        return (d, false);
    }
    // A folder with credentials but no email on record yet (mid-login) counts as free.
    if let Some(d) = watched.iter().find(|d| book.email_of(d).is_none()) {
        return (d.clone(), false);
    }
    let new = (2u32..)
        .map(|k| home.join(format!(".claude-{k}")))
        .find(|d| !watched.contains(d) && !d.exists())
        .expect("free folder");
    (new, true)
}

/// How the child's `CLAUDE_CONFIG_DIR` must be set for a config folder.
#[derive(Debug, PartialEq, Eq)]
pub enum ConfigEnv<'a> {
    /// `~/.claude` is Claude Code's default: the variable must be absent.
    Remove,
    /// Set it to this folder.
    Set(&'a Path),
}

/// Which way `CLAUDE_CONFIG_DIR` goes for `dir`.
pub fn config_env<'a>(dir: &'a Path, home: &Path) -> ConfigEnv<'a> {
    if dir == home.join(".claude") {
        ConfigEnv::Remove
    } else {
        ConfigEnv::Set(dir)
    }
}

/// Window title for an account's console: "Claude account #N (email)".
pub fn console_title(n: usize, email: Option<&str>) -> String {
    match email.map(str::trim).filter(|e| !e.is_empty()) {
        Some(e) => format!("Claude account #{n} ({e})"),
        None => format!("Claude account #{n}"),
    }
}

/// The fixed PowerShell script of the "Open Claude" console. It only reads
/// environment variables, so no path or email is ever put in command text.
pub const OPEN_CLAUDE_SCRIPT: &str = "$Host.UI.RawUI.WindowTitle = $env:CLAWD_TITLE; & $env:CLAWD_CLAUDE";

#[cfg(windows)]
fn apply_config_env(cmd: &mut std::process::Command, dir: &Path, home: &Path) {
    match config_env(dir, home) {
        ConfigEnv::Remove => cmd.env_remove("CLAUDE_CONFIG_DIR"),
        ConfigEnv::Set(d) => cmd.env("CLAUDE_CONFIG_DIR", d),
    };
}

/// Open a new console that logs one account in. The folder goes through
/// `CLAUDE_CONFIG_DIR` in the child's environment (removed for `~/.claude`),
/// never through a command string.
#[cfg(windows)]
pub fn open_login(claude: &Path, dir: &Path, home: &Path, n: usize) -> Result<(), String> {
    use std::os::windows::process::CommandExt;
    let mut cmd = std::process::Command::new("cmd.exe");
    // CREATE_NEW_CONSOLE; `cmd /k` keeps the window open afterwards. The
    // claude path travels in the environment, not in the command line.
    cmd.creation_flags(0x0000_0010);
    cmd.raw_arg(format!("/d /k title Claude account #{n} & \"%CLAWD_CLAUDE%\" auth login"));
    cmd.env("CLAWD_CLAUDE", claude);
    apply_config_env(&mut cmd, dir, home);
    cmd.spawn().map(|_| ()).map_err(|e| format!("Couldn't open the login window: {e}"))
}

/// Open a new PowerShell console running Claude Code for one account, in the
/// home folder. Title and claude path travel in the environment.
#[cfg(windows)]
pub fn open_claude(claude: &Path, dir: &Path, home: &Path, n: usize) -> Result<(), String> {
    use std::os::windows::process::CommandExt;
    let email = logged_in_email(dir, home);
    let mut cmd = std::process::Command::new("powershell.exe");
    cmd.creation_flags(0x0000_0010);
    cmd.args(["-NoExit", "-NoLogo", "-Command", OPEN_CLAUDE_SCRIPT]);
    cmd.current_dir(home);
    cmd.env("CLAWD_CLAUDE", claude);
    cmd.env("CLAWD_TITLE", console_title(n, email.as_deref()));
    apply_config_env(&mut cmd, dir, home);
    cmd.spawn().map(|_| ()).map_err(|e| format!("Couldn't open Claude: {e}"))
}

/// Not wired up off Windows: say what to run by hand.
#[cfg(not(windows))]
pub fn open_login(_claude: &Path, dir: &Path, _home: &Path, _n: usize) -> Result<(), String> {
    Err(format!("Run in a terminal: CLAUDE_CONFIG_DIR={} claude auth login", dir.display()))
}

#[cfg(not(windows))]
pub fn open_claude(_claude: &Path, dir: &Path, _home: &Path, _n: usize) -> Result<(), String> {
    Err(format!("Run in a terminal: CLAUDE_CONFIG_DIR={} claude", dir.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn folder_choice() {
        let home = std::env::temp_dir().join(format!("clawd-folder-choice-{}", std::process::id()));
        std::fs::remove_dir_all(&home).ok();
        std::fs::create_dir_all(&home).unwrap();
        let (a, b, c) = (home.join(".claude"), home.join(".claude-2"), home.join(".claude-3"));
        let mut book = AccountBook::new(vec!["m@x".into(), "l@x".into(), "z@x".into()]);
        book.observe(&a, Some("m@x"), 1);
        book.observe(&b, Some("l@x"), 1);
        let watched = vec![a.clone(), b.clone()];
        // logged in somewhere: that folder
        assert_eq!(folder_for("l@x", &book, &watched, &home), (b.clone(), false));
        // not logged in anywhere, no free watched folder: a new one
        assert_eq!(folder_for("z@x", &book, &watched, &home), (c.clone(), true));
        // .claude-3 exists on disk but is unwatched: skip it
        std::fs::create_dir_all(&c).unwrap();
        let d4 = home.join(".claude-4");
        assert_eq!(folder_for("z@x", &book, &watched, &home), (d4, true));
        std::fs::remove_dir_all(&c).unwrap();
        // .claude-2 exists on disk but is unwatched: skip it
        let only_a = vec![a.clone()];
        std::fs::create_dir_all(&b).unwrap();
        assert_eq!(folder_for("z@x", &book, &only_a, &home), (c.clone(), true));
        // a watched folder with nobody logged in is used
        let mut w2 = watched.clone();
        w2.push(c.clone());
        assert_eq!(folder_for("z@x", &book, &w2, &home), (c.clone(), false));
        // a logged-in folder that is not watched is still returned
        let mut book2 = AccountBook::new(vec!["z@x".into()]);
        book2.observe(&c, Some("z@x"), 1);
        assert_eq!(folder_for("z@x", &book2, &only_a, &home), (c.clone(), false));
        // two folders for one email: the first in timeline order
        let mut book3 = AccountBook::new(vec!["z@x".into()]);
        book3.observe(&c, Some("z@x"), 1);
        book3.observe(&b, Some("z@x"), 2);
        assert_eq!(folder_for("z@x", &book3, &watched, &home), (c, false));
        // logged out folder (observed as None) counts as free
        book.observe(&b, None, 2);
        assert_eq!(folder_for("z@x", &book, &watched, &home), (b, false));
        std::fs::remove_dir_all(&home).ok();
    }

    #[test]
    fn config_env_choice() {
        let home = Path::new("/h");
        assert_eq!(config_env(&home.join(".claude"), home), ConfigEnv::Remove);
        let d = home.join(".claude-2");
        assert_eq!(config_env(&d, home), ConfigEnv::Set(&d));
    }

    #[test]
    fn title_text() {
        assert_eq!(console_title(2, None), "Claude account #2");
        assert_eq!(console_title(2, Some("  ")), "Claude account #2");
        assert_eq!(console_title(1, Some("me@x.com")), "Claude account #1 (me@x.com)");
    }

    #[test]
    fn script_is_fixed_and_env_only() {
        assert!(OPEN_CLAUDE_SCRIPT.contains("$env:CLAWD_TITLE") && OPEN_CLAUDE_SCRIPT.contains("$env:CLAWD_CLAUDE"));
    }
}
