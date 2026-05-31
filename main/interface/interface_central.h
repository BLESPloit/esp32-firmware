#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- Tunable limits ----
#define CENTRAL_MENU_MAX_ITEMS      8
#define CENTRAL_MENU_STACK_DEPTH    4
#define CENTRAL_MENU_LABEL_LEN     24
#define CENTRAL_MENU_ID_LEN        24
#define CENTRAL_MENU_LUA_LEN       48

#define CENTRAL_STATE_KEY_LEN   10
#define CENTRAL_STATE_VAL_LEN   16
#define CENTRAL_STATE_MAX_ENTRIES  10

typedef struct {
    char id[CENTRAL_MENU_ID_LEN];
    char label[CENTRAL_MENU_LABEL_LEN];
} central_menu_item_t;

typedef struct {
    char id[CENTRAL_MENU_ID_LEN];
    char title[CENTRAL_MENU_LABEL_LEN];
    char on_enter[CENTRAL_MENU_LUA_LEN];
    central_menu_item_t *items;   // heap-allocated, owned by this struct
    int  item_count;
} central_menu_node_t;

typedef struct {
    central_menu_node_t *nodes;
    int  node_count;
    char initial_menu_id[CENTRAL_MENU_ID_LEN];
} central_menu_def_t;

typedef struct {
    char  stack[CENTRAL_MENU_STACK_DEPTH][CENTRAL_MENU_ID_LEN];
    int   stack_depth;

    const central_menu_item_t *current_items;  // ptr into current node
    int   current_item_count;

    char  current_title[CENTRAL_MENU_LABEL_LEN];
    int   selected_index;
} central_menu_state_t;

// ---- Lifecycle ----
bool interface_central_init(const char *filepath);
void interface_central_cleanup(void);

// ---- Physical button input ----
void interface_central_input_next(void);
void interface_central_input_back(void);
void interface_central_input_select(void);

// ---- Web input ----
void interface_central_web_select(const char *item_id);

// ---- Title / info helpers ----
// Replaces the base title of the current node
void interface_central_set_title(const char *title);

void interface_central_push_menu(const char *id);
void interface_central_pop_menu(void);


// STATES
// Set or update a state value. value=NULL clears it.
void interface_central_state_set(const char *key, const char *value);
// Returns current value string, or NULL if not set.
const char *interface_central_state_get(const char *key);
// Clear all state slots (e.g. on disconnect).
void interface_central_state_clear_all(void);