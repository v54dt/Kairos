//! kairos-record-verify — decode-checks a KQR archive (header + every record's
//! frame and CRC) and prints a summary. Exit 0 if fully valid, 1 otherwise.
//!
//! Usage:
//!   kairos-record-verify <FILE.kqr>
//!   zstd -dc FILE.kqr.zst | kairos-record-verify -

use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

use std::fs::File;
use std::io::{self, BufReader, Read};

use kairos_core::record::verify_reader;

fn main() {
    let path = match std::env::args().nth(1) {
        Some(p) => p,
        None => {
            eprintln!("usage: kairos-record-verify <FILE|->");
            std::process::exit(2);
        }
    };
    let reader: Box<dyn Read> = if path == "-" {
        Box::new(io::stdin().lock())
    } else {
        match File::open(&path) {
            Ok(f) => Box::new(BufReader::new(f)),
            Err(e) => {
                eprintln!("kairos-record-verify: {path}: {e}");
                std::process::exit(1);
            }
        }
    };
    match verify_reader(reader) {
        Ok(s) => {
            println!(
                "kairos-record-verify: {path} OK stream={} records={} bytes={} recv_ts=[{}..{}]",
                s.stream_id,
                s.records,
                s.bytes,
                s.first_recv_ts_us.unwrap_or(0),
                s.last_recv_ts_us.unwrap_or(0),
            );
        }
        Err(e) => {
            eprintln!("kairos-record-verify: {path} FAILED: {e}");
            std::process::exit(1);
        }
    }
}
