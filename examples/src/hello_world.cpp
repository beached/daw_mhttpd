// The smallest useful server: answers every request with "Hello, World!"
// on http://localhost:8080/ until Ctrl-C.
#include <daw/mhttpd/daw_mhttpd.h>

#include <csignal>
#include <cstdio>
#include <print>

int main( ) {
	// The server runs on MHD's own thread from here on
	auto d = daw::mhttpd::daemon{ { .port = 8080 }, []( daw::mhttpd::request & ) {
		                             return daw::mhttpd::response::from_static(
		                                      "Hello, World!" )
		                               .header( "Content-Type", "text/plain" );
	                             } };
	std::println( "Serving on http://localhost:{}/", d.port( ) );
	std::fflush( stdout );
	// Returns once a signal arrives and the server has stopped gracefully
	d.wait_for_signals( { SIGINT, SIGTERM } );
}
