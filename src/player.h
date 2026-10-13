#pragma once

#include "game.h"
#include "hoa_automaton.h"

#include <string>
#include <vector>
#include <set>
#include <map>
#include <utility>
#include <stdexcept>
#include <sstream>
#include <cstdio>
#include <array>

enum class Mode { PLAY, SHOW };

struct ShadowGuess {
    int v_show; // original-arena vertex
    int q_show; // rabin state after show continuation
    int q1;  // rabin state after case-3 yield edge (before show move)
};


using ProductState = std::pair<int, int>; // (arena_state, rabin_state)
using ControllerMap = std::map<ProductState, int>;  // (v, q) -> s
using RabinTransMap = std::map<ProductState, int>;  // (q, v) -> q'

class Eve {
public:
    // explicit Eve(Game& g) : game(g) {};
    explicit Eve(Game& g,
                 const std::string& controller_query_bin,
                 const std::string& bdd_file,
                 const std::string& hoa_file,
                 const std::vector<int>& play_id,
                 const std::vector<int>& choose_id,
                 const std::vector<int>& show_id,
                 const std::map<std::string, std::set<int>>& ap_verts)
        : game_(g), controller_query_bin_(controller_query_bin), bdd_file_(bdd_file),
          hoa_(HoaAutomaton::parse_file(hoa_file)), play_id_(play_id), choose_id_(choose_id),
          show_id_(show_id), ap_verts_(ap_verts), n_orig_(g.num_states)
    {
        build_transformed_to_orig();
        build_ap_valuation_cache();
    }

    struct BddResult { int next_transformed; int next_q; };

    BddResult query_bdd(int transformed_v, int q) const {
        // Call: controller_query <bdd_file> <transformed_v> <q>
        // Output: "<next_transformed> <next_q>" or "-1 -1"
        std::string cmd = controller_query_bin_ + " \"" + bdd_file_ + "\""
                          + " " + std::to_string(transformed_v) + " " + std::to_string(q);
        std::string out = run_cmd(cmd);
        std::istringstream iss(out);
        BddResult r{-1,-1};
        iss >> r.next_transformed >> r.next_q;
        return r;
    }

    // Eve's turn 
    // Returns {orig_next, q_next}, or {-1,-1} if not in winning region.
    // mode determines whether to look in play or show region.
    BddResult get_move(int orig_u, int q, Mode mode) const {
        int tv = (mode == Mode::PLAY) ? play_id_[orig_u] : show_id_[orig_u];
        if (tv < 0) throw std::logic_error(
            "Eve::get_move: no " + std::string(mode==Mode::PLAY?"play":"show")
            + " vertex for original vertex " + std::to_string(orig_u));
        auto r = query_bdd(tv, q);
        if (r.next_transformed == -1) return {-1, -1};
        return {to_orig(r.next_transformed), r.next_q};
    }

    // ── Automaton transition ─────────────────────────────────────────────
    // Compute the next Rabin state after moving to a known transformed
    // target vertex. Uses the HOA automaton directly — no BDD query needed.
    int hoa_step(int q, int transformed_target) const {
        if (transformed_target < 0
            || transformed_target >= (int)ap_val_cache_.size())
            throw std::out_of_range(
                "Eve::hoa_step: transformed vertex "
                + std::to_string(transformed_target) + " out of range");
        return hoa_.step_bits(q, ap_val_cache_[transformed_target]);
    }

    // ── Shadow-guess at an Adam vertex (u ∈ V!, owner==1) ───────────────
    // Computes the hypothetical "what if Adam had yielded" continuation:
    //
    //   1. Case-3 edge: (choose_id[u], q) → (show_id[u], q1)
    //      The automaton advances on seeing show_id[u].
    //   2. Show-strategy query: (show_id[u], q1) → (v_show_transformed, q_show)
    //      Eve's strategy picks the show-region move.
    //
    // The caller then compares Adam's real move to v_show (decoded to
    // original vertex). If they match → mode=SHOW, q=q_show; else →
    // mode=PLAY, q = hoa_step(q, play_id[real_move]).
    ShadowGuess shadow_guess(int orig_u, int q) const {
        int choose_v = choose_id_[orig_u];
        int show_v   = show_id_ [orig_u];
        if (choose_v < 0)
            throw std::logic_error(
                "Eve::shadow_guess: no choose vertex for original vertex "
                + std::to_string(orig_u) + " (is this an Eve vertex?)");

        // Case-3 yield edge: automaton step on show_id[u]
        int q1 = hoa_step(q, show_v);

        // Show-strategy query: Eve picks the show-region move from (show_v, q1)
        auto r = query_bdd(show_v, q1);
        if (r.next_transformed == -1) {
            // Show region unreachable/not in winning set from here — this
            // is unusual but possible if the subgame's winning region
            // doesn't cover this vertex in the show region. The caller
            // should fall back to play mode in this case.
            return {-1, -1, q1};
        }

        int v_show_orig = to_orig(r.next_transformed);
        return {v_show_orig, r.next_q, q1};
    }

