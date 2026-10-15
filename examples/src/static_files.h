// A handler that serves the files under a root folder. A request for a folder
// serves its index.html.
//
// Caching and partial content (RFC 9110):
//   * ETag, Last-Modified and Accept-Ranges on every file response
//   * If-None-Match, or If-Modified-Since when it is absent -> 304
//   * Range with a single byte range -> 206, or 416 when it is out of bounds;
//     other Range forms are ignored and the whole file is sent
//   * If-Range sends the range only while its validator still matches
#pragma once

#include <daw/mhttpd/daw_mhttpd.h>
#include <daw/mhttpd/daw_mhttpd_log.h>

#include <daw/daw_format_string_view.h>
#include <daw/integers/daw_integer_format.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <system_error>
#include <utility>

namespace static_files {
	namespace fs = std::filesystem;

	[[nodiscard]] inline daw::string_view content_type( fs::path const &p ) {
		auto const ext = p.extension( ).string( );
		struct entry {
			daw::string_view ext;
			daw::string_view type;
		};
		static constexpr entry types[] = {
		  { ".html", "text/html; charset=utf-8" },
		  { ".htm", "text/html; charset=utf-8" },
		  { ".css", "text/css; charset=utf-8" },
		  { ".js", "text/javascript; charset=utf-8" },
		  { ".mjs", "text/javascript; charset=utf-8" },
		  { ".json", "application/json" },
		  { ".txt", "text/plain; charset=utf-8" },
		  { ".xml", "application/xml" },
		  { ".svg", "image/svg+xml" },
		  { ".png", "image/png" },
		  { ".jpg", "image/jpeg" },
		  { ".jpeg", "image/jpeg" },
		  { ".gif", "image/gif" },
		  { ".webp", "image/webp" },
		  { ".ico", "image/x-icon" },
		  { ".wasm", "application/wasm" },
		  { ".pdf", "application/pdf" },
		};
		for( auto const &e : types ) {
			if( e.ext == ext ) {
				return e.type;
			}
		}
		return "application/octet-stream";
	}

	// Maps a URL path onto `root`. Returns nullopt if the result, after
	// resolving "..", "." and symlinks, is not inside `root`. `root` must be
	// canonical.
	[[nodiscard]] inline std::optional<fs::path> resolve( fs::path const &root,
	                                                      daw::string_view url ) {
		// MHD has already percent-decoded the path, so "%2e%2e" arrives as ".."
		auto const relative = fs::path( std::string{ url } ).relative_path( );
		auto ec = std::error_code{ };
		auto full = fs::weakly_canonical( root / relative, ec );
		if( ec ) {
			return std::nullopt;
		}
		auto const [root_end, _] =
		  std::mismatch( root.begin( ), root.end( ), full.begin( ), full.end( ) );
		if( root_end != root.end( ) ) {
			return std::nullopt;
		}
		return full;
	}

	namespace detail {
		// A non-empty run of digits and nothing else
		[[nodiscard]] inline std::optional<std::uint64_t>
		parse_uint( daw::string_view s ) {
			auto value = std::uint64_t{ };
			auto const *last = s.data( ) + s.size( );
			auto [p, ec] = std::from_chars( s.data( ), last, value );
			if( s.empty( ) or ec != std::errc{ } or p != last ) {
				return std::nullopt;
			}
			return value;
		}

