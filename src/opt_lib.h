// #pragma once

// #include "game.h"
// #include "player.h"
// #include "fairsyn_writer.h"

// #include <map>
// #include <vector>
// #include <set>
// #include <string>
// #include <stdexcept>
// #include <memory>

// struct SubgameKey {
//     std::vector<int> a_indices;
//     std::vector<int> g_indices;

//     bool operator<(const SubgameKey& o) const {
//         if (a_indices != o.a_indices) return a_indices < o.a_indices;
//         return g_indices < o.g_indices;
//     }
//     bool operator==(const SubgameKey& o) const {
//         return a_indices == o.a_indices && g_indices == o.g_indices;
//     }
// };

// struct SubgameEntry {
//     SubgameKey key;
//     std::unique_ptr<Eve> eve;

//     SubgameEntry() = default;
//     SubgameEntry(SubgameKey k, std::unique_ptr<Eve> e) : key(std::move(k)), eve(std::move(e)) {}
//     SubgameEntry(const SubgameEntry&) = delete;
//     SubgameEntry& operator=(const SubgameEntry&) = delete;
//     SubgameEntry(SubgameEntry&&) = default;
//     SubgameEntry& operator=(SubgameEntry&&) = default;
// };

// class OptLib {
//     public:
//         explicit OptLib(Game& g) : game_(g) {
//             build_vertex_maps();
//         }

//         OptLib(const OptLib&) = delete;
//         OptLib& operator=(const OptLib&) = delete;
//         OptLib(OptLib&&) = default;
//         OptLib& operator=(OptLib&&) = delete; // game_ is a reference

//         // Store a synthesized subgame strategy. Takes:
//         //   controller_query_bin — path to the controller_query binary
//         //   bdd_file             — per-subgame .bdd file from fairsyn
//         //   hoa_file             — per-subgame .hoa automaton from fairsyn
//         //   ap_verts             — AP name → transformed-vertex set, built by
//         //                          write_obliging_spec and passed in so Eve's
//         //                          HOA transition cache can be constructed.
//         /***** FIXED: store() no longer takes ap_verts — built internally by build_ap_verts() *****/
//         // store() only needs the two file paths — ap_verts is built
//         // internally from game_ + the vertex maps already computed in
//         // build_vertex_maps(), using the same logic as write_obliging_spec.
//         void store(const std::vector<int>& a_indices,
//                    const std::vector<int>& g_indices,
//                    const std::string& controller_query_bin,
//                    const std::string& bdd_file,
//                    const std::string& hoa_file)
//         {
//             SubgameKey key{a_indices, g_indices};
//             auto ap_verts = build_ap_verts(a_indices, g_indices);
//             auto eve = std::make_unique<Eve>(game_,
//                                              controller_query_bin,
//                                              bdd_file,
//                                              hoa_file,
//                                              play_id_, choose_id_, show_id_,
//                                              ap_verts);
//             entries_.insert_or_assign(key, SubgameEntry{key, std::move(eve)});
//         }

//         // Set the winning region W (from winning_vertices.txt after full-game synth).
//         void set_winning_region(const std::set<int>& W) { winning_region_ = W; }
//         const std::set<int>& winning_region() const { return winning_region_; }

//         bool has(const std::vector<int>& a_indices, const std::vector<int>& g_indices) const {
//             return entries_.count(SubgameKey{a_indices, g_indices}) > 0;
//         }

//         // ── Eve's turn (u ∈ V◦) ──────────────────────────────────────────
//         // Returns the original-arena next vertex, or -1 if not in winning
//         // region. Also returns the new Rabin state via q_out.
//         int query(int orig_u, int q, Mode mode,
//                   const std::vector<int>& a_indices,
//                   const std::vector<int>& g_indices,
//                   int& q_out) const
//         {
//             auto it = entries_.find(SubgameKey{a_indices, g_indices});
//             if (it == entries_.end())
//                 throw std::out_of_range("OptLib::query — subgame not found");
//             auto r = it->second.eve->get_move(orig_u, q, mode);
//             q_out = r.next_q;
//             return r.next_transformed; // already decoded to orig by get_move
//         }

