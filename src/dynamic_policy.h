#pragma once

#include "game.h"
#include "opt_lib.h"

#include <vector>
#include <map>
#include <cmath>
#include <numeric>
#include <random>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <cmath>
#include <iomanip>

inline std::vector<int> active_assumptions(int v, const std::vector<GR1_conjunct>& assumptions) {
    std::vector<int> active;
    for (int i = 0; i < (int)assumptions.size(); i++)
        if (assumptions[i].states.count(v))
            active.push_back(i);
    return active;
}

struct DynamicPolicyParams {
    double alpha = 0.9;
    double alpha_prime = -1.0; // if negative, use alpha_prime = alpha
    double lambda = 0.9;
    double lambda_prime = -1.0; // if negative, use lambda_prime = lambda
    unsigned seed = 42;
    int k = 1;
    double eps = 1e-9;
};

struct Candidate {
    double prob;
    Mask b;
};

struct AdamPhase {
    std::string name;
    int start_step;
    int end_step;
    std::map<int,int> transitions;
};

class AdamStrategy {
public:
    AdamStrategy() = default;

    void load(const std::string& path) {
        phases_.clear();
        global_transitions_.clear();

        std::ifstream f(path);
        if (!f) throw std::runtime_error("Cannot open Adam strategy file: " + path);

        AdamPhase* cur_phase = nullptr;
        bool has_phases = false;
        std::string line;

        while (std::getline(f, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            
            while (!line.empty() && (line.front()==' ' || line.front() == '\t')) line.erase(line.begin());

            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string first; iss >> first;

            if (first == "phase") {
                has_phases = true;
                phases_.push_back({});
                cur_phase = &phases_.back();
                int s, e;
                iss >> cur_phase->name >> s >> e;
                cur_phase->start_step = s;
                cur_phase->end_step   = e;
            } else {
                int from = std::stoi(first), to;
                iss >> to;
                if (has_phases && cur_phase)    cur_phase->transitions[from] = to;
                else    global_transitions_[from] = to;
            }
        }
        std::cerr << "[adam] Loaded strategy: " << (has_phases ? std::to_string(phases_.size()) + " phases" : 
                     "global (" + std::to_string(global_transitions_.size()) + " rules)") << " from " << path << "\n";
    }

    int query(int v, int t) const {
        if (phases_.empty()) {
            auto it = global_transitions_.find(v);
            return it != global_transitions_.end() ? it->second : -1;
        }
        const AdamPhase* active = &phases_.back();
        for (const auto& ph : phases_) {
            if (t >= ph.start_step && t <= ph.end_step) { active = &ph; break; }
        }

        auto it = active->transitions.find(v);
        return it != active->transitions.end() ? it->second : -1;
    }

    bool loaded() const { return !phases_.empty() || !global_transitions_.empty(); }

private:
    std::vector<AdamPhase> phases_;
    std::map<int,int> global_transitions_;
};

class DynamicPolicy {
    public:
        using Params = DynamicPolicyParams;
        using PolicyCandidate = Candidate;

        DynamicPolicy(Game& game, OptLib& lib, Params p = Params{}) : game_(game), lib_(lib), params_(p), rng_(p.seed)
        {
            if (!(p.eps > 0.0 && p.eps < 0.5)) throw std::invalid_argument("DynamicPolicy: eps must be in (0, 0.5)");

            const int m = game.n_a;
            w_A_.assign(m, 1.0);
            alpha_A_.assign(m, p.alpha);

            if (params_.alpha_prime  < 0) params_.alpha_prime  = params_.alpha;
            if (params_.lambda_prime < 0) params_.lambda_prime = params_.lambda;
            for (double x : {params_.alpha, params_.lambda, params_.alpha_prime, params_.lambda_prime})
                if (!(x > 0.0 && x < 1.0))
                    throw std::invalid_argument("DynamicPolicy: alpha/lambda values must be in (0, 1)");

            w_union_ = 1.0;
            alpha_union_ = params_.alpha_prime;

            std::cerr << "[info] monitors: alpha=" << params_.alpha << " lambda=" << params_.lambda;
            if (params_.alpha_prime != params_.alpha || params_.lambda_prime != params_.lambda)
                std::cerr << "  (union override: alpha'=" << params_.alpha_prime
                          << " lambda'=" << params_.lambda_prime << " — deviates from Alg. 3)";
            std::cerr << "  k=" << params_.k << " eps=" << params_.eps << "\n";

            n_subsets_ = Mask(1) << m;
            full_a_ = full_mask(m);

            for (const auto& key : lib.all_keys()) subgame_states_[key] = {};
            init_subgame_states();
            check_initial_q_convention();

            // Lambda priority chain for every B in A (Algorithm 1 order)
            chain_for_B_.resize(n_subsets_);
            for (Mask b = 0; b < n_subsets_; b++) {
                chain_for_B_[b] = build_chain(b);
                if (chain_for_B_[b].empty())
                    throw std::runtime_error("No strategy for B={" + fmt_mask(b) + "} — was A{empty}_G{empty} synthesized?");
            }

            steps_since_resample_ = 0;
            current_candidate_ = {1.0, full_a_};
        }

