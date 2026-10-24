#include "game.h"
#include "player.h"
#include "fairsyn_writer.h"
#include "subgame.h"
#include "opt_lib.h"
#include "dynamic_policy.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <string>
#include <vector>
#include <filesystem>
#include <numeric>
#include <iomanip>
#include <map>

static void print_usage(const char* prog) {
    std::cerr <<
        "Usage:\n"
        "  " << prog << " <game_file> transform [--output <dir>]\n"
        "  " << prog << " --arena <arena.txt> --spec <spec.txt> transform [--output <dir>]\n" "\n"
        "  " << prog << " <game_file> synth --fairsyn <bin> [--workdir <dir>]\n"
        "                                   [--winregion <file>] [--owl-path <path>]\n"
        "                                   [--parallel]\n"
        "  " << prog << " --arena <arena.txt> --spec <spec.txt> synth --fairsyn <bin> ...\n" "\n"
        "  " << prog << " <game_file> play --workdir <dir> --steps <N>\n"
        "                              [--trace <adam_strategy.txt>] [--csv <out.csv>]\n"
        "                              [--alpha <a>] [--alpha-prime <a'>] [--k <int>]\n"
        "                              [--lambda <l>] [--lambda-prime <l'>] [--seed <s>]\n"
        "  " << prog << " --arena <a.txt> --spec <s.txt> play ...\n";
}

static void load_optlib_from_dir(const std::string& workdir, const std::string& controller_query_bin, [[maybe_unused]] Game& game, OptLib& lib) {
    namespace fs = std::filesystem;
    if (!fs::exists(workdir)) {
        std::cerr << "[error] workdir does not exist: " << workdir << "\n";
        return;
    }

    auto parse_list = [](const std::string& s, char open, char close) {
        std::vector<int> res;
        auto p1 = s.find(open), p2 = s.find(close, p1);
        if (p1 == std::string::npos) return res;
        std::string inner = s.substr(p1 + 1, p2 - p1 - 1);
        if (inner == "empty") return res;
        std::stringstream ss(inner); std::string tok;
        while (std::getline(ss, tok, ','))
            if (!tok.empty()) res.push_back(std::stoi(tok));
        return res;
    };

    /***** FIXED: load winning_vertices.txt (written during synth phase) so build_ap_verts includes in_W *****/
    // Load winning region W if available
    std::string win_file = workdir + "/winning_vertices.txt";
    if (fs::exists(win_file)) {
        std::set<int> W;
        std::ifstream wf(win_file);
        int v; while (wf >> v) W.insert(v);
        lib.set_winning_region(W);
        std::cerr << "[info] Loaded winning region: " << W.size() << " vertices.\n";
    }

    for (const auto& entry : fs::directory_iterator(workdir)) {
        
        if (!entry.is_directory()) continue;

        std::string label = entry.path().filename().string();
        if (label.rfind("A{", 0) != 0) continue; 

        std::string bdd_f = entry.path().string() + "/controller.bdd";
        std::string hoa_f = entry.path().string() + "/rabin.hoa";
        if (!fs::exists(bdd_f) || !fs::exists(hoa_f)) continue;

        auto gp = label.find("_G");
        if (gp == std::string::npos) continue;
        auto a_idx = parse_list(label.substr(0, gp), '{', '}');
        auto g_idx = parse_list(label.substr(gp + 1), '{', '}');

        try {
            lib.store({to_mask(a_idx), to_mask(g_idx)}, controller_query_bin, bdd_f, hoa_f);
        } catch (const std::exception& e) {
            std::cerr << "[warn] " << label << ": " << e.what() << "\n";
        }
    }
    std::cerr << "[info] OptLib: " << lib.size() << " subgames loaded.\n";
}

static int mode_transform(Game& game, const GR1_spec& full_spec, const std::string& out_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(out_dir);

    std::string arena_f = out_dir + "/full_arena.txt";
    std::string spec_f = out_dir + "/full_spec.txt";
    std::string cfg_f = out_dir + "/full_config.json";
    std::string out_base = out_dir + "/full_output";

    FairsynConfig fc_dummy;
    write_obliging_iputs(game, full_spec, fc_dummy, arena_f, spec_f, cfg_f, out_base);

    std::cout << "Written: " << arena_f << "\n";
    std::cout << "Written: " << spec_f << "\n";
    std::cout << "Written: " << cfg_f << "\n";
    std::cout << "Transformed states: " << game.num_states * 2 << "\n";
    return 0;
}

