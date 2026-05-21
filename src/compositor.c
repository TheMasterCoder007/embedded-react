#include "er_scene.h"

ERNode *er_node_create(ERNodeType type) {
    (void)type;
    return NULL;
}

void er_node_destroy(ERNode *node) {
    (void)node;
}

void er_node_set_props(ERNode *node, const ERProps *props) {
    (void)node; (void)props;
}

void er_tree_append_child(ERNode *parent, ERNode *child) {
    (void)parent; (void)child;
}

void er_tree_remove_child(ERNode *parent, ERNode *child) {
    (void)parent; (void)child;
}

void er_tree_set_root(ERNode *root) {
    (void)root;
}

void er_commit(void) {
}

uint32_t er_now_ms(void) {
    return 0;
}
