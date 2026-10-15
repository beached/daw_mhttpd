// A small key/value server on http://localhost:8080/ showing the router with
// synchronous and coroutine routes, until Ctrl-C.
//
//   curl localhost:8080/items
//   curl -X PUT --data 'red' localhost:8080/items/colour
//   curl localhost:8080/items/colour
//   curl -X DELETE localhost:8080/items/colour
//   curl localhost:8080/items/colour/later
//   curl --data-binary @some-large-file localhost:8080/upload
#include <daw/mhttpd/daw_mhttpd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <utility>

namespace mhttpd = daw::mhttpd;

namespace {
	// Handlers run on several threads at once, so shared state needs a lock
	class item_store {
	public:
		[[nodiscard]] std::optional<std::string>
		get( std::string const &name ) const {
			auto lock = std::lock_guard{ m_mutex };
			if( auto it = m_items.find( name ); it != m_items.end( ) ) {
				return it->second;
			}
			return std::nullopt;
		}

		// Returns true if the item is new
		bool put( std::string name, std::string value ) {
			auto lock = std::lock_guard{ m_mutex };
			return m_items.insert_or_assign( std::move( name ), std::move( value ) )
			  .second;
		}

		bool erase( std::string const &name ) {
			auto lock = std::lock_guard{ m_mutex };
			return m_items.erase( name ) != 0;
		}

		// One "name=value" line per item
		[[nodiscard]] std::string list( ) const {
			auto lock = std::lock_guard{ m_mutex };
			auto out = std::string{ };
			for( auto const &[name, value] : m_items ) {
				out += name + '=' + value + '\n';
			}
			return out;
		}

	private:
		mutable std::mutex m_mutex;
		std::map<std::string, std::string> m_items;
	};

	// A callback style API, standing in for a database client or similar: calls
	// `on_done` later, from its own thread. It shares ownership of the store
	// because the server may stop, and main return, before it finishes.
	void lookup_slowly(
	  std::shared_ptr<item_store const> items, std::string name,
	  std::move_only_function<void( std::optional<std::string> )> on_done ) {
		std::thread{ [items = std::move( items ),
		              name = std::move( name ),
		              on_done = std::move( on_done )]( ) mutable {
			std::this_thread::sleep_for( std::chrono::milliseconds{ 500 } );
			on_done( items->get( name ) );
		} }
		  .detach( );
	}

	[[nodiscard]] mhttpd::response text( std::string body,
	                                     unsigned status = 200 ) {
		return mhttpd::response::from_owned( std::move( body ), status )
		  .header( "Content-Type", "text/plain" );
	}

	[[nodiscard]] std::string item_name( mhttpd::request const &req ) {
		// The route pattern guarantees the parameter exists
		return std::string{ *req.path_param( "name" ) };
	}
} // namespace

int main( ) {
	auto items = std::make_shared<item_store>( );
	auto routes = mhttpd::router{ };

	// Synchronous routes: they run once the body is buffered, limited by
	// config::max_body_size
	routes.get( "/items", [items]( mhttpd::request & ) {
		return text( items->list( ) );
	} );

	routes.get( "/items/{name}", [items]( mhttpd::request &req ) {
		if( auto value = items->get( item_name( req ) ) ) {
			return text( std::move( *value ) );
		}
		return text( "no such item\n", 404 );
	} );

	routes.put( "/items/{name}", [items]( mhttpd::request &req ) {
		auto const created =
		  items->put( item_name( req ), std::string{ req.body_text( ) } );
		return mhttpd::response::empty( created ? 201 : 204 );
	} );

	routes.delete_( "/items/{name}", [items]( mhttpd::request &req ) {
		return mhttpd::response::empty( items->erase( item_name( req ) ) ? 204
		                                                                 : 404 );
	} );

	// Coroutine routes: they start when the headers arrive

	// Awaits a callback API without holding up MHD's thread meanwhile
	routes.get(
	  "/items/{name}/later",
	  [items]( mhttpd::request &req ) -> mhttpd::task<mhttpd::response> {
		  // Copy what is needed from req before suspending
		  auto name = item_name( req );
		  auto value = co_await mhttpd::from_callback<std::optional<std::string>>(
		    [&]( mhttpd::completion<std::optional<std::string>> done ) {
			    lookup_slowly( items, name, std::move( done ) );
		    } );
		  if( not value ) {
			  co_return text( "no such item\n", 404 );
		  }
		  co_return text( std::move( *value ) );
	  } );

	// Reads the body piece by piece, so it is not limited by max_body_size
	routes.post(
	  "/upload", []( mhttpd::request &req ) -> mhttpd::task<mhttpd::response> {
		  auto size = std::size_t{ 0 };
		  while( auto chunk = co_await req.next_chunk( ) ) {
			  size += chunk->size( );
		  }
		  co_return text( "received " + std::to_string( size ) + " bytes\n" );
	  } );

	routes.not_found( []( mhttpd::request &req ) {
		return text( "nothing at " + std::string{ req.url( ) } + '\n', 404 );
	} );

	auto d = mhttpd::daemon{
	  { .port = 8080, .thread_pool_size = 4, .max_body_size = 64 * 1024 },
	  std::move( routes ) };
	std::println( "Serving on http://localhost:{}/", d.port( ) );
	std::fflush( stdout );
	d.wait_for_signals( { SIGINT, SIGTERM } );
}