        void load_adam_strategy(const std::string& path) { adam_strat_.load(path); }

        void set_csv(const std::string& path) {
            csv_.open(path);
            if (!csv_) throw std::runtime_error("Cannot open CSV: " + path);
            csv_ << std::setprecision(10);
            csv_ << "step,player,from_v,to_v,b_sub,a_sub,g_sub,w_eta";
            for (int i = 0; i < game_.n_a; i++) csv_ << ",w_A" << i;

            for (Mask b = 0; b < n_subsets_; b++) csv_ << ",p_" << fmt_mask(b, "|");

            for (int j = 0; j < game_.n_g; j++) csv_ << ",in_G" << j;
            csv_ << "\n";
        }

        int step(int v, int t = 0) {
            State_space* ss = game_.get_state_space();
            const auto& owner  = ss->get_owner();
            const auto& assums = game_.spec.assumptions;
            last_step_eve_ = false;

            // Liveness monitors 
            for (int i = 0; i < (int)assums.size(); i++) {
                if (assums[i].states.count(v)) { w_A_[i] = 1.0; alpha_A_[i] *= params_.lambda; }
                else w_A_[i] *= (1.0 - alpha_A_[i]);
            }

            bool v_in_any_A = false;
            for (const auto& a : assums)
                if (a.states.count(v)) { v_in_any_A = true; break; }
            if (v_in_any_A) { w_union_ = 1.0; alpha_union_ *= params_.lambda_prime; }
            else w_union_ *= (1.0 - alpha_union_);

            // Advance (mode,q) of one subgame along the real history
            auto advance_state = [&](SubgameState& s, const SubgameKey& key, int orig_u, bool u_is_adam, int real_v) {
                if (u_is_adam) {
                    ShadowGuess sg = lib_.shadow_guess(key, orig_u, s.q);
                    if (sg.v_show == -1 || real_v != sg.v_show) {
                        s.mode = Mode::PLAY;
                        s.q    = lib_.q_after_play_move(key, real_v, s.q);
                    } else {
                        s.mode = Mode::SHOW;
                        s.q    = sg.q_show;
                    }
                } else {
                    s.q = lib_.hoa_step(key, s.q, lib_.transformed_target(real_v));
                    s.mode = Mode::PLAY;
                }
            };

            // ADAM'S TURN
            if (owner[v] == 1) {
                auto& succs = ss->get_post(v);
                if (succs.empty()) { std::cerr << "[error] Dead end at v=" << v << "\n"; return -1; }
                int real_v = succs[0];
                if (adam_strat_.loaded()) {
                    int chosen = adam_strat_.query(v, t);
                    if (chosen != -1) {
                        bool ok = false;
                        for (int w : succs) if (w == chosen) { ok = true; break; }
                        if (ok) real_v = chosen;
                        else std::cerr << "[adam] Warning: " << v << "->" << chosen << " invalid, using succs[0]\n";
                    }
                }
                for (auto& [key, s] : subgame_states_)
                    advance_state(s, key, v, true, real_v);
                
                write_csv_row(t, false, v, real_v);
                
                return real_v;
            }

            // EVE'S TURN 
            if (steps_since_resample_ == 0) {
                current_candidate_ = sample_subgame(t);
                steps_since_resample_ = params_.k;
            }
            steps_since_resample_--;

            // first strategy in B's chain that is winning at (v, q_sub)
            const auto& chain = chain_for_B_[current_candidate_.b];
            int next_v = -1;
            SubgameKey used{};
            for (const auto& key : chain) {
                SubgameState& s = subgame_states_.at(key);
                int q_out = s.q;
                int cand = lib_.query(key, v, s.q, s.mode, q_out);
                if (cand != -1) {
                    // Sanity check: BDD's next_q must equal del(q, label(target)), the
                    // convention advance_state relies on for non-deployed subgames
                    int q_hoa = lib_.hoa_step(key, s.q, lib_.transformed_target(cand));
                    if (q_hoa != q_out)
                        std::cerr << "[warn] q convention mismatch at " << fmt_key(key)
                                << ": BDD next_q=" << q_out << ", HOA step=" << q_hoa
                                << " (v=" << v << " -> " << cand << ")\n";
                    next_v = cand; s.q = q_out; ; s.mode = Mode::PLAY; used = key;
                    break;
                }
            }
            if (next_v == -1) {
                std::cerr << "[error] Lambda(v=" << v << ", B={" << fmt_mask(current_candidate_.b) <<  "}) undefined — v no in W or automaton state desynced\n";
                return -1;
            }

            for (auto& [key, s] : subgame_states_) {
                if (key == used) continue;
                advance_state(s, key, v, false, next_v);
            }

            last_used_ = used;
            last_b_ = current_candidate_.b;
            last_step_eve_ = true;

            write_csv_row(t, true, v, next_v);

            return next_v;
        }

