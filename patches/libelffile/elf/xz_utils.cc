/*
 * Stubbed xz_utils.cc — XZ compression disabled for standalone builds.
 * XzCompress just copies data uncompressed, XzDecompress is a no-op.
 */

#include "elf/xz_utils.h"

#include <vector>
#include <cstring>

#include "base/array_ref.h"

namespace art {

void XzCompress(ArrayRef<const uint8_t> src,
                std::vector<uint8_t>* dst,
                int /*level*/,
                size_t /*block_size*/) {
  // No compression — just copy the data.
  dst->assign(src.begin(), src.end());
}

void XzDecompress(ArrayRef<const uint8_t> src, std::vector<uint8_t>* dst) {
  // Assume data is not actually compressed (since XzCompress is a no-op).
  dst->assign(src.begin(), src.end());
}

}  // namespace art
