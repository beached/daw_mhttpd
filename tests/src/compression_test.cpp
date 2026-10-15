#include "daw/mhttpd/daw_mhttpd.h"
#include "http_client.hpp"

#include <daw/daw_copyable_function.h>
#include <daw/daw_ensure.h>
#include <daw/daw_string_view.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined( DAW_MHTTPD_TEST_ZSTD )
#include <zstd.h>
#endif

namespace {
	using namespace daw_mhttpd_test;
	using daw::mhttpd::content_coding;

	[[nodiscard]] bool has_header( http_result const &r, std::string const &line ) {
		return r.headers.find( "\r\n" + line + "\r\n" ) != std::string::npos or
		       r.headers.ends_with( "\r\n" + line );
	}

	[[nodiscard]] bool has_header_named( http_result const &r,
	                                     std::string const &name ) {
		return r.headers.find( "\r\n" + name + ":" ) != std::string::npos;
	}

	[[nodiscard]] bool zstd_built( ) {
		return daw::mhttpd::coding_available( content_coding::zstd );
	}

	[[nodiscard]] std::string unzstd( std::string const &in ) {
#if defined( DAW_MHTTPD_TEST_ZSTD )
		auto const size = ZSTD_getFrameContentSize( in.data( ), in.size( ) );
		daw_ensure( size != ZSTD_CONTENTSIZE_ERROR and
		            size != ZSTD_CONTENTSIZE_UNKNOWN );
		auto out = std::string( size, '\0' );
		auto const n =
		  ZSTD_decompress( out.data( ), out.size( ), in.data( ), in.size( ) );
		daw_ensure( not ZSTD_isError( n ) and n == size );
		return out;
#else
		std::fputs( "zstd is not available to the tests\n", stderr );
		std::abort( );
		(void)in;
#endif
	}

	// Compressible and well over the default min_size
	[[nodiscard]] std::string const &text_body( ) {
		static auto const body = [] {
			auto s = std::string{ };
			for( auto i = 0; i < 200; ++i ) {
				s += std::format( "line {} of some very compressible text\n", i );
			}
			return s;
		}( );
		return body;
	}

	[[nodiscard]] daw::mhttpd::config compressing( ) {
		auto cfg = daw::mhttpd::config{ };
		cfg.compression.enabled = true;
		return cfg;
	}

	auto const accept_zstd = std::string{ "Accept-Encoding: gzip, zstd\r\n" };

	[[nodiscard]] auto text_handler( std::string type = "text/plain" ) {
		return [type = std::move( type )]( daw::mhttpd::request & ) {
			return daw::mhttpd::response::from_copy( text_body( ) )
			  .header( "Content-Type", type );
		};
	}

	void accepts_coding_parsing( ) {
		using daw::mhttpd::accepts_coding;
		constexpr auto z = content_coding::zstd;
		constexpr auto id = content_coding::identity;
		daw_ensure( accepts_coding( "zstd", z ) );
		daw_ensure( accepts_coding( "gzip, deflate, br, zstd", z ) );
		daw_ensure( accepts_coding( "ZSTD", z ) );
		daw_ensure( accepts_coding( " gzip ,zstd ; q=0.5", z ) );
		daw_ensure( accepts_coding( "zstd;q=1", z ) );
		daw_ensure( accepts_coding( "zstd;q=1.000", z ) );
		daw_ensure( accepts_coding( "zstd;q=0.001", z ) );
		daw_ensure( accepts_coding( "*", z ) );
		daw_ensure( accepts_coding( "gzip;q=1, *;q=0.1", z ) );
		daw_ensure( not accepts_coding( "", z ) );
		daw_ensure( not accepts_coding( "gzip, br", z ) );
		daw_ensure( not accepts_coding( "zstd;q=0", z ) );
		daw_ensure( not accepts_coding( "zstd;q=0.000", z ) );
		daw_ensure( not accepts_coding( "zstd;q=0, *", z ) ); // listed wins
		daw_ensure( not accepts_coding( "*;q=0", z ) );
		daw_ensure( not accepts_coding( "zstd;q=2", z ) );    // malformed
		daw_ensure( not accepts_coding( "zstd;q=0.5x", z ) ); // malformed
		daw_ensure( not accepts_coding( "zstdx", z ) );

		daw_ensure( accepts_coding( "", id ) );
		daw_ensure( accepts_coding( "gzip", id ) );
		daw_ensure( accepts_coding( "*;q=0, identity", id ) );
		daw_ensure( not accepts_coding( "identity;q=0", id ) );
		daw_ensure( not accepts_coding( "gzip, *;q=0", id ) );
	}

