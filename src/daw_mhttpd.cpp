#include "daw/mhttpd/daw_mhttpd.h"
#include "daw/mhttpd/daw_mhttpd_log.h"
#include "daw_mhttpd_compression.h"

#include <daw/daw_as.h>
#include <daw/daw_ensure.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <map>
#include <microhttpd.h>
#include <mutex>
#include <netinet/in.h>
#include <new>
#include <sched.h>
#include <shared_mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace daw::mhttpd {
	// ---------------------------------------------------------------------------
	// response
	// ---------------------------------------------------------------------------

	namespace {

		MHD_Response *check( MHD_Response *r ) {
			if( r == nullptr ) {
				throw std::bad_alloc{ };
			}
			return r;
		}

		struct stream_holder {
			void *producer;
			daw::fp_t<daw::i64( void *, daw::u64, std::span<std::byte> )> read;
			daw::fp_t<void( void * ) noexcept> free_fn;
		};

		// Signature fixed by MHD_ContentReaderCallback
		ssize_t stream_read( void *cls, daw::u64::value_type pos, char *buf,
		                     std::size_t max ) {
			auto *h = as<stream_holder *>( cls );
			auto const n =
			  h->read( h->producer, pos, as_writable_bytes( std::span( buf, max ) ) );
			if( n == stream_result::end( ).raw( ) ) {
				return MHD_CONTENT_READER_END_OF_STREAM;
			}
			if( n < 0 or n > max ) {
				return MHD_CONTENT_READER_END_WITH_ERROR;
			}
			return as<ssize_t>( n.value( ) );
		}

		void stream_free( void *cls ) {
			auto *h = as<stream_holder *>( cls );
			h->free_fn( h->producer );
			delete h;
		}

		// RFC 9110 section 5.6.2
		[[nodiscard]] constexpr bool is_token( daw::string_view s ) noexcept {
			if( s.empty( ) ) {
				return false;
			}
			return std::ranges::all_of( s, []( char c ) {
				return ( c >= 'a' and c <= 'z' ) or ( c >= 'A' and c <= 'Z' ) or
				       ( c >= '0' and c <= '9' ) or
				       std::string_view{ "!#$%&'*+-.^_`|~" }.contains( c );
			} );
		}

		[[nodiscard]] constexpr bool is_header_value( daw::string_view s ) noexcept {
			return std::ranges::none_of( s, []( char c ) {
				return c == '\r' or c == '\n' or c == '\0';
			} );
		}

		[[nodiscard]] constexpr char ascii_lower( char c ) noexcept {
			return ( c >= 'A' and c <= 'Z' ) ? as<char>( c - 'A' + 'a' ) : c;
		}

		[[nodiscard]] constexpr bool iequals( daw::string_view a,
		                                      daw::string_view b ) noexcept {
			return std::ranges::equal( a, b, []( char x, char y ) {
				return ascii_lower( x ) == ascii_lower( y );
			} );
		}

	} // namespace

	static_assert( std::is_nothrow_move_constructible_v<response> and
	               std::is_nothrow_move_assignable_v<response> );

	response response::empty( unsigned status ) {
		return response{ empty_body{ }, status };
	}

	response response::from_static( std::span<std::byte const> data,
	                                unsigned status ) {
		return response{ static_body{ data }, status };
	}

	response response::from_static( daw::string_view data, unsigned status ) {
		return from_static( std::as_bytes( std::span{ data } ), status );
	}

	response response::from_copy( std::span<std::byte const> data,
	                              unsigned status ) {
		return from_owned( std::vector<std::byte>( data.begin( ), data.end( ) ),
		                   status );
	}

	response response::from_copy( daw::string_view data, unsigned status ) {
		return from_copy( std::as_bytes( std::span{ data } ), status );
	}

	response response::from_owned_impl( std::span<std::byte const> bytes,
	                                    void *holder, free_function free_fn,
	                                    unsigned status ) {
		return response{ owned_body{ bytes, erased_owner{ holder, free_fn } },
		                 status };
	}

	// unique_ptr only calls this for valid descriptors
	void fd_closer::operator( )( int fd ) noexcept {
		::close( fd );
	}

	response response::from_fd( unique_fd fd, daw::u64 offset, daw::u64 size,
	                            unsigned status ) {
		return response{ fd_body{ std::move( fd ), offset, size }, status };
	}

	response response::stream_impl( daw::u64 size, std::size_t block_size,
	                                void *holder, read_function read,
	                                free_function free_fn, unsigned status ) {
		return response{
		  stream_body{ erased_owner{ holder, free_fn }, read, size, block_size },
		  status };
	}

	response &response::header( daw::string_view name,
	                            daw::string_view value ) & {
		if( not is_token( name ) or not is_header_value( value ) ) {
			throw std::invalid_argument{ "daw::mhttpd: invalid response header" };
		}
		m_headers.emplace_back( std::string{ name }, std::string{ value } );
		return *this;
	}

	std::optional<daw::string_view>
	response::header( daw::string_view name ) const noexcept {
		for( auto const &[n, v] : m_headers ) {
			if( iequals( n, name ) ) {
				return daw::string_view{ v };
			}
		}
		return std::nullopt;
	}

	MHD_Response *response::into_native( ) && {
		auto *r = std::visit(
		  []<typename Body>( Body &body ) -> MHD_Response * {
			  if constexpr( std::same_as<Body, empty_body> ) {
				  return check( MHD_create_response_empty( MHD_RF_NONE ) );
			  } else if constexpr( std::same_as<Body, static_body> ) {
				  return check( MHD_create_response_from_buffer_static(
				    body.bytes.size( ), body.bytes.data( ) ) );
			  } else if constexpr( std::same_as<Body, owned_body> ) {
				  auto *result = check(
				    MHD_create_response_from_buffer_with_free_callback_cls(
				      body.bytes.size( ),
				      body.bytes.data( ),
				      body.owner.free_fn( ),
				      body.owner.get( ) ) );
				  body.owner.release( );
				  return result;
			  } else if constexpr( std::same_as<Body, fd_body> ) {
				  auto *result = MHD_create_response_from_fd_at_offset64(
				    body.size.value( ), body.fd.get( ), body.offset.value( ) );
				  if( result == nullptr ) {
					  // body.fd still owns the descriptor and closes it
					  throw std::runtime_error{
					    "daw::mhttpd: MHD_create_response_from_fd_at_offset64 failed" };
				  }
				  (void)body.fd.release( ); // MHD closes it from here on
				  return result;
			  } else {
				  static_assert( std::same_as<Body, stream_body> );
				  auto holder = std::unique_ptr<stream_holder>( new stream_holder{
				    body.producer.get( ), body.read, body.producer.free_fn( ) } );
				  auto *result = check( MHD_create_response_from_callback(
				    body.size.value( ),
				    body.block_size,
				    &stream_read,
				    holder.get( ),
				    &stream_free ) );
				  // stream_free frees both from here on
				  (void)holder.release( );
				  body.producer.release( );
				  return result;
			  }
		  },
		  m_body );
		// MHD owns the body now; destroying `r` frees it
		try {
			for( auto const &[name, value] : m_headers ) {
				if( MHD_add_response_header( r, name.c_str( ), value.c_str( ) ) !=
				    MHD_YES ) {
					throw std::invalid_argument{ std::format(
					  "daw::mhttpd: MHD rejected the response header {}", name ) };
				}
			}
		} catch( ... ) {
			MHD_destroy_response( r );
			throw;
		}
		return r;
	}

	// ---------------------------------------------------------------------------
	// request
	// ---------------------------------------------------------------------------

	namespace {

		std::optional<daw::string_view>
		lookup( MHD_Connection *c, MHD_ValueKind kind, daw::string_view name ) {
			char const *value = nullptr;
			std::size_t size = 0;
			if( MHD_lookup_connection_value_n(
			      c, kind, name.data( ), name.size( ), &value, &size ) != MHD_YES ) {
				return std::nullopt;
			}
			return daw::string_view{ value != nullptr ? value : "", size };
		}

	} // namespace

	std::optional<daw::string_view>
	request::header( daw::string_view name ) const {
		return lookup( m_connection, MHD_HEADER_KIND, name );
	}

	std::optional<daw::string_view>
	request::query( daw::string_view name ) const {
		return lookup( m_connection, MHD_GET_ARGUMENT_KIND, name );
	}

	std::optional<daw::string_view>
	request::cookie( daw::string_view name ) const {
		return lookup( m_connection, MHD_COOKIE_KIND, name );
	}

	std::optional<daw::string_view>
	request::path_param( daw::string_view name ) const noexcept {
		for( auto const &[key, value] : m_path_params ) {
			if( key == name ) {
				return value;
			}
		}
		return std::nullopt;
	}

	sockaddr const *request::remote_sockaddr( ) const noexcept {
		auto const *info = MHD_get_connection_info(
		  m_connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS );
		return info != nullptr ? info->client_addr : nullptr;
	}

	std::string request::remote_address( ) const {
		auto const *addr = remote_sockaddr( );
		if( addr == nullptr ) {
			return { };
		}
		char buf[INET6_ADDRSTRLEN]{ };
		switch( addr->sa_family ) {
		case AF_INET: {
			auto v4 = sockaddr_in{ };
			std::memcpy( &v4, addr, sizeof v4 );
			if( ::inet_ntop( AF_INET, &v4.sin_addr, buf, sizeof buf ) == nullptr ) {
				return { };
			}
			break;
		}
		case AF_INET6: {
			auto v6 = sockaddr_in6{ };
			std::memcpy( &v6, addr, sizeof v6 );
			auto const ok =
			  IN6_IS_ADDR_V4MAPPED( &v6.sin6_addr )
			    ? ::inet_ntop( AF_INET, &v6.sin6_addr.s6_addr[12], buf, sizeof buf )
			    : ::inet_ntop( AF_INET6, &v6.sin6_addr, buf, sizeof buf );
			if( ok == nullptr ) {
				return { };
			}
			break;
		}
		default:
			return { };
		}
		return std::string{ buf };
	}

	// ---------------------------------------------------------------------------
	// Per-request state machine
	// ---------------------------------------------------------------------------
	//
	//   pending ──send()──────────────► ready      (handler thread queues
	//   directly) pending ──handler returns──► suspended ──send()──► ready (+
	//   MHD_resume_connection) any ──request completed──► done
	//
	// `claimed` makes completion single-shot across responder::send(), the
	// responder destructor and shutdown. Only the claimant writes `response`.
	//
	// Streamed bodies reuse `suspended` for backpressure: a chunk arriving while
	// no coroutine waits in next_chunk is left unconsumed and the connection is
	// suspended; next_chunk moves suspended -> pending and resumes it. MHD then
	// offers the same chunk again.
} // namespace daw::mhttpd