        void reset() {
            const int m = static_cast<int>(game_.spec.assumptions.size());
            w_A_.assign(m, 1.0);
            alpha_A_.assign(m, params_.alpha);
            w_union_ = 1.0;
            alpha_union_ = params_.alpha_prime;
            init_subgame_states();
            steps_since_resample_ = 0;
            current_candidate_ = {1.0, full_a_};
            last_used_ = {};
            last_b_ = 0;
            last_step_eve_ = false;
        }

        double w_empty() const { return 1 - w_union_; }
        double w_union() const { return w_union_; }
        double w_A(int i) const { return w_A_.at(i); }
        const SubgameKey& last_used() const { return last_used_; }
        Mask last_b() const { return last_b_; }
        bool last_step_was_eve() const { return last_step_eve_; }

        struct SubsetProb { Mask a_subset; double prob; };
        std::vector<SubsetProb> compute_subset_probs(int t) const {
            auto probs = subset_probs(t);
            std::vector<SubsetProb> out;
            out.reserve(n_subsets_);
            for (Mask b = 0; b < n_subsets_; b++) out.push_back({b, probs[b]});
            std::sort(out.begin(), out.end(),
                      [](const SubsetProb& x, const SubsetProb& y) { return x.prob > y.prob; });
            return out;
        }

        std::string debug_state(const SubgameKey& k) const {
            auto it = subgame_states_.find(k);
            if (it == subgame_states_.end()) return "NOT_FOUND";
            return std::string(it->second.mode == Mode::PLAY ? "PLAY" : "SHOW") + ",q=" + std::to_string(it->second.q);
        }

        void debug_dump_states(std::ostream& out) const {
            for (const auto& [key, s] : subgame_states_)
                out << "  " << fmt_key(key) << " " << (s.mode == Mode::PLAY ? "PLAY" : "SHOW") << " q=" << s.q << "\n";
        }

    private:
        Game& game_;
        OptLib& lib_;
        Params params_;

        double w_union_;
        double alpha_union_;
        std::vector<double> w_A_;
        std::vector<double> alpha_A_;

        Mask n_subsets_ = 1;
        Mask full_a_ = 0;

        // Per-subgame memory: (mode in {PLAY,SHOW}, q) — Theorem 3 memory-2 bound
        struct SubgameState { Mode mode = Mode::PLAY; int q = 0; };
        std::map<SubgameKey, SubgameState> subgame_states_;

        // chain_for_B_[B] = Lambda priority chain (Algorithm 1 order)
        std::vector<std::vector<SubgameKey>> chain_for_B_;

        SubgameKey last_used_{};
        Mask last_b_ = 0;
        bool last_step_eve_ = false;

        int steps_since_resample_;
        Candidate current_candidate_;

        AdamStrategy adam_strat_;
        std::ofstream csv_;
        std::mt19937 rng_;

        void init_subgame_states() {
            const int tv0 = lib_.transformed_target(game_.init);
            for (auto& [key, s] : subgame_states_) {
                s.mode = Mode::PLAY;
                s.q    = lib_.hoa_step(key, lib_.hoa_start_state(key), tv0);
            }
        }

