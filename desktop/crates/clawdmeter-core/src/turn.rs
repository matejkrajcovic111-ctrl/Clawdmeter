//! clawdmeter2: tokens of the current prompt ("turn") per account, for the
//! device's status line, counted like Claude Code's own spinner: the output
//! tokens of the replies since the last prompt. They come from the session's
//! transcript, `<config dir>/projects/<project>/<session id>.jsonl`, read
//! incrementally from where the last read stopped.

use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use chrono::DateTime;
use serde_json::Value;

/// A turn untouched this long is forgotten.
const FORGET_AFTER: i64 = 6 * 3600;
/// A run with no sign of life this long counts as over even without a Stop
/// (an interrupted turn fires none).
const STILL_RUNNING_FOR: i64 = 10 * 60;
/// At most this much transcript is read per refresh and turn.
const READ_CAP: u64 = 4 << 20;

#[derive(Debug, Default)]
struct Turn {
    /// Unix seconds of the prompt: replies from then on count.
    since: i64,
    path: Option<PathBuf>,
    offset: u64,
    /// Output tokens per message id (the largest seen: a reply's lines repeat its usage).
    replies: HashMap<String, u64>,
    /// Claude is (probably) thinking: the prompt or a tool result was the
    /// last thing seen, or the last reply block was a thinking one.
    thinking: bool,
    /// The run is still going: no Stop since the prompt. A prompt sent now
    /// adds to this run's count instead of starting a new one.
    open: bool,
    /// Unix seconds of the latest prompt or transcript line seen.
    last_seen: i64,
    /// The tool Claude called last and that hasn't answered yet.
    tool: Option<String>,
}

impl Turn {
    fn running(&self, at: i64) -> bool {
        self.open && at - self.last_seen < STILL_RUNNING_FOR
    }
}

impl Turn {
    fn tokens(&self) -> u64 {
        self.replies.values().sum()
    }
}

/// The latest turn of each (account, session).
#[derive(Debug, Default)]
pub struct Turns {
    turns: HashMap<(PathBuf, String), Turn>,
}

impl Turns {
    /// A prompt in `session` of `account` at `ts` starts a new turn, unless
    /// the previous one is still running: then the count carries on.
    pub fn prompt(&mut self, account: &Path, session: &str, ts: i64) {
        if session.is_empty() {
            return;
        }
        let key = (account.to_path_buf(), session.to_owned());
        if self.turns.get(&key).is_some_and(|t| t.since > ts) {
            return;
        }
        if let Some(t) = self.turns.get_mut(&key).filter(|t| t.running(ts)) {
            t.thinking = true;
            t.last_seen = t.last_seen.max(ts);
            return;
        }
        let old = self.turns.remove(&key);
        // Same transcript: keep reading from where we were.
        let (path, offset) = old.map(|t| (t.path, t.offset)).unwrap_or((None, 0));
        self.turns.insert(
            key,
            Turn {
                since: ts,
                path,
                offset,
                replies: HashMap::new(),
                thinking: true,
                open: true,
                last_seen: ts,
                tool: None,
            },
        );
    }

    /// `session` of `account` finished its turn (the Stop hook) at `ts`.
    pub fn stop(&mut self, account: &Path, session: &str, ts: i64) {
        if let Some(t) = self.turns.get_mut(&(account.to_path_buf(), session.to_owned())) {
            if ts >= t.since {
                t.open = false;
                t.tool = None;
            }
        }
    }

    /// Forget every turn of `account` (its login changed).
    pub fn forget(&mut self, account: &Path) {
        self.turns.retain(|(a, _), _| a != account);
    }

    /// Read what the transcripts gained since the last refresh.
    pub fn refresh(&mut self, now: i64) {
        self.turns.retain(|_, t| now - t.since < FORGET_AFTER);
        for ((account, session), t) in self.turns.iter_mut() {
            if t.path.is_none() {
                t.path = find_transcript(account, session);
            }
            if let Some(p) = t.path.clone() {
                read_new(&p, t);
            }
        }
    }

