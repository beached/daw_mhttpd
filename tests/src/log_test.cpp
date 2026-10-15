#include "daw/mhttpd/daw_mhttpd.h"
#include "daw/mhttpd/daw_mhttpd_log.h"
#include "http_client.hpp"
#include "static_files.h"

#include <daw/daw_ensure.h>
#include <daw/daw_string_view.h>

#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
	using namespace daw_mhttpd_test;
	namespace log = daw::mhttpd::log;

	// Captures the output of every component as "[name] [level] message" lines
	class captured_log {
	public:
		captured_log( ) {
			auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>( m_out );
			sink->set_pattern( "[%n] [%l] %v" );
			log::set_sinks( { std::move( sink ) } );
		}

		[[nodiscard]] std::string text( ) const {
			return m_out.str( );
		}

		[[nodiscard]] bool contains( std::string const &line ) const {
			return text( ).find( line ) != std::string::npos;
		}

	private:
		std::ostringstream m_out;
	};

	auto const throws = []( daw::mhttpd::request & ) -> daw::mhttpd::response {
		throw std::runtime_error{ "boom" };
	};

	auto const ok = []( daw::mhttpd::request & ) {
		return daw::mhttpd::response::from_static( "ok" );
	};

	void log_default_level_is_error( ) {
		for( auto c : log::all_components ) {
			daw_ensure( log::get( c ).level( ) == spdlog::level::err );
		}
	}

	void log_set_level( ) {
		log::set_level( spdlog::level::off );
		log::set_level( log::component::router, spdlog::level::debug );
		daw_ensure( log::get( log::component::server ).level( ) ==
		            spdlog::level::off );
		daw_ensure( log::get( log::component::router ).level( ) ==
		            spdlog::level::debug );
		daw_ensure( log::get( log::component::static_files ).level( ) ==
		            spdlog::level::off );
	}

	void log_registered_with_spdlog( ) {
		daw_ensure( spdlog::get( "daw_mhttpd.server" ).get( ) ==
		            &log::get( log::component::server ) );
		daw_ensure( spdlog::get( "daw_mhttpd.router" ).get( ) ==
		            &log::get( log::component::router ) );
		daw_ensure( spdlog::get( "daw_mhttpd.static_files" ).get( ) ==
		            &log::get( log::component::static_files ) );
	}

	void log_errors_by_default( ) {
		auto out = captured_log{ };
		{
			daw::mhttpd::daemon d{ { }, throws };
			daw_ensure( http( d.port( ), "GET", "/x" ).status == 500 );
			daw_ensure( http( d.port( ), "GET", "/y" ).status == 500 );
		}
		daw_ensure(
		  out.contains( "[daw_mhttpd.server] [error] handler for GET /x threw: boom" ) );
		daw_ensure(
		  out.contains( "[daw_mhttpd.server] [error] handler for GET /y threw: boom" ) );
		// Nothing below error: no start, stop or per-request lines
		daw_ensure( not out.contains( "[info]" ) );
		daw_ensure( not out.contains( "[debug]" ) );
	}

	void log_off_silences_errors( ) {
		auto out = captured_log{ };
		log::set_level( spdlog::level::off );
		{
			daw::mhttpd::daemon d{ { }, throws };
			daw_ensure( http( d.port( ), "GET", "/x" ).status == 500 );
		}
		daw_ensure( out.text( ).empty( ) );
	}

	void log_server_info_and_debug( ) {
		auto out = captured_log{ };
		log::set_level( log::component::server, spdlog::level::debug );
		auto port = std::uint16_t{ };
		auto threads = 0U;
		{
			daw::mhttpd::daemon d{ { .bind_address = "127.0.0.1" }, ok };
			port = d.port( );
			threads = d.thread_count( );
			daw_ensure( http( d.port( ), "GET", "/hello?x=1" ).status == 200 );
		}
		auto const p = std::to_string( port );
		daw_ensure( out.contains( "[daw_mhttpd.server] [info] listening on 127.0.0.1:" +
		                          p + std::format( " with {} thread{}", threads,
		                                           threads > 1 ? "s" : "" ) ) );
		daw_ensure( out.contains( "[daw_mhttpd.server] [debug] 127.0.0.1 GET /hello -> 200" ) );
		daw_ensure( out.contains( "[daw_mhttpd.server] [info] stopping the server on port " + p ) );
		daw_ensure( out.contains( "[daw_mhttpd.server] [info] stopped the server on port " + p ) );
	}

	// Starts a second daemon on a port that is already taken
	void fail_to_start( ) {
		daw::mhttpd::daemon first{ { .bind_address = "127.0.0.1" }, ok };
		auto threw = false;
		try {
			daw::mhttpd::daemon second{
			  { .port = first.port( ), .bind_address = "127.0.0.1" }, ok };
		} catch( std::runtime_error const & ) { threw = true; }
		daw_ensure( threw );
	}

	void log_libmicrohttpd_warnings( ) {
		auto out = captured_log{ };
		log::set_level( log::component::server, spdlog::level::warn );
		fail_to_start( );
		// MHD's own message, routed through the logger instead of stderr
		daw_ensure( out.contains( "[daw_mhttpd.server] [warning] libmicrohttpd: " ) );
		daw_ensure( out.contains(
		  "[daw_mhttpd.server] [error] could not start the server on 127.0.0.1:" ) );
	}

	void log_libmicrohttpd_hidden_at_error( ) {
		auto out = captured_log{ };
		fail_to_start( );
		// Only our own error at the default level
		daw_ensure( not out.contains( "libmicrohttpd" ) );
		daw_ensure( out.contains(
		  "[daw_mhttpd.server] [error] could not start the server on 127.0.0.1:" ) );
	}

	void log_router_debug( ) {
		auto out = captured_log{ };
		log::set_level( log::component::router, spdlog::level::debug );
		{
			auto routes = daw::mhttpd::router{ };
			routes.get( "/items/{name}", ok ).get( "/boom", throws );
			routes.get( "/coro-boom",
			            []( daw::mhttpd::request & )
			              -> daw::mhttpd::task<daw::mhttpd::response> {
				            throw std::runtime_error{ "coro boom" };
				            co_return daw::mhttpd::response::empty( );
			            } );
			daw::mhttpd::daemon d{ { .max_body_size = 4 }, std::move( routes ) };
			daw_ensure( http( d.port( ), "GET", "/items/a" ).status == 200 );
			daw_ensure( http( d.port( ), "POST", "/items/a", "x" ).status == 405 );
			daw_ensure( http( d.port( ), "GET", "/nope" ).status == 404 );
			daw_ensure( http( d.port( ), "PROPFIND", "/items/a" ).status == 501 );
			daw_ensure( http( d.port( ), "GET", "/boom", "too large" ).status == 413 );
			daw_ensure( http( d.port( ), "GET", "/boom" ).status == 500 );
			daw_ensure( http( d.port( ), "GET", "/coro-boom" ).status == 500 );
		}
		daw_ensure( out.contains( "[daw_mhttpd.router] [debug] GET /items/a -> GET /items/{name}" ) );
		daw_ensure( out.contains(
		  "[daw_mhttpd.router] [debug] POST /items/a: method not allowed, answering 405 (Allow: GET, HEAD)" ) );
		daw_ensure( out.contains( "[daw_mhttpd.router] [debug] GET /nope: no route matches" ) );
		daw_ensure(
		  out.contains( "[daw_mhttpd.router] [debug] PROPFIND /items/a: unknown method, answering 501" ) );
		daw_ensure(
		  out.contains( "[daw_mhttpd.router] [debug] GET /boom: body too large, answering 413" ) );
		daw_ensure( out.contains( "[daw_mhttpd.router] [error] handler for GET /boom threw: boom" ) );
		// Coroutine routes log under the router too
		daw_ensure( out.contains(
		  "[daw_mhttpd.router] [error] handler for GET /coro-boom threw: coro boom" ) );
		daw_ensure( not out.contains( "[daw_mhttpd.server] [error]" ) );
		// The server component stayed at error
		daw_ensure( not out.contains( "[daw_mhttpd.server] [debug]" ) );
	}

	void log_static_files_debug( ) {
		auto out = captured_log{ };
		log::set_level( log::component::static_files, spdlog::level::debug );
		auto const root = std::filesystem::temp_directory_path( ) /
		                  ( "daw_mhttpd_log_" + std::to_string( ::getpid( ) ) );
		std::filesystem::create_directories( root );
		{
			auto f = std::fopen( ( root / "a.txt" ).c_str( ), "w" );
			std::fputs( "a", f );
			std::fclose( f );
		}
		{
			daw::mhttpd::daemon d{ { }, static_files::static_file_handler{ root } };
			daw_ensure( http( d.port( ), "GET", "/a.txt" ).status == 200 );
			daw_ensure( http( d.port( ), "GET", "/b.txt" ).status == 404 );
		}
		std::filesystem::remove_all( root );
		daw_ensure( out.contains( "[daw_mhttpd.static_files] [debug] GET /a.txt -> 200" ) );
		daw_ensure( out.contains( "[daw_mhttpd.static_files] [debug] GET /b.txt -> 404" ) );
	}
} // namespace

