//  Copyright (c) 2007-2016 Hartmut Kaiser
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_RUNTIME_THREADS_EXECUTORS_CURRENT_EXECUTOR_HPP
#define HPX_RUNTIME_THREADS_EXECUTORS_CURRENT_EXECUTOR_HPP

#include <hpx/config.hpp>
#include <hpx/errors.hpp>
#include <hpx/threading_base/scheduler_mode.hpp>
#include <hpx/coroutines/thread_enums.hpp>
#include <hpx/runtime/threads/thread_executor.hpp>
#include <hpx/runtime/threads_fwd.hpp>
#include <hpx/state.hpp>
#include <hpx/timing/steady_clock.hpp>
#include <hpx/threading_base/thread_description.hpp>
#include <hpx/functional/unique_function.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

#include <hpx/config/warnings_prefix.hpp>

namespace hpx { namespace threads { namespace executors
{
    namespace detail
    {
        //////////////////////////////////////////////////////////////////////
        class HPX_EXPORT current_executor
          : public threads::detail::scheduled_executor_base
        {
        public:
            current_executor(policies::scheduler_base* scheduler);

            // Schedule the specified function for execution in this executor.
            // Depending on the subclass implementation, this may block in some
            // situations.
            void add(closure_type&& f, util::thread_description const& desc,
                threads::thread_state_enum initial_state, bool run_now,
                threads::thread_stacksize stacksize,
                threads::thread_schedule_hint schedulehint,
                error_code& ec) override;

            // Schedule given function for execution in this executor no sooner
            // than time abs_time. This call never blocks, and may violate
            // bounds on the executor's queue size.
            void add_at(util::steady_clock::time_point const& abs_time,
                closure_type&& f, util::thread_description const& desc,
                threads::thread_stacksize stacksize, error_code& ec) override;

            // Schedule given function for execution in this executor no sooner
            // than time rel_time from now. This call never blocks, and may
            // violate bounds on the executor's queue size.
            void add_after(util::steady_clock::duration const& rel_time,
                closure_type&& f, util::thread_description const& desc,
                threads::thread_stacksize stacksize, error_code& ec) override;

            // Return an estimate of the number of waiting tasks.
            std::uint64_t num_pending_closures(error_code& ec) const override;

            // Reset internal (round robin) thread distribution scheme
            void reset_thread_distribution() override;

            // Set the new scheduler mode
            void set_scheduler_mode(
                threads::policies::scheduler_mode mode) override;

            // Return the runtime status of the underlying scheduler
            hpx::state get_state() const;

            // retrieve executor id
            executor_id get_id() const override
            {
                return create_id(reinterpret_cast<std::size_t>(scheduler_base_));
            }

        protected:
            static threads::thread_result_type thread_function_nullary(
                closure_type func);

            // Return the requested policy element
            std::size_t get_policy_element(
                threads::detail::executor_parameter p,
                error_code& ec) const override;

        private:
            policies::scheduler_base* scheduler_base_;
        };
    }

    ///////////////////////////////////////////////////////////////////////////
    struct current_executor : public scheduled_executor
    {
        current_executor();
        explicit current_executor(policies::scheduler_base* scheduler);

        hpx::state get_state() const;
    };
}}}

namespace hpx { namespace threads {
    ///  Returns a reference to the executor which was used to create
    /// the given thread.
    ///
    /// \throws If <code>&ec != &throws</code>, never throws, but will set \a ec
    ///         to an appropriate value when an error occurs. Otherwise, this
    ///         function will throw an \a hpx#exception with an error code of
    ///         \a hpx#yield_aborted if it is signaled with \a wait_aborted.
    ///         If called outside of a HPX-thread, this function will throw
    ///         an \a hpx#exception with an error code of \a hpx::null_thread_id.
    ///         If this function is called while the thread-manager is not
    ///         running, it will throw an \a hpx#exception with an error code of
    ///         \a hpx#invalid_status.
    ///
    HPX_API_EXPORT threads::executors::current_executor get_executor(
        thread_id_type const& id, error_code& ec = throws);
}}

namespace hpx { namespace this_thread {
    /// Returns a reference to the executor which was used to create the current
    /// thread.
    ///
    /// \throws If <code>&ec != &throws</code>, never throws, but will set \a ec
    ///         to an appropriate value when an error occurs. Otherwise, this
    ///         function will throw an \a hpx#exception with an error code of
    ///         \a hpx#yield_aborted if it is signaled with \a wait_aborted.
    ///         If called outside of a HPX-thread, this function will throw
    ///         an \a hpx#exception with an error code of \a hpx::null_thread_id.
    ///         If this function is called while the thread-manager is not
    ///         running, it will throw an \a hpx#exception with an error code of
    ///         \a hpx#invalid_status.
    ///
    HPX_EXPORT threads::executors::current_executor get_executor(
        error_code& ec = throws);
}}

#include <hpx/config/warnings_suffix.hpp>

#endif /*HPX_RUNTIME_THREADS_EXECUTORS_CURRENT_EXECUTOR_HPP*/
