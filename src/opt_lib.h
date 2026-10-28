#pragma once

#include "game.h"
#include "player.h"
#include "fairsyn_writer.h"

#include <algorithm>
#include <map>
#include <vector>
#include <set>
#include <string>
#include <stdexcept>
#include <memory>
#include <optional>
#include <fstream>
#include <filesystem>

struct SubgameKey {
    std::vector<int> a_indices;
    std::vector<int> g_indices;

    bool operator<(const SubgameKey& o) const {
        if (a_indices != o.a_indices) return a_indices < o.a_indices;
        return g_indices < o.g_indices;
    }
    bool operator==(const SubgameKey& o) const {
        return a_indices == o.a_indices && g_indices == o.g_indices;
    }
};

struct SubgameEntry {
    SubgameKey key;
    std::unique_ptr<Eve> eve;
    std::set<int> z;
    bool has_z = false;

    SubgameEntry() = default;
    SubgameEntry(SubgameKey k, std::unique_ptr<Eve> e) : key(std::move(k)), eve(std::move(e)) {}
    SubgameEntry(const SubgameEntry&) = delete;
    SubgameEntry& operator=(const SubgameEntry&) = delete;
    SubgameEntry(SubgameEntry&&) = default;
    SubgameEntry& operator=(SubgameEntry&&) = default;
};

class OptLib {
    public:
        explicit OptLib(Game& g) : game_(g) {
            build_vertex_maps();
        }

        OptLib(const OptLib&) = delete;
        OptLib& operator=(const OptLib&) = delete;
        OptLib(OptLib&&) = default;
        OptLib& operator=(OptLib&&) = default;

        void store(const std::vector<int>& a_indices, const std::vector<int>& g_indices,
                   const std::string& controller_query_bin, const std::string& bdd_file, const std::string& hoa_file)
        {
            SubgameKey key{a_indices, g_indices};
            auto ap_verts = build_ap_verts(a_indices, g_indices);
            auto eve = std::make_unique<Eve>(game_, controller_query_bin,
                                             bdd_file, hoa_file, play_id_,
                                             choose_id_, show_id_, ap_verts);
            SubgameEntry entry{key, std::move(eve)};
            load_z(bdd_file, entry);
            entries_.insert_or_assign(key, std::move(entry));
        }

        std::optional<SubgameKey> best_for(int orig_v, const std::vector<int>& B) const {
            const SubgameEntry* best = nullptr;
            for (const auto& [k, e] : entries_) {
                if (k.g_indices.empty()) continue;
                bool sub = true;
                for (int a : k.a_indices)
                    if (std::find(B.begin(), B.end(), a) == B.end()) { sub = false; break; }
                if (!sub) continue;
                if (!in_z(e, orig_v)) continue;
                if (!best || k.g_indices.size() > best->key.g_indices.size()
                    || (k.g_indices.size() == best->key.g_indices.size()
                        && k.a_indices.size() < best->key.a_indices.size()))
                    best = &e;
            }
            if (!best) return std::nullopt;
            return best->key;
        }

        void set_winning_region(const std::set<int>& W) { winning_region_ = W; }
        const std::set<int>& winning_region() const { return winning_region_; }

        bool has(const std::vector<int>& a_indices, const std::vector<int>& g_indices) const {
            return entries_.count(SubgameKey{a_indices, g_indices}) > 0;
        }

        int query(int orig_u, int q, Mode mode, const std::vector<int>& a_indices,
                  const std::vector<int>& g_indices, int& q_out) const
        {
            auto it = entries_.find(SubgameKey{a_indices, g_indices});
            if (it == entries_.end())
                throw std::out_of_range("OptLib::query — subgame not found");
            auto r = it->second.eve->get_move(orig_u, q, mode);
            q_out = r.next_q;
            return r.next_transformed;
        }

        // Automaton transition via HOA
        int hoa_step(int q, int transformed_target, const std::vector<int>& a_indices,
                     const std::vector<int>& g_indices) const
        {
            auto it = entries_.find(SubgameKey{a_indices, g_indices});
            if (it == entries_.end())
                throw std::out_of_range("OptLib::hoa_step — subgame not found");
            return it->second.eve->hoa_step(q, transformed_target);
        }

        ShadowGuess shadow_guess(int orig_u, int q, const std::vector<int>& a_indices,
                                 const std::vector<int>& g_indices) const
        {
            auto it = entries_.find(SubgameKey{a_indices, g_indices});
            if (it == entries_.end())
                throw std::out_of_range("OptLib::shadow_guess — subgame not found");
            return it->second.eve->shadow_guess(orig_u, q);
        }

        int q_after_play_move(int orig_v, int q, const std::vector<int>& a_indices,
                              const std::vector<int>& g_indices) const
        {
            auto it = entries_.find(SubgameKey{a_indices, g_indices});
            if (it == entries_.end())
                throw std::out_of_range("OptLib::q_after_play_move — subgame not found");
            return it->second.eve->q_after_play_move(orig_v, q);
        }