        void check_initial_q_convention() const {
            const SubgameKey full{full_a_, full_mask(game_.n_g)};
            const auto& owner = game_.get_state_space()->get_owner();
            if (!lib_.has(full) || owner[game_.init] != 0) return;

            const int q_start = lib_.hoa_start_state(full);
            const int q_read = lib_.hoa_step(full, q_start, lib_.transformed_target(game_.init));
            int dummy;
            const bool mv_start = lib_.query(full, game_.init, q_start, Mode::PLAY, dummy) != -1;
            const bool mv_read = lib_.query(full, game_.init, q_read,  Mode::PLAY, dummy) != -1;

            std::cerr << "[info] init-q check (" << fmt_key(full) << ", v0=" << game_.init << "): "
                    << "q=Start(" << q_start << ") " << (mv_start ? "has move" : "NO move") << ", "
                    << "q=del(Start,v0)(" << q_read << ") " << (mv_read ? "has move" : "NO move") << "\n";

            if (!mv_read)
                std::cerr << "[warn] BDD has no move at (v0, del(Start,v0)) — either v0 not in W or fairsyn convention mismatches"
                            
        }

        // Library keys usable for B: A_tilde in B only (fewer assumptions = harder
        // game, so sound when B holds). Order: G_tilde by decreasing size, then A_tilde
        // by increasing size; ties broken by mask value for determinism.
        std::vector<SubgameKey> build_chain(Mask b) const {
            const int n_g = game_.n_g;
            auto pc = [](Mask x) { return __builtin_popcount(x); };

            std::vector<Mask> a_cands;
            for (Mask sub = b; ; sub = (sub - 1) & b) {
                a_cands.push_back(sub);
                if (sub == 0) break;
            }
            std::sort(a_cands.begin(), a_cands.end(),
                      [&](Mask x, Mask y) { return pc(x) != pc(y) ? pc(x) < pc(y) : x < y; });

            std::vector<Mask> g_cands;
            for (Mask g = 0; g < (Mask(1) << n_g); g++) g_cands.push_back(g);
            std::sort(g_cands.begin(), g_cands.end(),
                      [&](Mask x, Mask y) { return pc(x) != pc(y) ? pc(x) > pc(y) : x < y; });

            std::vector<SubgameKey> chain;
            for (Mask g : g_cands)
                for (Mask a : a_cands)
                    if (lib_.has({a, g})) chain.push_back({a, g});
            return chain;
        }

        static double odds(double w) { return (1.0 + w) / (1.0 - w); }

        double clip(double w) const { return std::clamp(w, params_.eps, 1.0 - params_.eps); }

        // Intermediate weight w_tilde_B
        double w_tilde(Mask b, double gamma_t) const {
            if (b == 0) return std::min(1.0 - 2.0 * clip(w_union_), 1.0 - gamma_t);
            const int m = static_cast<int>(w_A_.size());
            double w_min_in = 1.0, w_max_out = 0.0;
            for (int i = 0; i < m; i++) {
                const double w = clip(w_A_[i]);
                if ((b >> i) & 1) w_min_in  = std::min(w_min_in,  w);
                else w_max_out = std::max(w_max_out, w);
            }
            return w_min_in - std::max(w_max_out, gamma_t);
        }

        void write_csv_row(int t, bool eve, int v, int next_v) {
            if (!csv_.is_open()) return;
            csv_ << t << "," << (eve ? "E" : "A") << "," << v << "," << next_v << ",";

            if (eve) csv_ << fmt_mask(last_b_, "|") << "," << fmt_mask(last_used_.a, "|") << "," << fmt_mask(last_used_.g, "|");
            else csv_ << ",,";

            csv_ << "," << w_union_;
            for (double w : w_A_) csv_ << "," << w;

            for (double p : subset_probs(t)) csv_ << "," << p;

            for (const auto& g : game_.spec.guarantees) csv_ << "," << (g.states.count(next_v) ? 1 : 0);

            csv_ << "," << clip(w_union_);
            for (double w : w_A_) csv_ << "," << clip(w);

            csv_ << "\n";
        }

        // p_B for every B, indexed by mask (Eqs. 6–7)
        std::vector<double> subset_probs(int t) const {
            const double gamma_t = 1.0 / std::log(3.0 + static_cast<double>(t));
            std::vector<double> p(n_subsets_);
            double denom = 0.0;
            for (Mask b = 0; b < n_subsets_; b++) { p[b] = odds(w_tilde(b, gamma_t)); denom += p[b]; }
            if (denom < 1e-15) denom = 1e-15;
            for (auto& x : p) x /= denom;
            return p;
        }

        PolicyCandidate sample_subgame(int t) {
            auto probs = subset_probs(t);
            std::discrete_distribution<int> dist(probs.begin(), probs.end());
            Mask b = static_cast<Mask>(dist(rng_));
            return {probs[b], b};
        }
};