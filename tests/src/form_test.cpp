#include "daw/mhttpd/daw_mhttpd.h"
#include "http_client.hpp"

#include <daw/daw_ensure.h>
#include <daw/daw_string_view.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
	using namespace daw_mhttpd_test;

	constexpr char const boundary[] = "XyZzY-boundary";

	struct part {
		std::string name;
		std::string value;
		std::string filename{ }; // empty: a plain field
		std::string content_type{ };
	};

	[[nodiscard]] std::string multipart_body( std::vector<part> const &parts,
	                                          bool close = true ) {
		auto body = std::string{ };
		for( auto const &p : parts ) {
			body += "--";
			body += boundary;
			body += "\r\nContent-Disposition: form-data; name=\"" + p.name + "\"";
			if( not p.filename.empty( ) ) {
				body += "; filename=\"" + p.filename + "\"";
			}
			body += "\r\n";
			if( not p.content_type.empty( ) ) {
				body += "Content-Type: " + p.content_type + "\r\n";
			}
			body += "\r\n" + p.value + "\r\n";
		}
		if( close ) {
			body += "--";
			body += boundary;
			body += "--\r\n";
		}
		return body;
	}

	[[nodiscard]] std::string multipart_header( ) {
		return std::string{ "Content-Type: multipart/form-data; boundary=" } +
		       boundary + "\r\n";
	}

	// Lists the parsed fields as name[filename,type]=value lines
	[[nodiscard]] std::string describe( daw::mhttpd::form_data const &form ) {
		auto out = std::string{ };
		for( auto const &f : form.fields( ) ) {
			out += f.name;
			if( f.filename or f.content_type ) {
				out += "[" + f.filename.value_or( "-" ) + "," +
				       f.content_type.value_or( "-" ) + "]";
			}
			out += "=" + f.value + "\n";
		}
		return out;
	}

	auto const parse_and_describe = []( daw::mhttpd::request &req ) {
		try {
			return daw::mhttpd::response::from_owned(
			  describe( daw::mhttpd::parse_form( req ) ) );
		} catch( daw::mhttpd::form_error const &e ) {
			return daw::mhttpd::response::from_copy( daw::string_view{ e.what( ) },
			                                         400 );
		}
	};

	void form_urlencoded( ) {
		daw::mhttpd::daemon d{ { }, parse_and_describe };
		auto r = http( d.port( ),
		               "POST",
		               "/",
		               "a=1&b=hello+world&c=%26%3D&empty=&a=2",
		               "Content-Type: application/x-www-form-urlencoded\r\n" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "a=1\nb=hello world\nc=&=\nempty=\na=2\n" );
	}

	void form_multipart( ) {
		daw::mhttpd::daemon d{ { }, parse_and_describe };
		// Binary data that looks like the start of a boundary
		auto file = std::string{ "PK\x03\x04\r\n--XyZz\r\n-" };
		file.push_back( '\0' );
		file += "end";
		auto const body = multipart_body(
		  { { .name = "name", .value = "My World" },
		    { .name = "file",
		      .value = file,
		      .filename = "w.zip",
		      .content_type = "application/zip" },
		    { .name = "empty", .value = "" } } );
		auto r = http( d.port( ), "POST", "/", body, multipart_header( ) );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "name=My World\nfile[w.zip,application/zip]=" +
		                        file + "\nempty=\n" );
	}

	void form_find( ) {
		auto form = daw::mhttpd::form_data{
		  { { .name = "a", .value = "1" }, { .name = "a", .value = "2" } } };
		daw_ensure( form.find( "a" ) != nullptr );
		daw_ensure( form.find( "a" )->value == "1" );
		daw_ensure( form.find( "b" ) == nullptr );
	}

	void form_not_a_form( ) {
		daw::mhttpd::daemon d{ { }, parse_and_describe };
		daw_ensure( http( d.port( ), "POST", "/", "a=1", "Content-Type: text/plain\r\n" )
		              .status == 400 );
		daw_ensure( http( d.port( ), "POST", "/", "a=1" ).status == 400 );
	}

	void form_truncated_multipart( ) {
		daw::mhttpd::daemon d{ { }, parse_and_describe };
		auto const body =
		  multipart_body( { { .name = "name", .value = "x" } }, false );
		auto r = http( d.port( ), "POST", "/", body, multipart_header( ) );
		daw_ensure( r.status == 400 );
	}

	void form_handler_exception_propagates( ) {
		daw::mhttpd::daemon d{
		  { }, []( daw::mhttpd::request &req ) {
			  auto parser =
			    daw::mhttpd::form_parser{ req, []( daw::mhttpd::form_chunk const & ) {
				                             throw std::logic_error{ "stop" };
			                             } };
			  try {
				  parser.feed( req.body( ) );
				  parser.finish( );
			  } catch( std::logic_error const & ) {
				  return daw::mhttpd::response::empty( 422 );
			  }
			  return daw::mhttpd::response::empty( 200 );
		  } };
		auto const body = multipart_body( { { .name = "name", .value = "x" } } );
		daw_ensure( http( d.port( ), "POST", "/", body, multipart_header( ) ).status ==
		            422 );
	}

	// A large upload streamed through form_parser in a coroutine route, past
	// max_body_size, as a world upload would be
	void form_streamed_upload( ) {
		auto routes = daw::mhttpd::router{ };
		routes.post(
		  "/worlds",
		  []( daw::mhttpd::request &req ) -> daw::mhttpd::task<daw::mhttpd::response> {
			  auto name = std::string{ };
			  auto file_size = std::size_t{ 0 };
			  auto contiguous = true;
			  auto filename = std::string{ };
			  auto parser = daw::mhttpd::form_parser{
			    req, [&]( daw::mhttpd::form_chunk const &chunk ) {
				    if( chunk.name == "name" ) {
					    name.append(
					      reinterpret_cast<char const *>( chunk.data.data( ) ),
					      chunk.data.size( ) );
				    } else if( chunk.name == "file" ) {
					    contiguous = contiguous and chunk.offset == file_size;
					    file_size += chunk.data.size( );
					    filename = std::string{ chunk.filename.value_or( "" ) };
				    }
			    } };
			  while( auto chunk = co_await req.next_chunk( ) ) {
				  parser.feed( *chunk );
			  }
			  parser.finish( );
			  co_return daw::mhttpd::response::from_owned(
			    name + " " + filename + " " + std::to_string( file_size ) +
			    ( contiguous ? "" : " gaps" ) );
		  } );
		daw::mhttpd::daemon d{ { .max_body_size = 1024 }, std::move( routes ) };
		auto file = std::string( 4 * 1024 * 1024, 'z' );
		for( std::size_t n = 0; n < file.size( ); n += 4093 ) {
			file[n] = '-'; // some near-misses for the boundary search
		}
		auto const body = multipart_body(
		  { { .name = "name", .value = "Big" },
		    { .name = "file",
		      .value = file,
		      .filename = "big.zip",
		      .content_type = "application/zip" } } );
		auto r = http( d.port( ), "POST", "/worlds", body, multipart_header( ) );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "Big big.zip 4194304" );
	}
} // namespace

int main( int argc, char **argv ) {
	struct test_case {
		daw::string_view name;
		void ( *fn )( );
	};
	static constexpr test_case tests[] = {
	  { "form_urlencoded", &form_urlencoded },
	  { "form_multipart", &form_multipart },
	  { "form_find", &form_find },
	  { "form_not_a_form", &form_not_a_form },
	  { "form_truncated_multipart", &form_truncated_multipart },
	  { "form_handler_exception_propagates", &form_handler_exception_propagates },
	  { "form_streamed_upload", &form_streamed_upload },
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