namespace daw::mhttpd {
	namespace detail {

		// CPUs this process may run on. hardware_concurrency( ) counts every
		// online CPU, ignoring the affinity mask (taskset, cpusets).
		[[nodiscard]] unsigned available_cpus( ) noexcept {
			auto set = cpu_set_t{ };
			if( ::sched_getaffinity( 0, sizeof set, &set ) == 0 ) {
				if( auto const n = CPU_COUNT( &set ); n > 0 ) {
					return static_cast<unsigned>( n );
				}
			}
			return std::max( std::thread::hardware_concurrency( ), 1U );
		}

		// config::thread_pool_size, with 0 resolved to one thread per CPU. An
		// automatic pool is kept within connection_limit, which libmicrohttpd
		// splits between the threads.
		[[nodiscard]] unsigned resolve_thread_count( config const &config ) {
			if( config.thread_pool_size != 0 ) {
				return config.thread_pool_size;
			}
			auto const cpus = available_cpus( );
			return config.connection_limit != 0
			         ? std::min( cpus, config.connection_limit )
			         : cpus;
		}

		enum class request_state : daw::u8::value_type {
			pending,
			suspended,
			ready,
			done
		};

		// Content-Length, when present and valid
		[[nodiscard]] std::optional<daw::u64::value_type>
		content_length( request const &req ) {
			auto const len = req.header( MHD_HTTP_HEADER_CONTENT_LENGTH );
			if( not len ) {
				return std::nullopt;
			}
			daw::u64::value_type n = 0;
			auto [p, ec] =
			  std::from_chars( len->data( ), len->data( ) + len->size( ), n );
			if( ec != std::errc{ } or p != len->data( ) + len->size( ) ) {
				return std::nullopt;
			}
			return n;
		}

		// Links a suspended request into its slot's list; guarded by that slot's
		// mutex
		struct suspended_link {
			suspended_link *prev = nullptr;
			suspended_link *next = nullptr;
		};

		struct request_context : suspended_link,
		                         std::enable_shared_from_this<request_context> {
			request req;
			std::atomic<bool> claimed{ false };
			std::atomic<request_state> state{ request_state::pending };
			// Written by the claimant, read by the MHD thread after `ready`
			std::optional<response> pending;

			// Touched only on MHD threads.
			bool handler_invoked = false;
			bool body_too_large = false;
			bool registered = false;
			// The daemon_state slot it is registered in, while `registered`
			std::size_t slot = 0;

			// Set at creation
			bool stream_body = false;
			std::size_t max_body_size = 0;

			// Streamed body hand-off between MHD threads and the coroutine
			std::mutex body_mutex;
			std::coroutine_handle<> chunk_waiter; // guarded by body_mutex
			bool body_finished = false;           // guarded by body_mutex
			bool body_aborted = false;            // guarded by body_mutex
			// Written before the waiter is resumed, read by it
			std::optional<request::body_chunk> chunk;
			bool buffered_chunk_taken = false; // next_chunk on a buffered body

			// Wakes a coroutine waiting in next_chunk with request_aborted
			[[nodiscard]] std::coroutine_handle<> abort_body( ) {
				auto lock = std::lock_guard{ body_mutex };
				if( not body_finished ) {
					body_aborted = true;
				}
				chunk.reset( );
				return std::exchange( chunk_waiter, { } );
			}

			// Returns false if already completed. `r` may be empty (allocation
			// failed), in which case the connection is closed instead of answered.
			bool complete( std::optional<response> r ) noexcept {
				if( claimed.exchange( true, std::memory_order_acq_rel ) ) {
					return false;
				}
				pending = std::move( r );
				auto const prev =
				  state.exchange( request_state::ready, std::memory_order_acq_rel );
				// `claimed` makes completion single-shot, so nothing else made it ready
				daw_dbg_ensure( prev != request_state::ready );
				if( prev == request_state::suspended ) {
					MHD_resume_connection( req.m_connection );
				}
				return true;
			}
		};

