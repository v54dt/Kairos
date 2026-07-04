//! KQR v1 archive format: a framed, lossless log of raw Aeron fragments for
//! replay/backtest. One file per stream per day.
//!
//! File layout:
//! ```text
//! header (32B):  magic "KQR1" | version u16 | flags u16 | stream_id u32
//!                | created_ts_us i64 | reserved [u8;12]
//! record (16B + payload):  recv_ts_us i64 | len u32 | crc32c u32 | payload[len]
//! ```
//! `recv_ts_us` is the recorder's local receive time (CLOCK_REALTIME µs); the
//! broker's own timestamp already lives inside the capnp payload. crc32c covers
//! `recv_ts_us ++ len ++ payload` so a torn or corrupt tail is detectable.

use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

pub mod recorder;

pub const MAGIC: [u8; 4] = *b"KQR1";
pub const VERSION: u16 = 1;
pub const HEADER_LEN: usize = 32;
pub const RECORD_HEADER_LEN: usize = 16; // recv_ts_us(8) + len(4) + crc32c(4)
/// A record payload cannot exceed this; a larger length field is treated as
/// corruption (guards against a huge allocation on a torn/garbage length).
pub const MAX_PAYLOAD_LEN: usize = 16 * 1024 * 1024;

#[derive(Debug)]
pub enum RecordError {
    Io(io::Error),
    BadMagic,
    UnsupportedVersion(u16),
    ShortHeader,
    /// A record's frame declares more bytes than remain.
    Truncated,
    /// On-disk data does not match its checksum.
    Crc {
        expected: u32,
        got: u32,
    },
    /// Length field exceeds MAX_PAYLOAD_LEN.
    OversizeLen(u32),
}

impl From<io::Error> for RecordError {
    fn from(e: io::Error) -> Self {
        RecordError::Io(e)
    }
}

impl std::fmt::Display for RecordError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            RecordError::Io(e) => write!(f, "io: {e}"),
            RecordError::BadMagic => write!(f, "bad magic (not a KQR file)"),
            RecordError::UnsupportedVersion(v) => write!(f, "unsupported version {v}"),
            RecordError::ShortHeader => write!(f, "short header"),
            RecordError::Truncated => write!(f, "truncated record"),
            RecordError::Crc { expected, got } => {
                write!(
                    f,
                    "crc mismatch (expected {expected:#010x}, got {got:#010x})"
                )
            }
            RecordError::OversizeLen(n) => write!(f, "oversize record length {n}"),
        }
    }
}

impl std::error::Error for RecordError {}

// --- CRC32C (Castagnoli, reflected, poly 0x82F63B78) ------------------------

const fn crc32c_table() -> [u32; 256] {
    let mut table = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut crc = i as u32;
        let mut j = 0;
        while j < 8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0x82F6_3B78
            } else {
                crc >> 1
            };
            j += 1;
        }
        table[i] = crc;
        i += 1;
    }
    table
}

static CRC32C_TABLE: [u32; 256] = crc32c_table();

fn crc32c_update(mut crc: u32, data: &[u8]) -> u32 {
    for &b in data {
        crc = (crc >> 8) ^ CRC32C_TABLE[((crc ^ b as u32) & 0xFF) as usize];
    }
    crc
}

/// CRC32C of a single buffer.
pub fn crc32c(data: &[u8]) -> u32 {
    !crc32c_update(0xFFFF_FFFF, data)
}

fn record_crc(recv_ts_us: i64, len: u32, payload: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    crc = crc32c_update(crc, &recv_ts_us.to_le_bytes());
    crc = crc32c_update(crc, &len.to_le_bytes());
    crc = crc32c_update(crc, payload);
    !crc
}

// --- File header ------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FileHeader {
    pub version: u16,
    pub stream_id: u32,
    pub created_ts_us: i64,
}

impl FileHeader {
    pub fn new(stream_id: u32, created_ts_us: i64) -> Self {
        Self {
            version: VERSION,
            stream_id,
            created_ts_us,
        }
    }

    pub fn encode(&self) -> [u8; HEADER_LEN] {
        let mut b = [0u8; HEADER_LEN];
        b[0..4].copy_from_slice(&MAGIC);
        b[4..6].copy_from_slice(&self.version.to_le_bytes());
        // [6..8] flags reserved = 0
        b[8..12].copy_from_slice(&self.stream_id.to_le_bytes());
        b[12..20].copy_from_slice(&self.created_ts_us.to_le_bytes());
        // [20..32] reserved = 0
        b
    }

