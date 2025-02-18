// Copyright (c) 2018-2019 Marco Wang <m.aesophor@gmail.com>
#ifndef WMDERLAND_ACTION_H_
#define WMDERLAND_ACTION_H_

#include <string>

namespace wmderland {

class Action {
 public:
  enum class Type {
    NAVIGATE_LEFT,
    NAVIGATE_RIGHT,
    NAVIGATE_DOWN,
    NAVIGATE_UP,
    TILE_H,
    TILE_V,
    TOGGLE_FLOATING,
    TOGGLE_FULLSCREEN,
    GOTO_WORKSPACE,
    MOVE_WINDOW_TO_WORKSPACE,
    KILL,
    EXIT,
    RELOAD,
    DEBUG_CRASH,
    EXEC,
    UNDEFINED
  };

  explicit Action(const std::string& s);
  explicit Action(Action::Type type);
  Action(Action::Type type, const std::string& argument);
  virtual ~Action() = default;

  Action::Type type() const;
  const std::string& argument() const;

 private:
  static Action::Type StrToActionType(const std::string& s);

  Action::Type type_;
  std::string argument_;
};

} // namespace wmderland

#endif // WMDERLAND_ACTION_H_
