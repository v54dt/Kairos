use std::sync::atomic::Ordering;

use rusteron_media_driver::{AeronDriver, AeronDriverContext};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let ctx =
        AeronDriverContext::new().map_err(|e| anyhow::anyhow!("aeron driver context: {e:?}"))?;
    ctx.set_dir_delete_on_start(true)
        .map_err(|e| anyhow::anyhow!("set dir delete on start: {e:?}"))?;
    let (stop, handle) = AeronDriver::launch_embedded(ctx.clone(), false);
    println!(
        "kairos-driver: Aeron media driver running (dir={}); Ctrl-C to stop",
        ctx.get_dir()
    );

    tokio::signal::ctrl_c().await?;
    println!("kairos-driver: stopping");
    stop.store(true, Ordering::SeqCst);
    let _ = handle.join();
    Ok(())
}
