#include "daw/mhttpd/daw_mhttpd.h"
#include "http_client.hpp"

#include <daw/daw_ensure.h>
#include <daw/daw_size_literals.h>
#include <daw/daw_string_view.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sched.h>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
using namespace daw::size_literals;

namespace {

	using namespace daw_mhttpd_test;

	void sync_handler_with_header( ) {
		daw::mhttpd::daemon d{ { }, []( daw::mhttpd::request & ) {
			                      return daw::mhttpd::response::from_copy(
			                               daw::string_view{ "hello" } )
			                        .header( "X-Test", "yes" );
		                      } };
		auto r = http( d.port( ), "GET", "/" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "hello" );
		daw_ensure( r.headers.find( "X-Test: yes" ) != std::string::npos );
	}

	void request_accessors( ) {
		daw::mhttpd::daemon d{
		  { }, []( daw::mhttpd::request &req ) {
			  return daw::mhttpd::response::from_owned(
			    std::format( "{} {} {} {} {}",
			                 to_string( req.method( ) ),
			                 req.url( ),
			                 req.query( "q" ).value_or( "-" ),
			                 req.header( "x-custom" ).value_or( "-" ),
			                 req.query( "missing" ).value_or( "-" ) ) );
		  } };
		auto r = http( d.port( ), "GET", "/path?q=42", { }, "X-Custom: abc\r\n" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "GET /path 42 abc -" );
	}

	void post_body_echo( ) {
		daw::mhttpd::daemon d{
		  { }, []( daw::mhttpd::request &req ) {
			  return daw::mhttpd::response::from_owned(
			    std::vector<std::byte>( req.body( ).begin( ), req.body( ).end( ) ) );
		  } };
		std::string payload( 100'000, 'x' );
		auto r = http( d.port( ), "POST", "/", payload );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == payload );
	}

	// Larger than the up-front reserve, so the body also grows as it arrives
	void post_large_body_echo( ) {
		daw::mhttpd::daemon d{
		  { }, []( daw::mhttpd::request &req ) {
			  return daw::mhttpd::response::from_owned(
			    std::vector<std::byte>( req.body( ).begin( ), req.body( ).end( ) ) );
		  } };
		std::string payload( 700'000, 'y' );
		auto r = http( d.port( ), "POST", "/", payload );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == payload );
	}

	void mutable_body_in_place( ) {
		daw::mhttpd::daemon d{
		  { }, []( daw::mhttpd::request &req ) {
			  auto text = req.mutable_body_text( );
			  std::ranges::transform( text, text.begin( ), []( char c ) {
				  return c >= 'a' and c <= 'z' ? static_cast<char>( c - 'a' + 'A' )
				                               : c;
			  } );
			  daw_ensure( req.body_text( ) == "HELLO, WORLD" );
			  return daw::mhttpd::response::from_owned( std::string{
			    req.body_text( ).data( ), req.body_text( ).size( ) } );
		  } };
		auto r = http( d.port( ), "POST", "/", "hello, world" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "HELLO, WORLD" );
	}

	void take_body_moves_buffer( ) {
		daw::mhttpd::daemon d{
		  { }, []( daw::mhttpd::request &req ) {
			  auto const *data = req.body( ).data( );
			  auto body = req.take_body( );
			  daw_ensure( body.data( ) == data ); // moved, not copied
			  daw_ensure( req.body( ).empty( ) );
			  daw_ensure( req.mutable_body( ).empty( ) );
			  return daw::mhttpd::response::from_owned( std::move( body ) );
		  } };
		std::string payload( 100'000, 'x' );
		auto r = http( d.port( ), "POST", "/", payload );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == payload );
	}

	void body_too_large_content_length( ) {
		daw::mhttpd::daemon d{ { .max_body_size = 10 },
		                       []( daw::mhttpd::request & ) {
			                       return daw::mhttpd::response::empty( );
		                       } };
		auto r = http( d.port( ), "POST", "/", std::string( 100, 'x' ) );
		daw_ensure( r.status == 413 );
	}

	void body_too_large_chunked( ) {
		daw::mhttpd::daemon d{ { .max_body_size = 10 },
		                       []( daw::mhttpd::request & ) {
			                       return daw::mhttpd::response::empty( );
		                       } };
		auto r =
		  http( d.port( ),
		        "POST",
		        "/",
		        std::format( "64\r\n{}\r\n0\r\n\r\n", std::string( 100, 'x' ) ),
		        "Transfer-Encoding: chunked\r\n" );
		daw_ensure( r.status == 413 );
	}

	void static_response( ) {
		static constexpr daw::string_view text = "static body";
		daw::mhttpd::daemon d{ { }, []( daw::mhttpd::request & ) {
			                      return daw::mhttpd::response::from_static( text,
			                                                                 201 );
		                      } };
		auto r = http( d.port( ), "GET", "/" );
		daw_ensure( r.status == 201 );
		daw_ensure( r.body == text );
	}

	void stream_response( ) {
		daw::mhttpd::daemon d{
		  { }, []( daw::mhttpd::request & ) {
			  return daw::mhttpd::response::stream(
			    [n = 0]( daw::u64, std::span<std::byte> out ) mutable {
				    if( n == 3 ) {
					    return daw::mhttpd::stream_result::end( );
				    }
				    auto s = "part" + std::to_string( n++ );
				    std::memcpy( out.data( ), s.data( ), s.size( ) );
				    return daw::mhttpd::stream_result::bytes( s.size( ) );
			    } );
		  } };
		auto r = http( d.port( ), "GET", "/" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "part0part1part2" );
	}

	void responder_completed_from_another_thread( ) {
		std::vector<std::jthread> workers;
		std::mutex m;
		daw::mhttpd::daemon d{
		  { }, [&]( daw::mhttpd::request &req, daw::mhttpd::responder res ) {
			  std::string url{ req.url( ) }; // copy: request views end at send()
			  std::lock_guard lock{ m };
			  workers.emplace_back( [res = std::move( res ), url]( ) mutable {
				  std::this_thread::sleep_for( 50ms );
				  res.send( daw::mhttpd::response::from_owned(
				    std::format( "async {}", url ) ) );
			  } );
		  } };
		auto r = http( d.port( ), "GET", "/x" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "async /x" );
	}

	void dropped_responder_gives_500( ) {
		daw::mhttpd::daemon d{
		  { }, []( daw::mhttpd::request &, daw::mhttpd::responder ) {} };
		daw_ensure( http( d.port( ), "GET", "/" ).status == 500 );
	}

	void handler_exception_gives_500( ) {
		daw::mhttpd::daemon d{
		  { }, []( daw::mhttpd::request & ) -> daw::mhttpd::response {
			  throw std::runtime_error{ "boom" };
		  } };
		daw_ensure( http( d.port( ), "GET", "/" ).status == 500 );
	}

	void second_send_is_rejected( ) {
		std::promise<bool> second;
		daw::mhttpd::daemon d{
		  { }, [&]( daw::mhttpd::request &, daw::mhttpd::responder res ) {
			  daw::mhttpd::responder other = std::move( res );
			  other.send( daw::mhttpd::response::empty( 204 ) );
			  second.set_value( other.send( daw::mhttpd::response::empty( 500 ) ) );
		  } };
		daw_ensure( http( d.port( ), "GET", "/" ).status == 204 );
		daw_ensure( not second.get_future( ).get( ) );
	}