//         // ── Automaton transition via HOA (known target) ───────────────────
//         int hoa_step(int q, int transformed_target,
//                      const std::vector<int>& a_indices,
//                      const std::vector<int>& g_indices) const
//         {
//             auto it = entries_.find(SubgameKey{a_indices, g_indices});
//             if (it == entries_.end())
//                 throw std::out_of_range("OptLib::hoa_step — subgame not found");
//             return it->second.eve->hoa_step(q, transformed_target);
//         }

//         // ── Shadow-guess at an Adam vertex (u ∈ V!) ──────────────────────
//         ShadowGuess shadow_guess(int orig_u, int q,
//                                  const std::vector<int>& a_indices,
//                                  const std::vector<int>& g_indices) const
//         {
//             auto it = entries_.find(SubgameKey{a_indices, g_indices});
//             if (it == entries_.end())
//                 throw std::out_of_range("OptLib::shadow_guess — subgame not found");
//             return it->second.eve->shadow_guess(orig_u, q);
//         }

//         // ── Case-2 Rabin state (Adam's real move) ────────────────────────
//         int q_after_play_move(int orig_v, int q,
//                               const std::vector<int>& a_indices,
//                               const std::vector<int>& g_indices) const
//         {
//             auto it = entries_.find(SubgameKey{a_indices, g_indices});
//             if (it == entries_.end())
//                 throw std::out_of_range("OptLib::q_after_play_move — subgame not found");
//             return it->second.eve->q_after_play_move(orig_v, q);
//         }

//         // ── Realizability check ───────────────────────────────────────────
//         bool is_realizable(const std::vector<int>& a_indices,
//                            const std::vector<int>& g_indices) const
//         {
//             auto it = entries_.find(SubgameKey{a_indices, g_indices});
//             if (it == entries_.end()) return false;
//             return it->second.eve->is_realizable();
//         }

//         // ── HOA initial state (same for all subgames, but stored per-Eve) ─
//         int hoa_start_state(const std::vector<int>& a_indices,
//                             const std::vector<int>& g_indices) const
//         {
//             auto it = entries_.find(SubgameKey{a_indices, g_indices});
//             if (it == entries_.end())
//                 throw std::out_of_range("OptLib::hoa_start_state — subgame not found");
//             return it->second.eve->hoa_start_state();
//         }

//         std::vector<SubgameKey> all_keys() const {
//             std::vector<SubgameKey> keys;
//             keys.reserve(entries_.size());
//             for (const auto& [k, _] : entries_) keys.push_back(k);
//             return keys;
//         }

//         int size() const { return static_cast<int>(entries_.size()); }

//         /***** FIXED: get_play_id/choose_id/show_id are now PUBLIC accessors (needed by dynamic_policy.h) *****/
//         // Expose maps so subgame.h can pass them to store() / write_obliging_spec
//         const std::vector<int>& get_play_id()   const { return play_id_; }
//         const std::vector<int>& get_choose_id() const { return choose_id_; }
//         const std::vector<int>& get_show_id()   const { return show_id_; }

//     private:
//         Game& game_;
//         std::map<SubgameKey, SubgameEntry> entries_;
//         std::vector<int> play_id_;
//         std::vector<int> choose_id_;
//         std::vector<int> show_id_;
//         std::set<int>    winning_region_;  // transformed-arena vertex IDs in W

//         void build_vertex_maps() {
//             const int n = game_.num_states;
//             const auto& owner = game_.get_state_space()->get_owner();

//             std::vector<int> eve_verts, adam_verts;
//             for (int v = 0; v < n; v++) {
//                 if (owner[v] == 0) eve_verts.push_back(v);
//                 else                adam_verts.push_back(v);
//             }
//             const int n_eve  = static_cast<int>(eve_verts.size());
//             const int n_adam = static_cast<int>(adam_verts.size());

