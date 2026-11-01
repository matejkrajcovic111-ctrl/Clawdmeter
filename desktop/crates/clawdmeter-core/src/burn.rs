//! The "tokens burned this week" line on the display device.
//!
//! [`short_tokens`] gives the compact count (`12.4K`, `1.8M`). [`comparison`]
//! turns the same count into one playful line. The device font has no accents,
//! curly quotes or emoji, so every line is plain ASCII and at most
//! [`MAX_LINE`] characters.

/// Longest line the display shows, in characters.
pub const MAX_LINE: usize = 44;

/// The line for a week with no tokens.
const ZERO_LINE: &str = "Clean slate. Not a token burned.";

/// English words per token (rough).
const WORDS_PER_TOKEN: f64 = 0.75;
/// Words read aloud per minute.
const WORDS_PER_MINUTE: f64 = 150.0;
/// Words on a printed page.
const WORDS_PER_PAGE: f64 = 500.0;
/// Thickness of a printed page, in metres (0.1 mm).
const PAGE_THICKNESS_M: f64 = 0.0001;
/// Average adult height, in metres.
const PERSON_HEIGHT_M: f64 = 1.7;
/// Words in a feature-film screenplay.
const SCREENPLAY_WORDS: f64 = 22_000.0;
/// Words in a typical novel.
const NOVEL_WORDS: f64 = 90_000.0;
/// Words in the King James Bible.
const BIBLE_WORDS: f64 = 783_000.0;
/// Words in Shakespeare's complete works.
const SHAKESPEARE_WORDS: f64 = 880_000.0;
/// Minutes in a two-hour movie.
const MOVIE_MINUTES: f64 = 120.0;
/// Minutes in the extended Lord of the Rings trilogy.
const LOTR_MINUTES: f64 = 686.0;
/// Words in English Wikipedia.
const WIKIPEDIA_WORDS: f64 = 4.5e9;

/// Compact count: `950`, `12.4K`, `1.8M`, `2.3B`. One decimal, no trailing `.0`.
pub fn short_tokens(n: u64) -> String {
    if n < 1_000 {
        return n.to_string();
    }
    const STEPS: [(u128, &str); 3] = [(1_000, "K"), (1_000_000, "M"), (1_000_000_000, "B")];
    let mut i = 0;
    loop {
        let (unit, suffix) = STEPS[i];
        // Tenths of the unit, rounded half up.
        let tenths = (n as u128 * 10 + unit / 2) / unit;
        // Step up once rounding reaches 1000 of this unit (999_960 -> 1M).
        if tenths < 10_000 || i + 1 == STEPS.len() {
            let (whole, frac) = (tenths / 10, tenths % 10);
            return if frac == 0 { format!("{whole}{suffix}") } else { format!("{whole}.{frac}{suffix}") };
        }
        i += 1;
    }
}

/// One playful ASCII line (at most [`MAX_LINE`] chars) for `tokens`. `pick`
/// chooses among the lines that fit this size (`pick % count`), so the line
/// changes from week to week.
pub fn comparison(tokens: u64, pick: u64) -> String {
    if tokens == 0 {
        return ZERO_LINE.to_string();
    }
    let lines = candidates(tokens);
    lines[(pick % lines.len() as u64) as usize].clone()
}

