#!/bin/bash
set -e

echo "=== Kairos dev environment setup ==="

echo "[1/3] Rust components..."
rustup component add clippy rustfmt
cargo install cargo-watch 2>/dev/null || true

echo "[2/3] Tool versions:"
capnp --version 2>/dev/null || echo "  capnp MISSING"
cmake --version | head -1
g++ --version | head -1

echo "[3/3] Cargo check (core)..."
if [ -f core/Cargo.toml ]; then
    (cd core && cargo check 2>/dev/null) || echo "  cargo check skipped (deps may need network)"
fi

echo "=== Setup complete ==="
echo "Aeron media driver: run 'aeronmd' before starting core/sidecar."
