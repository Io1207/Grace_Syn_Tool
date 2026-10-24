#pragma once

#include "game.h"
#include "fairsyn_writer.h"
#include "opt_lib.h"

#include <vector>
#include <set>
#include <string>
#include <algorithm>
#include <functional>
#include <numeric>
#include <filesystem>
#include <cstdlib>
#include <sys/wait.h>   // WIFEXITED, WEXITSTATUS
#include <iostream>
#include <sstream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <thread>
#include <mutex>
#include <future>
#include <atomic>

inline std::vector<std::vector<int>> increasing_subsets(int n) {
    std::vector<std::vector<int>> result;
    for (int k = 0; k <= n; k++) {
        if (k == 0) { result.push_back({}); continue; }
        std::function<void(int,int,std::vector<int>&)> gen = [&](int start, int rem, std::vector<int>& cur) {
                if (rem == 0) { result.push_back(cur); return; }
                for (int i = start; i <= n - rem; i++) {
                    cur.push_back(i); gen(i+1,rem-1,cur); cur.pop_back();
                }
            };
        std::vector<int> cur; gen(0,k,cur);
    }
    return result;
}

inline std::vector<std::vector<int>> decreasing_subsets(int n) {
    std::vector<std::vector<int>> result;
    for (int k = n; k >= 0; k--) {
        if (k == 0) { result.push_back({}); continue; }
        std::function<void(int,int,std::vector<int>&)> gen = [&](int start, int rem, std::vector<int>& cur) {
                if (rem == 0) { result.push_back(cur); return; }
                for (int i = start; i <= n - rem; i++) {
                    cur.push_back(i); gen(i+1,rem-1,cur); cur.pop_back();
                }
            };
        std::vector<int> cur; gen(0,k,cur);
    }
    return result;
}

struct SynthesisConfig {
    std::string fairsyn_bin;
    /***** FIXED: new field controller_query_bin added to SynthesisConfig *****/
    std::string controller_query_bin; // path to controller_query binary
    std::string work_dir;
    FairsynConfig fc;
    GR1_spec full_spec;
    bool verbose = true;
    bool parallel = false;
    bool keep_tmp = false;
    std::string shared_arena_path;
};

// BDD-based realizability check: calls controller_query <bdd> -1 -1.
// Returns true if the winning region is nonempty.
inline bool extract_winning_region_bdd(const std::string& controller_query_bin,
                                       const std::string& bdd_file)
{
    // Call: controller_query <bdd> -1 -1
    // Returns "1" if winning region is nonempty (realizable), "0" otherwise.
    std::string cmd = controller_query_bin + " \"" + bdd_file + "\" -1 -1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;
    char buf[16] = {};
    if (!fgets(buf, sizeof(buf), pipe)) { pclose(pipe); return false; }
    pclose(pipe);
    // Trim trailing whitespace
    std::string result(buf);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
        result.pop_back();
    // Exact match: avoid false positives if output is "-1 -1" etc.
    return result == "1";
}

struct SubgameTask {
    std::vector<int> a_tilde;
    std::vector<int> g_tilde;
    std::string label;
    std::string arena_f;
    std::string spec_f;
    std::string cfg_f;
    // BDD-mode outputs (replaces ctrl_f/trans_f from text mode)
    std::string bdd_f;      // controller.bdd
    std::string hoa_f;      // rabin.hoa
};

inline int synth_level(const SubgameKey& k, int m, int n) {
    return (n - __builtin_popcount(k.g)) * (m + 1) + __builtin_popcount(k.a);
}

