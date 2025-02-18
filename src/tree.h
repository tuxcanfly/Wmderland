// Copyright (c) 2018-2019 Marco Wang <m.aesophor@gmail.com>
#ifndef WMDERLAND_TREE_H_
#define WMDERLAND_TREE_H_

#include <vector>
#include <unordered_map>

namespace wmderland {

class Client;

enum class TilingDirection {
  UNSPECIFIED,
  HORIZONTAL,
  VERTICAL
};

class Tree {
 public:
  Tree();
  virtual ~Tree();

  class Node {
   public:
    Node(Client* client);
    virtual ~Node();
  
    void AddChild(Tree::Node* child);
    void RemoveChild(Tree::Node* child);
    void InsertChildAfter(Tree::Node* child, Tree::Node* ref);

    Tree::Node* GetLeftSibling() const;
    Tree::Node* GetRightSibling() const;

    const std::vector<Tree::Node*>& children() const;
    Tree::Node* parent() const;
    Client* client() const;
    TilingDirection tiling_direction() const;
    bool leaf() const;

    void set_parent(Tree::Node* parent);
    void set_client(Client* client);
    void set_tiling_direction(TilingDirection tiling_direction);

    static std::unordered_map<Client*, Tree::Node*> mapper_;

   private:
    std::vector<Tree::Node*> children_;
    Tree::Node* parent_;

    Client* client_;
    TilingDirection tiling_direction_;
  };


  Tree::Node* GetTreeNode(Client* client) const;
  std::vector<Tree::Node*> GetLeaves() const;

  Tree::Node* root_node() const;
  Tree::Node* current_node() const;
  void set_current_node(Tree::Node* node);

  std::string Serialize() const;
  void Deserialize(std::string data);

 private:
  void DfsCleanUpHelper(Tree::Node* node) const;
  void DfsSerializeHelper(Tree::Node* node, std::string& data) const;

  Tree::Node* root_node_;
  Tree::Node* current_node_;
};

} // namespace wmderland

#endif // WMDERLAND_TREE_H_
