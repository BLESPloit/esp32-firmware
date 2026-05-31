#pragma once
#include <lua.h>
#include <lualib.h>


#define LUA_TASK_STACK_SIZE (8192)
//#define LUA_TASK_STACK_SIZE (4096)
#define LUA_TASK_PRIORITY (5)
#define LUA_QUEUE_SIZE (10)

// Event types
typedef enum {
    LUA_EVENT_CALL_HANDLER,      // Call handler with string parameter
    LUA_EVENT_CALL_FUNCTION,     // Call function with binary data
    LUA_EVENT_DELAYED_CALL,      // Call function after a predefined delay
} lua_event_type_t;

// Event structure
typedef struct {
    lua_event_type_t type;
    char func_name[32];
    uint8_t data[256];           // Input data
    size_t data_len;
    uint8_t *output;             // Output buffer (if needed)
    size_t *output_len;
    size_t max_len;
    SemaphoreHandle_t done_sem;  // Signal completion
    esp_err_t *result;           // Store result
} lua_event_t;


bool lua_is_task_running(void);
lua_State *lua_get_state(void);

// Initialize Lua with minimal libraries and dedicated task
esp_err_t lua_init_persistent_minimal(const char *script_path, bool central);

// Asynchronous call (non-blocking, for handlers like on_startup, on_action)
esp_err_t lua_call_handler_async(const char *func_name, const char *param);

// Synchronous call (blocking, for functions with return values)
esp_err_t lua_call_stateful(const char *func_name, 
                            const uint8_t *input, 
                            size_t input_len,
                            uint8_t *output, 
                            size_t *output_len, 
                            size_t max_len);

// Cleanup
void lua_cleanup(void);

// Debug: print stack usage
void lua_print_stack_usage(void);


/**
 * Parsed view of a lua field string.
 * Both pointers refer into the caller-supplied 'buf' — no extra allocation.
 *
 * func  → always valid after a successful parse
 * args  → NULL when no parentheses present (zero-arg call)
 */
typedef struct {
    const char *func;   // bare function name
    const char *args;   // argument string, or NULL if no parentheses
} lua_call_t;

/**
 * Parse src into buf (which must be at least strlen(src)+1 bytes),
 * then fill out->func and out->args with pointers into buf.
 *
 * The total usable space is always len(src): if the function name is
 * long there is simply less room for args, and vice-versa.
 *
 * Examples (src field is char[64]):
 *   "turn_off"              → func="turn_off"   args=NULL
 *   "set_color(AABBCC)"     → func="set_color"  args="AABBCC"
 *   "set_mode(off,fast)"    → func="set_mode"   args="off,fast"
 */
bool lua_call_parse(const char *src, char *buf, size_t buf_size, lua_call_t *out);

// relies on strlen
esp_err_t lua_call_from_field(const char *src);

void lua_vars_set_path(const char *path);
void lua_vars_inject(const char *vars_path);
void lua_uuids_inject(const char *path);