int main( int argc, char **argv ) {
	struct test_case {
		daw::string_view name;
		void ( *fn )( );
	};
	static constexpr test_case tests[] = {
	  { "log_default_level_is_error", &log_default_level_is_error },
	  { "log_set_level", &log_set_level },
	  { "log_registered_with_spdlog", &log_registered_with_spdlog },
	  { "log_errors_by_default", &log_errors_by_default },
	  { "log_off_silences_errors", &log_off_silences_errors },
	  { "log_server_info_and_debug", &log_server_info_and_debug },
	  { "log_libmicrohttpd_warnings", &log_libmicrohttpd_warnings },
	  { "log_libmicrohttpd_hidden_at_error", &log_libmicrohttpd_hidden_at_error },
	  { "log_router_debug", &log_router_debug },
	  { "log_static_files_debug", &log_static_files_debug },
	};
	for( auto const &t : tests ) {
		if( argc < 2 || t.name == argv[1] ) {
			std::println( "{}", t.name );
			std::fflush( stdout );
			t.fn( );
		}
	}
	if( argc >= 2 && std::ranges::none_of( tests, [&]( test_case const &t ) {
		    return t.name == argv[1];
	    } ) ) {
		std::println( stderr, "unknown test: {}", argv[1] );
		return EXIT_FAILURE;
	}
}