		namespace {

			std::optional<response> try_empty( unsigned status ) noexcept {
				try {
					return response::empty( status );
				} catch( ... ) { return std::nullopt; }
			}

		} // namespace

		struct daemon_state;
		// Defined with signal_registry below
		void unsubscribe_all_signals( daemon_state &state ) noexcept;

		namespace {
			[[nodiscard]] spdlog::logger &server_log( ) {
				return log::get( log::component::server );
			}

			// The message of the exception being handled
			[[nodiscard]] std::string current_exception_message( ) {
				try {
					std::rethrow_exception( std::current_exception( ) );
				} catch( std::exception const &e ) { return e.what( ); } catch( ... ) {
				}
				return "an exception not derived from std::exception";
			}

			// One debug line per answered request
			void log_response( request const &req, unsigned status ) noexcept {
				try {
					auto &logger = server_log( );
					if( logger.should_log( spdlog::level::debug ) ) {
						logger.debug( "{} {} {} -> {}",
						              req.remote_address( ),
						              req.method_name( ),
						              req.url( ),
						              status );
					}
				} catch( ... ) {}
			}

			[[nodiscard]] char const *
			termination_reason( MHD_RequestTerminationCode code ) noexcept {
				switch( code ) {
				case MHD_REQUEST_TERMINATED_COMPLETED_OK:
					return "completed";
				case MHD_REQUEST_TERMINATED_WITH_ERROR:
					return "error";
				case MHD_REQUEST_TERMINATED_TIMEOUT_REACHED:
					return "timeout";
				case MHD_REQUEST_TERMINATED_DAEMON_SHUTDOWN:
					return "server shutdown";
				case MHD_REQUEST_TERMINATED_READ_ERROR:
					return "read error";
				case MHD_REQUEST_TERMINATED_CLIENT_ABORT:
					return "client closed the connection";
				default:
					return "unknown reason";
				}
			}

			// libmicrohttpd's own messages, which it would otherwise print to
			// stderr. They carry no severity and many are client caused (a client
			// hanging up mid-request), so they are warnings; failing to start is
			// logged separately as an error. Signature fixed by MHD_LogCallback.
			void mhd_log( void *, char const *format, va_list args ) noexcept {
				try {
					auto &logger = server_log( );
					if( not logger.should_log( spdlog::level::warn ) ) {
						return;
					}
					char buffer[1024];
					auto const n = std::vsnprintf( buffer, sizeof buffer, format, args );
					if( n < 0 ) {
						return;
					}
					auto message = std::string_view{
					  buffer, std::min( static_cast<std::size_t>( n ), sizeof buffer - 1 ) };
					while( message.ends_with( '\n' ) ) {
						message.remove_suffix( 1 );
					}
					logger.warn( "libmicrohttpd: {}", message );
				} catch( ... ) {}
			}
		} // namespace

		// The suspended requests of one MHD thread. Each thread has its own, so
		// suspending and resuming do not contend across threads.
		struct alignas( 64 ) suspended_slot {
			std::mutex mutex;
			suspended_link head; // circular; empty when head.next == &head

			suspended_slot( ) {
				head.prev = head.next = &head;
			}
			suspended_slot( suspended_slot const & ) = delete;
			suspended_slot &operator=( suspended_slot const & ) = delete;

			// The caller holds `mutex` for these
			[[nodiscard]] bool empty( ) const noexcept {
				return head.next == &head;
			}

			void push( suspended_link &link ) noexcept {
				link.prev = &head;
				link.next = head.next;
				head.next->prev = &link;
				head.next = &link;
			}

			static void remove( suspended_link &link ) noexcept {
				link.prev->next = link.next;
				link.next->prev = link.prev;
				link.prev = link.next = nullptr;
			}
		};

		struct daemon_state {
			request_handler handler;
			bool stream_body = false;
			std::size_t max_body_size = 0;
			// config::compression, with `enabled` false when this build lacks zstd
			compression_config compression{ };
			std::chrono::milliseconds shutdown_grace{ };
			MHD_Daemon *daemon = nullptr;

			// Compressed from_static bodies; nullopt when compressing did not
			// shrink one. from_static data does not change while the daemon runs,
			// so its address and size identify it. Entries are never removed, so
			// responses point into them; MHD has freed every response before this
			// state is destroyed.
			struct static_key {
				void const *data;
				std::size_t size;
				bool operator==( static_key const & ) const = default;
			};
			struct static_key_hash {
				[[nodiscard]] std::size_t
				operator( )( static_key const &k ) const noexcept {
					return std::hash<void const *>{ }( k.data ) ^
					       ( std::hash<std::size_t>{ }( k.size ) * 0x9e3779b97f4a7c15U );
				}
			};
			// Charged per entry on top of its bytes, so remembering bodies that did
			// not shrink also counts against compression_config::cache_size
			static constexpr std::size_t static_entry_overhead = 64;
			mutable std::shared_mutex static_cache_mutex;
			mutable std::unordered_map<static_key,
			                           std::optional<std::vector<std::byte>>,
			                           static_key_hash>
			  static_cache;                              // guarded by static_cache_mutex
			mutable std::size_t static_cache_bytes = 0; // guarded by static_cache_mutex
			std::uint16_t port{ };
			unsigned thread_count = 1;

			std::mutex mutex;
			// Notified, under `mutex`, when a slot empties while stopping
			std::condition_variable drained;
			// Read by suspend( ) under a slot's mutex, so stop( ) either finds a
			// request in its slot or the request sees this
			std::atomic<bool> stopping{ false };
			std::condition_variable stop_cv;
			bool stop_requested = false; // guarded by mutex
			bool stopped = false;        // guarded by mutex
			std::mutex stop_mutex;       // serializes stop( )
			// One per MHD thread; set before MHD starts
			std::unique_ptr<suspended_slot[]> slots;
			std::size_t slot_count = 0;
			std::atomic<std::size_t> next_slot{ 0 };
			// Set once a signal is subscribed; avoids touching the signal
			// registry for daemons that never use it
			std::atomic<bool> uses_signals{ false };
			// Started by add_stop_signals: waits in wait( ), so a requested stop
			// happens without the owner calling it
			std::jthread stopper; // guarded by mutex

			daemon_state( ) = default;
			daemon_state( daemon_state const & ) = delete;
			daemon_state &operator=( daemon_state const & ) = delete;

			// The daemon has stopped by now, so the stopper's wait( ) has returned or
			// is about to
			~daemon_state( ) {
				if( stopper.joinable( ) ) {
					stopper.join( );
				}
			}

			void start_stopper( ) {
				auto lock = std::lock_guard{ mutex };
				if( not stopper.joinable( ) ) {
					stopper = std::jthread{ [this] {
						wait( );
					} };
				}
			}