	void compressible_types( ) {
		using daw::mhttpd::is_compressible_type;
		for( auto const *t :
		     { "text/plain", "text/html; charset=utf-8", "TEXT/CSS",
		       "application/json", "application/javascript", "image/svg+xml",
		       "application/problem+json", "application/atom+xml",
		       "application/wasm", " application/json ;charset=utf-8" } ) {
			daw_ensure( is_compressible_type( t ) );
		}
		for( auto const *t : { "image/png", "image/jpeg", "application/zip",
		                       "application/octet-stream", "video/mp4",
		                       "font/woff2", "text", "", "json" } ) {
			daw_ensure( not is_compressible_type( t ) );
		}
	}

	void compresses_when_accepted( ) {
		daw::mhttpd::daemon d{ compressing( ), text_handler( ) };
		auto const r = http( d.port( ), "GET", "/", { }, accept_zstd );
		daw_ensure( r.status == 200 );
		if( not zstd_built( ) ) {
			daw_ensure( r.body == text_body( ) );
			daw_ensure( not has_header_named( r, "Content-Encoding" ) );
			daw_ensure( not has_header_named( r, "Vary" ) );
			return;
		}
		daw_ensure( has_header( r, "Content-Encoding: zstd" ) );
		daw_ensure( has_header( r, "Vary: Accept-Encoding" ) );
		daw_ensure( has_header( r, std::format( "Content-Length: {}", r.body.size( ) ) ) );
		daw_ensure( r.body.size( ) < text_body( ).size( ) );
		daw_ensure( unzstd( r.body ) == text_body( ) );
	}

	void static_body_compressed( ) {
		daw::mhttpd::daemon d{ compressing( ), []( daw::mhttpd::request & ) {
			                      return daw::mhttpd::response::from_static(
			                               daw::string_view{ text_body( ) } )
			                        .header( "Content-Type", "application/json" );
		                      } };
		auto const r = http( d.port( ), "GET", "/", { }, accept_zstd );
		if( zstd_built( ) ) {
			daw_ensure( has_header( r, "Content-Encoding: zstd" ) );
			daw_ensure( unzstd( r.body ) == text_body( ) );
		}
	}

	[[nodiscard]] auto static_handler( daw::string_view body ) {
		return [body]( daw::mhttpd::request & ) {
			return daw::mhttpd::response::from_static( body ).header( "Content-Type",
			                                                          "text/plain" );
		};
	}

	// Every response decodes to the body, whether it came from the cache, was
	// compressed and cached, or was compressed because the cache was full
	void static_body_repeated( std::size_t cache_size ) {
		auto cfg = compressing( );
		cfg.compression.cache_size = cache_size;
		cfg.thread_pool_size = 4;
		daw::mhttpd::daemon d{ cfg, static_handler( text_body( ) ) };
		for( auto i = 0; i < 8; ++i ) {
			auto const r = http( d.port( ), "GET", "/", { }, accept_zstd );
			daw_ensure( r.status == 200 );
			if( zstd_built( ) ) {
				daw_ensure( has_header( r, "Content-Encoding: zstd" ) );
				daw_ensure( unzstd( r.body ) == text_body( ) );
			} else {
				daw_ensure( r.body == text_body( ) );
			}
			auto const plain = http( d.port( ), "GET", "/" );
			daw_ensure( plain.body == text_body( ) );
			daw_ensure( not has_header_named( plain, "Content-Encoding" ) );
		}
	}

	void static_body_cached( ) {
		static_body_repeated( daw::mhttpd::compression_config{ }.cache_size );
	}

	void static_cache_disabled( ) {
		static_body_repeated( 0 );
	}

	// Smaller than the compressed body, so nothing fits
	void static_cache_full( ) {
		static_body_repeated( 100 );
	}

