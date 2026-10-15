// daw_mhttpd - a C++23 wrapper around libmicrohttpd.
//
// Threading model: MHD runs its own polling thread (or thread pool). Handlers
// are invoked on those threads. A handler receives a move-only responder that
// may be completed from *any* thread; if the handler returns without sending,
// the connection is suspended and resumed when responder::send() is called.
// Handlers may also be coroutines returning task<response>.
#pragma once

#include "daw/mhttpd/daw_mhttpd_task.h"

#include <daw/daw_as.h>
#include <daw/daw_copyable_function.h>
#include <daw/daw_format_string_view.h>
#include <daw/daw_integer.h>
#include <daw/daw_string_view.h>
#include <daw/daw_unique_ptr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

struct MHD_Response;
struct MHD_Connection;
struct MHD_Daemon;
struct sockaddr;

namespace daw::mhttpd::detail {
	struct request_context;
	struct daemon_state;
	struct route_table;
} // namespace daw::mhttpd::detail

namespace daw::mhttpd::log {
	// The parts of the library that log; see daw_mhttpd_log.h to configure them
	enum class component : std::size_t {
		// The daemon: start and stop, signals, libmicrohttpd's own messages,
		// exceptions from handlers, and one debug line per request
		server,
		// Route matching, 404, 405, 501 and 413 answers, and exceptions from
		// route handlers
		router,
		// The static file handler in examples/src/static_files.h
		static_files
	};
} // namespace daw::mhttpd::log

namespace daw::mhttpd {
	class router;
	class form_parser;

	// ---------------------------------------------------------------------------
	// File descriptors
	// ---------------------------------------------------------------------------

	// Deleter for a POSIX file descriptor. -1 is empty; any descriptor >= 0,
	// including 0, is valid and is closed.
	struct fd_closer {
		using pointer = int;
		static constexpr int default_pointer_value = -1;

		[[nodiscard]] static constexpr bool is_valid( int fd ) noexcept {
			return fd >= 0;
		}

		static void operator( )( int fd ) noexcept;
	};

	// Owns a file descriptor and closes it when destroyed
	using unique_fd = daw::unique_ptr<int, fd_closer>;

	// ---------------------------------------------------------------------------
	// response
	// ---------------------------------------------------------------------------

	// Result of one call to a streaming body producer.
	class stream_result {
	public:
		static constexpr stream_result bytes( std::size_t n ) noexcept {
			return stream_result{ daw::i64( n ) };
		}
		static constexpr stream_result end( ) noexcept {
			return stream_result{ -1 };
		}
		static constexpr stream_result error( ) noexcept {
			return stream_result{ -2 };
		}

		[[nodiscard]] constexpr daw::i64 raw( ) const noexcept {
			return m_value;
		}

	private:
		constexpr explicit stream_result( daw::i64 v ) noexcept
		  : m_value{ v } {}
		daw::i64 m_value;
	};

	// A producer writes up to out.size() bytes of body at offset pos.
	template<class F>
	concept StreamProducer =
	  std::is_invocable_r_v<stream_result, F &, daw::u64, std::span<std::byte>>;

	// ---------------------------------------------------------------------------
	// Content codings
	// ---------------------------------------------------------------------------

	// The content codings (RFC 9110 section 8.4.1) the daemon can compress a
	// response with
	enum class content_coding : daw::u8::value_type { identity, zstd };

	[[nodiscard]] constexpr daw::string_view
	to_string( content_coding c ) noexcept {
		switch( c ) {
		case content_coding::zstd:
			return "zstd";
		case content_coding::identity:
		default:
			return "identity";
		}
	}

	// Whether this build can compress with `c`; false for zstd when built with
	// DAW_MHTTPD_ENABLE_ZSTD=OFF. Always true for identity.
	[[nodiscard]] bool coding_available( content_coding c ) noexcept;

