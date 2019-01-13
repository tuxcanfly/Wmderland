#ifndef WMDERLAND_TREE_HPP_
#define WMDERLAND_TREE_HPP_

#include <iostream>
#include <vector>
#include "tree_node.hpp"

class Client;

class Tree {
public:
    Tree();
    ~Tree();

    TreeNode* GetTreeNode(Client* client) const;
    std::vector<TreeNode*> GetAllLeaves() const;

    TreeNode* root() const;
    TreeNode* current() const;
    void set_root(TreeNode* root);
    void set_current(TreeNode* current);

    friend std::ostream& operator<< (std::ostream& os, const Tree& t);

private:
    TreeNode* root_;
    TreeNode* current_;
};

#endif
