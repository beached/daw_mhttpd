#include "daw/mhttpd/daw_mhttpd_form.h"

#include <daw/daw_as.h>

#include <microhttpd.h>

#include <optional>
#include <span>
#include <utility>

namespace daw::mhttpd::detail {
	struct form_parser_access {
		// Field names and part headers must fit; values are not buffered
		static constexpr std::size_t buffer_size = 16 * 1024;

		static MHD_Result on_data( void *cls, MHD_ValueKind, char const *key,
		                           char const *filename, char const *content_type,
		                           char const *, char const *data,
		                           std::uint64_t off, std::size_t size ) noexcept {
			auto &parser = *as<form_parser *>( cls );
			if( not parser.m_on_chunk ) {
				return MHD_NO; // being destroyed
			}
			auto const optional_view =
			  []( char const *p ) -> std::optional<daw::string_view> {
				if( p == nullptr ) {
					return std::nullopt;
				}
				return daw::string_view{ p };
			};
			try {
				parser.m_on_chunk(
				  form_chunk{ .name = key != nullptr ? daw::string_view{ key }
				                                     : daw::string_view{ },
				              .filename = optional_view( filename ),
				              .content_type = optional_view( content_type ),
				              .data = std::as_bytes( std::span{ data, size } ),
				              .offset = daw::u64( off ) } );
				return MHD_YES;
			} catch( ... ) {
				parser.m_error = std::current_exception( );
				return MHD_NO;
			}
		}

		static MHD_PostProcessor *create( form_parser &parser,
		                                  MHD_Connection *connection ) {
			return MHD_create_post_processor(
			  connection, buffer_size, &on_data, &parser );
		}
	};
} // namespace daw::mhttpd::detail

namespace daw::mhttpd {
	form_field const *form_data::find( daw::string_view name ) const noexcept {
		for( auto const &field : m_fields ) {
			if( field.name == name ) {
				return &field;
			}
		}
		return nullptr;
	}

	form_parser::form_parser( request const &req, chunk_handler on_chunk )
	  : m_on_chunk{ std::move( on_chunk ) } {
		m_pp = detail::form_parser_access::create( *this, req.m_connection );
		if( m_pp == nullptr ) {
			throw form_error{
			  "daw::mhttpd: the body is not application/x-www-form-urlencoded or "
			  "multipart/form-data" };
		}
	}

	form_parser::~form_parser( ) {
		if( m_pp != nullptr ) {
			m_on_chunk = nullptr; // destroying may deliver a last value; drop it
			(void)MHD_destroy_post_processor( m_pp );
		}
	}

	void form_parser::rethrow_if_failed( ) {
		if( m_error ) {
			std::rethrow_exception( std::exchange( m_error, nullptr ) );
		}
	}

	void form_parser::feed( std::span<std::byte const> data ) {
		if( m_pp == nullptr ) {
			throw form_error{ "daw::mhttpd: form_parser used after finish" };
		}
		if( data.empty( ) ) {
			return;
		}
		if( MHD_post_process( m_pp,
		                      reinterpret_cast<char const *>( data.data( ) ),
		                      data.size( ) ) != MHD_YES ) {
			m_on_chunk = nullptr;
			(void)MHD_destroy_post_processor( std::exchange( m_pp, nullptr ) );
			rethrow_if_failed( );
			throw form_error{ "daw::mhttpd: malformed form body" };
		}
	}

	void form_parser::finish( ) {
		if( m_pp == nullptr ) {
			throw form_error{ "daw::mhttpd: form_parser used after finish" };
		}
		auto const ok =
		  MHD_destroy_post_processor( std::exchange( m_pp, nullptr ) );
		rethrow_if_failed( );
		if( ok != MHD_YES ) {
			throw form_error{
			  "daw::mhttpd: the form body is incomplete or malformed" };
		}
	}

	form_data parse_form( request const &req ) {
		auto fields = std::vector<form_field>{ };
		auto parser =
		  form_parser{ req, [&]( form_chunk const &chunk ) {
			              if( chunk.offset == 0 or fields.empty( ) ) {
				              auto &field = fields.emplace_back( );
				              field.name = std::string{ chunk.name };
				              if( chunk.filename ) {
					              field.filename.emplace( *chunk.filename );
				              }
				              if( chunk.content_type ) {
					              field.content_type.emplace( *chunk.content_type );
				              }
			              }
			              auto const text = std::span{
			                reinterpret_cast<char const *>( chunk.data.data( ) ),
			                chunk.data.size( ) };
			              fields.back( ).value.append( text.begin( ), text.end( ) );
		              } };
		parser.feed( req.body( ) );
		parser.finish( );
		return form_data{ std::move( fields ) };
	}
} // namespace daw::mhttpd