	// Whether an Accept-Encoding value (RFC 9110 section 12.5.3) accepts `c`:
	// listed with a non-zero q, or not listed and covered by a "*" with a
	// non-zero q. identity is accepted unless refused with q=0, directly or by
	// "*". Coding names are case-insensitive; an element with a malformed q is
	// treated as q=0.
	[[nodiscard]] bool accepts_coding( daw::string_view accept_encoding,
	                                   content_coding c ) noexcept;

	// The default for compression_config::compressible: text/*, JSON,
	// JavaScript, XML, any +json or +xml type (SVG among them), WebAssembly,
	// NDJSON, and uncompressed fonts. Parameters such as "; charset=utf-8" are
	// ignored and matching is case-insensitive.
	[[nodiscard]] bool is_compressible_type( daw::string_view content_type ) noexcept;

	// Per-response choice about config::compression
	enum class compress : daw::u8::value_type {
		// Compressed when config::compression allows it
		automatic,
		// Sent as is, e.g. a body that mixes a secret with text the client
		// controls (BREACH)
		never
	};

	// A response body plus the status and headers to send it with. Nothing is
	// handed to MHD until the daemon sends it, so the status and headers can be
	// read and changed until then.
	class response {
	public:
		static constexpr daw::u64 unknown_size = daw::u64::max( );

		response( ) = delete;
		response( response && ) noexcept = default;
		response &operator=( response && ) noexcept = default;
		response( response const & ) = delete;
		response &operator=( response const & ) = delete;
		~response( ) = default;

		// Body with no content.
		[[nodiscard]] static response empty( unsigned status = 204 );

		// Zero-copy; the caller guarantees `data` stays valid and unchanged for
		// as long as the daemon runs, e.g. a literal or data loaded at startup.
		// The daemon caches the compressed form by address and size.
		[[nodiscard]] static response from_static( std::span<std::byte const> data,
		                                           unsigned status = 200 );
		[[nodiscard]] static response from_static( daw::string_view data,
		                                           unsigned status = 200 );

		// Copies `data` into storage the response owns.
		[[nodiscard]] static response from_copy( std::span<std::byte const> data,
		                                         unsigned status = 200 );
		[[nodiscard]] static response from_copy( daw::string_view data,
		                                         unsigned status = 200 );

		// Zero-copy; takes ownership of a contiguous container (std::string,
		// std::vector<std::byte>, ...) which is destroyed when MHD is done with it.
		template<std::ranges::contiguous_range R>
		requires( not std::is_lvalue_reference_v<R> and
		          std::ranges::sized_range<R> )
		[[nodiscard]] static response from_owned( R &&owner,
		                                          unsigned status = 200 ) {
			auto *holder = new R( std::move( owner ) );
			auto bytes = std::as_bytes( std::span{ std::ranges::data( *holder ),
			                                       std::ranges::size( *holder ) } );
			return from_owned_impl(
			  bytes,
			  holder,
			  []( void *p ) noexcept {
				  delete static_cast<R *>( p );
			  },
			  status );
		}

		// Body is [offset, offset + size) of the file; MHD may use sendfile. The
		// response owns `fd` from here on.
		[[nodiscard]] static response from_fd( unique_fd fd, daw::u64 offset,
		                                       daw::u64 size,
		                                       unsigned status = 200 );

		// Body produced on demand. `size` may be unknown_size (chunked encoding).
		template<StreamProducer F>
		[[nodiscard]] static response
		stream( F &&producer, daw::u64 size = unknown_size,
		        std::size_t block_size = 16 * 1024, unsigned status = 200 ) {
			using Fn = std::decay_t<F>;
			auto *holder = new Fn( std::forward<F>( producer ) );
			return stream_impl(
			  size,
			  block_size,
			  holder,
			  []( void *p, daw::u64 pos, std::span<std::byte> out ) -> daw::i64 {
				  try {
					  return ( *static_cast<Fn *>( p ) )( pos, out ).raw( );
				  } catch( ... ) { return stream_result::error( ).raw( ); }
			  },
			  []( void *p ) noexcept {
				  delete static_cast<Fn *>( p );
			  },
			  status );
		}

