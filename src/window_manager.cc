// Copyright (c) 2018-2019 Marco Wang <m.aesophor@gmail.com>
#include "window_manager.h"

extern "C" {
#include <X11/Xproto.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
}
#include <iostream>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

#include "client.h"
#include "config.h"
#include "util.h"

#define MOUSE_LEFT_BTN 1
#define MOUSE_MID_BTN 2
#define MOUSE_RIGHT_BTN 3

#define NORMAL_CURSOR 0
#define MOVE_CURSOR 1
#define RESIZE_CURSOR 3

#define WM_STATE_WITHDRAWN 0
#define WM_STATE_NORMAL 1
#define WM_STATE_ICONIC 3

using std::pair;
using std::string;
using std::vector;
using std::unordered_map;
using std::exception;

namespace wmderland {

WindowManager* WindowManager::instance_ = nullptr;
bool WindowManager::is_running_ = true;

WindowManager* WindowManager::GetInstance() {
  if (!instance_) {
    Display* dpy;
    try {
      instance_ = (dpy = XOpenDisplay(None)) ? new WindowManager(dpy) : nullptr;
    } catch (const std::bad_alloc& ex) {
      fputs("Out of memory\n", stderr);
      instance_ = nullptr;
    }
  }
  return instance_;
}

WindowManager::WindowManager(Display* dpy) 
    : dpy_(dpy), 
      root_window_(DefaultRootWindow(dpy_)),
      wmcheckwin_(XCreateSimpleWindow(dpy_, root_window_, 0, 0, 1, 1, 0, 0, 0)),
      cursors_(),
      prop_(new Properties(dpy_)),
      config_(new Config(dpy_, prop_.get(), CONFIG_FILE)),
      cookie_(dpy_, prop_.get(), COOKIE_FILE),
      ipc_evmgr_(),
      snapshot_(SNAPSHOT_FILE),
      docks_(),
      notifications_(),
      workspaces_(),
      current_(),
      btn_pressed_event_() {
  if (HasAnotherWmRunning()) {
    std::cerr << "Another window manager is already running." << std::endl;
    return;
  }

  // Export this env variable to fix java applications' rendering problem.
  const char* java_non_reparenting_fix = "_JAVA_AWT_WM_NONREPARENTING=1";
  putenv(const_cast<char*>(java_non_reparenting_fix));

  // Initialization.
  wm_utils::Init(dpy_, prop_.get(), root_window_);
  config_->Load();
  InitWorkspaces();
  InitProperties();
  InitXGrabs();
  InitCursors();
  XSync(dpy_, false);

  // Run the autostart_cmds defined in user's config.
  for (const auto& cmd : config_->autostart_cmds()) {
    sys_utils::ExecuteCmd(cmd);
  }
}

WindowManager::~WindowManager() {
  WM_LOG(INFO, "releasing resources");
  XCloseDisplay(dpy_);

  for (const auto workspace : workspaces_) {
    delete workspace;
  }
}


bool WindowManager::HasAnotherWmRunning() {
  // WindowManager::OnWmDetected is a special error handler which will set
  // WindowManager::is_running_ to false if another WM is already running.
  XSetErrorHandler(&WindowManager::OnWmDetected);
  XSelectInput(dpy_, root_window_, SubstructureNotifyMask | SubstructureRedirectMask);
  XSync(dpy_, false);
  XSetErrorHandler(&WindowManager::OnXError);
  return !is_running_;
}

void WindowManager::InitXGrabs() {
  // Define the key combinations which will send us X events based on the key combinations 
  // defined in user's config.
  for (const auto& rule : config_->keybind_rules()) {
    unsigned int modifier = rule.first.first;
    KeyCode keycode = rule.first.second;

    XGrabKey(dpy_, keycode, modifier, root_window_, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy_, keycode, modifier | LockMask, root_window_, True, GrabModeAsync, GrabModeAsync);
  }

  // Define which mouse clicks will send us X events.
  XGrabButton(dpy_, AnyButton, Mod4Mask, root_window_, True,
      ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);
}

void WindowManager::InitCursors() {
  cursors_[NORMAL_CURSOR] = XCreateFontCursor(dpy_, XC_left_ptr);
  cursors_[RESIZE_CURSOR] = XCreateFontCursor(dpy_, XC_sizing);
  cursors_[MOVE_CURSOR] = XCreateFontCursor(dpy_, XC_fleur);
  XDefineCursor(dpy_, root_window_, cursors_[NORMAL_CURSOR]);
}

void WindowManager::InitProperties() {
  char* win_mgr_name = const_cast<char*>(WIN_MGR_NAME);
  size_t win_mgr_name_len = std::strlen(win_mgr_name);

  // Set the name of window manager (i.e., Wmderland) on the root_window_ window,
  // so that other programs can acknowledge the name of this WM.
  XChangeProperty(dpy_, root_window_, prop_->net[atom::NET_WM_NAME], prop_->utf8string,
      8, PropModeReplace, reinterpret_cast<unsigned char*>(win_mgr_name), win_mgr_name_len);

  // Supporting window for _NET_WM_SUPPORTING_CHECK which tells other client
  // a compliant window manager exists.
  XChangeProperty(dpy_, wmcheckwin_, prop_->net[atom::NET_SUPPORTING_WM_CHECK], XA_WINDOW,
      32, PropModeReplace, reinterpret_cast<unsigned char*>(&wmcheckwin_), 1);

  XChangeProperty(dpy_, wmcheckwin_, prop_->net[atom::NET_SUPPORTING_WM_CHECK], prop_->utf8string,
      8, PropModeReplace, reinterpret_cast<unsigned char*>(win_mgr_name), win_mgr_name_len);

  XChangeProperty(dpy_, root_window_, prop_->net[atom::NET_SUPPORTING_WM_CHECK], XA_WINDOW,
      32, PropModeReplace, reinterpret_cast<unsigned char*>(&wmcheckwin_), 1);

  // Initialize NET_CLIENT_LIST to empty.
  XDeleteProperty(dpy_, root_window_, prop_->net[atom::NET_CLIENT_LIST]);

  // Set _NET_SUPPORTED to indicate which atoms are supported by this window manager.    
  XChangeProperty(dpy_, root_window_, prop_->net[atom::NET_SUPPORTED], XA_ATOM,
      32, PropModeReplace, reinterpret_cast<unsigned char*>(prop_->net), atom::NET_ATOM_SIZE);

  // Set _NET_NUMBER_OF_DESKTOP, _NET_CURRENT_DESKTOP, _NET_DESKTOP_VIEWPORT and _NET_DESKTOP_NAMES
  // to support polybar's xworkspace module.
  unsigned long workspace_count = workspaces_.size();
  XChangeProperty(dpy_, root_window_, prop_->net[atom::NET_NUMBER_OF_DESKTOPS], XA_CARDINAL,
      32, PropModeReplace, reinterpret_cast<unsigned char*>(&workspace_count), 1);

  XChangeProperty(dpy_, root_window_, prop_->net[atom::NET_CURRENT_DESKTOP], XA_CARDINAL,
      32, PropModeReplace, reinterpret_cast<unsigned char*>(&current_), 1);

  unsigned long desktop_viewport_cord[2] = {0, 0};
  XChangeProperty(dpy_, root_window_, prop_->net[atom::NET_DESKTOP_VIEWPORT], XA_CARDINAL,
      32, PropModeReplace, reinterpret_cast<unsigned char*>(desktop_viewport_cord), 2);
}

void WindowManager::InitWorkspaces() {
  char* names[workspaces_.size()];

  for (size_t i = 0; i < workspaces_.size(); i++) {
    // Initialize workspace objects.
    workspaces_[i] = new Workspace(dpy_, root_window_, config_.get(), i);

    // Copy workspace name const char* to names[i], which is needed later.
    // See the XSetTextProperty below. 
    names[i] = const_cast<char*>(workspaces_[i]->name());
  }

  // Set NET_DESKTOP_NAMES to display workspace names in polybar's xworkspace module.
  XTextProperty text_prop;
  Xutf8TextListToTextProperty(dpy_, names, workspaces_.size(), XUTF8StringStyle, &text_prop);
  XSetTextProperty(dpy_, root_window_, &text_prop, prop_->net[atom::NET_DESKTOP_NAMES]);
}


void WindowManager::Run() {
  XEvent event;

  while (is_running_) {
    // Retrieve and dispatch next X event.
    XNextEvent(dpy_, &event);
    switch (event.type) {
      case ConfigureRequest:
        OnConfigureRequest(event.xconfigurerequest);
        break;
      case MapRequest:
        OnMapRequest(event.xmaprequest);
        break;
      case MapNotify:
        OnMapNotify(event.xmap);
        break;
      case UnmapNotify:
        OnUnmapNotify(event.xunmap);
        break;
      case DestroyNotify:
        OnDestroyNotify(event.xdestroywindow);
        break;
      case KeyPress:
        OnKeyPress(event.xkey);
        break;
      case ButtonPress:
        OnButtonPress(event.xbutton);
        break;
      case ButtonRelease:
        OnButtonRelease(event.xbutton);
        break;
      case MotionNotify:
        OnMotionNotify(event.xbutton);
        break;
      case ClientMessage:
        OnClientMessage(event.xclient);
        break;
      default:
        // Unhandled X Events are ignored.
        break;
    }
  }
}

// Arranges the windows in current workspace to how they ought to be.
// 1. Tiled windows will be re-tiled.
// 2. Docks will be mapped, but it will be unmapped if there's a fullscreen window.
// 3. _NET_ACTIVE_WINDOW will be updated.
// 4. Floating windows and notifications will be raised to the top.
// 5. Fullscreen window will be resized to match the resolution again.
void WindowManager::ArrangeWindows() const {
  workspaces_[current_]->MapAllClients();
  MapDocks();
 
  Client* focused_client = workspaces_[current_]->GetFocusedClient();
  if (!focused_client) {
    wm_utils::ClearNetActiveWindow();
    return;
  }

  // Update NET_ACTIVE_WINDOW
  wm_utils::SetNetActiveWindow(focused_client->window());
  workspaces_[current_]->Tile(GetTilingArea());

  // Make sure the focused client is receiving input focus.
  workspaces_[current_]->SetFocusedClient(focused_client->window());

  // But floating clients including notifications should be on top of
  // any tiled clients.
  workspaces_[current_]->RaiseAllFloatingClients();
  RaiseNotifications();

  // Restore fullscreen application.
  if (workspaces_[current_]->is_fullscreen()) {
    UnmapDocks();
    focused_client->SetBorderWidth(0);
    focused_client->MoveResize(0, 0, GetDisplayResolution());
    focused_client->Raise();
  }
}


void WindowManager::OnConfigureRequest(const XConfigureRequestEvent& e) {
  XWindowChanges changes;
  changes.x = e.x;
  changes.y = e.y;
  changes.width = e.width;
  changes.height = e.height;
  changes.border_width = e.border_width;
  changes.sibling = e.above;
  changes.stack_mode = e.detail;
  XConfigureWindow(dpy_, e.window, e.value_mask, &changes);
  ArrangeWindows();
}

void WindowManager::OnMapRequest(const XMapRequestEvent& e) {
  // If user has requested to prohibit this window from being mapped, then don't map it.
  if (config_->ShouldProhibit(e.window)) {
    return;
  }

  wm_utils::SetWindowWmState(e.window, WM_STATE_NORMAL);

  // If this window is a dock (or bar), map it, add it to docks_
  // and arrange the workspace.
  if (wm_utils::IsDock(e.window)
      && std::find(docks_.begin(), docks_.end(), e.window) == docks_.end()) {
    XMapWindow(dpy_, e.window);
    docks_.push_back(e.window);
    workspaces_[current_]->Tile(GetTilingArea());
    return;
  }
  
  // If this window is wine steam dialog, just map it directly and don't manage it.
  pair<string, string> hint = wm_utils::GetXClassHint(e.window);
  if (wm_utils::IsDialog(e.window)) {
    if (hint.first == "Wine" && hint.second == "steam.exe") {
      XMapWindow(dpy_, e.window);
      return;
    }
  }

  // Pass all checks above -> we should manage this window.
  // Spawn this window in the specified workspace if such rule exists,
  // otherwise spawn it in current workspace.
  int target = config_->GetSpawnWorkspaceId(e.window);

  if (target == UNSPECIFIED_WORKSPACE) {
    target = current_;
  }


  // If this window is already in this workspace, don't add it to this workspace again.
  if (workspaces_[target]->Has(e.window)) {
    return;
  }

  bool should_float = config_->ShouldFloat(e.window)
    || wm_utils::IsDialog(e.window)
    || wm_utils::IsSplash(e.window)
    || wm_utils::IsUtility(e.window);

  bool should_fullscreen = config_->ShouldFullscreen(e.window)
    || wm_utils::HasNetWmStateFullscreen(e.window);

  Client* prev_focused_client = workspaces_[target]->GetFocusedClient();
  workspaces_[target]->UnsetFocusedClient();
  workspaces_[target]->Add(e.window);
  workspaces_[target]->GetClient(e.window)->set_mapped(true);
  workspaces_[target]->GetClient(e.window)->set_floating(should_float);
  UpdateClientList(); // update NET_CLIENT_LIST

  if (workspaces_[target]->is_fullscreen()) {
    workspaces_[target]->SetFocusedClient(prev_focused_client->window());
  }

  if (should_float) {
    SetFloating(e.window, true, /*use_default_size=*/false);
  }

  if (should_fullscreen) {
    SetFullscreen(e.window, true);
  }

  if (target == current_ && !workspaces_[current_]->is_fullscreen()) {
    ArrangeWindows();
  }
}

void WindowManager::OnMapNotify(const XMapEvent& e) {
  // Checking if a window is a notification in OnMapRequest() will fail (especially dunst),
  // So we perform the check here (after the window is mapped) instead.
  if (wm_utils::IsNotification(e.window) 
      && std::find(notifications_.begin(), notifications_.end(), e.window) == notifications_.end()) {
    notifications_.push_back(e.window);
  }

  auto it = Client::mapper_.find(e.window);
  if (it == Client::mapper_.end()) {
    return;
  }

  Client* c = it->second;
  c->set_mapped(true);
}

void WindowManager::OnUnmapNotify(const XUnmapEvent& e) {
  auto it = Client::mapper_.find(e.window);
  if (it == Client::mapper_.end()) {
    return;
  }

  // Some program unmaps their windows but does not remove them,
  // so if this window has just been unmapped, but it was not unmapped
  // by the user, then we will remove them for user.
  Client* c = it->second;
  pair<string, string> hint = wm_utils::GetXClassHint(c->window());

  if (!c->has_unmap_req_from_wm() && hint.first.find("Gimp") == hint.first.npos) {
    KillClient(c->window());
    XSync(dpy_, false); // make sure the event we just sent has been processed by server
    XDestroyWindow(dpy_, c->window()); // make sure the window is really destroyed
  } else {
    c->set_mapped(false);
    c->set_has_unmap_req_from_wm(false);
  }
}

void WindowManager::OnDestroyNotify(const XDestroyWindowEvent& e) {
  // If the window is a dock/bar or notification, remove it and tile the workspace.
  if (std::find(docks_.begin(), docks_.end(), e.window) != docks_.end()) {
    docks_.erase(std::remove(docks_.begin(), docks_.end(), e.window), docks_.end());
    workspaces_[current_]->Tile(GetTilingArea());
    return;
  } else if (wm_utils::IsNotification(e.window)) {
    notifications_.erase(std::remove(notifications_.begin(), notifications_.end(), e.window), notifications_.end());
    return;
  }

  // Set window state to withdrawn (wine application needs this to destroy windows properly).
  // TODO: Wine steam floating menu still laggy upon removal
  wm_utils::SetWindowWmState(e.window, WM_STATE_WITHDRAWN);

  // If we aren't managing this window, there's no need to proceed further.
  auto it = Client::mapper_.find(e.window);
  if (it == Client::mapper_.end()) {
    return;
  }

  Client* c = it->second;

  // If the client being destroyed is in fullscreen mode, make sure to unset the workspace's
  // fullscreen state.
  if (c->is_fullscreen()) {
    c->workspace()->set_fullscreen(false);
  }

  // Remove the corresponding client from the client tree.
  c->workspace()->Remove(e.window);
  UpdateClientList();
  ArrangeWindows();
}

void WindowManager::OnKeyPress(const XKeyEvent& e) {
  for (const auto& action : config_->GetKeybindActions(e.state, e.keycode)) {
    HandleAction(action);
  }
}

void WindowManager::OnButtonPress(const XButtonEvent& e) {
  auto it = Client::mapper_.find(e.subwindow);
  if (it == Client::mapper_.end()) {
    return;
  }

  Client* c = it->second;
  wm_utils::SetNetActiveWindow(c->window());
  c->workspace()->UnsetFocusedClient();
  c->workspace()->SetFocusedClient(c->window());
  c->workspace()->RaiseAllFloatingClients();
 
  if (c->is_floating() && !c->is_fullscreen()) {
    XDefineCursor(dpy_, root_window_, cursors_[e.button]);

    c->Raise();
    c->set_attr_cache(c->GetXWindowAttributes());
    btn_pressed_event_ = e;
  }
}

void WindowManager::OnButtonRelease(const XButtonEvent&) {
  auto it = Client::mapper_.find(btn_pressed_event_.subwindow);
  if (it == Client::mapper_.end()) {
    return;
  }

  Client* c = it->second;
  if (c->is_floating()) {
    XWindowAttributes attr = wm_utils::GetXWindowAttributes(btn_pressed_event_.subwindow);
    cookie_.Put(c->window(), {attr.x, attr.y, attr.width, attr.height});
  }

  btn_pressed_event_.subwindow = None;
  XDefineCursor(dpy_, root_window_, cursors_[NORMAL_CURSOR]);
}

void WindowManager::OnMotionNotify(const XButtonEvent& e) {
  auto it = Client::mapper_.find(btn_pressed_event_.subwindow);
  if (it == Client::mapper_.end()) {
    return;
  }

  Client* c = it->second;
  const XWindowAttributes& attr = c->attr_cache();
  int xdiff = e.x - btn_pressed_event_.x;
  int ydiff = e.y - btn_pressed_event_.y;
  int new_x = attr.x + ((btn_pressed_event_.button == MOUSE_LEFT_BTN) ? xdiff : 0);
  int new_y = attr.y + ((btn_pressed_event_.button == MOUSE_LEFT_BTN) ? ydiff : 0);
  int new_width = attr.width + ((btn_pressed_event_.button == MOUSE_RIGHT_BTN) ? xdiff : 0);
  int new_height = attr.height + ((btn_pressed_event_.button == MOUSE_RIGHT_BTN) ? ydiff : 0);

  int min_width = (c->size_hints().min_width > 0) ? c->size_hints().min_width : MIN_WINDOW_WIDTH;
  int min_height = (c->size_hints().min_height > 0) ? c->size_hints().min_height : MIN_WINDOW_HEIGHT;
  new_width = (new_width < min_width) ? min_width : new_width;
  new_height = (new_height < min_height) ? min_height : new_height;
  c->MoveResize(new_x, new_y, new_width, new_height);
}

void WindowManager::OnClientMessage(const XClientMessageEvent& e) {
  if (e.message_type == prop_->net[atom::NET_CURRENT_DESKTOP]) {
    if (e.data.l[0] >= 0 && e.data.l[0] < WORKSPACE_COUNT) {
      GotoWorkspace(e.data.l[0]);
    }
  } else if (e.message_type == prop_->wmderland_client_event) {
    ipc_evmgr_.Handle(e);
  }
}

// 1. Apply new border width and color to existing clients.
// 2. Re-arrange windows in current workspace.
// 3. Run all commands in config->autostart_cmds_on_reload_
void WindowManager::OnConfigReload() {
  for (const auto workspace : workspaces_) {
    for (const auto client : workspace->GetClients()) {
      client->SetBorderWidth(config_->border_width());
      client->SetBorderColor(config_->unfocused_color());
    }
  }
  ArrangeWindows();

  for (const auto& cmd : config_->autostart_cmds_on_reload()) {
    sys_utils::ExecuteCmd(cmd);
  }
}

int WindowManager::OnXError(Display*, XErrorEvent*) {
  return 0; // the error is discarded and the return value is ignored.
}

int WindowManager::OnWmDetected(Display*, XErrorEvent*) {
  is_running_ = false;
  return 0; // the return value is ignored.
}


void WindowManager::HandleAction(const Action& action) {
  Client* focused_client = workspaces_[current_]->GetFocusedClient();

  switch (action.type()) {
    case Action::Type::NAVIGATE_LEFT:
    case Action::Type::NAVIGATE_RIGHT:
    case Action::Type::NAVIGATE_UP:
    case Action::Type::NAVIGATE_DOWN:
      workspaces_[current_]->Navigate(action.type());
      break;
    case Action::Type::TILE_H:
      workspaces_[current_]->SetTilingDirection(TilingDirection::HORIZONTAL);
      break;
    case Action::Type::TILE_V:
      workspaces_[current_]->SetTilingDirection(TilingDirection::VERTICAL);
      break;
    case Action::Type::TOGGLE_FLOATING:
      if (!focused_client) return;
      SetFloating(focused_client->window(), !focused_client->is_floating(),
                  /*use_default_size=*/true);
      break;
    case Action::Type::TOGGLE_FULLSCREEN:
      if (!focused_client) return;
      SetFullscreen(focused_client->window(), !focused_client->is_fullscreen());
      break;
    case Action::Type::GOTO_WORKSPACE:
      GotoWorkspace(std::stoi(action.argument()) - 1);
      break;
    case Action::Type::MOVE_WINDOW_TO_WORKSPACE:
      if (!focused_client) return;
      MoveWindowToWorkspace(focused_client->window(), std::stoi(action.argument()) - 1);
      break;
    case Action::Type::KILL:
      if (!focused_client) return;
      KillClient(focused_client->window());
      break;
    case Action::Type::EXIT:
      is_running_ = false;
      break;
    case Action::Type::RELOAD:
      sys_utils::NotifySend("Reloading config...");
      config_->Load();
      OnConfigReload();
      break;
    case Action::Type::DEBUG_CRASH:
      WM_LOG(INFO, "Debug crash on demand.");
      throw std::runtime_error("Debug crash");
      break;
    case Action::Type::EXEC:
      sys_utils::ExecuteCmd(action.argument());
      break;
    default:
      break;
  }
}

void WindowManager::GotoWorkspace(int next) {
  if (current_ == next || next < 0 || next >= (int) workspaces_.size()) {
    return;
  }

  workspaces_[current_]->UnmapAllClients();
  workspaces_[next]->MapAllClients();
  current_ = next;
  ArrangeWindows();

  // Update _NET_CURRENT_DESKTOP
  XChangeProperty(dpy_, root_window_, prop_->net[atom::NET_CURRENT_DESKTOP], XA_CARDINAL,
      32, PropModeReplace, reinterpret_cast<unsigned char*>(&next), 1);
}

void WindowManager::MoveWindowToWorkspace(Window window, int next) {    
  auto it = Client::mapper_.find(window);
  if (current_ == next || it == Client::mapper_.end() ||
      next < 0 || next >= (int) workspaces_.size()) {
    return;
  }

  Client* c = it->second;
  if (workspaces_[current_]->is_fullscreen()) {
    SetFullscreen(c->window(), false);
  }

  c->Unmap();
  workspaces_[next]->UnsetFocusedClient();
  workspaces_[current_]->Move(window, workspaces_[next]);
  ArrangeWindows();
}


void WindowManager::SetFloating(Window window, bool floating, bool use_default_size) {
  auto it = Client::mapper_.find(window);
  if (it == Client::mapper_.end()) {
    return;
  }

  Client* c = it->second;
  if (c->is_fullscreen()) {
    return;
  }

  if (floating) {
    Client::Area area = GetFloatingWindowArea(window, use_default_size);
    c->MoveResize(area.x, area.y, area.w, area.h);
  }

  c->set_floating(floating);
  ArrangeWindows(); // floating windows won't be tiled
}

void WindowManager::SetFullscreen(Window window, bool fullscreen) {
  auto it = Client::mapper_.find(window);
  if (it == Client::mapper_.end()) {
    return;
  }

  Client* c = it->second;
  if (c->is_fullscreen() == fullscreen) {
    return;
  }

  c->set_fullscreen(fullscreen);
  c->workspace()->set_fullscreen(fullscreen);
  c->SetBorderWidth((fullscreen) ? 0 : config_->border_width());

  if (fullscreen) {
    UnmapDocks();
    c->set_attr_cache(c->GetXWindowAttributes());
    c->MoveResize(0, 0, GetDisplayResolution());
    c->workspace()->UnmapAllClients();
    c->Map();
    c->workspace()->SetFocusedClient(c->window());
  } else {
    MapDocks();
    const XWindowAttributes& attr = c->attr_cache();
    c->MoveResize(attr.x, attr.y, attr.width, attr.height);
    ArrangeWindows();
  }
 
  // Update window's _NET_WM_STATE_FULLSCREEN property.
  // If the window is set to be NOT fullscreen, we will simply write a nullptr with 0 elements.
  Atom* atom = (fullscreen) ? &prop_->net[atom::NET_WM_STATE_FULLSCREEN] : nullptr;
  XChangeProperty(dpy_, window, prop_->net[atom::NET_WM_STATE], XA_ATOM,
      32, PropModeReplace, reinterpret_cast<unsigned char*>(atom), fullscreen);
}

void WindowManager::KillClient(Window window) {
  Atom* supported_protocols = nullptr;
  int num_supported_protocols = 0;

  // First try to kill the client gracefully via ICCCM. If the client does not support
  // this method, then we'll perform the brutal XKillClient().
  if (XGetWMProtocols(dpy_, window, &supported_protocols, &num_supported_protocols) 
      && (std::find(supported_protocols, supported_protocols + num_supported_protocols, 
          prop_->wm[atom::WM_DELETE]) != supported_protocols + num_supported_protocols)) {
    XEvent msg;
    memset(&msg, 0, sizeof(msg));
    msg.xclient.type = ClientMessage;
    msg.xclient.message_type = prop_->wm[atom::WM_PROTOCOLS];
    msg.xclient.window = window;
    msg.xclient.format = 32;
    msg.xclient.data.l[0] = prop_->wm[atom::WM_DELETE];
    XSendEvent(dpy_, window, false, 0, &msg);
  } else {
    XKillClient(dpy_, window);
  }
}


inline void WindowManager::MapDocks() const {
  for (const auto window : docks_) {
    XMapWindow(dpy_, window);
  }
}

inline void WindowManager::UnmapDocks() const {
  for (const auto window : docks_) {
    XUnmapWindow(dpy_, window);
  }
}

inline void WindowManager::RaiseNotifications() const {
  for (const auto window : notifications_) {
    XRaiseWindow(dpy_, window);
  }
}


pair<int, int> WindowManager::GetDisplayResolution() const {
  XWindowAttributes root_window_attr = wm_utils::GetXWindowAttributes(root_window_);
  return {root_window_attr.width, root_window_attr.height};
}

Client::Area WindowManager::GetTilingArea() const {
  pair<int, int> res = GetDisplayResolution();
  Client::Area tiling_area = {0, 0, res.first, res.second};

  for (const auto window : docks_) {
    XWindowAttributes dock_attr = wm_utils::GetXWindowAttributes(window);

    if (dock_attr.y == 0) {
      // If the dock is at the top of the screen.
      tiling_area.y += dock_attr.height;
      tiling_area.h -= dock_attr.height;
    } else if (dock_attr.y + dock_attr.height == tiling_area.y + tiling_area.h) {
      // If the dock is at the bottom of the screen.
      tiling_area.h -= dock_attr.height;
    } else if (dock_attr.x == 0) {
      // If the dock is at the leftmost of the screen.
      tiling_area.x += dock_attr.width;
      tiling_area.w -= dock_attr.width;
    } else if (dock_attr.x + dock_attr.width == tiling_area.x + tiling_area.w) {
      // If the dock is at the rightmost of the screen.
      tiling_area.w -= dock_attr.width;
    }
  }

  return tiling_area;
}

Client::Area WindowManager::GetFloatingWindowArea(Window window, bool use_default_size) {
  Client::Area area;

  auto it = Client::mapper_.find(window);
  if (it == Client::mapper_.end()) {
    return area;
  }

  if (use_default_size) {
    pair<int, int> res = GetDisplayResolution();
    area.w = DEFAULT_FLOATING_WINDOW_WIDTH;
    area.h = DEFAULT_FLOATING_WINDOW_HEIGHT;
    area.x = res.first / 2 - area.w / 2;
    area.y = res.second / 2 - area.h / 2;
    return area;
  }


  // If not using default floating window size, then do the following.
  Client::Area cookie_area = cookie_.Get(window);
  XSizeHints hints = wm_utils::GetWmNormalHints(window);

  // Determine floating window's x and y.
  if (cookie_area.x > 0 && cookie_area.h > 0) {
    area.x = cookie_area.x;
    area.y = cookie_area.y;
  } else if (hints.x > 0 && hints.y > 0) {
    area.x = hints.x;
    area.y = hints.y;
  } else {
    pair<int, int> res = GetDisplayResolution();
    XWindowAttributes attr = wm_utils::GetXWindowAttributes(window);
    area.x = res.first / 2 - attr.width / 2;
    area.y = res.second / 2 - attr.height / 2;
  }
 
  // Determine floating window's w and h.
  if (cookie_area.w > 0 && cookie_area.h > 0) { // has entry in cookie
    area.w = cookie_area.w;
    area.h = cookie_area.h;
  } else if (hints.flags & PSize) { // program specified size
    area.w = hints.width;
    area.h = hints.height;
  } else if (hints.flags & PMinSize) { // program specified min size
    area.w = hints.min_width;
    area.h = hints.min_height;
  } else if (hints.flags & PBaseSize) { // program specified base size
    area.w = hints.base_width;
    area.h = hints.base_height;
  } else {
    area.w = DEFAULT_FLOATING_WINDOW_WIDTH;
    area.h = DEFAULT_FLOATING_WINDOW_HEIGHT;
  }

  return area;
}


void WindowManager::UpdateClientList() {
  XDeleteProperty(dpy_, root_window_, prop_->net[atom::NET_CLIENT_LIST]);

  for (const auto workspace : workspaces_) {
    for (const auto client : workspace->GetClients()) {
      Window window = client->window();
      XChangeProperty(dpy_, root_window_, prop_->net[atom::NET_CLIENT_LIST], XA_WINDOW,
          32, PropModeAppend, reinterpret_cast<unsigned char*>(&window), 1);
    }
  }
}


Snapshot& WindowManager::snapshot() {
  return snapshot_;
}

} // namespace wmderland
