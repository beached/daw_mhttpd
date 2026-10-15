// Hello, World! from a coroutine handler on http://localhost:8080/ until
// Ctrl-C. The greeting is produced on another thread, standing in for any
// callback based API (a database client, a child process, ...); the handler
// awaits it without blocking MHD's thread.
#include <daw/mhttpd/daw_mhttpd.h>

#include <csignal>
#include <cstdio>
#include <functional>
#include <print>
#include <string>
#include <thread>

namespace mhttpd = daw::mhttpd;

// A callback style API: calls `on_done` later, from its own thread
static void
fetch_greeting_async( std::move_only_function<void( std::string )> on_done ) {
	std::thread{ [on_done = std::move( on_done )]( ) mutable {
		on_done( "Hello, World!" );
	} }
	  .detach( );
}

int main( ) {
	auto d = mhttpd::daemon{
	  { .port = 8080 },
	  []( mhttpd::request & ) -> mhttpd::task<mhttpd::response> {
		  // Suspends here; resumes on the thread that calls `done`
		  auto greeting = co_await mhttpd::from_callback<std::string>(
		    []( mhttpd::completion<std::string> done ) {
			    fetch_greeting_async( std::move( done ) );
		    } );
		  co_return mhttpd::response::from_owned( std::move( greeting ) )
		    .header( "Content-Type", "text/plain" );
	  } };
	std::println( "Serving on http://localhost:{}/", d.port( ) );
	std::fflush( stdout );
	d.wait_for_signals( { SIGINT, SIGTERM } );
}