			using context_ptr = std::shared_ptr<request_context>;

			static MHD_Result on_access( void *cls, MHD_Connection *conn,
			                             char const *url, char const *method,
			                             char const *version, char const *upload_data,
			                             std::size_t *upload_size,
			                             void **req_cls ) noexcept {
				try {
					return as<daemon_state *>( cls )->access(
					  conn, url, method, version, upload_data, upload_size, req_cls );
				} catch( ... ) {
					try {
						server_log( ).error( "closing the connection for {} {}: {}",
						                     method,
						                     url,
						                     current_exception_message( ) );
					} catch( ... ) {}
					return MHD_NO;
				}
			}

			static void on_completed( void *cls, MHD_Connection *, void **req_cls,
			                          MHD_RequestTerminationCode code ) noexcept {
				auto *holder = as<context_ptr *>( *req_cls );
				if( holder == nullptr ) {
					return;
				}
				*req_cls = nullptr;
				auto &ctx = **holder;
				if( code != MHD_REQUEST_TERMINATED_COMPLETED_OK ) {
					try {
						server_log( ).debug( "{} {} ended early: {}",
						                     ctx.req.method_name( ),
						                     ctx.req.url( ),
						                     termination_reason( code ) );
					} catch( ... ) {}
				}
				auto waiter =
				  ctx.stream_body ? ctx.abort_body( ) : std::coroutine_handle<>{ };
				auto const prev =
				  ctx.state.exchange( request_state::done, std::memory_order_acq_rel );
				// MHD never completes a suspended connection; stop( ) resumes them all
				// before stopping MHD
				daw_dbg_ensure( prev != request_state::suspended );
				if( ctx.registered ) {
					as<daemon_state *>( cls )->unregister( *holder );
				}
				delete holder;
				// The coroutine's responder keeps the context alive; do not touch ctx
				if( waiter ) {
					waiter.resume( );
				}
			}

			MHD_Result access( MHD_Connection *conn, char const *url,
			                   char const *method, char const *version,
			                   char const *upload_data, std::size_t *upload_size,
			                   void **req_cls ) {
				auto *holder = as<context_ptr *>( *req_cls );

				// First call: headers only.
				if( holder == nullptr ) {
					auto ctx = std::make_shared<request_context>( );
					ctx->stream_body = stream_body;
					ctx->max_body_size = max_body_size;
					ctx->req.m_ctx = ctx.get( );
					ctx->req.m_connection = conn;
					ctx->req.m_method_name = method;
					ctx->req.m_method = parse_method( ctx->req.m_method_name );
					ctx->req.m_url = url;
					ctx->req.m_version = version;
					if( compression.enabled ) {
						if( auto ae = ctx->req.header( MHD_HTTP_HEADER_ACCEPT_ENCODING );
						    ae and accepts_coding( *ae, content_coding::zstd ) ) {
							ctx->req.m_response_coding = content_coding::zstd;
						}
					}
					*req_cls = new context_ptr{ ctx };

					if( stream_body ) {
						// The handler decides how much body it reads; it can answer
						// before reading any of it
						ctx->handler_invoked = true;
						invoke_handler( ctx );
						if( ctx->state.load( std::memory_order_acquire ) ==
						        request_state::ready and
						    has_body( ctx->req ) ) {
							// Early response: MHD discards the body and closes the
							// connection
							return queue_ready( conn, *ctx );
						}
						// Without a body, MHD calls again right away to say the request
						// is complete. Queueing there keeps the connection alive;
						// queueing now would make MHD close it.
						return MHD_YES;
					}

					if( auto n = content_length( ctx->req ); n and *n > max_body_size ) {
						// Early response: MHD discards the body and closes the
						// connection.
						ctx->handler_invoked = true;
						return queue( conn, ctx->req, response::empty( 413 ) );
					}
					return MHD_YES;
				}

				auto &ctx = **holder;
				if( ctx.registered ) {
					// Being called means we are no longer suspended
					unregister( *holder );
				}

				// Body chunk: only valid during this call.
				if( *upload_size != 0 ) {
					if( ctx.stream_body ) {
						return deliver_chunk( conn, *holder, upload_data, upload_size );
					}
					if( not ctx.body_too_large ) {
						auto &body = ctx.req.m_body;
						if( body.size( ) + *upload_size > max_body_size ) {
							ctx.body_too_large = true;
							std::vector<std::byte>{ }.swap( body );
						} else {
							auto const *p =
							  reinterpret_cast<std::byte const *>( upload_data );
							body.insert( body.end( ), p, p + *upload_size );
						}
					}
					*upload_size = 0;
					return MHD_YES;
				}

				// request fully received.
				if( ctx.stream_body ) {
					finish_body( ctx );
				} else if( not ctx.handler_invoked ) {
					ctx.handler_invoked = true;
					if( ctx.body_too_large ) {
						ctx.complete( try_empty( 413 ) );
					} else {
						invoke_handler( *holder );
					}
				}

				auto const state = ctx.state.load( std::memory_order_acquire );
				if( state == request_state::pending ) {
					suspend( conn, *holder ); // wait for the response
					return MHD_YES;
				}
				if( state != request_state::ready ) {
					return MHD_YES;
				}
				return queue_ready( conn, ctx );
			}

			// Whether a body follows the headers
			[[nodiscard]] static bool has_body( request const &req ) {
				if( req.header( MHD_HTTP_HEADER_TRANSFER_ENCODING ) ) {
					return true;
				}
				auto const len = req.header( MHD_HTTP_HEADER_CONTENT_LENGTH );
				return len and *len != "0";
			}

			// The responder shares ownership of the context with *req_cls
			void invoke_handler( context_ptr const &holder ) {
				try {
					handler( holder->req, responder{ holder } );
				} catch( ... ) {
					log_handler_exception( log::component::server, holder->req );
					holder->complete( try_empty( 500 ) );
				}
			}

			MHD_Result queue_ready( MHD_Connection *conn,
			                               request_context &ctx ) {
				daw_dbg_ensure( ctx.state.load( std::memory_order_acquire ) ==
				                request_state::ready );
				if( not ctx.pending ) {
					try {
						server_log( ).error(
						  "could not build a response for {} {}; closing the connection",
						  ctx.req.method_name( ),
						  ctx.req.url( ) );
					} catch( ... ) {}
					return MHD_NO;
				}
				auto r = std::move( *ctx.pending );
				ctx.pending.reset( );
				return queue( conn, ctx.req, std::move( r ) );
			}

			MHD_Result deliver_chunk( MHD_Connection *conn, context_ptr const &holder,
			                          char const *upload_data,
			                          std::size_t *upload_size ) {
				auto &ctx = *holder;
				if( ctx.claimed.load( std::memory_order_acquire ) ) {
					// A response exists and cannot be queued while body data is
					// pending, so read and drop the rest; the body is now truncated
					*upload_size = 0;
					if( auto waiter = ctx.abort_body( ) ) {
						waiter.resume( );
					}
					return MHD_YES;
				}
				auto waiter = std::coroutine_handle<>{ };
				{
					auto lock = std::lock_guard{ ctx.body_mutex };
					// MHD delivers no body data after the final call
					daw_dbg_ensure( not ctx.body_finished );
					if( not ctx.chunk_waiter ) {
						// Backpressure: leave the data unconsumed until next_chunk.
						// Under the lock so next_chunk sees `suspended` once it can
						// register.
						suspend( conn, holder );
						return MHD_YES;
					}
					waiter = std::exchange( ctx.chunk_waiter, { } );
					ctx.chunk.emplace( reinterpret_cast<std::byte const *>( upload_data ),
					                   *upload_size );
				}
				*upload_size = 0;
				waiter.resume( ); // the chunk is only valid during this call
				return MHD_YES;
			}