		// Adds a header; adding a name again sends it twice. Throws
		// std::invalid_argument if `name` is not an HTTP token or `value` holds
		// CR, LF or NUL. Headers MHD manages itself, such as Content-Length, are
		// checked when the response is sent; one it rejects there is logged and
		// answered 500.
		response &header( daw::string_view name, daw::string_view value ) &;
		response &&header( daw::string_view name, daw::string_view value ) && {
			return std::move( header( name, value ) );
		}

		// The value of the first header named `name`, compared
		// case-insensitively
		[[nodiscard]] std::optional<daw::string_view>
		header( daw::string_view name ) const noexcept;

		response &status( unsigned code ) & noexcept {
			m_status = code;
			return *this;
		}
		response &&status( unsigned code ) && noexcept {
			return std::move( status( code ) );
		}

		[[nodiscard]] unsigned status( ) const noexcept {
			return m_status;
		}

		response &compression( compress c ) & noexcept {
			m_compression = c;
			return *this;
		}
		response &&compression( compress c ) && noexcept {
			return std::move( compression( c ) );
		}

		[[nodiscard]] compress compression( ) const noexcept {
			return m_compression;
		}

	private:
		friend struct detail::daemon_state;

		using free_function = void ( * )( void * ) noexcept;
		using read_function = daw::i64 ( * )( void *, daw::u64,
		                                      std::span<std::byte> );

		// Owns a type-erased object until release( ) hands it to MHD
		class erased_owner {
		public:
			erased_owner( void *ptr, free_function free_fn ) noexcept
			  : m_ptr{ ptr }
			  , m_free{ free_fn } {}
			erased_owner( erased_owner &&other ) noexcept
			  : m_ptr{ std::exchange( other.m_ptr, nullptr ) }
			  , m_free{ other.m_free } {}
			erased_owner &operator=( erased_owner &&other ) noexcept {
				if( this != &other ) {
					reset( );
					m_ptr = std::exchange( other.m_ptr, nullptr );
					m_free = other.m_free;
				}
				return *this;
			}
			erased_owner( erased_owner const & ) = delete;
			erased_owner &operator=( erased_owner const & ) = delete;
			~erased_owner( ) {
				reset( );
			}

			[[nodiscard]] void *get( ) const noexcept {
				return m_ptr;
			}
			[[nodiscard]] free_function free_fn( ) const noexcept {
				return m_free;
			}
			void release( ) noexcept {
				m_ptr = nullptr;
			}

		private:
			void reset( ) noexcept {
				if( m_ptr != nullptr ) {
					m_free( std::exchange( m_ptr, nullptr ) );
				}
			}

			void *m_ptr;
			free_function m_free;
		};

		struct empty_body {};
		struct static_body {
			std::span<std::byte const> bytes;
		};
		// `bytes` points into the object `owner` holds
		struct owned_body {
			std::span<std::byte const> bytes;
			erased_owner owner;
		};
		struct fd_body {
			unique_fd fd;
			daw::u64 offset;
			daw::u64 size;
		};
		struct stream_body {
			erased_owner producer;
			read_function read;
			daw::u64 size;
			std::size_t block_size;
		};
		using body_type =
		  std::variant<empty_body, static_body, owned_body, fd_body, stream_body>;

		response( body_type body, unsigned status ) noexcept
		  : m_body{ std::move( body ) }
		  , m_status{ status } {}

		static response from_owned_impl( std::span<std::byte const> bytes,
		                                 void *holder, free_function free_fn,
		                                 unsigned status );
		static response stream_impl( daw::u64 size, std::size_t block_size,
		                             void *holder, read_function read,
		                             free_function free_fn, unsigned status );

