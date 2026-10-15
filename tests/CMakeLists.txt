# Copyright (c) Darrell Wright
#
# Distributed under the GNU Lesser General Public License, Version 2.1. (See
# accompanying file LICENSE)
#
# Official repository: https://github.com/beached/cpp_microhttpd
#

add_executable( daw_mhttpd_test src/daw_mhttpd_test.cpp )
target_link_libraries( daw_mhttpd_test PRIVATE daw::daw-mhttpd daw::daw-header-libraries )
target_compile_options( daw_mhttpd_test PRIVATE -Wall -Wextra )

add_executable( static_files_test src/static_files_test.cpp )
target_include_directories( static_files_test PRIVATE ${PROJECT_SOURCE_DIR}/examples/src )
target_link_libraries( static_files_test PRIVATE daw::daw-mhttpd daw::daw-header-libraries )
target_compile_options( static_files_test PRIVATE -Wall -Wextra )
add_test( NAME static_files_test COMMAND static_files_test )

# Each test runs in its own process; daw_ensure terminates on failure
set( DAW_MHTTPD_TESTS
		 sync_handler_with_header
		 response_headers_readable_before_send
		 invalid_response_header_throws
		 header_rejected_by_mhd_gives_500
		 unsent_response_frees_body
		 request_accessors
		 post_body_echo
		 post_large_body_echo
		 mutable_body_in_place
		 take_body_moves_buffer
		 body_too_large_content_length
		 body_too_large_chunked
		 static_response
		 stream_response
		 responder_completed_from_another_thread
		 dropped_responder_gives_500
		 handler_exception_gives_500
		 second_send_is_rejected
		 race_between_send_and_suspend
		 stop_answers_suspended_requests_with_503
		 stop_answers_requests_suspended_on_every_thread
		 coroutine_handler_completes_synchronously
		 coroutine_handler_resumed_on_another_thread
		 coroutine_handler_exception_gives_500
		 abandoned_completion_throws
		 sync_wait_deep_chain
		 sync_wait_propagates_exception
		 coroutine_outlives_daemon
		 stream_body_bypasses_buffer_limit
		 read_body_echo
		 read_body_large_echo
		 read_body_then_mutate_and_take
		 read_body_too_large_gives_413
		 early_response_without_reading_body
		 backpressure_while_coroutine_busy
		 respond_before_body_finished
		 client_disconnect_aborts_body
		 request_stop_from_handler
		 wait_returns_when_stopped_elsewhere
		 request_stop_before_wait
		 request_method
		 unique_fd_ownership
		 keep_alive_every_handler_style
		 early_response_with_body_closes
		 remote_address_ipv4
		 bind_ipv4_loopback
		 bind_ipv6_loopback
		 bind_dual_stack
		 bind_invalid_address_throws
		 connection_limit_queues_extra
		 per_ip_connection_limit_closes_extra
		 connection_limit_below_pool_throws
		 thread_count_resolved
		 single_thread_serves_many_keep_alive
		 wait_for_signals_stops_on_signal
		 remove_stop_signals_restores_handler
		 second_signal_during_shutdown_gets_original_handler
		 one_signal_stops_every_daemon
		 wait_for_uncatchable_signal_throws
		 wait_for_signals_after_stop_returns
		 add_stop_signals_stops_without_waiting
		 add_stop_signals_then_wait
		 add_stop_signals_makes_request_stop_stop
		 add_then_remove_stop_signals
		 add_stop_signals_then_destroy
		 added_signal_ends_wait_for_signals
		 )
foreach( test_name IN LISTS DAW_MHTTPD_TESTS )
	add_test( NAME ${test_name} COMMAND daw_mhttpd_test ${test_name} )
endforeach()

add_executable( router_test src/router_test.cpp )
target_link_libraries( router_test PRIVATE daw::daw-mhttpd daw::daw-header-libraries )
target_compile_options( router_test PRIVATE -Wall -Wextra )
set( DAW_MHTTPD_ROUTER_TESTS
		 router_path_params
		 router_literal_beats_param
		 router_first_added_wins_tie
		 router_rest_param
		 router_root_and_trailing_slash
		 router_method_not_allowed
		 router_head_uses_get
		 router_not_found_handler
		 router_unknown_method
		 router_sync_route_gets_body
		 router_callback_route
		 router_handler_exception_gives_500
		 router_coroutine_route_streams_body
		 router_bad_patterns_throw
		 )
foreach( test_name IN LISTS DAW_MHTTPD_ROUTER_TESTS )
	add_test( NAME ${test_name} COMMAND router_test ${test_name} )
endforeach()

add_executable( form_test src/form_test.cpp )
target_link_libraries( form_test PRIVATE daw::daw-mhttpd daw::daw-header-libraries )
target_compile_options( form_test PRIVATE -Wall -Wextra )
set( DAW_MHTTPD_FORM_TESTS
		 form_urlencoded
		 form_multipart
		 form_find
		 form_not_a_form
		 form_truncated_multipart
		 form_handler_exception_propagates
		 form_streamed_upload
		 )
foreach( test_name IN LISTS DAW_MHTTPD_FORM_TESTS )
	add_test( NAME ${test_name} COMMAND form_test ${test_name} )
endforeach()

add_executable( log_test src/log_test.cpp )
target_include_directories( log_test PRIVATE ${PROJECT_SOURCE_DIR}/examples/src )
target_link_libraries( log_test PRIVATE daw::daw-mhttpd daw::daw-header-libraries )
target_compile_options( log_test PRIVATE -Wall -Wextra )
set( DAW_MHTTPD_LOG_TESTS
		 log_default_level_is_error
		 log_set_level
		 log_registered_with_spdlog
		 log_errors_by_default
		 log_off_silences_errors
		 log_server_info_and_debug
		 log_libmicrohttpd_warnings
		 log_libmicrohttpd_hidden_at_error
		 log_router_debug
		 log_static_files_debug
		 )
foreach( test_name IN LISTS DAW_MHTTPD_LOG_TESTS )
	add_test( NAME ${test_name} COMMAND log_test ${test_name} )
endforeach()

add_executable( compression_test src/compression_test.cpp )
target_link_libraries( compression_test PRIVATE daw::daw-mhttpd daw::daw-header-libraries )
if( DAW_MHTTPD_ENABLE_ZSTD )
	# To decompress what the daemon sends
	target_link_libraries( compression_test PRIVATE ${DAW_MHTTPD_ZSTD_TARGET} )
	target_compile_definitions( compression_test PRIVATE DAW_MHTTPD_TEST_ZSTD )
endif()
target_compile_options( compression_test PRIVATE -Wall -Wextra )
set( DAW_MHTTPD_COMPRESSION_TESTS
		 accepts_coding_parsing
		 compressible_types
		 compresses_when_accepted
		 static_body_compressed
		 static_body_cached
		 static_cache_disabled
		 static_cache_full
		 static_bodies_kept_apart
		 static_no_gain_sends_identity
		 identity_without_accept_encoding
		 disabled_by_default
		 ineligible_responses_untouched
		 existing_content_encoding_untouched
		 etag_becomes_weak
		 vary_merged
		 custom_compressible_predicate
		 no_gain_sends_identity
		 response_coding_reported
		 router_responses_compressed
		 )
foreach( test_name IN LISTS DAW_MHTTPD_COMPRESSION_TESTS )
	add_test( NAME ${test_name} COMMAND compression_test ${test_name} )
endforeach()
