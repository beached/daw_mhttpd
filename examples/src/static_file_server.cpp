// Serves a folder over HTTP until Ctrl-C.
//
//   static_file_server [root=.] [port=8080]
#include "static_files.h"

#include <daw/mhttpd/daw_mhttpd.h>

#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <print>

int main( int argc, char **argv ) {
	auto const *root = argc > 1 ? argv[1] : ".";
	auto port = std::uint16_t{ 8080 };
	if( argc > 2 ) {
		auto const *last = argv[2] + std::strlen( argv[2] );
		if( auto [p, ec] = std::from_chars( argv[2], last, port );
		    ec != std::errc{ } or p != last ) {
			std::println( stderr, "invalid port: {}", argv[2] );
			return EXIT_FAILURE;
		}
	}

	try {
		auto files = static_files::static_file_handler{ root };
		auto d = daw::mhttpd::daemon{ { .port = port }, files };
		std::println( "Serving {} on http://localhost:{}/",
		              files.root( ).string( ),
		              d.port( ) );

		d.wait_for_signals( { SIGINT, SIGTERM } );
	} catch( std::exception const &e ) {
		std::println( stderr, "error: {}", e.what( ) );
		return EXIT_FAILURE;
	}
}