	// Several distinct static bodies served from one cache
	void static_bodies_kept_apart( ) {
		static auto const bodies = [] {
			auto v = std::vector<std::string>{ };
			for( auto i = 0; i < 4; ++i ) {
				v.push_back( std::format( "{}{}", i, text_body( ) ) );
			}
			return v;
		}( );
		daw::mhttpd::daemon d{ compressing( ), []( daw::mhttpd::request &req ) {
			                      auto const &b = bodies[static_cast<std::size_t>(
			                        req.url( ).back( ) - '0' )];
			                      return daw::mhttpd::response::from_static(
			                               daw::string_view{ b } )
			                        .header( "Content-Type", "text/plain" );
		                      } };
		for( auto round = 0; round < 2; ++round ) {
			for( auto i = 0; i < 4; ++i ) {
				auto const r =
				  http( d.port( ), "GET", std::format( "/{}", i ), { }, accept_zstd );
				daw_ensure( ( zstd_built( ) ? unzstd( r.body ) : r.body ) ==
				            bodies[static_cast<std::size_t>( i )] );
			}
		}
	}

	// A static body that does not shrink is remembered and still sent as is
	void static_no_gain_sends_identity( ) {
		static auto const noise = [] {
			auto s = std::string( 8192, '\0' );
			auto gen = std::mt19937{ 7 };
			std::ranges::generate( s, [&] {
				return static_cast<char>( gen( ) );
			} );
			return s;
		}( );
		daw::mhttpd::daemon d{ compressing( ), static_handler( noise ) };
		for( auto i = 0; i < 3; ++i ) {
			auto const r = http( d.port( ), "GET", "/", { }, accept_zstd );
			daw_ensure( r.body == noise );
			daw_ensure( not has_header_named( r, "Content-Encoding" ) );
			daw_ensure( has_header( r, "Vary: Accept-Encoding" ) == zstd_built( ) );
		}
	}

	void identity_without_accept_encoding( ) {
		daw::mhttpd::daemon d{ compressing( ), text_handler( ) };
		for( auto const &extra :
		     { std::string{ }, std::string{ "Accept-Encoding: gzip, br\r\n" },
		       std::string{ "Accept-Encoding: zstd;q=0\r\n" } } ) {
			auto const r = http( d.port( ), "GET", "/", { }, extra );
			daw_ensure( r.body == text_body( ) );
			daw_ensure( not has_header_named( r, "Content-Encoding" ) );
			// Caches must still key on Accept-Encoding
			daw_ensure( has_header( r, "Vary: Accept-Encoding" ) == zstd_built( ) );
		}
	}

	void disabled_by_default( ) {
		daw::mhttpd::daemon d{ { }, text_handler( ) };
		auto const r = http( d.port( ), "GET", "/", { }, accept_zstd );
		daw_ensure( r.body == text_body( ) );
		daw_ensure( not has_header_named( r, "Content-Encoding" ) );
		daw_ensure( not has_header_named( r, "Vary" ) );
	}

	// Each of these is sent as is, without Vary: no client would get it
	// compressed
	void ineligible_responses_untouched( ) {
		auto const small = std::string( 100, 'a' );
		auto const cases = std::vector<std::pair<
		  std::string, daw::copyable_function<daw::mhttpd::response( ) const>>>{
		  // below min_size
		  { small, [&] {
			  return daw::mhttpd::response::from_copy( small ).header(
			    "Content-Type", "text/plain" );
		  } },
		  // not a compressible type
		  { text_body( ), [] {
			  return daw::mhttpd::response::from_copy( text_body( ) )
			    .header( "Content-Type", "image/png" );
		  } },
		  // no Content-Type
		  { text_body( ), [] {
			  return daw::mhttpd::response::from_copy( text_body( ) );
		  } },
		  // opted out
		  { text_body( ), [] {
			  return daw::mhttpd::response::from_copy( text_body( ) )
			    .header( "Content-Type", "text/plain" )
			    .compression( daw::mhttpd::compress::never );
		  } },
		  // a part of the representation
		  { text_body( ), [] {
			  return daw::mhttpd::response::from_copy( text_body( ), 206 )
			    .header( "Content-Type", "text/plain" )
			    .header( "Content-Range",
			             std::format( "bytes 0-{}/9999", text_body( ).size( ) - 1 ) );
		  } },
		  // a stream
		  { text_body( ), [] {
			  return daw::mhttpd::response::stream(
			           [done = false]( daw::u64, std::span<std::byte> out ) mutable {
				           if( done ) {
					           return daw::mhttpd::stream_result::end( );
				           }
				           done = true;
				           std::ranges::copy( std::as_bytes( std::span{ text_body( ) } ),
				                              out.begin( ) );
				           return daw::mhttpd::stream_result::bytes(
				             text_body( ).size( ) );
			           } )
			    .header( "Content-Type", "text/plain" );
		  } } };
		for( auto const &[expected, make] : cases ) {
			daw::mhttpd::daemon d{ compressing( ), [&]( daw::mhttpd::request & ) {
				                      return make( );
			                      } };
			auto const r = http( d.port( ), "GET", "/", { }, accept_zstd );
			daw_ensure( r.status == 200 or r.status == 206 );
			daw_ensure( not has_header_named( r, "Content-Encoding" ) );
			daw_ensure( not has_header_named( r, "Vary" ) );
			daw_ensure( r.body == expected );
		}
	}

