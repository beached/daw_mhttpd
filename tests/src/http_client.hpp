// Minimal blocking HTTP/1.1 client used by the tests
#pragma once

#include "daw/mhttpd/daw_mhttpd.h"

#include <daw/daw_integer.h>
#include <daw/daw_string_view.h>

#include <arpa/inet.h>
#include <format>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace daw_mhttpd_test {

	struct http_result {
		int status = 0; // 0 = connection closed without a status line
		std::string headers;
		std::string body;
	};

	inline std::string dechunk( daw::string_view in ) {
		std::string out;
		while( !in.empty( ) ) {
			auto eol = in.find( "\r\n" );
			auto n = std::stoul( std::string{ in.substr( 0, eol ) }, nullptr, 16 );
			if( n == 0 ) {
				break;
			}
			auto chunk = in.substr( eol + 2, n );
			out.append( chunk.data( ), chunk.size( ) );
			in.remove_prefix( eol + 2 + n + 2 );
		}
		return out;
	}

	// Minimal blocking HTTP/1.1 client; sends `Connection: close` and reads to
	// EOF.
	inline daw::mhttpd::unique_fd connect_to( std::uint16_t port ) {
		daw::mhttpd::unique_fd fd = ::socket( AF_INET, SOCK_STREAM, 0 );
		sockaddr_in addr{ };
		addr.sin_family = AF_INET;
		addr.sin_port = htons( port );
		addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
		if( ::connect( fd.get( ),
		               reinterpret_cast<sockaddr *>( &addr ),
		               sizeof addr ) != 0 ) {
			::close( fd.get( ) );
			return -1;
		}
		return fd;
	}

	// Connects to [::1]
	inline int connect_to_v6( std::uint16_t port ) {
		int fd = ::socket( AF_INET6, SOCK_STREAM, 0 );
		sockaddr_in6 addr{ };
		addr.sin6_family = AF_INET6;
		addr.sin6_port = htons( port );
		addr.sin6_addr = in6addr_loopback;
		if( ::connect( fd, reinterpret_cast<sockaddr *>( &addr ), sizeof addr ) !=
		    0 ) {
			::close( fd );
			return -1;
		}
		return fd;
	}

	inline void send_all( daw::mhttpd::unique_fd &fd, daw::string_view data ) {
		while( not data.empty( ) ) {
			auto n = ::send( fd.get( ), data.data( ), data.size( ), MSG_NOSIGNAL );
			if( n <= 0 ) {
				return;
			}
			data.remove_prefix( static_cast<std::size_t>( n ) );
		}
	}

	inline http_result read_response( daw::mhttpd::unique_fd fd ) {
		std::string raw;
		char buf[4096];
		for( ssize_t n; ( n = ::recv( fd.get( ), buf, sizeof buf, 0 ) ) > 0; ) {
			raw.append( buf, static_cast<std::size_t>( n ) );
		}
		fd.reset( );

		http_result r;
		auto hdr_end = raw.find( "\r\n\r\n" );
		if( raw.size( ) < 12 || hdr_end == std::string::npos ) {
			return r;
		}
		r.status = std::stoi( raw.substr( 9, 3 ) );
		r.headers = raw.substr( 0, hdr_end );
		r.body = raw.substr( hdr_end + 4 );
		if( r.headers.find( "Transfer-Encoding: chunked" ) != std::string::npos ) {
			r.body = dechunk( r.body );
		}
		return r;
	}

	// Sends one request on the connected socket `fd` and closes it
	inline http_result http_on( daw::mhttpd::unique_fd fd,
	                            std::string const &method,
	                            std::string const &target,
	                            std::string const &body = { },
	                            std::string const &extra_headers = { } ) {
		if( not fd ) {
			return { };
		}
		auto const content_length =
		  not body.empty( ) and
		      extra_headers.find( "Transfer-Encoding" ) == std::string::npos
		    ? std::format( "Content-Length: {}\r\n", body.size( ) )
		    : std::string{ };
		auto const req = std::format(
		  "{} {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}{}\r\n{}",
		  method,
		  target,
		  extra_headers,
		  content_length,
		  body );
		send_all( fd, req );
		return read_response( std::move( fd ) );
	}

	inline http_result http( std::uint16_t port, std::string const &method,
	                         std::string const &target,
	                         std::string const &body = { },
	                         std::string const &extra_headers = { } ) {
		return http_on( connect_to( port ), method, target, body, extra_headers );
	}

	// Sends `count` GET requests for `target` one after another on a single
	// keep-alive connection. Stops early if the server closes it; the result
	// then has fewer entries than `count`.
	inline std::vector<http_result>
	http_keep_alive( std::uint16_t port, std::string const &target, int count ) {
		auto results = std::vector<http_result>{ };
		auto fd = connect_to( port );
		if( not fd ) {
			return results;
		}
		auto const req =
		  std::format( "GET {} HTTP/1.1\r\nHost: localhost\r\n\r\n", target );
		auto raw = std::string{ };
		for( int n = 0; n < count; ++n ) {
			send_all( fd, req );
			// Read until this response's headers and Content-Length body are in
			auto r = http_result{ };
			while( true ) {
				auto const hdr_end = raw.find( "\r\n\r\n" );
				if( hdr_end != std::string::npos ) {
					r.headers = raw.substr( 0, hdr_end );
					auto length = std::size_t{ 0 };
					if( auto p = r.headers.find( "Content-Length: " );
					    p != std::string::npos ) {
						length = std::stoul( r.headers.substr( p + 16 ) );
					}
					if( raw.size( ) >= hdr_end + 4 + length ) {
						r.status = std::stoi( raw.substr( 9, 3 ) );
						r.body = raw.substr( hdr_end + 4, length );
						raw.erase( 0, hdr_end + 4 + length );
						break;
					}
				}
				char buf[4096];
				auto const got = ::recv( fd.get( ), buf, sizeof buf, 0 );
				if( got <= 0 ) {
					fd.reset( );
					return results; // closed by the server
				}
				raw.append( buf, static_cast<std::size_t>( got ) );
			}
			results.push_back( std::move( r ) );
		}
		fd.reset( );
		return results;
	}

	inline http_result http_v6( std::uint16_t port, std::string const &method,
	                            std::string const &target ) {
		return http_on( connect_to_v6( port ), method, target );
	}
} // namespace daw_mhttpd_test
