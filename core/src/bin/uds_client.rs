use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

use kairos_core::decode::decode_quote_bytes;
use kairos_core::encode::encode_subscribe;
use kairos_core::uds::frame::{read_frame, write_frame};
use kairos_core::uds::path::quote_socket_path;
use tokio::net::UnixStream;

fn px(mantissa: i64, scale: u8) -> f64 {
    mantissa as f64 / 10f64.powi(scale as i32)
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let symbols: Vec<String> = std::env::args().skip(1).collect();
    if symbols.is_empty() {
        eprintln!("usage: kairos-uds-client <symbol>...");
        std::process::exit(1);
    }

    let mut stream = UnixStream::connect(quote_socket_path()).await?;
    let refs: Vec<&str> = symbols.iter().map(String::as_str).collect();
    write_frame(&mut stream, &encode_subscribe(&refs)).await?;
    println!("kairos-uds-client: subscribed {symbols:?}; waiting for quotes...");

    while let Some(frame) = read_frame(&mut stream).await? {
        match decode_quote_bytes(&frame) {
            Ok(q) => {
                let bid = q.bids.first().map(|l| px(l.price_mantissa, l.price_scale));
                let ask = q.asks.first().map(|l| px(l.price_mantissa, l.price_scale));
                println!(
                    "{:<8} bid={bid:?} ask={ask:?} last={} x{}{}",
                    q.symbol,
                    px(q.last_price, q.last_scale),
                    q.last_volume,
                    if q.is_trial { " (試撮)" } else { "" }
                );
            }
            Err(e) => eprintln!("decode error: {e:?}"),
        }
    }
    println!("kairos-uds-client: connection closed");
    Ok(())
}
