use std::time::Duration;

use anyhow::Context as _;
use rusteron_client::*;

pub const DEFAULT_STREAM_ID: i32 = 1001;
/// Reverse control stream (core -> sidecar): carries the desired subscription
/// set so the sidecar can dynamically (un)subscribe upstream on concords.
pub const CONTROL_STREAM_ID: i32 = 1002;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_OFFER_RETRIES: usize = 5;

/// Aeron directory to use: an explicit `--aeron-dir` wins, else a non-empty
/// `$KAIROS_AERON_DIR` (so a replay environment can point driver+core+replayd at
/// an isolated dir without code changes), else `None` (Aeron's own default). When
/// both are unset, behavior is unchanged.
pub fn resolve_aeron_dir(explicit: Option<&str>) -> Option<String> {
    if let Some(d) = explicit {
        return Some(d.to_owned());
    }
    std::env::var("KAIROS_AERON_DIR")
        .ok()
        .filter(|d| !d.is_empty())
}

/// Build a started client and report the Aeron directory it actually resolved to
/// (an explicit/env dir, else Aeron's own default) so a caller can probe driver
/// liveness against the same CnC file.
fn client(aeron_dir: Option<&str>) -> anyhow::Result<(Aeron, String)> {
    let ctx = AeronContext::new().context("aeron context")?;
    if let Some(dir) = resolve_aeron_dir(aeron_dir) {
        ctx.set_dir(&dir.as_str().into_c_string())
            .map_err(|e| anyhow::anyhow!("set aeron dir: {e:?}"))?;
    }
    let dir = ctx.get_dir().to_owned();
    let aeron = Aeron::new(&ctx).context("aeron client")?;
    aeron.start().context("aeron start")?;
    Ok((aeron, dir))
}

pub struct AeronSub {
    _aeron: Aeron,
    subscription: AeronSubscription,
    aeron_dir: String,
}

impl AeronSub {
    pub fn connect(aeron_dir: Option<&str>, stream_id: i32) -> anyhow::Result<Self> {
        let (aeron, dir) = client(aeron_dir)?;
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
            aeron_dir: dir,
        })
    }

    /// Whether the media driver's to-driver consumer heartbeat is fresher than
    /// `timeout_ms`. The driver conductor writes this beat every duty cycle regardless
    /// of quote flow, so it reads active on an idle-but-healthy weekend and inactive
    /// only when the driver is gone (heartbeat stale, cleared to NULL, or CnC unmapped).
    pub fn driver_active(&self, timeout_ms: i64) -> bool {
        let Ok(cnc) = AeronCnc::new_on_heap(&self.aeron_dir) else {
            return false;
        };
        match cnc.get_to_driver_heartbeat_ms() {
            Ok(heartbeat_ms) if heartbeat_ms > 0 => {
                Aeron::epoch_clock() - heartbeat_ms <= timeout_ms
            }
            _ => false,
        }
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
        let (aeron, _dir) = client(aeron_dir)?;
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

#[cfg(test)]
mod tests {
    use super::resolve_aeron_dir;

    #[test]
    fn resolve_aeron_dir_precedence() {
        // Explicit always wins, regardless of the env.
        unsafe { std::env::set_var("KAIROS_AERON_DIR", "/env/dir") };
        assert_eq!(
            resolve_aeron_dir(Some("/explicit")).as_deref(),
            Some("/explicit")
        );
        // No explicit -> non-empty env is used.
        assert_eq!(resolve_aeron_dir(None).as_deref(), Some("/env/dir"));
        // Empty env is ignored (treated as unset).
        unsafe { std::env::set_var("KAIROS_AERON_DIR", "") };
        assert_eq!(resolve_aeron_dir(None), None);
        // Unset -> None (unchanged default behavior).
        unsafe { std::env::remove_var("KAIROS_AERON_DIR") };
        assert_eq!(resolve_aeron_dir(None), None);
    }
}
