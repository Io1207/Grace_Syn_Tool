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
#include <fstream>
#include <sstream>
#include <optional>

inline std::vector<int> active_assumptions(int v, const std::vector<GR1_conjunct>& assumptions) {
    std::vector<int> active;
    for (int i = 0; i < (int)assumptions.size(); i++)
        if (assumptions[i].states.count(v))
            active.push_back(i);
    return active;
}

inline std::vector<std::vector<int>> all_subsets_of(int m) {
    std::vector<std::vector<int>> result;
    for (int mask = 0; mask < (1 << m); mask++) {
        std::vector<int> s;
        for (int i = 0; i < m; i++)
            if (mask & (1 << i)) s.push_back(i);
        result.push_back(s);
    }
    return result;
}

struct DynamicPolicyParams {
    double alpha = 0.9;
    double alpha_prime = 0.5;
    double lambda = 0.9;
    double lambda_prime = 0.9;
    unsigned seed = 42;
    int k = 1;
};

struct Candidate {
    double prob;
    std::vector<int> a_idx;
    std::vector<int> g_idx;
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
                cur_phase->end_step = e;
            } else {
                int from = std::stoi(first), to;
                iss >> to;
                if (has_phases && cur_phase) cur_phase->transitions[from] = to;
                else  global_transitions_[from] = to;
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
            int m = game.n_a;
            int n_g = game.n_g;

            w_A_.assign(m, 1.0);
            alpha_A_.assign(m, p.alpha);
            w_union_ = 1.0;
            alpha_union_ = p.alpha_prime;

            all_a_subsets_ = all_subsets_of(m);

            full_a_.resize(m); std::iota(full_a_.begin(), full_a_.end(), 0);
            full_g_.resize(n_g); std::iota(full_g_.begin(), full_g_.end(), 0);

            reset_active();
        }

        void load_adam_strategy(const std::string& path) { adam_strat_.load(path); }

        void set_csv(const std::string& path) {
            csv_.open(path);
            if (!csv_) throw std::runtime_error("Cannot open CSV: " + path);
            csv_ << "step,from_v,to_v,a_sub,g_sub,w_empty";
            for (int i = 0; i < (int)game_.spec.assumptions.size(); i++)
                csv_ << ",w_A" << i;
            for (int j = 0; j < (int)game_.spec.guarantees.size(); j++)
                csv_ << ",in_G" << j;
            csv_ << "\n";
        }

        int step(int v, int t = 0) {
            State_space* ss = game_.get_state_space();
            const auto& owner = ss->get_owner();
            const auto& assums = game_.spec.assumptions;

            // Liveness monitors
            for (int i = 0; i < (int)assums.size(); i++) {
                if (assums[i].states.count(v)) {
                    w_A_[i] = 1.0; alpha_A_[i] *= params_.lambda;
                } else {
                    w_A_[i] *= (1.0 - alpha_A_[i]);
                }
            }
            bool v_in_any_A = false;
            for (const auto& a : assums)
                if (a.states.count(v)) { v_in_any_A = true; break; }
            if (v_in_any_A) { w_union_ = 1.0; alpha_union_ *= params_.lambda_prime; }
            else              w_union_ *= (1.0 - alpha_union_);

            // ADAM'S TURN
            if (owner[v] == 1) {
                auto& succs = ss->get_post(v);
                if (succs.empty()) { std::cerr << "[error] Dead end at v=" << v << "\n"; return -1; }
                int real_v = succs[0];
                if (adam_strat_.loaded()) {
                    int chosen = adam_strat_.query(v, t);
                    if (chosen != -1) {
                        bool ok = false;
                        for (int w : succs) if (w==chosen){ok=true;break;}
                        if (ok) real_v = chosen;
                        else std::cerr << "[adam] Warning: " << v << "->" << chosen << " invalid, using succs[0]\n";
                    }
                }
                
                if (have_active_) {
                    try {
                        ShadowGuess sg = lib_.shadow_guess(v, active_.q, active_key_.first, active_key_.second);
                        if (sg.v_show == -1 || real_v != sg.v_show) {
                            active_.mode = Mode::PLAY;
                            active_.q    = lib_.q_after_play_move(real_v, active_.q, active_key_.first, active_key_.second);
                        } else {
                            active_.mode = Mode::SHOW;
                            active_.q = sg.q_show;
                        }
                    } catch (...) { }
                }
                return real_v;
            }

            // EVE'S TURN
            if (steps_since_resample_ == 0 || !have_B_) {
                current_B_ = sample_B(t);
                have_B_ = true;
                steps_since_resample_ = std::max(1, params_.k);
            }
            steps_since_resample_--;

            SubgameKey2 target = lookup(v, current_B_);
            if (!have_active_ || target != active_key_) activate(target);

            int next_v = -1, q_out = active_.q;
            try {
                next_v = lib_.query(v, active_.q, active_.mode, active_key_.first, active_key_.second, q_out);
            } catch (...) { next_v = -1; }

            if (next_v == -1) {
                activate(active_key_);
                try {
                    next_v = lib_.query(v, active_.q, active_.mode, active_key_.first, active_key_.second, q_out);
                } catch (...) { next_v = -1; }
                if (next_v == -1) {
                    activate({full_a_, full_g_});
                    try {
                        next_v = lib_.query(v, active_.q, active_.mode, active_key_.first, active_key_.second, q_out);
                    } catch (...) { next_v = -1; }
                }
                if (next_v == -1) { std::cerr << "[error] Full fallback -1 at v=" << v << "\n"; return -1; }
            }
            active_.q = q_out;

            last_a_sub_ = active_key_.first;
            last_g_sub_ = active_key_.second;

            if (csv_.is_open()) {
                csv_ << t << "," << v << "," << next_v << ","
                     << format_vector(last_a_sub_) << "," << format_vector(last_g_sub_)
                     << "," << w_union_ << "," << (1.0-w_union_);
                for (int i=0;i<(int)assums.size();i++) csv_ << "," << w_A_[i];
                for (int j=0;j<(int)game_.spec.guarantees.size();j++)
                    csv_ << "," << (int)(game_.spec.guarantees[j].states.count(next_v)>0);
                csv_ << "\n";
            }
            return next_v;
        }

        void reset() {
            int m = static_cast<int>(game_.spec.assumptions.size());
            w_A_.assign(m, 1.0);
            alpha_A_.assign(m, params_.alpha);
            w_union_ = 1.0;
            alpha_union_ = params_.alpha_prime;
            steps_since_resample_ = 0;
            have_B_ = false;
            reset_active();
            last_a_sub_.clear();
            last_g_sub_.clear();
        }

        double w_empty() const { return 1 - w_union_; }
        double w_union() const { return w_union_; }
        double w_A(int i) const { return w_A_.at(i); }
        const std::vector<int>& last_a_tilde() const { return last_a_sub_; }
        const std::vector<int>& last_g_tilde() const { return last_g_sub_; }

        struct SubsetProb { std::vector<int> a_subset; double prob; };
        std::vector<SubsetProb> compute_subset_probs(int t) const {
            std::vector<double> p = subset_probabilities(t);
            std::vector<SubsetProb> result;
            for (int si = 0; si < (int)all_a_subsets_.size(); si++)
                result.push_back({all_a_subsets_[si], p[si]});
            std::sort(result.begin(), result.end(),
                      [](const SubsetProb& a, const SubsetProb& b){ return a.prob > b.prob; });
            return result;
        }

        std::string debug_state(const std::vector<int>& a, const std::vector<int>& g) const {
            if (!have_active_ || active_key_.first != a || active_key_.second != g) return "NOT_ACTIVE";
            return std::string(active_.mode == Mode::PLAY ? "PLAY" : "SHOW") + ",q=" + std::to_string(active_.q);
        }

        void debug_dump_states(std::ostream& out) const {
            if (!have_active_) { out << "  (no active strategy)\n"; return; }
            out << "  A={" << format_vector_plain(active_key_.first) << "} G={"
                << format_vector_plain(active_key_.second) << "} "
                << (active_.mode==Mode::PLAY?"PLAY":"SHOW") << " q=" << active_.q << "\n";
        }

    private:
        struct SubgameState { Mode mode = Mode::PLAY; int q = 0; };
        using SubgameKey2 = std::pair<std::vector<int>,std::vector<int>>;

        Game& game_;
        OptLib& lib_;
        Params params_;

        double w_union_;
        double alpha_union_;
        std::vector<double> w_A_;
        std::vector<double> alpha_A_;

        std::vector<std::vector<int>> all_a_subsets_;
        std::vector<int> full_a_;
        std::vector<int> full_g_;

        bool have_active_ = false;
        SubgameKey2 active_key_;
        SubgameState active_;

        std::vector<int> current_B_;
        bool have_B_ = false;
        int steps_since_resample_ = 0;

        std::vector<int> last_a_sub_;
        std::vector<int> last_g_sub_;

        AdamStrategy adam_strat_;
        std::ofstream csv_;
        std::mt19937 rng_;

        void reset_active() {
            have_active_ = false;
            active_ = SubgameState{};
            active_key_ = {full_a_, full_g_};
        }

        void activate(const SubgameKey2& key) {
            active_key_ = key;
            active_ = SubgameState{};
            try { active_.q = lib_.hoa_start_state(key.first, key.second); } catch (...) { active_.q = 0; }
            have_active_ = true;
        }

        SubgameKey2 lookup(int v, const std::vector<int>& B) const {
            auto k = lib_.best_for(v, B);
            if (k) return {k->a_indices, k->g_indices};
            return {full_a_, full_g_};
        }

        std::vector<double> subset_probabilities(int t) const {
            auto odds = [](double w_tilde) -> double {
                w_tilde = std::max(w_tilde, -1.0 + 1e-6);
                if (w_tilde >= 1.0 - 1e-12) return 1e9;
                return (1.0 + w_tilde) / (1.0 - w_tilde);
            };
            double gamma_t = 1.0 / std::log(3.0 + static_cast<double>(t));
            double w_empty = std::min(1.0 - 2.0 * w_union_, 1.0 - gamma_t);

            std::vector<double> w_odds(all_a_subsets_.size(), 0.0);
            for (int si = 0; si < (int)all_a_subsets_.size(); si++) {
                const auto& subset = all_a_subsets_[si];
                if (subset.empty()) { w_odds[si] = odds(w_empty); continue; }
                double w_min_in = 1.0;
                for (int i : subset) w_min_in = std::min(w_min_in, w_A_[i]);
                double w_max_out = 0.0;
                for (int i = 0; i < (int)w_A_.size(); i++)
                    if (std::find(subset.begin(), subset.end(), i) == subset.end())
                        w_max_out = std::max(w_max_out, w_A_[i]);
                w_odds[si] = odds(w_min_in - std::max(w_max_out, gamma_t));
            }
            double denom = 0.0;
            for (double w : w_odds) denom += w;
            if (denom < 1e-15) denom = 1e-15;
            for (double& w : w_odds) w /= denom;
            return w_odds;
        }

        std::vector<int> sample_B(int t) {
            std::vector<double> probs = subset_probabilities(t);
            std::discrete_distribution<int> dist(probs.begin(), probs.end());
            return all_a_subsets_[dist(rng_)];
        }

        static std::string format_vector_plain(const std::vector<int>& v) {
            std::string s;
            for (size_t i = 0; i < v.size(); i++) { if (i) s += ","; s += std::to_string(v[i]); }
            return s;
        }

        static std::string format_vector (const std::vector<int>& v) {
            if (v.empty()) return "empty";
            return format_vector_plain(v);
        }
};