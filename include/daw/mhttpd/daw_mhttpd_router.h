// Routes requests to handlers by method and path.
#pragma once

#include "daw/mhttpd/daw_mhttpd.h"

#include <daw/daw_string_view.h>

#include <memory>
#include <utility>

namespace daw::mhttpd::detail {
	struct route_table;
} // namespace daw::mhttpd::detail

namespace daw::mhttpd {
	// Matches a request's method and path against a list of routes and runs the
	// handler of the best match. Pass it to the daemon once every route is
	// added:
	//
	//   auto routes = daw::mhttpd::router{ };
	//   routes.get( "/worlds/{name}", []( daw::mhttpd::request &req ) {
	//     return daw::mhttpd::response::from_copy( *req.path_param( "name" ) );
	//   } );
	//   auto d = daw::mhttpd::daemon{ { }, std::move( routes ) };
	//
	// Patterns are paths whose segments are literal text, "{name}" for exactly
	// one non-empty segment, or "{name...}" as the last segment for the rest of
	// the path, slashes included and possibly empty. The captures are read with
	// request::path_param. Matching is case-sensitive, a trailing slash is
	// significant, and it runs on the percent-decoded path, so an encoded "/"
	// splits segments like a literal one.
	//
	// When several routes match, the one whose earlier segments are more
	// specific wins: literal before "{name}" before "{name...}"; on a tie the
	// route added first wins. A path that matches only routes for other
	// methods is answered 405 with an Allow header; HEAD uses the GET route when
	// there is no HEAD route. Anything else goes to the not_found handler,
	// which by default answers 404.
	//
	// Handlers take any of the daemon's three styles. Synchronous and callback
	// handlers run once the body is buffered, bounded by config::max_body_size
	// (413 past it). Coroutine handlers start when the headers arrive and read
	// the body themselves, so one route can stream a large upload while the
	// others keep the limit. 404 and 405 are sent without reading the body.
	class router {
	public:
		router( );
		router( router && ) noexcept;
		router &operator=( router && ) noexcept;
		router( router const & ) = delete;
		router &operator=( router const & ) = delete;
		~router( );

		// Throws std::invalid_argument for a malformed pattern
		template<class F>
		requires AsyncHandler<F> or SyncHandler<F> or CoroutineHandler<F>
		router &add( http_method method, daw::string_view pattern, F &&handler ) {
			add_route( method,
			           pattern,
			           detail::make_request_handler( std::forward<F>( handler ),
			                                         log::component::router ),
			           CoroutineHandler<F> );
			return *this;
		}

		template<class F>
		router &get( daw::string_view pattern, F &&handler ) {
			return add( http_method::GET, pattern, std::forward<F>( handler ) );
		}
		template<class F>
		router &post( daw::string_view pattern, F &&handler ) {
			return add( http_method::POST, pattern, std::forward<F>( handler ) );
		}
		template<class F>
		router &put( daw::string_view pattern, F &&handler ) {
			return add( http_method::PUT, pattern, std::forward<F>( handler ) );
		}
		template<class F>
		router &patch( daw::string_view pattern, F &&handler ) {
			return add( http_method::PATCH, pattern, std::forward<F>( handler ) );
		}
		template<class F>
		router &delete_( daw::string_view pattern, F &&handler ) {
			return add( http_method::DELETE, pattern, std::forward<F>( handler ) );
		}

		// Runs for requests no route matches, instead of answering 404
		template<class F>
		requires AsyncHandler<F> or SyncHandler<F> or CoroutineHandler<F>
		router &not_found( F &&handler ) {
			set_not_found(
			  detail::make_request_handler( std::forward<F>( handler ),
			                                         log::component::router ),
			  CoroutineHandler<F> );
			return *this;
		}

		// The handler the daemon runs; it expects headers-only invocation, which
		// daemon( config, router ) arranges
		[[nodiscard]] request_handler into_handler( ) &&;

	private:
		void add_route( http_method method, daw::string_view pattern,
		                request_handler handler, bool reads_own_body );
		void set_not_found( request_handler handler, bool reads_own_body );

		std::unique_ptr<detail::route_table> m_table;
	};
} // namespace daw::mhttpd
