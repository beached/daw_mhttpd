cmake_minimum_required( VERSION 4.2.3 )
project( daw-mhttpd VERSION 0.1.0 LANGUAGES CXX )

include( GNUInstallDirs )

# Off by default when pulled in with FetchContent or add_subdirectory
option( DAW_MHTTPD_INSTALL "Generate the install target" ${PROJECT_IS_TOP_LEVEL} )

# Without it the API is the same, but config::compression never compresses
option( DAW_MHTTPD_ENABLE_ZSTD "Compress responses with the zstd content coding" ON )

# Link libmicrohttpd.a and its dependencies' static libraries, e.g. for a -static executable
option( DAW_MHTTPD_STATIC_MHD "Link libmicrohttpd statically" OFF )

option( DAW_USE_PACKAGE_MANAGEMENT "Do not use FetchContent and assume dependencies are installed" OFF )
if( DAW_USE_PACKAGE_MANAGEMENT )
	find_package( daw-header-libraries REQUIRED )
	find_package( daw-integer REQUIRED )
	# Must be built with SPDLOG_USE_STD_FORMAT; daw_mhttpd_log.h checks
	find_package( spdlog CONFIG REQUIRED )
	if( DAW_MHTTPD_ENABLE_ZSTD )
		find_package( zstd CONFIG REQUIRED )
	endif()
else()
	# Fetched dependencies must install too, or the export below has no targets to refer to
	set( SPDLOG_INSTALL ${DAW_MHTTPD_INSTALL} )
	add_subdirectory( extern )
endif()

include( cmake/daw_mhttpd_find_mhd.cmake )
if( NOT MHD_FOUND )
	message( FATAL_ERROR "libmicrohttpd 1.0 or newer was not found with pkg-config" )
endif()

add_library( ${PROJECT_NAME} src/daw_mhttpd.cpp src/daw_mhttpd_compression.cpp src/daw_mhttpd_form.cpp src/daw_mhttpd_log.cpp src/daw_mhttpd_router.cpp )
add_library( daw::${PROJECT_NAME} ALIAS ${PROJECT_NAME} )
target_include_directories( ${PROJECT_NAME} PUBLIC
														$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
														$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}> )
target_compile_features( ${PROJECT_NAME} PUBLIC cxx_std_23 )
target_link_libraries( ${PROJECT_NAME} PUBLIC daw::daw-header-libraries daw::daw-integer spdlog::spdlog PRIVATE PkgConfig::MHD )
target_compile_options( ${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic )
if( DAW_MHTTPD_ENABLE_ZSTD )
	# zstd::libzstd is the installed name (1.5.6+), libzstd the FetchContent one;
	# older installs only have the shared and static targets
	if( TARGET zstd::libzstd )
		set( DAW_MHTTPD_ZSTD_TARGET zstd::libzstd )
	elseif( TARGET libzstd )
		set( DAW_MHTTPD_ZSTD_TARGET libzstd )
	elseif( TARGET zstd::libzstd_shared )
		set( DAW_MHTTPD_ZSTD_TARGET zstd::libzstd_shared )
	else()
		set( DAW_MHTTPD_ZSTD_TARGET zstd::libzstd_static )
	endif()
	target_link_libraries( ${PROJECT_NAME} PRIVATE ${DAW_MHTTPD_ZSTD_TARGET} )
	target_compile_definitions( ${PROJECT_NAME} PRIVATE DAW_MHTTPD_HAS_ZSTD )
endif()
set_target_properties( ${PROJECT_NAME} PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} )

if( DAW_MHTTPD_INSTALL )
	set( DAW_MHTTPD_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}" )

	install( TARGETS ${PROJECT_NAME}
					 EXPORT ${PROJECT_NAME}_Targets
					 ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
					 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
					 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} )
	install( DIRECTORY ${PROJECT_SOURCE_DIR}/include/daw DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} )

	install( EXPORT ${PROJECT_NAME}_Targets
					 FILE ${PROJECT_NAME}Targets.cmake
					 NAMESPACE daw::
					 DESTINATION ${DAW_MHTTPD_INSTALL_CMAKEDIR} )

	include( CMakePackageConfigHelpers )
	configure_package_config_file(
			"${PROJECT_SOURCE_DIR}/cmake/${PROJECT_NAME}Config.cmake.in"
			"${PROJECT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
			INSTALL_DESTINATION ${DAW_MHTTPD_INSTALL_CMAKEDIR} )
	# 0.x releases make no compatibility promises between minor versions
	write_basic_package_version_file( "${PROJECT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
																		VERSION ${PROJECT_VERSION}
																		COMPATIBILITY SameMinorVersion )
	install( FILES "${PROJECT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
								 "${PROJECT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
								 "${PROJECT_SOURCE_DIR}/cmake/daw_mhttpd_find_mhd.cmake"
					 DESTINATION ${DAW_MHTTPD_INSTALL_CMAKEDIR} )
endif()

option( DAW_MHTTPD_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL} )
option( DAW_ENABLE_TESTING_ONLY "Build unit tests" OFF )
# Turns on both of the above
option( DAW_ENABLE_TESTING "Build unit tests and examples" OFF )

if( DAW_MHTTPD_BUILD_EXAMPLES OR DAW_ENABLE_TESTING )
	add_subdirectory( examples )
endif()

if( DAW_ENABLE_TESTING_ONLY OR DAW_ENABLE_TESTING )
	enable_testing()
	add_subdirectory( tests )
endif()