	// send() on another thread races the handler's suspend; every request must
	// complete.
	void race_between_send_and_suspend( ) {
		std::mutex m;
		std::vector<std::jthread> workers;
		daw::mhttpd::daemon d{
		  { .thread_pool_size = 4 },
		  [&]( daw::mhttpd::request &, daw::mhttpd::responder res ) {
			  std::lock_guard lock{ m };
			  workers.emplace_back( [res = std::move( res )]( ) mutable {
				  res.send(
				    daw::mhttpd::response::from_copy( daw::string_view{ "ok" } ) );
			  } );
		  } };
		constexpr int clients = 8;
		constexpr int per_client = 50;
		std::atomic<int> ok{ 0 };
		{
			std::vector<std::jthread> cs;
			for( int c = 0; c < clients; ++c ) {
				cs.emplace_back( [&] {
					for( int i = 0; i < per_client; ++i ) {
						auto r = http( d.port( ), "GET", "/" );
						if( r.status == 200 && r.body == "ok" ) {
							++ok;
						}
					}
				} );
			}
		}
		daw_ensure( ok.load( ) == clients * per_client );
	}

	void stop_answers_suspended_requests_with_503( ) {
		std::mutex m;
		std::optional<daw::mhttpd::responder> parked;
		std::promise<void> got_request;
		auto d = std::make_unique<daw::mhttpd::daemon>(
		  daw::mhttpd::config{ },
		  [&]( daw::mhttpd::request &, daw::mhttpd::responder res ) {
			  std::lock_guard lock{ m };
			  parked.emplace( std::move( res ) );
			  got_request.set_value( );
		  } );
		auto port = d->port( );
		auto client = std::async( std::launch::async, [port] {
			return http( port, "GET", "/" );
		} );
		got_request.get_future( ).wait( );

		d->stop( );
		daw_ensure( client.get( ).status == 503 );

		std::lock_guard lock{ m };
		daw_ensure( not parked->send( daw::mhttpd::response::empty( ) ) );
		d.reset( );
		parked.reset( ); // destroying after the daemon is gone must be harmless
	}

	// Requests suspended on several MHD threads are in different slots; stop
	// must find and answer all of them
	void stop_answers_requests_suspended_on_every_thread( ) {
		constexpr int clients = 16;
		std::mutex m;
		std::vector<daw::mhttpd::responder> parked;
		std::promise<void> all_parked;
		auto d = std::make_unique<daw::mhttpd::daemon>(
		  daw::mhttpd::config{ .thread_pool_size = 4 },
		  [&]( daw::mhttpd::request &, daw::mhttpd::responder res ) {
			  std::lock_guard lock{ m };
			  parked.push_back( std::move( res ) );
			  if( parked.size( ) == clients ) {
				  all_parked.set_value( );
			  }
		  } );
		auto const port = d->port( );
		std::vector<std::future<http_result>> results;
		for( int c = 0; c < clients; ++c ) {
			results.push_back( std::async( std::launch::async, [port] {
				return http( port, "GET", "/" );
			} ) );
		}
		all_parked.get_future( ).wait( );

		d->stop( );
		for( auto &r : results ) {
			daw_ensure( r.get( ).status == 503 );
		}
		std::lock_guard lock{ m };
		for( auto &res : parked ) {
			daw_ensure( not res.send( daw::mhttpd::response::empty( ) ) );
		}
	}

	// -------------------------------------------------------------------------
	// Coroutines
	// -------------------------------------------------------------------------

	using daw::mhttpd::completion;
	using daw::mhttpd::request;
	using daw::mhttpd::response;
	using daw::mhttpd::task;

	// Owns the threads that complete callbacks, standing in for another
	// codebase's event loop
	struct worker_pool {
		std::mutex m;
		std::vector<std::jthread> threads;

		template<typename F>
		void run( F &&f ) {
			auto lock = std::lock_guard{ m };
			threads.emplace_back( std::forward<F>( f ) );
		}
	};

	task<std::string> async_upper( std::string s, worker_pool &workers ) {
		co_return co_await daw::mhttpd::from_callback<std::string>(
		  [&workers, s = std::move( s )]( completion<std::string> done ) mutable {
			  workers.run( [s = std::move( s ), done = std::move( done )]( ) mutable {
				  std::this_thread::sleep_for( 20ms );
				  for( auto &c : s ) {
					  c = static_cast<char>( std::toupper( c ) );
				  }
				  done( std::move( s ) );
			  } );
		  } );
	}

	void coroutine_handler_completes_synchronously( ) {
		daw::mhttpd::daemon d{ { }, []( request & ) -> task<response> {
			                      co_return response::from_copy(
			                        daw::string_view{ "coro" } );
		                      } };
		auto r = http( d.port( ), "GET", "/" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "coro" );
	}

	void coroutine_handler_resumed_on_another_thread( ) {
		auto workers = worker_pool{ };
		daw::mhttpd::daemon d{
		  { }, [&workers]( request &req ) -> task<response> {
			  auto url = std::string{ req.url( ) }; // copy before suspending
			  auto upper = co_await async_upper( std::move( url ), workers );
			  co_return response::from_owned( std::move( upper ) );
		  } };
		auto r = http( d.port( ), "GET", "/abc" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "/ABC" );
	}

	void coroutine_handler_exception_gives_500( ) {
		daw::mhttpd::daemon d{ { }, []( request & ) -> task<response> {
			                      throw std::runtime_error{ "boom" };
			                      co_return response::empty( );
		                      } };
		daw_ensure( http( d.port( ), "GET", "/" ).status == 500 );
	}

	void abandoned_completion_throws( ) {
		daw::mhttpd::daemon d{ { }, []( request & ) -> task<response> {
			                      auto abandoned = false;
			                      try {
				                      co_await daw::mhttpd::from_callback<int>(
				                        []( completion<int> ) {} );
			                      } catch( daw::mhttpd::callback_abandoned const & ) {
				                      abandoned = true;
			                      }
			                      co_return response::empty( abandoned ? 502 : 200 );
		                      } };
		daw_ensure( http( d.port( ), "GET", "/" ).status == 502 );
	}

	task<int> identity( int i ) {
		co_return i;
	}

	task<long long> sum_chain( int n ) {
		auto total = 0LL;
		for( auto i = 0; i < n; ++i ) {
			total += co_await identity( i );
			// Completed inside start, so co_await continues without suspending
			total +=
			  co_await daw::mhttpd::from_callback<int>( []( completion<int> done ) {
				  done( 0 );
			  } );
		}
		co_return total;
	}

	void sync_wait_deep_chain( ) {
		constexpr auto n = 100'000;
		daw_ensure( daw::mhttpd::sync_wait( sum_chain( n ) ) ==
		            static_cast<long long>( n ) * ( n - 1 ) / 2 );
	}

	task<void> throws_after_resume( worker_pool &workers ) {
		co_await daw::mhttpd::from_callback( [&]( completion<> done ) {
			workers.run( [done = std::move( done )]( ) mutable {
				done.fail( std::make_exception_ptr( std::logic_error{ "fail" } ) );
			} );
		} );
	}

