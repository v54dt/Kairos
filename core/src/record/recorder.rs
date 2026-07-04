//! A1 recorder runtime: subscribes to an Aeron stream and archives raw fragments
//! to dated KQR files. The poll thread never touches disk (copy + timestamp +
//! try_send only); a writer thread drains a bounded channel to disk. If the disk
//! stalls, only the recorder's own records drop — never the shared feed.

use std::fs::{File, OpenOptions};
use std::io::{self, BufWriter, Seek, SeekFrom};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{Receiver, TrySendError, sync_channel};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::ipc::aeron::AeronSub;
use crate::record::{FileHeader, RECORD_HEADER_LEN, RecordWriter, record_path, recover_file};

/// Bounded queue between the poll thread and the writer thread. ~seconds of
/// buffer at TW quote rates; overflow drops (counted), never blocks the poll.
const CHANNEL_CAP: usize = 65_536;
const FSYNC_BYTES: usize = 4 * 1024 * 1024;
const FSYNC_INTERVAL: Duration = Duration::from_secs(5);
const POLL_FRAGMENT_LIMIT: usize = 256;

#[derive(Default)]
pub struct Stats {
    pub records: AtomicU64,
    pub bytes: AtomicU64,
    pub drops: AtomicU64,
    pub write_errs: AtomicU64,
}

/// CLOCK_REALTIME microseconds — comparable to the broker `quote_ts_us` carried
/// inside the payload, so feed latency = recv_ts_us − quote_ts_us.
pub fn recv_ts_now_us() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_micros() as i64)
        .unwrap_or(0)
}

/// Civil date from days since the Unix epoch (Howard Hinnant's algorithm).
fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365; // [0, 399]
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32; // [1, 31]
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32; // [1, 12]
    (if m <= 2 { y + 1 } else { y }, m, d)
}

/// `YYYYMMDD` in UTC+8 for a CLOCK_REALTIME µs timestamp (TWSE trading date).
pub fn yyyymmdd_utc8(recv_ts_us: i64) -> String {
    let secs = recv_ts_us.div_euclid(1_000_000) + 8 * 3600;
    let (y, m, d) = civil_from_days(secs.div_euclid(86_400));
    format!("{y:04}{m:02}{d:02}")
}

/// Owns the current dated KQR file for one stream, rotating at the UTC+8 day
/// boundary and fsyncing on a bytes/time policy.
pub struct Rotator {
    out_dir: PathBuf,
    stream_id: i32,
    day: Option<String>,
    writer: Option<RecordWriter<BufWriter<File>>>,
    unsynced_bytes: usize,
    last_sync: Instant,
}

impl Rotator {
    pub fn new(out_dir: PathBuf, stream_id: i32) -> Self {
        Self {
            out_dir,
            stream_id,
            day: None,
            writer: None,
            unsynced_bytes: 0,
            last_sync: Instant::now(),
        }
    }

    /// Append one record, rotating to a new dated file first if the day changed.
    /// Returns the on-disk record size (for stats).
    pub fn write(&mut self, recv_ts_us: i64, payload: &[u8]) -> io::Result<usize> {
        let day = yyyymmdd_utc8(recv_ts_us);
        if self.day.as_deref() != Some(day.as_str()) {
            self.open(&day)?;
        }
        let w = self.writer.as_mut().expect("writer open after open()");
        w.append(recv_ts_us, payload)?;
        let n = RECORD_HEADER_LEN + payload.len();
        self.unsynced_bytes += n;
        if self.unsynced_bytes >= FSYNC_BYTES || self.last_sync.elapsed() >= FSYNC_INTERVAL {
            self.sync()?;
        }
        Ok(n)
    }

    /// Flush buffered bytes and fsync the underlying file.
    pub fn sync(&mut self) -> io::Result<()> {
        if let Some(w) = self.writer.as_mut() {
            w.flush()?;
            w.get_ref().get_ref().sync_data()?; // BufWriter -> File
        }
        self.unsynced_bytes = 0;
        self.last_sync = Instant::now();
        Ok(())
    }

