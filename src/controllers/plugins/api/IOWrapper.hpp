// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once
#ifdef CHATTERINO_HAVE_PLUGINS
#    include <QString>
#    include <sol/types.hpp>
#    include <sol/variadic_args.hpp>
#    include <sol/variadic_results.hpp>

struct lua_State;

namespace chatterino::lua::api {

const char *const REG_REAL_IO_NAME = "real_lua_io_lib";

sol::variadic_results io_open(sol::this_state L, QString filename,
                              QString strmode);
sol::variadic_results io_open_modeless(sol::this_state L, QString filename);

sol::variadic_results io_lines(sol::this_state L, QString filename,
                               sol::variadic_args args);
sol::variadic_results io_lines_noargs(sol::this_state L);

sol::variadic_results io_input_argless(sol::this_state L);
sol::variadic_results io_input_file(sol::this_state L, sol::userdata file);
sol::variadic_results io_input_name(sol::this_state L, QString filename);

sol::variadic_results io_output_argless(sol::this_state L);
sol::variadic_results io_output_file(sol::this_state L, sol::userdata file);
sol::variadic_results io_output_name(sol::this_state L, QString filename);

bool io_close_argless(sol::this_state L);
bool io_close_file(sol::this_state L, sol::userdata file);

void io_flush_argless(sol::this_state L);
void io_flush_file(sol::this_state L, sol::userdata file);

sol::variadic_results io_read(sol::this_state L, sol::variadic_args args);

sol::variadic_results io_write(sol::this_state L, sol::variadic_args args);

void io_popen();
void io_tmpfile();

}  // namespace chatterino::lua::api
#endif
