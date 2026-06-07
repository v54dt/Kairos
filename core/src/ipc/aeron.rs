use std::time::Duration;

use anyhow::Context as _;
use rusteron_client::*;

pub const DEFAULT_STREAM_ID: i32 = 1001;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_OFFER_RETRIES: usize = 5;

fn client(aeron_dir: Option<&str>) -> anyhow::Result<Aeron> {
    let ctx = AeronContext::new().context("aeron context")?;
    if let Some(dir) = aeron_dir {
        ctx.set_dir(&dir.into_c_string())
            .map_err(|e| anyhow::anyhow!("set aeron dir: {e:?}"))?;
    }
    let aeron = Aeron::new(&ctx).context("aeron client")?;
    aeron.start().context("aeron start")?;
    Ok(aeron)
}

pub struct AeronSub {
    _aeron: Aeron,
    subscription: AeronSubscription,
}

impl AeronSub {
    pub fn connect(aeron_dir: Option<&str>, stream_id: i32) -> anyhow::Result<Self> {
        let aeron = client(aeron_dir)?;
        let subscription = aeron
            .add_subscription(
                AERON_IPC_STREAM,
                stream_id,
                Handlers::no_available_image_handler(),
                Handlers::no_unavailable_image_handler(),
                CONNECT_TIMEOUT,
            )
            .map_err(|e| anyhow::anyhow!("add subscription: {e:?}"))?;
        Ok(Self {
            _aeron: aeron,
            subscription,
        })
    }

    pub fn poll<F: FnMut(&[u8])>(
        &self,
        mut on_msg: F,
        fragment_limit: usize,
    ) -> anyhow::Result<i32> {
        self.subscription
            .poll_once(
                move |data: &[u8], _header: AeronHeader| on_msg(data),
                fragment_limit,
            )
            .map_err(|e| anyhow::anyhow!("poll: {e:?}"))
    }
}

pub struct AeronPub {
    _aeron: Aeron,
    publication: AeronPublication,
}

impl AeronPub {
    pub fn connect(aeron_dir: Option<&str>, stream_id: i32) -> anyhow::Result<Self> {
        let aeron = client(aeron_dir)?;
        let publication = aeron
            .add_publication(AERON_IPC_STREAM, stream_id, CONNECT_TIMEOUT)
            .map_err(|e| anyhow::anyhow!("add publication: {e:?}"))?;
        Ok(Self {
            _aeron: aeron,
            publication,
        })
    }

    pub fn offer(&self, data: &[u8]) -> anyhow::Result<()> {
        let mut delay = Duration::from_micros(100);
        for attempt in 0..MAX_OFFER_RETRIES {
            if self
                .publication
                .offer(data, Handlers::no_reserved_value_supplier_handler())
                >= 0
            {
                return Ok(());
            }
            if attempt + 1 < MAX_OFFER_RETRIES {
                std::thread::sleep(delay);
                delay = (delay * 2).min(Duration::from_millis(100));
            }
        }
        anyhow::bail!("offer failed after {MAX_OFFER_RETRIES} retries (backpressure)")
    }
}