/// implementing Algorithm 1 from the paper
// also allows for parallel execution of the loops 
inline int run_synthesis_loop(Game& game, const SynthesisConfig& cfg, OptLib& lib)
{
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    fs::create_directories(cfg.work_dir);

    const int  m = game.n_a, n = game.n_g, n_orig = game.num_states, n_tv = 2 * n_orig;
    const Mask full_a = full_mask(m), full_g = full_mask(n);

    const auto& owner = game.get_state_space()->get_owner();
    const auto& play_id = lib.get_play_id();
    const auto& show_id = lib.get_show_id();
    std::vector<int> eve_orig;
    for (int v = 0; v < n_orig; v++) if (owner[v] == 0) eve_orig.push_back(v);

    FairsynConfig fc = cfg.fc;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned max_threads = cfg.parallel ? hw : 1u;
    if (fc.n_workers <= 0)
        fc.n_workers = static_cast<int>(std::max(1u, hw / max_threads));
    if (cfg.verbose)
        std::cout << "[synth] fairsyn: ubdd_type=" << fc.ubdd_type << " n_workers=" << 
                     fc.n_workers << " x " << max_threads << " concurrent call(s)\n";

    struct Job {
        SubgameKey key;
        std::string label, tmp_dir, spec_f, cfg_f, out_f, bdd_f, hoa_f, win_f;
        int rc = -1;
    };

    std::chrono::duration<double, std::milli> synth_wall_ms{0};
    int synth_calls = 0, n_unreal = 0, n_failed = 0, n_stored = 0, n_pruned = 0;
    std::mutex mu;

    std::set<int> W;
    std::vector<char> W_bits(n_tv, 0);
    std::map<SubgameKey, std::vector<char>> solved_Z;

    auto make_job = [&](const SubgameKey& k) {
        Job j;
        j.key = k;
        j.label = fmt_key(k);
        j.tmp_dir = cfg.work_dir + "/tmp_" + j.label;
        j.spec_f = j.tmp_dir + "/spec.txt";
        j.cfg_f = j.tmp_dir + "/config.json";
        j.out_f = j.tmp_dir + "/output.txt";
        j.bdd_f = j.tmp_dir + "/controller.bdd";
        j.hoa_f = j.tmp_dir + "/rabin.hoa";
        j.win_f = j.tmp_dir + "/winning_vertices.txt";
        return j;
    };

    // Write spec + config. Spec gets G(in_W) once W is known (i.e. for all but the full game)
    auto prepare = [&](Job& j) -> bool {
        try {
            fs::create_directories(j.tmp_dir);
            for (const std::string& f : {j.bdd_f, j.bdd_f + ".meta", j.hoa_f, j.win_f})
                fs::remove(f);
            write_obliging_spec(game, cfg.full_spec, from_mask(j.key.a), from_mask(j.key.g), W, j.spec_f);
            write_config_json(cfg.shared_arena_path, fs::absolute(j.spec_f).string(), fs::absolute(j.out_f).string(),
                              fs::absolute(j.hoa_f).string(), fs::absolute(j.bdd_f).string(),
                              fs::absolute(j.win_f).string(), fc, j.cfg_f);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "  [warn] " << j.label << ": write failed: " << e.what() << "\n";
            return false;
        }
    };

    auto run = [&](Job& j) {
        std::string cmd = cfg.fairsyn_bin + " \"" + j.cfg_f + "\"";
        auto t0 = Clock::now();
        j.rc = std::system(cmd.c_str());
        auto dt = Clock::now() - t0;
        std::lock_guard<std::mutex> lk(mu);
        synth_wall_ms += dt;
        synth_calls++;
        if (cfg.verbose) std::cout << "  [" << (j.rc == 0 ? "done" : "fail") << "] " << j.label << "\n";
    };

    auto run_all = [&](std::vector<Job>& jobs) {
        unsigned n_threads = max_threads;
        n_threads = std::min<unsigned>(n_threads, static_cast<unsigned>(jobs.size()));
        std::atomic<size_t> next{0};
        auto worker = [&]() {
            for (size_t i; (i = next.fetch_add(1)) < jobs.size(); ) run(jobs[i]);
        };
        if (n_threads <= 1) { worker(); return; }
        std::vector<std::thread> ts;
        for (unsigned t = 0; t < n_threads; t++) ts.emplace_back(worker);
        for (auto& t : ts) t.join();
    };

    auto finish = [&](Job& j) -> bool {
        bool ok = false;
        if (j.rc != 0) {
            n_failed++;
            int code = WIFEXITED(j.rc) ? WEXITSTATUS(j.rc) : -1;
            
            std::cerr << "  [fail] " << j.label << ": fairsyn exit code " << code << " (config: " << j.cfg_f << ")\n";
            
        } else if (!extract_winning_region_bdd(cfg.controller_query_bin, j.bdd_f)) {
            n_unreal++;
            if (cfg.verbose) std::cout << "  [unrealizable] " << j.label << "\n";
        } else {
            const std::string sub_dir  = cfg.work_dir + "/" + j.label;
            const std::string bdd_perm = sub_dir + "/controller.bdd";
            const std::string hoa_perm = sub_dir + "/rabin.hoa";
            const auto ow = fs::copy_options::overwrite_existing;
            try {
                fs::create_directories(sub_dir);
                fs::copy_file(j.bdd_f, bdd_perm, ow);
                if (fs::exists(j.bdd_f + ".meta")) fs::copy_file(j.bdd_f + ".meta", bdd_perm + ".meta", ow);
                fs::copy_file(j.hoa_f, hoa_perm, ow);
                if (fs::exists(j.win_f)) fs::copy_file(j.win_f, sub_dir + "/winning_vertices.txt", ow);
                lib.store(j.key, cfg.controller_query_bin, bdd_perm, hoa_perm);
                n_stored++;
                ok = true;
                if (cfg.verbose) std::cout << "  [ok] stored: " << j.label << "\n";
            } catch (const std::exception& e) {
                std::cerr << "  [warn] " << j.label << ": store failed: " << e.what() << "\n";
            }

            if (ok) {
                try {
                    std::ifstream wf(j.win_f);
                    if (!wf) throw std::runtime_error("winning_vertices file not written");
                    std::vector<char> z(n_tv, 0);
                    int tv;
                    while (wf >> tv) {
                        if (tv < 0 || tv >= n_tv)
                            throw std::runtime_error("winning vertex " + std::to_string(tv) + " out of range");
                        z[tv] = 1;
                    }
                    solved_Z[j.key] = std::move(z);
                } catch (const std::exception& e) {
                    std::cerr << "  [warn] " << j.label << ": " << e.what()
                              << " — excluded from pruning coverage\n";
                }
            }
        }
        if (!cfg.keep_tmp) { std::error_code ec; fs::remove_all(j.tmp_dir, ec); }
        return ok;
    };

    auto is_covered = [&](const SubgameKey& cand) {
        const int lvl = synth_level(cand, m, n);
        std::vector<const std::vector<char>*> earlier;
        for (const auto& [k, z] : solved_Z)
            if (synth_level(k, m, n) < lvl && is_subset(k.a, cand.a)) earlier.push_back(&z);
        if (earlier.empty()) return false;

        for (int u : eve_orig) {
            const int copies[2] = {play_id[u], show_id[u]};
            bool covered = false;
            for (const auto* z : earlier) {
                bool all_in = true;
                for (int tv : copies)
                    if (W_bits[tv] && !(*z)[tv]) { all_in = false; break; }
                if (all_in) { covered = true; break; }
            }
            if (!covered) return false;
        }
        return true;
    };

    {
        std::vector<Job> first{make_job({full_a, full_g})};
        if (cfg.verbose) std::cout << "[synth] Full game " << first[0].label << " (defines W)\n";
        if (prepare(first[0])) { run_all(first); finish(first[0]); }

        auto it = solved_Z.find(first[0].key);
        if (it == solved_Z.end()) {
            std::cerr << "[error] Full game unrealizable or its winning set is unavailable - W undefined, aborting synthesis.\n";
            return n_stored;
        }
        W_bits = it->second;
        for (int tv = 0; tv < n_tv; tv++) if (W_bits[tv]) W.insert(tv);
        if (W.empty()) {
            std::cerr << "[error] W is empty — aborting synthesis.\n";
            return n_stored;
        }
        std::ofstream wp(cfg.work_dir + "/winning_vertices.txt");
        for (int tv : W) wp << tv << "\n";
        lib.set_winning_region(W);

        if (cfg.verbose) std::cout << "[synth] |W| = " << W.size() << " transformed vertices\n";
        if (!W.count(lib.transformed_target(game.init)))
            std::cerr << "[warn] initial vertex " << game.init << " is not in W — the paper's guarantees do not apply from it.\n";
    }

    // Remaining subgames, level by level
    std::map<int, std::vector<SubgameKey>> by_level;
    for (Mask g = 0; g < (Mask(1) << n); g++)
        for (Mask a = 0; a < (Mask(1) << m); a++) {
            if (a == full_a && g == full_g) continue;
            if (g == 0 && a != 0) continue;
            by_level[synth_level({a, g}, m, n)].push_back({a, g});
        }

    for (auto& [lvl, keys] : by_level) {
        std::vector<Job> jobs;
        for (const auto& k : keys) {
            if (is_covered(k)) {
                n_pruned++;
                if (cfg.verbose) std::cout << "  [pruned] " << fmt_key(k) << " — earlier strategies cover W\n";
                continue;
            }
            Job j = make_job(k);
            if (prepare(j)) jobs.push_back(std::move(j));
            else            n_failed++;
        }
        if (jobs.empty()) continue;
        if (cfg.verbose) std::cout << "[synth] level " << lvl << ": " << jobs.size() << " call(s)\n";
        run_all(jobs);
        for (auto& j : jobs) finish(j);
    }

    const int total = 1 + static_cast<int>([&]{ size_t s = 0; for (auto& [l, k] : by_level) s += k.size(); return s; }());
    std::cout << "\n[timing] Subgames total     : " << total << "\n"
                   "[timing] Synthesis calls    : " << synth_calls << "\n"
                   "[timing] Pruned (coverage)  : " << n_pruned << "\n"
                   "[timing] Realizable/stored  : " << n_stored << "\n"
                   "[timing] Unrealizable       : " << n_unreal << "\n"
                   "[timing] Failed             : " << n_failed << "\n"
                   "[timing] Synthesis time     : "
              << std::fixed << std::setprecision(1) << synth_wall_ms.count() << " ms"
              << (cfg.parallel ? " (summed over threads)" : "") << "\n";

    {
        auto all_keys = lib.all_keys();
        if (!all_keys.empty()) {
            auto best = std::max_element(all_keys.begin(), all_keys.end(),
                [](const SubgameKey& x, const SubgameKey& y) {
                    int gx = __builtin_popcount(x.g), gy = __builtin_popcount(y.g);
                    if (gx != gy) return gx < gy;
                    return __builtin_popcount(x.a) > __builtin_popcount(y.a);
                });
            std::cout << "\n[best]  Most guarantees: " << __builtin_popcount(best->g)
                      << "/" << n << " with " << __builtin_popcount(best->a)
                      << "/" << m << " assumptions\n"
                      << "        Subgame: " << fmt_key(*best) << "\n";
        }
    }
    std::cout << "\n";
    return n_stored;
}