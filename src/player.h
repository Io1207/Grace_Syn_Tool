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

    int hoa_step(int q, int transformed_target) const {
        if (transformed_target < 0
            || transformed_target >= (int)ap_val_cache_.size())
            throw std::out_of_range(
                "Eve::hoa_step: transformed vertex "
                + std::to_string(transformed_target) + " out of range");
        return hoa_.step_bits(q, ap_val_cache_[transformed_target]);
    }

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
            // Show region unreachable/not in winning set from here - this
            // is unusual but possible if the subgame's winning region
            // doesn't cover this vertex in the show region. Fall back to play mode
            return {-1, -1, q1};
        }

        int v_show_orig = to_orig(r.next_transformed);
        return {v_show_orig, r.next_q, q1};
    }

    bool is_realizable() const {
        std::string cmd = controller_query_bin_
                          + " \"" + bdd_file_ + "\" -1 -1";
        std::string out = run_cmd(cmd);
        std::istringstream iss(out);
        int result;
        if (!(iss >> result)) return false;
        return result == 1;
    }

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
    [[maybe_unused]] Game& game_;
    std::string controller_query_bin_;
    std::string bdd_file_;
    HoaAutomaton hoa_;

    std::vector<int> play_id_;
    std::vector<int> choose_id_;
    std::vector<int> show_id_;
    std::map<std::string, std::set<int>> ap_verts_;

    int n_orig_;
    std::vector<int> transformed_to_orig_;  // transformed id → original id

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

    static std::string run_cmd(const std::string& cmd) {
        std::array<char, 256> buf;
        std::string result;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) throw std::runtime_error("Eve::run_cmd: popen failed for: " + cmd);
        while (fgets(buf.data(), (int)buf.size(), pipe))
            result += buf.data();
        pclose(pipe);
        while (!result.empty() && (result.back()=='\n'||result.back()=='\r'||result.back()==' '))
            result.pop_back();
        return result;
    }
};