// Serves a temporary folder with the static_files example handler
#include "http_client.hpp"
#include "static_files.h"

#include <daw/daw_ensure.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using daw_mhttpd_test::http;

namespace {
	void write_file( fs::path const &p, std::string const &contents ) {
		auto out = std::ofstream( p, std::ios::binary );
		out << contents;
	}

	std::string header_value( daw_mhttpd_test::http_result const &r,
	                          std::string const &name ) {
		auto const key = std::format( "\r\n{}: ", name );
		auto const pos = r.headers.find( key );
		if( pos == std::string::npos ) {
			return { };
		}
		auto const start = pos + key.size( );
		return r.headers.substr( start, r.headers.find( "\r\n", start ) - start );
	}

	bool has_header( daw_mhttpd_test::http_result const &r,
	                 std::string const &line ) {
		return r.headers.find( std::format( "\r\n{}\r\n", line ) ) !=
		         std::string::npos or
		       r.headers.ends_with( std::format( "\r\n{}", line ) );
	}

	// tmp/www is served; tmp/secret.txt sits next to it and must stay private
	struct temp_site {
		fs::path dir;

		temp_site( ) {
			auto tmpl =
			  ( fs::temp_directory_path( ) / "daw_mhttpd_XXXXXX" ).string( );
			daw_ensure( ::mkdtemp( tmpl.data( ) ) != nullptr );
			dir = tmpl;
			fs::create_directories( dir / "www" / "sub" );
			write_file( dir / "www" / "index.html", "<h1>home</h1>" );
			write_file( dir / "www" / "style.css", "body{}" );
			write_file( dir / "www" / "digits.txt", "0123456789" );
			write_file( dir / "www" / "sub" / "index.html", "<h1>sub</h1>" );
			write_file( dir / "secret.txt", "secret" );
			fs::create_symlink( dir / "secret.txt", dir / "www" / "link.txt" );
		}

		~temp_site( ) {
			auto ec = std::error_code{ };
			fs::remove_all( dir, ec );
		}
	};
} // namespace

