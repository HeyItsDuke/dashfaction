#pragma once

#include <cstddef>
#include <string>
#include "../rf/multi.h"

void misc_init();
void set_jump_to_multi_server_list(bool jump);
void start_join_multi_game_sequence(const rf::NetAddr& addr, const std::string& password);
bool multi_join_game(const rf::NetAddr& addr, const std::string& password);
void g_solid_render_ui();
