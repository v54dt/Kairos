use tokio::io::{AsyncReadExt, AsyncWriteExt};

const MAX_FRAME_LEN: usize = 1 << 20;

pub async fn write_frame<W>(w: &mut W, payload: &[u8]) -> std::io::Result<()>
where
    W: AsyncWriteExt + Unpin,
{
    w.write_all(&(payload.len() as u32).to_le_bytes()).await?;
    w.write_all(payload).await?;
    w.flush().await
}

pub async fn read_frame<R>(r: &mut R) -> std::io::Result<Option<Vec<u8>>>
where
    R: AsyncReadExt + Unpin,
{
    let mut len_buf = [0u8; 4];
    match r.read_exact(&mut len_buf).await {
        Ok(_) => {}
        Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(e) => return Err(e),
    }
    let len = u32::from_le_bytes(len_buf) as usize;
    if len > MAX_FRAME_LEN {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "frame too large",
        ));
    }
    let mut buf = vec![0u8; len];
    r.read_exact(&mut buf).await?;
    Ok(Some(buf))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn frame_roundtrip() {
        let (mut a, mut b) = tokio::io::duplex(256);
        write_frame(&mut a, b"hello-kairos-frame").await.unwrap();
        let got = read_frame(&mut b).await.unwrap().unwrap();
        assert_eq!(got, b"hello-kairos-frame");
    }

    #[tokio::test]
    async fn read_frame_eof_returns_none() {
        let (a, mut b) = tokio::io::duplex(256);
        drop(a);
        assert!(read_frame(&mut b).await.unwrap().is_none());
    }
}