			static void finish_body( request_context &ctx ) {
				auto waiter = std::coroutine_handle<>{ };
				{
					auto lock = std::lock_guard{ ctx.body_mutex };
					if( ctx.body_finished ) {
						return;
					}
					ctx.body_finished = true;
					if( not ctx.body_aborted ) {
						ctx.chunk.reset( );
						waiter = std::exchange( ctx.chunk_waiter, { } );
					}
				}
				if( waiter ) {
					waiter.resume( ); // next_chunk yields std::nullopt
				}
			}

			// The response may be compressed for some client; config::compression
			// lists the conditions
			[[nodiscard]] bool encodable( response const &r ) const noexcept {
				if( not compression.enabled or r.compression( ) == compress::never ) {
					return false;
				}
				auto const status = r.status( );
				if( status < 200 or status == 204 or status == 206 or status == 304 ) {
					return false;
				}
				auto const *bytes = std::visit(
				  []<typename Body>( Body const &body )
				    -> std::span<std::byte const> const * {
					  if constexpr( requires { body.bytes; } ) {
						  return &body.bytes;
					  } else {
						  return nullptr;
					  }
				  },
				  r.m_body );
				if( bytes == nullptr or bytes->size( ) < compression.min_size or
				    r.header( "Content-Encoding" ) or r.header( "Content-Range" ) ) {
					return false;
				}
				auto const type = r.header( "Content-Type" );
				if( not type ) {
					return false;
				}
				try {
					return compression.compressible ? compression.compressible( *type )
					                                : is_compressible_type( *type );
				} catch( ... ) { return false; }
			}

			// Adds Accept-Encoding to Vary unless it, or "*", is already there
			static void add_vary( response &r ) {
				for( auto &[name, value] : r.m_headers ) {
					if( not iequals( name, "Vary" ) ) {
						continue;
					}
					auto rest = daw::string_view{ value };
					while( not rest.empty( ) ) {
						auto field = rest.pop_front_until( ',' );
						field.trim( );
						if( field == "*" or iequals( field, "Accept-Encoding" ) ) {
							return;
						}
					}
				}
				for( auto &[name, value] : r.m_headers ) {
					if( iequals( name, "Vary" ) ) {
						value += ", Accept-Encoding";
						return;
					}
				}
				r.m_headers.emplace_back( "Vary", "Accept-Encoding" );
			}

			// Compresses a from_static body the first time it is sent and reuses
			// the result after that. Returns the body to send, or nullopt to send
			// it as is.
			[[nodiscard]] std::optional<response::body_type>
			compress_static( std::span<std::byte const> bytes ) const {
				auto const key = static_key{ bytes.data( ), bytes.size( ) };
				auto const from_entry =
				  []( std::optional<std::vector<std::byte>> const &entry )
				  -> std::optional<response::body_type> {
					if( not entry ) {
						return std::nullopt;
					}
					return response::static_body{ *entry };
				};
				{
					auto lock = std::shared_lock{ static_cache_mutex };
					if( auto it = static_cache.find( key ); it != static_cache.end( ) ) {
						return from_entry( it->second );
					}
				}
				auto out = detail::zstd_compress( bytes, compression.zstd_level );
				if( not out ) {
					return std::nullopt; // not remembered, so it is tried again
				}
				if( out->size( ) >= bytes.size( ) ) {
					out.reset( );
				}
				auto const cost = static_entry_overhead + ( out ? out->size( ) : 0 );
				{
					auto lock = std::unique_lock{ static_cache_mutex };
					// Another thread may have compressed it meanwhile
					if( auto it = static_cache.find( key ); it != static_cache.end( ) ) {
						return from_entry( it->second );
					}
					if( cost <= compression.cache_size - static_cache_bytes ) {
						static_cache_bytes += cost;
						return from_entry(
						  static_cache.emplace( key, std::move( out ) ).first->second );
					}
				}
				// The cache is full: this response owns its copy
				if( not out ) {
					return std::nullopt;
				}
				return std::move( response::from_owned( std::move( *out ) ).m_body );
			}

			// Applies config::compression to `r`. A failure to compress sends the
			// body as is.
			void encode( request const &req, response &r ) const {
				if( not encodable( r ) ) {
					return;
				}
				add_vary( r );
				if( req.response_coding( ) != content_coding::zstd ) {
					return;
				}
				auto body = std::visit(
				  [&]<typename Body>( Body const &b ) -> std::optional<response::body_type> {
					  if constexpr( std::same_as<Body, response::static_body> ) {
						  return compress_static( b.bytes );
					  } else if constexpr( requires { b.bytes; } ) {
						  auto out = detail::zstd_compress( b.bytes, compression.zstd_level );
						  if( not out or out->size( ) >= b.bytes.size( ) ) {
							  return std::nullopt;
						  }
						  return std::move( response::from_owned( std::move( *out ) ).m_body );
					  } else {
						  return std::nullopt; // encodable( ) rules these out
					  }
				  },
				  r.m_body );
				if( not body ) {
					return;
				}
				r.m_body = std::move( *body );
				r.m_headers.emplace_back( "Content-Encoding",
				                          std::string{ to_string( content_coding::zstd ) } );
				for( auto &[name, value] : r.m_headers ) {
					if( iequals( name, "ETag" ) and value.starts_with( '"' ) ) {
						value.insert( 0, "W/" );
					}
				}
			}

			// Builds the MHD_Response from `r` and queues it. One MHD rejects, such
			// as for a header it does not allow, is logged and answered 500.
			MHD_Result queue( MHD_Connection *conn, request const &req,
			                  response r ) const {
				try {
					encode( req, r );
				} catch( ... ) {
					try {
						server_log( ).warn( "could not compress the response for {} {}: {}",
						                    req.method_name( ),
						                    req.url( ),
						                    current_exception_message( ) );
					} catch( ... ) {}
				}
				auto status = r.status( );
				MHD_Response *native = nullptr;
				try {
					native = std::move( r ).into_native( );
				} catch( ... ) {
					try {
						server_log( ).error(
						  "could not build the response for {} {}: {}; answering 500",
						  req.method_name( ),
						  req.url( ),
						  current_exception_message( ) );
					} catch( ... ) {}
					try {
						native = response::empty( 500 ).into_native( );
						status = 500;
					} catch( ... ) { return MHD_NO; }
				}
				log_response( req, status );
				auto const result = MHD_queue_response( conn, status, native );
				// MHD holds its own reference while it sends
				MHD_destroy_response( native );
				return result;
			}

