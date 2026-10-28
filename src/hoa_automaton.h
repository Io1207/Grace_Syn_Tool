#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

class HoaAutomaton {
public:
    // Parse from an already-open stream
    static HoaAutomaton parse(std::istream& in) {
        HoaAutomaton aut;
        aut.parse_impl(in);
        return aut;
    }

    static HoaAutomaton parse_file(const std::string& path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("Cannot open HOA file: " + path);
        return parse(f);
    }

    int start_state() const { return start_; }
    int num_states() const { return num_states_; }

    int ap_index(const std::string& name) const {
        auto it = ap_name_to_idx_.find(name);
        return (it == ap_name_to_idx_.end()) ? -1 : it->second;
    }

    int step(int q, const std::set<std::string>& true_aps) const {
        std::vector<bool> val(ap_names_.size(), false);
        for (const auto& name : true_aps) {
            int idx = ap_index(name);
            if (idx != -1) val[idx] = true;
        }
        return step_bits(q, val);
    }

    int step_bits(int q, const std::vector<bool>& val) const {
        if (q < 0 || q >= (int)transitions_.size())
            throw std::out_of_range("HoaAutomaton::step: state " + std::to_string(q) + " out of range");
        for (const auto& [guard, target] : transitions_[q]) {
            if (eval_guard(guard, val)) return target;
        }
        throw std::runtime_error("HoaAutomaton::step: no matching transition from state "
                                  + std::to_string(q) + " (automaton not exhaustive for this valuation)");
    }

private:
    struct GuardNode {
        enum Kind { TRUE, FALSE, AP, NOT, AND, OR } kind;
        int ap_idx = -1;
        std::vector<GuardNode> children;
    };

    int start_ = 0;
    int num_states_ = 0;
    std::vector<std::string> ap_names_;
    std::map<std::string, int> ap_name_to_idx_;
    std::vector<std::vector<std::pair<GuardNode,int>>> transitions_;

    static bool eval_guard(const GuardNode& g, const std::vector<bool>& val) {
        switch (g.kind) {
            case GuardNode::TRUE: return true;
            case GuardNode::FALSE: return false;
            case GuardNode::AP: return val[g.ap_idx];
            case GuardNode::NOT: return !eval_guard(g.children[0], val);
            case GuardNode::AND:
                for (const auto& c : g.children) if (!eval_guard(c, val)) return false;
                return true;
            case GuardNode::OR:
                for (const auto& c : g.children) if (eval_guard(c, val)) return true;
                return false;
        }
        return false;
    }

    // Guard parser
    // Grammar (standard HOA label-expr precedence, '!' tightest, then '&', then '|'):
    //        or := and ('|' and)*
    //        and := not ('&' not)*
    //        not := '!' not | atom
    //        atom := '(' or ')' | 't' | 'f' | <ap-index digits>
    struct GuardParser {
        const std::string& s;
        size_t pos = 0;

        void skip_ws() { while (pos < s.size() && s[pos] == ' ') pos++; }

        GuardNode parse_or() {
            GuardNode lhs = parse_and();
            skip_ws();
            if (pos >= s.size() || s[pos] != '|') return lhs;
            GuardNode node; node.kind = GuardNode::OR;
            node.children.push_back(std::move(lhs));
            while (pos < s.size() && s[pos] == '|') {
                pos++;
                node.children.push_back(parse_and());
                skip_ws();
            }
            return node;
        }

        GuardNode parse_and() {
            GuardNode lhs = parse_not();
            skip_ws();
            if (pos >= s.size() || s[pos] != '&') return lhs;
            GuardNode node; node.kind = GuardNode::AND;
            node.children.push_back(std::move(lhs));
            while (pos < s.size() && s[pos] == '&') {
                pos++;
                node.children.push_back(parse_not());
                skip_ws();
            }
            return node;
        }

        GuardNode parse_not() {
            skip_ws();
            if (pos < s.size() && s[pos] == '!') {
                pos++;
                GuardNode node; node.kind = GuardNode::NOT;
                node.children.push_back(parse_not());
                return node;
            }
            return parse_atom();
        }

