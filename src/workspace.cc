// Copyright (c) 2018-2019 Marco Wang <m.aesophor@gmail.com>
#include "workspace.h"

#include <algorithm>
#include <stack>

#include "client.h"
#include "window_manager.h"
#include "util.h"

using std::stack;
using std::vector;
using std::string;

namespace wmderland {

class WindowManager;

Workspace::Workspace(Display* dpy, Window root_window, Config* config, int id)
    : dpy_(dpy),
      root_window_(root_window),
      config_(config),
      client_tree_(),
      id_(id),
      name_(std::to_string(id)),
      is_fullscreen_() {}


bool Workspace::Has(Window window) const {
  return GetClient(window) != nullptr;
}

void Workspace::Add(Window window) {
  Client* new_client = new Client(dpy_, window, /*workspace=*/this);
  Tree::Node* new_node = new Tree::Node(new_client);

  // If there are no windows at all, then add this new node as the root's child.
  // Otherwise, add this new node as current node's sibling.
  if (!client_tree_.current_node()) {
    client_tree_.root_node()->AddChild(new_node);
  } else {
    Tree::Node* current_node = client_tree_.current_node();
    current_node->parent()->InsertChildAfter(new_node, current_node);
  }

  // Don't transfer current node if this workspace has a window in fullscreen!
  if (!is_fullscreen_) {
    client_tree_.set_current_node(new_node);
  }
}

void Workspace::Remove(Window window) {
  Client* c = GetClient(window);
  if (!c) {
    return;
  }

  Tree::Node* node = client_tree_.GetTreeNode(c);
  if (!node) {
    return;
  }

  // Get leaves and find the index of the node we're going to remove.
  vector<Tree::Node*> nodes = client_tree_.GetLeaves();
  ptrdiff_t idx = find(nodes.begin(), nodes.end(), node) - nodes.begin();

  // Remove this node from its parent.
  Tree::Node* parent_node = node->parent();
  parent_node->RemoveChild(node);
  delete node;
  delete c;

  // If its parent has no children left, then remove parent from its grandparent 
  // (If this parent is not the root).
  while (parent_node->children().empty() && parent_node != client_tree_.root_node()) {
    Tree::Node* grandparent_node = parent_node->parent();
    grandparent_node->RemoveChild(parent_node);
    delete parent_node;
    parent_node = grandparent_node;
  }

  // Decide which node shall be set as the new current Tree::Node. If there are no
  // windows left, set current to nullptr.
  nodes.erase(std::remove(nodes.begin(), nodes.end(), node), nodes.end());

  if (nodes.empty()) {
    client_tree_.set_current_node(nullptr);
    return;
  }

  // If idx is out of bound, decrement it by one.
  if (idx >= static_cast<ptrdiff_t>(nodes.size())) {
    idx--;
  }
  client_tree_.set_current_node(nodes[idx]);
}

void Workspace::Move(Window window, Workspace* new_workspace) {
  Client* c = GetClient(window);
  if (!c) {
    return;
  }

  bool is_floating = c->is_floating();
  bool has_unmap_req_from_wm = c->has_unmap_req_from_wm();

  // This will delete current Client* c, and erase it from Client::mapper_
  this->Remove(window);

  // This will allocate a new Client* c', and insert it into Client::mapper_
  new_workspace->Add(window);

  // Transfer old client's state to the new client.
  c = new_workspace->GetClient(window);
  c->set_floating(is_floating);
  c->set_fullscreen(false);
  c->set_has_unmap_req_from_wm(has_unmap_req_from_wm);
}

void Workspace::Tile(const Client::Area& tiling_area) const {
  // If there are no clients in this workspace or all clients are floating, return at once.
  if (!client_tree_.current_node() || GetTilingClients().empty()) {
    return;
  }

  int border_width = config_->border_width();
  int gap_width = config_->gap_width();

  int x = tiling_area.x + gap_width / 2;
  int y = tiling_area.y + gap_width / 2;
  int w = tiling_area.w - gap_width;
  int h = tiling_area.h - gap_width;

  DfsTileHelper(client_tree_.root_node(), x, y, w, h, border_width, gap_width);
}

void Workspace::DfsTileHelper(Tree::Node* node, int x, int y, int w, int h, 
                              int border_width, int gap_width) const {
  vector<Tree::Node*> children = node->children(); // pass by value here

  // We don't care about floating windows. Remove them.
  children.erase(std::remove_if(children.begin(), children.end(), [](Tree::Node* n) {
      return n->client() && n->client()->is_floating(); }), children.end());

  // Calculate each child's x, y, width and height based on node's tiling direction.
  TilingDirection dir = node->tiling_direction();
  int child_x = x;
  int child_y = y;
  int child_width = (dir == TilingDirection::HORIZONTAL) ? w / children.size() : w;
  int child_height = (dir == TilingDirection::VERTICAL) ? h / children.size() : h;

  for (size_t i = 0; i < children.size(); i++) {
    Tree::Node* child = children[i];
    if (node->tiling_direction() == TilingDirection::HORIZONTAL) child_x = x + child_width * i;
    if (node->tiling_direction() == TilingDirection::VERTICAL) child_y = y + child_height * i;

    if (child->leaf()) {
      int new_x = child_x + gap_width / 2;
      int new_y = child_y + gap_width / 2;
      int new_width = child_width - border_width * 2 - gap_width;
      int new_height = child_height - border_width * 2 - gap_width;
      child->client()->MoveResize(new_x, new_y, new_width, new_height);
    } else {
      DfsTileHelper(child, child_x, child_y, child_width, child_height, border_width, gap_width);
    }
  }
}

void Workspace::SetTilingDirection(TilingDirection tiling_direction) {
  if (!client_tree_.current_node()) {
    client_tree_.root_node()->set_tiling_direction(tiling_direction);
    return;
  }

  Tree::Node* current_node = client_tree_.current_node();
  Client* current_node_client = current_node->client();

  // If the user has specified a tiling direction on current node, then
  // 1. Let current node become an internal node.
  //    i.  set tiling direction
  //    ii. set client = nullptr
  // 2. Add the original current node as this internal node's child.
  // 3. Set the first child of this internal node as the new current node.
  current_node->set_tiling_direction(tiling_direction);
  current_node->set_client(nullptr);

  current_node->AddChild(new Tree::Node(current_node_client));
  client_tree_.set_current_node(current_node->children().front());
}


void Workspace::MapAllClients() const {
  for (const auto leaf : client_tree_.GetLeaves()) {
    if (leaf != client_tree_.root_node()) {
      leaf->client()->Map();
    }
  }
}

void Workspace::UnmapAllClients() const {
  for (auto leaf : client_tree_.GetLeaves()) {
    if (leaf != client_tree_.root_node()) {
      leaf->client()->Unmap();
    }
  }
}

void Workspace::RaiseAllFloatingClients() const {
  for (auto c : GetFloatingClients()) {
    c->Raise();
  }
}

void Workspace::SetFocusedClient(Window window) {
  Client* c = GetClient(window);
  if (!c) {
    return;
  }

  // Raise the window to the top and set input focus to it.
  c->Raise();
  c->SetInputFocus();
  c->SetBorderColor(config_->focused_color());
  client_tree_.set_current_node(client_tree_.GetTreeNode(c));
}

void Workspace::UnsetFocusedClient() const {
  if (!client_tree_.current_node()) {
    return;
  }

  Client* c = client_tree_.current_node()->client();
  if (c) {
    c->SetBorderColor(config_->unfocused_color());
  }
}


Client* Workspace::GetFocusedClient() const {
  if (!client_tree_.current_node()) {
    return nullptr;
  }
  return client_tree_.current_node()->client();
}

Client* Workspace::GetClient(Window window) const {
  // We'll get the corresponding client using client mapper
  // whose time complexity is O(1).
  auto it = Client::mapper_.find(window);
  if (it == Client::mapper_.end()) {
    return nullptr;
  }

  // But we have to check if it belongs to current workspace!
  Client* c = it->second;
  return (c->workspace() == this) ? c : nullptr;
}

vector<Client*> Workspace::GetClients() const {
  vector<Client*> clients;

  for (auto leaf : client_tree_.GetLeaves()) {
    if (leaf != client_tree_.root_node()) {
      clients.push_back(leaf->client());
    }
  }
  return clients;
}

vector<Client*> Workspace::GetFloatingClients() const {
  vector<Client*> clients = GetClients();
  clients.erase(std::remove_if(clients.begin(), clients.end(), [](Client* c) {
      return !c->is_floating(); }), clients.end());
  return clients;
}

vector<Client*> Workspace::GetTilingClients() const {
  vector<Client*> clients = GetClients();
  clients.erase(std::remove_if(clients.begin(), clients.end(), [](Client* c) {
      return c->is_floating(); }), clients.end());
  return clients;
}


void Workspace::Navigate(Action::Type focus_action_type) {
  // Do not let user navigate between windows if
  // 1. there's no currently focused client
  // 2. current workspace is in fullscreen mode (there's a fullscreen window)
  if (!client_tree_.current_node() || this->is_fullscreen()) {
    return;
  }

  TilingDirection target_direction = TilingDirection::HORIZONTAL;
  bool find_leftward = true;

  switch (focus_action_type) {
    case Action::Type::NAVIGATE_LEFT:
      target_direction = TilingDirection::HORIZONTAL;
      find_leftward = true;
      break;
    case Action::Type::NAVIGATE_RIGHT:
      target_direction = TilingDirection::HORIZONTAL;
      find_leftward = false;
      break;
    case Action::Type::NAVIGATE_UP:
      target_direction = TilingDirection::VERTICAL;
      find_leftward = true;
      break;
    case Action::Type::NAVIGATE_DOWN:
      target_direction = TilingDirection::VERTICAL;
      find_leftward = false;
      break;
    default:
      return;
  }

 
  for (Tree::Node* node = client_tree_.current_node(); node; node = node->parent()) {
    Tree::Node* sibling = (find_leftward) ? node->GetLeftSibling() : node->GetRightSibling();

    if (node->parent()->tiling_direction() == target_direction && sibling) {
      node = sibling;
      while (!node->leaf()) {
        node = (find_leftward) ? node->children().back() : node->children().front();
      }
      UnsetFocusedClient();
      SetFocusedClient(node->client()->window());
      client_tree_.set_current_node(node);
      WindowManager::GetInstance()->ArrangeWindows();
      return;
    } else if (node->parent() == client_tree_.root_node()) {
      return;
    }
  }
}


Config* Workspace::config() const {
  return config_;
}

int Workspace::id() const {
  return id_;
}

const char* Workspace::name() const {
  return name_.c_str();
}

bool Workspace::is_fullscreen() const {
  return is_fullscreen_;
}


void Workspace::set_name(const string& name) {
  name_ = name;
}

void Workspace::set_fullscreen(bool fullscreen) {
  is_fullscreen_ = fullscreen;
}


string Workspace::Serialize() const {
  return client_tree_.Serialize();
}

void Workspace::Deserialize(string data) {
  client_tree_.Deserialize(data);
}

} // namespace wmderland