	void existing_content_encoding_untouched( ) {
		daw::mhttpd::daemon d{ compressing( ), []( daw::mhttpd::request & ) {
			                      return daw::mhttpd::response::from_copy( text_body( ) )
			                        .header( "Content-Type", "text/plain" )
			                        .header( "Content-Encoding", "gzip" );
		                      } };
		auto const r = http( d.port( ), "GET", "/", { }, accept_zstd );
		daw_ensure( r.body == text_body( ) );
		daw_ensure( has_header( r, "Content-Encoding: gzip" ) );
		daw_ensure( not has_header( r, "Content-Encoding: zstd" ) );
	}

	void etag_becomes_weak( ) {
		if( not zstd_built( ) ) {
			return;
		}
		for( auto const &[sent, expected] :
		     { std::pair<std::string, std::string>{ "\"v1\"", "W/\"v1\"" },
		       std::pair<std::string, std::string>{ "W/\"v1\"", "W/\"v1\"" } } ) {
			daw::mhttpd::daemon d{ compressing( ), [&]( daw::mhttpd::request & ) {
				                      return daw::mhttpd::response::from_copy(
				                               text_body( ) )
				                        .header( "Content-Type", "text/plain" )
				                        .header( "ETag", sent );
			                      } };
			auto const compressed = http( d.port( ), "GET", "/", { }, accept_zstd );
			daw_ensure( has_header( compressed, "ETag: " + expected ) );
			// The uncompressed response keeps the tag it was given
			auto const plain = http( d.port( ), "GET", "/" );
			daw_ensure( has_header( plain, "ETag: " + sent ) );
		}
	}

	void vary_merged( ) {
		if( not zstd_built( ) ) {
			return;
		}
		for( auto const &[sent, expected] :
		     { std::pair<std::string, std::string>{ "Origin",
		                                            "Origin, Accept-Encoding" },
		       std::pair<std::string, std::string>{ "Origin, accept-encoding",
		                                            "Origin, accept-encoding" },
		       std::pair<std::string, std::string>{ "*", "*" } } ) {
			daw::mhttpd::daemon d{ compressing( ), [&]( daw::mhttpd::request & ) {
				                      return daw::mhttpd::response::from_copy(
				                               text_body( ) )
				                        .header( "Content-Type", "text/plain" )
				                        .header( "Vary", sent );
			                      } };
			auto const r = http( d.port( ), "GET", "/", { }, accept_zstd );
			daw_ensure( has_header( r, "Vary: " + expected ) );
			daw_ensure( r.headers.find( "Vary:" ) == r.headers.rfind( "Vary:" ) );
		}
	}

