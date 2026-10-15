#include "daw_mhttpd_compression.h"

#include "daw/mhttpd/daw_mhttpd.h"

#include <daw/daw_as.h>

#include <algorithm>
#include <memory>
#include <string_view>

#if defined( DAW_MHTTPD_HAS_ZSTD )
#include <zstd.h>
#endif

namespace daw::mhttpd {
	namespace {
		[[nodiscard]] constexpr char ascii_lower( char c ) noexcept {
			return ( c >= 'A' and c <= 'Z' ) ? as<char>( c - 'A' + 'a' ) : c;
		}

		[[nodiscard]] constexpr bool iequals( std::string_view a,
		                                      std::string_view b ) noexcept {
			return std::ranges::equal( a, b, []( char x, char y ) {
				return ascii_lower( x ) == ascii_lower( y );
			} );
		}

		[[nodiscard]] constexpr bool iends_with( std::string_view s,
		                                         std::string_view suffix ) noexcept {
			return s.size( ) >= suffix.size( ) and
			       iequals( s.substr( s.size( ) - suffix.size( ) ), suffix );
		}

		[[nodiscard]] constexpr std::string_view trim( std::string_view s ) noexcept {
			auto const is_ows = []( char c ) {
				return c == ' ' or c == '\t';
			};
			while( not s.empty( ) and is_ows( s.front( ) ) ) {
				s.remove_prefix( 1 );
			}
			while( not s.empty( ) and is_ows( s.back( ) ) ) {
				s.remove_suffix( 1 );
			}
			return s;
		}

		// qvalue = ( "0" [ "." 0*3DIGIT ] ) / ( "1" [ "." 0*3("0") ] ); a
		// malformed one counts as zero
		[[nodiscard]] constexpr bool qvalue_is_nonzero( std::string_view q ) noexcept {
			if( q.empty( ) or q.size( ) > 5 or ( q[0] != '0' and q[0] != '1' ) ) {
				return false;
			}
			auto const fraction = q.substr( 1 );
			if( not fraction.empty( ) ) {
				if( fraction[0] != '.' or
				    not std::ranges::all_of( fraction.substr( 1 ), []( char c ) {
					    return c >= '0' and c <= '9';
				    } ) ) {
					return false;
				}
			}
			if( q[0] == '1' ) {
				return fraction.size( ) <= 1 or
				       std::ranges::all_of( fraction.substr( 1 ), []( char c ) {
					       return c == '0';
				       } );
			}
			return fraction.size( ) > 1 and
			       std::ranges::any_of( fraction.substr( 1 ), []( char c ) {
				       return c != '0';
			       } );
		}

		struct accept_element {
			std::string_view coding;
			bool acceptable;
		};

		// Calls `f` with each element of an Accept-Encoding value
		template<typename F>
		constexpr void for_each_element( std::string_view header, F &&f ) {
			while( not header.empty( ) ) {
				auto const comma = header.find( ',' );
				auto element = trim( header.substr( 0, comma ) );
				header = comma == std::string_view::npos ? std::string_view{ }
				                                         : header.substr( comma + 1 );
				if( element.empty( ) ) {
					continue;
				}
				auto const semi = element.find( ';' );
				auto const coding = trim( element.substr( 0, semi ) );
				auto acceptable = true;
				auto params = semi == std::string_view::npos
				                ? std::string_view{ }
				                : element.substr( semi + 1 );
				while( not params.empty( ) ) {
					auto const next = params.find( ';' );
					auto const param = trim( params.substr( 0, next ) );
					params = next == std::string_view::npos ? std::string_view{ }
					                                        : params.substr( next + 1 );
					if( param.size( ) >= 2 and ascii_lower( param[0] ) == 'q' ) {
						auto const value = trim( param.substr( 1 ) );
						if( value.starts_with( '=' ) ) {
							acceptable = qvalue_is_nonzero( trim( value.substr( 1 ) ) );
						}
					}
				}
				f( accept_element{ coding, acceptable } );
			}
		}
	} // namespace

	bool coding_available( content_coding c ) noexcept {
		switch( c ) {
		case content_coding::identity:
			return true;
		case content_coding::zstd:
#if defined( DAW_MHTTPD_HAS_ZSTD )
			return true;
#else
			return false;
#endif
		}
		return false;
	}

