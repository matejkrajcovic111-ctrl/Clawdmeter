//! Per-account token history for the device's bar chart: local transcripts
//! merged with the totals that remote servers report.

use std::collections::hash_map::DefaultHasher;
use std::collections::BTreeMap;
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use chrono::NaiveDate;
use clawdmeter_relay::AccountDays;
use serde::Serialize;

use crate::{burn, tokens};

/// Days in the chart.
const DAYS: usize = 30;
/// Days in the weekly total.
const WEEK: usize = 7;
/// Largest device message, in bytes.
const MAX_MESSAGE: usize = 500;
/// Per-hour value cap in `hd` (hundreds of tokens), keeps the message small.
const HD_CAP: u64 = 99_999;
/// Remote reports older than this are ignored.
const MAX_REPORT_AGE: Duration = Duration::from_secs(3 * 86_400);

/// Token history of one account.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AccountHistory {
    /// 1-based account number.
    pub account: u8,
    /// Billed tokens per day, 30 entries, oldest first, ending today.
    pub days: Vec<u64>,
    /// Billed tokens over the last 7 days, today included.
    pub week: u64,
    /// Today's output tokens per local hour 0..23; `None` when only remote
    /// reports (without hours) exist.
    pub hours: Option<[u64; 24]>,
}

/// Read every `relay-tokens-*.json` in `state_dir`. Unreadable or invalid
/// files, and files not modified for 3 days, are ignored.
pub fn remote_reports(state_dir: &Path) -> Vec<Vec<AccountDays>> {
    let Ok(rd) = std::fs::read_dir(state_dir) else {
        return Vec::new();
    };
    let now = SystemTime::now();
    let mut names: Vec<PathBuf> = rd
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.starts_with("relay-tokens-") && n.ends_with(".json"))
        })
        .collect();
    names.sort();
    names
        .into_iter()
        .filter_map(|p| {
            let modified = std::fs::metadata(&p).and_then(|m| m.modified()).ok()?;
            if now.duration_since(modified).is_ok_and(|age| age > MAX_REPORT_AGE) {
                return None;
            }
            let text = std::fs::read_to_string(&p).ok()?;
            serde_json::from_str::<Vec<AccountDays>>(&text).ok()
        })
        .collect()
}

/// Scan each config dir's transcripts for the last 35 days.
pub fn scan_local(config_dirs: &[PathBuf], today: NaiveDate) -> Vec<Vec<tokens::DayTotals>> {
    let since = today - chrono::Days::new(34);
    config_dirs.iter().map(|d| tokens::scan_dir(d, since, &chrono::Local)).collect()
}

/// Scan every config dir's transcripts for the last 35 days, crediting each
/// message to `who(folder, timestamp)` (the email logged in then); messages
/// with no email are skipped.
pub fn scan_local_by(
    config_dirs: &[PathBuf],
    today: NaiveDate,
    who: &dyn Fn(&Path, i64) -> Option<String>,
) -> BTreeMap<String, Vec<tokens::DayTotals>> {
    let since = today - chrono::Days::new(34);
    tokens::scan_dirs_by(config_dirs, since, &chrono::Local, who)
}

/// Merge local and remote totals into one history per account
/// (`1..=config_dirs.len()`). Pure.
pub fn build(
    config_dirs: &[PathBuf],
    local: &[Vec<tokens::DayTotals>],
    remote: &[Vec<AccountDays>],
    today: NaiveDate,
) -> Vec<AccountHistory> {
    (0..config_dirs.len())
        .map(|idx| {
            let account = u8::try_from(idx + 1).unwrap_or(u8::MAX);
            let mut days = tokens::last_n_days(local.get(idx).map_or(&[], Vec::as_slice), today, DAYS as u32);
            for report in remote {
                for ad in report.iter().filter(|a| usize::from(a.i) == idx + 1) {
                    for (date, n) in &ad.days {
                        let Ok(d) = NaiveDate::parse_from_str(date, "%Y-%m-%d") else {
                            continue;
                        };
                        let back = (today - d).num_days();
                        if (0..DAYS as i64).contains(&back) {
                            let slot = &mut days[DAYS - 1 - back as usize];
                            *slot = slot.saturating_add(*n);
                        }
                    }
                }
            }
            let week = days[DAYS - WEEK..].iter().fold(0u64, |a, b| a.saturating_add(*b));
            let hours = local.get(idx).and_then(|l| l.iter().find(|t| t.date == today)).map(|t| t.hours);
            AccountHistory { account, days, week, hours }
        })
        .collect()
}

#[derive(Serialize)]
struct Msg<'a> {
    h: u8,
    i: u8,
    d: Vec<u64>,
    wt: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    hd: Option<Vec<u64>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    j: Option<&'a str>,
}

