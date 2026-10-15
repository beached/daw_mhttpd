#include "daw/mhttpd/daw_mhttpd_log.h"
#include "daw/mhttpd/daw_mhttpd.h"

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace daw::mhttpd::log {
	namespace {
		constexpr char const *names[] = {
		  "daw_mhttpd.server", "daw_mhttpd.router", "daw_mhttpd.static_files" };
		static_assert( std::size( names ) == all_components.size( ) );

		struct state {
			// Every logger writes here; dist_sink makes swapping its sinks thread
			// safe
			std::shared_ptr<spdlog::sinks::dist_sink_mt> sink =
			  std::make_shared<spdlog::sinks::dist_sink_mt>( );
			std::array<std::shared_ptr<spdlog::logger>, all_components.size( )>
			  loggers;

			state( ) {
				sink->add_sink( std::make_shared<spdlog::sinks::stderr_color_sink_mt>( ) );
				for( std::size_t n = 0; n < loggers.size( ); ++n ) {
					auto &logger = loggers[n];
					logger = std::make_shared<spdlog::logger>( names[n], sink );
					try {
						spdlog::register_logger( logger );
					} catch( spdlog::spdlog_ex const & ) {
						// The application already has a logger by this name; ours
						// still works, it just cannot be found through spdlog::get
					}
					// After registering, which applies spdlog's global level
					logger->set_level( spdlog::level::err );
				}
			}
		};

		// Never destroyed, so a daemon stopping during static destruction can
		// still log
		[[nodiscard]] state &get_state( ) {
			static auto *const s = new state{ };
			return *s;
		}

		// Create and register the loggers at startup, so spdlog::get finds them
		// before the library first logs
		[[maybe_unused]] state const &eager_state = get_state( );
	} // namespace

	spdlog::logger &get( component c ) {
		return *get_state( ).loggers[static_cast<std::size_t>( c )];
	}

	void set_level( spdlog::level::level_enum level ) {
		for( auto const &logger : get_state( ).loggers ) {
			logger->set_level( level );
		}
	}

	void set_level( component c, spdlog::level::level_enum level ) {
		get( c ).set_level( level );
	}

	void set_sinks( std::vector<spdlog::sink_ptr> sinks ) {
		get_state( ).sink->set_sinks( std::move( sinks ) );
	}
} // namespace daw::mhttpd::log

namespace daw::mhttpd::detail {
	void log_handler_exception( log::component c, request const &req ) noexcept {
		try {
			auto &logger = log::get( c );
			if( not logger.should_log( spdlog::level::err ) ) {
				return;
			}
			auto what = std::string{ "an exception not derived from std::exception" };
			try {
				std::rethrow_exception( std::current_exception( ) );
			} catch( std::exception const &e ) { what = e.what( ); } catch( ... ) {
			}
			logger.error( "handler for {} {} threw: {}",
			              req.method_name( ),
			              req.url( ),
			              what );
		} catch( ... ) {
			// Logging must not turn a handled error into a crash
		}
	}
} // namespace daw::mhttpd::detail
