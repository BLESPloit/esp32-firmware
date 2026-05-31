#pragma once

// Button structure definition 
typedef struct {
    char id[32];
    char name[64];
    int physical_button_id;      // -1 if not present
    char physical_button_press[16]; // "single", "double", "long", etc.
    char lua[64];                // Lua callback function name
} interface_button_t;

typedef struct {
    interface_button_t *buttons;
    int button_count;
} interface_manager_t;

bool interface_init(const char *filepath);
void interface_cleanup(void);
interface_manager_t* parse_interface_json_file(const char *filepath);
void interface_call_lua(const char* element_id);
void interface_handle_physical_button(uint8_t physical_button_id, const char *press_type);