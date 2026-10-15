#include "daw/mhttpd/daw_mhttpd_router.h"
#include "daw/mhttpd/daw_mhttpd_log.h"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace daw::mhttpd::detail {
	enum class segment_kind : daw::u8::value_type {
		// Declared from most to least specific; routes compare by this order
		literal,
		param,
		rest
	};

	struct segment {
		segment_kind kind;
		std::string text; // the literal text or the parameter name
	};

	using path_params = std::vector<std::pair<std::string, daw::string_view>>;

	struct route {
		http_method method;
		std::string pattern; // as given, for the log
		std::vector<segment> segments;
		request_handler handler;
	};

	namespace {
		[[nodiscard]] spdlog::logger &router_log( ) {
			return log::get( log::component::router );
		}

		// "/a/b/" -> { "a", "b", "" }. `path` must start with '/'.
		[[nodiscard]] std::vector<daw::string_view>
		split_path( daw::string_view path ) {
			auto result = std::vector<daw::string_view>{ };
			path.remove_prefix( 1 );
			while( true ) {
				auto const pos = path.find( '/' );
				if( pos == daw::string_view::npos ) {
					result.push_back( path );
					return result;
				}
				result.push_back( path.substr( 0, pos ) );
				path.remove_prefix( pos + 1 );
			}
		}

		[[noreturn]] void bad_pattern( daw::string_view pattern,
		                               char const *reason ) {
			throw std::invalid_argument{ "daw::mhttpd: route pattern '" +
			                             std::string{ pattern } + "' " + reason };
		}

		[[nodiscard]] std::vector<segment> parse_pattern( daw::string_view pattern ) {
			if( pattern.empty( ) or pattern.front( ) != '/' ) {
				bad_pattern( pattern, "must start with '/'" );
			}
			auto const parts = split_path( pattern );
			auto result = std::vector<segment>{ };
			result.reserve( parts.size( ) );
			for( auto part : parts ) {
				if( part.starts_with( '{' ) and part.ends_with( '}' ) ) {
					part.remove_prefix( 1 );
					part.remove_suffix( 1 );
					auto kind = segment_kind::param;
					if( part.ends_with( "..." ) ) {
						part.remove_suffix( 3 );
						kind = segment_kind::rest;
					}
					if( part.empty( ) ) {
						bad_pattern( pattern, "has an unnamed parameter" );
					}
					if( std::ranges::any_of( result, [&]( segment const &s ) {
						    return s.kind != segment_kind::literal and s.text == part;
					    } ) ) {
						bad_pattern( pattern, "repeats a parameter name" );
					}
					result.push_back( { kind, std::string{ part } } );
				} else if( part.find( '{' ) != daw::string_view::npos or
				           part.find( '}' ) != daw::string_view::npos ) {
					bad_pattern( pattern,
					             "has a parameter that is not a whole segment" );
				} else {
					result.push_back( { segment_kind::literal, std::string{ part } } );
				}
			}
			if( auto const it =
			      std::ranges::find( result, segment_kind::rest, &segment::kind );
			    it != result.end( ) and it + 1 != result.end( ) ) {
				bad_pattern( pattern, "has a {name...} that is not the last segment" );
			}
			return result;
		}

		// On a match, fills `params` with views into `path`
		[[nodiscard]] bool matches( route const &r,
		                            std::vector<daw::string_view> const &parts,
		                            daw::string_view path, path_params &params ) {
			params.clear( );
			for( std::size_t n = 0; n < r.segments.size( ); ++n ) {
				auto const &seg = r.segments[n];
				if( seg.kind == segment_kind::rest ) {
					auto value = daw::string_view{ };
					if( n < parts.size( ) ) {
						auto const offset = as<std::size_t>( parts[n].data( ) - path.data( ) );
						value = path.substr( offset );
					}
					params.emplace_back( seg.text, value );
					return true;
				}
				if( n >= parts.size( ) ) {
					return false;
				}
				if( seg.kind == segment_kind::literal ) {
					if( parts[n] != seg.text ) {
						return false;
					}
				} else {
					if( parts[n].empty( ) ) {
						return false;
					}
					params.emplace_back( seg.text, parts[n] );
				}
			}
			return r.segments.size( ) == parts.size( );
		}

		// Whether a is more specific than b, segment by segment
		[[nodiscard]] bool more_specific( route const &a, route const &b ) {
			return std::ranges::lexicographical_compare(
			  a.segments, b.segments, std::less{ }, &segment::kind, &segment::kind );
		}

		// Reads the whole body before running a handler that expects it buffered
		detached buffer_body_then( std::shared_ptr<request_handler const> handler,
		                           request &req, responder res ) {
			try {
				(void)co_await req.read_body( );
			} catch( body_too_large const & ) {
				try {
					router_log( ).debug( "{} {}: body too large, answering 413",
					                     req.method_name( ),
					                     req.url( ) );
					res.send( response::empty( 413 ) );
				} catch( ... ) {}
				co_return;
			} catch( ... ) {
				// The body will not arrive; res answers 500 if anyone is listening
				co_return;
			}
			try {
				( *handler )( req, std::move( res ) );
			} catch( ... ) {
				// The handler's copy of the responder answered 500
				log_handler_exception( log::component::router, req );
			}
		}

		[[nodiscard]] request_handler buffer_body_first( request_handler handler ) {
			return [fn = std::make_shared<request_handler const>(
			          std::move( handler ) )]( request &req, responder res ) {
				buffer_body_then( fn, req, std::move( res ) );
			};
		}

		void send_status( responder &res, unsigned status ) {
			res.send( response::empty( status ) );
		}
	} // namespace

	struct route_table {
		static constexpr auto method_count = as<std::size_t>( http_method::UNKNOWN );

		// Once sort( ) has run, grouped by method and most specific first within
		// each, so the first match is the best
		std::vector<route> routes;
		// The routes for method m are routes[method_starts[m], method_starts[m + 1])
		std::array<std::size_t, method_count + 1> method_starts{ };
		std::optional<request_handler> not_found;

		// Stable, so among equally specific routes the one added first stays first
		void sort( ) {
			std::ranges::stable_sort( routes, []( route const &a, route const &b ) {
				if( a.method != b.method ) {
					return a.method < b.method;
				}
				return more_specific( a, b );
			} );
			for( std::size_t m = 0; m <= method_count; ++m ) {
				method_starts[m] = as<std::size_t>(
				  std::ranges::lower_bound(
				    routes, as<http_method>( m ), { }, &route::method ) -
				  routes.begin( ) );
			}
		}

		[[nodiscard]] std::span<route const> routes_for( http_method method ) const {
			auto const m = as<std::size_t>( method );
			return std::span{ routes }.subspan( method_starts[m],
			                                    method_starts[m + 1] -
			                                      method_starts[m] );
		}

		[[nodiscard]] route const *best_route( http_method method,
		                                       std::vector<daw::string_view> const &parts,
		                                       daw::string_view path,
		                                       path_params &params ) const {
			for( auto const &r : routes_for( method ) ) {
				if( matches( r, parts, path, params ) ) {
					return &r;
				}
			}
			return nullptr;
		}

		// "GET, HEAD, POST" for the methods with a route matching the path
		[[nodiscard]] std::string allowed_methods(
		  std::vector<daw::string_view> const &parts, daw::string_view path ) const {
			bool allowed[method_count]{ };
			auto scratch = path_params{ };
			for( std::size_t m = 0; m < method_count; ++m ) {
				allowed[m] = std::ranges::any_of(
				  routes_for( as<http_method>( m ) ),
				  [&]( route const &r ) { return matches( r, parts, path, scratch ); } );
			}
			allowed[as<std::size_t>( http_method::HEAD )] |=
			  allowed[as<std::size_t>( http_method::GET )];
			auto result = std::string{ };
			for( std::size_t n = 0; n < std::size( allowed ); ++n ) {
				if( allowed[n] ) {
					if( not result.empty( ) ) {
						result += ", ";
					}
					auto const name = to_string( as<http_method>( n ) );
					result.append( name.data( ), name.size( ) );
				}
			}
			return result;
		}

		void dispatch( request &req, responder res ) const {
			auto &logger = router_log( );
			if( req.method( ) == http_method::UNKNOWN ) {
				logger.debug( "{} {}: unknown method, answering 501",
				              req.method_name( ),
				              req.url( ) );
				send_status( res, 501 );
				return;
			}
			auto const path = req.url( );
			if( path.empty( ) or path.front( ) != '/' ) {
				run_not_found( req, std::move( res ) );
				return;
			}
			auto const parts = split_path( path );
			auto params = path_params{ };
			auto const *r = best_route( req.method( ), parts, path, params );
			if( r == nullptr and req.method( ) == http_method::HEAD ) {
				r = best_route( http_method::GET, parts, path, params );
			}
			if( r != nullptr ) {
				logger.debug( "{} {} -> {} {}",
				              req.method_name( ),
				              req.url( ),
				              to_string( r->method ),
				              r->pattern );
				req.m_path_params = std::move( params );
				r->handler( req, std::move( res ) );
				return;
			}
			if( auto allow = allowed_methods( parts, path ); not allow.empty( ) ) {
				logger.debug( "{} {}: method not allowed, answering 405 (Allow: {})",
				              req.method_name( ),
				              req.url( ),
				              allow );
				res.send( response::empty( 405 ).header( "Allow", allow ) );
				return;
			}
			logger.debug( "{} {}: no route matches", req.method_name( ), req.url( ) );
			run_not_found( req, std::move( res ) );
		}

		void run_not_found( request &req, responder res ) const {
			if( not_found ) {
				( *not_found )( req, std::move( res ) );
			} else {
				send_status( res, 404 );
			}
		}
	};
} // namespace daw::mhttpd::detail