static int mode_synth(Game& game, const std::string& fairsyn_bin, const std::string& controller_query_bin,
                      const std::string& workdir, [[maybe_unused]] const std::string& winregion_f,
                      const FairsynConfig& fc, const GR1_spec& full_spec, bool parallel, bool keep_tmp = false) {
    // std::set<int> W = load_win_region(winregion_f, game.num_states);
    namespace fs = std::filesystem;
    std::string shared_arena = (fs::path(workdir) / "shared_arena.txt").string();
    fs::create_directories(workdir);
    write_obliging_arena(game, shared_arena);
    std::cerr << "[info] Shared arena written: " << shared_arena << "\n";

    OptLib lib(game);
    SynthesisConfig cfg;
    cfg.fairsyn_bin = fairsyn_bin;
    cfg.controller_query_bin = controller_query_bin;
    cfg.work_dir = workdir;
    cfg.fc = fc;
    cfg.full_spec = full_spec;
    cfg.verbose = true;
    cfg.parallel = parallel;
    cfg.keep_tmp = keep_tmp;
    cfg.shared_arena_path = fs::absolute(shared_arena).string();
    int ok = run_synthesis_loop(game, cfg, lib);
    std::cout << "Done. Realizable subgames: " << ok << "\n";
    return 0;
}

