/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include "../http_protocol.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t scan = 0, end;

    if (size > KH_HEADER_CAP + 1)
        return 0;
    end = kh_header_end((const char *)data, size, &scan);
    if (end)
        (void)kh_parse((const char *)data, end);
    (void)kh_parse((const char *)data, size);
    return 0;
}
