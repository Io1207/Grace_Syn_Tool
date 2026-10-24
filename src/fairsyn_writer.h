#pragma once

#include "game.h"
#include <string>
#include <vector>
#include <set>
#include <ostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <numeric>
#include <thread>

struct SubgameSpec {
    std::vector<int> a_indices;
    std::vector<int> g_indices;
};

struct VertexMap {
    std::vector<int> eve_verts;
    std::vector<int> adam_verts;
    std::vector<int> all_verts;
    std::vector<int> fair_id;
    int n_used = 0;
};

inline VertexMap build_vertex_map(const Game& game, const std::set<int>& win_region) {
    const State_space* ss = game.get_state_space();
    const auto& owner = ss->get_owner();
    int n = game.num_states;

    VertexMap vm;
    vm.fair_id.assign(n, -1);

    for (int v = 0; v < n; v++) {
        if (!win_region.count(v)) continue;
        if ((owner)[v] == 0) vm.eve_verts.push_back(v);
        else vm.adam_verts.push_back(v);
    }
    vm.all_verts.insert(vm.all_verts.end(), vm.eve_verts.begin(),  vm.eve_verts.end());
    vm.all_verts.insert(vm.all_verts.end(), vm.adam_verts.begin(), vm.adam_verts.end());
    vm.n_used = static_cast<int>(vm.all_verts.size());

    for (int i = 0; i < vm.n_used; i++)  vm.fair_id[vm.all_verts[i]] = i + 1;

    return vm;
}

inline void write_arena_file(const Game& game, [[maybe_unused]] const std::set<int>& win_region, const VertexMap& vm, std::ostream& out) {
    const State_space* ss = game.get_state_space();
    const auto& post = ss->get_post();

    out << vm.n_used << "\n";

    for (int i = 0; i < (int)vm.eve_verts.size(); i++) {
        if (i) out << " ";
        out << vm.fair_id[vm.eve_verts[i]];
    }
    out << "\n";

    for (int i = 0; i < (int)vm.adam_verts.size(); i++) {
        if (i) out << " ";
        out << vm.fair_id[vm.adam_verts[i]];
    }
    out << "\n";

    for (int v : vm.all_verts) {
        bool first = true;
        for (int w : (post)[v]) {
            if (vm.fair_id[w] == -1) continue;
            if (!first) out << " ";
            out << vm.fair_id[w];
            first = false;
        }
        out << "\n";
    }
}

inline void write_arena_file(const Game& game, [[maybe_unused]] const std::set<int>& win_region, const VertexMap& vm, const std::string& path) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot open arena file: " + path);
    write_arena_file(game, win_region, vm, f);
}

inline void write_spec_file(const Game& game, const SubgameSpec& spec, const VertexMap& vm, std::ostream& out) {
    std::vector<std::string> ap_names;
    std::vector<std::set<int>> ap_verts;

    for (int ai : spec.a_indices) {
        ap_names.push_back("a" + std::to_string(ai));
        std::set<int> vs;
        for (int s : game.spec.assumptions[ai].states)
            if (s < (int)vm.fair_id.size() && vm.fair_id[s] != -1)
                vs.insert(vm.fair_id[s]);
        ap_verts.push_back(std::move(vs));
    }

    for (int gi : spec.g_indices) {
        ap_names.push_back("g" + std::to_string(gi));
        std::set<int> vs;
        for (int s : game.spec.guarantees[gi].states)
            if (s < (int)vm.fair_id.size() && vm.fair_id[s] != -1)
                vs.insert(vm.fair_id[s]);
        ap_verts.push_back(std::move(vs));
    }

    for (int i = 0; i < (int)ap_names.size(); i++) {
        if (i) out << ", ";
        out << ap_names[i];
    }
    out << "\n";

    for (int i = 0; i < (int)ap_names.size(); i++) {
        out << ap_names[i];
        for (int fv : ap_verts[i])
            out << ", " << fv;
        out << "\n";
    }

    std::string ant, con;
    for (int k = 0; k < (int)spec.a_indices.size(); k++) {
        if (k) ant += " & ";
        ant += "GF a" + std::to_string(spec.a_indices[k]);
    }
    for (int k = 0; k < (int)spec.g_indices.size(); k++) {
        if (k) con += " & ";
        con += "GF g" + std::to_string(spec.g_indices[k]);
    }
    if (ant.empty()) ant = "true";
    if (con.empty()) con = "true";

    out << ant << " -> " << con << "\n";
}

