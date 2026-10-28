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

class Eve {
public:
    explicit Eve(Game& g, const std::string& controller_query_bin, const std::string& bdd_file,
                 const std::string& hoa_file, const std::vector<int>& play_id, const std::vector<int>& choose_id,
                 const std::vector<int>& show_id, const std::map<std::string, std::set<int>>& ap_verts)
        : game_(g), controller_query_bin_(controller_query_bin), bdd_file_(bdd_file),
         hoa_(HoaAutomaton::parse_file(hoa_file)), play_id_(play_id), choose_id_(choose_id),
         show_id_(show_id), ap_verts_(ap_verts), n_orig_(g.num_states)
    {
        build_transformed_to_orig();
        build_ap_valuation_cache();
    }

    struct BddResult { int next_transformed; int next_q; };

    BddResult query_bdd(int transformed_v, int q) const {
        std::string cmd = controller_query_bin_ + " \"" + bdd_file_ + "\"" + " " + std::to_string(transformed_v)
                          + " " + std::to_string(q);
        std::string out = run_cmd(cmd);
        std::istringstream iss(out);
        BddResult r{-1,-1};
        iss >> r.next_transformed >> r.next_q;
        return r;
    }

    BddResult get_move(int orig_u, int q, Mode mode) const {
        int tv = (mode == Mode::PLAY) ? play_id_[orig_u] : show_id_[orig_u];
        if (tv < 0) throw std::logic_error( "Eve::get_move: no " + std::string(mode==Mode::PLAY?"play":"show")
                                         + " vertex for original vertex " + std::to_string(orig_u));
        auto r = query_bdd(tv, q);
        if (r.next_transformed == -1) return {-1, -1};
        return {to_orig(r.next_transformed), r.next_q};
    }

    int hoa_step(int q, int transformed_target) const {
        if (transformed_target < 0 || transformed_target >= (int)ap_val_cache_.size())
            throw std::out_of_range( "Eve::hoa_step: transformed vertex "
                                    + std::to_string(transformed_target) + " out of range" );
        return hoa_.step_bits(q, ap_val_cache_[transformed_target]);
    }

    ShadowGuess shadow_guess(int orig_u, int q) const {
        int choose_v = choose_id_[orig_u];
        int show_v   = show_id_ [orig_u];
        if (choose_v < 0)
            throw std::logic_error( "Eve::shadow_guess: no choose vertex for original vertex "
                                    + std::to_string(orig_u) + " (is this an Eve vertex?)");

        int q1 = hoa_step(q, show_v);

        auto r = query_bdd(show_v, q1);
        if (r.next_transformed == -1) { return {-1, -1, q1}; }

        int v_show_orig = to_orig(r.next_transformed);
        return {v_show_orig, r.next_q, q1};
    }

    int q_after_play_move(int orig_v, int q) const {
        int play_v = play_id_[orig_v];
        if (play_v < 0)
            throw std::logic_error( "Eve::q_after_play_move: no play vertex for original vertex "
                                   + std::to_string(orig_v) + " (is this an Adam vertex? Adam has no play copy.)");
        return hoa_step(q, play_v);
    }

    bool is_realizable() const {
        std::string cmd = controller_query_bin_ + " \"" + bdd_file_ + "\" -1 -1";
        std::string out = run_cmd(cmd);
        std::istringstream iss(out);
        int result;
        if (!(iss >> result)) return false;
        return result == 1;
    }

    int n_orig() const { return n_orig_; }
    int hoa_start_state() const { return hoa_.start_state(); }

    int to_orig(int transformed_v) const {
        if (transformed_v < 0 || transformed_v >= (int)transformed_to_orig_.size())
            throw std::out_of_range( "Eve::to_orig: transformed vertex "
                                    + std::to_string(transformed_v) + " out of range");
        int orig = transformed_to_orig_[transformed_v];
        if (orig == -1)
            throw std::out_of_range( "Eve::to_orig: transformed vertex " + std::to_string(transformed_v)
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
    std::vector<int> transformed_to_orig_;

    std::vector<std::vector<bool>> ap_val_cache_;

    void build_transformed_to_orig() {
        int sz = 2 * n_orig_;
        transformed_to_orig_.assign(sz, -1);
        for (int v = 0; v < n_orig_; v++) {
            if (play_id_[v]!= -1) transformed_to_orig_[play_id_[v]] = v;
            if (choose_id_[v]!= -1) transformed_to_orig_[choose_id_[v]] = v;
            if (show_id_[v]!= -1) transformed_to_orig_[show_id_[v]] = v;
        }
    }

    void build_ap_valuation_cache() {
        int sz = 2 * n_orig_;
        int hoa_n_aps = 0;
        for (const auto& [ap_name, vset] : ap_verts_) {
            int idx = hoa_.ap_index(ap_name);
            if (idx >= hoa_n_aps) hoa_n_aps = idx + 1;
        }
        ap_val_cache_.assign(sz, std::vector<bool>(hoa_n_aps, false));

        for (const auto& [ap_name, vset] : ap_verts_) {
            int ap_idx = hoa_.ap_index(ap_name);
            if (ap_idx < 0) continue;
            for (int tv : vset) {
                if (tv >= 0 && tv < sz && ap_idx < hoa_n_aps)
                    ap_val_cache_[tv][ap_idx] = true;
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