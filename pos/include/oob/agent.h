#pragma once

#include <iostream>
#include <vector>

#include "pos/include/common.h"
#include "pos/include/oob.h"

namespace oob_functions {


namespace agent_register_client {
    static constexpr uint64_t kMaxJobNameLen =  256;

    // payload format
    typedef struct oob_payload {
        /* client */
        char job_name[kMaxJobNameLen+1];
        /* server */
        bool is_registered;
    } oob_payload_t;

    // metadata of the client-side call
    typedef struct oob_call_data {
        std::string job_name;
    } oob_call_data_t;
} // namespace agent_register_client


} // namespace oob_functions