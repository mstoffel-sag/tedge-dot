#!/bin/sh
# Check that no release build can include the synthetic `bench` module (the performance
# harness's, bench/ and doc/performance.md):
#
# - no Rust feature list in .goreleaser.yaml or a Dockerfile names `bench`, and `bench` is
#   not a default feature;
# - impl/c/cross/build.sh, the C release configure step, passes -DTDOT_BENCH=OFF after its
#   passthrough arguments, and nothing turns TDOT_BENCH on in it or in a Dockerfile.
set -eu
cd "$(dirname "$0")/.."

python3 - <<'PY'
import re, subprocess, sys, tomllib

errors = []

def feature_lists(text):
    for m in re.finditer(r"--features[= ]+([A-Za-z0-9_,\-\"']+)", text):
        yield m.group(1).strip("\"'").split(",")

dockerfiles = [f for f in subprocess.run(["git", "ls-files"], capture_output=True, text=True,
               check=True).stdout.split() if f.rsplit("/", 1)[-1].startswith("Dockerfile")]
for path in [".goreleaser.yaml", *dockerfiles]:
    text = open(path).read()
    if any("bench" in features for features in feature_lists(text)):
        errors.append(f"{path}: a release feature list names `bench`")
    if re.search(r"TDOT_BENCH=(ON|1|TRUE|YES)", text, re.I):
        errors.append(f"{path}: turns TDOT_BENCH on")

defaults = tomllib.load(open("impl/rust/Cargo.toml", "rb"))["features"]["default"]
if "bench" in defaults:
    errors.append("impl/rust/Cargo.toml: `bench` is a default feature")

build = open("impl/c/cross/build.sh").read()
if re.search(r"TDOT_BENCH=(ON|1|TRUE|YES)", build, re.I):
    errors.append("impl/c/cross/build.sh: turns TDOT_BENCH on")
configure = re.search(r'^cmake -B "\$OUT".*?(?=^cmake --build)', build, re.M | re.S)
step = configure.group(0) if configure else ""
args = step.find('"$@"')
off = step.find("-DTDOT_BENCH=OFF")
if off < 0:
    errors.append("impl/c/cross/build.sh: the release configure step does not pass -DTDOT_BENCH=OFF")
elif args >= 0 and off < args:
    errors.append('impl/c/cross/build.sh: -DTDOT_BENCH=OFF must come after "$@", or an argument can override it')

for e in errors:
    print(f"error: {e}", file=sys.stderr)
sys.exit(1 if errors else 0)
PY
