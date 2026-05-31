#pragma once

void initialize_console(void);
void ask_reboot(void);

// web proxy
bool console_web_proxy_upload_intercept(const char *line);
void register_file_commands(void);
void register_ws_proxy(void);