//             play_id_.assign(n, -1);
//             choose_id_.assign(n, -1);
//             show_id_.assign(n, -1);

//             for (int i = 0; i < n_eve;  i++) play_id_ [eve_verts [i]] = i;
//             for (int i = 0; i < n_adam; i++) choose_id_[adam_verts[i]] = n_eve + i;
//             for (int v = 0; v < n; v++)      show_id_  [v]             = n_eve + n_adam + v;
//         }

//         // Build the AP-vertex map for a given (a_tilde, g_tilde) subgame.
//         // Mirrors write_obliging_spec's AP construction exactly, using the
//         // vertex maps already computed by build_vertex_maps().
//         std::map<std::string, std::set<int>>
//         build_ap_verts(const std::vector<int>& a_tilde,
//                        const std::vector<int>& g_tilde) const
//         {
//             const int n = game_.num_states;
//             const auto& owner = game_.get_state_space()->get_owner();
//             const auto& spec  = game_.spec;

//             auto lift_all = [&](const std::set<int>& verts) {
//                 std::set<int> s;
//                 for (int v : verts) {
//                     if (owner[v] == 0) s.insert(play_id_[v]);
//                     else               s.insert(choose_id_[v]);
//                     s.insert(show_id_[v]);
//                 }
//                 return s;
//             };

//             std::map<std::string, std::set<int>> ap_verts;

//             // in_play: play copies of Eve vertices
//             {
//                 std::set<int> s;
//                 for (int v = 0; v < n; v++)
//                     if (owner[v] == 0 && play_id_[v] >= 0) s.insert(play_id_[v]);
//                 ap_verts["in_play"] = std::move(s);
//             }

//             /***** FIXED: replaced per-assumption a{i}_show with single "show" AP *****/
//             // "show": all show-region vertices
//             {
//                 std::set<int> s;
//                 for (int v = 0; v < n; v++) s.insert(show_id_[v]);
//                 ap_verts["show"] = std::move(s);
//             }

//             // a{i}_all for i in a_tilde
//             for (int i : a_tilde)
//                 ap_verts["a" + std::to_string(i) + "_all"] =
//                     lift_all(spec.assumptions[i].states);

//             // a{i}_all for assumptions NOT in a_tilde (needed for Ψ_full = GF(a{i}_all & show))
//             std::set<int> a_tilde_set(a_tilde.begin(), a_tilde.end());
//             for (int i = 0; i < (int)spec.assumptions.size(); i++) {
//                 if (!a_tilde_set.count(i))
//                     ap_verts["a" + std::to_string(i) + "_all"] =
//                         lift_all(spec.assumptions[i].states);
//             }

//             // g{j}_all for j in g_tilde
//             for (int j : g_tilde)
//                 ap_verts["g" + std::to_string(j) + "_all"] =
//                     lift_all(spec.guarantees[j].states);

//             // in_W: transformed vertices in the full-game winning region.
//             // Only added when W is known (non-empty).
//             if (!winning_region_.empty()) {
//                 ap_verts["in_W"] = winning_region_;
//             }

//             return ap_verts;
//         }
// };

#pragma once

#include "game.h"
#include "player.h"
#include "fairsyn_writer.h"

#include <cstdint>
#include <map>
#include <vector>
#include <set>
#include <string>
#include <stdexcept>
#include <memory>

using Mask = uint32_t;

inline Mask to_mask(const std::vector<int>& v) { Mask m = 0; for (int i : v) m |= (Mask(1) << i); return m; }

inline std::vector<int> from_mask(Mask m) {
    std::vector<int> v;
    for (int i = 0; m; i++, m >>= 1) if (m & 1) v.push_back(i);
    return v;
}
inline bool is_subset(Mask sub, Mask sup) { return (sub & sup) == sub; }
inline Mask full_mask(int n) { return n >= 32 ? ~Mask(0) : ((Mask(1) << n) - 1); }

