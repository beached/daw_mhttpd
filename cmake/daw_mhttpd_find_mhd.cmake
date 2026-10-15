# Defines PkgConfig::MHD for libmicrohttpd. Shared by the build and the installed package config.
#
# With DAW_MHTTPD_STATIC_MHD the target uses pkg-config's --static information and
# prefers libmicrohttpd.a and the .a of each of its dependencies. pkg_check_modules'
# own IMPORTED_TARGET only uses the shared Libs line and resolves -lmicrohttpd to
# libmicrohttpd.so, which a -static link cannot use.
#
# Sets MHD_FOUND

if( TARGET PkgConfig::MHD )
	set( MHD_FOUND TRUE )
	return()
endif()

find_package( PkgConfig QUIET )
if( NOT PKG_CONFIG_FOUND )
	set( MHD_FOUND FALSE )
	return()
endif()

if( NOT DAW_MHTTPD_STATIC_MHD )
	pkg_check_modules( MHD QUIET IMPORTED_TARGET libmicrohttpd>=1.0 )
	return()
endif()

pkg_check_modules( MHD QUIET libmicrohttpd>=1.0 )
if( NOT MHD_FOUND )
	return()
endif()

add_library( PkgConfig::MHD INTERFACE IMPORTED )
set_target_properties( PkgConfig::MHD PROPERTIES
											 INTERFACE_INCLUDE_DIRECTORIES "${MHD_STATIC_INCLUDE_DIRS}"
											 INTERFACE_COMPILE_OPTIONS "${MHD_STATIC_CFLAGS_OTHER}" )

set( _daw_mhttpd_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES} )
set( _daw_mhttpd_mhd_libs )
foreach( _daw_mhttpd_lib IN LISTS MHD_STATIC_LIBRARIES )
	set( CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX} )
	find_library( DAW_MHTTPD_MHD_${_daw_mhttpd_lib}_LIBRARY ${_daw_mhttpd_lib}
								HINTS ${MHD_STATIC_LIBRARY_DIRS} )
	set( CMAKE_FIND_LIBRARY_SUFFIXES ${_daw_mhttpd_saved_suffixes} )
	if( DAW_MHTTPD_MHD_${_daw_mhttpd_lib}_LIBRARY )
		list( APPEND _daw_mhttpd_mhd_libs "${DAW_MHTTPD_MHD_${_daw_mhttpd_lib}_LIBRARY}" )
	elseif( _daw_mhttpd_lib STREQUAL "microhttpd" )
		message( WARNING "DAW_MHTTPD_STATIC_MHD is ON but no static libmicrohttpd was found" )
		set( MHD_FOUND FALSE )
		return()
	else()
		# No .a installed (e.g. Debian ships no libp11-kit.a); let the linker pick,
		# which works unless the whole link is -static
		list( APPEND _daw_mhttpd_mhd_libs "${_daw_mhttpd_lib}" )
	endif()
endforeach()
set_target_properties( PkgConfig::MHD PROPERTIES
											 INTERFACE_LINK_LIBRARIES "${_daw_mhttpd_mhd_libs}"
											 INTERFACE_LINK_DIRECTORIES "${MHD_STATIC_LIBRARY_DIRS}"
											 INTERFACE_LINK_OPTIONS "${MHD_STATIC_LDFLAGS_OTHER}" )
unset( _daw_mhttpd_mhd_libs )
unset( _daw_mhttpd_saved_suffixes )
