//! Token usage per local day, totalled from Claude Code's session transcripts.
//!
//! Transcripts are JSONL files under `<config dir>/projects/**.jsonl`. Assistant
//! lines carry `message.usage`; the same message can repeat on several lines
//! (streamed content blocks), so each `message.id` (else `requestId`) counts once.

use std::collections::{BTreeMap, HashSet};
use std::fs::{self, File};
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use chrono::{DateTime, NaiveDate, TimeZone, Timelike};
use serde_json::Value;

/// Token totals for one local day.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DayTotals {
    /// The local day.
    pub date: NaiveDate,
    /// Fresh input tokens.
    pub input: u64,
    /// Output tokens.
    pub output: u64,
    /// Cache-creation input tokens.
    pub cache_write: u64,
    /// Cache-read input tokens (excluded from [`DayTotals::billed`]).
    pub cache_read: u64,
    /// Output tokens per local hour 0..23.
    pub hours: [u64; 24],
}

impl DayTotals {
    fn empty(date: NaiveDate) -> Self {
        DayTotals { date, input: 0, output: 0, cache_write: 0, cache_read: 0, hours: [0; 24] }
    }

    /// Input + output + cache writes; cache reads are left out ("tokens burned").
    pub fn billed(&self) -> u64 {
        self.input.saturating_add(self.output).saturating_add(self.cache_write)
    }
}

/// Running totals plus the ids already counted.
#[derive(Debug, Default)]
pub struct Accumulator {
    seen: HashSet<String>,
    days: BTreeMap<NaiveDate, DayTotals>,
}

impl Accumulator {
    /// An empty accumulator.
    pub fn new() -> Self {
        Self::default()
    }

    /// Feed one transcript line. Unparseable, non-assistant, usage-less and
    /// already-counted lines are ignored.
    pub fn add_line<Tz: TimeZone>(&mut self, line: &str, tz: &Tz) {
        let line = line.trim();
        if line.is_empty() {
            return;
        }
        let Ok(v) = serde_json::from_str::<Value>(line) else { return };
        if v.get("type").and_then(Value::as_str) != Some("assistant") {
            return;
        }
        let Some(usage) = v.pointer("/message/usage").filter(|u| u.is_object()) else { return };
        let Some(ts) = v.get("timestamp").and_then(Value::as_str) else { return };
        let Ok(ts) = DateTime::parse_from_rfc3339(ts) else { return };
        let local = ts.with_timezone(tz);
        let date = local.date_naive();
        let hour = local.hour() as usize;

        let id =
            v.pointer("/message/id").and_then(Value::as_str).or_else(|| v.get("requestId").and_then(Value::as_str));
        if let Some(id) = id {
            if !self.seen.insert(id.to_owned()) {
                return;
            }
        }

        let n = |k: &str| usage.get(k).and_then(Value::as_u64).unwrap_or(0);
        let day = self.days.entry(date).or_insert_with(|| DayTotals::empty(date));
        day.input = day.input.saturating_add(n("input_tokens"));
        day.output = day.output.saturating_add(n("output_tokens"));
        day.hours[hour] = day.hours[hour].saturating_add(n("output_tokens"));
        day.cache_write = day.cache_write.saturating_add(n("cache_creation_input_tokens"));
        day.cache_read = day.cache_read.saturating_add(n("cache_read_input_tokens"));
    }

    /// Days on or after `since`, sorted ascending.
    pub fn finish(self, since: NaiveDate) -> Vec<DayTotals> {
        self.days.into_values().filter(|d| d.date >= since).collect()
    }
}

/// Total the lines of one or more transcripts (pure; no filesystem).
pub fn totals_from_lines<'a, Tz: TimeZone>(
    lines: impl IntoIterator<Item = &'a str>,
    since: NaiveDate,
    tz: &Tz,
) -> Vec<DayTotals> {
    let mut acc = Accumulator::new();
    for l in lines {
        acc.add_line(l, tz);
    }
    acc.finish(since)
}

/// Scan `<config_dir>/projects` recursively and total usage per local day from
/// `since` on. Files not modified since (about) `since` are skipped.
pub fn scan_dir<Tz: TimeZone>(config_dir: &Path, since: NaiveDate, tz: &Tz) -> Vec<DayTotals> {
    // One day of slack so timezone offsets never skip a relevant file.
    let cutoff_secs = since.and_hms_opt(0, 0, 0).map(|d| d.and_utc().timestamp()).unwrap_or(0) - 86_400;
    let cutoff = if cutoff_secs > 0 {
        SystemTime::UNIX_EPOCH + Duration::from_secs(cutoff_secs as u64)
    } else {
        SystemTime::UNIX_EPOCH
    };

    let mut files = Vec::new();
    collect_jsonl(&config_dir.join("projects"), &mut files);

    let mut acc = Accumulator::new();
    for path in files {
        if let Ok(modified) = fs::metadata(&path).and_then(|m| m.modified()) {
            if modified < cutoff {
                continue;
            }
        }
        let Ok(f) = File::open(&path) else { continue };
        // A read error (e.g. invalid UTF-8) ends that file.
        for line in BufReader::new(f).lines().map_while(Result::ok) {
            acc.add_line(&line, tz);
        }
    }
    acc.finish(since)
}

