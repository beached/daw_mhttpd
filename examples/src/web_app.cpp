// A single-page app server until Ctrl-C. Each request falls through three
// layers:
//
//   1. API routes:    GET /api/time, POST /api/echo
//   2. Static files:  anything else is looked up in the web root
//   3. Fallback:      a page path with no file, such as /settings, gets the
//                     app's index.html so client-side routing can handle it
//
// Unknown /api/ paths and missing files with an extension (/missing.png)
// stay 404, so mistakes are not hidden behind the app's HTML.
//
//   web_app [root=examples/www] [port=8080]
#include "static_files.h"

#include <daw/mhttpd/daw_mhttpd.h>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace fs = std::filesystem;
namespace mhttpd = daw::mhttpd;

namespace {
	[[nodiscard]] mhttpd::response text( std::string body, unsigned status = 200 ) {
		return mhttpd::response::from_owned( std::move( body ), status )
		  .header( "Content-Type", "text/plain; charset=utf-8" );
	}

	// A path whose last segment has no '.', e.g. /settings or /users/42, is a
	// page of the app rather than a file
	[[nodiscard]] bool is_page_path( daw::string_view url ) {
		auto const last = url.substr( url.rfind( '/' ) + 1 );
		return last.find( '.' ) == daw::string_view::npos;
	}

	// The fallback: the app's index.html for page paths the server has no file
	// for. Sent with sendfile, like the static files.
	class app_fallback {
	public:
		explicit app_fallback( fs::path const &root )
		  : m_index{ root / "index.html" } {}

		[[nodiscard]] mhttpd::response serve( ) const {
			auto fd = mhttpd::unique_fd{ ::open( m_index.c_str( ), O_RDONLY | O_CLOEXEC ) };
			struct stat st{ };
			if( not fd or ::fstat( fd.get( ), &st ) != 0 ) {
				return text( "Not Found\n", 404 );
			}
			return mhttpd::response::from_fd(
			         std::move( fd ), 0U, static_cast<std::uint64_t>( st.st_size ) )
			  .header( "Content-Type", "text/html; charset=utf-8" )
			  // The same URL can be any page of the app; do not cache it as a file
			  .header( "Cache-Control", "no-cache" );
		}

	private:
		fs::path m_index;
	};
} // namespace

int main( int argc, char **argv ) {
	auto const *root = argc > 1 ? argv[1] : DAW_MHTTPD_EXAMPLE_WWW;
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
		auto const files = static_files::static_file_handler{ root };
		auto const fallback = app_fallback{ files.root( ) };
		auto routes = mhttpd::router{ };

		// 1. API routes
		routes.get( "/api/time", []( mhttpd::request & ) {
			auto const now = std::chrono::floor<std::chrono::seconds>(
			  std::chrono::system_clock::now( ) );
			return text( std::format( "{:%FT%TZ}\n", now ) );
		} );
		routes.post( "/api/echo", []( mhttpd::request &req ) {
			return text( std::string{ req.body_text( ) } );
		} );

		// 2 and 3: everything the routes do not match. A route path with the
		// wrong method, such as GET /api/echo, never gets here; the router
		// answers 405.
		routes.not_found( [files, fallback]( mhttpd::request &req ) {
			auto const url = req.url( );
			if( url.starts_with( "/api/" ) ) {
				return text( "no such API\n", 404 );
			}
			auto res = files( req );
			if( res.status( ) == 404 and is_page_path( url ) and
			    ( req.method( ) == mhttpd::http_method::GET or
			      req.method( ) == mhttpd::http_method::HEAD ) ) {
				return fallback.serve( );
			}
			return res;
		} );

		auto d = mhttpd::daemon{ { .port = port, .thread_pool_size = 4 },
		                         std::move( routes ) };
		std::println( "Serving {} on http://localhost:{}/",
		              files.root( ).string( ),
		              d.port( ) );

		d.wait_for_signals( { SIGINT, SIGTERM } );
	} catch( std::exception const &e ) {
		std::println( stderr, "error: {}", e.what( ) );
		return EXIT_FAILURE;
	}
}