inline void write_spec_file(const Game& game, const SubgameSpec& spec, const VertexMap& vm, const std::string& path) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot open spec file: " + path);
    write_spec_file(game, spec, vm, f);
}

struct FairsynConfig {
    std::string owl_path;
    int ubdd_type = 2; // 0=Cudd (stable but slow), 1=Sylvan (fast), 2=Sylvan+Parallel
    int verbose = 0;
    bool accelerated = false;
    int M = 10;
    int n_workers  = 0;
};

inline void write_config_json(const std::string& arena_file, const std::string& spec_file, const std::string& output_base,
                              const std::string& hoa_file, const std::string& controller_bdd_file,
                              const std::string& winning_vertices_file, const FairsynConfig& fc, std::ostream& out) 
{    
    const int workers = fc.n_workers > 0 ? fc.n_workers : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    out << "{\n";
    out << "  \"arena_file\": \""  << arena_file  << "\",\n";
    out << "  \"spec_file\": \""   << spec_file   << "\",\n";
    out << "  \"output_file\": \"" << output_base << "\",\n";
    out << "  \"owl_path\": \""    << fc.owl_path << "\",\n";
    if (!hoa_file.empty())
        out << "  \"hoa_file\": \"" << hoa_file << "\",\n";
    if (!controller_bdd_file.empty())
        out << "  \"controller_bdd_file\": \"" << controller_bdd_file << "\",\n";
    if (!winning_vertices_file.empty())
        out << "  \"winning_vertices_file\": \"" << winning_vertices_file << "\",\n";
    out << "  \"ubdd_type\": "   << fc.ubdd_type << ",\n";
    out << "  \"verbose\": "     << fc.verbose   << ",\n";
    out << "  \"accelerated\": " << (fc.accelerated ? "true" : "false") << ",\n";
    out << "  \"M\": "           << fc.M         << ",\n";
    out << "  \"n_workers\": "   << workers      << "\n";
    out << "}\n";
}

inline void write_config_json(const std::string& arena_file, const std::string& spec_file, const std::string& output_base, const FairsynConfig& fc, std::ostream& out) {
    write_config_json(arena_file, spec_file, output_base, "", "", "", fc, out);
}

inline void write_config_json(const std::string& arena_file, const std::string& spec_file, const std::string& output_base,
                              const std::string& hoa_file, const std::string& controller_bdd_file,
                              const std::string& winning_vertices_file,
                              const FairsynConfig& fc, const std::string& path) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot open config file: " + path);
    write_config_json(arena_file, spec_file, output_base, hoa_file, controller_bdd_file, winning_vertices_file, fc, f);
}

inline void write_config_json(const std::string& arena_file, const std::string& spec_file, const std::string& output_base, const FairsynConfig& fc, const std::string& path) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot open config file: " + path);
    write_config_json(arena_file, spec_file, output_base, "", "", "", fc, f);
}

inline void write_fairsyn_inputs(const Game& game, const SubgameSpec& spec, const std::set<int>& win_region, const FairsynConfig& fc, const std::string& arena_path,
                                 const std::string& spec_path, const std::string& config_path, const std::string& output_base) {
    VertexMap vm = build_vertex_map(game, win_region);
    write_arena_file(game, win_region, vm, arena_path);
    write_spec_file(game, spec, vm, spec_path);
    write_config_json(arena_path, spec_path, output_base, fc, config_path);
}

