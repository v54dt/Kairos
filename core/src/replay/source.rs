//! KQR playback source: opens one or more `.kqr` files and yields their records
//! merged by `recv_ts_us`, so multiple streams of the same day interleave exactly
//! as they were recorded. A torn tail or a CRC failure stops that one file with a
//! counted warning (the shared reader recovers; replay never panics).

use std::cmp::Ordering;
use std::collections::BinaryHeap;
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::{Path, PathBuf};

use crate::record::{RecordError, RecordReader, record_path};

/// One recorded fragment tagged with the stream it came from.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReplayRecord {
    pub stream_id: u32,
    pub recv_ts_us: i64,
    pub payload: Vec<u8>,
}

type BoxReader = RecordReader<Box<dyn Read + Send>>;

struct Cursor {
    reader: BoxReader,
    stream_id: u32,
    done: bool,
}

/// Heap entry ordered ascending on `(recv_ts_us, cursor_idx)`. Because each cursor
/// holds at most one entry at a time, that pair is a total order, so identical
/// timestamps break ties deterministically by the file's position in the input
/// list (earlier file wins).
struct HeapItem {
    recv_ts_us: i64,
    cursor_idx: usize,
    payload: Vec<u8>,
}

impl PartialEq for HeapItem {
    fn eq(&self, other: &Self) -> bool {
        self.recv_ts_us == other.recv_ts_us && self.cursor_idx == other.cursor_idx
    }
}
impl Eq for HeapItem {}
impl Ord for HeapItem {
    fn cmp(&self, other: &Self) -> Ordering {
        // Reversed so `BinaryHeap` (a max-heap) yields the smallest key first.
        (other.recv_ts_us, other.cursor_idx).cmp(&(self.recv_ts_us, self.cursor_idx))
    }
}
impl PartialOrd for HeapItem {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

/// A merge-ordered playback source over one or more KQR files.
pub struct KqrSource {
    cursors: Vec<Cursor>,
    heap: BinaryHeap<HeapItem>,
    warnings: u64,
}

impl KqrSource {
    /// Open an explicit list of files. Order in the list is the tie-break for
    /// records that share a `recv_ts_us`.
    pub fn open_files(paths: &[PathBuf]) -> Result<Self, RecordError> {
        let mut readers: Vec<Box<dyn Read + Send>> = Vec::with_capacity(paths.len());
        for p in paths {
            let f = File::open(p)?;
            readers.push(Box::new(BufReader::new(f)));
        }
        Self::from_readers(readers)
    }

    /// Open every `<data_dir>/kqr/<yyyymmdd>/s<stream>-<yyyymmdd>.kqr` for the
    /// given streams, using recordd's naming convention verbatim.
    pub fn open_selector(
        data_dir: &Path,
        yyyymmdd: &str,
        streams: &[u32],
    ) -> Result<Self, RecordError> {
        let paths: Vec<PathBuf> = streams
            .iter()
            .map(|s| record_path(data_dir, *s, yyyymmdd))
            .collect();
        Self::open_files(&paths)
    }

    /// Build a source over already-open readers (each positioned at a KQR header).
    /// Boxing the reader keeps this reusable for in-memory test fixtures.
    pub fn from_readers(readers: Vec<Box<dyn Read + Send>>) -> Result<Self, RecordError> {
        let mut cursors = Vec::with_capacity(readers.len());
        for r in readers {
            let (header, reader) = RecordReader::open(r)?;
            cursors.push(Cursor {
                reader,
                stream_id: header.stream_id,
                done: false,
            });
        }
        let mut src = Self {
            cursors,
            heap: BinaryHeap::new(),
            warnings: 0,
        };
        for idx in 0..src.cursors.len() {
            src.advance(idx);
        }
        Ok(src)
    }

    /// Total number of files stopped early by a torn tail or corrupt record.
    pub fn warnings(&self) -> u64 {
        self.warnings
    }

    /// The set of distinct stream ids across the opened files.
    pub fn stream_ids(&self) -> Vec<u32> {
        let mut ids: Vec<u32> = self.cursors.iter().map(|c| c.stream_id).collect();
        ids.sort_unstable();
        ids.dedup();
        ids
    }