    pub fn decode(buf: &[u8]) -> Result<Self, RecordError> {
        if buf.len() < HEADER_LEN {
            return Err(RecordError::ShortHeader);
        }
        if buf[0..4] != MAGIC {
            return Err(RecordError::BadMagic);
        }
        let version = u16::from_le_bytes(buf[4..6].try_into().unwrap());
        if version != VERSION {
            return Err(RecordError::UnsupportedVersion(version));
        }
        let stream_id = u32::from_le_bytes(buf[8..12].try_into().unwrap());
        let created_ts_us = i64::from_le_bytes(buf[12..20].try_into().unwrap());
        Ok(Self {
            version,
            stream_id,
            created_ts_us,
        })
    }
}

// --- Records ----------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OwnedRecord {
    pub recv_ts_us: i64,
    pub payload: Vec<u8>,
}

/// Writes a KQR header once, then appends records. Generic over the sink so the
/// recorder can wrap a `BufWriter<File>` and tests can use a `Vec<u8>`.
pub struct RecordWriter<W: Write> {
    inner: W,
}

impl<W: Write> RecordWriter<W> {
    pub fn create(mut inner: W, header: &FileHeader) -> io::Result<Self> {
        inner.write_all(&header.encode())?;
        Ok(Self { inner })
    }

    /// Wrap a writer already positioned at the end of a valid KQR file (header
    /// present); used to append after a same-day restart.
    pub fn from_writer(inner: W) -> Self {
        Self { inner }
    }

    pub fn get_ref(&self) -> &W {
        &self.inner
    }

    pub fn append(&mut self, recv_ts_us: i64, payload: &[u8]) -> io::Result<()> {
        let len = payload.len() as u32;
        let crc = record_crc(recv_ts_us, len, payload);
        self.inner.write_all(&recv_ts_us.to_le_bytes())?;
        self.inner.write_all(&len.to_le_bytes())?;
        self.inner.write_all(&crc.to_le_bytes())?;
        self.inner.write_all(payload)?;
        Ok(())
    }

    pub fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }

    pub fn into_inner(self) -> W {
        self.inner
    }
}

/// Reads the header, then yields records. Iterating stops at a clean EOF and
/// surfaces a torn/corrupt tail as `Err`.
pub struct RecordReader<R: Read> {
    inner: R,
}

impl<R: Read> RecordReader<R> {
    /// Reads and validates the file header, returning it plus a reader positioned
    /// at the first record.
    pub fn open(mut inner: R) -> Result<(FileHeader, Self), RecordError> {
        let mut hdr = [0u8; HEADER_LEN];
        if read_full(&mut inner, &mut hdr)? < HEADER_LEN {
            return Err(RecordError::ShortHeader);
        }
        let header = FileHeader::decode(&hdr)?;
        Ok((header, Self { inner }))
    }

    /// Next record: `Ok(Some)` for a valid record, `Ok(None)` at a clean EOF
    /// (record boundary), `Err` for a torn or corrupt record.
    pub fn read_record(&mut self) -> Result<Option<OwnedRecord>, RecordError> {
        let mut rh = [0u8; RECORD_HEADER_LEN];
        match read_full(&mut self.inner, &mut rh)? {
            0 => return Ok(None),
            RECORD_HEADER_LEN => {}
            _ => return Err(RecordError::Truncated),
        }
        let recv_ts_us = i64::from_le_bytes(rh[0..8].try_into().unwrap());
        let len = u32::from_le_bytes(rh[8..12].try_into().unwrap());
        let crc = u32::from_le_bytes(rh[12..16].try_into().unwrap());
        if len as usize > MAX_PAYLOAD_LEN {
            return Err(RecordError::OversizeLen(len));
        }
        let mut payload = vec![0u8; len as usize];
        if read_full(&mut self.inner, &mut payload)? < len as usize {
            return Err(RecordError::Truncated);
        }
        let got = record_crc(recv_ts_us, len, &payload);
        if got != crc {
            return Err(RecordError::Crc { expected: crc, got });
        }
        Ok(Some(OwnedRecord {
            recv_ts_us,
            payload,
        }))
    }
}

impl<R: Read> Iterator for RecordReader<R> {
    type Item = Result<OwnedRecord, RecordError>;
    fn next(&mut self) -> Option<Self::Item> {
        match self.read_record() {
            Ok(Some(rec)) => Some(Ok(rec)),
            Ok(None) => None,
            Err(e) => Some(Err(e)),
        }
    }
}