/// Like [`scan_dir`] over several folders, but each assistant message is
/// credited to `who(folder, unix timestamp)`; messages it returns `None` for
/// are skipped.
pub fn scan_dirs_by<Tz: TimeZone>(
    config_dirs: &[PathBuf],
    since: NaiveDate,
    tz: &Tz,
    who: &dyn Fn(&Path, i64) -> Option<String>,
) -> BTreeMap<String, Vec<DayTotals>> {
    let cutoff_secs = since.and_hms_opt(0, 0, 0).map(|d| d.and_utc().timestamp()).unwrap_or(0) - 86_400;
    let cutoff = SystemTime::UNIX_EPOCH + Duration::from_secs(cutoff_secs.max(0) as u64);
    let mut accs: BTreeMap<String, Accumulator> = BTreeMap::new();
    for dir in config_dirs {
        let mut files = Vec::new();
        collect_jsonl(&dir.join("projects"), &mut files);
        for path in files {
            if fs::metadata(&path).and_then(|m| m.modified()).is_ok_and(|m| m < cutoff) {
                continue;
            }
            let Ok(f) = File::open(&path) else { continue };
            for line in BufReader::new(f).lines().map_while(Result::ok) {
                let Some(ts) = line_timestamp(&line) else { continue };
                if let Some(email) = who(dir, ts) {
                    accs.entry(email).or_default().add_line(&line, tz);
                }
            }
        }
    }
    accs.into_iter().map(|(e, a)| (e, a.finish(since))).collect()
}

/// Unix time of an assistant transcript line (cheap pre-check, then parse).
fn line_timestamp(line: &str) -> Option<i64> {
    if !line.contains("\"assistant\"") {
        return None;
    }
    let v: Value = serde_json::from_str(line.trim()).ok()?;
    DateTime::parse_from_rfc3339(v.get("timestamp")?.as_str()?).ok().map(|t| t.timestamp())
}

fn collect_jsonl(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        match e.file_type() {
            Ok(t) if t.is_dir() => collect_jsonl(&p, out),
            Ok(t) if t.is_file() && p.extension().is_some_and(|x| x == "jsonl") => out.push(p),
            _ => {}
        }
    }
}

/// Billed tokens for each of the last `n` days ending `today`, zero-filled, oldest first.
pub fn last_n_days(totals: &[DayTotals], today: NaiveDate, n: u32) -> Vec<u64> {
    (0..n)
        .rev()
        .map(|back| {
            let day = today - chrono::Days::new(u64::from(back));
            totals.iter().filter(|t| t.date == day).map(DayTotals::billed).sum()
        })
        .collect()
}

