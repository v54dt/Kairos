//! Stream-id -> source mapping. Replaces hardcoded 1001/1002 assumptions so a
//! second broker feed (fubon, reserved 1003/1004) can be subscribed by config
//! without code changes. Env-first like the Aeron dir: `KAIROS_STREAMS` overrides,
//! otherwise the default table is byte-identical to today (quotes 1001 source 0,
//! control 1002).

use crate::ipc::aeron::{CONTROL_STREAM_ID, DEFAULT_STREAM_ID};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StreamRole {
    Quotes,
    Control,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StreamEntry {
    pub stream_id: i32,
    pub source: u16,
    pub role: StreamRole,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StreamTable {
    entries: Vec<StreamEntry>,
}

impl Default for StreamTable {
    fn default() -> Self {
        Self {
            entries: vec![
                StreamEntry {
                    stream_id: DEFAULT_STREAM_ID,
                    source: 0,
                    role: StreamRole::Quotes,
                },
                StreamEntry {
                    stream_id: CONTROL_STREAM_ID,
                    source: 0,
                    role: StreamRole::Control,
                },
            ],
        }
    }
}

impl StreamTable {
    /// Explicit `KAIROS_STREAMS` wins; an empty/unset env yields the default table
    /// (unchanged behavior).
    pub fn from_env() -> anyhow::Result<Self> {
        match std::env::var("KAIROS_STREAMS") {
            Ok(s) if !s.trim().is_empty() => Self::parse(&s),
            _ => Ok(Self::default()),
        }
    }

    /// Parse a comma-separated table of `id:source[:role]` entries. `role` defaults
    /// to `quotes`. Stream ids must be unique and each quotes source unique, so two
    /// streams can never merge into one book.
    pub fn parse(spec: &str) -> anyhow::Result<Self> {
        let mut entries = Vec::new();
        for field in spec.split(',') {
            let field = field.trim();
            if field.is_empty() {
                continue;
            }
            let mut parts = field.split(':');
            let id_str = parts.next().unwrap_or_default().trim();
            let src_str = parts
                .next()
                .ok_or_else(|| anyhow::anyhow!("stream entry '{field}' missing source"))?
                .trim();
            let role = match parts.next().map(str::trim) {
                None | Some("quotes") => StreamRole::Quotes,
                Some("control") => StreamRole::Control,
                Some(other) => anyhow::bail!("unknown stream role '{other}' in '{field}'"),
            };
            if parts.next().is_some() {
                anyhow::bail!("too many fields in stream entry '{field}'");
            }
            let stream_id: i32 = id_str
                .parse()
                .map_err(|_| anyhow::anyhow!("bad stream id '{id_str}' in '{field}'"))?;
            let source: u16 = src_str
                .parse()
                .map_err(|_| anyhow::anyhow!("bad source '{src_str}' in '{field}'"))?;
            entries.push(StreamEntry {
                stream_id,
                source,
                role,
            });
        }
        if entries.is_empty() {
            anyhow::bail!("KAIROS_STREAMS is empty");
        }
        for (i, e) in entries.iter().enumerate() {
            if entries[..i].iter().any(|p| p.stream_id == e.stream_id) {
                anyhow::bail!("duplicate stream id {}", e.stream_id);
            }
            if e.role == StreamRole::Quotes
                && entries[..i]
                    .iter()
                    .any(|p| p.role == StreamRole::Quotes && p.source == e.source)
            {
                anyhow::bail!("duplicate quotes source {} (would merge books)", e.source);
            }
        }
        Ok(Self { entries })
    }

    pub fn quotes(&self) -> impl Iterator<Item = &StreamEntry> {
        self.entries.iter().filter(|e| e.role == StreamRole::Quotes)
    }

    /// The control stream id (first control entry), else the default.
    pub fn control_stream_id(&self) -> i32 {
        self.entries
            .iter()
            .find(|e| e.role == StreamRole::Control)
            .map(|e| e.stream_id)
            .unwrap_or(CONTROL_STREAM_ID)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_table_matches_today() {
        let t = StreamTable::default();
        let quotes: Vec<_> = t.quotes().collect();
        assert_eq!(quotes.len(), 1);
        assert_eq!(quotes[0].stream_id, 1001);
        assert_eq!(quotes[0].source, 0);
        assert_eq!(t.control_stream_id(), 1002);
    }

    #[test]
    fn parse_default_spelling_equals_default() {
        let t = StreamTable::parse("1001:0:quotes,1002:0:control").unwrap();
        assert_eq!(t, StreamTable::default());
    }

    #[test]
    fn parse_two_feeds() {
        let t = StreamTable::parse("1001:0:quotes,1003:1:quotes,1002:0:control").unwrap();
        let quotes: Vec<_> = t.quotes().collect();
        assert_eq!(quotes.len(), 2);
        assert_eq!((quotes[0].stream_id, quotes[0].source), (1001, 0));
        assert_eq!((quotes[1].stream_id, quotes[1].source), (1003, 1));
        assert_eq!(t.control_stream_id(), 1002);
    }

    #[test]
    fn role_defaults_to_quotes() {
        let t = StreamTable::parse("1001:0").unwrap();
        assert_eq!(t.quotes().count(), 1);
    }

    #[test]
    fn rejects_duplicate_stream_id() {
        assert!(StreamTable::parse("1001:0:quotes,1001:1:quotes").is_err());
    }

    #[test]
    fn rejects_duplicate_quotes_source() {
        assert!(StreamTable::parse("1001:0:quotes,1003:0:quotes").is_err());
    }

    #[test]
    fn rejects_bad_fields() {
        assert!(StreamTable::parse("1001").is_err());
        assert!(StreamTable::parse("abc:0").is_err());
        assert!(StreamTable::parse("1001:x").is_err());
        assert!(StreamTable::parse("1001:0:bogus").is_err());
        assert!(StreamTable::parse("1001:0:quotes:extra").is_err());
    }
}