			void suspend( MHD_Connection *conn, context_ptr const &holder ) {
				auto &ctx = *holder;
				// Every call to the access handler unregisters first, so being
				// registered here would mean suspending twice without a resume
				daw_dbg_ensure( not ctx.registered );
				MHD_suspend_connection( conn );

				ctx.slot = current_slot( );
				bool stop_now = false;
				{
					auto &slot = slots[ctx.slot];
					auto lock = std::lock_guard{ slot.mutex };
					slot.push( ctx );
					stop_now = stopping.load( std::memory_order_acquire );
				}
				ctx.registered = true;

				auto expected = request_state::pending;
				if( not ctx.state.compare_exchange_strong(
				      expected,
				      request_state::suspended,
				      std::memory_order_acq_rel ) ) {
					// send() won the race after we checked `pending`; it did not resume,
					// so we do.
					MHD_resume_connection( conn );
				} else if( stop_now ) {
					ctx.complete( shutdown_response( ) );
				}
			}

			// Uses the slot the request was suspended in, which may belong to
			// another thread when MHD completes requests during shutdown
			void unregister( context_ptr const &holder ) {
				holder->registered = false;
				auto &slot = slots[holder->slot];
				bool emptied = false;
				{
					auto lock = std::lock_guard{ slot.mutex };
					suspended_slot::remove( *holder );
					emptied = slot.empty( );
				}
				if( emptied and stopping.load( std::memory_order_acquire ) ) {
					auto lock = std::lock_guard{ mutex };
					drained.notify_all( );
				}
			}

			// This thread's slot, assigned the first time it suspends a request.
			// Threads beyond slot_count share slots, which is only slower.
			[[nodiscard]] std::size_t current_slot( ) noexcept {
				struct assignment {
					daemon_state const *owner = nullptr;
					std::size_t slot = 0;
				};
				thread_local auto mine = assignment{ };
				if( mine.owner != this ) {
					mine = { this, next_slot.fetch_add( 1, std::memory_order_relaxed ) };
				}
				return mine.slot % slot_count;
			}

			// Locks one slot at a time; the caller holds `mutex`
			[[nodiscard]] bool all_slots_empty( ) {
				for( std::size_t n = 0; n < slot_count; ++n ) {
					auto lock = std::lock_guard{ slots[n].mutex };
					if( not slots[n].empty( ) ) {
						return false;
					}
				}
				return true;
			}

			void init_slots( std::size_t count ) {
				slot_count = std::max( count, std::size_t{ 1 } );
				slots = std::make_unique<suspended_slot[]>( slot_count );
			}

			static std::optional<response> shutdown_response( ) noexcept {
				try {
					return response::empty( 503 ).header( "Connection", "close" );
				} catch( ... ) { return try_empty( 503 ); }
			}

			void request_stop( ) noexcept {
				{
					auto lock = std::lock_guard{ mutex };
					stop_requested = true;
				}
				stop_cv.notify_all( );
			}

			void wait( ) noexcept {
				{
					auto lock = std::unique_lock{ mutex };
					stop_cv.wait( lock, [&] {
						return stop_requested or stopped;
					} );
				}
				stop( );
			}

			void stop( ) noexcept {
				// First, so that a second signal during the grace period gets the
				// original disposition. Before the daemon == nullptr check, so a
				// subscription made after an earlier stop( ) is dropped too.
				if( uses_signals.load( std::memory_order_acquire ) ) {
					unsubscribe_all_signals( *this );
				}
				auto stop_lock = std::lock_guard{ stop_mutex };
				if( daemon == nullptr ) {
					return;
				}
				try {
					server_log( ).info( "stopping the server on port {}", port );
				} catch( ... ) {}
				auto const deadline =
				  std::chrono::steady_clock::now( ) + shutdown_grace;
				if( auto s = MHD_quiesce_daemon( daemon ); s != MHD_INVALID_SOCKET ) {
					::close( s );
				}

				std::vector<context_ptr> pending;
				stopping.store( true, std::memory_order_seq_cst );
				for( std::size_t n = 0; n < slot_count; ++n ) {
					auto &slot = slots[n];
					auto lock = std::lock_guard{ slot.mutex };
					for( auto *link = slot.head.next; link != &slot.head;
					     link = link->next ) {
						// Alive while listed: on_completed unregisters before freeing
						pending.push_back(
						  static_cast<request_context *>( link )->shared_from_this( ) );
					}
				}
				if( not pending.empty( ) ) {
					try {
						server_log( ).debug( "answering {} waiting requests with 503",
						                     pending.size( ) );
					} catch( ... ) {}
				}
				for( auto &ctx : pending ) {
					ctx->complete( shutdown_response( ) );
				}
				{
					std::unique_lock lock{ mutex };
					drained.wait_until( lock, deadline, [&] {
						return all_slots_empty( );
					} );
				}
				// Let queued responses finish sending. Idle keep-alive connections
				// hold this up until the deadline.
				while( std::chrono::steady_clock::now( ) < deadline ) {
					auto const *info =
					  MHD_get_daemon_info( daemon, MHD_DAEMON_INFO_CURRENT_CONNECTIONS );
					if( info == nullptr or info->num_connections == 0 ) {
						break;
					}
					std::this_thread::sleep_for( std::chrono::milliseconds{ 5 } );
				}
				MHD_stop_daemon( daemon );
				daemon = nullptr;
				try {
					server_log( ).info( "stopped the server on port {}", port );
				} catch( ... ) {}
				{
					auto lock = std::lock_guard{ mutex };
					stopped = true;
				}
				stop_cv.notify_all( );
			}
		};


		// -------------------------------------------------------------------------
		// Signals
		// -------------------------------------------------------------------------
		//
		// Signal dispositions are per process, so every daemon shares one
		// registry. The handler only writes the signal number to a pipe, which is
		// async-signal-safe; a watcher thread reads it and calls request_stop( ) on
		// the subscribed daemons. Unlike sigwait, this works whichever thread the
		// signal is delivered to, so the signals need not be blocked before MHD
		// starts its threads.
		namespace {
			// Write end of the pipe; set before the first handler is installed
			int g_signal_pipe = -1;

			void forward_signal( int sig ) {
				auto const saved_errno = errno;
				// Non-blocking: if the pipe is full, a stop is already pending
				(void)::write( g_signal_pipe, &sig, sizeof sig );
				errno = saved_errno;
			}

			// "SIGINT", or "signal 42"
			[[nodiscard]] std::string signal_name( int sig ) {
#if defined( __GLIBC__ )
				if( auto const *abbrev = ::sigabbrev_np( sig ) ) {
					return std::string{ "SIG" } + abbrev;
				}
#endif
				return "signal " + std::to_string( sig );
			}

			[[nodiscard]] bool can_catch( int sig ) noexcept {
				struct sigaction current{ };
				return sig != SIGKILL and sig != SIGSTOP and
				       ::sigaction( sig, nullptr, &current ) == 0;
			}

			class signal_registry {
			public:
				// Never destroyed: the watcher thread runs until the process exits
				static signal_registry &instance( ) {
					static auto *const registry = new signal_registry{ };
					return *registry;
				}