	bool accepts_coding( daw::string_view accept_encoding,
	                     content_coding c ) noexcept {
		auto const name = std::string_view{ to_string( c ).data( ),
		                                    to_string( c ).size( ) };
		auto listed = std::optional<bool>{ };
		auto star = std::optional<bool>{ };
		for_each_element(
		  std::string_view{ accept_encoding.data( ), accept_encoding.size( ) },
		  [&]( accept_element const &e ) {
			  if( iequals( e.coding, name ) ) {
				  listed = listed.value_or( false ) or e.acceptable;
			  } else if( e.coding == "*" ) {
				  star = star.value_or( false ) or e.acceptable;
			  }
		  } );
		if( listed ) {
			return *listed;
		}
		if( c == content_coding::identity ) {
			return star.value_or( true );
		}
		return star.value_or( false );
	}

	bool is_compressible_type( daw::string_view content_type ) noexcept {
		auto type = std::string_view{ content_type.data( ), content_type.size( ) };
		type = trim( type.substr( 0, type.find( ';' ) ) );
		auto const slash = type.find( '/' );
		if( slash == std::string_view::npos ) {
			return false;
		}
		if( iequals( type.substr( 0, slash ), "text" ) ) {
			return true;
		}
		if( iends_with( type, "+json" ) or iends_with( type, "+xml" ) ) {
			return true;
		}
		static constexpr std::string_view types[] = {
		  "application/json",       "application/javascript",
		  "application/xml",        "application/wasm",
		  "application/x-ndjson",   "application/ecmascript",
		  "application/x-javascript", "font/ttf",
		  "font/otf",               "application/vnd.ms-fontobject" };
		return std::ranges::any_of( types, [&]( std::string_view t ) {
			return iequals( type, t );
		} );
	}
} // namespace daw::mhttpd

namespace daw::mhttpd::detail {
#if defined( DAW_MHTTPD_HAS_ZSTD )
	namespace {
		// RFC 9659 section 3: at most an 8 MiB window
		constexpr int max_window_log = 23;

		struct cctx_deleter {
			void operator( )( ZSTD_CCtx *c ) const noexcept {
				ZSTD_freeCCtx( c );
			}
		};

		// One context per thread, reused for every response it compresses
		[[nodiscard]] ZSTD_CCtx *thread_cctx( ) {
			thread_local auto cctx =
			  std::unique_ptr<ZSTD_CCtx, cctx_deleter>{ ZSTD_createCCtx( ) };
			return cctx.get( );
		}
	} // namespace

	std::optional<std::vector<std::byte>>
	zstd_compress( std::span<std::byte const> in, int level ) {
		auto *cctx = thread_cctx( );
		if( cctx == nullptr ) {
			return std::nullopt;
		}
		(void)ZSTD_CCtx_reset( cctx, ZSTD_reset_session_and_parameters );
		if( ZSTD_isError(
		      ZSTD_CCtx_setParameter( cctx, ZSTD_c_compressionLevel, level ) ) ) {
			return std::nullopt;
		}
		// Only levels 20 to 22 use a larger window (zstd's lib/compress/
		// zstd_clevels.h); setting it for the others would enlarge theirs
		if( level > 19 ) {
			if( ZSTD_isError( ZSTD_CCtx_setParameter(
			      cctx, ZSTD_c_windowLog, max_window_log ) ) ) {
				return std::nullopt;
			}
		}
		auto out = std::vector<std::byte>( ZSTD_compressBound( in.size( ) ) );
		auto const n =
		  ZSTD_compress2( cctx, out.data( ), out.size( ), in.data( ), in.size( ) );
		if( ZSTD_isError( n ) ) {
			return std::nullopt;
		}
		out.resize( n );
		// The bound is about the input's size; the response holds this buffer
		// until it is sent
		out.shrink_to_fit( );
		return out;
	}
#else
	std::optional<std::vector<std::byte>>
	zstd_compress( std::span<std::byte const>, int ) {
		return std::nullopt;
	}
#endif
} // namespace daw::mhttpd::detail
