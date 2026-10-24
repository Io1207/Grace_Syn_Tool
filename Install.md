# Building and running Grace

## Requirements

- Linux x86-64 (Owl is used as its prebuilt linux-amd64 distribution)
- C++17 compiler: GCC ≥ 9 or Clang ≥ 10
- CMake ≥ 3.16
- `unzip`
- `git` and `curl` (or `wget`) — only if `third_party/` is not already populated
- Genie's library build tools (for CUDD, Sylvan and cpphoafparser): `pkg-config`, `autoconf`, `automake`,
  `libtool`, `wget`, GMP headers. On Debian/Ubuntu:
  `sudo apt install pkg-config autoconf automake libtool wget libgmp-dev`
- Optional, for plots: Python ≥ 3.8 with the packages in `build/requirements.txt`

## Option A — Docker (Recommended)

```bash
docker build -f build/Dockerfile -t grace .   # from the root
docker run -it --rm grace
# inside the container:
./script/smoke_test.sh
```

## Option B — Native build

```bash
pip install -r build/requirements.txt
chmod +x build/build_all.sh script/*.sh   # in case execute permissions were lost (e.g. extracted from a zip)
./build/build_all.sh
./script/smoke_test.sh
```

`build/build_all.sh` uses local copies in `third_party/` when present and fetches
them otherwise, so an archived artifact with `third_party/` included builds
without network access. To populate `third_party/` without building (e.g. to
package the artifact): `FETCH_ONLY=1 ./build/build_all.sh`.

FairSyn is built on [Genie](https://gitlab.com/kmallik/genie). The script clones Genie into
`third_party/genie` and runs Genie's `install.sh`, which downloads and builds CUDD, Sylvan (v1.7.0) and
cpphoafparser (0.99.2) into `third_party/genie-deps` (no `sudo` needed). This step always needs network
access, even when `third_party/` was pre-fetched with `FETCH_ONLY=1`.

### Manual build

```bash
# 1. Owl 21.0 (Linux amd64 release)
curl -fLO https://github.com/owl-toolkit/owl/releases/download/release-21.0/owl-linux-amd64-21.0.zip
unzip owl-linux-amd64-21.0.zip -d third_party/
mv third_party/owl-*21.0 third_party/owl

# 2. Genie and its libraries (CUDD, Sylvan, cpphoafparser)
git clone https://gitlab.com/kmallik/genie.git third_party/genie
(cd third_party/genie && bash ./install.sh "$PWD/../genie-deps/")

# 3. FairSyn (genie_HOME and CMAKE_PREFIX_PATH must be absolute paths)
git clone --recursive https://gitlab.com/kmallik/fairsyn.git third_party/fairsyn
cmake -S third_party/fairsyn -B third_party/fairsyn/build -DCMAKE_BUILD_TYPE=Release \
      -Dgenie_HOME="$PWD/third_party/genie" -DCMAKE_PREFIX_PATH="$PWD/third_party/genie-deps"
cmake --build third_party/fairsyn/build -j

# 4. Grace
cmake -S build -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j
```

### Expected result

| Binary | Path |
|---|---|
| Grace | `build/bin/grace` |
| FairSyn synthesis | `third_party/fairsyn/build/bin/controller_synthesis` |
| FairSyn controller queries | `third_party/fairsyn/build/bin/controller_query` |
| Owl | `third_party/owl/bin/owl` |

The scripts use these paths by default. To use tools installed elsewhere, set
`GRACE_FAIRSYN`, `GRACE_CTRL_QUERY` and `GRACE_OWL` (the Owl
`bin/` directory).

## Smoke test

```bash
./script/smoke_test.sh [game_file]
```

Runs synthesis plus 100 play steps on a small benchmark (a few minutes) and prints `PASS` on success

## Running experiments

**Synthesis + one play run:**

```bash
./script/run_pipeline.sh benchmarks/<game>.txt --mode all --steps <num_steps> --plot
```

Synthesis statistics (subgames, calls, pruned, realizable/unrealizable, time)
are at the end of `benchmarks/<game>/synth.log`.

**Convergence (RQ2):** after synthesis, run several seeded play runs:

```bash
./script/batch_play.sh benchmarks/<game>.txt traces/<trace>.txt --runs <num_runs> --steps <steps> --plot
cat benchmarks/batch_results/<game>/*/rq2.txt
```

`--ainf` is the set of assumptions the trace fulfils in the long run
(`|`-separated indices, or `empty`). Each `rq2.txt` reports the first step after which p_{A_inf} stays above 0.99.

**Paper defaults:** $\alpha$ = 0.5 and $\lambda$ = 0.9 for all monitors, resampling at every
Eve step (`--k 1`), $\varepsilon$ = 1e-9. These are the default values used

**Resource limits:** the paper used a 1-hour timeout (on the synthesis calls alone) and a 5 GB memory limit for synthesis. The scripts do not enforce these

> Note: the paper's timeout is only over the synthesis calls, some example take a long time in the rabiniser call. The entries reported in Section 6 of the paper correspond to time taken for synthesis calls alone

**Parallel synthesis:** `--parallel` runs independent subgames concurrently and splits cores between them. Use `--n-workers <N>` to set FairSyn threads per call and `--ubdd-type` to choose the BDD backend (default 2, Sylvan + parallel)

## Diagnostics

| Message | Meaning |
|---|---|
| `[error] Full game unrealizable … aborting` | The specification has no graceful winning region; nothing to adapt. |
| `[warn] initial vertex … is not in W` | The adaptive strategy's guarantees do not apply from this start. |
| `[warn] q convention mismatch` | The BDD and HOA disagree on the automaton state; please report. |
| `[warn] … store failed` | A synthesized subgame could not be loaded; it will be missing from the library. |
| `[error]` `$\Lambda$(v=…, B=…) undefined` | No library strategy is winning at the current state (v $\notin$ W or a state desync) |