    /// Output tokens of `account`'s latest turn (the latest prompt among its
    /// sessions); `None` when no prompt has been seen.
    pub fn tokens(&self, account: &Path) -> Option<u64> {
        self.latest(account).map(Turn::tokens)
    }

    /// Unix seconds of `account`'s latest prompt.
    pub fn since(&self, account: &Path) -> Option<i64> {
        self.latest(account).map(|t| t.since)
    }

    /// Whether `account`'s latest turn looks like Claude is thinking (the
    /// transcript only records finished blocks, so this is a guess).
    pub fn thinking(&self, account: &Path) -> Option<bool> {
        self.latest(account).map(|t| t.thinking)
    }

    /// clawdmeter2: the tool `account`'s latest turn is running right now
    /// (its name as Claude Code calls it), if any.
    pub fn tool(&self, account: &Path) -> Option<&str> {
        self.latest(account).filter(|t| t.open).and_then(|t| t.tool.as_deref())
    }

    fn latest(&self, account: &Path) -> Option<&Turn> {
        self.turns.iter().filter(|((a, _), _)| a == account).map(|(_, t)| t).max_by_key(|t| t.since)
    }
}

fn find_transcript(account: &Path, session: &str) -> Option<PathBuf> {
    let name = format!("{session}.jsonl");
    std::fs::read_dir(account.join("projects")).ok()?.flatten().map(|e| e.path().join(&name)).find(|p| p.is_file())
}

fn read_new(path: &Path, t: &mut Turn) {
    let Ok(mut f) = File::open(path) else { return };
    let Ok(len) = f.metadata().map(|m| m.len()) else { return };
    if len < t.offset {
        t.offset = 0; // rewritten
    }
    if len == t.offset || f.seek(SeekFrom::Start(t.offset)).is_err() {
        return;
    }
    let mut buf = Vec::new();
    if f.take(READ_CAP).read_to_end(&mut buf).is_err() {
        return;
    }
    // Whole lines only: a half-written last line waits for the next read.
    let Some(end) = buf.iter().rposition(|&b| b == b'\n') else {
        // One line longer than the cap (a big pasted image / tool result):
        // skip it, else every read would stop on it. Its tail fails to parse
        // and is ignored.
        if buf.len() as u64 == READ_CAP {
            t.offset += READ_CAP;
        }
        return;
    };
    for line in buf[..end].split(|&b| b == b'\n') {
        add_line(t, line);
    }
    t.offset += end as u64 + 1;
}

fn add_line(t: &mut Turn, line: &[u8]) {
    let Ok(v) = serde_json::from_slice::<Value>(line) else { return };
    let kind = v.get("type").and_then(Value::as_str);
    if kind != Some("assistant") && kind != Some("user") {
        return;
    }
    let Some(ts) = v.get("timestamp").and_then(Value::as_str).and_then(|s| DateTime::parse_from_rfc3339(s).ok()) else {
        return;
    };
    if ts.timestamp() < t.since {
        return;
    }
    t.last_seen = t.last_seen.max(ts.timestamp());
    if kind == Some("user") {
        // A message you typed (a mid-turn one fires no prompt hook): while
        // the run is going it adds to the same count (Claude Code would
        // restart its own); after the run ended it starts a new one.
        if typed_by_you(&v) {
            if !t.open {
                t.since = ts.timestamp();
                t.replies.clear();
                t.open = true;
            }
            t.thinking = true;
        } else if has_block(&v, "tool_result") {
            t.thinking = true; // the model is about to answer again
            t.tool = None;
        }
        return;
    }
    match last_block_type(&v) {
        Some("thinking" | "redacted_thinking") => t.thinking = true,
        Some("text") => {
            t.thinking = false;
            t.tool = None;
        }
        Some("tool_use") => {
            t.thinking = false;
            t.tool = last_tool_name(&v).map(str::to_owned);
        }
        _ => {}
    }
    let Some(out) = v.pointer("/message/usage/output_tokens").and_then(Value::as_u64) else { return };
    let id = v.pointer("/message/id").or_else(|| v.get("requestId")).and_then(Value::as_str).unwrap_or("").to_owned();
    let e = t.replies.entry(id).or_insert(0);
    *e = (*e).max(out);
}