        bool is_realizable(const std::vector<int>& a_indices, const std::vector<int>& g_indices) const
        {
            auto it = entries_.find(SubgameKey{a_indices, g_indices});
            if (it == entries_.end()) return false;
            return it->second.eve->is_realizable();
        }

        int hoa_start_state(const std::vector<int>& a_indices, const std::vector<int>& g_indices) const
        {
            auto it = entries_.find(SubgameKey{a_indices, g_indices});
            if (it == entries_.end())
                throw std::out_of_range("OptLib::hoa_start_state — subgame not found");
            return it->second.eve->hoa_start_state();
        }

        std::vector<SubgameKey> all_keys() const {
            std::vector<SubgameKey> keys;
            keys.reserve(entries_.size());
            for (const auto& [k, _] : entries_) keys.push_back(k);
            return keys;
        }

        int size() const { return static_cast<int>(entries_.size()); }

        const std::vector<int>& get_play_id()   const { return play_id_; }
        const std::vector<int>& get_choose_id() const { return choose_id_; }
        const std::vector<int>& get_show_id()   const { return show_id_; }

    private:
        static bool in_z(const SubgameEntry& e, int orig_v) { return !e.has_z || e.z.count(orig_v) > 0; }

        void load_z(const std::string& bdd_file, SubgameEntry& e) const {
            namespace fs = std::filesystem;
            fs::path zf = fs::path(bdd_file).parent_path() / "eve_winning.txt";
            if (!fs::exists(zf)) return;
            std::vector<int> play_to_orig;
            for (int v = 0; v < (int)play_id_.size(); v++)
                if (play_id_[v] >= 0) {
                    if ((int)play_to_orig.size() <= play_id_[v]) play_to_orig.resize(play_id_[v] + 1, -1);
                    play_to_orig[play_id_[v]] = v;
                }
            std::ifstream f(zf);
            int pid;
            while (f >> pid)
                if (pid >= 0 && pid < (int)play_to_orig.size() && play_to_orig[pid] >= 0)
                    e.z.insert(play_to_orig[pid]);
            e.has_z = true;
        }

        Game& game_;
        std::map<SubgameKey, SubgameEntry> entries_;
        std::vector<int> play_id_;
        std::vector<int> choose_id_;
        std::vector<int> show_id_;
        std::set<int> winning_region_;

        void build_vertex_maps() {
            const int n = game_.num_states;
            const auto& owner = game_.get_state_space()->get_owner();

            std::vector<int> eve_verts, adam_verts;
            for (int v = 0; v < n; v++) {
                if (owner[v] == 0) eve_verts.push_back(v);
                else adam_verts.push_back(v);
            }
            const int n_eve = static_cast<int>(eve_verts.size());
            const int n_adam = static_cast<int>(adam_verts.size());

            play_id_.assign(n, -1);
            choose_id_.assign(n, -1);
            show_id_.assign(n, -1);

            for (int i = 0; i < n_eve;  i++) play_id_ [eve_verts [i]] = i;
            for (int i = 0; i < n_adam; i++) choose_id_[adam_verts[i]] = n_eve + i;
            for (int v = 0; v < n; v++) show_id_[v] = n_eve + n_adam + v;
        }

        std::map<std::string, std::set<int>>
        build_ap_verts(const std::vector<int>& a_tilde, const std::vector<int>& g_tilde) const
        {
            const int n = game_.num_states;
            const auto& owner = game_.get_state_space()->get_owner();
            const auto& spec = game_.spec;

            auto lift_all = [&](const std::set<int>& verts) {
                std::set<int> s;
                for (int v : verts) {
                    if (owner[v] == 0) s.insert(play_id_[v]);
                    else    s.insert(choose_id_[v]);
                    s.insert(show_id_[v]);
                }
                return s;
            };

            std::map<std::string, std::set<int>> ap_verts;

            // in_play
            {
                std::set<int> s;
                for (int v = 0; v < n; v++)
                    if (owner[v] == 0 && play_id_[v] >= 0) s.insert(play_id_[v]);
                ap_verts["in_play"] = std::move(s);
            }

            // "show": all show-region vertices
            {
                std::set<int> s;
                for (int v = 0; v < n; v++) s.insert(show_id_[v]);
                ap_verts["show"] = std::move(s);
            }

            // a{i}_all for i in a_tilde
            for (int i : a_tilde)
                ap_verts["a" + std::to_string(i) + "_all"] = lift_all(spec.assumptions[i].states);

            std::set<int> a_tilde_set(a_tilde.begin(), a_tilde.end());
            for (int i = 0; i < (int)spec.assumptions.size(); i++) {
                if (!a_tilde_set.count(i))
                    ap_verts["a" + std::to_string(i) + "_all"] = lift_all(spec.assumptions[i].states);
            }

            for (int j : g_tilde)
                ap_verts["g" + std::to_string(j) + "_all"] = lift_all(spec.guarantees[j].states);

            // in_W: transformed vertices in the full-game winning region
            if (!winning_region_.empty()) {
                ap_verts["in_W"] = winning_region_;
            }
            return ap_verts;
        }
};