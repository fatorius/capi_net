#pragma once

#include <string>
#include <string_view>

namespace capi::server {

// Compressão gzip (formato de arquivo, não zlib cru) para game_pgns.pgn_gz.
// Os bytes comprimidos trafegam em std::string. Lançam std::runtime_error.
std::string gzip_compress(std::string_view plain);
std::string gzip_decompress(std::string_view compressed);

}  // namespace capi::server