inline void write_obliging_arena(const Game& orig, std::ostream& out) {
    const int n = orig.num_states;
    const auto& old_owner = orig.get_state_space()->get_owner();
    const auto& old_post = orig.get_state_space()->get_post();

    std::vector<int> eve_verts, adam_verts;
    for (int v = 0; v < n; v++) {
        if (old_owner[v] == 0) eve_verts.push_back(v);
        else adam_verts.push_back(v);
    }

    const int n_eve = static_cast<int>(eve_verts.size());
    const int n_adam = n - n_eve;
    const int sz = 2 * n;

    std::vector<int> play_id(n, -1), choose_id(n, -1), show_id(n, -1);
    for (int i= 0; i < n_eve; i++) play_id[eve_verts[i]] = i;
    for (int i = 0; i < n_adam; i++) choose_id[adam_verts[i]] = n_eve + i;
    for (int v = 0; v < n; v++) show_id[v] = n + v;

    out << sz << "\n";
    {
        bool first = true;
        for (int v : eve_verts)
            { if (!first) out << " "; out << play_id[v]; first = false; }
        for (int v = 0; v < n; v++)
            { if (!first) out << " "; out << show_id[v]; first = false; }
        out << "\n";
    }

    {
        bool first = true;
        for (int v : adam_verts)
            { if (!first) out << " "; out << choose_id[v]; first = false; }
        out << "\n";
    }

    std::vector<std::vector<int>> new_post(sz);
    for (int u = 0; u < n; u++) {
        bool is_adam = (old_owner[u] == 1);
        const auto& succs = old_post[u];
        if (!is_adam) {
            for (int v : succs) {
                int dst = (old_owner[v] == 0) ? play_id[v] : choose_id[v];
                new_post[play_id[u]].push_back(dst);
                new_post[show_id[u]].push_back(dst);
            }
        } else {
            for (int v : succs) {
                int dst = (old_owner[v] == 0) ? play_id[v] : choose_id[v];
                new_post[choose_id[u]].push_back(dst);
                new_post[show_id[u]].push_back(show_id[v]);
            }
            new_post[choose_id[u]].push_back(show_id[u]);
        }
    }
    
    for (int v = 0; v < sz; v++) {
        bool first = true;
        for (int w : new_post[v]) { if (!first) out << " "; out << w; first = false; }
        out << "\n";
    }
}

inline void write_obliging_arena(const Game& orig, const std::string& path) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot open arena file: " + path);
    write_obliging_arena(orig, f);
}

