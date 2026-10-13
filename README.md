# Grace — Adaptive Strategies for GR(1) Games

Prototype implementation of gracefully robust adaptive strategies for GR(1)
games: a static phase that precomputes a library of optimal graceful strategies
(Section 4, Algorithm 1) and a dynamic phase that monitors the environment's
assumptions and probabilistically mixes the library at runtime (Section 5,
Algorithms 2–3).

Build instructions: see [INSTALL.md](INSTALL.md).

## Directory layout

```
Grace/
├── README.md                     this file
├── INSTALL.md                    build and run instructions
├── src/                          Grace C++ sources
│   ├── main.cpp                  CLI: transform / synth / play
│   ├── game.h, game.cpp          game graph, GR(1) spec, parsers
│   ├── fairsyn_writer.h          obliging arena/spec + FairSyn config writer
│   ├── subgame.h                 Strategy Library Synthesis + coverage pruning
│   ├── opt_lib.h                 strategy library, bitmask keys, vertex maps
│   ├── player.h                  single strategy: BDD queries, memory tracking
│   ├── hoa_automaton.h           HOA parser and stepping
│   └── dynamic_policy.h          monitors, weights, sampling, $\lambda$ lookup
│
├── script/                       run-time scripts
│   ├── run_pipeline.sh           synthesis + play for one game
│   ├── batch_play.sh             repeated play runs (traces × seeds), RQ2 reports
│   ├── smoke_test.sh             end-to-end check on a small benchmark
│   └── plot_policy.py            plots and RQ2 convergence metric
│
├── build/                        build definitions (and, after building, outputs)
│   ├── CMakeLists.txt            builds build/bin/Grace from ../src
│   ├── build_all.sh              fetches Owl + FairSyn, builds everything
│   ├── Dockerfile                reproducible build environment
│   ├── requirements.txt          Python packages for plotting
│   ├── cmake/                    (generated) CMake build tree
│   └── bin/Grace             (generated) the main binary
│
├── benchmarks/                   game files used in the evaluation
├── traces/                       Adam strategy files for play / RQ2
│
└── third_party/                  populated by build/build_all.sh (or pre-fetched)
    ├── fairsyn/                  cloned from https://gitlab.com/kmallik/fairsyn.git
    └── owl/                      Owl 21.0, unpacked from owl-linux-amd64-21.0.zip
```

## Source files


| File | Implements |
|---|---|
| `src/main.cpp` | CLI entry point. Parses the game, dispatches the `transform` / `synth` / `play` modes, loads the strategy library from a workdir, runs the play loop and prints the summary |
| `src/game.h`, `src/game.cpp` | Game graph and GR(1) specification data structures; parsers for the supported input formats; in-memory play/choose/show transformation |
| `src/fairsyn_writer.h` | Writes FairSyn inputs: the obliging arena (play/choose/show copies), the obliging specification with the Safe(W) constraint, and the FairSyn config JSON |
| `src/subgame.h` | Static synthesis (Algorithm 1): solves the full game to obtain W, then every subgame in $\lambda$-priority order, skipping subgames that earlier strategies provably cover (coverage pruning); optional parallel execution |
| `src/opt_lib.h` | Strategy library: bitmask subgame keys, per-subgame strategy objects, vertex maps between the original and transformed arenas, AP vertex sets |
| `src/player.h` | One synthesized strategy: BDD queries through `controller_query`, automaton-state tracking via the HOA automaton, and the shadow-guess memory update projecting the obliging-arena strategy onto the original game |
| `src/hoa_automaton.h` | Minimal HOA parser and deterministic transition function (acceptance is not evaluated) |
| `src/dynamic_policy.h` | Adaptive strategy (Algorithms 2–3): asymptotic liveness monitors, intermediate weights and odds, sampling distribution, per-vertex $\lambda$(v, B) lookup, per-subgame memory, Adam trace replay, CSV logging |
| `script/plot_policy.py` | Plots guarantee visits, monitor scores and p_B over time; reports the RQ2 convergence step |


## Scripts

| Script | Purpose |
|---|---|
| `build/build_all.sh` | Downloads Owl 21.0 and clones FairSyn if not present, then builds FairSyn and Grace |
| `script/smoke_test.sh` | Runs synthesis and 100 play steps on a small benchmark and checks the outputs |
| `script/run_pipeline.sh` | `transform`, `synth`, `play` or `all` for one game file. `--help` lists options |
| `script/batch_play.sh` | Play phase for several Adam traces × runs with random seeds; per-run CSV, log, plot and RQ2 report |

## Input formats

**Game file**: The first line is `<num_states> <initial_state>`, followed by
`ARENA` and one line per state: `<id> <player> <succ1,succ2,...>` (player `1` =
Eve, `0` = Adam). Then `ENV_ASSUMPTIONS <k>` / `SYS_GUARANTEES <k>` sections, each with `k` lines `<label> <v1,v2,...>` listing the Büchi vertex set. Alternatively, an `APS` section defines named atomic propositions, and
conjuncts are given as `GF(<boolean formula>)`.

**Adam trace (`--trace`)**: Lines `<from> <to>` fix Adam's move at `<from>`. They can be grouped into phases `phase <name> <start_step> <end_step>`, so the
environment can change behaviour over time. `#` starts a comment.

## Outputs (per game)

```
<game_stem>/
├── shared_arena.txt              obliging arena given to FairSyn
├── winning_vertices.txt          W (winning region of the full game, transformed ids)
├── A{...}_G{...}/                one directory per stored subgame strategy
│   ├── controller.bdd, controller.bdd.meta
│   ├── rabin.hoa
│   └── winning_vertices.txt
├── synth.log                     synthesis log incl. pruning and timing summary
├── play.csv                      play trace, one row per step
├── play.log                      play output
└── play_plot.png                 with --plot
```

**`play.csv` columns**: `step`, `player` (`E`/`A`), `from_v`, `to_v`; on Eve
rows, the sampled assumption set `b_sub` and the deployed strategy `a_sub` /
`g_sub` (`|`-separated indices, `empty` for ∅); the union-monitor score
`w_eta`; per-assumption monitor scores `w_A<i>`; the sampling probability
`p_<B>` of every assumption subset; and `in_G<j>` (1 if the step's target is in
guarantee j)