				void subscribe( daemon_state &state, std::span<int const> signals ) {
					for( int sig : signals ) {
						if( not can_catch( sig ) ) {
							throw std::invalid_argument{
							  "daw::mhttpd: cannot wait for signal " +
							  std::to_string( sig ) };
						}
					}
					auto lock = std::lock_guard{ m_mutex };
					state.uses_signals.store( true, std::memory_order_release );
					for( int sig : signals ) {
						auto [it, inserted] = m_signals.try_emplace( sig );
						auto &entry = it->second;
						if( inserted ) {
							struct sigaction action{ };
							action.sa_handler = &forward_signal;
							sigemptyset( &action.sa_mask );
							action.sa_flags = SA_RESTART;
							::sigaction( sig, &action, &entry.previous );
						}
						if( std::ranges::find( entry.daemons, &state ) ==
						    entry.daemons.end( ) ) {
							entry.daemons.push_back( &state );
						}
					}
				}

				void unsubscribe( daemon_state &state,
				                  std::span<int const> signals ) noexcept {
					auto lock = std::lock_guard{ m_mutex };
					for( int sig : signals ) {
						if( auto it = m_signals.find( sig ); it != m_signals.end( ) ) {
							remove( it, state );
						}
					}
				}

				void unsubscribe_all( daemon_state &state ) noexcept {
					auto lock = std::lock_guard{ m_mutex };
					for( auto it = m_signals.begin( ); it != m_signals.end( ); ) {
						it = remove( it, state );
					}
				}

			private:
				struct entry {
					struct sigaction previous{ };
					std::vector<daemon_state *> daemons;
				};

				signal_registry( ) {
					int fds[2];
					if( ::pipe2( fds, O_CLOEXEC ) != 0 ) {
						throw std::system_error{
						  errno, std::system_category( ), "daw::mhttpd: pipe2" };
					}
					// The handler must never block
					::fcntl( fds[1], F_SETFL, O_NONBLOCK );
					m_read = fds[0];
					g_signal_pipe = fds[1];
					std::thread{ [this] {
						watch( );
					} }.detach( );
				}

				void watch( ) noexcept {
					while( true ) {
						int sig = 0;
						auto const n = ::read( m_read, &sig, sizeof sig );
						if( n < 0 and errno == EINTR ) {
							continue;
						}
						if( n != sizeof sig ) {
							return;
						}
						// Under the lock, so a daemon cannot unsubscribe and be destroyed
						// while being stopped
						auto lock = std::lock_guard{ m_mutex };
						if( auto it = m_signals.find( sig ); it != m_signals.end( ) ) {
							try {
								server_log( ).info( "received signal {} ({}); stopping",
								                    sig,
								                    signal_name( sig ) );
							} catch( ... ) {}
							for( auto *state : it->second.daemons ) {
								state->request_stop( );
							}
						}
					}
				}

				// Restores the original disposition once no daemon waits on the
				// signal. Returns the next entry.
				std::map<int, entry>::iterator
				remove( std::map<int, entry>::iterator it, daemon_state &state ) {
					std::erase( it->second.daemons, &state );
					if( not it->second.daemons.empty( ) ) {
						return std::next( it );
					}
					::sigaction( it->first, &it->second.previous, nullptr );
					return m_signals.erase( it );
				}

				std::mutex m_mutex;
				std::map<int, entry> m_signals; // guarded by m_mutex
				int m_read = -1;
			};
		} // namespace

		void unsubscribe_all_signals( daemon_state &state ) noexcept {
			signal_registry::instance( ).unsubscribe_all( state );
		}
	} // namespace detail
} // namespace daw::mhttpd

namespace daw::mhttpd {
	// ---------------------------------------------------------------------------
	// request body
	// ---------------------------------------------------------------------------

	bool request::chunk_awaiter::await_suspend( std::coroutine_handle<> h ) {
		auto &ctx = *m_req->m_ctx;
		if( not ctx.stream_body ) {
			// Buffered: the whole body as one chunk, then the end
			if( not std::exchange( ctx.buffered_chunk_taken, true ) and
			    not m_req->m_body.empty( ) ) {
				ctx.chunk.emplace( m_req->m_body );
			} else {
				ctx.chunk.reset( );
			}
			return false;
		}
		auto lock = std::lock_guard{ ctx.body_mutex };
		if( ctx.body_aborted or ctx.body_finished ) {
			ctx.chunk.reset( );
			return false;
		}
		// Only one co_await req.next_chunk( ) may be pending at a time
		daw_dbg_ensure( not ctx.chunk_waiter );
		ctx.chunk_waiter = h;
		// If the connection was suspended for backpressure, let MHD offer the
		// pending chunk again. The response path never suspends before
		// body_finished, so `suspended` here always means backpressure.
		auto expected = detail::request_state::suspended;
		if( ctx.state.compare_exchange_strong( expected,
		                                       detail::request_state::pending,
		                                       std::memory_order_acq_rel ) ) {
			MHD_resume_connection( m_req->m_connection );
		}
		return true;
	}

	std::optional<request::body_chunk> request::chunk_awaiter::await_resume( ) {
		auto &ctx = *m_req->m_ctx;
		if( ctx.stream_body ) {
			auto lock = std::lock_guard{ ctx.body_mutex };
			if( ctx.body_aborted ) {
				throw request_aborted{ };
			}
		}
		return std::exchange( ctx.chunk, std::nullopt );
	}

	task<request::body_chunk> request::read_body( ) {
		if( not m_ctx->stream_body ) {
			co_return body_chunk{ m_body };
		}
		auto const max = m_ctx->max_body_size;
		while( auto chunk = co_await next_chunk( ) ) {
			if( m_body.size( ) + chunk->size( ) > max ) {
				throw body_too_large{ };
			}
			m_body.insert( m_body.end( ), chunk->begin( ), chunk->end( ) );
		}
		co_return body_chunk{ m_body };
	}

	// ---------------------------------------------------------------------------
	// responder
	// ---------------------------------------------------------------------------

	responder &responder::operator=( responder &&other ) noexcept {
		if( this != &other ) {
			if( m_ctx ) {
				m_ctx->complete( detail::try_empty( 500 ) );
			}
			m_ctx = std::move( other.m_ctx );
		}
		return *this;
	}

	responder::~responder( ) {
		if( m_ctx ) {
			m_ctx->complete( detail::try_empty( 500 ) );
		}
	}

	bool responder::send( response response ) {
		if( not m_ctx ) {
			return false;
		}
		auto ctx = std::move( m_ctx );
		return ctx->complete( std::move( response ) );
	}

	// ---------------------------------------------------------------------------
	// daemon
	// ---------------------------------------------------------------------------

	daemon::daemon( config const &config, request_handler handler )
	  : daemon{ config, std::move( handler ), false } {}

	// Routes buffer the body themselves when their handler needs it, so the
	// daemon always invokes the router on the headers
	daemon::daemon( config const &config, router routes )
	  : daemon{ config, std::move( routes ).into_handler( ), true } {}

	namespace {
		struct listen_address {
			sockaddr_storage storage{ };
			socklen_t size = 0;
			unsigned flags = 0; // MHD_USE_IPv6 or MHD_USE_DUAL_STACK
		};

