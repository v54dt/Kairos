pub mod aeron;

use std::time::Duration;

/// Spin → yield → park so an idle Aeron poll loop doesn't burn a core.
pub fn idle_backoff(idle: &mut u32) {
    *idle = idle.saturating_add(1);
    if *idle < 10 {
        std::hint::spin_loop();
    } else if *idle < 20 {
        std::thread::yield_now();
    } else {
        std::thread::sleep(Duration::from_micros(50));
    }
}
