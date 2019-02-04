#include <glog/logging.h>
#include <fstream>
#include <sstream>
#include "config.hpp"
#include "util.hpp"

using std::hex;
using std::pair;
using std::string;
using std::vector;
using std::stringstream;
using std::unordered_map;
using tiling::Action;

Config* Config::instance_;

Config* Config::GetInstance() {
    if (!instance_) {
        instance_ = new Config(CONFIG_FILE);
    }
    return instance_;
}

Config::Config(string filename) {
    // Load the default global values.
    gap_width_ = DEFAULT_GAP_WIDTH;
    border_width_ = DEFAULT_BORDER_WIDTH;
    min_window_width_ = MIN_WINDOW_WIDTH;
    min_window_height_ = MIN_WINDOW_HEIGHT;
    focused_color_ = DEFAULT_FOCUSED_COLOR;
    unfocused_color_ = DEFAULT_UNFOCUSED_COLOR;

    // Load the default keybinds.
    SetKeybindAction(DEFAULT_TILE_H_KEY, Action::TILE_H);
    SetKeybindAction(DEFAULT_TILE_V_KEY, Action::TILE_V);
    SetKeybindAction(DEFAULT_FOCUS_LEFT_KEY, Action::FOCUS_LEFT);
    SetKeybindAction(DEFAULT_FOCUS_RIGHT_KEY, Action::FOCUS_RIGHT);
    SetKeybindAction(DEFAULT_FOCUS_DOWN_KEY, Action::FOCUS_DOWN);
    SetKeybindAction(DEFAULT_FOCUS_UP_KEY, Action::FOCUS_UP);
    SetKeybindAction(DEFAULT_TOGGLE_FLOATING_KEY, Action::TOGGLE_FLOATING);
    SetKeybindAction(DEFAULT_TOGGLE_FULLSCREEN_KEY, Action::TOGGLE_FULLSCREEN);
    SetKeybindAction(DEFAULT_KILL_KEY, Action::KILL);
    SetKeybindAction(DEFAULT_EXIT_KEY, Action::EXIT);

    // If the file starts with ~, convert it to full path first.
    filename = sys_utils::ToAbsPath(filename);

    std::ifstream file(filename);
    string line;

    while (std::getline(file, line)) {
        string_utils::Strip(line);

        if (!line.empty() && line.at(0) != ';') {
            ReplaceSymbols(line);
            vector<string> tokens = string_utils::Split(line, ' ');
            string& first_token = tokens[0];

            if (first_token == "set") {
                string key = tokens[1];
                string value = tokens[3];
                if (string_utils::StartsWith(key, VARIABLE_PREFIX)) {
                    symtab_[key] = value;
                } else {
                    global_vars_[key] = value;
                }
            } else if (first_token == "assign") {
                string wm_class_name = tokens[1];
                string workspace_id = tokens[3];
                short workspace_id_short;
                stringstream(workspace_id) >> workspace_id_short;
                spawn_rules_[wm_class_name] = workspace_id_short;
            } else if (first_token == "floating") {
                string wm_class_name = tokens[1];
                string should_float = tokens[2];
                bool should_float_bool;
                stringstream(should_float) >> std::boolalpha >> should_float_bool;
                float_rules_[wm_class_name] = should_float_bool;
            } else if (first_token == "bindsym") {
                string modifier_and_key = tokens[1];
                string action_str = string_utils::Split(line, ' ', 2)[2];
                SetKeybindAction(modifier_and_key, wm_utils::StrToAction(action_str));

                if (string_utils::StartsWith(action_str, "exec")) {
                    SetKeybindAction(modifier_and_key, wm_utils::StrToAction(action_str));
                    string command = string_utils::Split(action_str, ' ', 1)[1];
                    keybind_cmds_[modifier_and_key] = command;
                }
            } else if (first_token == "exec") {
                string cmd = string_utils::Split(line, ' ', 1)[1];
                autostart_rules_.push_back(cmd);
            } else {
                LOG(INFO) << "Unrecognized symbol: " << first_token << ". Ignoring...";
            }
        }
    }

    file.close();

    // Override default global values with the values specified in config.
    stringstream(global_vars_["gap_width"]) >> gap_width_;
    stringstream(global_vars_["border_width"]) >> border_width_;
    stringstream(global_vars_["min_window_width"]) >> min_window_width_;
    stringstream(global_vars_["min_window_height"]) >> min_window_height_;
    stringstream(global_vars_["focused_color"]) >> hex >> focused_color_;
    stringstream(global_vars_["unfocused_color"]) >> hex >> unfocused_color_;
}

Config::~Config() {}


Action Config::GetKeybindAction(const string& modifier, const string& key) const {
    return keybind_rules_.at(modifier + '+' + key);
}

void Config::SetKeybindAction(const string& modifier_and_key, Action action) {
    keybind_rules_[modifier_and_key] = action;
}

void Config::ReplaceSymbols(string& s) const {
    for (auto symtab_record : symtab_) {
        string_utils::Replace(s, symtab_record.first, symtab_record.second);
    }
}


unsigned short Config::gap_width() const {
    return gap_width_;
}

unsigned short Config::border_width() const {
    return border_width_;
}

unsigned short Config::min_window_width() const {
    return min_window_width_;
}

unsigned short Config::min_window_height() const {
    return min_window_height_;
}

unsigned long Config::focused_color() const {
    return focused_color_;
}

unsigned long Config::unfocused_color() const {
    return unfocused_color_;
}


const unordered_map<string, string>& Config::global_vars() const {
    return global_vars_;
}

const unordered_map<string, short>& Config::spawn_rules() const {
    return spawn_rules_;
}

const unordered_map<string, bool>& Config::float_rules() const {
    return float_rules_;
}

const unordered_map<string, Action>& Config::keybind_rules() const {
    return keybind_rules_;
}

const unordered_map<string, string>& Config::keybind_cmds() const {
    return keybind_cmds_;
}

const vector<string>& Config::autostart_rules() const {
    return autostart_rules_;
}
