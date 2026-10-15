#include "daw/mhttpd/daw_mhttpd.h"
#include "http_client.hpp"

#include <daw/daw_ensure.h>
#include <daw/daw_string_view.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
	using namespace daw_mhttpd_test;

	[[nodiscard]] bool has_header( http_result const &r, std::string const &line ) {
		return r.headers.find( "\r\n" + line + "\r\n" ) != std::string::npos or
		       r.headers.ends_with( "\r\n" + line );
	}

	// Answers with the route's name and its path parameters
	[[nodiscard]] auto reply( std::string name ) {
		return [name = std::move( name )]( daw::mhttpd::request &req ) {
			auto body = name;
			for( auto const *param : { "name", "list", "path" } ) {
				if( auto value = req.path_param( param ) ) {
					body += ' ';
					body += param;
					body += '=';
					body += std::string{ *value };
				}
			}
			return daw::mhttpd::response::from_owned( std::move( body ) );
		};
	}

	void router_path_params( ) {
		auto routes = daw::mhttpd::router{ };
		routes.get( "/worlds/{name}", reply( "world" ) )
		  .get( "/players/{list}/{name}", reply( "player" ) );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		daw_ensure( http( d.port( ), "GET", "/worlds/abc" ).body ==
		            "world name=abc" );
		daw_ensure( http( d.port( ), "GET", "/players/ops/steve" ).body ==
		            "player name=steve list=ops" );
		// The query string is not part of the path
		daw_ensure( http( d.port( ), "GET", "/worlds/abc?lines=5" ).body ==
		            "world name=abc" );
		// Percent-decoded before matching
		daw_ensure( http( d.port( ), "GET", "/worlds/a%20b" ).body ==
		            "world name=a b" );
		// {name} needs a non-empty segment, and the segment count must match
		daw_ensure( http( d.port( ), "GET", "/worlds/" ).status == 404 );
		daw_ensure( http( d.port( ), "GET", "/worlds" ).status == 404 );
		daw_ensure( http( d.port( ), "GET", "/worlds/abc/def" ).status == 404 );
	}

	void router_literal_beats_param( ) {
		auto routes = daw::mhttpd::router{ };
		// Added in the order least to most specific, so order alone would pick
		// the wrong one
		routes.get( "/{path...}", reply( "rest" ) )
		  .get( "/worlds/{name}", reply( "param" ) )
		  .get( "/worlds/new", reply( "literal" ) )
		  .get( "/{list}/new", reply( "param-literal" ) );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		daw_ensure( http( d.port( ), "GET", "/worlds/new" ).body == "literal" );
		daw_ensure( http( d.port( ), "GET", "/worlds/old" ).body ==
		            "param name=old" );
		// The first segment decides: literal "worlds" beats {list}
		daw_ensure( http( d.port( ), "GET", "/other/new" ).body ==
		            "param-literal list=other" );
		daw_ensure( http( d.port( ), "GET", "/x/y/z" ).body == "rest path=x/y/z" );
	}

	void router_first_added_wins_tie( ) {
		auto routes = daw::mhttpd::router{ };
		routes.get( "/a/{name}", reply( "first" ) )
		  .get( "/a/{list}", reply( "second" ) );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		daw_ensure( http( d.port( ), "GET", "/a/b" ).body == "first name=b" );
	}

	void router_rest_param( ) {
		auto routes = daw::mhttpd::router{ };
		routes.get( "/files/{path...}", reply( "files" ) );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		daw_ensure( http( d.port( ), "GET", "/files/a/b.txt" ).body ==
		            "files path=a/b.txt" );
		daw_ensure( http( d.port( ), "GET", "/files/a/" ).body ==
		            "files path=a/" );
		daw_ensure( http( d.port( ), "GET", "/files/" ).body == "files path=" );
		daw_ensure( http( d.port( ), "GET", "/files" ).body == "files path=" );
		daw_ensure( http( d.port( ), "GET", "/filesx" ).status == 404 );
	}

	void router_root_and_trailing_slash( ) {
		auto routes = daw::mhttpd::router{ };
		routes.get( "/", reply( "root" ) ).get( "/a", reply( "a" ) );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		daw_ensure( http( d.port( ), "GET", "/" ).body == "root" );
		daw_ensure( http( d.port( ), "GET", "/a" ).body == "a" );
		daw_ensure( http( d.port( ), "GET", "/a/" ).status == 404 );
		daw_ensure( http( d.port( ), "GET", "/A" ).status == 404 );
	}

	void router_method_not_allowed( ) {
		auto routes = daw::mhttpd::router{ };
		routes.post( "/worlds/new", reply( "create" ) )
		  .delete_( "/worlds/{name}", reply( "delete" ) )
		  .get( "/status", reply( "status" ) );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		auto r = http( d.port( ), "GET", "/worlds/new" );
		daw_ensure( r.status == 405 );
		daw_ensure( has_header( r, "Allow: POST, DELETE" ) );
		// The DELETE route matches with name=new
		daw_ensure( http( d.port( ), "DELETE", "/worlds/new" ).body ==
		            "delete name=new" );
		r = http( d.port( ), "PUT", "/status", "x" );
		daw_ensure( r.status == 405 );
		daw_ensure( has_header( r, "Allow: GET, HEAD" ) );
		daw_ensure( http( d.port( ), "GET", "/nothing" ).status == 404 );
	}

	void router_head_uses_get( ) {
		auto routes = daw::mhttpd::router{ };
		routes.get( "/a", reply( "get" ) ).get( "/b", reply( "get b" ) );
		routes.add( daw::mhttpd::http_method::HEAD,
		            "/b",
		            []( daw::mhttpd::request & ) {
			            return daw::mhttpd::response::empty( 200 ).header( "X-Head",
			                                                               "yes" );
		            } );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		auto r = http( d.port( ), "HEAD", "/a" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body.empty( ) );
		daw_ensure( has_header( r, "Content-Length: 3" ) );
		// A HEAD route beats the GET one
		r = http( d.port( ), "HEAD", "/b" );
		daw_ensure( r.status == 200 );
		daw_ensure( has_header( r, "X-Head: yes" ) );
	}

	void router_not_found_handler( ) {
		auto routes = daw::mhttpd::router{ };
		routes.get( "/a", reply( "a" ) )
		  .not_found( []( daw::mhttpd::request &req ) {
			  return daw::mhttpd::response::from_owned(
			    "missing " + std::string{ req.url( ) } + " " +
			      std::string{ req.body_text( ) },
			    404 );
		  } );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		auto r = http( d.port( ), "POST", "/b", "body" );
		daw_ensure( r.status == 404 );
		daw_ensure( r.body == "missing /b body" );
		// 405 still wins over not_found for a known path
		daw_ensure( http( d.port( ), "POST", "/a", "x" ).status == 405 );
	}

	void router_unknown_method( ) {
		auto routes = daw::mhttpd::router{ };
		routes.get( "/a", reply( "a" ) );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		daw_ensure( http( d.port( ), "PROPFIND", "/a" ).status == 501 );
	}

	void router_sync_route_gets_body( ) {
		auto routes = daw::mhttpd::router{ };
		routes.post( "/echo/{name}", []( daw::mhttpd::request &req ) {
			return daw::mhttpd::response::from_owned(
			  std::string{ *req.path_param( "name" ) } + ":" +
			  std::string{ req.body_text( ) } );
		} );
		daw::mhttpd::daemon d{ { .max_body_size = 1000 }, std::move( routes ) };
		auto const payload = std::string( 1000, 'x' );
		auto r = http( d.port( ), "POST", "/echo/n", payload );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "n:" + payload );
		// Chunked bodies are buffered too
		r = http( d.port( ),
		          "POST",
		          "/echo/c",
		          "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n",
		          "Transfer-Encoding: chunked\r\n" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "c:abcde" );
		daw_ensure( http( d.port( ), "POST", "/echo/n", payload + "x" ).status ==
		            413 );
	}

	void router_callback_route( ) {
		auto routes = daw::mhttpd::router{ };
		routes.post( "/later",
		             []( daw::mhttpd::request &req, daw::mhttpd::responder res ) {
			             auto body = std::string{ req.body_text( ) };
			             std::thread{ [res = std::move( res ),
			                           body = std::move( body )]( ) mutable {
				             res.send( daw::mhttpd::response::from_owned(
				               std::move( body ) ) );
			             } }
			               .detach( );
		             } );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		auto r = http( d.port( ), "POST", "/later", "hello" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "hello" );
	}

	void router_handler_exception_gives_500( ) {
		auto routes = daw::mhttpd::router{ };
		routes.get( "/sync", []( daw::mhttpd::request & ) -> daw::mhttpd::response {
			throw std::runtime_error{ "boom" };
		} );
		routes.get( "/coro",
		            []( daw::mhttpd::request & )
		              -> daw::mhttpd::task<daw::mhttpd::response> {
			            throw std::runtime_error{ "boom" };
			            co_return daw::mhttpd::response::empty( );
		            } );
		daw::mhttpd::daemon d{ { }, std::move( routes ) };
		daw_ensure( http( d.port( ), "GET", "/sync" ).status == 500 );
		daw_ensure( http( d.port( ), "GET", "/coro" ).status == 500 );
	}

	void router_coroutine_route_streams_body( ) {
		auto routes = daw::mhttpd::router{ };
		routes.post( "/upload/{name}",
		             []( daw::mhttpd::request &req )
		               -> daw::mhttpd::task<daw::mhttpd::response> {
			             // Copy before suspending
			             auto name = std::string{ *req.path_param( "name" ) };
			             auto total = std::size_t{ 0 };
			             while( auto chunk = co_await req.next_chunk( ) ) {
				             total += chunk->size( );
			             }
			             co_return daw::mhttpd::response::from_owned(
			               name + " " + std::to_string( total ) );
		             } )
		  .post( "/small", reply( "small" ) );
		daw::mhttpd::daemon d{ { .max_body_size = 10 }, std::move( routes ) };
		auto const payload = std::string( 1'000'000, 'x' );
		auto r = http( d.port( ), "POST", "/upload/w", payload );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "w 1000000" );
		// The other routes keep the limit
		daw_ensure( http( d.port( ), "POST", "/small", payload ).status == 413 );
	}

	void router_bad_patterns_throw( ) {
		for( auto const *pattern : { "",
		                             "a",
		                             "/a/{}",
		                             "/a/{...}",
		                             "/{a...}/b",
		                             "/a{b}",
		                             "/{a}x",
		                             "/{a}/{a}" } ) {
			auto threw = false;
			try {
				daw::mhttpd::router{ }.get( pattern, reply( "x" ) );
			} catch( std::invalid_argument const & ) { threw = true; }
			daw_ensure( threw );
		}
		auto threw = false;
		try {
			daw::mhttpd::router{ }.add(
			  daw::mhttpd::http_method::UNKNOWN, "/a", reply( "x" ) );
		} catch( std::invalid_argument const & ) { threw = true; }
		daw_ensure( threw );
	}
} // namespace