    fn open(&mut self, day: &str) -> io::Result<()> {
        self.sync()?; // flush the previous day before switching
        let path = record_path(&self.out_dir, self.stream_id as u32, day);
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let writer = if path.exists() {
            // Same-day restart: drop any torn tail, then append after the header.
            let valid = recover_file(&path).map_err(io::Error::other)?;
            let mut f = OpenOptions::new().read(true).write(true).open(&path)?;
            f.set_len(valid)?;
            f.seek(SeekFrom::End(0))?;
            RecordWriter::from_writer(BufWriter::new(f))
        } else {
            let f = OpenOptions::new()
                .create(true)
                .write(true)
                .truncate(true)
                .open(&path)?;
            RecordWriter::create(
                BufWriter::new(f),
                &FileHeader::new(self.stream_id as u32, recv_ts_now_us()),
            )?
        };
        self.writer = Some(writer);
        self.day = Some(day.to_owned());
        self.unsynced_bytes = 0;
        self.last_sync = Instant::now();
        Ok(())
    }
}

/// Drains the channel to dated files until all senders drop (recorder stopping).
fn write_loop(rx: Receiver<(i64, Vec<u8>)>, out_dir: PathBuf, stream_id: i32, stats: Arc<Stats>) {
    let mut rot = Rotator::new(out_dir, stream_id);
    while let Ok((ts, bytes)) = rx.recv() {
        match rot.write(ts, &bytes) {
            Ok(n) => {
                stats.records.fetch_add(1, Ordering::Relaxed);
                stats.bytes.fetch_add(n as u64, Ordering::Relaxed);
            }
            Err(e) => {
                stats.write_errs.fetch_add(1, Ordering::Relaxed);
                eprintln!("kairos-recordd: write error (stream {stream_id}): {e}");
            }
        }
    }
    if let Err(e) = rot.sync() {
        eprintln!("kairos-recordd: final sync error (stream {stream_id}): {e}");
    }
}

fn idle_backoff(idle: &mut u32) {
    *idle = idle.saturating_add(1);
    if *idle < 10 {
        std::hint::spin_loop();
    } else if *idle < 20 {
        std::thread::yield_now();
    } else {
        std::thread::sleep(Duration::from_micros(50));
    }
}

/// Records one Aeron stream to `out_dir` until `stop` is set. Connects the
/// subscription on the calling thread (Aeron handles are not `Send`) and spawns
/// a writer thread. On connect failure the process exits (a recorder that can't
/// reach the feed is a hard failure the supervisor should restart).
pub fn run_stream(
    stream_id: i32,
    aeron_dir: Option<String>,
    out_dir: PathBuf,
    stop: Arc<AtomicBool>,
    stats: Arc<Stats>,
) {
    let sub = match AeronSub::connect(aeron_dir.as_deref(), stream_id) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("kairos-recordd: FATAL aeron connect failed (stream {stream_id}): {e:?}");
            std::process::exit(1);
        }
    };
    let (tx, rx) = sync_channel::<(i64, Vec<u8>)>(CHANNEL_CAP);
    let writer = {
        let stats = stats.clone();
        std::thread::spawn(move || write_loop(rx, out_dir, stream_id, stats))
    };

    let mut idle: u32 = 0;
    let mut last_err: Option<Instant> = None;
    while !stop.load(Ordering::Relaxed) {
        let n = sub.poll(
            |data| match tx.try_send((recv_ts_now_us(), data.to_vec())) {
                Ok(()) => {}
                Err(TrySendError::Full(_)) => {
                    stats.drops.fetch_add(1, Ordering::Relaxed);
                }
                Err(TrySendError::Disconnected(_)) => {}
            },
            POLL_FRAGMENT_LIMIT,
        );
        match n {
            Ok(0) => idle_backoff(&mut idle),
            Ok(_) => idle = 0,
            Err(e) => {
                if last_err.is_none_or(|t| t.elapsed() >= Duration::from_secs(5)) {
                    eprintln!("kairos-recordd: poll error (stream {stream_id}): {e:?}");
                    last_err = Some(Instant::now());
                }
                idle_backoff(&mut idle);
            }
        }
    }
    drop(tx); // close the channel so the writer drains and exits
    let _ = writer.join();
}

#[cfg(test)]
mod tests {
    use std::io::Write;

    use super::*;
    use crate::record::{RecordReader, valid_prefix_len};