/***** FIXED: mode_play() now takes controller_query_bin parameter (was missing) *****/
static int mode_play(Game& game, const std::string& workdir, const std::string& controller_query_bin, int steps, DynamicPolicyParams params, const std::string& trace_file, 
                     const std::string& csv_file) {
    OptLib lib(game);
    load_optlib_from_dir(workdir, controller_query_bin, game, lib);
    if (lib.size() == 0) {
        std::cerr << "[error] No subgame strategies in '" << workdir << "'; run synth first.\n";
        return 1;
    }

    DynamicPolicy policy(game, lib, params);

    if (!trace_file.empty()) policy.load_adam_strategy(trace_file);
    
    if (!csv_file.empty()) {
        policy.set_csv(csv_file);
        std::cerr << "[info] CSV output: " << csv_file << "\n";
    }

    // auto fmt_vec = [](const std::vector<int>& vec) {
    //     if (vec.empty()) return std::string("empty");
    //     std::string s;
    //     for (int i = 0; i < (int)vec.size(); i++) {
    //         if (i) s += ",";
    //         s += std::to_string(vec[i]);
    //     }
    //     return s;
    // };

    std::map<SubgameKey, int> subgame_visits;

    int v = game.init;
    std::cout << "step 0: v=" << v << "\n";

    // std::map<std::pair<std::vector<int>,std::vector<int>>, int> subgame_visits;

    for (int t = 1; t <= steps; t++) {
        int next = policy.step(v, t);

        if (next == -1) {
            std::cerr << "No move at v=" << v << " (t=" << t << ")\n";
            break;
        }

        // std::cout << "step " << t << ": v=" << next << "  A_sub={" 
        //           << [&](){
        //                 std::string s;
        //                 for (int i = 0; i < (int)policy.last_a_tilde().size(); i++) {
        //                     if (i) s += ",";
        //                     s += std::to_string(policy.last_a_tilde()[i]);
        //                 }
        //                 return s; }() << "}  G_sub={" << [&](){
        //                 std::string s;
        //                 for (int i = 0; i < (int)policy.last_g_tilde().size(); i++) {
        //                     if (i) s += ",";
        //                     s += std::to_string(policy.last_g_tilde()[i]);
        //                 }
        //                 return s; }()
        //           << "}  w_empty=" << policy.w_empty() << "\n";
        // v = next;
        // auto key = std::make_pair(policy.last_a_tilde(), policy.last_g_tilde());
        // if (!policy.last_a_tilde().empty() || !policy.last_g_tilde().empty()) subgame_visits[key]++;

        if (policy.last_step_was_eve()) subgame_visits[policy.last_used()]++;

        std::cout << "step " << t << ": v=" << next << "  " << fmt_key(policy.last_used())
                << "  B={" << fmt_mask(policy.last_b()) << "}  w0="
                << std::fixed << std::setprecision(3) << policy.w_empty() << "\n";

        // std::cout << "step " << t << ": v=" << next << "  A={" << fmt_vec(policy.last_a_tilde())
        //           << "}  G={" << fmt_vec(policy.last_g_tilde()) << "}  w0=" << std::fixed << std::setprecision(3) << policy.w_empty() << "\n";
        // Debug: dump subgame states every 10 steps
        if (t % 10 == 0) {
            std::cout << "  [debug] subgame states at t=" << t << ":\n";
            policy.debug_dump_states(std::cout);
        }
        v = next;
    }

    // ── Post-play subgame summary ─────────────────────────────────────────
    std::cout << "\n════════════════════════════════════════\n";
    std::cout << " Post-play Subgame Summary\n";
    std::cout << "════════════════════════════════════════\n";
    // std::cout << " Final subgame : A={"
    //           << [&](){ std::string s; for(int i=0; i<(int)policy.last_a_tilde().size(); i++) { if(i) s += ","; s+=std::to_string(policy.last_a_tilde()[i]);} return s.empty() ? "empty" : s; }() << "} G={"
    //           << [&](){ std::string s; for(int i=0; i<(int)policy.last_g_tilde().size(); i++) { if(i) s += ","; s+=std::to_string(policy.last_g_tilde()[i]);} return s.empty() ? "empty" : s; }() << "}\n";

    std::cout << " Final subgame : " << fmt_key(policy.last_used()) << "\n";

    std::cout << " w_empty = " << std::fixed << std::setprecision(4) << policy.w_empty() << "\n";
    std::cout << "\n Assumption subset sampling probabilities (final step):\n";
    auto subset_probs = policy.compute_subset_probs(steps);
    for (const auto& sp : subset_probs) {
        // std::string a_str = sp.a_subset.empty() ? "empty" : "";
        // for (int i = 0; i < (int)sp.a_subset.size(); i++) {
        //     if (i) a_str += ",";
        //     a_str += std::to_string(sp.a_subset[i]);
        // }
        // std::cout << "  B={" << a_str << "} : " << std::setprecision(4) << sp.prob << "\n";

        std::cout << "  B={" << fmt_mask(sp.a_subset) << "} : " << std::setprecision(4) << sp.prob << "\n";
    }

    // top 5 most visited subgames
    std::cout << "\n Top subgames by Eve-step visit count:\n";
    // std::vector<std::pair<int, std::pair<std::vector<int>, std::vector<int>>>> ranked;
    // for (const auto& [key, cnt] : subgame_visits) ranked.push_back({cnt, key});
    // std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b){
    //     return a.first > b.first;
    // });
    // int show = std::min(5, (int)ranked.size());
    // for (int i = 0; i < show; i++) {
    //     auto fmt_vec = [](const std::vector<int>& vec) {
    //         if (vec.empty()) return std::string("empty");
    //         std::string s;
    //         for (int j = 0; j < (int)vec.size(); j++) {
    //             if (j) s += ",";
    //             s += std::to_string(vec[j]);
    //         }
    //         return s;
    //     };
    //     std::cout << "  " << (i+1) << ": A={" << fmt_vec(ranked[i].second.first) << "} G={" << fmt_vec(ranked[i].second.second) << "} — " << ranked[i].first << " Eve steps\n";

    std::vector<std::pair<int, SubgameKey>> ranked;
    for (const auto& [key, cnt] : subgame_visits) ranked.push_back({cnt, key});
    std::sort(ranked.begin(), ranked.end(),
            [](const auto& x, const auto& y) { return x.first > y.first; });
    int show = std::min(5, (int)ranked.size());
    for (int i = 0; i < show; i++){
        std::cout << "  " << (i + 1) << ": " << fmt_key(ranked[i].second) 
                  << " — " << ranked[i].first << " Eve steps\n";
    }
    std::cout << "════════════════════════════════════════\n\n";

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) { print_usage(argv[0]); return 1; }

    // Collect all flags first so --arena/--spec can appear anywhere
    std::string game_file;
    std::string arena_file;
    std::string spec_file;
    std::string mode;
    std::string out_dir = "output";
    std::string workdir = "output";
    std::string fairsyn_bin = "controller_synthesis";
    /***** FIXED: controller_query_bin added — pass with --controller-query <path> *****/
    std::string controller_query_bin = "controller_query";
    std::string winregion_f;
    std::string trace_file;
    std::string csv_file;
    int steps = 10000;
    DynamicPolicyParams dp;
    FairsynConfig fc;
    bool cfg_parallel = false;
    bool cfg_keep_tmp = false;

    // First positional arg: game_file or --arena flag.
    int arg_start = 1;
    if (std::string(argv[1]) == "--arena") {
        if (argc < 6) { print_usage(argv[0]); return 1; }
        arena_file = argv[2];
        if (std::string(argv[3]) != "--spec") { print_usage(argv[0]); return 1; }
        spec_file = argv[4];
        mode = argv[5];
        arg_start = 6;
    } else {
        game_file = argv[1];
        mode = argv[2];
        arg_start = 3;
    }

    for (int i = arg_start; i < argc; i++) {
        std::string a = argv[i];
        auto nxt = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << a << " needs argument\n"; exit(1); }
            return argv[++i];
        };
        if (a == "--output") out_dir = nxt();
        else if (a == "--workdir") workdir = nxt();
        else if (a == "--fairsyn") fairsyn_bin = nxt();
        else if (a == "--controller-query") controller_query_bin = nxt();
        else if (a == "--winregion") winregion_f = nxt();
        else if (a == "--trace") trace_file = nxt();
        else if (a == "--csv") csv_file = nxt();
        else if (a == "--steps") steps = std::stoi(nxt());
        else if (a == "--alpha") dp.alpha = std::stod(nxt());
        else if (a == "--alpha-prime")  dp.alpha_prime = std::stod(nxt());
        else if (a == "--lambda") dp.lambda = std::stod(nxt());
        else if (a == "--lambda-prime") dp.lambda_prime = std::stod(nxt());
        else if (a == "--eps") dp.eps = std::stod(nxt());
        else if (a == "--k") dp.k = std::stoi(nxt());
        else if (a == "--seed") dp.seed = std::stoul(nxt());
        else if (a == "--parallel") cfg_parallel = true;
        else if (a == "--skip-delete") cfg_keep_tmp = true;
        else if (a == "--owl-path") fc.owl_path = nxt();
        else if (a == "--ubdd-type") fc.ubdd_type = std::stoi(nxt());
        else if (a == "--verbose-syn") fc.verbose = std::stoi(nxt());
        else if (a == "--no-accel") fc.accelerated = false;
        else if (a == "--accel") fc.accelerated = true;
        else if (a == "--M") fc.M  = std::stoi(nxt());
        else if (a == "--n-workers") fc.n_workers = std::stoi(nxt());
        else { std::cerr << "Unknown: " << a << "\n"; print_usage(argv[0]); return 1; }
    }

    // Load the game.
    GR1_spec full_spec;
    Game game = [&]() -> Game {
        if (!arena_file.empty()) {
            std::cerr << "[info] Loading fairsyn format: " << arena_file << " + " << spec_file << "\n";
            return parse_game_fairsyn(arena_file, spec_file);
        } else {
            std::ifstream gf(game_file);
            if (!gf) { std::cerr << "Cannot open: " << game_file << "\n"; exit(1); }
            
            std::string first_line;
            
            std::string content_str((std::istreambuf_iterator<char>(gf)), std::istreambuf_iterator<char>());
            if (content_str.find("APS") != std::string::npos) {
                // Format with APS section header + GF(...) conjuncts
                std::istringstream iss(content_str);
                return parse_with_spec(iss, full_spec);
            } else {
                // Format with ENV_ASSUMPTIONS/SYS_GUARANTEES + "apname vertex_list" lines.
                // parse_game handles this format and now also sets n_a/n_g correctly.
                /***** FIXED: parse_game now sets n_a/n_g (see game.cpp fix) so this path works *****/
                std::istringstream iss(content_str);
                Game g = parse_game(iss);
                // Copy spec into full_spec for the synthesis loop
                full_spec.assumptions  = g.spec.assumptions;
                full_spec.guarantees   = g.spec.guarantees;
                full_spec.atomic_props = g.spec.atomic_props;
                return g;
            }
        }
    }();

    std::cerr << "[info] Game: " << game.num_states << " states, " << game.spec.assumptions.size() << " assumptions, " << game.spec.guarantees.size()  << " guarantees\n";

    // Warn early if controller_query binary is not findable
    if (mode == "synth" || mode == "play") {
        std::string check_cmd = "which " + controller_query_bin + " > /dev/null 2>&1";
        if (std::system(check_cmd.c_str()) != 0) {
            // Try as direct path
            std::ifstream bin_check(controller_query_bin);
            if (!bin_check.good())
                std::cerr << "[warn] controller_query binary not found: '"
                          << controller_query_bin
                          << "'. Set with --controller-query <path>.\n";
        }
    }

    if (mode == "transform") return mode_transform(game, full_spec, out_dir);
    else if (mode == "synth") return mode_synth(game, fairsyn_bin, controller_query_bin, workdir, winregion_f, fc, full_spec, cfg_parallel, cfg_keep_tmp);
    else if (mode == "play") return mode_play(game, workdir, controller_query_bin, steps, dp, trace_file, csv_file);
    else { std::cerr << "Unknown mode: " << mode << "\n"; print_usage(argv[0]); return 1; }
}