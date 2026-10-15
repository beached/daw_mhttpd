// HTML form bodies: application/x-www-form-urlencoded and multipart/form-data
#pragma once

#include "daw/mhttpd/daw_mhttpd.h"

#include <daw/daw_integer.h>
#include <daw/daw_string_view.h>

#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

struct MHD_PostProcessor;

namespace daw::mhttpd::detail {
	struct form_parser_access;
} // namespace daw::mhttpd::detail

namespace daw::mhttpd {
	// The body is not a form, or is not a well-formed one
	class form_error : public std::runtime_error {
	public:
		using std::runtime_error::runtime_error;
	};

	struct form_field {
		std::string name{ };
		// Set for a file upload; may be empty if the browser sent no name
		std::optional<std::string> filename{ };
		std::optional<std::string> content_type{ };
		std::string value{ };
	};

	class form_data {
	public:
		form_data( ) = default;
		explicit form_data( std::vector<form_field> fields ) noexcept
		  : m_fields{ std::move( fields ) } {}

		[[nodiscard]] std::span<form_field const> fields( ) const noexcept {
			return m_fields;
		}

		// The first field named `name`, or nullptr
		[[nodiscard]] form_field const *find( daw::string_view name ) const noexcept;

	private:
		std::vector<form_field> m_fields;
	};

	// Parses the buffered body as a form; for a coroutine handler, after
	// read_body( ). Throws form_error if the Content-Type is not a form type or
	// the body is malformed.
	[[nodiscard]] form_data parse_form( request const &req );

	// A piece of one field's value. offset 0 starts a new field; later pieces
	// of the same field follow with increasing offsets. The views are only
	// valid during the call.
	struct form_chunk {
		daw::string_view name;
		std::optional<daw::string_view> filename;
		std::optional<daw::string_view> content_type;
		std::span<std::byte const> data;
		daw::u64 offset;
	};

	// Incremental form parser for bodies too large to buffer, typically fed
	// from request::next_chunk in a coroutine handler:
	//
	//   auto parser = daw::mhttpd::form_parser{ req, [&]( auto const &chunk ) {
	//     ...
	//   } };
	//   while( auto chunk = co_await req.next_chunk( ) ) {
	//     parser.feed( *chunk );
	//   }
	//   parser.finish( );
	//
	// on_chunk runs inside feed and finish. Only the parser buffers, and only
	// field names and part headers; values pass straight through.
	class form_parser {
	public:
		using chunk_handler = std::move_only_function<void( form_chunk const & )>;

		// Throws form_error if the request's Content-Type is not a form type
		form_parser( request const &req, chunk_handler on_chunk );
		form_parser( form_parser const & ) = delete;
		form_parser &operator=( form_parser const & ) = delete;
		~form_parser( );

		// Throws form_error for a malformed body, or what on_chunk threw. The
		// parser is unusable after it throws.
		void feed( std::span<std::byte const> data );

		// Call after the last feed: delivers the last url-encoded value and
		// throws form_error if the body ended early
		void finish( );

	private:
		friend struct detail::form_parser_access;
		void rethrow_if_failed( );

		chunk_handler m_on_chunk;
		MHD_PostProcessor *m_pp = nullptr;
		std::exception_ptr m_error;
	};
} // namespace daw::mhttpd
