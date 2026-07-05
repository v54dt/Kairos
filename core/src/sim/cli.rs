//! kairos-sim argument parsing — a pure `parse` over the arg vector so every
//! subcommand and flag is unit-testable without a process. The binary turns the
//! resulting `Command` into orchestration.

use std::path::PathBuf;

/// Namespace + binary overrides shared by every subcommand. Empty/`None` means
/// "use the sim default"; the resolver applies precedence.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Opts {
    pub symbols: Vec<String>,
    pub aeron_dir: Option<String>,
    pub quote_sock: Option<String>,
    pub order_sock: Option<String>,
    pub hubd: Option<String>,
    pub bin_dir: Option<String>,
    pub prob: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Command {
    Up(Opts),
    Replay {
        sources: Vec<PathBuf>,
        speed: Option<f64>,
        opts: Opts,
    },
    Down(Opts),
    Status(Opts),
}

const USAGE: &str = "usage: kairos-sim <up|replay|down|status> [options]\n  \
    up                         bring up an isolated sim pipeline (needs --symbols)\n  \
    replay <FILE|DIR>...       bring up + replay KQR tapes/dirs (needs --symbols)\n  \
    down                       tear down any running sim children (idempotent)\n  \
    status                     report what is up\n\
    options:\n  \
    --symbols A,B,...           symbols the sim hub fills (required for up/replay)\n  \
    --speed N                   replay acceleration factor (>0; default realtime)\n  \
    --prob                      use the ProbQueue fill model (default conservative)\n  \
    --aeron-dir DIR             override the sim Aeron dir\n  \
    --quote-sock PATH           override the sim quote socket\n  \
    --order-sock PATH           override the sim order socket\n  \
    --hubd PATH                 path to kairos_sim_hubd\n  \
    --bin-dir DIR               dir holding kairos-driver/kairos-core/kairos-replayd";

fn parse_symbols(spec: &str, out: &mut Vec<String>) {
    for s in spec.split(',').map(str::trim).filter(|s| !s.is_empty()) {
        out.push(s.to_owned());
    }
}

/// Parse args (excluding argv[0]). Errors on an unknown subcommand/flag, a missing
/// flag value, a non-positive `--speed`, a missing replay source, or empty symbols
/// where a hub must run.
pub fn parse(args: &[String]) -> anyhow::Result<Command> {
    let (sub, rest) = args
        .split_first()
        .ok_or_else(|| anyhow::anyhow!("no subcommand\n{USAGE}"))?;

    let mut opts = Opts::default();
    let mut sources: Vec<PathBuf> = Vec::new();
    let mut speed: Option<f64> = None;

    let mut it = rest.iter();
    while let Some(a) = it.next() {
        let mut next = || {
            it.next()
                .cloned()
                .ok_or_else(|| anyhow::anyhow!("{a} needs a value"))
        };
        match a.as_str() {
            "--symbols" => parse_symbols(&next()?, &mut opts.symbols),
            "--speed" => speed = Some(next()?.parse::<f64>()?),
            "--prob" => opts.prob = true,
            "--aeron-dir" => opts.aeron_dir = Some(next()?),
            "--quote-sock" => opts.quote_sock = Some(next()?),
            "--order-sock" => opts.order_sock = Some(next()?),
            "--hubd" => opts.hubd = Some(next()?),
            "--bin-dir" => opts.bin_dir = Some(next()?),
            "-h" | "--help" => anyhow::bail!("{USAGE}"),
            _ if a.starts_with('-') => anyhow::bail!("unknown flag {a}\n{USAGE}"),
            _ => sources.push(PathBuf::from(a)),
        }
    }

    if let Some(s) = speed
        && (s <= 0.0 || !s.is_finite())
    {
        anyhow::bail!("--speed must be a positive factor, got {s}");
    }

    match sub.as_str() {
        "up" => {
            require_symbols(&opts)?;
            Ok(Command::Up(opts))
        }
        "replay" => {
            require_symbols(&opts)?;
            if sources.is_empty() {
                anyhow::bail!("replay needs at least one KQR file or dir");
            }
            Ok(Command::Replay {
                sources,
                speed,
                opts,
            })
        }
        "down" => Ok(Command::Down(opts)),
        "status" => Ok(Command::Status(opts)),
        other => anyhow::bail!("unknown subcommand {other}\n{USAGE}"),
    }
}

