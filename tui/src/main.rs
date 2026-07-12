use std::io::IsTerminal;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Result;
use crossterm::event::{Event, EventStream, KeyEventKind};
use futures::StreamExt;

use kairos_tui::app::{self, Cli, Config, Shared, Snapshot, Tab};
use kairos_tui::input::{
    JournalDrill, OverviewKey, RiskKey, ScenState, apply_drill_key, apply_fills_key, apply_halt,
    apply_overview_key, apply_scenarios_key, fills_key, halt_ui, journal_drill_key, open_target,
    overview_key, risk_tab_key, scenarios_key, service_ui, should_quit, spawn_scenario_action,
    tab_for, to_halt_key,
};
use kairos_tui::panels;
use kairos_tui::sources;
use kairos_tui::sources::feed::{self, FeedState};
use kairos_tui::sources::halt::{self, HaltPrompt};
use kairos_tui::sources::scenario_ctl;
use kairos_tui::sources::service::{self, ConfirmPrompt};
use kairos_tui::terminal;

const TICK: Duration = Duration::from_millis(750);
const DRILL_REFRESH: Duration = Duration::from_secs(2);

#[tokio::main]
async fn main() -> Result<()> {
    let cfg = match app::parse_args() {
        Cli::Run(cfg) => cfg,
        Cli::Help => {
            println!("{}", app::USAGE);
            return Ok(());
        }
        Cli::Version => {
            println!("{}", app::version_line());
            return Ok(());
        }
    };

    if !std::io::stdout().is_terminal() {
        eprintln!("kairos-top: not a terminal; run in an interactive TTY (or use --help).");
        std::process::exit(1);
    }

    let socket = kairos_core::uds::path::quote_socket_path();

    let shared = Arc::new(Shared::default());
    let feed_state = Arc::new(Mutex::new(FeedState::default()));

    let blacklist_path = sources::blacklist::resolve_blacklist_path(
        cfg.blacklist_path.as_ref().and_then(|p| p.to_str()),
        std::env::var("KAIROS_BLACKLIST_CSV").ok().as_deref(),
    );
    let kqr_dir = cfg.data_dir.join("kqr");

    tokio::spawn(feed::run(socket, cfg.symbols.clone(), feed_state.clone()));
    tokio::spawn(app::refresh_systemd(shared.clone()));
    tokio::spawn(app::refresh_journal(shared.clone()));
    tokio::spawn(app::refresh_recorder(shared.clone(), cfg.data_dir.clone()));
    tokio::spawn(app::refresh_hub_status(shared.clone()));
    tokio::spawn(app::refresh_scenarios(
        shared.clone(),
        cfg.journal_dir.clone(),
    ));
    tokio::spawn(app::refresh_supervisor(
        shared.clone(),
        cfg.supervisor_sock.clone(),
    ));
    tokio::spawn(app::refresh_timers(shared.clone()));
    tokio::spawn(app::refresh_blacklist(shared.clone(), blacklist_path));
    tokio::spawn(app::refresh_archive(shared.clone(), kqr_dir));
    tokio::spawn(app::refresh_events(shared.clone()));

    let halt_path = halt::hub_halt_path();

    let mut term = terminal::enter()?;
    let res = run(&mut term, &shared, &feed_state, &cfg, halt_path).await;
    terminal::restore()?;
    res
}

