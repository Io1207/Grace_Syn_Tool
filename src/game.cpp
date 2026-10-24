#include "game.h"
#include <limits>
#include <algorithm>
#include <stdexcept>

State_space::State_space(int n)
    : owner (n, 0), post (n), pre (n), region(n, PLAY), num_states(n) {}

State_space::State_space(int n, int initial, std::vector<int> own, std::vector<std::vector<int>> edges)
    : owner (std::move(own)), post (std::move(edges)), pre (n), region (n, PLAY), num_states(n), init (initial) {
    for (int i = 0; i < n; i++)
        for (int j : post[i]) pre[j].push_back(i);
}

void State_space::add_edge(int from, int to) {
    post[from].push_back(to);
    pre[to].push_back(from);
}

Game::Game(int n, int initial, State_space ss, bool live, bool safe, bool fair)
    : ss(std::move(ss)), num_states(n), init(initial), liveness(live), safety(safe), fairness(fair) {}

Game Game::transform_play_choose_show() const {
    const int n = num_states;
    const auto& old_owner = ss.get_owner();
    const auto& old_post = ss.get_post();

    std::vector<int> eve_verts, adam_verts;
    for (int v = 0; v < n; v++) {
        if (old_owner[v] == 0) eve_verts.push_back(v);
        else adam_verts.push_back(v);
    }
    const int n_eve = static_cast<int>(eve_verts.size());
    const int n_adam = static_cast<int>(adam_verts.size());
    const int sz = n_eve + n_adam + n;

    std::vector<int> play_id(n, -1), choose_id(n, -1), show_id(n, -1);
    for (int i = 0; i < n_eve;  i++) play_id[eve_verts[i]] = i;
    for (int i = 0; i < n_adam; i++) choose_id[adam_verts[i]] = n_eve + i;
    for (int v = 0; v < n; v++) show_id[v] = n_eve + n_adam + v;

    std::vector<int>              new_owner (sz, 0);
    std::vector<std::vector<int>> new_post  (sz);
    std::vector<int>              new_region(sz, PLAY);

    for (int v : eve_verts) {
        new_owner[play_id[v]] = 0; new_region[play_id[v]] = PLAY;
    }
    for (int v : adam_verts) {
        new_owner[choose_id[v]] = 1; new_region[choose_id[v]] = CHOOSE;
    }
    for (int v = 0; v < n; v++) {
        new_owner[show_id[v]] = 0; new_region[show_id[v]] = SHOW;
    }

    for (int u = 0; u < n; u++) {
        const auto& succs = old_post[u];
        if (old_owner[u] == 0) {
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

    int new_init = (old_owner[init] == 0) ? play_id[init] : choose_id[init];

    State_space new_ss(sz);
    new_ss.init = new_init;
    new_ss.get_owner() = std::move(new_owner);
    new_ss.get_post() = std::move(new_post);
    new_ss.get_region() = std::move(new_region);
    for (int u = 0; u < sz; u++)
        for (int w : new_ss.get_post()[u])
            new_ss.get_pre()[w].push_back(u);

    Game result(sz, new_init, std::move(new_ss), liveness, safety, fairness);

    GR1_spec& rs = result.spec;
    rs.atomic_props = spec.atomic_props;

    for (const auto& a : spec.assumptions) {
        GR1_conjunct lifted;
        lifted.phi_str = a.phi_str;
        for (int s : a.states) lifted.states.insert(show_id[s]);
        rs.assumptions.push_back(std::move(lifted));
    }
    for (const auto& g : spec.guarantees) {
        GR1_conjunct lifted;
        lifted.phi_str = g.phi_str;
        for (int s : g.states) {
            if (old_owner[s] == 0) lifted.states.insert(play_id[s]);
            else lifted.states.insert(choose_id[s]);
            lifted.states.insert(show_id[s]);
        }
        rs.guarantees.push_back(std::move(lifted));
    }

    return result;
}

void Game::write_HOA(std::ostream& out) const
{
    out << "States: " << num_states << "\n";
    out << "Start: "  << ss.init   << "\n";
    for (int v = 0; v < num_states; v++)
        if (ss.get_owner()[v] == 0) out << v << " ";
    out << "\n";
    for (int v = 0; v < num_states; v++)
        if (ss.get_owner()[v] == 1) out << v << " ";
    out << "\n";
}

void Game::write_HOA(const std::string& filename) const
{
    std::ofstream f(filename);
    if (!f) { std::cerr << "Could not open " << filename << "\n"; return; }
    write_HOA(f);
}
namespace {

    struct PropParser {
        const std::string& expr;
        const std::map<std::string, std::set<int>>& ap_map;
        int n;
        size_t pos;

        std::set<int> all_verts() const {
            std::set<int> s;
            for (int i = 0; i < n; i++) s.insert(i);
            return s;
        }

        void skip_ws() { while (pos < expr.size() && expr[pos] == ' ') pos++; }
        
        std::string read_name() {
            skip_ws();
            size_t start = pos;
            while (pos < expr.size() && (isalnum(expr[pos]) || expr[pos] == '_')) pos++;
            return expr.substr(start, pos - start);
        }

        std::set<int> parse_or();
        std::set<int> parse_and();
        std::set<int> parse_not();
        std::set<int> parse_atom();
    };

    std::set<int> PropParser::parse_or() {
        auto lhs = parse_and(); skip_ws();
        while (pos < expr.size() && expr[pos] == '|') {
            pos++; auto rhs = parse_and();
            for (int v : rhs) lhs.insert(v);
        }
        return lhs;
    }

    std::set<int> PropParser::parse_and() {
        auto lhs = parse_not(); skip_ws();
        while (pos < expr.size() && expr[pos] == '&') {
            pos++; auto rhs = parse_not();
            std::set<int> res;
            for (int v : lhs) if (rhs.count(v)) res.insert(v);
            lhs = res;
        }
        return lhs;
    }

    std::set<int> PropParser::parse_not() {
        skip_ws();
        if (pos < expr.size() && expr[pos] == '!') {
            pos++; auto inner = parse_atom();
            std::set<int> res;
            for (int i = 0; i < n; i++) if (!inner.count(i)) res.insert(i);
            return res;
        }
        return parse_atom();
    }

    std::set<int> PropParser::parse_atom() {
        skip_ws();
        if (pos < expr.size() && expr[pos] == '(') {
            pos++; auto inner = parse_or(); skip_ws();
            if (pos < expr.size() && expr[pos] == ')') pos++;
            return inner;
        }
        auto name = read_name();
        if (name == "true") return all_verts();
        if (name == "false") return {};
        auto it = ap_map.find(name);
        if (it == ap_map.end())
            throw std::runtime_error("Unknown AP '" + name + "' in: " + expr);
        return it->second;
    }

    std::set<int> eval_prop(const std::string& expr, const std::map<std::string, std::set<int>>& ap_map, int n) {
        PropParser p{expr, ap_map, n, 0};
        return p.parse_or();
    }

    std::string strip_GF(const std::string& s) {
        std::string t = s;
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back()  == ' ') t.pop_back();
        if (t.size() >= 4 && (t.substr(0,3) == "GF(" || t.substr(0,3) == "gf("))
            return t.substr(3, t.size() - 4);
        if (t.size() >= 3 && (t.substr(0,2) == "GF" || t.substr(0,2) == "gf"))
            return t.substr(2);
        return t;
    }

}

// ---------------------------------------------------------------------------
// parse_game
//
// ENV_ASSUMPTIONS / SYS_GUARANTEES lines:
//   <formula_label> <v1>,<v2>,...
// The vertex list IS the states (Büchi set)
// ---------------------------------------------------------------------------
Game parse_game(std::istream& in)
{
    int n, initial;
    in >> n >> initial;

    std::vector<int> own  (n, 0);
    std::vector<std::vector<int>> edges(n);

    std::string token;
    in >> token; // "ARENA"

    for (int i = 0; i < n; i++) {
        int s, player;
        std::string edge_list;
        in >> s >> player >> edge_list;
        // own[s] = player;
        /***** FIXED: input convention 1=Eve,0=Adam normalized to internal 0=Eve,1=Adam *****/
        own[s] = (player == 1) ? 0 : 1;
        if (!edge_list.empty() && edge_list != "-") {
            std::stringstream ss2(edge_list);
            std::string e;
            while (std::getline(ss2, e, ','))
                if (!e.empty()) edges[s].push_back(std::stoi(e));
        }
    }

    State_space ss(n, initial, std::move(own), std::move(edges));
    Game g(n, initial, std::move(ss), true, false, false);

    auto parse_verts = [](const std::string& s) {
        std::set<int> result;
        std::stringstream ss(s); std::string tok;
        while (std::getline(ss, tok, ','))
            if (!tok.empty()) result.insert(std::stoi(tok));
        return result;
    };

    while (in >> token) {
        int cnt; in >> cnt;
        in.ignore();
        bool is_assum = (token == "ENV_ASSUMPTIONS");
        for (int i = 0; i < cnt; i++) {
            std::string phi_str, state_list;
            in >> phi_str >> state_list;
            in.ignore();
            GR1_conjunct cj;
            cj.phi_str = phi_str;
            cj.states = parse_verts(state_list);
            if (is_assum) g.spec.assumptions.push_back(std::move(cj));
            else    g.spec.guarantees.push_back(std::move(cj));
        }
    }
    
    g.n_a = static_cast<int>(g.spec.assumptions.size());
    g.n_g = static_cast<int>(g.spec.guarantees.size());
    return g;
}

Game parse_game_fairsyn(const std::string& arena_file, const std::string& spec_file) {
    std::ifstream af(arena_file);
    if (!af) throw std::runtime_error("Cannot open arena file: " + arena_file);

    int n;
    af >> n;
    af.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    std::vector<int> own(n, 0);
    std::vector<std::vector<int>> edges(n);

    { std::string line; std::getline(af, line);
      std::istringstream iss(line); int fid;
      while (iss >> fid) own[fid - 1] = 0; }
    { std::string line; std::getline(af, line);
      std::istringstream iss(line); int fid;
      while (iss >> fid) own[fid - 1] = 1; }

    std::vector<int> eve_verts, adam_verts;
    for (int i = 0; i < n; i++) {
        if (own[i] == 0) eve_verts.push_back(i);
        else adam_verts.push_back(i);
    }
    std::vector<int> order;
    order.insert(order.end(), eve_verts.begin(),  eve_verts.end());
    order.insert(order.end(), adam_verts.begin(), adam_verts.end());

    for (int row = 0; row < n; row++) {
        std::string line;
        if (!std::getline(af, line)) break;
        std::istringstream iss(line); int fid;
        while (iss >> fid) edges[order[row]].push_back(fid - 1);
    }

    int initial = eve_verts.empty() ? 0 : eve_verts[0];
    State_space ss(n, initial, std::move(own), std::move(edges));
    Game g(n, initial, std::move(ss), true, false, false);

    std::ifstream sf(spec_file);
    if (!sf) throw std::runtime_error("Cannot open spec file: " + spec_file);

    std::string ap_line;
    std::getline(sf, ap_line);
    std::vector<std::string> ap_names;
    { std::istringstream iss(ap_line); std::string tok;
      while (std::getline(iss, tok, ',')) {
            auto b = tok.find_first_not_of(" \t"), e = tok.find_last_not_of(" \t");

            if (b != std::string::npos)
                ap_names.push_back(tok.substr(b, e - b + 1));
      }
    }

    std::map<std::string, std::set<int>> ap_verts;
    for (const auto& name : ap_names) ap_verts[name] = {};

    std::string line;
    while (std::getline(sf, line)) {
        if (line.empty()) continue;
        auto comma = line.find(',');
        std::string first = (comma != std::string::npos) ? line.substr(0, comma) : line;

        auto b = first.find_first_not_of(" \t"), e = first.find_last_not_of(" \t");

        if (b == std::string::npos) continue;
        first = first.substr(b, e - b + 1);
        if (!ap_verts.count(first)) break; // LTL formula line
        std::istringstream iss(line.substr(comma + 1)); std::string tok;
        while (std::getline(iss, tok, ',')) {
            auto tb = tok.find_first_not_of(" \t"), te = tok.find_last_not_of(" \t");

            if (tb == std::string::npos) continue;

            tok = tok.substr(tb, te - tb + 1);
            if (!tok.empty()) ap_verts[first].insert(std::stoi(tok) - 1);
        }
    }

    std::vector<std::pair<int,std::string>> a_aps, g_aps;
    for (const auto& name : ap_names) {
        if (name.empty()) continue;
        int idx = name.size() > 1 ? std::stoi(name.substr(1)) : 0;
        if (name[0] == 'a') a_aps.push_back({idx, name});
        else if (name[0] == 'g') g_aps.push_back({idx, name});
    }
    std::sort(a_aps.begin(), a_aps.end());
    std::sort(g_aps.begin(), g_aps.end());

    for (const auto& [idx, name] : a_aps)
        g.spec.assumptions.push_back({name, ap_verts[name]});
    for (const auto& [idx, name] : g_aps)
        g.spec.guarantees.push_back({name, ap_verts[name]});

    return g;
}

// parse_game_with_spec  (rich format with APS section)
// Populates both g.spec and the caller's spec reference
// (may be the same object or a copy for the synthesis loop)
Game parse_with_spec(std::istream& in, GR1_spec& out_spec)
{
    int n, initial;
    in >> n >> initial;

    std::vector<int> own (n, 0);
    std::vector<std::vector<int>> edges(n);

    std::string token;
    in >> token;

    for (int i = 0; i < n; i++) {
        int s, player; std::string el;
        in >> s >> player >> el;
        
        own[s] = (player == 1) ? 0 : 1;
        if (!el.empty() && el != "-") {
            std::stringstream ss2(el); std::string e;
            while (std::getline(ss2, e, ','))
                if (!e.empty()) edges[s].push_back(std::stoi(e));
        }
    }

    State_space ss(n, initial, std::move(own), std::move(edges));
    Game g(n, initial, std::move(ss), true, false, false);

    std::map<std::string, std::set<int>> ap_map;

    auto parse_vset = [](const std::string& s) {
        std::set<int> res; std::stringstream ss(s); std::string tok;
        while (std::getline(ss, tok, ','))
            if (!tok.empty()) res.insert(std::stoi(tok));
        return res;
    };

    // Parse cnt conjunct lines from lines[start..], push to g.spec and out_spec
    auto parse_conjuncts = [&](const std::vector<std::string>& lines, int start, int cnt, bool is_assum) -> int {
        int found = 0, i = start;
        while (found < cnt && i < (int)lines.size()) {
            std::string line = lines[i++];
            while (!line.empty() && (line.front()==' '||line.front()=='\t'))
                line.erase(line.begin());
            if (line.empty()) continue;
            GR1_conjunct cj;
            cj.phi_str = strip_GF(line);
            cj.states = eval_prop(cj.phi_str, ap_map, n);
            if (is_assum) {
                g.spec.assumptions.push_back(cj);
                out_spec.assumptions.push_back(std::move(cj));
            } else {
                g.spec.guarantees.push_back(cj);
                out_spec.guarantees.push_back(std::move(cj));
            }
            found++;
        }
        return i;
    };

    auto process_stream_conjuncts = [&](int cnt, bool is_assum) {
        in.ignore();
        for (int i = 0; i < cnt; ) {
            std::string line; std::getline(in, line);
            while (!line.empty() && (line.front()==' '||line.front()=='\t'))
                line.erase(line.begin());
            if (line.empty()) continue;
            GR1_conjunct cj;
            cj.phi_str   = strip_GF(line);
            cj.states = eval_prop(cj.phi_str, ap_map, n);
            if (is_assum) {
                g.spec.assumptions.push_back(cj);
                out_spec.assumptions.push_back(std::move(cj));
            } else {
                g.spec.guarantees.push_back(cj);
                out_spec.guarantees.push_back(std::move(cj));
            }
            i++;
        }
    };

    while (in >> token) {
        if (token == "APS") {
            std::string line; std::getline(in, line);
            std::vector<std::string> rest;
            while (std::getline(in, line)) rest.push_back(line);

            int li = 0;
            while (li < (int)rest.size()) {
                if (rest[li].empty()) { li++; continue; }
                std::istringstream iss(rest[li]);
                std::string first; iss >> first;
                if (first == "ENV_ASSUMPTIONS" || first == "SYS_GUARANTEES") {
                    int cnt; iss >> cnt;
                    li = parse_conjuncts(rest, li + 1, cnt, first == "ENV_ASSUMPTIONS");
                } else {
                    std::string vs; iss >> vs;
                    AtomicProp ap{first, parse_vset(vs)};
                    ap_map[first] = ap.states;
                    g.spec.atomic_props.push_back(ap);
                    out_spec.atomic_props.push_back(std::move(ap));
                    li++;
                }
            }
        } else if (token == "ENV_ASSUMPTIONS") {
            int cnt; in >> cnt;
            process_stream_conjuncts(cnt, true);
        } else if (token == "SYS_GUARANTEES") {
            int cnt; in >> cnt;
            process_stream_conjuncts(cnt, false);
        }
    }
    g.n_a = static_cast<int>(g.spec.assumptions.size());
    g.n_g = static_cast<int>(g.spec.guarantees.size());
    return g;
}