	void custom_compressible_predicate( ) {
		using predicate = decltype( daw::mhttpd::compression_config::compressible );
		// It runs on several threads at once, so it must not mutate itself
		auto counting = [n = 0]( daw::string_view ) mutable {
			return ++n > 0;
		};
		static_assert( not std::is_constructible_v<predicate, decltype( counting )> );
		static_assert( std::is_constructible_v<predicate,
		                                       decltype( &daw::mhttpd::is_compressible_type )> );
		static_assert( std::is_copy_constructible_v<daw::mhttpd::config> );

		auto cfg = compressing( );
		cfg.compression.compressible = []( daw::string_view type ) {
			return type == "application/x-custom";
		};
		cfg.compression.min_size = 16;
		daw::mhttpd::daemon d{ cfg, []( daw::mhttpd::request &req ) {
			                      return daw::mhttpd::response::from_copy( text_body( ) )
			                        .header( "Content-Type",
			                                 req.url( ) == "/custom"
			                                   ? "application/x-custom"
			                                   : "text/plain" );
		                      } };
		auto const custom = http( d.port( ), "GET", "/custom", { }, accept_zstd );
		daw_ensure( has_header( custom, "Content-Encoding: zstd" ) == zstd_built( ) );
		auto const text = http( d.port( ), "GET", "/text", { }, accept_zstd );
		daw_ensure( not has_header_named( text, "Content-Encoding" ) );
		daw_ensure( text.body == text_body( ) );
	}

	void no_gain_sends_identity( ) {
		auto noise = std::string( 8192, '\0' );
		auto gen = std::mt19937{ 42 };
		std::ranges::generate( noise, [&] {
			return static_cast<char>( gen( ) );
		} );
		daw::mhttpd::daemon d{ compressing( ), [&]( daw::mhttpd::request & ) {
			                      return daw::mhttpd::response::from_copy( noise )
			                        .header( "Content-Type", "text/plain" );
		                      } };
		auto const r = http( d.port( ), "GET", "/", { }, accept_zstd );
		daw_ensure( r.body == noise );
		daw_ensure( not has_header_named( r, "Content-Encoding" ) );
		daw_ensure( has_header( r, "Vary: Accept-Encoding" ) == zstd_built( ) );
	}

	void response_coding_reported( ) {
		auto const echo = []( daw::mhttpd::request &req ) {
			return daw::mhttpd::response::from_copy(
			  to_string( req.response_coding( ) ) );
		};
		{
			daw::mhttpd::daemon d{ compressing( ), echo };
			daw_ensure( http( d.port( ), "GET", "/", { }, accept_zstd ).body ==
			            ( zstd_built( ) ? "zstd" : "identity" ) );
			daw_ensure( http( d.port( ), "GET", "/" ).body == "identity" );
		}
		{
			daw::mhttpd::daemon d{ { }, echo };
			daw_ensure( http( d.port( ), "GET", "/", { }, accept_zstd ).body ==
			            "identity" );
		}
	}

	void router_responses_compressed( ) {
		auto routes = daw::mhttpd::router{ };
		routes.get( "/a", text_handler( "application/json" ) );
		daw::mhttpd::daemon d{ compressing( ), std::move( routes ) };
		auto const r = http( d.port( ), "GET", "/a", { }, accept_zstd );
		daw_ensure( has_header( r, "Content-Encoding: zstd" ) == zstd_built( ) );
		if( zstd_built( ) ) {
			daw_ensure( unzstd( r.body ) == text_body( ) );
		}
	}
} // namespace

int main( int argc, char **argv ) {
	struct test_case {
		daw::string_view name;
		void ( *fn )( );
	};
	static constexpr test_case tests[] = {
	  { "accepts_coding_parsing", &accepts_coding_parsing },
	  { "compressible_types", &compressible_types },
	  { "compresses_when_accepted", &compresses_when_accepted },
	  { "static_body_compressed", &static_body_compressed },
	  { "static_body_cached", &static_body_cached },
	  { "static_cache_disabled", &static_cache_disabled },
	  { "static_cache_full", &static_cache_full },
	  { "static_bodies_kept_apart", &static_bodies_kept_apart },
	  { "static_no_gain_sends_identity", &static_no_gain_sends_identity },
	  { "identity_without_accept_encoding", &identity_without_accept_encoding },
	  { "disabled_by_default", &disabled_by_default },
	  { "ineligible_responses_untouched", &ineligible_responses_untouched },
	  { "existing_content_encoding_untouched",
	    &existing_content_encoding_untouched },
	  { "etag_becomes_weak", &etag_becomes_weak },
	  { "vary_merged", &vary_merged },
	  { "custom_compressible_predicate", &custom_compressible_predicate },
	  { "no_gain_sends_identity", &no_gain_sends_identity },
	  { "response_coding_reported", &response_coding_reported },
	  { "router_responses_compressed", &router_responses_compressed },
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