		// Builds the MHD_Response, which the caller then owns; the body belongs to
		// it from here on. Throws if MHD rejects the body or a header.
		[[nodiscard]] MHD_Response *into_native( ) &&;

		body_type m_body;
		std::vector<std::pair<std::string, std::string>> m_headers;
		unsigned m_status;
		compress m_compression = compress::automatic;
	};

	// ---------------------------------------------------------------------------
	// request
	// ---------------------------------------------------------------------------

	// ---------------------------------------------------------------------------
	// http_method
	// ---------------------------------------------------------------------------

	// The standard HTTP methods (RFC 9110 and RFC 5789). Anything else, such as
	// WebDAV's PROPFIND, is UNKNOWN; request::method_name( ) has the original.
	enum class http_method : daw::u8::value_type {
		GET,
		POST,
		HEAD,
		PUT,
		DELETE,
		CONNECT,
		OPTIONS,
		TRACE,
		PATCH,
		UNKNOWN
	};

	namespace pm_impl {
		[[nodiscard]] DAW_ATTRIB_INLINE constexpr std::uint64_t
		method_key( daw::string_view name ) {
			auto values = std::array<char, 8>{ };
			std::ranges::copy( name.data( ), name.data_end( ), values.data( ) );
			values[7] = as<char>( name.size( ) );
			return std::bit_cast<std::uint64_t>( values );
		}
	} // namespace pm_impl
	// http_method names are case-sensitive (RFC 9110 section 9.1): "get" is not
	// GET but an unrecognized method, which a server should answer with 501
	[[nodiscard]] constexpr http_method
	parse_method( daw::string_view name ) noexcept {
		if( name.size( ) > 7 ) {
			[[unlikely]] return http_method::UNKNOWN;
		}
		switch( pm_impl::method_key( name ) ) {
		[[likely]] case pm_impl::method_key( "GET" ):
			return http_method::GET;
		[[likely]] case pm_impl::method_key( "POST" ):
			return http_method::POST;
		case pm_impl::method_key( "HEAD" ):
			return http_method::HEAD;
		case pm_impl::method_key( "PUT" ):
			return http_method::PUT;
		case pm_impl::method_key( "DELETE" ):
			return http_method::DELETE;
		case pm_impl::method_key( "CONNECT" ):
			return http_method::CONNECT;
		case pm_impl::method_key( "OPTIONS" ):
			return http_method::OPTIONS;
		case pm_impl::method_key( "TRACE" ):
			return http_method::TRACE;
		case pm_impl::method_key( "PATCH" ):
			return http_method::PATCH;
		[[unlikely]] default:
			return http_method::UNKNOWN;
		}
	}

	[[nodiscard]] constexpr daw::string_view to_string( http_method m ) noexcept {
		switch( m ) {
		[[likely]] case http_method::GET:
			return "GET";
		[[likely]] case http_method::POST:
			return "POST";
		case http_method::HEAD:
			return "HEAD";
		case http_method::PUT:
			return "PUT";
		case http_method::DELETE:
			return "DELETE";
		case http_method::CONNECT:
			return "CONNECT";
		case http_method::OPTIONS:
			return "OPTIONS";
		case http_method::TRACE:
			return "TRACE";
		case http_method::PATCH:
			return "PATCH";
		case http_method::UNKNOWN:
		default:
			[[unlikely]] return "UNKNOWN";
		}
	}

	// Thrown by request::read_body when the body exceeds config::max_body_size.
	// A coroutine handler that lets it escape answers 413.
	class body_too_large : public std::runtime_error {
	public:
		body_too_large( )
		  : std::runtime_error{ "daw::mhttpd: request body too large" } {}
	};

	// Thrown by request::next_chunk when the rest of the body will never
	// arrive: the client disconnected, or a response was sent and the remaining
	// body is being discarded.
	class request_aborted : public std::runtime_error {
	public:
		request_aborted( )
		  : std::runtime_error{ "daw::mhttpd: request body aborted" } {}
	};