        GuardNode parse_atom() {
            skip_ws();
            if (pos < s.size() && s[pos] == '(') {
                pos++;
                GuardNode inner = parse_or();
                skip_ws();
                if (pos < s.size() && s[pos] == ')') pos++;
                return inner;
            }
            if (pos < s.size() && s[pos] == 't') { pos++; GuardNode n; n.kind = GuardNode::TRUE; return n; }
            if (pos < s.size() && s[pos] == 'f') { pos++; GuardNode n; n.kind = GuardNode::FALSE; return n; }
            size_t start = pos;
            while (pos < s.size() && isdigit((unsigned char)s[pos])) pos++;
            if (pos == start)
                throw std::runtime_error("HoaAutomaton: cannot parse guard atom at position "
                                          + std::to_string(start) + " in '" + s + "'");
            GuardNode n; n.kind = GuardNode::AP;
            n.ap_idx = std::stoi(s.substr(start, pos - start));
            return n;
        }
    };

    static GuardNode parse_guard(const std::string& text) {
        GuardParser p{text, 0};
        return p.parse_or();
    }

    void parse_impl(std::istream& in) {
        std::string line;
        bool in_body = false;
        int cur_state = -1;

        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::string trimmed = trim(line);
            if (trimmed.empty()) continue;

            if (!in_body) {
                if (starts_with(trimmed, "States:")) {
                    num_states_ = std::stoi(trim(trimmed.substr(7)));
                    transitions_.resize(num_states_);
                } else if (starts_with(trimmed, "Start:")) {
                    start_ = std::stoi(trim(trimmed.substr(6)));
                } else if (starts_with(trimmed, "AP:")) {
                    parse_ap_line(trimmed.substr(3));
                } else if (trimmed == "--BODY--") {
                    in_body = true;
                    if ((int)transitions_.size() < num_states_)
                        transitions_.resize(num_states_);
                }
                continue;
            }

            if (trimmed == "--END--") break;

            if (starts_with(trimmed, "State:")) {
                std::istringstream iss(trimmed.substr(6));
                iss >> cur_state;
                if (cur_state >= (int)transitions_.size())
                    transitions_.resize(cur_state + 1);
                continue;
            }

            if (!trimmed.empty() && trimmed[0] == '[') {
                size_t close = trimmed.find(']');
                if (close == std::string::npos)
                    throw std::runtime_error("HoaAutomaton: malformed transition line: " + trimmed);
                std::string guard_text = trimmed.substr(1, close - 1);
                std::istringstream rest(trimmed.substr(close + 1));
                int target;
                if (!(rest >> target))
                    throw std::runtime_error("HoaAutomaton: missing target state in: " + trimmed);

                if (cur_state < 0)
                    throw std::runtime_error("HoaAutomaton: transition line before any 'State:' line");

                GuardNode guard = parse_guard(guard_text);
                transitions_[cur_state].emplace_back(std::move(guard), target);
            }
        }

        if (num_states_ == 0) num_states_ = (int)transitions_.size();
    }

    void parse_ap_line(const std::string& rest) {
        std::istringstream iss(rest);
        int count;
        iss >> count;
        ap_names_.clear();
        ap_name_to_idx_.clear();
        std::string token;
        while (iss >> token) {
            if (token.size() >= 2 && token.front() == '"' && token.back() == '"')
                token = token.substr(1, token.size() - 2);
            int idx = (int)ap_names_.size();
            ap_names_.push_back(token);
            ap_name_to_idx_[token] = idx;
        }
        if ((int)ap_names_.size() != count)
            throw std::runtime_error("HoaAutomaton: AP: line declared " + std::to_string(count)
                                      + " APs but found " + std::to_string(ap_names_.size()));
    }

    static std::string trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t");
        return s.substr(b, e - b + 1);
    }

    static bool starts_with(const std::string& s, const std::string& prefix) {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }
};