	void sync_wait_propagates_exception( ) {
		auto workers = worker_pool{ };
		auto caught = false;
		try {
			daw::mhttpd::sync_wait( throws_after_resume( workers ) );
		} catch( std::logic_error const & ) { caught = true; }
		daw_ensure( caught );
	}

	// The coroutine finishes after the daemon is gone; its handler captures must
	// still be alive and its send must be ignored
	void coroutine_outlives_daemon( ) {
		auto parked = std::optional<completion<>>{ };
		auto parked_mutex = std::mutex{ };
		auto got_request = std::promise<void>{ };
		auto finished = std::promise<int>{ };
		auto d = std::make_unique<daw::mhttpd::daemon>(
		  daw::mhttpd::config{ },
		  [&, value = std::make_unique<int>( 42 )]( request & ) -> task<response> {
			  co_await daw::mhttpd::from_callback( [&]( completion<> done ) {
				  auto lock = std::lock_guard{ parked_mutex };
				  parked.emplace( std::move( done ) );
				  got_request.set_value( );
			  } );
			  finished.set_value( *value );
			  co_return response::empty( );
		  } );
		auto port = d->port( );
		auto client = std::async( std::launch::async, [port] {
			return http( port, "GET", "/" );
		} );
		got_request.get_future( ).wait( );

		d.reset( );
		daw_ensure( client.get( ).status == 503 );

		auto lock = std::lock_guard{ parked_mutex };
		( *parked )( );
		daw_ensure( finished.get_future( ).get( ) == 42 );
	}

	// -------------------------------------------------------------------------
	// Streamed request bodies
	// -------------------------------------------------------------------------

	std::string make_body( std::size_t n ) {
		auto s = std::string( n, '\0' );
		for( std::size_t i = 0; i < n; ++i ) {
			s[i] = static_cast<char>( 'a' + ( i * 31 ) % 26 );
		}
		return s;
	}

	std::size_t checksum( std::span<std::byte const> data,
	                      std::size_t seed = 0 ) {
		for( auto b : data ) {
			seed = seed * 131 + static_cast<std::size_t>( b );
		}
		return seed;
	}

	std::size_t checksum( std::string const &s ) {
		return checksum( std::as_bytes( std::span{ s } ) );
	}

	// Reads the body chunk by chunk and answers "<bytes> <checksum>"
	task<response> count_body( request &req ) {
		auto bytes = std::size_t{ 0 };
		auto sum = std::size_t{ 0 };
		while( auto chunk = co_await req.next_chunk( ) ) {
			bytes += chunk->size( );
			sum = checksum( *chunk, sum );
		}
		co_return response::from_owned( std::format( "{} {}", bytes, sum ) );
	}

