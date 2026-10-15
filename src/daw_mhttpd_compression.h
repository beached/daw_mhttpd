// Internal to daw::mhttpd: the compressors behind config::compression
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace daw::mhttpd::detail {
	// `in` as one zstd frame with its size recorded, or std::nullopt if zstd
	// fails or this build has no zstd. The window is kept to 8 MiB, the most
	// RFC 9659 lets a client be asked for.
	[[nodiscard]] std::optional<std::vector<std::byte>>
	zstd_compress( std::span<std::byte const> in, int level );
} // namespace daw::mhttpd::detail