inline std::string fmt_mask(Mask m, const char* sep = ",") {
    if (m == 0) return "empty";
    std::string s;
    for (int i : from_mask(m)) { if (!s.empty()) s += sep; s += std::to_string(i); }
    return s;
}

struct SubgameKey {
    Mask a = 0; // A_tilde
    Mask g = 0; // G_tilde
    bool operator<(const SubgameKey& o) const { return a != o.a ? a < o.a : g < o.g; }
    bool operator==(const SubgameKey& o) const { return a == o.a && g == o.g; }
    bool operator!=(const SubgameKey& o) const { return !(*this == o); }
};

inline std::string fmt_key(const SubgameKey& k) { return "A{" + fmt_mask(k.a) + "}_G{" + fmt_mask(k.g) + "}"; }

struct SubgameEntry {
    SubgameKey key;
    std::unique_ptr<Eve> eve;

    SubgameEntry() = default;
    SubgameEntry(SubgameKey k, std::unique_ptr<Eve> e) : key(k), eve(std::move(e)) {}
    SubgameEntry(const SubgameEntry&) = delete;
    SubgameEntry& operator=(const SubgameEntry&) = delete;
    SubgameEntry(SubgameEntry&&) = default;
    SubgameEntry& operator=(SubgameEntry&&) = default;
};

class OptLib {
    public:
        explicit OptLib(Game& g) : game_(g) { build_vertex_maps(); }

        OptLib(const OptLib&) = delete;
        OptLib& operator=(const OptLib&) = delete;
        OptLib(OptLib&&) = default;
        OptLib& operator=(OptLib&&) = delete; // game_ is a reference

        void store(const SubgameKey& k, const std::string& controller_query_bin,
                   const std::string& bdd_file, const std::string& hoa_file)
        {
            auto ap_verts = build_ap_verts(from_mask(k.a), from_mask(k.g));
            auto eve = std::make_unique<Eve>(game_, controller_query_bin, bdd_file, hoa_file,
                                             play_id_, choose_id_, show_id_, ap_verts);
            entries_.insert_or_assign(k, SubgameEntry{k, std::move(eve)});
        }

        void set_winning_region(const std::set<int>& W) { winning_region_ = W; }
        const std::set<int>& winning_region() const { return winning_region_; }

        bool has(const SubgameKey& k) const { return entries_.count(k) > 0; }

        // Transformed vertex the token lands on when the real play moves to orig_v
        // out of the play/choose region. Mirrors write_obliging_arena's targets:
        // Eve vertex -> play copy, Adam vertex -> choose copy
        int transformed_target(int orig_v) const {
            const auto& owner = game_.get_state_space()->get_owner();
            int tv = (owner[orig_v] == 0) ? play_id_[orig_v] : choose_id_[orig_v];
            if (tv < 0)
                throw std::logic_error("OptLib::transformed_target: no play/choose copy for vertex " + 
                      std::to_string(orig_v));
            return tv;
        }

        // Eve's turn: original-arena next vertex, or -1 if (u,q) not winning
        int query(const SubgameKey& k, int orig_u, int q, Mode mode, int& q_out) const {
            auto r = get_eve(k, "query").get_move(orig_u, q, mode);
            q_out = r.next_q;
            return r.next_transformed;
        }

        int hoa_step(const SubgameKey& k, int q, int transformed_target) const {
            return get_eve(k, "hoa_step").hoa_step(q, transformed_target);
        }

        ShadowGuess shadow_guess(const SubgameKey& k, int orig_u, int q) const {
            return get_eve(k, "shadow_guess").shadow_guess(orig_u, q);
        }

        int q_after_play_move(const SubgameKey& k, int orig_v, int q) const {
            return get_eve(k, "q_after_play_move").hoa_step(q, transformed_target(orig_v));
        }

        bool is_realizable(const SubgameKey& k) const {
            auto it = entries_.find(k);
            return it != entries_.end() && it->second.eve->is_realizable();
        }

