#!/usr/bin/env bash
set -Eeuo pipefail
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBMC_SANITIZE=OFF -DBMC_TSAN=OFF -DBMC_TLS="${BMC_TLS:-OFF}" "$@"
cmake --build build-release --parallel 2
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
package="$stage/bmc-lite-release"
mkdir -p "$package/build-release"
install -m 0755 build-release/bmc-lite "$package/build-release/bmc-lite"
cp -a config deploy tools docs README.md README.en.md "$package/"
tar -C "$stage" -czf bmc-lite-release.tar.gz bmc-lite-release
sha256sum bmc-lite-release.tar.gz > bmc-lite-release.tar.gz.sha256
echo "Created bmc-lite-release.tar.gz and its checksum."