	// View of the incoming request. All string_views point into MHD's
	// connection memory and stay valid until the responder for this request is
	// completed.
	//
	// Body: coroutine handlers start as soon as the headers arrive and read the
	// body with next_chunk( ) or read_body( ). All other handlers start after
	// the body is buffered and use body( ).
	class request {
	public:
		using body_chunk = std::span<std::byte const>;

		class [[nodiscard]] chunk_awaiter {
		public:
			[[nodiscard]] bool await_ready( ) const noexcept {
				return false;
			}
			[[nodiscard]] bool await_suspend( std::coroutine_handle<> h );
			std::optional<body_chunk> await_resume( );

		private:
			friend class request;
			explicit chunk_awaiter( request *req ) noexcept
			  : m_req{ req } {}
			request *m_req;
		};

		[[nodiscard]] http_method method( ) const noexcept {
			return m_method;
		}
		// The method exactly as sent, e.g. for http_method::UNKNOWN
		[[nodiscard]] daw::string_view method_name( ) const noexcept {
			return m_method_name;
		}
		[[nodiscard]] daw::string_view url( ) const noexcept {
			return m_url;
		}
		[[nodiscard]] daw::string_view version( ) const noexcept {
			return m_version;
		}

		[[nodiscard]] std::optional<daw::string_view>
		header( daw::string_view name ) const;
		[[nodiscard]] std::optional<daw::string_view>
		query( daw::string_view name ) const;
		[[nodiscard]] std::optional<daw::string_view>
		cookie( daw::string_view name ) const;

		// A path parameter captured by a router pattern, e.g. "name" for
		// "/worlds/{name}"
		[[nodiscard]] std::optional<daw::string_view>
		path_param( daw::string_view name ) const noexcept;

		// The client's IP address as text, e.g. "192.0.2.7" or "2001:db8::1". An
		// IPv4 client of a dual-stack daemon is reported as IPv4, not as
		// "::ffff:192.0.2.7". Empty if MHD does not know the address.
		[[nodiscard]] std::string remote_address( ) const;

		// The client's socket address (sockaddr_in or sockaddr_in6), or nullptr
		[[nodiscard]] sockaddr const *remote_sockaddr( ) const noexcept;

		// The coding the daemon compresses an eligible response with: zstd when
		// config::compression is enabled and the client's Accept-Encoding accepts
		// it, otherwise identity. A handler serving precompressed files can use
		// it to pick one, and must then set Content-Encoding itself.
		[[nodiscard]] content_coding response_coding( ) const noexcept {
			return m_response_coding;
		}

		// co_await yields the next piece of the body, or std::nullopt at its end.
		// The chunk is only valid until the coroutine next suspends. While the
		// coroutine is not waiting here, MHD stops reading from the client.
		// Throws request_aborted if the body will not be completed.
		[[nodiscard]] chunk_awaiter next_chunk( ) noexcept {
			return chunk_awaiter{ this };
		}

		// Reads the whole body into body( ), bounded by config::max_body_size;
		// throws body_too_large past the limit
		[[nodiscard]] task<body_chunk> read_body( );

		// The buffered request body; for coroutine handlers, after read_body( )
		[[nodiscard]] std::span<std::byte const> body( ) const noexcept {
			return m_body;
		}
		[[nodiscard]] daw::string_view body_text( ) const noexcept {
			return { reinterpret_cast<char const *>( m_body.data( ) ),
			         m_body.size( ) };
		}

		// Mutable views of the buffered body, for parsing or decoding in place.
		// Same availability and lifetime as body( ). Streamed chunks from
		// next_chunk( ) point into MHD's buffer and stay read-only.
		[[nodiscard]] std::span<std::byte> mutable_body( ) noexcept {
			return m_body;
		}
		[[nodiscard]] std::span<char> mutable_body_text( ) noexcept {
			return { reinterpret_cast<char *>( m_body.data( ) ), m_body.size( ) };
		}

