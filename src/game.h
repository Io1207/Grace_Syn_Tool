#pragma once

#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <memory>

enum Region { PLAY = 0, CHOOSE = 1, SHOW = 2 };

struct AtomicProp {
    std::string name;
    std::set<int> states;
};

struct GR1_conjunct {
    std::string phi_str;
    std::set<int> states;
};

struct GR1_spec {
    std::vector<AtomicProp> atomic_props;
    std::vector<GR1_conjunct> assumptions;
    std::vector<GR1_conjunct> guarantees;
};

class State_space {
private:
    std::vector<int> owner;
    std::vector<std::vector<int>> post;
    std::vector<std::vector<int>> pre;
    std::vector<int> region;
    std::vector<int> sys_vertices;
    std::vector<int> env_vertices;

public:
    int num_states;
    int init;

    State_space() = default;
    explicit State_space(int n);
    State_space(int n, int initial, std::vector<int> own_, std::vector<std::vector<int>> edges_);

    State_space(const State_space&) = delete;
    State_space& operator=(const State_space&) = delete;
    State_space(State_space&&) = default;
    State_space& operator=(State_space&&) = default;

    std::vector<int>& get_owner() { return owner; }
    const std::vector<int>& get_owner() const { return owner; }
    std::vector<int>& get_post(int s) { return post[s]; }
    const std::vector<int>& get_post(int s) const { return post[s]; }
    std::vector<int>& get_pre(int s) { return pre[s]; }
    std::vector<std::vector<int>>& get_pre() { return pre; }
    const std::vector<std::vector<int>>& get_post() const { return post; }
    std::vector<std::vector<int>>& get_post() { return post; }

    std::vector<int>& get_region() { return region; }
    const std::vector<int>& get_region() const { return region; }

    std::vector<int>* get_sys_vertices() { return &sys_vertices; }
    std::vector<int>* get_env_vertices() { return &env_vertices; }
    void set_sys_env_vertices ();

    void add_edge(int from, int to);
};

class Game {
    private:
        State_space ss;

    public:
        int num_states = 0;
        int init = 0;
        bool liveness = true;
        bool safety = false;
        bool fairness = false;

        int n_a;
        int n_g;

        GR1_spec spec;

        Game() = default;
        Game(int n, int initial, State_space arena, bool live, bool safe, bool fair);

        Game(const Game&) = delete;
        Game& operator=(const Game&) = delete;
        Game(Game&&) = default;
        Game& operator=(Game&&) = default;

        State_space* get_state_space() { return &ss; }
        const State_space* get_state_space() const { return &ss; }

        Game transform_play_choose_show() const;

        void write_HOA(std::ostream& out) const;
        void write_HOA(const std::string& filename) const;
};

Game parse_game(std::istream& in);
Game parse_game_fairsyn(const std::string& arena_file, const std::string& spec_file);
Game parse_with_spec(std::istream& arena_in, GR1_spec& specs);