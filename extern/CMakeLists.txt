# Copyright (c) Darrell Wright
#
# Distributed under the GNU Lesser General Public License, Version 2.1. (See
# accompanying file LICENSE)
#
# Official repository: https://github.com/beached/cpp_microhttpd
#

# This prevents tests running on dependencies
set( DAW_ENABLE_TESTING OFF )

include( FetchContent )

FetchContent_Declare(
		daw_header_libraries
		GIT_REPOSITORY https://github.com/beached/header_libraries.git
		GIT_TAG v2
		GIT_SHALLOW 1
		SYSTEM
		)


FetchContent_Declare(
		daw_integer
		GIT_REPOSITORY https://github.com/beached/daw_integer.git
		GIT_TAG release
		GIT_SHALLOW 1
		SYSTEM
		)

# daw_mhttpd logs with std::format, not fmt
set( SPDLOG_USE_STD_FORMAT ON )
FetchContent_Declare(
		spdlog
		GIT_REPOSITORY https://github.com/gabime/spdlog.git
		GIT_TAG v1.17.0
		GIT_SHALLOW 1
		SYSTEM
		)

set( DAW_MHTTPD_FETCHED daw_header_libraries daw_integer spdlog )

if( DAW_MHTTPD_ENABLE_ZSTD )
	# zstd uses option( ); let these variables set them
	set( CMAKE_POLICY_DEFAULT_CMP0077 NEW )
	set( ZSTD_BUILD_PROGRAMS OFF )
	set( ZSTD_BUILD_TESTS OFF )
	set( ZSTD_BUILD_SHARED OFF )
	set( ZSTD_BUILD_STATIC ON )
	set( ZSTD_LEGACY_SUPPORT OFF )
	# Responses are compressed one at a time on MHD's threads
	set( ZSTD_MULTITHREAD_SUPPORT OFF )
	# zstd always adds install rules; keep them out unless daw-mhttpd installs,
	# which needs zstd's export set for its own
	if( NOT DAW_MHTTPD_INSTALL )
		set( DAW_MHTTPD_ZSTD_EXCLUDE EXCLUDE_FROM_ALL )
	endif()
	FetchContent_Declare(
			zstd
			GIT_REPOSITORY https://github.com/facebook/zstd.git
			GIT_TAG v1.5.7
			GIT_SHALLOW 1
			SOURCE_SUBDIR build/cmake
			SYSTEM
			${DAW_MHTTPD_ZSTD_EXCLUDE}
			)
	list( APPEND DAW_MHTTPD_FETCHED zstd )
endif()

FetchContent_MakeAvailable( ${DAW_MHTTPD_FETCHED} )

if( DAW_MHTTPD_ENABLE_ZSTD )
	# Linked into daw-mhttpd, which may be a shared library
	set_target_properties( libzstd_static PROPERTIES POSITION_INDEPENDENT_CODE ON )
endif()
