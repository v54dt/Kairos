//! Minimal fail-soft JSON field readers shared by the order-journal, hub-status
//! and supervisor parsers. Not a general JSON parser: each reader locates a
//! `"key":` needle and decodes just that value, degrading to `None`/defaults on
//! anything malformed rather than panicking.

pub(crate) fn int(s: &str, key: &str) -> Option<i64> {
    let needle = format!("\"{key}\":");
    let start = s.find(&needle)? + needle.len();
    let rest = &s[start..];
    let end = rest
        .find(|c: char| c != '-' && !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

pub(crate) fn boolean(s: &str, key: &str) -> Option<bool> {
    let needle = format!("\"{key}\":");
    let start = s.find(&needle)? + needle.len();
    let rest = s[start..].trim_start();
    if rest.starts_with("true") {
        Some(true)
    } else if rest.starts_with("false") {
        Some(false)
    } else {
        None
    }
}

// Read the JSON string value of `"key":"..."`, decoding every escape the exec
// emitters produce (\" \\ \/ \b \f \n \r \t \uXXXX) and stopping at the first
// UNescaped quote. Unknown/short escapes are kept verbatim (fail-soft: at worst
// a garbled display char, never a panic) so a new-server payload degrades safely.
pub(crate) fn string(s: &str, key: &str) -> Option<String> {
    let needle = format!("\"{key}\":\"");
    let start = s.find(&needle)? + needle.len();
    let mut out = String::new();
    let mut chars = s[start..].chars();
    while let Some(c) = chars.next() {
        match c {
            '"' => return Some(out),
            '\\' => match chars.next() {
                Some('"') => out.push('"'),
                Some('\\') => out.push('\\'),
                Some('/') => out.push('/'),
                Some('b') => out.push('\u{08}'),
                Some('f') => out.push('\u{0c}'),
                Some('n') => out.push('\n'),
                Some('r') => out.push('\r'),
                Some('t') => out.push('\t'),
                Some('u') => {
                    let hex: String = (&mut chars).take(4).collect();
                    let cp = (hex.len() == 4)
                        .then(|| u32::from_str_radix(&hex, 16).ok())
                        .flatten();
                    out.push(cp.and_then(char::from_u32).unwrap_or('\u{fffd}'));
                }
                Some(other) => {
                    out.push('\\');
                    out.push(other);
                }
                None => break,
            },
            c => out.push(c),
        }
    }
    Some(out)
}

/// Split the `"<array_key>":[ {..}, {..} ]` array into its top-level `{..}`
/// objects, honouring brace depth so nested objects never split early.
pub(crate) fn objects<'a>(s: &'a str, array_key: &str) -> Vec<&'a str> {
    let mut out = Vec::new();
    let needle = format!("\"{array_key}\":[");
    let arr_start = match s.find(&needle) {
        Some(i) => i + needle.len(),
        None => return out,
    };
    let bytes = s.as_bytes();
    let mut depth = 0i32;
    let mut obj_start = None;
    for i in arr_start..bytes.len() {
        match bytes[i] {
            b'{' => {
                if depth == 0 {
                    obj_start = Some(i);
                }
                depth += 1;
            }
            b'}' => {
                depth -= 1;
                if depth == 0
                    && let Some(st) = obj_start.take()
                {
                    out.push(&s[st..=i]);
                }
            }
            b']' if depth == 0 => break,
            _ => {}
        }
    }
    out
}
