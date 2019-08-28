//  Copyright (c) 2007-2016 Hartmut Kaiser
//  Copyright (c)      2011 Bryce Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_RUNTIME_RUNTIME_DISTRIBUTED_HPP)
#define HPX_RUNTIME_RUNTIME_DISTRIBUTED_HPP

#include <hpx/config.hpp>
#include <hpx/performance_counters/registry.hpp>
#include <hpx/runtime.hpp>
#include <hpx/runtime_impl_local.hpp>
#include <hpx/runtime/applier/applier.hpp>
#include <hpx/runtime/components/server/console_error_sink_singleton.hpp>
#include <hpx/runtime/naming/resolver_client.hpp>
#include <hpx/runtime/parcelset/locality.hpp>
#include <hpx/runtime/parcelset/parcelhandler.hpp>
#include <hpx/runtime/parcelset/parcelport.hpp>
#include <hpx/runtime/threads/policies/callback_notifier.hpp>
#include <hpx/util/generate_unique_ids.hpp>
#include <hpx/util/io_service_pool.hpp>
#include <hpx/util_fwd.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <hpx/config/warnings_prefix.hpp>

namespace hpx
{
    class HPX_EXPORT runtime_distributed
    {
    public:
        virtual ~runtime_distributed() {}

        virtual parcelset::parcelhandler& get_parcel_handler() = 0;
        virtual parcelset::parcelhandler const& get_parcel_handler() const = 0;
        virtual naming::resolver_client& get_agas_client() = 0;
        virtual parcelset::endpoints_type const& endpoints() const = 0;
        virtual applier::applier& get_applier() = 0;
        virtual std::uint64_t get_runtime_support_lva() const = 0;
        virtual std::uint64_t get_memory_lva() const = 0;
        virtual naming::gid_type get_next_id(std::size_t count = 1) = 0;
        virtual util::unique_id_ranges& get_id_pool() = 0;

        virtual void register_message_handler(char const* message_handler_type,
            char const* action, error_code& ec = throws) = 0;
        virtual parcelset::policies::message_handler* create_message_handler(
            char const* message_handler_type, char const* action,
            parcelset::parcelport* pp, std::size_t num_messages,
            std::size_t interval, error_code& ec = throws) = 0;
        virtual serialization::binary_filter* create_binary_filter(
            char const* binary_filter_type, bool compress,
            serialization::binary_filter* next_filter, error_code& ec = throws) = 0;
    };
}

#include <hpx/config/warnings_suffix.hpp>

#endif
