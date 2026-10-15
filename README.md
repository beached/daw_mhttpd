# daw_mhttpd - C++23 HTTP Server on libmicrohttpd

## Content
  * [Intro](#intro)
  * [Requirements](#requirements)
  * [Adding it to a Project](#adding-it-to-a-project)
  * [Quick Start](#quick-start)
  * [Handlers](#handlers)
  * [Requests](#requests)
  * [Responses](#responses)
  * [Compression](#compression)
  * [Routing](#routing)
  * [Forms](#forms)
  * [Coroutines](#coroutines)
  * [Running and Stopping](#running-and-stopping)
  * [Configuration](#configuration)
  * [Errors](#errors)
  * [Logging](#logging)
  * [Threads and Lifetimes](#threads-and-lifetimes)
  * [Examples](#examples)
  * [Building and Testing](#building-and-testing)
  * [License](#license)

## Intro
###### [Top](#content)

daw_mhttpd wraps [GNU libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) (MHD) in a C++23 interface. MHD does the networking and HTTP parsing on its own threads; you provide a handler that turns a request into a response.

  * Three handler styles: synchronous, callback (complete from any thread), and coroutine
  * Coroutine handlers can `co_await` callback based APIs, so it fits next to other event loops without depending on one; there is no Asio dependency
  * Streamed request bodies with backpressure, or a buffered body with a size limit
  * A router with path parameters, and form parsing (url-encoded and multipart), buffered or streamed
  * Zero-copy responses from static data, owned containers, and file descriptors (`sendfile`)
  * Graceful shutdown: in-flight requests finish, waiting ones get `503`

```c++
#include <daw/mhttpd/daw_mhttpd.h>

auto d = daw::mhttpd::daemon{ { }, []( daw::mhttpd::request & ) {
	return daw::mhttpd::response::from_static( "Hello, World!" );
} };
// serving on d.port( ) until d is destroyed
```

Everything is in namespace `daw::mhttpd`; the main header is `<daw/mhttpd/daw_mhttpd.h>`.

## Requirements
###### [Top](#content)

  * A C++23 compiler; developed with GCC 15
  * CMake 4.2.3 or newer
  * libmicrohttpd 1.0 or newer, found through `pkg-config`
  * A POSIX system (Linux is what it is tested on)
  * [header_libraries](https://github.com/beached/header_libraries) and [daw_integer](https://github.com/beached/daw_integer), fetched automatically by CMake
  * [spdlog](https://github.com/gabime/spdlog) built with `SPDLOG_USE_STD_FORMAT`, fetched automatically by CMake. An installed spdlog that uses fmt instead stops the build with an error.
  * [zstd](https://github.com/facebook/zstd) for [compression](#compression), fetched automatically by CMake; `-DDAW_MHTTPD_ENABLE_ZSTD=OFF` builds without it

On Debian/Ubuntu: `apt install libmicrohttpd-dev pkg-config`

## Adding it to a Project
###### [Top](#content)

Add it with `FetchContent` or `add_subdirectory`, then link `daw::daw-mhttpd`:

```cmake
include( FetchContent )
FetchContent_Declare(
		daw-mhttpd
		GIT_REPOSITORY https://github.com/beached/cpp_microhttpd.git
		GIT_TAG release
		)
FetchContent_MakeAvailable( daw-mhttpd )

target_link_libraries( my_server PRIVATE daw::daw-mhttpd )
```

Its dependencies are fetched by default. Set `DAW_USE_PACKAGE_MANAGEMENT=ON` to use installed packages instead. When it is pulled in this way, the examples are not built and nothing is installed unless you turn on `DAW_MHTTPD_BUILD_EXAMPLES` or `DAW_MHTTPD_INSTALL`.

Or install it and use `find_package`:

```shell
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/opt/daw
cmake --build build
cmake --install build
```

```cmake
find_package( daw-mhttpd REQUIRED )
target_link_libraries( my_server PRIVATE daw::daw-mhttpd )
```

When the dependencies are fetched, they are installed alongside it, so the prefix is self-contained. The package config looks for libmicrohttpd with pkg-config, so `pkg-config` must be available wherever `find_package` runs.

To link libmicrohttpd statically, e.g. for a `-static` executable, set `DAW_MHTTPD_STATIC_MHD=ON`. It uses `pkg-config --static` and links the `.a` of libmicrohttpd and of each library it needs. A library with no `.a` installed is linked as a shared library. That is fine for a normal link, but a `-static` link then fails. For example, Debian ships no `libp11-kit.a`, so its gnutls-enabled libmicrohttpd cannot be linked fully statically.

## Quick Start
###### [Top](#content)

A server that runs until Ctrl-C or `kill`:

```c++
#include <daw/mhttpd/daw_mhttpd.h>

#include <csignal>

int main( ) {
	auto d = daw::mhttpd::daemon{
	  { .port = 8080 }, []( daw::mhttpd::request & ) {
		  return daw::mhttpd::response::from_static( "Hello, World!" )
		    .header( "Content-Type", "text/plain" );
	  } };
	// Returns once a signal arrives and the server has stopped gracefully
	d.wait_for_signals( { SIGINT, SIGTERM } );
}
```

## Handlers
###### [Top](#content)

The `daemon` constructor takes a [`config`](#configuration) and a handler, and picks the handler style from its signature.

**Synchronous**: return the response.
```c++
[]( daw::mhttpd::request &req ) -> daw::mhttpd::response {
	return daw::mhttpd::response::from_copy( req.url( ) );
}
```

**Callback**: take a `responder` and call `send` once, from any thread. If the handler returns without sending, the connection is suspended until `send` is called, so MHD's thread is free for other connections. A `responder` destroyed without sending answers `500`.
```c++
[&pool]( daw::mhttpd::request &, daw::mhttpd::responder res ) {
	pool.post( [res = std::move( res )]( ) mutable {
		res.send( daw::mhttpd::response::from_owned( expensive_work( ) ) );
	} );
}
```

**Coroutine**: return `task<response>`; see [Coroutines](#coroutines).
```c++
[]( daw::mhttpd::request &req ) -> daw::mhttpd::task<daw::mhttpd::response> {
	auto body = co_await req.read_body( );
	co_return daw::mhttpd::response::from_copy( body );
}
```

By default the daemon runs one thread per CPU, so several threads call the same handler at once; set `thread_pool_size = 1` for a single thread. Handlers must be callable as `const` for that reason. A `mutable` lambda is rejected at compile time; keep shared mutable state behind a mutex or atomics. Handlers that capture move-only objects are fine.

## Requests
###### [Top](#content)

| Member | Returns |
|---|---|
| `method( )` | `http_method` enum: `GET`, `HEAD`, `POST`, `PUT`, `DELETE`, `CONNECT`, `OPTIONS`, `TRACE`, `PATCH`, or `UNKNOWN` |
| `method_name( )` | the method exactly as sent, e.g. `"PROPFIND"` for an `UNKNOWN` method |
| `url( )` | the path, percent-decoded, without the query string |
| `version( )` | e.g. `"HTTP/1.1"` |
| `header( name )` | `std::optional<daw::string_view>`; names are case-insensitive |
| `query( name )` | a query string argument |
| `cookie( name )` | a cookie value |
| `path_param( name )` | a parameter captured by a [route](#routing) pattern |
| `remote_address( )` | the client's IP address as text, e.g. `"192.0.2.7"` or `"::1"`; IPv4 clients of a dual-stack daemon are reported as IPv4 |
| `remote_sockaddr( )` | the client's `sockaddr const *` (`sockaddr_in` or `sockaddr_in6`) |
| `body( )`, `body_text( )` | the buffered body, as bytes or text |
| `next_chunk( )`, `read_body( )` | coroutine handlers only; see below |

Method names are case-sensitive, as RFC 9110 requires: `get` is `http_method::UNKNOWN`, which a server should answer with `501`. `parse_method` and `to_string( http_method )` are `constexpr`.

The string views point into MHD's memory and stay valid until the response is sent; copy anything you need after that.

### The request body

Synchronous and callback handlers run after the whole body has arrived, and read it with `body( )`. Bodies larger than `config::max_body_size` are answered with `413` before the handler runs.

Coroutine handlers start as soon as the headers arrive, and decide how to read the body:
```c++
// Everything at once, limited by config::max_body_size; past the limit it
// throws body_too_large, which answers 413 if not caught
auto body = co_await req.read_body( );

// Or piece by piece, with no size limit
while( auto chunk = co_await req.next_chunk( ) ) {
	consume( *chunk ); // std::span<std::byte const>, valid until the next co_await
}
```

While the coroutine is not waiting in `next_chunk`, MHD stops reading from the client, so a slow consumer applies backpressure instead of buffering. A coroutine can also respond before reading any of the body, for example with `401`; MHD then discards the body. If the client disconnects, or a response was already sent, `next_chunk` throws `request_aborted` rather than reporting an incomplete body as complete.

## Responses
###### [Top](#content)

`response` is move-only, and nothing is handed to libmicrohttpd until it is sent, so its status and headers can be read and changed until then. Status and headers chain:
```c++
return daw::mhttpd::response::from_copy( json_text, 201 )
  .header( "Content-Type", "application/json" )
  .header( "Location", "/items/42" );
```

| Factory | Body | Copies |
|---|---|---|
| `empty( status = 204 )` | none | - |
| `from_static( data )` | data that stays valid and unchanged while the daemon runs, e.g. a literal | no |
| `from_copy( data )` | any bytes or text | once |
| `from_owned( std::move( container ) )` | a `std::string`, `std::vector<std::byte>`, or other contiguous container, freed when MHD is done | no |
| `from_fd( std::move( fd ), offset, size )` | part of a file, from a `unique_fd` (e.g. `unique_fd{ ::open( path, O_RDONLY ) }`); the response takes ownership and MHD may use `sendfile` | no |
| `stream( producer, size = unknown_size )` | produced on demand; an unknown size uses chunked encoding | written straight into MHD's buffer |

A streaming producer is called with the current offset and a buffer to fill:
```c++
auto lines = daw::mhttpd::response::stream(
  [n = 0]( daw::u64 /*pos*/, std::span<std::byte> out ) mutable {
	  if( n == 3 ) {
		  return daw::mhttpd::stream_result::end( );
	  }
	  auto const line = std::format( "line {}\n", n++ );
	  std::memcpy( out.data( ), line.data( ), line.size( ) );
	  return daw::mhttpd::stream_result::bytes( line.size( ) );
  } );
```
MHD calls a producer from one thread at a time, so it may be `mutable`.

`header( name )` reads back the first header with that name, ignoring case. `header( name, value )` throws `std::invalid_argument` for a name that is not an HTTP token or a value containing CR, LF or NUL. Headers libmicrohttpd sets itself, such as `Content-Length`, are rejected only when the response is sent; that is logged and answered `500`.

## Compression
###### [Top](#content)

With compression enabled, the daemon compresses responses with zstd for clients whose `Accept-Encoding` allows it. Handlers do not change:
```c++
auto cfg = daw::mhttpd::config{ };
cfg.compression.enabled = true;

routes.get( "/api/worlds", []( daw::mhttpd::request & ) {
  return daw::mhttpd::response::from_copy( worlds_json( ) )
    .header( "Content-Type", "application/json" );
} );
```

A response is compressed when all of these hold:
  * its body is buffered (`from_static`, `from_copy`, `from_owned`) and at least `min_size` bytes; files and streams are sent as is
  * its `Content-Type` is compressible: by default text, JSON, JavaScript, XML, any `+json` or `+xml` type such as SVG, WebAssembly and uncompressed fonts (`is_compressible_type`)
  * it has no `Content-Encoding` or `Content-Range`, and its status is not 1xx, `204`, `206` or `304`
  * the handler did not opt out with `.compression( daw::mhttpd::compress::never )`, e.g. for a page that mixes a secret with text the client controls (BREACH)
  * the client accepts zstd, which `request::response_coding( )` reports
  * the result is smaller than the body

Every response that meets the conditions on the response itself gets `Vary: Accept-Encoding`, so caches keep the compressed and uncompressed versions apart. A compressed one gets `Content-Encoding: zstd`, and a strong `ETag` becomes weak (`W/"..."`); `If-None-Match` compares weakly, so it still matches. If compression fails, the body is sent as is.

A `from_static` body is compressed only once: the daemon keeps the result, keyed by the body's address and size, until it is destroyed. That is why `from_static` data must not change while the daemon runs. `from_copy` and `from_owned` bodies are compressed on every response.

| `compression_config` field | Default | Meaning |
|---|---|---|
| `enabled` | `false` | compress at all; no effect when built without zstd |
| `zstd_level` | `3` | 1 to 22, or negative for faster and larger output; the window is kept within RFC 9659's 8 MiB |
| `min_size` | 1 KiB | smaller bodies are sent as is |
| `cache_size` | 16 MiB | limit on the compressed `from_static` bodies kept; past it they are compressed on every response; `0` disables the cache |
| `compressible` | empty | `bool( daw::string_view content_type )` replacing `is_compressible_type`; called concurrently |

For precompressed files, check `req.response_coding( ) == daw::mhttpd::content_coding::zstd`, send the `.zst` file, and set `Content-Encoding: zstd` and `Vary: Accept-Encoding` yourself; the daemon leaves a response with a `Content-Encoding` alone.

## Routing
###### [Top](#content)

A `router` (`<daw/mhttpd/daw_mhttpd_router.h>`, included by the main header) picks a handler by method and path. Build it, then hand it to the daemon:
```c++
auto routes = daw::mhttpd::router{ };
routes.get( "/worlds/{name}", []( daw::mhttpd::request &req ) {
	return daw::mhttpd::response::from_copy( *req.path_param( "name" ) );
} )
.post( "/worlds/new", create_world )
.delete_( "/worlds/{name}", delete_world )
.get( "/files/{path...}", serve_file )
.not_found( []( daw::mhttpd::request &req ) {
	return daw::mhttpd::response::empty( 404 );
} );
auto d = daw::mhttpd::daemon{ { .port = 8080 }, std::move( routes ) };
```

  * Patterns are made of segments: literal text, `{name}` for one non-empty segment, or `{name...}` as the last segment for the rest of the path, slashes included and possibly empty. A malformed pattern throws `std::invalid_argument`.
  * Matching is case-sensitive and runs on the percent-decoded path without the query string. A trailing slash is significant.
  * When several routes match, the one whose earlier segments are more specific wins: literal, then `{name}`, then `{name...}`. On a tie the route added first wins. So `/worlds/new` beats `/worlds/{name}` whatever order they were added in.
  * A path that only matches routes for other methods gets `405` with an `Allow` header. `HEAD` falls back to the `GET` route. An unrecognized method gets `501`. Anything else goes to `not_found`, which answers `404` by default.
  * `get`, `post`, `put`, `patch` and `delete_` are shorthands for `add( http_method, pattern, handler )`.

Routes take all three handler styles. Synchronous and callback routes run after the body is buffered, limited by `config::max_body_size`. Coroutine routes start on the headers and read the body themselves, so one route can stream a large upload while the others keep a small limit. `404` and `405` are sent without reading the body.

## Forms
###### [Top](#content)

`<daw/mhttpd/daw_mhttpd_form.h>` (included by the main header) parses `application/x-www-form-urlencoded` and `multipart/form-data` bodies, using MHD's post processor.

A buffered body:
```c++
auto form = daw::mhttpd::parse_form( req ); // throws form_error if it is not a valid form
if( auto const *name = form.find( "name" ) ) {
	use( name->value );
}
for( auto const &field : form.fields( ) ) {
	// field.name, field.value, and for uploads field.filename and field.content_type
}
```

A body too large to buffer, streamed through a coroutine route:
```c++
routes.post( "/upload", []( daw::mhttpd::request &req ) -> daw::mhttpd::task<daw::mhttpd::response> {
	auto out = std::ofstream{ "upload.bin", std::ios::binary };
	auto parser = daw::mhttpd::form_parser{ req, [&]( daw::mhttpd::form_chunk const &chunk ) {
		if( chunk.name == "file" ) {
			// chunk.offset == 0 starts a new field
			out.write( reinterpret_cast<char const *>( chunk.data.data( ) ), chunk.data.size( ) );
		}
	} };
	while( auto chunk = co_await req.next_chunk( ) ) {
		parser.feed( *chunk );
	}
	parser.finish( ); // throws form_error if the body ended early
	co_return daw::mhttpd::response::empty( 201 );
} );
```
The parser buffers only field names and part headers; values go straight to the callback. An exception thrown by the callback comes out of `feed` or `finish`.

## Coroutines
###### [Top](#content)

The coroutine support is in `<daw/mhttpd/daw_mhttpd_task.h>`, included by the main header.

  * `task<T>` starts when awaited, and has no executor: when it finishes, its awaiter continues on whichever thread finished it. Await it as `co_await std::move( task )` or directly on a call's result.
  * `from_callback<T>( start )` turns a callback API into something to `co_await`. `start` receives a move-only `completion<T>`; call `done( value )`, or `done.fail( exception_ptr )`, from any thread. A `completion` destroyed without being called throws `callback_abandoned` from the `co_await`, so a lost callback cannot hang the coroutine.
  * `sync_wait( task )` runs a task to completion from ordinary code.

Awaiting another library's callback, such as a database client or a child process:
```c++
auto d = daw::mhttpd::daemon{
  { }, [&db]( daw::mhttpd::request &req ) -> daw::mhttpd::task<daw::mhttpd::response> {
	  auto id = std::string{ req.query( "id" ).value_or( "" ) }; // copy before suspending
	  auto row = co_await daw::mhttpd::from_callback<std::string>(
	    [&]( daw::mhttpd::completion<std::string> done ) {
		    db.async_lookup( id, std::move( done ) );
	    } );
	  co_return daw::mhttpd::response::from_owned( std::move( row ) );
  } };
```

After the `co_await`, the coroutine runs on the database client's thread; sending the response from there is safe. If a coroutine handler throws, the client gets `500`.

`completion` is move-only; to pass it to an API that needs a copyable `std::function`, wrap it in a `std::shared_ptr`.

## Running and Stopping
###### [Top](#content)

The server starts in the `daemon` constructor and runs on MHD's threads.

| Member | Does |
|---|---|
| `port( )` | the port in use; useful with `config::port = 0` |
| `wait( )` | blocks until `request_stop( )` is called, then stops the server and returns |
| `request_stop( )` | asks `wait( )` to stop; safe from any thread, including inside a handler |
| `wait_for_signals( { SIGINT, SIGTERM } )` | `wait( )`, with any of the signals acting as `request_stop( )` |
| `add_stop_signals( { SIGINT, SIGTERM } )` | the same without blocking: a background thread stops the server when one arrives |
| `remove_stop_signals( { SIGINT } )` | stops those signals from stopping this server; safe from any thread, including while another waits in `wait_for_signals` |
| `stop( )` | stops now, on the calling thread; also run by the destructor |

Stopping is graceful: new connections are refused, requests waiting on a `responder` or coroutine are answered `503` with `Connection: close`, and open connections get up to `config::shutdown_grace` to finish. An idle keep-alive connection holds shutdown up to that limit.

Do not call `stop( )` from inside a handler: it waits for MHD's threads, which are running the handler. Use `request_stop( )` there, for example in a `/shutdown` route.

`add_stop_signals` suits a program whose main thread has other work. Its background thread also acts on `request_stop( )`, so after calling it, a `/shutdown` route's `request_stop( )` stops the server without anyone calling `wait( )`. `wait( )` still works for blocking until the server has stopped. Both methods add to the signals already added.

`wait_for_signals` and `add_stop_signals` install a handler for each signal, replacing its current disposition. The signals do not need to be blocked first, and arrive on whichever thread the kernel picks. Once the server starts stopping, the original disposition comes back. That way a second Ctrl-C during the grace period still terminates the process as usual. `remove_stop_signals` also restores a signal's original disposition once no daemon is waiting on it. Several daemons may wait on the same signal, and one signal stops them all. A signal that cannot be caught, such as `SIGKILL`, throws `std::invalid_argument` before anything is installed.

## Configuration
###### [Top](#content)

```c++
auto cfg = daw::mhttpd::config{
  .port = 8080,
  .thread_pool_size = 4,
  .max_body_size = 10 * 1024 * 1024,
};
```

| Field | Default | Meaning |
|---|---|---|
| `port` | `0` | `0` picks a free port; see `daemon::port( )` |
| `bind_address` | empty | numeric address to listen on: empty is every IPv4 interface, `"::"` every interface (IPv4 and IPv6), `"127.0.0.1"` or `"::1"` only that one; anything else throws `std::invalid_argument` |
| `thread_pool_size` | `0` | threads serving connections and running synchronous handlers. `0` is one per CPU the process may run on (its affinity mask, as set by `taskset` or cpusets; a container's CPU quota is not seen), kept within `connection_limit`. `1` is a single thread. `daemon::thread_count( )` reports the number used |
| `max_body_size` | 1 MiB | limit for buffered bodies and `read_body( )`; larger gets `413` |
| `connection_timeout` | 30 s | idle connections are closed after this |
| `connection_limit` | `0` | most connections open at once; past it new ones wait in the listen queue until one closes. `0` is libmicrohttpd's default, `FD_SETSIZE - 4`. With a pool it is split between the threads and must be at least an explicit `thread_pool_size` |
| `per_ip_connection_limit` | `0` | most connections open at once from one address; past it new ones are closed; `0` is no limit |
| `shutdown_grace` | 5 s | how long shutdown waits for open connections |
| `compression` | disabled | see [Compression](#compression) |

A single thread uses `poll( )` rather than epoll: libmicrohttpd 1.0.1 to 1.0.5 can stop serving keep-alive connections on a single epoll thread once about 128 are open. Pools use epoll and are not affected.

## Errors
###### [Top](#content)

| Situation | Result |
|---|---|
| A handler throws | `500` |
| A `responder` is destroyed without `send` | `500` |
| The body is larger than `max_body_size` (buffered or `read_body`) | `413`; coroutines see `body_too_large` |
| No route matches the path | `404`, or the router's `not_found` handler |
| Routes match the path but not the method | `405` with `Allow` |
| A form is malformed, or the body is not a form | `form_error` |
| The client disconnects mid-body | `next_chunk` throws `request_aborted` |
| A `completion` is destroyed without being called | `co_await` throws `callback_abandoned` |
| The server stops while a request waits | `503` |
| A second `send` | returns `false` and is ignored |

## Logging
###### [Top](#content)

Each part of the library logs through its own [spdlog](https://github.com/gabime/spdlog) logger, configured in `<daw/mhttpd/daw_mhttpd_log.h>`:

| Component | Logger | Logs |
|---|---|---|
| `log::component::server` | `daw_mhttpd.server` | start and stop (info), signals (info), libmicrohttpd's own messages (warn), failing to start and handler exceptions (error), one line per answered request (debug) |
| `log::component::router` | `daw_mhttpd.router` | the route each request matched, `404`, `405`, `501` and `413` answers (debug), exceptions from route handlers of every style (error) |
| `log::component::static_files` | `daw_mhttpd.static_files` | each request of the example's static file handler and its status (debug) |

Every component starts at `error`, so nothing but errors is logged until a level is set:
```c++
namespace log = daw::mhttpd::log;
log::set_level( spdlog::level::info );                         // every component
log::set_level( log::component::router, spdlog::level::debug ); // one component
log::set_level( spdlog::level::off );                          // nothing at all
log::set_sinks( spdlog::default_logger( )->sinks( ) );         // into the application's log
```

Output goes to stderr until `set_sinks` replaces it; that is safe while the server runs. The loggers are registered with spdlog under the names above, so `spdlog::get` and spdlog's global controls, such as `spdlog::set_level`, reach them too. libmicrohttpd's messages carry no severity, and many are caused by clients, such as one hanging up mid-request, so all of them are logged as warnings.

## Threads and Lifetimes
###### [Top](#content)

  * Handlers run on MHD's threads. A slow synchronous handler holds up the connections on its thread; use a `responder` or a coroutine for slow work.
  * `responder::send` and `completion` may be called from any thread.
  * `request` values (`url( )`, headers, body chunks) are only valid until the response is sent. Body chunks are only valid until the coroutine next suspends.
  * A coroutine may outlive `stop( )`; the handler object it came from is kept alive for it, and its late `send` is ignored.

## Examples
###### [Top](#content)

  * [`examples/src/static_file_server.cpp`](examples/src/static_file_server.cpp) serves a folder, with `index.html` as the default document:
    ```
    static_file_server [root=.] [port=8080]
    ```
    The handler in [`examples/src/static_files.h`](examples/src/static_files.h) sends files with `sendfile`, blocks paths that leave the folder (`..`, encoded `..`, symlinks), and supports `ETag`, `Last-Modified`, `304 Not Modified`, and byte `Range` requests including `If-Range`.
  * [`examples/src/hello_world.cpp`](examples/src/hello_world.cpp) and [`examples/src/hello_world_async.cpp`](examples/src/hello_world_async.cpp) are the smallest synchronous and coroutine servers. Each answers every request on `http://localhost:8080/` until Ctrl-C.
  * [`examples/src/routing.cpp`](examples/src/routing.cpp) is a small key/value server on `http://localhost:8080/` built with the [router](#routing). Synchronous routes list, read, write and delete items. One coroutine route awaits a callback-based lookup; another streams an upload past `max_body_size`. The file's header comment lists `curl` commands to try.
  * [`examples/src/web_app.cpp`](examples/src/web_app.cpp) serves a single-page app from [`examples/www`](examples/www). It layers API routes, static files and a fallback: a request that matches no route is looked up as a file, and a page path with no file, such as `/about`, gets `index.html` so the app's own routing can show it. Unknown `/api/` paths and missing files such as `/missing.png` still get `404`.
    ```
    web_app [root=examples/www] [port=8080]
    ```

The examples are built by default; `-DDAW_MHTTPD_BUILD_EXAMPLES=OFF` skips them unless `DAW_ENABLE_TESTING` is on.

## Building and Testing
###### [Top](#content)

```shell
cmake -S . -B build -DDAW_ENABLE_TESTING=ON
cmake --build build
ctest --test-dir build
```

| Option | Default | |
|---|---|---|
| `DAW_ENABLE_TESTING` | `OFF` | build the tests and the examples, whatever the two options below say |
| `DAW_ENABLE_TESTING_ONLY` | `OFF` | build the tests |
| `DAW_MHTTPD_BUILD_EXAMPLES` | `ON` when top level | build the examples |
| `DAW_MHTTPD_INSTALL` | `ON` when top level | generate the install target |
| `DAW_USE_PACKAGE_MANAGEMENT` | `OFF` | use installed dependencies instead of fetching them |
| `DAW_MHTTPD_ENABLE_ZSTD` | `ON` | link zstd for [compression](#compression); without it the API is the same but nothing is compressed |

## License
###### [Top](#content)

GNU Lesser General Public License, version 2.1; see [LICENSE](LICENSE).