		// Moves the buffered body out, e.g. to transform it and answer with
		// response::from_owned without copying. body( ) is empty afterward.
		[[nodiscard]] std::vector<std::byte> take_body( ) noexcept {
			return std::exchange( m_body, { } );
		}

	private:
		friend struct detail::request_context;
		friend struct detail::daemon_state;
		friend struct detail::route_table;
		friend class form_parser;

		MHD_Connection *m_connection = nullptr;
		detail::request_context *m_ctx = nullptr;
		http_method m_method = http_method::UNKNOWN;
		content_coding m_response_coding = content_coding::identity;
		daw::string_view m_method_name;
		daw::string_view m_url;
		daw::string_view m_version;
		std::vector<std::byte> m_body;
		// Names are copied from the route; values point into m_url
		std::vector<std::pair<std::string, daw::string_view>> m_path_params;
	};

	// ---------------------------------------------------------------------------
	// responder
	// ---------------------------------------------------------------------------

	// Move-only, single-shot completion for one request. send() may be called
	// from any thread. Destroying an unsent responder answers 500.
	class responder {
	public:
		responder( responder && ) noexcept = default;
		responder &operator=( responder &&other ) noexcept;
		responder( responder const & ) = delete;
		responder &operator=( responder const & ) = delete;
		~responder( );

		// Returns false if the request was already completed (e.g. by shutdown).
		bool send( response response );

		[[nodiscard]] explicit operator bool( ) const noexcept {
			return m_ctx != nullptr;
		}

	private:
		friend struct detail::daemon_state;
		explicit responder( std::shared_ptr<detail::request_context> ctx ) noexcept
		  : m_ctx{ std::move( ctx ) } {}

		std::shared_ptr<detail::request_context> m_ctx;
	};

	// ---------------------------------------------------------------------------
	// daemon
	// ---------------------------------------------------------------------------

	// Handlers are invoked concurrently when the daemon runs more than one
	// thread, which is the default (config::thread_pool_size), so they must be
	// const-invocable; mutable state needs its own synchronization.
	using request_handler =
	  std::move_only_function<void( request &, responder ) const>;

	template<class F>
	concept AsyncHandler =
	  std::is_invocable_v<std::decay_t<F> const &, request &, responder>;

	template<class F>
	concept SyncHandler =
	  std::is_invocable_r_v<response, std::decay_t<F> const &, request &>;

	template<class F>
	concept CoroutineHandler =
	  std::is_invocable_v<std::decay_t<F> const &, request &> and
	  std::same_as<std::invoke_result_t<std::decay_t<F> const &, request &>,
	               task<response>>;
} // namespace daw::mhttpd

namespace daw::mhttpd::detail {
	// Logs the exception being handled as an error of `c`. Only call it inside
	// a catch block.
	void log_handler_exception( log::component c, request const &req ) noexcept;

	// Owns the handler through `fn` so a coroutine still running after the
	// daemon is destroyed does not touch a destroyed handler. Exceptions are
	// logged under `log_as`.
	template<typename Fn>
	detached run_coroutine_handler( std::shared_ptr<Fn const> fn, request &req,
	                                responder res, log::component log_as ) {
		auto result = std::optional<response>{ };
		auto too_large = false;
		try {
			result.emplace( co_await std::invoke( *fn, req ) );
		} catch( body_too_large const & ) { too_large = true; } catch( ... ) {
			log_handler_exception( log_as, req );
		}
		try {
			if( too_large ) {
				result.emplace( response::empty( 413 ) );
			}
			if( result ) {
				res.send( std::move( *result ) );
			}
		} catch( ... ) {}
		// otherwise res is destroyed unsent and answers 500
	}

