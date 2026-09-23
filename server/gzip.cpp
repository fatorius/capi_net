#include "server/gzip.hpp"

#include <zlib.h>

#include <stdexcept>

namespace capi::server {

namespace {
constexpr int kGzipWindowBits = 15 + 16;  // +16 = cabeçalho gzip
constexpr std::size_t kChunk = 16384;
}  // namespace

std::string gzip_compress(std::string_view plain) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, kGzipWindowBits, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(plain.data()));
    zs.avail_in = static_cast<uInt>(plain.size());

    std::string out;
    int rc;
    do {
        char buf[kChunk];
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        rc = deflate(&zs, Z_FINISH);
        if (rc == Z_STREAM_ERROR) {
            deflateEnd(&zs);
            throw std::runtime_error("deflate failed");
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (rc != Z_STREAM_END);
    deflateEnd(&zs);
    return out;
}

std::string gzip_decompress(std::string_view compressed) {
    z_stream zs{};
    if (inflateInit2(&zs, kGzipWindowBits) != Z_OK) {
        throw std::runtime_error("inflateInit2 failed");
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    zs.avail_in = static_cast<uInt>(compressed.size());

    std::string out;
    int rc;
    do {
        char buf[kChunk];
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&zs);
            throw std::runtime_error("invalid gzip data");
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (rc == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) {
            inflateEnd(&zs);
            throw std::runtime_error("truncated gzip data");
        }
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

}  // namespace capi::server