// Reference values computed independently (Python) over the little-endian
// bytes of each value
static_assert( static_files::detail::fnv1a64{ }.value( ) ==
               0xcbf2'9ce4'8422'2325U );
static_assert( static_files::detail::fnv1a64{ }.add( 0x61U ).value( ) ==
               0x6926'124a'7b14'33c4U );
static_assert(
  static_files::detail::fnv1a64{ }.add( 1U ).add( 2U ).add( 3U ).value( ) ==
  0xda2b'fb22'5e0d'1f05U );

int main( ) {
	auto site = temp_site{ };
	auto d =
	  daw::mhttpd::daemon{ { }, static_files::static_file_handler{ site.dir / "www" } };
	auto const port = d.port( );

	// The default document
	auto r = http( port, "GET", "/" );
	daw_ensure( r.status == 200 );
	daw_ensure( r.body == "<h1>home</h1>" );
	daw_ensure( has_header( r, "Content-Type: text/html; charset=utf-8" ) );

	r = http( port, "GET", "/style.css" );
	daw_ensure( r.status == 200 );
	daw_ensure( r.body == "body{}" );
	daw_ensure( has_header( r, "Content-Type: text/css; charset=utf-8" ) );

	// Folders: redirect to the trailing slash, then serve their index.html
	r = http( port, "GET", "/sub" );
	daw_ensure( r.status == 301 );
	daw_ensure( has_header( r, "Location: /sub/" ) );
	r = http( port, "GET", "/sub/" );
	daw_ensure( r.status == 200 );
	daw_ensure( r.body == "<h1>sub</h1>" );

	daw_ensure( http( port, "GET", "/missing.html" ).status == 404 );

	// Nothing outside the root, however it is spelled
	for( auto const *path : { "/../secret.txt",
	                          "/sub/../../secret.txt",
	                          "/%2e%2e/secret.txt",
	                          "/%2E%2E%2Fsecret.txt",
	                          "/link.txt" } ) {
		r = http( port, "GET", path );
		daw_ensure( r.status == 404 );
		daw_ensure( r.body.find( "secret" ) == std::string::npos );
	}

	// HEAD: same headers, no body
	r = http( port, "HEAD", "/" );
	daw_ensure( r.status == 200 );
	daw_ensure( r.body.empty( ) );
	daw_ensure( has_header( r, "Content-Length: 13" ) );

	// ---------------------------------------------------------------------
	// Validators and conditional requests
	// ---------------------------------------------------------------------
	r = http( port, "GET", "/" );
	auto const etag = header_value( r, "ETag" );
	auto const last_modified = header_value( r, "Last-Modified" );
	daw_ensure( etag.size( ) == 18 and etag.front( ) == '"' and
	            etag.back( ) == '"' );
	daw_ensure( last_modified.ends_with( " GMT" ) );
	daw_ensure( has_header( r, "Accept-Ranges: bytes" ) );
	// Stable for an unchanged file, different for a different file
	daw_ensure( header_value( http( port, "GET", "/" ), "ETag" ) == etag );
	daw_ensure( header_value( http( port, "GET", "/style.css" ), "ETag" ) !=
	            etag );
	// Servers sharing a seed agree on tags; a different seed gives different ones
	{
		auto const etag_with_seed = [&]( daw::u64 seed ) {
			auto other = daw::mhttpd::daemon{
			  { }, static_files::static_file_handler{ site.dir / "www", seed } };
			return header_value( http( other.port( ), "GET", "/" ), "ETag" );
		};
		daw_ensure( etag_with_seed( 0U ) == etag );
		daw_ensure( etag_with_seed( 42U ) != etag );
		daw_ensure( etag_with_seed( 42U ) == etag_with_seed( 42U ) );
	}

	auto const if_none_match = [&]( std::string const &value ) {
		return http( port, "GET", "/", { }, std::format( "If-None-Match: {}\r\n", value ) );
	};
	r = if_none_match( etag );
	daw_ensure( r.status == 304 );
	daw_ensure( r.body.empty( ) );
	daw_ensure( header_value( r, "ETag" ) == etag );
	daw_ensure( has_header( r, "Content-Length: 13" ) );      // the file's length
	daw_ensure( if_none_match( std::format( "W/{}", etag ) ).status == 304 ); // weak compare
	daw_ensure( if_none_match( std::format( "\"x\", {}", etag ) ).status == 304 ); // list
	daw_ensure( if_none_match( "*" ).status == 304 );
	daw_ensure( if_none_match( "\"x\"" ).status == 200 );

	auto const if_modified_since = [&]( std::string const &value,
	                                    std::string const &extra = { } ) {
		return http(
		  port, "GET", "/", { }, std::format( "If-Modified-Since: {}\r\n{}", value, extra ) );
	};
	daw_ensure( if_modified_since( last_modified ).status == 304 );
	daw_ensure( if_modified_since( "Sun, 06 Nov 1994 08:49:37 GMT" ).status ==
	            200 );
	daw_ensure( if_modified_since( "not a date" ).status == 200 );
	// If-None-Match wins over If-Modified-Since
	daw_ensure(
	  if_modified_since( last_modified, "If-None-Match: \"x\"\r\n" ).status ==
	  200 );

	// ---------------------------------------------------------------------
	// Range
	// ---------------------------------------------------------------------
	auto const range = [&]( std::string const &value,
	                        std::string const &extra = { },
	                        std::string const &method = "GET" ) {
		return http(
		  port, method, "/digits.txt", { }, std::format( "Range: {}\r\n{}", value, extra ) );
	};
	auto const expect_partial = [&]( daw_mhttpd_test::http_result const &res,
	                                 std::string const &body,
	                                 std::string const &content_range ) {
		daw_ensure( res.status == 206 );
		daw_ensure( res.body == body );
		daw_ensure( header_value( res, "Content-Range" ) == content_range );
		daw_ensure( header_value( res, "Content-Length" ) ==
		            std::format( "{}", body.size( ) ) );
	};
	expect_partial( range( "bytes=2-5" ), "2345", "bytes 2-5/10" );
	expect_partial( range( "bytes=7-" ), "789", "bytes 7-9/10" );
	expect_partial( range( "bytes=-3" ), "789", "bytes 7-9/10" );
	expect_partial( range( "bytes=-100" ), "0123456789", "bytes 0-9/10" );
	expect_partial( range( "bytes=5-100" ), "56789", "bytes 5-9/10" );
	expect_partial( range( "BYTES = 0-0" ), "0", "bytes 0-0/10" );

	for( auto const *bad : { "bytes=10-", "bytes=-0" } ) {
		r = range( bad );
		daw_ensure( r.status == 416 );
		daw_ensure( header_value( r, "Content-Range" ) == "bytes */10" );
	}
	// Ignored: malformed, other units, several ranges
	for( auto const *ignored :
	     { "bytes=5-2", "bytes=x-3", "bytes=1", "items=0-1", "bytes=0-1,4-5" } ) {
		r = range( ignored );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "0123456789" );
	}
	// Range only applies to GET
	r = range( "bytes=2-5", { }, "HEAD" );
	daw_ensure( r.status == 200 );
	daw_ensure( r.body.empty( ) );

	// If-Range: the range only while the validator still matches
	auto const digits_etag =
	  header_value( http( port, "GET", "/digits.txt" ), "ETag" );
	auto const digits_date =
	  header_value( http( port, "GET", "/digits.txt" ), "Last-Modified" );
	expect_partial( range( "bytes=0-1", std::format( "If-Range: {}\r\n", digits_etag ) ),
	                "01",
	                "bytes 0-1/10" );
	expect_partial( range( "bytes=0-1", std::format( "If-Range: {}\r\n", digits_date ) ),
	                "01",
	                "bytes 0-1/10" );
	for( auto const &stale :
	     { std::string{ "\"x\"" },
	       std::format( "W/{}", digits_etag ),
	       std::string{ "Sun, 06 Nov 1994 08:49:37 GMT" } } ) {
		r = range( "bytes=0-1", std::format( "If-Range: {}\r\n", stale ) );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "0123456789" );
	}
	// A matching If-None-Match wins over Range
	daw_ensure(
	  range( "bytes=0-1", std::format( "If-None-Match: {}\r\n", digits_etag ) ).status ==
	  304 );

	r = http( port, "POST", "/", "x" );
	daw_ensure( r.status == 405 );
	daw_ensure( has_header( r, "Allow: GET, HEAD" ) );
	daw_ensure( http( port, "PROPFIND", "/" ).status == 501 );
}