	void stream_body_bypasses_buffer_limit( ) {
		daw::mhttpd::daemon d{ { .max_body_size = 16 }, &count_body };
		auto body = make_body( 3_MB );
		auto r = http( d.port( ), "POST", "/", body );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body ==
		            std::format( "{} {}", body.size( ), checksum( body ) ) );
	}

	void read_body_echo( ) {
		daw::mhttpd::daemon d{
		  { }, []( request &req ) -> task<response> {
			  auto body = co_await req.read_body( );
			  co_return response::from_owned(
			    std::vector<std::byte>( body.begin( ), body.end( ) ) );
		  } };
		auto body = make_body( 100'000 );
		auto r = http( d.port( ), "POST", "/", body );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == body );
	}

	void read_body_large_echo( ) {
		daw::mhttpd::daemon d{
		  { }, []( request &req ) -> task<response> {
			  auto body = co_await req.read_body( );
			  co_return response::from_owned(
			    std::vector<std::byte>( body.begin( ), body.end( ) ) );
		  } };
		auto body = make_body( 700'000 );
		auto r = http( d.port( ), "POST", "/", body );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == body );
	}

	// Coroutine handlers can mutate or take the body once read_body( ) has
	// buffered it
	void read_body_then_mutate_and_take( ) {
		daw::mhttpd::daemon d{
		  { }, []( request &req ) -> task<response> {
			  (void)co_await req.read_body( );
			  std::ranges::reverse( req.mutable_body( ) );
			  co_return response::from_owned( req.take_body( ) );
		  } };
		auto body = make_body( 700'000 );
		auto r = http( d.port( ), "POST", "/", body );
		std::ranges::reverse( body );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == body );
	}

	void read_body_too_large_gives_413( ) {
		daw::mhttpd::daemon d{ { .max_body_size = 10 },
		                       []( request &req ) -> task<response> {
			                       co_await req.read_body( );
			                       co_return response::empty( );
		                       } };
		auto r =
		  http( d.port( ),
		        "POST",
		        "/",
		        std::format( "64\r\n{}\r\n0\r\n\r\n", std::string( 100, 'x' ) ),
		        "Transfer-Encoding: chunked\r\n" );
		daw_ensure( r.status == 413 );
	}

	// Answered from the headers alone; the client never sends the body
	void early_response_without_reading_body( ) {
		daw::mhttpd::daemon d{ { }, []( request & ) -> task<response> {
			                      co_return response::empty( 401 );
		                      } };
		auto fd = connect_to( d.port( ) );
		send_all( fd,
		          "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
		          "100000\r\n\r\n" );
		daw_ensure( read_response( std::move( fd ) ).status == 401 );
	}

	// While the coroutine waits on something else MHD stops reading; the body
	// must still arrive complete and in order
	void backpressure_while_coroutine_busy( ) {
		using namespace daw::size_literals;
		auto workers = worker_pool{ };
		daw::mhttpd::daemon d{
		  { }, [&workers]( request &req ) -> task<response> {
			  auto bytes = std::size_t{ 0 };
			  auto sum = std::size_t{ 0 };
			  while( auto chunk = co_await req.next_chunk( ) ) {
				  bytes += chunk->size( );
				  sum = checksum( *chunk, sum );
				  // chunk is invalid after this suspension
				  co_await daw::mhttpd::from_callback( [&]( completion<> done ) {
					  workers.run( [done = std::move( done )]( ) mutable {
						  std::this_thread::sleep_for( 2ms );
						  done( );
					  } );
				  } );
			  }
			  co_return response::from_owned( std::format( "{} {}", bytes, sum ) );
		  } };
		auto body = make_body( 1_MB );
		auto r = http( d.port( ), "POST", "/", body );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body ==
		            std::format( "{} {}", body.size( ), checksum( body ) ) );
	}

	void respond_before_body_finished( ) {
		daw::mhttpd::daemon d{ { }, []( request &req ) -> task<response> {
			                      co_await req.next_chunk( );
			                      co_return response::from_copy(
			                        daw::string_view{ "early" } );
		                      } };
		auto r = http( d.port( ), "POST", "/", make_body( 2_MB ) );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "early" );
	}

	void client_disconnect_aborts_body( ) {
		auto aborted = std::promise<bool>{ };
		daw::mhttpd::daemon d{ { }, [&]( request &req ) -> task<response> {
			                      auto was_aborted = false;
			                      try {
				                      while( co_await req.next_chunk( ) ) {}
			                      } catch( daw::mhttpd::request_aborted const & ) {
				                      was_aborted = true;
			                      }
			                      aborted.set_value( was_aborted );
			                      co_return response::empty( );
		                      } };
		auto fd = connect_to( d.port( ) );
		send_all( fd,
		          "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
		          "100000\r\n\r\n0123456789" );
		std::this_thread::sleep_for( 50ms );
		fd.reset( );
		auto result = aborted.get_future( );
		daw_ensure( result.wait_for( 5s ) == std::future_status::ready );
		daw_ensure( result.get( ) );
	}

	// -------------------------------------------------------------------------
	// wait / request_stop
	// -------------------------------------------------------------------------

	void request_stop_from_handler( ) {
		auto daemon = std::atomic<daw::mhttpd::daemon *>{ nullptr };
		daw::mhttpd::daemon d{ { }, [&]( request &req ) {
			                      if( req.url( ) == "/shutdown" ) {
				                      daemon.load( )->request_stop( );
				                      return response::empty( 202 );
			                      }
			                      return response::empty( 200 );
		                      } };
		daemon = &d;
		auto const port = d.port( );
		daw_ensure( http( port, "GET", "/" ).status == 200 );

		auto client = std::async( std::launch::async, [port] {
			return http( port, "GET", "/shutdown" );
		} );
		d.wait( );
		// The handler's own response is delivered before the server stops
		daw_ensure( client.get( ).status == 202 );
		daw_ensure( http( port, "GET", "/" ).status == 0 ); // no longer listening
	}

	void wait_returns_when_stopped_elsewhere( ) {
		daw::mhttpd::daemon d{ { }, []( request & ) {
			                      return response::empty( );
		                      } };
		auto stoppers = std::vector<std::jthread>{ };
		for( auto i = 0; i < 2; ++i ) { // concurrent stop( ) calls are safe
			stoppers.emplace_back( [&d] {
				std::this_thread::sleep_for( 20ms );
				d.stop( );
			} );
		}
		d.wait( );
		daw_ensure( http( d.port( ), "GET", "/" ).status == 0 );
	}

	void request_stop_before_wait( ) {
		daw::mhttpd::daemon d{ { }, []( request & ) {
			                      return response::empty( );
		                      } };
		d.request_stop( );
		d.wait( ); // returns immediately
		daw_ensure( http( d.port( ), "GET", "/" ).status == 0 );
	}

	// -------------------------------------------------------------------------
	// http_method
	// -------------------------------------------------------------------------

	using daw::mhttpd::http_method;
	static_assert( daw::mhttpd::parse_method( "GET" ) == http_method::GET );
	static_assert( daw::mhttpd::parse_method( "PATCH" ) == http_method::PATCH );
	// Case-sensitive per RFC 9110 section 9.1
	static_assert( daw::mhttpd::parse_method( "get" ) == http_method::UNKNOWN );
	static_assert( daw::mhttpd::parse_method( "Get" ) == http_method::UNKNOWN );
	static_assert( daw::mhttpd::parse_method( "pAtCh" ) == http_method::UNKNOWN );
	static_assert( daw::mhttpd::parse_method( "PROPFIND" ) ==
	               http_method::UNKNOWN );
	static_assert( daw::mhttpd::parse_method( "" ) == http_method::UNKNOWN );
	static_assert( [] {
		for( auto m : { http_method::GET,
		                http_method::HEAD,
		                http_method::POST,
		                http_method::PUT,
		                http_method::DELETE,
		                http_method::CONNECT,
		                http_method::OPTIONS,
		                http_method::TRACE,
		                http_method::PATCH } ) {
			if( daw::mhttpd::parse_method( to_string( m ) ) != m ) {
				return false;
			}
		}
		return true;
	}( ) );

	void request_method( ) {
		daw::mhttpd::daemon d{ { }, []( daw::mhttpd::request &req ) {
			                      return daw::mhttpd::response::from_owned(

			                        std::format( "{} {}",
			                                     to_string( req.method( ) ),
			                                     req.method_name( ) ) );
		                      } };
		daw_ensure( http( d.port( ), "GET", "/" ).body == "GET GET" );
		daw_ensure( http( d.port( ), "POST", "/", "x" ).body == "POST POST" );
		daw_ensure( http( d.port( ), "PATCH", "/", "x" ).body == "PATCH PATCH" );
		daw_ensure( http( d.port( ), "DELETE", "/" ).body == "DELETE DELETE" );
		daw_ensure( http( d.port( ), "PROPFIND", "/" ).body == "UNKNOWN PROPFIND" );
	}

	// Keep-alive must survive every handler style. A response a coroutine makes
	// while MHD only has the headers used to close the connection every time.
	void keep_alive_every_handler_style( ) {
		auto const check =
		  []( std::uint16_t port, std::string const &target, int status ) {
			  auto const results = http_keep_alive( port, target, 3 );
			  daw_ensure( results.size( ) == 3 );
			  for( auto const &r : results ) {
				  daw_ensure( r.status == status );
				  daw_ensure( r.headers.find( "Connection: close" ) ==
				              std::string::npos );
			  }
		  };
		{
			daw::mhttpd::daemon d{ { }, []( daw::mhttpd::request & ) {
				                      return daw::mhttpd::response::from_static( "s" );
			                      } };
			check( d.port( ), "/", 200 );
		}
		{
			daw::mhttpd::daemon d{
			  { },
			  [](
			    daw::mhttpd::request & ) -> daw::mhttpd::task<daw::mhttpd::response> {
				  co_return daw::mhttpd::response::from_static( "c" );
			  } };
			check( d.port( ), "/", 200 );
		}
		{
			auto routes = daw::mhttpd::router{ };
			routes.get( "/sync", []( daw::mhttpd::request & ) {
				return daw::mhttpd::response::from_static( "s" );
			} );
			routes.get( "/coro",
			            []( daw::mhttpd::request & )
			              -> daw::mhttpd::task<daw::mhttpd::response> {
				            co_return daw::mhttpd::response::from_static( "c" );
			            } );
			routes.post( "/post-only", []( daw::mhttpd::request & ) {
				return daw::mhttpd::response::empty( );
			} );
			daw::mhttpd::daemon d{ { }, std::move( routes ) };
			check( d.port( ), "/sync", 200 );
			check( d.port( ), "/coro", 200 );
			check( d.port( ), "/missing", 404 );
			check( d.port( ), "/post-only", 405 );
		}
	}

	// A response before the body is read still closes the connection, since
	// the rest of the body must be discarded
	void early_response_with_body_closes( ) {
		daw::mhttpd::daemon d{
		  { },
		  []( daw::mhttpd::request & ) -> daw::mhttpd::task<daw::mhttpd::response> {
			  co_return daw::mhttpd::response::empty( 401 );
		  } };
		auto r = http( d.port( ), "POST", "/", std::string( 1000, 'x' ) );
		daw_ensure( r.status == 401 );
	}

	// -------------------------------------------------------------------------
	// Addresses
	// -------------------------------------------------------------------------

	auto const echo_remote_address = []( daw::mhttpd::request &req ) {
		return daw::mhttpd::response::from_owned( req.remote_address( ) );
	};

	void remote_address_ipv4( ) {
		daw::mhttpd::daemon d{ { }, []( daw::mhttpd::request &req ) {
			                      auto const *addr = req.remote_sockaddr( );
			                      daw_ensure( addr != nullptr );
			                      daw_ensure( addr->sa_family == AF_INET );
			                      return daw::mhttpd::response::from_owned(
			                        req.remote_address( ) );
		                      } };
		auto r = http( d.port( ), "GET", "/" );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "127.0.0.1" );
	}

	void bind_ipv4_loopback( ) {
		daw::mhttpd::daemon d{ { .bind_address = "127.0.0.1" },
		                       echo_remote_address };
		daw_ensure( d.port( ) != 0 );
		daw_ensure( http( d.port( ), "GET", "/" ).body == "127.0.0.1" );
		// Not listening on IPv6
		daw_ensure( http_v6( d.port( ), "GET", "/" ).status == 0 );
	}

	void bind_ipv6_loopback( ) {
		daw::mhttpd::daemon d{ { .bind_address = "::1" }, echo_remote_address };
		daw_ensure( http_v6( d.port( ), "GET", "/" ).body == "::1" );
		// Not listening on IPv4
		daw_ensure( http( d.port( ), "GET", "/" ).status == 0 );
	}

	void bind_dual_stack( ) {
		daw::mhttpd::daemon d{ { .bind_address = "::" }, echo_remote_address };
		// IPv4 clients are reported as IPv4, not as ::ffff:127.0.0.1
		daw_ensure( http( d.port( ), "GET", "/" ).body == "127.0.0.1" );
		daw_ensure( http_v6( d.port( ), "GET", "/" ).body == "::1" );
	}

	// Opens a keep-alive connection and waits for its response, so the daemon
	// has accepted it and holds it open
	[[nodiscard]] daw::mhttpd::unique_fd held_connection( std::uint16_t port ) {
		auto fd = connect_to( port );
		daw_ensure( static_cast<bool>( fd ) );
		send_all( fd, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n" );
		char buf[256];
		daw_ensure( ::recv( fd.get( ), buf, sizeof buf, 0 ) > 0 );
		return fd;
	}

	// Once the held connection closes the daemon accepts again; it notices the
	// close on its own thread, so this retries for a while
	[[nodiscard]] bool accepts_again( std::uint16_t port ) {
		auto const until =
		  std::chrono::steady_clock::now( ) + std::chrono::seconds{ 5 };
		while( std::chrono::steady_clock::now( ) < until ) {
			if( http( port, "GET", "/" ).status == 200 ) {
				return true;
			}
			std::this_thread::sleep_for( std::chrono::milliseconds{ 10 } );
		}
		return false;
	}

	// At the limit the daemon stops accepting: a new connection waits in the
	// listen queue and is answered once a held one closes
	void connection_limit_queues_extra( ) {
		daw::mhttpd::daemon d{ { .connection_limit = 1 },
		                       []( daw::mhttpd::request & ) {
			                       return daw::mhttpd::response::from_static( "ok" );
		                       } };
		auto held = held_connection( d.port( ) );
		auto waiting = connect_to( d.port( ) );
		daw_ensure( static_cast<bool>( waiting ) );
		send_all( waiting,
		          "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n" );
		auto pfd = pollfd{ waiting.get( ), POLLIN, 0 };
		daw_ensure( ::poll( &pfd, 1, 200 ) == 0 );
		held.reset( );
		auto const r = read_response( std::move( waiting ) );
		daw_ensure( r.status == 200 );
		daw_ensure( r.body == "ok" );
	}

	void per_ip_connection_limit_closes_extra( ) {
		daw::mhttpd::daemon d{ { .thread_pool_size = 2,
		                         .per_ip_connection_limit = 1 },
		                       []( daw::mhttpd::request & ) {
			                       return daw::mhttpd::response::from_static( "ok" );
		                       } };
		auto held = held_connection( d.port( ) );
		daw_ensure( http( d.port( ), "GET", "/" ).status == 0 );
		held.reset( );
		daw_ensure( accepts_again( d.port( ) ) );
	}

	[[nodiscard]] unsigned allowed_cpus( ) {
		auto set = cpu_set_t{ };
		daw_ensure( ::sched_getaffinity( 0, sizeof set, &set ) == 0 );
		return static_cast<unsigned>( CPU_COUNT( &set ) );
	}

	void thread_count_resolved( ) {
		auto const ok = []( daw::mhttpd::request & ) {
			return daw::mhttpd::response::empty( );
		};
		{
			daw::mhttpd::daemon d{ { }, ok };
			daw_ensure( d.thread_count( ) == allowed_cpus( ) );
		}
		{
			daw::mhttpd::daemon d{ { .thread_pool_size = 1 }, ok };
			daw_ensure( d.thread_count( ) == 1 );
		}
		{
			daw::mhttpd::daemon d{ { .thread_pool_size = 3 }, ok };
			daw_ensure( d.thread_count( ) == 3 );
		}
		{
			// An automatic pool stays within the connection limit
			daw::mhttpd::daemon d{ { .connection_limit = 1 }, ok };
			daw_ensure( d.thread_count( ) == 1 );
		}
	}

	// Reads one response whose body is `body` from a keep-alive connection;
	// false if none arrives within a second
	[[nodiscard]] bool read_keep_alive_response( daw::mhttpd::unique_fd &fd,
	                                             daw::string_view body ) {
		auto raw = std::string{ };
		char buf[512];
		while( true ) {
			auto const end = raw.find( "\r\n\r\n" );
			if( end != std::string::npos and raw.size( ) >= end + 4 + body.size( ) ) {
				return raw.substr( end + 4 ) == body;
			}
			auto pfd = pollfd{ fd.get( ), POLLIN, 0 };
			if( ::poll( &pfd, 1, 1000 ) != 1 ) {
				return false;
			}
			auto const n = ::recv( fd.get( ), buf, sizeof buf, 0 );
			if( n <= 0 ) {
				return false;
			}
			raw.append( buf, static_cast<std::size_t>( n ) );
		}
	}

	// libmicrohttpd 1.0.1 to 1.0.5 can stop serving keep-alive connections on
	// a single epoll thread once about 128 are open; the daemon uses poll( )
	// there. Sending on every connection before reading makes them all ready at
	// once, which is what stalls it.
	void single_thread_serves_many_keep_alive( ) {
		daw::mhttpd::daemon d{ { .thread_pool_size = 1 },
		                       []( daw::mhttpd::request & ) {
			                       return daw::mhttpd::response::from_static( "ok" );
		                       } };
		auto conns = std::vector<daw::mhttpd::unique_fd>{ };
		for( auto i = 0; i < 256; ++i ) {
			conns.push_back( connect_to( d.port( ) ) );
			daw_ensure( static_cast<bool>( conns.back( ) ) );
		}
		for( auto round = 0; round < 200; ++round ) {
			for( auto &fd : conns ) {
				send_all( fd, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n" );
			}
			for( auto &fd : conns ) {
				daw_ensure( read_keep_alive_response( fd, "ok" ) );
			}
		}
	}

	void connection_limit_below_pool_throws( ) {
		auto threw = false;
		try {
			daw::mhttpd::daemon d{ { .thread_pool_size = 4, .connection_limit = 2 },
			                       []( daw::mhttpd::request & ) {
				                       return daw::mhttpd::response::empty( );
			                       } };
		} catch( std::invalid_argument const & ) { threw = true; }
		daw_ensure( threw );
	}

	void bind_invalid_address_throws( ) {
		for( auto const *address : { "localhost", "127.0.0", "::1%lo", " ::1" } ) {
			auto threw = false;
			try {
				daw::mhttpd::daemon d{ { .bind_address = address },
				                       echo_remote_address };
			} catch( std::invalid_argument const & ) { threw = true; }
			daw_ensure( threw );
		}
	}

	// -------------------------------------------------------------------------
	// Signals
	// -------------------------------------------------------------------------

	using signal_handler = void ( * )( int );

	[[nodiscard]] signal_handler current_handler( int sig ) {
		struct sigaction action{ };
		::sigaction( sig, nullptr, &action );
		return action.sa_handler;
	}

	void wait_until_handler( int sig, auto pred ) {
		while( not pred( current_handler( sig ) ) ) {
			std::this_thread::sleep_for( 1ms );
		}
	}

	[[nodiscard]] bool is_default( signal_handler h ) {
		return h == SIG_DFL;
	}

	std::atomic<int> g_counted_signals{ 0 };
	void count_signal( int ) {
		g_counted_signals.fetch_add( 1 );
	}

	void install_counting_handler( int sig ) {
		struct sigaction action{ };
		action.sa_handler = &count_signal;
		sigemptyset( &action.sa_mask );
		::sigaction( sig, &action, nullptr );
	}

	auto const hello = []( daw::mhttpd::request & ) {
		return daw::mhttpd::response::from_static( "hello" );
	};

	void wait_for_signals_stops_on_signal( ) {
		daw::mhttpd::daemon d{ { }, hello };
		auto const port = d.port( );
		auto raiser = std::jthread{ [&] {
			wait_until_handler( SIGUSR2, std::not_fn( is_default ) );
			daw_ensure( http( port, "GET", "/" ).body == "hello" );
			::kill( ::getpid( ), SIGUSR2 );
		} };
		d.wait_for_signals( { SIGUSR1, SIGUSR2 } );
		raiser.join( );
		// Stopped, and both signals are back to their original disposition
		daw_ensure( http( port, "GET", "/" ).status == 0 );
		daw_ensure( is_default( current_handler( SIGUSR1 ) ) );
		daw_ensure( is_default( current_handler( SIGUSR2 ) ) );
	}

	void remove_stop_signals_restores_handler( ) {
		install_counting_handler( SIGUSR1 );
		daw::mhttpd::daemon d{ { }, hello };
		auto const port = d.port( );
		auto raiser = std::jthread{ [&] {
			wait_until_handler( SIGUSR1, []( signal_handler h ) {
				return h != &count_signal;
			} );
			d.remove_stop_signals( { SIGUSR1, SIGHUP } ); // SIGHUP was never added
			daw_ensure( current_handler( SIGUSR1 ) == &count_signal );
			daw_ensure( is_default( current_handler( SIGHUP ) ) );
			::kill( ::getpid( ), SIGUSR1 );
			while( g_counted_signals.load( ) == 0 ) {
				std::this_thread::sleep_for( 1ms );
			}
			// Still serving
			daw_ensure( http( port, "GET", "/" ).body == "hello" );
			::kill( ::getpid( ), SIGUSR2 );
		} };
		d.wait_for_signals( { SIGUSR1, SIGUSR2 } );
		raiser.join( );
		daw_ensure( g_counted_signals.load( ) == 1 );
		daw_ensure( current_handler( SIGUSR1 ) == &count_signal );
		daw_ensure( is_default( current_handler( SIGUSR2 ) ) );
	}

	void second_signal_during_shutdown_gets_original_handler( ) {
		install_counting_handler( SIGUSR1 );
		daw::mhttpd::daemon d{ { .shutdown_grace = 500ms }, hello };
		auto const port = d.port( );
		auto raiser = std::jthread{ [&] {
			wait_until_handler( SIGUSR1, []( signal_handler h ) {
				return h != &count_signal;
			} );
			// An idle connection holds the shutdown for the grace period
			auto const idle = connect_to( port );
			daw_ensure( idle );
			std::this_thread::sleep_for( 50ms );
			::kill( ::getpid( ), SIGUSR1 );
			// The shutdown has started once the original handler is back
			wait_until_handler( SIGUSR1, []( signal_handler h ) {
				return h == &count_signal;
			} );
			::kill( ::getpid( ), SIGUSR1 );
			while( g_counted_signals.load( ) == 0 ) {
				std::this_thread::sleep_for( 1ms );
			}
		} };
		d.wait_for_signals( { SIGUSR1 } );
		raiser.join( );
		daw_ensure( g_counted_signals.load( ) == 1 );
	}

	void one_signal_stops_every_daemon( ) {
		daw::mhttpd::daemon d1{ { }, hello };
		daw::mhttpd::daemon d2{ { }, hello };
		auto waiter = std::jthread{ [&] {
			d2.wait_for_signals( { SIGUSR1 } );
		} };
		auto raiser = std::jthread{ [&] {
			wait_until_handler( SIGUSR1, std::not_fn( is_default ) );
			// Give the other daemon time to subscribe as well
			std::this_thread::sleep_for( 200ms );
			::kill( ::getpid( ), SIGUSR1 );
		} };
		d1.wait_for_signals( { SIGUSR1 } );
		waiter.join( );
		raiser.join( );
		daw_ensure( http( d1.port( ), "GET", "/" ).status == 0 );
		daw_ensure( http( d2.port( ), "GET", "/" ).status == 0 );
		daw_ensure( is_default( current_handler( SIGUSR1 ) ) );
	}

	void wait_for_uncatchable_signal_throws( ) {
		daw::mhttpd::daemon d{ { }, hello };
		for( int sig : { SIGKILL, SIGSTOP, 0, 9999 } ) {
			auto threw = false;
			try {
				d.wait_for_signals( { SIGUSR1, sig } );
			} catch( std::invalid_argument const & ) { threw = true; }
			daw_ensure( threw );
			// Nothing was installed, and the daemon still runs
			daw_ensure( is_default( current_handler( SIGUSR1 ) ) );
			daw_ensure( http( d.port( ), "GET", "/" ).body == "hello" );
		}
	}

	void wait_for_signals_after_stop_returns( ) {
		daw::mhttpd::daemon d{ { }, hello };
		d.stop( );
		d.wait_for_signals( { SIGUSR1 } );
		// The subscription made after stopping is dropped again
		daw_ensure( is_default( current_handler( SIGUSR1 ) ) );
	}

	// Waits until the server no longer accepts connections
	void wait_until_stopped( std::uint16_t port ) {
		while( http( port, "GET", "/" ).status != 0 ) {
			std::this_thread::sleep_for( 1ms );
		}
	}

	void add_stop_signals_stops_without_waiting( ) {
		daw::mhttpd::daemon d{ { }, hello };
		auto const port = d.port( );
		d.add_stop_signals( { SIGUSR1, SIGUSR2 } ); // does not block
		daw_ensure( not is_default( current_handler( SIGUSR1 ) ) );
		daw_ensure( http( port, "GET", "/" ).body == "hello" );
		::kill( ::getpid( ), SIGUSR2 );
		wait_until_stopped( port );
		// Stopping restores both
		wait_until_handler( SIGUSR1, is_default );
		daw_ensure( is_default( current_handler( SIGUSR2 ) ) );
	}

	void add_stop_signals_then_wait( ) {
		daw::mhttpd::daemon d{ { }, hello };
		auto const port = d.port( );
		d.add_stop_signals( { SIGUSR1 } );
		auto raiser = std::jthread{ [] {
			std::this_thread::sleep_for( 50ms );
			::kill( ::getpid( ), SIGUSR1 );
		} };
		// Returns once the stopper thread has stopped the server
		d.wait( );
		daw_ensure( http( port, "GET", "/" ).status == 0 );
	}

	void add_stop_signals_makes_request_stop_stop( ) {
		daw::mhttpd::daemon d{ { }, [&d]( daw::mhttpd::request & ) {
			                      d.request_stop( );
			                      return daw::mhttpd::response::empty( 202 );
		                      } };
		auto const port = d.port( );
		d.add_stop_signals( { SIGUSR1 } );
		daw_ensure( http( port, "POST", "/shutdown" ).status == 202 );
		wait_until_stopped( port );
		wait_until_handler( SIGUSR1, is_default );
	}

	void add_then_remove_stop_signals( ) {
		install_counting_handler( SIGUSR1 );
		daw::mhttpd::daemon d{ { }, hello };
		auto const port = d.port( );
		d.add_stop_signals( { SIGUSR1, SIGUSR2 } );
		daw_ensure( current_handler( SIGUSR1 ) != &count_signal );
		d.remove_stop_signals( { SIGUSR1 } );
		daw_ensure( current_handler( SIGUSR1 ) == &count_signal );
		::kill( ::getpid( ), SIGUSR1 );
		while( g_counted_signals.load( ) == 0 ) {
			std::this_thread::sleep_for( 1ms );
		}
		daw_ensure( http( port, "GET", "/" ).body == "hello" );
		::kill( ::getpid( ), SIGUSR2 );
		wait_until_stopped( port );
		daw_ensure( g_counted_signals.load( ) == 1 );
	}

	void add_stop_signals_then_destroy( ) {
		{
			daw::mhttpd::daemon d{ { }, hello };
			d.add_stop_signals( { SIGUSR1 } );
		} // must not hang on the stopper thread
		daw_ensure( is_default( current_handler( SIGUSR1 ) ) );
	}

	void added_signal_ends_wait_for_signals( ) {
		daw::mhttpd::daemon d{ { }, hello };
		d.add_stop_signals( { SIGUSR1 } );
		auto raiser = std::jthread{ [] {
			wait_until_handler( SIGUSR2, std::not_fn( is_default ) );
			::kill( ::getpid( ), SIGUSR1 );
		} };
		d.wait_for_signals( { SIGUSR2 } );
		raiser.join( );
		daw_ensure( http( d.port( ), "GET", "/" ).status == 0 );
		daw_ensure( is_default( current_handler( SIGUSR1 ) ) );
		daw_ensure( is_default( current_handler( SIGUSR2 ) ) );
	}

	// -------------------------------------------------------------------------
	// unique_fd
	// -------------------------------------------------------------------------

	static_assert( not daw::mhttpd::fd_closer::is_valid( -1 ) );
	static_assert(
	  daw::mhttpd::fd_closer::is_valid( 0 ) ); // 0 is a valid descriptor

	// -------------------------------------------------------------------------
	// Response headers and ownership before sending
	// -------------------------------------------------------------------------

	void response_headers_readable_before_send( ) {
		auto r = daw::mhttpd::response::from_copy( daw::string_view{ "x" } )
		           .header( "Content-Type", "text/plain" )
		           .header( "X-Twice", "first" )
		           .header( "X-Twice", "second" );
		daw_ensure( r.header( "content-type" ) == "text/plain" );
		daw_ensure( r.header( "X-TWICE" ) == "first" );
		daw_ensure( not r.header( "X-Missing" ) );

		daw::mhttpd::daemon d{ { }, []( daw::mhttpd::request & ) {
			                      auto res = daw::mhttpd::response::from_copy(
			                                   daw::string_view{ "hi" } )
			                                   .header( "X-Twice", "first" )
			                                   .header( "X-Twice", "second" );
			                      // Still changeable after it is built
			                      if( res.header( "X-Twice" ) ) {
				                      res.status( 202 );
			                      }
			                      return res;
		                      } };
		auto const resp = http( d.port( ), "GET", "/" );
		daw_ensure( resp.status == 202 );
		daw_ensure( resp.body == "hi" );
		daw_ensure( resp.headers.find( "X-Twice: first" ) != std::string::npos );
		daw_ensure( resp.headers.find( "X-Twice: second" ) != std::string::npos );
	}

	void invalid_response_header_throws( ) {
		using namespace std::string_view_literals;
		auto const bad = std::vector<std::pair<std::string_view, std::string_view>>{
		  { ""sv, "v"sv },
		  { "X Bad"sv, "v"sv },
		  { "X:Bad"sv, "v"sv },
		  { "X-Ok"sv, "a\r\nInjected: 1"sv },
		  { "X-Ok"sv, "a\nb"sv },
		  { "X-Ok"sv, "a\0b"sv } };
		for( auto const &[name, value] : bad ) {
			auto threw = false;
			try {
				(void)daw::mhttpd::response::empty( ).header(
				  daw::string_view{ name.data( ), name.size( ) },
				  daw::string_view{ value.data( ), value.size( ) } );
			} catch( std::invalid_argument const & ) { threw = true; }
			daw_ensure( threw );
		}
	}

	// MHD sets Content-Length itself and rejects one from the application
	// when the response is sent
	void header_rejected_by_mhd_gives_500( ) {
		daw::mhttpd::daemon d{ { }, []( daw::mhttpd::request & ) {
			                      return daw::mhttpd::response::from_copy(
			                               daw::string_view{ "hello" } )
			                        .header( "Content-Length", "5" );
		                      } };
		daw_ensure( http( d.port( ), "GET", "/" ).status == 500 );
	}

	void unsent_response_frees_body( ) {
		auto token = std::make_shared<int>( 0 );
		auto const producer = [token]( daw::u64, std::span<std::byte> ) {
			return daw::mhttpd::stream_result::end( );
		};
		{
			auto r = daw::mhttpd::response::stream( producer );
			daw_ensure( token.use_count( ) == 3 ); // token, producer, r
		}
		daw_ensure( token.use_count( ) == 2 );
		{
			auto r = daw::mhttpd::response::stream( producer );
			auto moved = std::move( r );
			daw_ensure( token.use_count( ) == 3 );
			// Assigning over a response frees the body it held
			moved = daw::mhttpd::response::empty( );
			daw_ensure( token.use_count( ) == 2 );
		}
		daw_ensure( token.use_count( ) == 2 );
	}

	void unique_fd_ownership( ) {
		auto const is_open = []( int fd ) {
			return ::fcntl( fd, F_GETFD ) != -1;
		};
		auto const open_null = [] {
			return daw::mhttpd::unique_fd{
			  ::open( "/dev/null", O_RDONLY | O_CLOEXEC ) };
		};

		auto raw = -1;
		{
			auto fd = open_null( );
			daw_ensure( fd );
			raw = fd.get( );
			daw_ensure( is_open( raw ) );
		}
		daw_ensure( not is_open( raw ) ); // closed on destruction

		{
			auto fd = open_null( );
			raw = fd.get( );
			daw_ensure( fd.release( ) == raw );
			daw_ensure( not fd );
		}
		daw_ensure( is_open( raw ) ); // release gave up ownership
		::close( raw );

		{
			auto empty = daw::mhttpd::unique_fd{ };
			daw_ensure( not empty );
			daw_ensure( empty.get( ) == -1 );
		}
		{
			// Descriptor 0 is held, not treated as empty; release it so stdin
			// stays open
			auto zero = daw::mhttpd::unique_fd{ 0 };
			daw_ensure( zero );
			daw_ensure( zero.release( ) == 0 );
		}
		{
			auto failed =
			  daw::mhttpd::unique_fd{ ::open( "/no/such/file", O_RDONLY ) };
			daw_ensure( not failed );
		}

		// The response owns the descriptor and closes it when destroyed
		{
			auto fd = open_null( );
			raw = fd.get( );
			{
				auto r = daw::mhttpd::response::from_fd( std::move( fd ), 0U, 0U );
				daw_ensure( not fd );
				daw_ensure( is_open( raw ) );
			}
			daw_ensure( not is_open( raw ) );
		}
	}
} // namespace

int main( int argc, char **argv ) {
	struct test_case {
		daw::string_view name;
		void ( *fn )( );
	};
	static constexpr test_case tests[] = {
	  { "sync_handler_with_header", &sync_handler_with_header },
	  { "response_headers_readable_before_send",
	    &response_headers_readable_before_send },
	  { "invalid_response_header_throws", &invalid_response_header_throws },
	  { "header_rejected_by_mhd_gives_500", &header_rejected_by_mhd_gives_500 },
	  { "unsent_response_frees_body", &unsent_response_frees_body },
	  { "request_accessors", &request_accessors },
	  { "post_body_echo", &post_body_echo },
	  { "post_large_body_echo", &post_large_body_echo },
	  { "mutable_body_in_place", &mutable_body_in_place },
	  { "take_body_moves_buffer", &take_body_moves_buffer },
	  { "body_too_large_content_length", &body_too_large_content_length },
	  { "body_too_large_chunked", &body_too_large_chunked },
	  { "static_response", &static_response },
	  { "stream_response", &stream_response },
	  { "responder_completed_from_another_thread",
	    &responder_completed_from_another_thread },
	  { "dropped_responder_gives_500", &dropped_responder_gives_500 },
	  { "handler_exception_gives_500", &handler_exception_gives_500 },
	  { "second_send_is_rejected", &second_send_is_rejected },
	  { "race_between_send_and_suspend", &race_between_send_and_suspend },
	  { "stop_answers_suspended_requests_with_503",
	    &stop_answers_suspended_requests_with_503 },
	  { "stop_answers_requests_suspended_on_every_thread",
	    &stop_answers_requests_suspended_on_every_thread },
	  { "coroutine_handler_completes_synchronously",
	    &coroutine_handler_completes_synchronously },
	  { "coroutine_handler_resumed_on_another_thread",
	    &coroutine_handler_resumed_on_another_thread },
	  { "coroutine_handler_exception_gives_500",
	    &coroutine_handler_exception_gives_500 },
	  { "abandoned_completion_throws", &abandoned_completion_throws },
	  { "sync_wait_deep_chain", &sync_wait_deep_chain },
	  { "sync_wait_propagates_exception", &sync_wait_propagates_exception },
	  { "coroutine_outlives_daemon", &coroutine_outlives_daemon },
	  { "stream_body_bypasses_buffer_limit", &stream_body_bypasses_buffer_limit },
	  { "read_body_echo", &read_body_echo },
	  { "read_body_large_echo", &read_body_large_echo },
	  { "read_body_then_mutate_and_take", &read_body_then_mutate_and_take },
	  { "read_body_too_large_gives_413", &read_body_too_large_gives_413 },
	  { "early_response_without_reading_body",
	    &early_response_without_reading_body },
	  { "backpressure_while_coroutine_busy", &backpressure_while_coroutine_busy },
	  { "respond_before_body_finished", &respond_before_body_finished },
	  { "client_disconnect_aborts_body", &client_disconnect_aborts_body },
	  { "request_stop_from_handler", &request_stop_from_handler },
	  { "wait_returns_when_stopped_elsewhere",
	    &wait_returns_when_stopped_elsewhere },
	  { "request_stop_before_wait", &request_stop_before_wait },
	  { "request_method", &request_method },
	  { "unique_fd_ownership", &unique_fd_ownership },
	  { "keep_alive_every_handler_style", &keep_alive_every_handler_style },
	  { "early_response_with_body_closes", &early_response_with_body_closes },
	  { "remote_address_ipv4", &remote_address_ipv4 },
	  { "bind_ipv4_loopback", &bind_ipv4_loopback },
	  { "bind_ipv6_loopback", &bind_ipv6_loopback },
	  { "bind_dual_stack", &bind_dual_stack },
	  { "bind_invalid_address_throws", &bind_invalid_address_throws },
	  { "connection_limit_queues_extra", &connection_limit_queues_extra },
	  { "per_ip_connection_limit_closes_extra",
	    &per_ip_connection_limit_closes_extra },
	  { "connection_limit_below_pool_throws",
	    &connection_limit_below_pool_throws },
	  { "thread_count_resolved", &thread_count_resolved },
	  { "single_thread_serves_many_keep_alive",
	    &single_thread_serves_many_keep_alive },
	  { "wait_for_signals_stops_on_signal", &wait_for_signals_stops_on_signal },
	  { "remove_stop_signals_restores_handler",
	    &remove_stop_signals_restores_handler },
	  { "second_signal_during_shutdown_gets_original_handler",
	    &second_signal_during_shutdown_gets_original_handler },
	  { "one_signal_stops_every_daemon", &one_signal_stops_every_daemon },
	  { "wait_for_uncatchable_signal_throws",
	    &wait_for_uncatchable_signal_throws },
	  { "wait_for_signals_after_stop_returns",
	    &wait_for_signals_after_stop_returns },
	  { "add_stop_signals_stops_without_waiting",
	    &add_stop_signals_stops_without_waiting },
	  { "add_stop_signals_then_wait", &add_stop_signals_then_wait },
	  { "add_stop_signals_makes_request_stop_stop",
	    &add_stop_signals_makes_request_stop_stop },
	  { "add_then_remove_stop_signals", &add_then_remove_stop_signals },
	  { "add_stop_signals_then_destroy", &add_stop_signals_then_destroy },
	  { "added_signal_ends_wait_for_signals",
	    &added_signal_ends_wait_for_signals },
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