namespace daw::mhttpd {
	router::router( )
	  : m_table{ std::make_unique<detail::route_table>( ) } {}

	router::router( router && ) noexcept = default;
	router &router::operator=( router && ) noexcept = default;
	router::~router( ) = default;

	void router::add_route( http_method method, daw::string_view pattern,
	                        request_handler handler, bool reads_own_body ) {
		if( method == http_method::UNKNOWN ) {
			throw std::invalid_argument{
			  "daw::mhttpd: a route needs a known HTTP method" };
		}
		auto segments = detail::parse_pattern( pattern );
		if( not reads_own_body ) {
			handler = detail::buffer_body_first( std::move( handler ) );
		}
		m_table->routes.push_back( { method,
		                             std::string{ pattern },
		                             std::move( segments ),
		                             std::move( handler ) } );
	}

	void router::set_not_found( request_handler handler, bool reads_own_body ) {
		if( not reads_own_body ) {
			handler = detail::buffer_body_first( std::move( handler ) );
		}
		m_table->not_found = std::move( handler );
	}

	request_handler router::into_handler( ) && {
		m_table->sort( );
		return [table = std::shared_ptr<detail::route_table const>{
		          std::move( m_table ) }]( request &req, responder res ) {
			table->dispatch( req, std::move( res ) );
		};
	}
} // namespace daw::mhttpd