	// log_as: the component that logs exceptions from a coroutine handler
	template<class F>
	request_handler
	make_request_handler( F &&f, log::component log_as = log::component::server ) {
		if constexpr( AsyncHandler<F> ) {
			return request_handler{ std::forward<F>( f ) };
		} else if constexpr( CoroutineHandler<F> ) {
			using Fn = std::decay_t<F>;
			return [fn = std::shared_ptr<Fn const>{ std::make_shared<Fn>(
			          std::forward<F>( f ) ) },
			        log_as]( request &req, responder res ) {
				run_coroutine_handler( fn, req, std::move( res ), log_as );
			};
		} else {
			return [fn = std::forward<F>( f )]( request &req, responder res ) {
				res.send( fn( req ) );
			};
		}
	}

} // namespace daw::mhttpd::detail

namespace daw::mhttpd {
	// Compression of response bodies. The daemon compresses a response when
	// all of these hold:
	//   * the response has a buffered body (from_static, from_copy, from_owned)
	//     of at least min_size bytes; files and streams are sent as is
	//   * its Content-Type passes `compressible`
	//   * it has no Content-Encoding, no Content-Range, and its status is not
	//     1xx, 204, 206 or 304
	//   * response::compression( ) is compress::automatic
	//   * the client accepts the coding (request::response_coding( ))
	//   * the result is smaller than the body
	// A from_static body is compressed once: the result, or that it did not
	// shrink, is kept until the daemon is destroyed, up to cache_size bytes.
	// Every response that meets the conditions on the response itself gets
	// Vary: Accept-Encoding, whether or not this client got it compressed. A
	// compressed response gets Content-Encoding, and a strong ETag becomes weak
	// (W/"..."), since its bytes differ from the uncompressed response.
	struct compression_config {
		// No effect when built with DAW_MHTTPD_ENABLE_ZSTD=OFF
		bool enabled = false;
		// zstd's levels: 1 to 22, or negative for faster and larger output
		int zstd_level = 3;
		std::size_t min_size = 1024;
		// Limit on the compressed from_static bodies kept; once reached, further
		// ones are compressed on every response. 0 disables the cache.
		std::size_t cache_size = 16 * 1024 * 1024;
		// Whether a Content-Type is worth compressing; empty uses
		// is_compressible_type. Called concurrently from MHD's threads, so it is
		// invoked as const and a mutable lambda is rejected.
		daw::copyable_function<bool( daw::string_view content_type ) const>
		  compressible{ };
	};

	struct config {
		std::uint16_t port{ }; // 0 = pick an ephemeral port
		// Numeric address to listen on. Empty listens on every IPv4 interface;
		// "::" listens on every interface, IPv4 and IPv6; "127.0.0.1" or "::1"
		// only on that address. Anything else throws std::invalid_argument.
		std::string bind_address{ };
		// Threads that serve connections and run synchronous handlers. 0 is one
		// per CPU this process may run on (its affinity mask; a container's CPU
		// quota is not seen), kept within connection_limit. 1 is a single thread,
		// so handlers never run concurrently.
		unsigned thread_pool_size = 0;
		std::size_t max_body_size = 1024 * 1024; // larger bodies get 413
		std::chrono::seconds connection_timeout{ 30 };
		// Most connections open at once, across the thread pool; past it the
		// daemon stops accepting, so new ones wait in the listen queue until one
		// closes. 0 uses libmicrohttpd's default
		// (FD_SETSIZE - 4). With a pool it is split between the threads, so it
		// must be at least an explicit thread_pool_size, or the constructor
		// throws std::invalid_argument.
		unsigned connection_limit = 0;
		// Most connections open at once from one IP address; further ones are
		// closed as they arrive. 0 = no limit
		unsigned per_ip_connection_limit = 0;
		// Bytes of memory each connection gets for reading the request line and
		// headers and for its read and write buffers. libmicrohttpd zeroes it
		// between keep-alive requests, so a smaller one costs less CPU per
		// request and less memory per open connection. Requests whose headers
		// do not fit get 431. 0 uses libmicrohttpd's default (32 KiB).
		std::size_t connection_memory_limit = 0;
		std::chrono::milliseconds shutdown_grace{
		  5000 }; // wait for suspended requests on stop
		compression_config compression{ };
	};