        int hoa_start_state(const SubgameKey& k) const {
            return get_eve(k, "hoa_start_state").hoa_start_state();
        }

        std::vector<SubgameKey> all_keys() const {
            std::vector<SubgameKey> keys;
            keys.reserve(entries_.size());
            for (const auto& [k, _] : entries_) keys.push_back(k);
            return keys;
        }

        int size() const { return static_cast<int>(entries_.size()); }

        const std::vector<int>& get_play_id() const { return play_id_; }
        const std::vector<int>& get_choose_id() const { return choose_id_; }
        const std::vector<int>& get_show_id() const { return show_id_; }

    private:
        Game& game_;
        std::map<SubgameKey, SubgameEntry> entries_;
        std::vector<int> play_id_;
        std::vector<int> choose_id_;
        std::vector<int> show_id_;
        std::set<int> winning_region_;

        const Eve& get_eve(const SubgameKey& k, const char* who) const {
            auto it = entries_.find(k);
            if (it == entries_.end())
                throw std::out_of_range(std::string("OptLib::") + who + " — subgame " + fmt_key(k) + " not found");
            return *it->second.eve;
        }

        void build_vertex_maps() {
            const int n = game_.num_states;
            const auto& owner = game_.get_state_space()->get_owner();

            std::vector<int> eve_verts, adam_verts;
            for (int v = 0; v < n; v++) {
                if (owner[v] == 0) eve_verts.push_back(v);
                else adam_verts.push_back(v);
            }
            const int n_eve  = static_cast<int>(eve_verts.size());
            const int n_adam = static_cast<int>(adam_verts.size());

            play_id_.assign(n, -1);
            choose_id_.assign(n, -1);
            show_id_.assign(n, -1);

            for (int i = 0; i < n_eve;  i++) play_id_ [eve_verts [i]] = i;
            for (int i = 0; i < n_adam; i++) choose_id_[adam_verts[i]] = n_eve + i;
            for (int v = 0; v < n; v++) show_id_[v] = n_eve + n_adam + v;
        }

        // Build the AP-vertex map for a given (a_tilde, g_tilde) subgame.
        // Mirrors write_obliging_spec's AP construction exactly, using the
        // vertex maps already computed by build_vertex_maps().
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
                    else s.insert(choose_id_[v]);
                    s.insert(show_id_[v]);
                }
                return s;
            };

            std::map<std::string, std::set<int>> ap_verts;

            // in_play: play copies of Eve vertices
            {
                std::set<int> s;
                for (int v = 0; v < n; v++)
                    if (owner[v] == 0 && play_id_[v] >= 0) s.insert(play_id_[v]);
                ap_verts["in_play"] = std::move(s);
            }

            {
                std::set<int> s;
                for (int v = 0; v < n; v++) s.insert(show_id_[v]);
                ap_verts["show"] = std::move(s);
            }

            // a{i}_all for i in a_tilde
            for (int i : a_tilde)
                ap_verts["a" + std::to_string(i) + "_all"] = lift_all(spec.assumptions[i].states);

            // a{i}_all for assumptions NOT in a_tilde (needed for psi_full = GF(a{i}_all & show))
            std::set<int> a_tilde_set(a_tilde.begin(), a_tilde.end());
            for (int i = 0; i < (int)spec.assumptions.size(); i++) {
                if (!a_tilde_set.count(i))
                    ap_verts["a" + std::to_string(i) + "_all"] = lift_all(spec.assumptions[i].states);
            }

            // g{j}_all for j in g_tilde
            for (int j : g_tilde)
                ap_verts["g" + std::to_string(j) + "_all"] =
                    lift_all(spec.guarantees[j].states);

            // in_W: transformed vertices in the full-game winning region. Only added when W is known (non-empty)
            if (!winning_region_.empty()) {
                ap_verts["in_W"] = winning_region_;
            }

            return ap_verts;
        }
};