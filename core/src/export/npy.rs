//! A minimal, byte-exact writer for the NumPy `.npy` v1.0 format: 6-byte magic,
//! a `1.0` version, a u16-LE header length, an ASCII header dict padded to a
//! 64-byte boundary with a trailing newline, then the raw little-endian C-order
//! data buffer. This reproduces `numpy.save` semantics for 1-D arrays so a golden
//! byte string can lock the layout; nothing is hand-rolled beyond what the format
//! defines.

use std::io::{self, Write};

const MAGIC: &[u8] = b"\x93NUMPY";
const ALIGN: usize = 64;

/// Write a 1-D `.npy` v1.0 array.
///
/// `descr` is the exact text of the dtype `descr` value as NumPy would `repr` it:
/// e.g. `'<i8'` for a scalar dtype, or `[('ev', '<u8'), ('exch_ts', '<i8')]` for a
/// structured dtype. `n_rows` is the array length; `data` must be exactly
/// `n_rows * itemsize` bytes in little-endian C order.
pub fn write_npy<W: Write>(w: &mut W, descr: &str, n_rows: usize, data: &[u8]) -> io::Result<()> {
    let header = format!("{{'descr': {descr}, 'fortran_order': False, 'shape': ({n_rows},), }}");
    // magic(6) + version(2) + header-length(2) + header + trailing newline,
    // padded with spaces so the whole preamble is a multiple of 64 bytes.
    let unpadded = MAGIC.len() + 2 + 2 + header.len() + 1;
    let pad = (ALIGN - (unpadded % ALIGN)) % ALIGN;
    let total_header = header.len() + pad + 1;
    w.write_all(MAGIC)?;
    w.write_all(&[0x01, 0x00])?;
    w.write_all(&(total_header as u16).to_le_bytes())?;
    w.write_all(header.as_bytes())?;
    for _ in 0..pad {
        w.write_all(b" ")?;
    }
    w.write_all(b"\n")?;
    w.write_all(data)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn build(descr: &str, n_rows: usize, data: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        write_npy(&mut out, descr, n_rows, data).unwrap();
        out
    }

    #[test]
    fn scalar_i64_matches_numpy_golden() {
        // numpy.save of np.array([1, 2, 3], dtype='<i8').
        let mut data = Vec::new();
        for v in [1i64, 2, 3] {
            data.extend_from_slice(&v.to_le_bytes());
        }
        let got = build("'<i8'", 3, &data);

        let mut want = Vec::new();
        want.extend_from_slice(b"\x93NUMPY\x01\x00");
        want.extend_from_slice(&118u16.to_le_bytes());
        want.extend_from_slice(b"{'descr': '<i8', 'fortran_order': False, 'shape': (3,), }");
        want.extend(std::iter::repeat_n(b' ', 60));
        want.push(b'\n');
        for v in [1i64, 2, 3] {
            want.extend_from_slice(&v.to_le_bytes());
        }
        assert_eq!(got, want);
    }

    #[test]
    fn structured_event_header_is_aligned_and_terminated() {
        // A 1-element structured record with the hftbacktest 64-byte event dtype.
        let descr = "[('ev', '<u8'), ('exch_ts', '<i8'), ('local_ts', '<i8'), \
             ('px', '<f8'), ('qty', '<f8'), ('order_id', '<u8'), ('ival', '<i8'), \
             ('fval', '<f8')]";
        let data = [0u8; 64];
        let got = build(descr, 1, &data);

        // Header length field is the padded header size; preamble is 64-aligned.
        let hlen = u16::from_le_bytes([got[8], got[9]]) as usize;
        let preamble = 10 + hlen;
        assert_eq!(preamble % ALIGN, 0);
        assert_eq!(got[preamble - 1], b'\n');
        // Data follows the aligned preamble, unchanged.
        assert_eq!(&got[preamble..], &data[..]);
        // The header text carries the exact structured descr.
        let header = std::str::from_utf8(&got[10..preamble]).unwrap();
        assert!(header.starts_with(&format!("{{'descr': {descr},")));
    }

    #[test]
    fn empty_array_is_valid() {
        let got = build("'<i8'", 0, &[]);
        let hlen = u16::from_le_bytes([got[8], got[9]]) as usize;
        assert_eq!((10 + hlen) % ALIGN, 0);
        assert_eq!(got.len(), 10 + hlen);
    }
}