inline void write_obliging_spec (const Game& original, const GR1_spec& spec, const std::vector<int>& a_tilde, 
                                 const std::vector<int>& g_tilde, const std::set<int>& w_verts, std::ostream& out) {
    const int n = original.num_states;
    const auto& old_owner = original.get_state_space()->get_owner();

    std::vector<int> eve_verts, adam_verts;
    for (int v = 0; v < n; v++) {
        if (old_owner[v] == 0) eve_verts.push_back(v);
        else adam_verts.push_back(v);
    }
    const int n_eve = static_cast<int>(eve_verts.size());
    const int n_adam = n - n_eve;

    std::vector<int> play_id(n, -1), choose_id(n, -1), show_id(n, -1);
    for (int i= 0; i < n_eve; i++) play_id[eve_verts[i]] = i;
    for (int i = 0; i < n_adam; i++) choose_id[adam_verts[i]] = n_eve + i;
    for (int v = 0; v < n; v++) show_id[v] = n + v;

    auto lift_all = [&](const std::set<int>& verts) {
        std::set<int> s;
        for (int v: verts) {
            if (old_owner[v] == 0) s.insert(play_id[v]);
            else s.insert(choose_id[v]);
            s.insert(show_id[v]);
        }
        return s;
    };

    std::vector<std::string> ap_names;
    std::vector<std::set<int>> ap_verts;
    {
        ap_names.push_back("in_play");
        std::set<int> vs;
        for (int v : eve_verts) vs.insert(play_id[v]);
        ap_verts.push_back(std::move(vs));
    }

    // "show" AP: all show-region vertices (indices n_eve+n_adam .. 2n-1)
    {
        ap_names.push_back("show");
        std::set<int> vs;
        for (int v = 0; v < n; v++) vs.insert(show_id[v]);
        ap_verts.push_back(std::move(vs));
    }

    for (int i : a_tilde) {
        ap_names.push_back("a" + std::to_string(i) + "_all");
        ap_verts.push_back(lift_all(spec.assumptions[i].states));
    }

    std::set<int> a_tilde_set(a_tilde.begin(), a_tilde.end());
    for (int i = 0; i < (int)spec.assumptions.size(); i++) {
        if (!a_tilde_set.count(i)) {
            ap_names.push_back("a" + std::to_string(i) + "_all");
            ap_verts.push_back(lift_all(spec.assumptions[i].states));
        }
    }

    for (int i : g_tilde) {
        ap_names.push_back("g" + std::to_string(i) + "_all");
        ap_verts.push_back(lift_all(spec.guarantees[i].states));
    }

    if (!w_verts.empty()) {
        ap_names.push_back("in_W");
        std::set<int> wv(w_verts.begin(), w_verts.end());
        ap_verts.push_back(wv);
    }

    for (int i = 0; i < (int)ap_names.size(); i++) {
        if (i) out << " ";
        out << ap_names[i];
    }
    out << "\n";

    for (int i = 0; i < (int)ap_names.size(); i++) {
        out << ap_names[i];
        for (int v : ap_verts[i]) out << " " << v;
        out << "\n";
    }

    std::string phi;
    if (g_tilde.empty()) {
        phi = "true";
    } else {
        std::string guar_conj;
        for (size_t k = 0; k < g_tilde.size(); k++) {
            if (k) guar_conj += " & ";
            guar_conj += "GF(g" + std::to_string(g_tilde[k]) + "_all)";
        }
        if (a_tilde.empty()) {
            phi = guar_conj;
        } else {
            std::string assum_conj;
            for (size_t k = 0; k < a_tilde.size(); k++) {
                if (k) assum_conj += " & ";
                assum_conj += "GF(a" + std::to_string(a_tilde[k]) + "_all)";
            }
            phi = "(!(" + assum_conj + ")) | (" + guar_conj + ")";
        }
    }

    std::string b_or_psi;
    if (spec.assumptions.empty()) {
        b_or_psi = "GF(in_play)";
    } else {
        std::string psi_full;
        for (int i = 0; i < (int)spec.assumptions.size(); i++) {
            if (i) psi_full += " & ";
            psi_full += "GF(a" + std::to_string(i) + "_all & show)";
        }
        b_or_psi = "(" + psi_full + ") | GF(in_play)";
    }

    if (!w_verts.empty()) {
        if (phi == "true") phi = "G(in_W)";
        else phi = "(" + phi + ") & G(in_W)";
    }

    out << "(" << phi << ") & (" << b_or_psi << ")\n";
}

inline void write_obliging_spec (const Game& original, const GR1_spec& spec,
                                  const std::vector<int>& a_tilde, const std::vector<int>& g_tilde,
                                  const std::set<int>& w_verts, const std::string& path) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot open spec file: " + path);
    write_obliging_spec(original, spec, a_tilde, g_tilde, w_verts, f);
}

/***** FIXED: backward-compat overloads pass empty {} for w_verts (no G(in_W) constraint) *****/
// Backward-compatible overload: no W constraint (used for the full-game call itself)
inline void write_obliging_spec (const Game& original, const GR1_spec& spec,
                                  const std::vector<int>& a_tilde, const std::vector<int>& g_tilde,
                                  std::ostream& out) {
    write_obliging_spec(original, spec, a_tilde, g_tilde, {}, out);
}
inline void write_obliging_spec (const Game& original, const GR1_spec& spec,
                                  const std::vector<int>& a_tilde, const std::vector<int>& g_tilde,
                                  const std::string& path) {
    write_obliging_spec(original, spec, a_tilde, g_tilde, {}, path);
}

inline void write_obliging_iputs (const Game& original, const GR1_spec& spec, const FairsynConfig& fc, const std::string& arena_path, 
                                  const std::string& spec_path, const std::string& config_path, const std::string& output_base) {
    write_obliging_arena(original, arena_path);
    
    std::vector<int> a_all(spec.assumptions.size());
    std::iota(a_all.begin(), a_all.end(), 0);
    std::vector<int> g_all(spec.guarantees.size());
    std::iota(g_all.begin(), g_all.end(), 0);
    
    write_obliging_spec(original, spec, a_all, g_all, spec_path);

    // write_config_json(arena_path, spec_path, output_base, fc, config_path);
    namespace fs = std::filesystem;
    write_config_json(fs::absolute(arena_path).string(), fs::absolute(spec_path).string(), fs::absolute(output_base).string(), fc, config_path);
}