// Logging. Each part of the library has its own spdlog logger, so their levels
// can be set separately:
//
//   daw::mhttpd::log::set_level( spdlog::level::info );   // every component
//   daw::mhttpd::log::set_level( daw::mhttpd::log::component::router,
//                                spdlog::level::debug );
//   daw::mhttpd::log::set_level( spdlog::level::off );    // silence everything
//
// Every component starts at spdlog::level::err, so only errors are logged
// until a level is set. Output goes to stderr unless set_sinks replaces it.
//
// The loggers are also registered with spdlog as "daw_mhttpd.server",
// "daw_mhttpd.router" and "daw_mhttpd.static_files", so spdlog::get and
// spdlog's global controls such as spdlog::set_level reach them as well.
#pragma once

#include <spdlog/spdlog.h>

#ifndef SPDLOG_USE_STD_FORMAT
#error "daw_mhttpd needs spdlog built with SPDLOG_USE_STD_FORMAT"
#endif

#include "daw/mhttpd/daw_mhttpd.h"

#include <array>
#include <vector>

namespace daw::mhttpd::log {
	// log::component is declared in daw_mhttpd.h

	inline constexpr auto all_components = std::array{
	  component::server, component::router, component::static_files };

	// The logger of a component, e.g. to log from code that extends it
	[[nodiscard]] spdlog::logger &get( component c );

	void set_level( spdlog::level::level_enum level );
	void set_level( component c, spdlog::level::level_enum level );

	// Sends the output of every component to `sinks` instead, e.g. to an
	// application's own with set_sinks( spdlog::default_logger( )->sinks( ) ).
	// Safe while the server runs.
	void set_sinks( std::vector<spdlog::sink_ptr> sinks );
} // namespace daw::mhttpd::log