    /// Read the next record from one cursor into the heap. A reader `Err` (torn or
    /// corrupt) counts one warning and retires the cursor — the reader iterator is
    /// undefined after its first error, so it is never polled again.
    fn advance(&mut self, idx: usize) {
        let cursor = &mut self.cursors[idx];
        if cursor.done {
            return;
        }
        match cursor.reader.read_record() {
            Ok(Some(rec)) => self.heap.push(HeapItem {
                recv_ts_us: rec.recv_ts_us,
                cursor_idx: idx,
                payload: rec.payload,
            }),
            Ok(None) => cursor.done = true,
            Err(e) => {
                cursor.done = true;
                self.warnings += 1;
                eprintln!(
                    "kairos-replayd: stream {} stopping at a bad record: {e}",
                    cursor.stream_id
                );
            }
        }
    }
}

impl Iterator for KqrSource {
    type Item = ReplayRecord;

    fn next(&mut self) -> Option<ReplayRecord> {
        let item = self.heap.pop()?;
        let stream_id = self.cursors[item.cursor_idx].stream_id;
        self.advance(item.cursor_idx);
        Some(ReplayRecord {
            stream_id,
            recv_ts_us: item.recv_ts_us,
            payload: item.payload,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::record::{FileHeader, RecordWriter};

    fn kqr(stream_id: u32, records: &[(i64, &[u8])]) -> Vec<u8> {
        let mut w = RecordWriter::create(Vec::new(), &FileHeader::new(stream_id, 0)).unwrap();
        for (ts, p) in records {
            w.append(*ts, p).unwrap();
        }
        w.into_inner()
    }

    fn reader(buf: Vec<u8>) -> Box<dyn Read + Send> {
        Box::new(std::io::Cursor::new(buf))
    }

    fn source(bufs: Vec<Vec<u8>>) -> KqrSource {
        KqrSource::from_readers(bufs.into_iter().map(reader).collect()).unwrap()
    }

    #[test]
    fn merges_two_streams_in_ts_order() {
        let a = kqr(1001, &[(10, b"a10"), (30, b"a30"), (50, b"a50")]);
        let b = kqr(1002, &[(20, b"b20"), (40, b"b40")]);
        let got: Vec<(u32, i64)> = source(vec![a, b])
            .map(|r| (r.stream_id, r.recv_ts_us))
            .collect();
        assert_eq!(
            got,
            vec![(1001, 10), (1002, 20), (1001, 30), (1002, 40), (1001, 50)]
        );
    }

    #[test]
    fn identical_ts_breaks_tie_by_file_order() {
        let a = kqr(1001, &[(5, b"first")]);
        let b = kqr(1002, &[(5, b"second")]);
        // b listed before a -> b (cursor 0) wins the tie.
        let got: Vec<u32> = source(vec![b, a]).map(|r| r.stream_id).collect();
        assert_eq!(got, vec![1002, 1001]);
    }

    #[test]
    fn ts_backwards_across_files_does_not_panic() {
        let a = kqr(1001, &[(100, b"late")]);
        let b = kqr(1002, &[(10, b"early")]);
        let got: Vec<i64> = source(vec![a, b]).map(|r| r.recv_ts_us).collect();
        assert_eq!(got, vec![10, 100]);
    }

    #[test]
    fn truncated_tail_is_counted_and_stops_that_file() {
        let full = kqr(1001, &[(1, b"aaaa"), (2, b"bbbb")]);
        let torn = full[..full.len() - 2].to_vec(); // cut inside the last payload
        let mut src = source(vec![torn]);
        let got: Vec<Vec<u8>> = src.by_ref().map(|r| r.payload).collect();
        assert_eq!(got, vec![b"aaaa".to_vec()]);
        assert_eq!(src.warnings(), 1);
    }

    #[test]
    fn empty_header_only_file_yields_nothing() {
        let empty = kqr(1001, &[]);
        let mut src = source(vec![empty]);
        assert!(src.next().is_none());
        assert_eq!(src.warnings(), 0);
    }

    #[test]
    fn stream_ids_are_deduped() {
        let a = kqr(1001, &[(1, b"x")]);
        let b = kqr(1002, &[(2, b"y")]);
        assert_eq!(source(vec![a, b]).stream_ids(), vec![1001, 1002]);
    }
}