/// The lines that make sense at this size, in a fixed order. Never empty
/// for `tokens >= 1`.
fn candidates(tokens: u64) -> Vec<String> {
    let words = tokens as f64 * WORDS_PER_TOKEN;
    let minutes = words / WORDS_PER_MINUTE;
    let pages = words / WORDS_PER_PAGE;
    let mut out = vec![
        format!("Tokens burned: {}. Impressive.", short_tokens(tokens)),
        "Clawd is pretending this was not work.".to_string(),
        "Clawd would like a nap, probably.".to_string(),
    ];
    if tokens >= 1_000 {
        out.push("Clawd yawns. Carry on.".to_string());
    }
    if tokens >= 12_000 {
        let h = amount(minutes / 60.0);
        out.push(format!("Read aloud nonstop: {h} {}", noun(&h, "hour", "hours")));
    }
    if tokens >= 24_000 {
        let m = amount(minutes / MOVIE_MINUTES);
        out.push(format!("Read aloud: {m} two-hour {}", noun(&m, "movie", "movies")));
    }
    if tokens >= 30_000 {
        let s = amount(words / SCREENPLAY_WORDS);
        out.push(format!("Enough for {s} feature-film {}", noun(&s, "screenplay", "screenplays")));
    }
    if tokens >= 50_000 {
        out.push("Clawd needs a snack after that one.".to_string());
    }
    if tokens >= 100_000 {
        let n = amount(words / NOVEL_WORDS);
        out.push(format!("Clawd read {n} {} this week", noun(&n, "novel", "novels")));
    }
    if tokens >= 150_000 {
        let n = amount(minutes / LOTR_MINUTES);
        out.push(format!("Read aloud: {n} extended LOTR {}", noun(&n, "trilogy", "trilogies")));
    }
    if tokens >= 500_000 {
        out.push("Clawd has filed this under: a lot.".to_string());
    }
    if tokens >= 1_000_000 {
        let n = amount(words / BIBLE_WORDS);
        out.push(format!("= {n} x the King James Bible"));
        let n = amount(words / SHAKESPEARE_WORDS);
        out.push(format!("= {n} x Shakespeare's complete works"));
    }
    if tokens >= 2_000_000 {
        let h = amount(pages * PAGE_THICKNESS_M);
        let p = amount(pages * PAGE_THICKNESS_M / PERSON_HEIGHT_M);
        out.push(format!("Printed, a {h} m stack: {p} {} tall", noun(&p, "person", "people")));
    }
    if tokens >= 5_000_000 {
        out.push("Clawd is writing you a thank-you note.".to_string());
    }
    if tokens >= 1_000_000_000 {
        let p = amount(words / WIKIPEDIA_WORDS * 100.0);
        out.push(format!("Share of English Wikipedia: {p}%"));
        out.push("Clawd is going to need a bigger hat.".to_string());
    }
    out
}

/// A count on a comparison line: whole numbers from 10 up, one decimal below,
/// no trailing `.0`, compact (`341K`) once it gets large (whole billions only).
fn amount(x: f64) -> String {
    if x >= 1e9 {
        format!("{:.0}B", x / 1e9)
    } else if x >= 10_000.0 {
        short_tokens(x.round() as u64)
    } else if x >= 9.95 {
        format!("{x:.0}")
    } else {
        let s = format!("{x:.1}");
        s.strip_suffix(".0").unwrap_or(&s).to_string()
    }
}

/// `one` when the shown count is exactly 1, else `many`.
fn noun<'a>(shown: &str, one: &'a str, many: &'a str) -> &'a str {
    if shown == "1" {
        one
    } else {
        many
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compact_counts() {
        assert_eq!(short_tokens(0), "0");
        assert_eq!(short_tokens(950), "950");
        assert_eq!(short_tokens(999), "999");
        assert_eq!(short_tokens(1_000), "1K");
        assert_eq!(short_tokens(12_400), "12.4K");
        assert_eq!(short_tokens(999_960), "1M", "rounding steps up a unit");
        assert_eq!(short_tokens(1_800_000), "1.8M");
        assert_eq!(short_tokens(2_000_000), "2M", "no trailing .0");
        assert_eq!(short_tokens(2_300_000_000), "2.3B");
        assert_eq!(short_tokens(10_000_000_000), "10B");
    }

    #[test]
    fn example_lines() {
        assert_eq!(comparison(950, 0), "Tokens burned: 950. Impressive.");
        assert_eq!(comparison(12_400, 4), "Read aloud nonstop: 1 hour");
        assert_eq!(comparison(1_800_000, 0), "Tokens burned: 1.8M. Impressive.");
        assert_eq!(comparison(2_300_000_000, 0), "Tokens burned: 2.3B. Impressive.");
    }

    #[test]
    fn lines_are_ascii_and_short() {
        let sizes =
            [0, 1, 999, 10_000, 100_000, 1_000_000, 10_000_000, 100_000_000, 1_000_000_000, 10_000_000_000, u64::MAX];
        for tokens in sizes {
            for pick in 0..=20 {
                let line = comparison(tokens, pick);
                assert!(line.is_ascii(), "{tokens}/{pick}: {line}");
                assert!(line.len() <= MAX_LINE, "{tokens}/{pick} is {} chars: {line}", line.len());
                assert!(!line.contains("0.0"), "{tokens}/{pick}: {line}");
            }
        }
    }

    #[test]
    fn picks_vary_the_line() {
        let mut seen: Vec<String> = (0..=20).map(|p| comparison(5_000_000, p)).collect();
        seen.sort();
        seen.dedup();
        assert!(seen.len() >= 2, "{seen:?}");
    }

    #[test]
    fn zero_tokens_is_fixed() {
        for pick in 0..=20 {
            assert_eq!(comparison(0, pick), "Clean slate. Not a token burned.");
        }
    }
}
