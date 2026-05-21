#ifndef EMBEDDED_REACT_ER_SCENE_H
#define EMBEDDED_REACT_ER_SCENE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ERNode ERNode;

typedef enum {
    ER_NODE_VIEW = 0,
    ER_NODE_TEXT,
    ER_NODE_IMAGE,
    ER_NODE_SCROLL_VIEW,
    ER_NODE_FLAT_LIST,
    ER_NODE_PRESSABLE,
    ER_NODE_TEXT_INPUT,
    ER_NODE_ACTIVITY_INDICATOR,
    ER_NODE_SWITCH,
    ER_NODE_MODAL
} ERNodeType;

typedef enum {
    ER_PROP_OPACITY = 0,
    ER_PROP_TRANSLATE_X,
    ER_PROP_TRANSLATE_Y,
    ER_PROP_SCALE_X,
    ER_PROP_SCALE_Y,
    ER_PROP_ROTATE_Z,
    ER_PROP_BACKGROUND_COLOR,
    ER_PROP_COLOR
} ERAnimProp;

typedef enum {
    ER_EVENT_PRESS = 0,
    ER_EVENT_LONG_PRESS,
    ER_EVENT_PRESS_IN,
    ER_EVENT_PRESS_OUT,
    ER_EVENT_TOUCH_START,
    ER_EVENT_TOUCH_MOVE,
    ER_EVENT_TOUCH_END,
    ER_EVENT_SCROLL,
    ER_EVENT_LAYOUT
} EREventType;

typedef struct ERProps      ERProps;
typedef struct ERAnimConfig ERAnimConfig;
typedef struct EREventData  EREventData;

typedef void (*EREventFn)(ERNode *node, const EREventData *data, void *user_data);

ERNode *er_node_create(ERNodeType type);
void    er_node_destroy(ERNode *node);
void    er_node_set_props(ERNode *node, const ERProps *props);

void er_tree_append_child(ERNode *parent, ERNode *child);
void er_tree_remove_child(ERNode *parent, ERNode *child);
void er_tree_set_root(ERNode *root);

void er_commit(void);

uint32_t er_now_ms(void);
void     er_font_load(const char *name, const void *ttf_buf, size_t len);
void     er_image_load(const char *name, const void *argb_buf, int w, int h);

void er_anim_start(ERNode *node, ERAnimProp prop, float value, const ERAnimConfig *cfg);
void er_anim_cancel(ERNode *node, ERAnimProp prop);

void er_event_set(ERNode *node, EREventType event, EREventFn fn, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