	class daemon {
	public:
		template<class F>
		requires AsyncHandler<F> or SyncHandler<F> or CoroutineHandler<F>
		daemon( config const &config, F &&handler )
		  : daemon{ config,
		            detail::make_request_handler( std::forward<F>( handler ) ),
		            CoroutineHandler<F> } {}

		// The handler is invoked once the body is buffered
		daemon( config const &config, request_handler handler );

		// Dispatches each request to the matching route of `routes`; see
		// daw_mhttpd_router.h
		daemon( config const &config, router routes );
		daemon( daemon && ) noexcept;
		daemon &operator=( daemon && ) noexcept;
		~daemon( );

		[[nodiscard]] std::uint16_t port( ) const noexcept;
		// Threads serving connections: config::thread_pool_size, or what 0
		// resolved to
		[[nodiscard]] unsigned thread_count( ) const noexcept;

		// Blocks until request_stop( ) is called, then stops the server; returns
		// once it has stopped. Also returns if stop( ) is called elsewhere.
		void wait( ) noexcept;

		// Asks wait( ) to stop the server. Safe from any thread, including inside
		// a handler.
		void request_stop( ) noexcept;

		// Like wait( ), with any of `signals` (e.g. SIGINT, SIGTERM) acting as
		// request_stop( ): blocks until one arrives, then stops the server
		// gracefully and returns. Adds to any signals already added. Installs a
		// process-wide handler for each signal,
		// replacing the current disposition until the server starts stopping, so
		// a second signal during the shutdown grace period gets the original
		// behaviour (for SIGINT, terminating the process). Several daemons may
		// wait on the same signal; one signal stops them all. Throws
		// std::invalid_argument, before installing anything, for a signal that
		// cannot be caught such as SIGKILL.
		void wait_for_signals( std::span<int const> signals );
		void wait_for_signals( std::initializer_list<int> signals ) {
			wait_for_signals( std::span{ signals.begin( ), signals.size( ) } );
		}

		// Like wait_for_signals without blocking: when any of `signals` arrives,
		// a background thread stops the server gracefully. That thread also acts
		// on request_stop( ), which from then on stops the server without anyone
		// calling wait( ). wait( ) can still be used to block until it has
		// stopped. Same signal rules and exceptions as wait_for_signals.
		void add_stop_signals( std::span<int const> signals );
		void add_stop_signals( std::initializer_list<int> signals ) {
			add_stop_signals( std::span{ signals.begin( ), signals.size( ) } );
		}

		// Stops `signals` from stopping this server, e.g. from another thread
		// while one waits in wait_for_signals. A signal no daemon waits on any
		// more gets its original disposition back. Signals it is not waiting on
		// are ignored.
		void remove_stop_signals( std::span<int const> signals ) noexcept;
		void remove_stop_signals( std::initializer_list<int> signals ) noexcept {
			remove_stop_signals( std::span{ signals.begin( ), signals.size( ) } );
		}

		// Stops accepting, answers 503 to suspended requests, gives open
		// connections up to config::shutdown_grace to finish, then stops MHD.
		// Safe to call more than once and from several threads, but not from
		// inside a handler: it waits for MHD's threads, which run the handlers;
		// use request_stop( ) there.
		void stop( ) noexcept;

	private:
		// stream_body: invoke the handler when the headers arrive and deliver the
		// body through request::next_chunk
		daemon( config const &config, request_handler handler, bool stream_body );

		std::unique_ptr<detail::daemon_state> m_state;
	};
} // namespace daw::mhttpd

#include "daw/mhttpd/daw_mhttpd_form.h"
#include "daw/mhttpd/daw_mhttpd_router.h"