/// Read up to `buf.len()` bytes, tolerating short reads; returns the count read
/// (< buf.len() only at EOF).
fn read_full<R: Read>(r: &mut R, buf: &mut [u8]) -> io::Result<usize> {
    let mut n = 0;
    while n < buf.len() {
        match r.read(&mut buf[n..]) {
            Ok(0) => break,
            Ok(k) => n += k,
            Err(ref e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(n)
}

// --- Torn-tail recovery + naming --------------------------------------------

/// Byte length of the valid prefix of a KQR buffer: the header plus every leading
/// record whose frame is complete and CRC-valid. A torn or corrupt tail (e.g. a
/// crash mid-write) is excluded. Errors only if the header itself is bad.
pub fn valid_prefix_len(data: &[u8]) -> Result<usize, RecordError> {
    if data.len() < HEADER_LEN {
        return Err(RecordError::ShortHeader);
    }
    FileHeader::decode(&data[..HEADER_LEN])?;
    let mut off = HEADER_LEN;
    while data.len() - off >= RECORD_HEADER_LEN {
        let recv_ts_us = i64::from_le_bytes(data[off..off + 8].try_into().unwrap());
        let len = u32::from_le_bytes(data[off + 8..off + 12].try_into().unwrap()) as usize;
        let crc = u32::from_le_bytes(data[off + 12..off + 16].try_into().unwrap());
        if len > MAX_PAYLOAD_LEN {
            break;
        }
        let payload_start = off + RECORD_HEADER_LEN;
        if data.len() - payload_start < len {
            break; // torn payload
        }
        let payload = &data[payload_start..payload_start + len];
        if record_crc(recv_ts_us, len as u32, payload) != crc {
            break; // corrupt record
        }
        off = payload_start + len;
    }
    Ok(off)
}

/// Scan a file and return the byte length of its valid prefix (see
/// `valid_prefix_len`); the caller truncates the file to this to drop a torn tail.
pub fn recover_file(path: &Path) -> Result<u64, RecordError> {
    let data = std::fs::read(path)?;
    Ok(valid_prefix_len(&data)? as u64)
}

/// Summary of a verified KQR stream.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VerifySummary {
    pub stream_id: u32,
    pub records: u64,
    pub bytes: u64, // total payload bytes
    pub first_recv_ts_us: Option<i64>,
    pub last_recv_ts_us: Option<i64>,
}

/// Reads and validates an entire KQR stream — header plus every record's frame
/// and CRC — returning a summary, or the first error. Used by the ship-time
/// verify step (e.g. `zstd -dc file.kqr.zst | kairos-record-verify -`).
pub fn verify_reader<R: Read>(r: R) -> Result<VerifySummary, RecordError> {
    let (header, reader) = RecordReader::open(r)?;
    let mut s = VerifySummary {
        stream_id: header.stream_id,
        records: 0,
        bytes: 0,
        first_recv_ts_us: None,
        last_recv_ts_us: None,
    };
    for rec in reader {
        let rec = rec?;
        s.records += 1;
        s.bytes += rec.payload.len() as u64;
        s.first_recv_ts_us.get_or_insert(rec.recv_ts_us);
        s.last_recv_ts_us = Some(rec.recv_ts_us);
    }
    Ok(s)
}

/// Archive path for a stream on a given day:
/// `<base>/kqr/<yyyymmdd>/s<stream>-<yyyymmdd>.kqr`.
pub fn record_path(base_dir: &Path, stream_id: u32, yyyymmdd: &str) -> PathBuf {
    base_dir
        .join("kqr")
        .join(yyyymmdd)
        .join(format!("s{stream_id}-{yyyymmdd}.kqr"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32c_known_vector() {
        // Standard CRC-32C check value for "123456789".
        assert_eq!(crc32c(b"123456789"), 0xE306_9283);
        assert_eq!(crc32c(b""), 0);
    }

    #[test]
    fn header_round_trip() {
        let h = FileHeader::new(1001, 1_720_000_000_000_000);
        let decoded = FileHeader::decode(&h.encode()).unwrap();
        assert_eq!(decoded, h);
        assert_eq!(decoded.version, VERSION);
        assert_eq!(decoded.stream_id, 1001);
    }

    #[test]
    fn header_errors() {
        assert!(matches!(
            FileHeader::decode(&[0u8; 10]),
            Err(RecordError::ShortHeader)
        ));
        let mut b = FileHeader::new(1001, 0).encode();
        b[0] = b'X';
        assert!(matches!(FileHeader::decode(&b), Err(RecordError::BadMagic)));
        let mut b = FileHeader::new(1001, 0).encode();
        b[4] = 9; // version = 9
        assert!(matches!(
            FileHeader::decode(&b),
            Err(RecordError::UnsupportedVersion(9))
        ));
    }

    fn build(records: &[(i64, &[u8])]) -> Vec<u8> {
        let mut w = RecordWriter::create(Vec::new(), &FileHeader::new(1001, 42)).unwrap();
        for (ts, p) in records {
            w.append(*ts, p).unwrap();
        }
        w.into_inner()
    }

    #[test]
    fn record_round_trip() {
        let recs: &[(i64, &[u8])] = &[(10, b"hello"), (20, b""), (30, &[0xff, 0x00, 0x42])];
        let buf = build(recs);
        let (hdr, mut r) = RecordReader::open(&buf[..]).unwrap();
        assert_eq!(hdr.stream_id, 1001);
        for (ts, p) in recs {
            let got = r.read_record().unwrap().unwrap();
            assert_eq!(got.recv_ts_us, *ts);
            assert_eq!(got.payload, *p);
        }
        assert!(r.read_record().unwrap().is_none()); // clean EOF
        assert_eq!(valid_prefix_len(&buf).unwrap(), buf.len());
    }

    #[test]
    fn iterator_yields_all() {
        let buf = build(&[(1, b"a"), (2, b"bb"), (3, b"ccc")]);
        let (_, r) = RecordReader::open(&buf[..]).unwrap();
        let got: Vec<_> = r.map(|x| x.unwrap().payload).collect();
        assert_eq!(got, vec![b"a".to_vec(), b"bb".to_vec(), b"ccc".to_vec()]);
    }

    #[test]
    fn torn_tail_recovers_prefix() {
        let buf = build(&[(1, b"aaaa"), (2, b"bbbb")]);
        // Byte length of everything up to (but not including) the last record.
        let prefix = build(&[(1, b"aaaa")]).len();
        // Truncate somewhere inside the last record's payload.
        let torn = &buf[..buf.len() - 2];
        assert_eq!(valid_prefix_len(torn).unwrap(), prefix);

        let (_, mut r) = RecordReader::open(torn).unwrap();
        assert_eq!(r.read_record().unwrap().unwrap().payload, b"aaaa");
        assert!(matches!(r.read_record(), Err(RecordError::Truncated)));
    }

    #[test]
    fn corrupt_crc_is_caught() {
        let mut buf = build(&[(1, b"aaaa"), (2, b"bbbb")]);
        let prefix = build(&[(1, b"aaaa")]).len();
        let last = buf.len() - 1;
        buf[last] ^= 0xFF; // flip a byte in the last record's payload
        assert_eq!(valid_prefix_len(&buf).unwrap(), prefix);

        let (_, mut r) = RecordReader::open(&buf[..]).unwrap();
        assert_eq!(r.read_record().unwrap().unwrap().payload, b"aaaa");
        assert!(matches!(r.read_record(), Err(RecordError::Crc { .. })));
    }

    #[test]
    fn oversize_len_is_rejected() {
        let mut buf = RecordWriter::create(Vec::new(), &FileHeader::new(1001, 0))
            .unwrap()
            .into_inner();
        buf.extend_from_slice(&5i64.to_le_bytes()); // recv_ts
        buf.extend_from_slice(&(MAX_PAYLOAD_LEN as u32 + 1).to_le_bytes()); // len
        buf.extend_from_slice(&0u32.to_le_bytes()); // crc
        assert_eq!(valid_prefix_len(&buf).unwrap(), HEADER_LEN);
        let (_, mut r) = RecordReader::open(&buf[..]).unwrap();
        assert!(matches!(r.read_record(), Err(RecordError::OversizeLen(_))));
    }

    #[test]
    fn path_layout() {
        let p = record_path(Path::new("data"), 1001, "20260704");
        assert_eq!(p, Path::new("data/kqr/20260704/s1001-20260704.kqr"));
    }

    #[test]
    fn verify_reader_summarizes_and_catches_corruption() {
        let buf = build(&[(10, b"hello"), (20, b"world!!")]);
        let s = verify_reader(&buf[..]).unwrap();
        assert_eq!(s.stream_id, 1001);
        assert_eq!(s.records, 2);
        assert_eq!(s.bytes, 12);
        assert_eq!(s.first_recv_ts_us, Some(10));
        assert_eq!(s.last_recv_ts_us, Some(20));

        let mut bad = buf.clone();
        let last = bad.len() - 1;
        bad[last] ^= 0xFF;
        assert!(matches!(
            verify_reader(&bad[..]),
            Err(RecordError::Crc { .. })
        ));
    }
}