/// Billed tokens on or after `since`.
pub fn sum_since(totals: &[DayTotals], since: NaiveDate) -> u64 {
    totals.iter().filter(|t| t.date >= since).map(DayTotals::billed).sum()
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::{FixedOffset, Utc};

    fn d(y: i32, m: u32, day: u32) -> NaiveDate {
        NaiveDate::from_ymd_opt(y, m, day).unwrap()
    }

    fn line(ts: &str, id: &str, i: u64, o: u64, cw: u64, cr: u64) -> String {
        format!(
            r#"{{"type":"assistant","timestamp":"{ts}","requestId":"req_{id}","message":{{"id":"{id}","usage":{{"input_tokens":{i},"cache_creation_input_tokens":{cw},"cache_read_input_tokens":{cr},"output_tokens":{o}}}}}}}"#
        )
    }

    #[test]
    fn dedupes_repeated_message_ids() {
        let a = line("2026-10-28T14:00:00Z", "msg_1", 10, 20, 30, 1000);
        let b = line("2026-10-28T14:00:01Z", "msg_2", 1, 2, 3, 4);
        let out = totals_from_lines([a.as_str(), a.as_str(), b.as_str(), a.as_str()], d(2026, 10, 1), &Utc);
        assert_eq!(out.len(), 1);
        assert_eq!((out[0].input, out[0].output, out[0].cache_write, out[0].cache_read), (11, 22, 33, 1004));
        assert_eq!(out[0].billed(), 66);
    }

    #[test]
    fn buckets_output_by_local_hour() {
        let tz = FixedOffset::east_opt(2 * 3600).unwrap();
        let a = line("2026-10-28T21:59:59Z", "a", 0, 5, 0, 0); // 23:59 local
        let b = line("2026-10-28T22:00:00Z", "b", 0, 7, 0, 0); // 00:00 local next day
        let c = line("2026-10-28T22:30:00Z", "c", 0, 3, 0, 0);
        let out = totals_from_lines([a.as_str(), b.as_str(), c.as_str()], d(2026, 10, 1), &tz);
        assert_eq!(out[0].hours[23], 5);
        assert_eq!(out[0].hours.iter().sum::<u64>(), 5);
        assert_eq!(out[1].hours[0], 10);
        assert_eq!(out[1].hours.iter().sum::<u64>(), out[1].output);
    }

    #[test]
    fn falls_back_to_request_id() {
        let l = r#"{"type":"assistant","timestamp":"2026-10-28T14:00:00Z","requestId":"r1","message":{"usage":{"input_tokens":5,"output_tokens":5}}}"#;
        let out = totals_from_lines([l, l], d(2026, 10, 1), &Utc);
        assert_eq!(out[0].billed(), 10);
    }

    #[test]
    fn buckets_by_local_day_across_midnight() {
        let tz = FixedOffset::east_opt(2 * 3600).unwrap();
        // 21:59Z = 23:59 local on the 28th; 22:00Z = 00:00 local on the 29th.
        let a = line("2026-10-28T21:59:59Z", "a", 1, 0, 0, 0);
        let b = line("2026-10-28T22:00:00Z", "b", 10, 0, 0, 0);
        let out = totals_from_lines([a.as_str(), b.as_str()], d(2026, 10, 1), &tz);
        assert_eq!(out.len(), 2);
        assert_eq!((out[0].date, out[0].input), (d(2026, 10, 28), 1));
        assert_eq!((out[1].date, out[1].input), (d(2026, 10, 29), 10));
    }

    #[test]
    fn ignores_bad_and_irrelevant_lines() {
        let good = line("2026-10-28T10:00:00Z", "g", 7, 0, 0, 0);
        let old = line("2026-09-01T10:00:00Z", "old", 99, 0, 0, 0);
        let lines = [
            "",
            "not json {",
            r#"{"type":"user","timestamp":"2026-10-28T10:00:00Z","message":{"usage":{"input_tokens":50}}}"#,
            r#"{"type":"assistant","timestamp":"2026-10-28T10:00:00Z","message":{"id":"x"}}"#,
            r#"{"type":"assistant","timestamp":"garbage","message":{"id":"y","usage":{"input_tokens":50}}}"#,
            good.as_str(),
            old.as_str(),
        ];
        let out = totals_from_lines(lines, d(2026, 10, 1), &Utc);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].billed(), 7);
    }

    #[test]
    fn last_n_days_zero_fills() {
        let t = |date, input| DayTotals { date, input, output: 0, cache_write: 0, cache_read: 500, hours: [0; 24] };
        let totals = [t(d(2026, 10, 27), 5), t(d(2026, 10, 29), 9), t(d(2026, 10, 1), 100)];
        assert_eq!(last_n_days(&totals, d(2026, 10, 29), 4), vec![0, 5, 0, 9]);
        assert!(last_n_days(&totals, d(2026, 10, 29), 0).is_empty());
        assert_eq!(sum_since(&totals, d(2026, 10, 27)), 14);
    }

    #[test]
    fn scans_nested_dirs() {
        let root = std::env::temp_dir().join(format!("clawdmeter-tokens-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let proj = root.join("projects").join("-proj");
        let nested = proj.join("sess").join("subagents");
        fs::create_dir_all(&nested).unwrap();
        let a = line("2026-10-28T10:00:00Z", "a", 1, 2, 3, 4);
        let b = line("2026-10-29T10:00:00Z", "b", 10, 20, 30, 40);
        fs::write(proj.join("one.jsonl"), format!("{a}\n{a}\nbroken\n")).unwrap();
        fs::write(nested.join("two.jsonl"), format!("{b}\n")).unwrap();
        fs::write(proj.join("ignore.txt"), &b).unwrap();

        let out = scan_dir(&root, d(2026, 10, 1), &Utc);
        let _ = fs::remove_dir_all(&root);
        assert_eq!(out.len(), 2);
        assert_eq!(out[0].billed(), 6);
        assert_eq!(out[1].billed(), 60);
        assert_eq!(out[1].cache_read, 40);
    }
}