		constexpr auto day_names =
		  std::array{ "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
		constexpr auto month_names = std::array{ "Jan",
		                                         "Feb",
		                                         "Mar",
		                                         "Apr",
		                                         "May",
		                                         "Jun",
		                                         "Jul",
		                                         "Aug",
		                                         "Sep",
		                                         "Oct",
		                                         "Nov",
		                                         "Dec" };

		// IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT"; independent of the
		// C locale
		[[nodiscard]] inline std::string format_http_date( std::time_t t ) {
			auto tm = std::tm{ };
			::gmtime_r( &t, &tm );
			return std::format( "{}, {:02} {} {:04} {:02}:{:02}:{:02} GMT",
			                    day_names[static_cast<std::size_t>( tm.tm_wday )],
			                    tm.tm_mday,
			                    month_names[static_cast<std::size_t>( tm.tm_mon )],
			                    tm.tm_year + 1900,
			                    tm.tm_hour,
			                    tm.tm_min,
			                    tm.tm_sec );
		}

		// Only IMF-fixdate; the obsolete RFC 850 and asctime forms are treated as
		// unparseable, which just means the condition is ignored
		[[nodiscard]] inline std::optional<std::time_t>
		parse_http_date( daw::string_view s ) {
			s.trim( );
			// "Sun, 06 Nov 1994 08:49:37 GMT"
			if( s.size( ) != 29 or s[3] != ',' or s[4] != ' ' or s[7] != ' ' or
			    s[11] != ' ' or s[16] != ' ' or s[19] != ':' or s[22] != ':' or
			    s.substr( 25 ) != " GMT" ) {
				return std::nullopt;
			}
			auto const num = [&]( std::size_t pos, std::size_t len ) {
				return parse_uint( s.substr( pos, len ) );
			};
			auto const month = std::ranges::find( month_names, s.substr( 8, 3 ) );
			auto const day = num( 5, 2 );
			auto const year = num( 12, 4 );
			auto const hour = num( 17, 2 );
			auto const minute = num( 20, 2 );
			auto const second = num( 23, 2 );
			if( month == month_names.end( ) or not day or not year or not hour or
			    not minute or not second ) {
				return std::nullopt;
			}
			auto tm = std::tm{ };
			tm.tm_year = static_cast<int>( *year ) - 1900;
			tm.tm_mon = static_cast<int>( month - month_names.begin( ) );
			tm.tm_mday = static_cast<int>( *day );
			tm.tm_hour = static_cast<int>( *hour );
			tm.tm_min = static_cast<int>( *minute );
			tm.tm_sec = static_cast<int>( *second );
			return ::timegm( &tm );
		}

		// 64-bit FNV-1a over the little-endian bytes of each value added, so the
		// result does not depend on the host's byte order. The multiply relies on
		// wrapping, hence mul_wrapped.
		class fnv1a64 {
			daw::u64 m_hash = daw::u64( 0xcbf2'9ce4'8422'2325U );

		public:
			constexpr fnv1a64 &add( daw::u64 value ) {
				constexpr auto prime = daw::u64( 0x0000'0100'0000'01b3U );
				for( auto shift = 0U; shift < 64U; shift += 8U ) {
					m_hash ^= ( value >> shift ) & 0xFFU;
					m_hash = m_hash.mul_wrapped( prime );
				}
				return *this;
			}

			[[nodiscard]] constexpr daw::u64 value( ) const {
				return m_hash;
			}
		};

		// Strong validator: a hash of inode, size and modification time, so a
		// replaced or modified file gets a new tag without the tag exposing the
		// inode. The seed is hashed first; servers that share a seed produce the
		// same tags for the same file metadata.
		[[nodiscard]] inline std::string make_etag( struct stat const &st,
		                                            daw::u64 seed ) {
			auto const hash = fnv1a64{ }
			                    .add( seed )
			                    .add( daw::u64( st.st_ino ) )
			                    .add( daw::i64( st.st_size ).as_unsigned( ) )
			                    .add( daw::i64( st.st_mtim.tv_sec ).as_unsigned( ) )
			                    .add( daw::i64( st.st_mtim.tv_nsec ).as_unsigned( ) )
			                    .value( );
			return std::format( "\"{:016x}\"", hash );
		}

		// If-None-Match: "*" or a list of entity tags, compared weakly (a W/
		// prefix is ignored on both sides)
		[[nodiscard]] inline bool none_match_hits( daw::string_view header,
		                                           daw::string_view etag ) {
			header.trim( );
			if( header == "*" ) {
				return true;
			}
			while( not header.empty( ) ) {
				auto tag = header.pop_front_until( ',' ).trim_copy( );
				if( tag.starts_with( "W/" ) ) {
					tag.remove_prefix( 2 );
				}
				if( tag == etag ) {
					return true;
				}
			}
			return false;
		}

		// If-Range holds one validator, compared strongly: a weak tag never
		// matches, a date must equal Last-Modified exactly
		[[nodiscard]] inline bool
		if_range_matches( daw::string_view header, daw::string_view etag,
		                  daw::string_view last_modified ) {
			header.trim( );
			if( header.starts_with( "W/" ) ) {
				return false;
			}
			if( header.starts_with( "\"" ) ) {
				return header == etag;
			}
			return header == last_modified;
		}

		struct byte_range {
			std::uint64_t first;
			std::uint64_t length;
		};

		enum class range_result { ignore, unsatisfiable, satisfiable };

		// A single "bytes=" range. Malformed headers, other units and multiple
		// ranges are ignored, which RFC 9110 allows.
		[[nodiscard]] inline std::pair<range_result, byte_range>
		parse_range( daw::string_view header, std::uint64_t size ) {
			constexpr auto ignore = std::pair{ range_result::ignore, byte_range{ } };
			constexpr auto unsatisfiable =
			  std::pair{ range_result::unsatisfiable, byte_range{ } };
			// pop_front_until consumes everything when the delimiter is missing,
			// so check for it first: "bytes=1" and "bytes=1-" differ
			if( header.find( '=' ) == daw::string_view::npos ) {
				return ignore;
			}
			// The range unit is case-insensitive
			auto const unit = header.pop_front_until( '=' ).trim_copy( );
			if( unit.size( ) != 5 or
			    not std::ranges::equal(
			      unit, daw::string_view{ "bytes" }, []( char a, char b ) {
				      return ( a | 0x20 ) == b;
			      } ) ) {
				return ignore;
			}
			auto spec = header.trim_copy( );
			if( spec.find( ',' ) != daw::string_view::npos or
			    spec.find( '-' ) == daw::string_view::npos ) {
				return ignore;
			}
			auto const first_text = spec.pop_front_until( '-' ).trim_copy( );
			auto const last_text = spec.trim_copy( );

			if( first_text.empty( ) ) { // suffix: the last N bytes
				auto const n = parse_uint( last_text );
				if( not n ) {
					return ignore;
				}
				if( *n == 0 or size == 0 ) {
					return unsatisfiable;
				}
				auto const length = std::min( *n, size );
				return { range_result::satisfiable, { size - length, length } };
			}
			auto const first = parse_uint( first_text );
			if( not first ) {
				return ignore;
			}
			auto last = size == 0 ? 0 : size - 1;
			if( not last_text.empty( ) ) {
				auto const l = parse_uint( last_text );
				if( not l or *l < *first ) {
					return ignore;
				}
				last = std::min( *l, last );
			}
			if( *first >= size ) {
				return unsatisfiable;
			}
			return { range_result::satisfiable, { *first, last - *first + 1 } };
		}
	} // namespace detail

	class static_file_handler {
		fs::path m_root;
		daw::u64 m_seed;

		static daw::mhttpd::response not_found( ) {
			return daw::mhttpd::response::from_static( "Not Found\n", 404 )
			  .header( "Content-Type", "text/plain; charset=utf-8" );
		}

	public:
		// `seed` keys the ETag hash; give every server behind a load balancer the
		// same seed so they agree on tags
		explicit static_file_handler( fs::path const &root, daw::u64 seed = 0U )
		  : m_root{ fs::canonical( root ) }
		  , m_seed{ seed } {}

		[[nodiscard]] fs::path const &root( ) const noexcept {
			return m_root;
		}

		daw::mhttpd::response operator( )( daw::mhttpd::request &req ) const {
			auto res = respond( req );
			auto &logger = daw::mhttpd::log::get( daw::mhttpd::log::component::static_files );
			logger.debug( "{} {} -> {}", req.method_name( ), req.url( ), res.status( ) );
			return res;
		}

	private:
		daw::mhttpd::response respond( daw::mhttpd::request &req ) const {
			using daw::mhttpd::http_method;
			using daw::mhttpd::response;
			switch( req.method( ) ) {
			case http_method::GET:
			case http_method::HEAD: // MHD sends the headers and leaves out the body
				break;
			case http_method::UNKNOWN:
				return response::empty( 501 );
			default:
				return response::empty( 405 ).header( "Allow", "GET, HEAD" );
			}

			auto path = resolve( m_root, req.url( ) );
			if( not path ) {
				daw::mhttpd::log::get( daw::mhttpd::log::component::static_files )
				  .debug( "{} is outside the root or not a valid path", req.url( ) );
				return not_found( ); // do not reveal that the path exists outside root
			}
			auto ec = std::error_code{ };
			if( fs::is_directory( *path, ec ) ) {
				auto const url = req.url( );
				if( url.empty( ) or url.back( ) != '/' ) {
					// Relative links in the index only work under the trailing slash
					return response::empty( 301 ).header( "Location",
					                                      std::format( "{}/", url ) );
				}
				*path /= "index.html";
			}

			// Closed automatically on every path that does not hand it to a
			// response
			auto fd = daw::mhttpd::unique_fd{
			  ::open( path->c_str( ), O_RDONLY | O_CLOEXEC ) };
			if( not fd ) {
				return not_found( );
			}
			struct stat st{ };
			if( ::fstat( fd.get( ), &st ) != 0 or not S_ISREG( st.st_mode ) ) {
				return not_found( );
			}
			auto const size = static_cast<std::uint64_t>( st.st_size );
			auto const etag = detail::make_etag( st, m_seed );
			auto const last_modified = detail::format_http_date( st.st_mtim.tv_sec );
			auto const with_validators = [&]( response r ) {
				return std::move( r )
				  .header( "ETag", etag )
				  .header( "Last-Modified", last_modified );
			};

			// Conditional requests come before Range (RFC 9110 section 13.2.2)
			auto not_modified = false;
			if( auto inm = req.header( "If-None-Match" ) ) {
				not_modified = detail::none_match_hits( *inm, etag );
			} else if( auto ims = req.header( "If-Modified-Since" ) ) {
				auto const since = detail::parse_http_date( *ims );
				not_modified = since and st.st_mtim.tv_sec <= *since;
			}
			if( not_modified ) {
				// MHD leaves out the body of a 304 but sends the file's
				// Content-Length
				return with_validators(
				  response::from_fd( std::move( fd ), 0U, size, 304 ) );
			}

			// from_fd owns fd and lets MHD use sendfile
			auto const range_header = req.method( ) == http_method::GET
			                            ? req.header( "Range" )
			                            : std::nullopt;
			auto const if_range = req.header( "If-Range" );
			if( range_header and
			    ( not if_range or
			      detail::if_range_matches( *if_range, etag, last_modified ) ) ) {
				auto const [result, range] = detail::parse_range( *range_header, size );
				if( result == detail::range_result::unsatisfiable ) {
					return response::empty( 416 ).header(
					  "Content-Range", std::format( "bytes */{}", size ) );
				}
				if( result == detail::range_result::satisfiable ) {
					auto const content_range =
					  std::format( "bytes {}-{}/{}",
					               range.first,
					               range.first + range.length - 1,
					               size );
					return with_validators(
					         response::from_fd(
					           std::move( fd ), range.first, range.length, 206 ) )
					  .header( "Content-Range", content_range )
					  .header( "Content-Type", content_type( *path ) )
					  .header( "Accept-Ranges", "bytes" );
				}
			}
			return with_validators( response::from_fd( std::move( fd ), 0U, size ) )
			  .header( "Content-Type", content_type( *path ) )
			  .header( "Accept-Ranges", "bytes" );
		}
	};
} // namespace static_files