int main( int argc, char **argv ) {
	struct test_case {
		daw::string_view name;
		void ( *fn )( );
	};
	static constexpr test_case tests[] = {
	  { "router_path_params", &router_path_params },
	  { "router_literal_beats_param", &router_literal_beats_param },
	  { "router_first_added_wins_tie", &router_first_added_wins_tie },
	  { "router_rest_param", &router_rest_param },
	  { "router_root_and_trailing_slash", &router_root_and_trailing_slash },
	  { "router_method_not_allowed", &router_method_not_allowed },
	  { "router_head_uses_get", &router_head_uses_get },
	  { "router_not_found_handler", &router_not_found_handler },
	  { "router_unknown_method", &router_unknown_method },
	  { "router_sync_route_gets_body", &router_sync_route_gets_body },
	  { "router_callback_route", &router_callback_route },
	  { "router_handler_exception_gives_500",
	    &router_handler_exception_gives_500 },
	  { "router_coroutine_route_streams_body",
	    &router_coroutine_route_streams_body },
	  { "router_bad_patterns_throw", &router_bad_patterns_throw },
	};
	for( auto const &t : tests ) {
		if( argc < 2 || t.name == argv[1] ) {
			std::printf(
			  "%.*s\n", static_cast<int>( t.name.size( ) ), t.name.data( ) );
			std::fflush( stdout );
			t.fn( );
		}
	}
	if( argc >= 2 && std::ranges::none_of( tests, [&]( test_case const &t ) {
		    return t.name == argv[1];
	    } ) ) {
		std::fprintf( stderr, "unknown test: %s\n", argv[1] );
		return EXIT_FAILURE;
	}
}