async fn run(
    term: &mut terminal::Tui,
    shared: &Shared,
    feed_state: &Mutex<FeedState>,
    cfg: &Config,
    halt_path: Option<PathBuf>,
) -> Result<()> {
    let mut events = EventStream::new();
    let mut tick = tokio::time::interval(TICK);
    let mut tab = Tab::Overview;
    let mut prompt = HaltPrompt::Idle;
    let mut halt_result: Option<String> = None;
    let mut sel: usize = 0;
    let mut fills_sel: usize = 0;
    let mut confirm = ConfirmPrompt::Idle;
    let action_result: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let mut scen = ScenState::default();
    let mut drill: Option<JournalDrill> = None;
    loop {
        tokio::select! {
            _ = tick.tick() => {
                if let Some(d) = drill.as_mut()
                    && d.last_spawn.elapsed() >= DRILL_REFRESH
                {
                    d.spawn_tail();
                }
                let snap = Snapshot::capture(shared, feed_state);
                let ui = halt_ui(&halt_path, &prompt, &halt_result);
                let service = service_ui(sel, &confirm, &action_result);
                let scenario = scen.ui();
                let drill_view = drill.as_ref().map(|d| d.view());
                let mut clamped_rev: Option<usize> = None;
                term.draw(|frame| {
                    panels::render(frame, &snap, cfg, tab, &ui, &service, &scenario, fills_sel);
                    if tab == Tab::Overview && let Some(v) = &drill_view {
                        clamped_rev = Some(panels::render_drill_overlay(frame, v));
                    }
                })?;
                if let (Some(d), Some(r)) = (drill.as_mut(), clamped_rev) {
                    d.rev = r;
                }
            }
            Some(Ok(event)) = events.next() => {
                let key = match &event {
                    Event::Key(k) if k.kind == KeyEventKind::Press => *k,
                    _ => continue,
                };
                let snap = Snapshot::capture(shared, feed_state);
                let mut redraw = false;
                if prompt.is_active() {
                    if let Some(hk) = to_halt_key(&key) {
                        let (next, action) = halt::handle_key(&prompt, hk);
                        prompt = next;
                        if let Some(a) = action {
                            halt_result = Some(apply_halt(a, halt_path.as_deref()));
                        }
                        redraw = true;
                    }
                } else if confirm.is_active() {
                    if let Some(hk) = to_halt_key(&key) {
                        let (next, action) = service::handle_key(&confirm, hk);
                        confirm = next;
                        if let Some(a) = action {
                            let slot = action_result.clone();
                            *slot.lock().unwrap() =
                                Some(format!("{} {} ...", a.verb.as_str(), a.unit));
                            tokio::spawn(async move {
                                let msg = service::run_action(a.verb, &a.unit).await;
                                *slot.lock().unwrap() = Some(msg);
                            });
                        }
                        redraw = true;
                    }
                } else if scen.confirm.is_active() {
                    if let Some(hk) = to_halt_key(&key) {
                        let (next, action) = scenario_ctl::handle_key(&scen.confirm, hk);
                        scen.confirm = next;
                        if let Some(a) = action {
                            spawn_scenario_action(a, cfg.supervisor_sock.clone(), scen.result.clone());
                        }
                        redraw = true;
                    }
                } else if drill.is_some() {
                    if should_quit(&event) {
                        return Ok(());
                    }
                    if let Some(dk) = journal_drill_key(&key) {
                        if apply_drill_key(dk, drill.as_mut().unwrap()) {
                            drill = None;
                        }
                        redraw = true;
                    }
                } else if let Some(ok) = overview_key(tab, &key) {
                    if ok == OverviewKey::OpenJournal {
                        if let Some(unit) =
                            open_target(&prompt, &confirm, &scen.confirm, &snap, sel)
                        {
                            let mut d = JournalDrill::new(unit);
                            d.spawn_tail();
                            drill = Some(d);
                        }
                    } else {
                        apply_overview_key(ok, &mut sel, &mut confirm, &snap);
                    }
                    redraw = true;
                } else if let Some(sk) = scenarios_key(tab, &key) {
                    apply_scenarios_key(sk, &snap, &mut scen);
                    redraw = true;
                } else if let Some(fk) = fills_key(tab, &key) {
                    apply_fills_key(fk, &snap, &mut fills_sel);
                    redraw = true;
                } else if let Some(rk) = risk_tab_key(tab, &key) {
                    if halt_path.is_some() {
                        prompt = match rk {
                            RiskKey::Halt => HaltPrompt::ConfirmHalt(String::new()),
                            RiskKey::Resume => HaltPrompt::ConfirmResume(String::new()),
                        };
                    } else {
                        halt_result = Some("kill switch unavailable (no runtime dir)".to_string());
                    }
                    redraw = true;
                } else if should_quit(&event) {
                    return Ok(());
                } else if let Some(next) = tab_for(&event, tab) {
                    tab = next;
                    redraw = true;
                }
                if redraw {
                    let ui = halt_ui(&halt_path, &prompt, &halt_result);
                    let service = service_ui(sel, &confirm, &action_result);
                    let scenario = scen.ui();
                    let drill_view = drill.as_ref().map(|d| d.view());
                    let mut clamped_rev: Option<usize> = None;
                    term.draw(|frame| {
                        panels::render(frame, &snap, cfg, tab, &ui, &service, &scenario, fills_sel);
                        if tab == Tab::Overview && let Some(v) = &drill_view {
                            clamped_rev = Some(panels::render_drill_overlay(frame, v));
                        }
                    })?;
                    if let (Some(d), Some(r)) = (drill.as_mut(), clamped_rev) {
                        d.rev = r;
                    }
                }
            }
        }
    }
}