/// The device message `{"h":1,"i":..,"d":[30 thousands],"wt":..,"hd":[24 hundreds],"j":".."}`,
/// at most 500 bytes (the joke is dropped first, then `hd`). `hd` is today's
/// output per local hour in hundreds of tokens, rounded up, and only present
/// when hourly data exists.
pub fn message(h: &AccountHistory, pick: u64) -> String {
    let d: Vec<u64> = h.days.iter().map(|&t| t / 1000 + u64::from(t % 1000 >= 500)).collect();
    let joke = burn::comparison(h.week, pick);
    let hd =
        h.hours.map(|hs| hs.iter().map(|&t| (t / 100 + u64::from(t % 100 != 0)).min(HD_CAP)).collect::<Vec<u64>>());
    let mut msg = Msg { h: 1, i: h.account, d, wt: h.week, hd, j: Some(&joke) };
    let mut s = serde_json::to_string(&msg).unwrap_or_default();
    if s.len() > MAX_MESSAGE {
        msg.j = None;
        s = serde_json::to_string(&msg).unwrap_or_default();
    }
    if s.len() > MAX_MESSAGE && msg.hd.is_some() {
        msg.hd = None;
        s = serde_json::to_string(&msg).unwrap_or_default();
    }
    s
}

/// Cheap hash of the days and week, to resend only on change.
pub fn signature(h: &AccountHistory) -> u64 {
    let mut s = DefaultHasher::new();
    h.days.hash(&mut s);
    h.week.hash(&mut s);
    h.hours.hash(&mut s);
    s.finish()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn today() -> NaiveDate {
        NaiveDate::from_ymd_opt(2026, 10, 29).unwrap()
    }

    fn local_day(date: &str, output: u64) -> tokens::DayTotals {
        tokens::DayTotals {
            date: date.parse().unwrap(),
            input: 0,
            output,
            cache_write: 0,
            cache_read: 0,
            hours: [0; 24],
        }
    }

    fn rep(i: u8, days: &[(&str, u64)]) -> AccountDays {
        AccountDays { i, days: days.iter().map(|(d, n)| (d.to_string(), *n)).collect() }
    }

    fn dirs(n: usize) -> Vec<PathBuf> {
        (0..n).map(|i| PathBuf::from(format!("/c{i}"))).collect()
    }

    #[test]
    fn history_split_at_switch() {
        let root = std::env::temp_dir().join(format!("cm-hist-by-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let proj = root.join("projects").join("p");
        std::fs::create_dir_all(&proj).unwrap();
        let line = |ts: &str, id: &str, out: u64| {
            format!(
                r#"{{"type":"assistant","timestamp":"{ts}","message":{{"id":"{id}","usage":{{"output_tokens":{out}}}}}}}"#
            )
        };
        let text = [
            line("2026-10-29T10:00:00Z", "a", 5),
            line("2026-10-29T11:00:00Z", "b", 7),
            line("2026-10-29T13:00:00Z", "c", 100),
        ]
        .join("\n");
        std::fs::write(proj.join("s.jsonl"), text).unwrap();
        let t = chrono::DateTime::parse_from_rfc3339("2026-10-29T12:00:00Z").unwrap().timestamp();
        let who = move |_: &Path, ts: i64| Some(if ts < t { "m@x" } else { "l@x" }.to_owned());
        let out = scan_local_by(std::slice::from_ref(&root), today(), &who);
        let _ = std::fs::remove_dir_all(&root);
        let sum = |e: &str| out[e].iter().map(tokens::DayTotals::billed).sum::<u64>();
        assert_eq!((sum("m@x"), sum("l@x")), (12, 100));
        let none = |_: &Path, _: i64| None;
        assert!(scan_local_by(&[], today(), &none).is_empty());
    }

    #[test]
    fn merges_local_and_remotes() {
        let local = vec![vec![local_day("2026-10-29", 100)], vec![local_day("2026-10-29", 7)]];
        let r1 = vec![rep(1, &[("2026-10-29", 10), ("2026-10-28", 5)])];
        let r2 = vec![rep(1, &[("2026-10-29", 1)]), rep(2, &[("2026-10-27", 3)])];
        let h = build(&dirs(2), &local, &[r1, r2], today());
        assert_eq!(h.len(), 2);
        assert_eq!(h[0].account, 1);
        assert_eq!(h[0].days[29], 111);
        assert_eq!(h[0].days[28], 5);
        assert_eq!(h[0].week, 116);
        assert_eq!(h[1].days[29], 7);
        assert_eq!(h[1].days[27], 3);
        assert_eq!(h[1].week, 10);
    }

    #[test]
    fn window_zero_fill_and_bad_dates() {
        let r = vec![rep(1, &[("2026-09-29", 9), ("2026-09-30", 4), ("2026-10-30", 8), ("garbage", 6)])];
        let h = build(&dirs(1), &[vec![]], &[r], today());
        assert_eq!(h[0].days.len(), 30);
        assert_eq!(h[0].days[0], 4); // 2026-09-30 is 29 days back
        assert_eq!(h[0].days.iter().sum::<u64>(), 4);
        assert_eq!(h[0].week, 0);
    }

    #[test]
    fn week_is_last_seven() {
        let days: Vec<(String, u64)> = (0..10).map(|b| ((today() - chrono::Days::new(b)).to_string(), 1)).collect();
        let h = build(&dirs(1), &[], &[vec![AccountDays { i: 1, days }]], today());
        assert_eq!(h[0].week, 7);
        assert_eq!(h[0].days.iter().sum::<u64>(), 10);
    }

    #[test]
    fn message_format() {
        let mut days = vec![0u64; 30];
        days[28] = 1_499;
        days[29] = 1_500;
        let h = AccountHistory { account: 2, days, week: 2_999, hours: None };
        let expect_j = serde_json::to_string(&burn::comparison(2_999, 0)).unwrap();
        let expect = format!("{{\"h\":1,\"i\":2,\"d\":[{}1,2],\"wt\":2999,\"j\":{}}}", "0,".repeat(28), expect_j);
        assert_eq!(message(&h, 0), expect);
    }

    #[test]
    fn hd_rounds_up_and_formats() {
        let mut hours = [0u64; 24];
        hours[0] = 1;
        hours[1] = 100;
        hours[2] = 101;
        hours[23] = 250;
        let h = AccountHistory { account: 1, days: vec![0; 30], week: 0, hours: Some(hours) };
        let m = message(&h, 0);
        let v: serde_json::Value = serde_json::from_str(&m).unwrap();
        let hd: Vec<u64> = v["hd"].as_array().unwrap().iter().map(|x| x.as_u64().unwrap()).collect();
        assert_eq!(hd.len(), 24);
        assert_eq!((hd[0], hd[1], hd[2], hd[3], hd[23]), (1, 1, 2, 0, 3));
        // key order: wt, hd, j
        assert!(m.find("\"wt\"").unwrap() < m.find("\"hd\"").unwrap());
    }

    #[test]
    fn hd_omitted_without_hours() {
        let h = AccountHistory { account: 1, days: vec![0; 30], week: 0, hours: None };
        assert!(!message(&h, 0).contains("hd"));
        let r = vec![rep(1, &[("2026-10-29", 10)])];
        assert_eq!(build(&dirs(1), &[], &[r], today())[0].hours, None);
    }

    #[test]
    fn build_takes_today_hours_from_local() {
        let mut t = local_day("2026-10-29", 50);
        t.hours[9] = 50;
        let mut y = local_day("2026-10-28", 70);
        y.hours[3] = 70;
        let h = build(&dirs(1), &[vec![y, t]], &[], today());
        let hs = h[0].hours.unwrap();
        assert_eq!((hs[9], hs[3]), (50, 0));
    }

    #[test]
    fn realistic_heavy_day_keeps_hd() {
        let h =
            AccountHistory { account: 1, days: vec![5_000_000; 30], week: 35_000_000, hours: Some([2_000_000; 24]) };
        let m = message(&h, 1);
        assert!(m.len() <= 500, "{} bytes", m.len());
        assert!(m.contains("\"hd\""));
    }

    #[test]
    fn worst_case_fits() {
        let h = AccountHistory {
            account: 255,
            days: vec![u64::MAX / 2000; 30],
            week: u64::MAX,
            hours: Some([u64::MAX; 24]),
        };
        let m = message(&h, 3);
        assert!(m.len() <= 500, "{} bytes", m.len());
        assert!(serde_json::from_str::<serde_json::Value>(&m).is_ok());
    }

    #[test]
    fn signature_changes() {
        let a = AccountHistory { account: 1, days: vec![1; 30], week: 7, hours: None };
        let mut b = a.clone();
        assert_eq!(signature(&a), signature(&b));
        b.days[3] = 2;
        assert_ne!(signature(&a), signature(&b));
        let mut c = a.clone();
        c.hours = Some([0; 24]);
        assert_ne!(signature(&a), signature(&c));
        let mut e = c.clone();
        e.hours = Some([0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        assert_ne!(signature(&c), signature(&e));
    }

    #[test]
    fn remote_reports_reads_valid_only() {
        let dir = std::env::temp_dir().join(format!("clawdmeter-history-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("relay-tokens-a.json"), r#"[{"i":1,"days":[["2026-10-29",5]]}]"#).unwrap();
        std::fs::write(dir.join("relay-tokens-bad.json"), "not json").unwrap();
        std::fs::write(dir.join("other.json"), r#"[{"i":1,"days":[]}]"#).unwrap();
        let r = remote_reports(&dir);
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(r, vec![vec![rep(1, &[("2026-10-29", 5)])]]);
        assert!(remote_reports(&dir).is_empty());
    }
}
