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
    std::string controller_query_bin;
    std::string work_dir;
    FairsynConfig fc;
    GR1_spec full_spec;
    bool verbose = true;
    bool parallel = false;
    // for debugging
    bool keep_tmp = false;
    bool no_prune = false;
    std::string shared_arena_path;
};

inline bool extract_winning_region_bdd(const std::string& controller_query_bin, const std::string& bdd_file)
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

inline uint32_t to_mask(const std::vector<int>& v) {
    uint32_t m = 0; for (int i : v) m |= (1u << i); return m;
}
inline bool is_subset_mask(uint32_t sub, uint32_t sup) {
    return (sub & sup) == sub;
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

/// implementing Algorithm 1 from the paper
// also allows for parallel execution of the loops 
inline int run_synthesis_loop(Game& game, const SynthesisConfig& cfg, OptLib& lib)
{
    namespace fs = std::filesystem;
    fs::create_directories(cfg.work_dir);

    const int m = game.n_a;
    const int n = game.n_g;
    const int n_orig = game.num_states;

    const auto& owner = game.get_state_space()->get_owner();
    std::vector<int> eve_v, adam_v;
    for (int v = 0; v < n_orig; v++) {
        if (owner[v] == 0) eve_v.push_back(v);
        else adam_v.push_back(v);
    }
    const int n_eve = static_cast<int>(eve_v.size());
    const int n_adam = n_orig - n_eve;
    std::vector<int> play_id(n_orig,-1), choose_id(n_orig,-1), show_id(n_orig,-1);
    for (int i=0; i < n_eve; i++) play_id[eve_v[i]] = i;
    for (int i=0; i < n_adam; i++) choose_id[adam_v[i]] = n_eve+i;
    for (int v=0; v < n_orig; v++) show_id[v] = n_orig + v;

    auto g_seqs = decreasing_subsets(n);
    auto a_seqs = increasing_subsets(m);

    auto fmt = [](const std::vector<int>& v) {
        if (v.empty()) return std::string("empty");
        std::string s;
        for (int i=0;i<(int)v.size();i++) { if(i) s+=","; s+=std::to_string(v[i]); }
        return s;
    };

    using Clock = std::chrono::steady_clock;
    std::chrono::duration<double, std::milli> synth_wall_ms{0};
    int synth_calls = 0;
    int synth_failures = 0;
    int success_count = 0;
    int skipped_full = 0;
    int skipped_triv = 0;

    // ── Shared helpers ──────────────────────────────────────────────────────
    // Single shared temp dir —> reused across all subgames
    // If realizable -> move outputs to dedicated subgame dir
    // If unrealizable -> typically delete tmp and use for next subgame
    const std::string tmp_dir = cfg.work_dir + "/tmp_synth";
    const std::string tmp_spec = tmp_dir + "/spec.txt";
    const std::string tmp_cfg = tmp_dir + "/config.json";
    const std::string tmp_out = tmp_dir + "/output.txt";
    const std::string tmp_bdd = tmp_dir + "/controller.bdd";
    const std::string tmp_hoa = tmp_dir + "/rabin.hoa";
    fs::create_directories(tmp_dir);

    std::vector<std::pair<uint32_t,uint32_t>> realized_masks;
    struct RealizedDir { uint32_t a; uint32_t g; std::string dir; };
    std::vector<RealizedDir> realized_dirs;
    std::set<int> last_Z;

    // Run fairsyn for one (a_tilde, g_tilde) pair and store if realizable.
    // Returns true if stored successfully.
    auto run_one = [&](const std::vector<int>& a_tilde, const std::vector<int>& g_tilde,
                        const std::set<int>& w_verts = {}) -> bool
    {
        uint32_t a_mask = to_mask(a_tilde);
        uint32_t g_mask = to_mask(g_tilde);
        // Dominated checks happen in the outer loop (sequential) or post-synthesis (parallel) - not inside run_one itself

        std::string label = "A{"+fmt(a_tilde)+"}_G{"+fmt(g_tilde)+"}";
        if (cfg.verbose) std::cout << "[subgame] " << label << "\n";

        try {
            write_obliging_spec(game, cfg.full_spec, a_tilde, g_tilde, w_verts, tmp_spec);
            std::string win_verts_path = fs::absolute(tmp_dir + "/winning_vertices.txt").string();
            write_config_json(cfg.shared_arena_path, fs::absolute(tmp_spec).string(),
                              fs::absolute(tmp_out).string(), fs::absolute(tmp_hoa).string(),
                              fs::absolute(tmp_bdd).string(), win_verts_path,
                              cfg.fc, tmp_cfg);
        } catch (const std::exception& e) {
            std::cerr << "  [warn] write failed: " << e.what() << "\n";
            return false;
        }

        fs::remove(tmp_bdd);
        fs::remove(tmp_hoa);
        fs::remove(std::string(tmp_bdd) + ".meta");
        fs::remove(tmp_dir + "/winning_vertices.txt");

        std::string cmd = cfg.fairsyn_bin + " \"" + tmp_cfg + "\"";
        if (cfg.verbose) std::cout << "  $ " << cmd << "\n";
        std::cout << std::flush;  // keep log lines ordered before the child's output
        auto t0 = Clock::now();
        int rc = std::system(cmd.c_str());
        synth_wall_ms += Clock::now() - t0;
        synth_calls++;
        if (rc != 0) {
            synth_failures++;
            // WEXITSTATUS extracts the real exit code from the status returned by system()
            int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
            std::cerr << "  [fail] fairsyn exited with code " << exit_code
                      << " (raw status=" << rc << ")\n"
                      << "  [fail] config: " << tmp_cfg << "\n"
                      << "  [fail] arena:  " << cfg.shared_arena_path << "\n"
                      << "  [fail] spec:   " << tmp_spec << "\n";
            return false;
        }

        // BDD realizability check
        if (!extract_winning_region_bdd(cfg.controller_query_bin, tmp_bdd)) {
            synth_failures++;
            if (cfg.verbose)
                std::cout << "  [unrealizable] BDD is zero — skipping.\n";
            return false;
        }

        // Move outputs to permanent subgame directory
        std::string sub_dir  = cfg.work_dir + "/" + label;
        std::string bdd_perm = sub_dir + "/controller.bdd";
        std::string hoa_perm = sub_dir + "/rabin.hoa";
        try {
            fs::create_directories(sub_dir);
            fs::copy_file(tmp_bdd, bdd_perm, fs::copy_options::overwrite_existing);
            std::string tmp_meta = tmp_bdd + ".meta";
            if (fs::exists(tmp_meta))
                fs::copy_file(tmp_meta, bdd_perm+".meta",
                              fs::copy_options::overwrite_existing);
            fs::copy_file(tmp_hoa, hoa_perm, fs::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            std::cerr << "  [warn] move failed: " << e.what() << "\n";
            return false;
        }

        try {
            lib.store(a_tilde, g_tilde,
                      cfg.controller_query_bin, bdd_perm, hoa_perm);
            success_count++;
            realized_masks.emplace_back(a_mask, g_mask);
            realized_dirs.push_back({a_mask, g_mask, sub_dir});
            last_Z.clear();
            {
                std::ifstream zf(tmp_dir + "/winning_vertices.txt");
                int zv;
                while (zf >> zv)
                    if (zv >= 0 && zv < (int)eve_v.size()) last_Z.insert(zv);
            }
            // Persist Z next to the controller: Gamma(v, B) lookup at play time needs to
            // know from which vertices this (A~, G~) strategy is winning
            {
                std::ofstream zo(sub_dir + "/eve_winning.txt");
                for (int zv : last_Z) zo << zv << "\n";
            }
            if (cfg.verbose)
                std::cout << "  [ok] stored: " << label << "\n";
            return true;
        } catch (const std::exception& e) {
            std::cerr << "  [warn] store failed: " << e.what() << "\n";
            return false;
        }
    };

    // A skipped (A~, G~) is still saved when some realized entry (A0, G0) dominates it
    // That strategy is also winning for (A~, G~), so its controller + automaton are copied
    // into the A~->G~ folder and registered in the library. No synth call
    // Return true if an alias was stored
    auto store_alias = [&](const std::vector<int>& a_tilde, const std::vector<int>& g_tilde) -> bool
    {
        uint32_t a_mask = to_mask(a_tilde), g_mask = to_mask(g_tilde);
        const RealizedDir* dom = nullptr;
        for (const auto& r : realized_dirs) {
            if ((r.a & a_mask) == r.a && (g_mask & r.g) == g_mask) {
                if (!dom || __builtin_popcount(r.g) < __builtin_popcount(dom->g)) dom = &r;
            }
        }
        if (!dom) return false;
        std::string label   = "A{"+fmt(a_tilde)+"}_G{"+fmt(g_tilde)+"}";
        std::string sub_dir = cfg.work_dir + "/" + label;
        std::string bdd_perm = sub_dir + "/controller.bdd";
        std::string hoa_perm = sub_dir + "/rabin.hoa";
        try {
            fs::create_directories(sub_dir);
            fs::copy_file(dom->dir + "/controller.bdd", bdd_perm, fs::copy_options::overwrite_existing);
            if (fs::exists(dom->dir + "/controller.bdd.meta"))
                fs::copy_file(dom->dir + "/controller.bdd.meta", bdd_perm + ".meta",
                              fs::copy_options::overwrite_existing);
            fs::copy_file(dom->dir + "/rabin.hoa", hoa_perm, fs::copy_options::overwrite_existing);
            if (fs::exists(dom->dir + "/eve_winning.txt"))
                fs::copy_file(dom->dir + "/eve_winning.txt", sub_dir + "/eve_winning.txt",
                              fs::copy_options::overwrite_existing);
            lib.store(a_tilde, g_tilde, cfg.controller_query_bin, bdd_perm, hoa_perm);
            success_count++;
            realized_dirs.push_back({a_mask, g_mask, sub_dir});
            if (cfg.verbose)
                std::cout << "[alias] " << label << " saved from " << fs::path(dom->dir).filename().string()
                          << " (no synthesis call)\n";
            return true;
        } catch (const std::exception& e) {
            std::cerr << "  [warn] alias store failed for " << label << ": " << e.what() << "\n";
            return false;
        }
    };

    // Always synthesize the full game to get W
    // This provides the guaranteed fallback for all other subgames at runtime
    std::vector<int> full_a(m); std::iota(full_a.begin(), full_a.end(), 0);
    std::vector<int> full_g(n); std::iota(full_g.begin(), full_g.end(), 0);

    if (cfg.verbose)
        std::cout << "[synth] Synthesizing full game (A=all, G=all) first...\n";
    bool full_game_ok = run_one(full_a, full_g);  // empty w_verts for full-game itself
    if (!full_game_ok)
        std::cerr << "[warn] Full game (all_a→all_g) is unrealizable — "
                  << "runtime fallback will not work correctly.\n";

    // Read W from the winning_vertices.txt fairsyn just wrote
    std::set<int> W;
    {
        std::string win_file = tmp_dir + "/winning_vertices.txt";
        std::string win_perm = cfg.work_dir + "/winning_vertices.txt";
        std::ifstream wf(win_file);
        if (wf) {
            int v; while (wf >> v) W.insert(v);
            
            {
                const auto* ss0 = game.get_state_space();
                const auto& own0 = ss0->get_owner();
                int nn = game.num_states;
                std::vector<int> eve0, adam0;
                for (int u = 0; u < nn; u++) (own0[u] == 0 ? eve0 : adam0).push_back(u);
                int ne = (int)eve0.size();
                std::vector<int> pid(nn, -1), cid(nn, -1);
                for (int i = 0; i < ne; i++) pid[eve0[i]] = i;
                for (int i = 0; i < (int)adam0.size(); i++) cid[adam0[i]] = ne + i;
                std::set<int> cand;
                for (int u : adam0) cand.insert(cid[u]);
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (int u : adam0) {
                        int c = cid[u];
                        if (!cand.count(c)) continue;
                        bool ok = W.count(nn + u) > 0;            // show(u)
                        for (int x : ss0->get_post(u)) {
                            if (!ok) break;
                            int dst = (own0[x] == 0) ? pid[x] : cid[x];
                            ok = (own0[x] == 0) ? W.count(dst) > 0 : cand.count(dst) > 0;
                        }
                        if (!ok) { cand.erase(c); changed = true; }
                    }
                }
                for (int c : cand) W.insert(c);
            }
            std::ofstream wp(win_perm);
            for (int v2 : W) wp << v2 << "\n";
            if (cfg.verbose)
                std::cout << "[synth] Winning region |W|=" << W.size()
                          << " transformed vertices.\n";
        } else {
            std::cerr << "[warn] Could not read winning_vertices.txt — "
                      << "G(in_W) constraint will not be applied.\n";
        }
    }

    // ========================================================================
    // SEQUENTIAL MODE
    // ========================================================================
    if (!cfg.parallel) {
        // Coverage bookkeeping for Algorithm 1: covered[A'] = Eve vertices v (play IDs)
        // for which some realized (A~, G~) with A~ subset of A' already provides Gamma(v, A').
        std::set<int> T;                                   // Eve vertices of W
        for (int i = 0; i < (int)eve_v.size(); i++) if (W.count(i)) T.insert(i);
        std::vector<std::set<int>> covered(size_t(1) << m);
        auto add_cov = [&](uint32_t amask) {
            for (uint32_t b = 0; b < (1u << m); b++)
                if ((b & amask) == amask) covered[b].insert(last_Z.begin(), last_Z.end());
        };
        auto is_cov = [&](uint32_t amask) {
            for (int v : T) if (!covered[amask].count(v)) return false;
            return true;
        };
        auto all_cov = [&]() {
            for (uint32_t b = 0; b < (1u << m); b++) if (!is_cov(b)) return false;
            return true;
        };
        bool halt = false;
        if (full_game_ok) add_cov(to_mask(full_a));        // full game covers all of W for A
        for (const auto& g_tilde : g_seqs) {
            if (halt) break;
            if (g_tilde.empty()) {
                // G~ = empty only requires staying in W (Condition (c)); the full-game
                // strategy already does that, so it serves as Gamma(v, .) for every
                // vertex that no nonempty G~ covers. No synth call needed
                skipped_triv += static_cast<int>(a_seqs.size());
                continue;
            }

            for (const auto& a_tilde : a_seqs) {
                // Skip the full-game subgame — already done
                if (a_tilde == full_a && g_tilde == full_g) continue;

                uint32_t a_mask = to_mask(a_tilde);
                if (!cfg.no_prune && full_game_ok && !T.empty() && is_cov(a_mask)) {
                    if (!store_alias(a_tilde, g_tilde) && cfg.verbose)
                        std::cout << "[prune] A{" << fmt(a_tilde) << "}_G{" << fmt(g_tilde)
                                  << "} — every Eve vertex of W already covered for this A. Skipping.\n";
                    skipped_full++;
                    continue;
                }

                if (run_one(a_tilde, g_tilde, W)) {
                    add_cov(a_mask);
                    if (all_cov()) { halt = true; break; }
                }
            }
        }
    }

    // ========================================================================
    // PARALLEL MODE
    // ========================================================================
    else {
        if (cfg.verbose)
            std::cout << "[parallel] Writing input files for all subgames...\n";

        std::vector<SubgameTask> tasks;
        for (const auto& g_tilde : g_seqs) {
            if (g_tilde.empty()) { skipped_triv += static_cast<int>(a_seqs.size()); continue; }
            for (const auto& a_tilde : a_seqs) {
                if (a_tilde == full_a && g_tilde == full_g) continue;
                std::string label = "A{"+fmt(a_tilde)+"}_G{"+fmt(g_tilde)+"}";
                std::string par_tmp = cfg.work_dir + "/tmp_" + label;
                SubgameTask task;
                task.a_tilde = a_tilde; task.g_tilde = g_tilde;
                task.label = label;
                task.spec_f = par_tmp + "/spec.txt";
                task.cfg_f = par_tmp + "/config.json";
                task.bdd_f = par_tmp + "/controller.bdd";
                task.hoa_f = par_tmp + "/rabin.hoa";
                std::string par_out = par_tmp + "/output.txt";
                try {
                    fs::create_directories(par_tmp);
                    write_obliging_spec(game, cfg.full_spec, a_tilde, g_tilde, W, task.spec_f);
                    write_config_json(cfg.shared_arena_path,
                                      fs::absolute(task.spec_f).string(),
                                      fs::absolute(par_out).string(),
                                      fs::absolute(task.hoa_f).string(),
                                      fs::absolute(task.bdd_f).string(),
                                      fs::absolute(par_tmp + "/winning_vertices.txt").string(),
                                      cfg.fc, task.cfg_f);
                    tasks.push_back(std::move(task));
                } catch (const std::exception& e) {
                    std::cerr << "[warn] write failed for " << label << ": " << e.what() << "\n";
                }
            }
        }

        unsigned int max_threads = std::max(1u, std::thread::hardware_concurrency());
        if (cfg.verbose)
            std::cout << "[parallel] Running " << tasks.size()
                      << " synthesis calls across " << max_threads << " threads.\n";

        std::vector<int> return_codes(tasks.size(), -1);
        std::mutex print_mutex;
        std::atomic<int> task_idx{0};

        auto worker = [&]() {
            while (true) {
                int idx = task_idx.fetch_add(1);
                if (idx >= (int)tasks.size()) break;
                const auto& task = tasks[idx];
                std::string cmd = cfg.fairsyn_bin + " \"" + task.cfg_f + "\"";
                std::cout << std::flush;
                auto t0 = Clock::now();
                int rc = std::system(cmd.c_str());
                auto t1 = Clock::now();
                {
                    std::lock_guard<std::mutex> lk(print_mutex);
                    synth_wall_ms += t1 - t0;
                    synth_calls++;
                    if (rc != 0) synth_failures++;
                    if (cfg.verbose)
                        std::cout << "  [" << (rc==0?"ok":"fail") << "] " << task.label << "\n";
                }
                return_codes[idx] = rc;
            }
        };

        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < max_threads; i++) threads.emplace_back(worker);
        for (auto& t : threads) t.join();

        for (int i = 0; i < (int)tasks.size(); i++) {
            if (return_codes[i] != 0) continue;
            const auto& task = tasks[i];

            if (!extract_winning_region_bdd(cfg.controller_query_bin, task.bdd_f)) {
                if (cfg.verbose)
                    std::cout << "  [unrealizable] " << task.label << " — BDD is zero.\n";
                synth_failures++;
                continue;
            }

            std::string sub_dir  = cfg.work_dir + "/" + task.label;
            std::string bdd_perm = sub_dir + "/controller.bdd";
            std::string hoa_perm = sub_dir + "/rabin.hoa";
            try {
                fs::create_directories(sub_dir);
                fs::copy_file(task.bdd_f, bdd_perm, fs::copy_options::overwrite_existing);
                std::string tmp_meta = task.bdd_f + ".meta";
                if (fs::exists(tmp_meta))
                    fs::copy_file(tmp_meta, bdd_perm+".meta",
                                  fs::copy_options::overwrite_existing);
                fs::copy_file(task.hoa_f, hoa_perm, fs::copy_options::overwrite_existing);

                lib.store(task.a_tilde, task.g_tilde,
                          cfg.controller_query_bin, bdd_perm, hoa_perm);
                success_count++;
                {
                    // Eve play IDs of this entry's winning region (see sequential run_one)
                    std::ifstream zf(fs::path(task.bdd_f).parent_path() / "winning_vertices.txt");
                    std::ofstream zo(sub_dir + "/eve_winning.txt");
                    int zv;
                    while (zf >> zv)
                        if (zv >= 0 && zv < (int)eve_v.size()) zo << zv << "\n";
                }
                if (cfg.verbose)
                    std::cout << "  [stored] " << task.label << "\n";
            } catch (const std::exception& e) {
                std::cerr << "[warn] store failed for " << task.label << ": " << e.what() << "\n";
            }
        }
    }

    if (cfg.verbose && (skipped_full + skipped_triv) > 0)
        std::cout << "[prune] Skipped " << skipped_triv
                  << " trivial (G~=empty) + " << skipped_full << " dominated\n";

    double total_ms = synth_wall_ms.count();
    int successes = synth_calls - synth_failures;
    std::cout << "\n[timing] Synthesis calls    : " << synth_calls   << "\n"
                 "[timing] Realizable         : " << successes     << "\n"
                 "[timing] Unrealizable       : " << synth_failures << "\n"
                 "[timing] Total wall time    : "
              << std::fixed << std::setprecision(1) << total_ms << " ms\n";
    if (successes > 0)
        std::cout << "[timing] Avg per realizable : "
                  << std::setprecision(1) << total_ms/successes << " ms\n";
    if (cfg.parallel)
        std::cout << "[timing] (parallel — wall time reflects longest thread)\n";
    std::cout << "\n";

    {
        auto all_keys = lib.all_keys();
        if (!all_keys.empty()) {
            auto best = std::max_element(all_keys.begin(), all_keys.end(),
                [](const SubgameKey& a, const SubgameKey& b) {
                    if (a.g_indices.size() != b.g_indices.size())
                        return a.g_indices.size() < b.g_indices.size();
                    return a.a_indices.size() > b.a_indices.size();
                });
            std::cout << "\n[best]  Most guarantees: " << best->g_indices.size()
                      << "/" << n << " with " << best->a_indices.size()
                      << "/" << m << " assumptions\n"
                      << "        Subgame: A{" << fmt(best->a_indices)
                      << "}_G{" << fmt(best->g_indices) << "}\n";
        }
    }
    std::cout << "\n";

    if (!cfg.keep_tmp) {
        fs::remove_all(tmp_dir);
        if (cfg.verbose)
            std::cout << "[synth] Temp dir removed: " << tmp_dir << "\n";
    } else {
        std::cout << "[synth] Temp dir kept (--skip-delete): " << tmp_dir << "\n";
    }

    return success_count;
}