fn last_tool_name(v: &Value) -> Option<&str> {
    v.pointer("/message/content")?.as_array()?.last()?.get("name")?.as_str()
}

fn last_block_type(v: &Value) -> Option<&str> {
    v.pointer("/message/content")?.as_array()?.last()?.get("type")?.as_str()
}

fn has_block(v: &Value, kind: &str) -> bool {
    v.pointer("/message/content")
        .and_then(Value::as_array)
        .is_some_and(|bs| bs.iter().any(|b| b.get("type").and_then(Value::as_str) == Some(kind)))
}

/// A transcript `user` line that is your own text: not a tool result, and
/// not one Claude Code adds itself (`isMeta`, a compaction summary).
fn typed_by_you(v: &Value) -> bool {
    let flag = |k: &str| v.get(k).and_then(Value::as_bool).unwrap_or(false);
    if flag("isMeta") || flag("isCompactSummary") {
        return false;
    }
    match v.pointer("/message/content") {
        Some(Value::String(s)) => !s.trim().is_empty(),
        Some(Value::Array(blocks)) => {
            !blocks.is_empty() && blocks.iter().all(|b| b.get("type").and_then(Value::as_str) == Some("text"))
        }
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn line(ts: &str, id: &str, out: u64) -> String {
        format!(
            r#"{{"type":"assistant","timestamp":"{ts}","message":{{"id":"{id}","usage":{{"input_tokens":9,"output_tokens":{out}}}}}}}"#
        ) + "\n"
    }

    #[test]
    fn counts_replies_since_the_prompt() {
        let dir = std::env::temp_dir().join(format!("cm-turn-{}", std::process::id()));
        let proj = dir.join("projects").join("p");
        std::fs::create_dir_all(&proj).unwrap();
        let file = proj.join("s1.jsonl");
        let mut f = File::create(&file).unwrap();
        // 2026-10-30T10:00:00Z = 1793354400
        let prompt = DateTime::parse_from_rfc3339("2026-10-30T10:00:00Z").unwrap().timestamp();
        f.write_all(line("2026-10-30T09:59:00Z", "old", 500).as_bytes()).unwrap();
        f.write_all(line("2026-10-30T10:00:05Z", "a", 100).as_bytes()).unwrap();
        f.write_all(line("2026-10-30T10:00:06Z", "a", 120).as_bytes()).unwrap(); // same reply, more
        f.write_all(br#"{"type":"user","timestamp":"2026-10-30T10:00:07Z"}"#).unwrap();
        f.write_all(b"\n").unwrap();

        let mut t = Turns::default();
        assert_eq!(t.tokens(&dir), None);
        t.prompt(&dir, "s1", prompt);
        t.refresh(prompt + 10);
        assert_eq!(t.tokens(&dir), Some(120));

        // More of the turn, the last line half-written.
        f.write_all(line("2026-10-30T10:00:20Z", "b", 30).as_bytes()).unwrap();
        f.write_all(br#"{"type":"assist"#).unwrap();
        t.refresh(prompt + 20);
        assert_eq!(t.tokens(&dir), Some(150));

        // A prompt while the run is going keeps counting.
        t.prompt(&dir, "s1", prompt + 30);
        assert_eq!(t.tokens(&dir), Some(150));
        assert_eq!(t.since(&dir), Some(prompt));

        // After the Stop, the next prompt starts again from zero.
        t.stop(&dir, "s1", prompt + 40);
        t.prompt(&dir, "s1", prompt + 60);
        f.write_all(b"ant\"}\n").unwrap();
        f.write_all(line("2026-10-30T10:01:05Z", "c", 7).as_bytes()).unwrap();
        t.refresh(prompt + 70);
        assert_eq!(t.tokens(&dir), Some(7));

        // Neither a tool result nor a message you type mid-turn restarts it.
        f.write_all(br#"{"type":"user","timestamp":"2026-10-30T10:01:10Z","message":{"content":[{"type":"tool_result","content":"ok"}]}}"#).unwrap();
        f.write_all(b"\n").unwrap();
        f.write_all(line("2026-10-30T10:01:20Z", "d", 3).as_bytes()).unwrap();
        t.refresh(prompt + 80);
        assert_eq!(t.tokens(&dir), Some(10));
        f.write_all(br#"{"type":"user","timestamp":"2026-10-30T10:01:30Z","message":{"content":"also this"}}"#)
            .unwrap();
        f.write_all(b"\n").unwrap();
        f.write_all(line("2026-10-30T10:01:35Z", "e", 4).as_bytes()).unwrap();
        t.refresh(prompt + 100);
        assert_eq!(t.tokens(&dir), Some(14));
        assert_eq!(t.since(&dir), Some(prompt + 60));

        // After a Stop, a typed message starts a new count.
        t.stop(&dir, "s1", prompt + 105);
        f.write_all(br#"{"type":"user","timestamp":"2026-10-30T10:01:50Z","message":{"content":"next"}}"#).unwrap();
        f.write_all(
            b"
",
        )
        .unwrap();
        f.write_all(line("2026-10-30T10:01:55Z", "f", 2).as_bytes()).unwrap();
        t.refresh(prompt + 120);
        assert_eq!(t.tokens(&dir), Some(2));
        assert_eq!(t.since(&dir), Some(prompt + 110));

        // A run silent for 10 minutes (an interrupt sends no Stop) is over.
        t.prompt(&dir, "s1", prompt + 120 + STILL_RUNNING_FOR);
        assert_eq!(t.tokens(&dir), Some(0));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn thinking_heuristic() {
        let dir = std::env::temp_dir().join(format!("cm-think-{}", std::process::id()));
        let proj = dir.join("projects").join("p");
        std::fs::create_dir_all(&proj).unwrap();
        let mut f = File::create(proj.join("s1.jsonl")).unwrap();
        let prompt = DateTime::parse_from_rfc3339("2026-10-30T10:00:00Z").unwrap().timestamp();
        let mut t = Turns::default();
        assert_eq!(t.thinking(&dir), None);
        t.prompt(&dir, "s1", prompt);
        t.refresh(prompt + 1);
        assert_eq!(t.thinking(&dir), Some(true), "right after the prompt");
        let asst = |ts: &str, block: &str| {
            format!(r#"{{"type":"assistant","timestamp":"{ts}","message":{{"id":"m","content":[{block}]}}}}"#) + "\n"
        };
        let mut step = |t: &mut Turns, text: String, n: i64| {
            f.write_all(text.as_bytes()).unwrap();
            t.refresh(prompt + n);
            t.thinking(&dir)
        };
        let th = r#"{"type":"thinking","thinking":"hm"}"#;
        assert_eq!(step(&mut t, asst("2026-10-30T10:00:02Z", th), 3), Some(true));
        let tu = r#"{"type":"text","text":"x"},{"type":"tool_use","id":"1","name":"B","input":{}}"#;
        assert_eq!(step(&mut t, asst("2026-10-30T10:00:03Z", tu), 4), Some(false));
        assert_eq!(t.tool(&dir), Some("B"), "the tool it called");
        let tr = r#"{"type":"user","timestamp":"2026-10-30T10:00:04Z","message":{"content":[{"type":"tool_result","content":"ok"}]}}"#;
        assert_eq!(step(&mut t, tr.to_owned() + "\n", 5), Some(true));
        assert_eq!(t.tool(&dir), None, "answered");
        let txt = r#"{"type":"text","text":"done"}"#;
        assert_eq!(step(&mut t, asst("2026-10-30T10:00:05Z", txt), 6), Some(false));
        let rd = r#"{"type":"redacted_thinking","data":"x"}"#;
        assert_eq!(step(&mut t, asst("2026-10-30T10:00:06Z", rd), 7), Some(true));
        // A message you type mid-turn starts thinking again.
        assert_eq!(step(&mut t, asst("2026-10-30T10:00:07Z", txt), 8), Some(false));
        let typed = r#"{"type":"user","timestamp":"2026-10-30T10:00:08Z","message":{"content":"more"}}"#;
        assert_eq!(step(&mut t, typed.to_owned() + "\n", 9), Some(true));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