    #[test]
    fn civil_date_vectors() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        // 2026-07-04 = 20638 days after the epoch.
        assert_eq!(civil_from_days(20_638), (2026, 7, 4));
    }

    // Inverse of civil_from_days (Howard Hinnant): (y,m,d) -> days since epoch.
    fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
        let y = if m <= 2 { y - 1 } else { y };
        let era = if y >= 0 { y } else { y - 399 } / 400;
        let yoe = (y - era * 400) as u64;
        let m_off = if m > 2 { m - 3 } else { m + 9 } as u64;
        let doy = (153 * m_off + 2) / 5 + (d as u64 - 1);
        let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        era * 146_097 + doe as i64 - 719_468
    }

    // CLOCK_REALTIME µs for the given UTC+8 wall-clock date/hour (hh in 0..24
    // stays on the same UTC+8 day).
    fn ts_utc8(y: i64, m: u32, d: u32, hh: i64) -> i64 {
        (days_from_civil(y, m, d) * 86_400 + hh * 3600 - 8 * 3600) * 1_000_000
    }

    #[test]
    fn yyyymmdd_utc8_rolls_at_local_midnight() {
        assert_eq!(yyyymmdd_utc8(ts_utc8(2026, 7, 4, 0)), "20260704"); // 00:00 UTC+8
        assert_eq!(yyyymmdd_utc8(ts_utc8(2026, 7, 4, 23)), "20260704"); // 23:00 UTC+8
        assert_eq!(yyyymmdd_utc8(ts_utc8(2026, 7, 3, 23)), "20260703"); // just before the roll
    }

    fn tmp_dir(tag: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("kairos-rec-{}-{}", std::process::id(), tag));
        let _ = std::fs::remove_dir_all(&p);
        p
    }

    #[test]
    fn rotator_writes_and_reads_back() {
        let dir = tmp_dir("rw");
        let mut rot = Rotator::new(dir.clone(), 1001);
        let payloads: &[&[u8]] = &[b"aaa", b"", b"cccc"];
        for (i, p) in payloads.iter().enumerate() {
            rot.write(ts_utc8(2026, 7, 4, 2 + i as i64), p).unwrap();
        }
        rot.sync().unwrap();

        let path = record_path(&dir, 1001, "20260704");
        let data = std::fs::read(&path).unwrap();
        assert_eq!(valid_prefix_len(&data).unwrap(), data.len());
        let (hdr, r) = RecordReader::open(&data[..]).unwrap();
        assert_eq!(hdr.stream_id, 1001);
        let got: Vec<Vec<u8>> = r.map(|x| x.unwrap().payload).collect();
        assert_eq!(got, vec![b"aaa".to_vec(), b"".to_vec(), b"cccc".to_vec()]);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn rotator_creates_one_file_per_day() {
        let dir = tmp_dir("rot");
        let mut rot = Rotator::new(dir.clone(), 1001);
        rot.write(ts_utc8(2026, 7, 4, 3), b"day1").unwrap();
        rot.write(ts_utc8(2026, 7, 5, 3), b"day2").unwrap();
        rot.sync().unwrap();

        for (day, payload) in [("20260704", b"day1"), ("20260705", b"day2")] {
            let data = std::fs::read(record_path(&dir, 1001, day)).unwrap();
            let (_, r) = RecordReader::open(&data[..]).unwrap();
            let got: Vec<Vec<u8>> = r.map(|x| x.unwrap().payload).collect();
            assert_eq!(got, vec![payload.to_vec()]);
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn restart_appends_after_recovering_torn_tail() {
        let dir = tmp_dir("restart");
        {
            let mut rot = Rotator::new(dir.clone(), 1001);
            rot.write(ts_utc8(2026, 7, 4, 3), b"first").unwrap();
            rot.sync().unwrap();
        }
        // Simulate a crash mid-write: append a garbage (torn) record tail.
        let path = record_path(&dir, 1001, "20260704");
        {
            let mut f = OpenOptions::new().append(true).open(&path).unwrap();
            f.write_all(&[0xAB; 7]).unwrap(); // < RECORD_HEADER_LEN, torn
        }
        // Restart: same-day write should recover the tail and append cleanly.
        {
            let mut rot = Rotator::new(dir.clone(), 1001);
            rot.write(ts_utc8(2026, 7, 4, 4), b"second").unwrap();
            rot.sync().unwrap();
        }
        let data = std::fs::read(&path).unwrap();
        assert_eq!(valid_prefix_len(&data).unwrap(), data.len());
        let (_, r) = RecordReader::open(&data[..]).unwrap();
        let got: Vec<Vec<u8>> = r.map(|x| x.unwrap().payload).collect();
        assert_eq!(got, vec![b"first".to_vec(), b"second".to_vec()]);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
