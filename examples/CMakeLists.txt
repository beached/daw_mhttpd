# Copyright (c) Darrell Wright
#
# Distributed under the GNU Lesser General Public License, Version 2.1. (See
# accompanying file LICENSE)
#
# Official repository: https://github.com/beached/cpp_microhttpd
#

foreach( example IN ITEMS hello_world hello_world_async routing static_file_server web_app )
	add_executable( ${example} src/${example}.cpp )
	target_link_libraries( ${example} PRIVATE daw::daw-mhttpd )
	target_compile_options( ${example} PRIVATE -Wall -Wextra )
endforeach()

# The sample site web_app serves when no root is given
target_compile_definitions( web_app PRIVATE DAW_MHTTPD_EXAMPLE_WWW="${CMAKE_CURRENT_SOURCE_DIR}/www" )