fn require_symbols(opts: &Opts) -> anyhow::Result<()> {
    if opts.symbols.is_empty() {
        anyhow::bail!("--symbols is required (the sim hub needs at least one symbol to fill)");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(args: &[&str]) -> Vec<String> {
        args.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn up_with_symbols() {
        let c = parse(&v(&["up", "--symbols", "2330,2317"])).unwrap();
        match c {
            Command::Up(o) => assert_eq!(o.symbols, vec!["2330", "2317"]),
            _ => panic!("wrong command"),
        }
    }

    #[test]
    fn up_requires_symbols() {
        assert!(parse(&v(&["up"])).is_err());
    }

    #[test]
    fn replay_parses_source_and_speed() {
        let c = parse(&v(&[
            "replay",
            "/tmp/t.kqr",
            "--symbols",
            "2330",
            "--speed",
            "60",
        ]))
        .unwrap();
        match c {
            Command::Replay {
                sources,
                speed,
                opts,
            } => {
                assert_eq!(sources, vec![PathBuf::from("/tmp/t.kqr")]);
                assert_eq!(speed, Some(60.0));
                assert_eq!(opts.symbols, vec!["2330"]);
            }
            _ => panic!("wrong command"),
        }
    }

    #[test]
    fn replay_accepts_multiple_sources() {
        let c = parse(&v(&["replay", "/a.kqr", "/b.kqr", "--symbols", "2330"])).unwrap();
        match c {
            Command::Replay { sources, .. } => {
                assert_eq!(
                    sources,
                    vec![PathBuf::from("/a.kqr"), PathBuf::from("/b.kqr")]
                );
            }
            _ => panic!("wrong command"),
        }
    }

    #[test]
    fn replay_requires_source() {
        assert!(parse(&v(&["replay", "--symbols", "2330"])).is_err());
    }

    #[test]
    fn replay_requires_symbols() {
        assert!(parse(&v(&["replay", "/tmp/t.kqr"])).is_err());
    }

    #[test]
    fn bad_speed_rejected() {
        assert!(parse(&v(&["replay", "/t", "--symbols", "x", "--speed", "0"])).is_err());
        assert!(parse(&v(&["replay", "/t", "--symbols", "x", "--speed", "-1"])).is_err());
        assert!(parse(&v(&["replay", "/t", "--symbols", "x", "--speed", "nan"])).is_err());
    }

    #[test]
    fn down_and_status_need_no_symbols() {
        assert!(matches!(parse(&v(&["down"])).unwrap(), Command::Down(_)));
        assert!(matches!(
            parse(&v(&["status"])).unwrap(),
            Command::Status(_)
        ));
    }

    #[test]
    fn overrides_captured() {
        let c = parse(&v(&[
            "up",
            "--symbols",
            "x",
            "--aeron-dir",
            "/d",
            "--quote-sock",
            "/q",
            "--order-sock",
            "/o",
            "--hubd",
            "/h",
            "--bin-dir",
            "/b",
            "--prob",
        ]))
        .unwrap();
        match c {
            Command::Up(o) => {
                assert_eq!(o.aeron_dir.as_deref(), Some("/d"));
                assert_eq!(o.quote_sock.as_deref(), Some("/q"));
                assert_eq!(o.order_sock.as_deref(), Some("/o"));
                assert_eq!(o.hubd.as_deref(), Some("/h"));
                assert_eq!(o.bin_dir.as_deref(), Some("/b"));
                assert!(o.prob);
            }
            _ => panic!("wrong command"),
        }
    }

    #[test]
    fn unknown_flag_and_subcommand_error() {
        assert!(parse(&v(&["up", "--nope"])).is_err());
        assert!(parse(&v(&["frobnicate"])).is_err());
        assert!(parse(&[]).is_err());
    }

    #[test]
    fn missing_flag_value_errors() {
        assert!(parse(&v(&["up", "--symbols"])).is_err());
    }
}