    // // ── Automaton step for case-2 (Adam's real move) ─────────────────────
    // // Given Adam's actual original-arena move v (at an Adam vertex in play
    // // mode), computes the new Rabin state via the case-2 edge:
    // //   (choose_id[u], q) → (play_id[v], q_play)
    // // Note: q here is the state BEFORE the shadow-guess yield edge (i.e.,
    // // the same q passed to shadow_guess), not q1 from inside shadow_guess.
    // int q_after_play_move(int orig_v, int q) const {
    //     int play_v = play_id_[orig_v];
    //     if (play_v < 0)
    //         throw std::logic_error(
    //             "Eve::q_after_play_move: no play vertex for original vertex "
    //             + std::to_string(orig_v)
    //             + " (is this an Adam vertex? Adam has no play copy.)");
    //     // HOA automaton step: seeing play_id[v] after being at choose_id[u]
    //     return hoa_step(q, play_v);
    // }

    // ── Realizability check ───────────────────────────────────────────────
    // Uses controller_query's sentinel (-1,-1) input to check if the BDD
    // is satisfiable (winning region nonempty). Returns true if realizable.
    bool is_realizable() const {
        std::string cmd = controller_query_bin_
                          + " \"" + bdd_file_ + "\" -1 -1";
        std::string out = run_cmd(cmd);
        std::istringstream iss(out);
        int result;
        if (!(iss >> result)) return false;
        return result == 1;
    }

    // ── Accessors ─────────────────────────────────────────────────────────
    int n_orig() const { return n_orig_; }
    int hoa_start_state() const { return hoa_.start_state(); }

    // Decode a transformed G'-vertex back to the original arena vertex.
    int to_orig(int transformed_v) const {
        if (transformed_v < 0
            || transformed_v >= (int)transformed_to_orig_.size())
            throw std::out_of_range(
                "Eve::to_orig: transformed vertex "
                + std::to_string(transformed_v) + " out of range");
        int orig = transformed_to_orig_[transformed_v];
        if (orig == -1)
            throw std::out_of_range(
                "Eve::to_orig: transformed vertex "
                + std::to_string(transformed_v)
                + " has no original-arena mapping");
        return orig;
    }

private:
    Game& game_;
    std::string controller_query_bin_;
    std::string bdd_file_;
    HoaAutomaton hoa_;

    std::vector<int> play_id_;
    std::vector<int> choose_id_;
    std::vector<int> show_id_;
    std::map<std::string, std::set<int>> ap_verts_;

    int n_orig_;
    std::vector<int> transformed_to_orig_;  // transformed id → original id

    // Per transformed-vertex AP valuation bitvector (cached once at
    // construction — the AP sets never change for a given subgame).
    // ap_val_cache_[transformed_v][ap_idx] = true iff AP ap_idx holds there.
    std::vector<std::vector<bool>> ap_val_cache_;

    void build_transformed_to_orig() {
        int sz = 2 * n_orig_;
        transformed_to_orig_.assign(sz, -1);
        for (int v = 0; v < n_orig_; v++) {
            if (play_id_[v]   != -1) transformed_to_orig_[play_id_[v]]   = v;
            if (choose_id_[v] != -1) transformed_to_orig_[choose_id_[v]] = v;
            if (show_id_[v]   != -1) transformed_to_orig_[show_id_[v]]   = v;
        }
    }

    // Build ap_val_cache_: for each transformed vertex, a bitvector of
    // which APs (in the HOA automaton's AP ordering) are true there
    // This avoids repeated string-set construction at query time
    void build_ap_valuation_cache() {
        int sz = 2 * n_orig_;

        const int n_aps = hoa_.num_aps();
        ap_val_cache_.assign(sz, std::vector<bool>(n_aps, false));

        for (int idx = 0; idx < n_aps; idx++) {
            const std::string& name = hoa_.ap_names()[idx];
            auto it = ap_verts_.find(name);
            if (it == ap_verts_.end())
                throw std::runtime_error("Eve: HOA AP '" + name + "' has no vertex set (missing winning_vertices.txt, or spec writer and "
                                        "OptLib::build_ap_verts out of sync?)");
            for (int tv : it->second) {
                if (tv < 0 || tv >= sz)
                    throw std::runtime_error("Eve: AP '" + name + "' contains transformed vertex "
                                            + std::to_string(tv) + " outside [0, " + std::to_string(sz) + ")");
                ap_val_cache_[tv][idx] = true;
            }
        }
    }

    // Run a shell command and return its stdout as a string
    // Thin wrapper around popen — called at most a handful of times per
    // game step (3-4 for Adam vertices, 1 for Eve vertices)
    static std::string run_cmd(const std::string& cmd) {
        std::array<char, 256> buf;
        std::string result;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) throw std::runtime_error("Eve::run_cmd: popen failed for: " + cmd);
        while (fgets(buf.data(), (int)buf.size(), pipe))
            result += buf.data();
        pclose(pipe);
        // Strip trailing whitespace/newline
        while (!result.empty() && (result.back()=='\n'||result.back()=='\r'||result.back()==' '))
            result.pop_back();
        return result;
    }
};