		// Throws std::invalid_argument unless `address` is a numeric IPv4 or IPv6
		// address
		listen_address parse_bind_address( std::string const &address,
		                                   std::uint16_t port ) {
			auto result = listen_address{ };
			auto v4 = sockaddr_in{ };
			if( ::inet_pton( AF_INET, address.c_str( ), &v4.sin_addr ) == 1 ) {
				v4.sin_family = AF_INET;
				v4.sin_port = htons( port );
				std::memcpy( &result.storage, &v4, sizeof v4 );
				result.size = sizeof v4;
				return result;
			}
			auto v6 = sockaddr_in6{ };
			if( ::inet_pton( AF_INET6, address.c_str( ), &v6.sin6_addr ) == 1 ) {
				v6.sin6_family = AF_INET6;
				v6.sin6_port = htons( port );
				std::memcpy( &result.storage, &v6, sizeof v6 );
				result.size = sizeof v6;
				// "::" also accepts IPv4 clients
				result.flags = IN6_IS_ADDR_UNSPECIFIED( &v6.sin6_addr )
				                 ? MHD_USE_DUAL_STACK
				                 : MHD_USE_IPv6;
				return result;
			}
			throw std::invalid_argument{ "daw::mhttpd: invalid bind_address '" +
			                             address + "'" };
		}
	} // namespace

	daemon::daemon( config const &config, request_handler handler,
	                bool stream_body )
	  : m_state{ std::make_unique<detail::daemon_state>( ) } {
		auto &s = *m_state;
		s.handler = std::move( handler );
		s.stream_body = stream_body;
		s.max_body_size = config.max_body_size;
		s.shutdown_grace = config.shutdown_grace;
		s.compression = config.compression;
		s.compression.enabled = config.compression.enabled and
		                        coding_available( content_coding::zstd );
		auto const threads = detail::resolve_thread_count( config );
		s.thread_count = threads;
		s.init_slots( threads );

		using option_value = decltype( MHD_OptionItem::value );
		std::vector<MHD_OptionItem> opts{
		  // First, or MHD prints messages about the options before it to stderr
		  { MHD_OPTION_EXTERNAL_LOGGER,
		    reinterpret_cast<option_value>( &detail::mhd_log ),
		    nullptr },
		  { MHD_OPTION_NOTIFY_COMPLETED,
		    reinterpret_cast<option_value>( &detail::daemon_state::on_completed ),
		    &s },
		  { MHD_OPTION_CONNECTION_TIMEOUT,
		    as<option_value>( config.connection_timeout.count( ) ),
		    nullptr },
		};
		if( config.connection_limit != 0 ) {
			if( config.connection_limit < threads ) {
				throw std::invalid_argument{
				  "daw::mhttpd: connection_limit is less than thread_pool_size" };
			}
			opts.push_back( { MHD_OPTION_CONNECTION_LIMIT,
			                  as<option_value>( config.connection_limit ),
			                  nullptr } );
		}
		if( config.per_ip_connection_limit != 0 ) {
			opts.push_back( { MHD_OPTION_PER_IP_CONNECTION_LIMIT,
			                  as<option_value>( config.per_ip_connection_limit ),
			                  nullptr } );
		}
		if( config.connection_memory_limit != 0 ) {
			opts.push_back( { MHD_OPTION_CONNECTION_MEMORY_LIMIT,
			                  as<option_value>( config.connection_memory_limit ),
			                  nullptr } );
		}
		if( threads > 1 ) {
			opts.push_back( { MHD_OPTION_THREAD_POOL_SIZE,
			                  as<option_value>( threads ),
			                  nullptr } );
		}
		// Must outlive MHD_start_daemon
		auto bind = listen_address{ };
		if( not config.bind_address.empty( ) ) {
			bind = parse_bind_address( config.bind_address, config.port );
			opts.push_back( { MHD_OPTION_SOCK_ADDR_LEN,
			                  as<option_value>( bind.size ),
			                  &bind.storage } );
		}
		opts.push_back( { MHD_OPTION_END, 0, nullptr } );

		// A single thread uses poll( ): libmicrohttpd 1.0.1 to 1.0.5 can stop
		// serving keep-alive connections with one epoll thread once about 128 are
		// open. Pools are not affected.
		s.daemon = MHD_start_daemon( ( threads > 1 ? MHD_USE_AUTO_INTERNAL_THREAD
		                                           : MHD_USE_POLL_INTERNAL_THREAD ) |
		                               MHD_ALLOW_SUSPEND_RESUME |
		                               MHD_USE_ERROR_LOG | bind.flags,
		                             config.port,
		                             nullptr,
		                             nullptr,
		                             &detail::daemon_state::on_access,
		                             &s,
		                             MHD_OPTION_ARRAY,
		                             opts.data( ),
		                             MHD_OPTION_END );
		if( s.daemon == nullptr ) {
			detail::server_log( ).error( "could not start the server on {}:{}",
			                             config.bind_address.empty( )
			                               ? "*"
			                               : config.bind_address,
			                             config.port );
			throw std::runtime_error{ "daw::mhttpd: MHD_start_daemon failed" };
		}
		auto const *info =
		  MHD_get_daemon_info( s.daemon, MHD_DAEMON_INFO_BIND_PORT );
		s.port = info != nullptr ? info->port : config.port;
		detail::server_log( ).info(
		  "listening on {}:{} with {} thread{}",
		  config.bind_address.empty( ) ? "*" : config.bind_address,
		  s.port,
		  threads,
		  threads > 1 ? "s" : "" );
	}

	daemon::daemon( daemon && ) noexcept = default;

	daemon &daemon::operator=( daemon &&other ) noexcept {
		if( this != &other ) {
			stop( );
			m_state = std::move( other.m_state );
		}
		return *this;
	}

	daemon::~daemon( ) {
		stop( );
	}

	std::uint16_t daemon::port( ) const noexcept {
		return m_state ? m_state->port : 0;
	}

	unsigned daemon::thread_count( ) const noexcept {
		return m_state ? m_state->thread_count : 0;
	}

	void daemon::request_stop( ) noexcept {
		if( m_state ) {
			m_state->request_stop( );
		}
	}

	void daemon::wait( ) noexcept {
		if( m_state ) {
			m_state->wait( );
		}
	}

	void daemon::wait_for_signals( std::span<int const> signals ) {
		if( not m_state ) {
			return;
		}
		detail::signal_registry::instance( ).subscribe( *m_state, signals );
		// Stopping, here or elsewhere, unsubscribes
		m_state->wait( );
	}

	void daemon::add_stop_signals( std::span<int const> signals ) {
		if( not m_state ) {
			return;
		}
		detail::signal_registry::instance( ).subscribe( *m_state, signals );
		m_state->start_stopper( );
	}

	void daemon::remove_stop_signals( std::span<int const> signals ) noexcept {
		if( m_state and m_state->uses_signals.load( std::memory_order_acquire ) ) {
			detail::signal_registry::instance( ).unsubscribe( *m_state, signals );
		}
	}

	void daemon::stop( ) noexcept {
		if( m_state ) {
			m_state->stop( );
		}
	}

} // namespace daw::mhttpd
