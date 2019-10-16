//  Copyright (c) 2007-2018 Hartmut Kaiser
//  Copyright (c)      2011 Bryce Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/config.hpp>
#include <hpx/assertion.hpp>
#include <hpx/concurrency/thread_name.hpp>
#include <hpx/custom_exception_info.hpp>
#include <hpx/errors.hpp>
#include <hpx/functional.hpp>
#include <hpx/logging.hpp>
#include <hpx/runtime.hpp>
#include <hpx/runtime/config_entry.hpp>
#include <hpx/runtime/shutdown_function.hpp>
#include <hpx/runtime/startup_function.hpp>
#include <hpx/runtime/thread_hooks.hpp>
#include <hpx/runtime/threads/policies/scheduler_mode.hpp>
#include <hpx/runtime/threads/scoped_background_timer.hpp>
#include <hpx/runtime/threads/threadmanager.hpp>
#include <hpx/state.hpp>
#include <hpx/thread_support/set_thread_name.hpp>
#include <hpx/timing/high_resolution_clock.hpp>
#include <hpx/topology/topology.hpp>
#include <hpx/util/apex.hpp>
#include <hpx/util/command_line_handling.hpp>
#include <hpx/util/debugging.hpp>
#include <hpx/util/safe_lexical_cast.hpp>
#include <hpx/util/static_reinit.hpp>
#include <hpx/util/thread_mapper.hpp>
#include <hpx/util/yield_while.hpp>
#include <hpx/version.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

///////////////////////////////////////////////////////////////////////////////
// Make sure the system gets properly shut down while handling Ctrl-C and other
// system signals
#if defined(HPX_WINDOWS)

namespace hpx {
    ///////////////////////////////////////////////////////////////////////////
    void handle_termination(char const* reason)
    {
        if (get_config_entry("hpx.attach_debugger", "") == "exception")
        {
            util::attach_debugger();
        }

        if (get_config_entry("hpx.diagnostics_on_terminate", "1") == "1")
        {
            std::cerr
#if defined(HPX_HAVE_STACKTRACES)
                << "{stack-trace}: " << hpx::util::trace() << "\n"
#endif
                << "{what}: " << (reason ? reason : "Unknown reason") << "\n"
                << full_build_string();    // add full build information
        }
    }

    HPX_EXPORT BOOL WINAPI termination_handler(DWORD ctrl_type)
    {
        switch (ctrl_type)
        {
        case CTRL_C_EVENT:
            handle_termination("Ctrl-C");
            return TRUE;

        case CTRL_BREAK_EVENT:
            handle_termination("Ctrl-Break");
            return TRUE;

        case CTRL_CLOSE_EVENT:
            handle_termination("Ctrl-Close");
            return TRUE;

        case CTRL_LOGOFF_EVENT:
            handle_termination("Logoff");
            return TRUE;

        case CTRL_SHUTDOWN_EVENT:
            handle_termination("Shutdown");
            return TRUE;

        default:
            break;
        }
        return FALSE;
    }
}    // namespace hpx

#else

#include <signal.h>
#include <stdlib.h>
#include <string.h>

namespace hpx {
    ///////////////////////////////////////////////////////////////////////////
    HPX_EXPORT HPX_NORETURN void termination_handler(int signum)
    {
        if (signum != SIGINT &&
            get_config_entry("hpx.attach_debugger", "") == "exception")
        {
            util::attach_debugger();
        }

        if (get_config_entry("hpx.diagnostics_on_terminate", "1") == "1")
        {
            char* reason = strsignal(signum);
            std::cerr
#if defined(HPX_HAVE_STACKTRACES)
                << "{stack-trace}: " << hpx::util::trace() << "\n"
#endif
                << "{what}: " << (reason ? reason : "Unknown signal") << "\n"
                << full_build_string();    // add full build information
        }
        std::abort();
    }
}    // namespace hpx

#endif

///////////////////////////////////////////////////////////////////////////////
namespace hpx {
    ///////////////////////////////////////////////////////////////////////////
    HPX_EXPORT void HPX_CDECL new_handler()
    {
        HPX_THROW_EXCEPTION(out_of_memory, "new_handler",
            "new allocator failed to allocate memory");
    }

    void set_error_handlers()
    {
#if defined(HPX_WINDOWS)
        // Set console control handler to allow server to be stopped.
        SetConsoleCtrlHandler(hpx::termination_handler, TRUE);
#else
        struct sigaction new_action;
        new_action.sa_handler = hpx::termination_handler;
        sigemptyset(&new_action.sa_mask);
        new_action.sa_flags = 0;

        sigaction(SIGINT, &new_action, nullptr);     // Interrupted
        sigaction(SIGBUS, &new_action, nullptr);     // Bus error
        sigaction(SIGFPE, &new_action, nullptr);     // Floating point exception
        sigaction(SIGILL, &new_action, nullptr);     // Illegal instruction
        sigaction(SIGPIPE, &new_action, nullptr);    // Bad pipe
        sigaction(SIGSEGV, &new_action, nullptr);    // Segmentation fault
        sigaction(SIGSYS, &new_action, nullptr);     // Bad syscall
#endif

        std::set_new_handler(hpx::new_handler);
    }

    ///////////////////////////////////////////////////////////////////////////
    namespace strings {
        char const* const runtime_mode_names[] = {
            "invalid",    // -1
            "console",    // 0
            "worker",     // 1
            "connect",    // 2
            "default",    // 3
        };
    }

    char const* get_runtime_mode_name(runtime_mode state)
    {
        if (state < runtime_mode_invalid || state >= runtime_mode_last)
            return "invalid (value out of bounds)";
        return strings::runtime_mode_names[state + 1];
    }

    runtime_mode get_runtime_mode_from_name(std::string const& mode)
    {
        for (std::size_t i = 0; i < runtime_mode_last; ++i)
        {
            if (mode == strings::runtime_mode_names[i])
                return static_cast<runtime_mode>(i - 1);
        }
        return runtime_mode_invalid;
    }

    namespace strings {
        char const* const runtime_state_names[] = {
            "state_invalid",         // -1
            "state_initialized",     // 0
            "state_pre_startup",     // 1
            "state_startup",         // 2
            "state_pre_main",        // 3
            "state_starting",        // 4
            "state_running",         // 5
            "state_suspended",       // 6
            "state_pre_sleep",       // 7
            "state_sleeping",        // 8
            "state_pre_shutdown",    // 9
            "state_shutdown",        // 10
            "state_stopping",        // 11
            "state_terminating",     // 12
            "state_stopped"          // 13
        };
    }

    char const* get_runtime_state_name(state st)
    {
        if (st < state_invalid || st >= last_valid_runtime_state)
            return "invalid (value out of bounds)";
        return strings::runtime_state_names[st + 1];
    }

    ///////////////////////////////////////////////////////////////////////////
    threads::policies::callback_notifier::on_startstop_type
        global_on_start_func;
    threads::policies::callback_notifier::on_startstop_type global_on_stop_func;
    threads::policies::callback_notifier::on_error_type global_on_error_func;

    ///////////////////////////////////////////////////////////////////////////
    runtime::runtime(util::runtime_configuration& rtcfg)
      : ini_(rtcfg)
      , instance_number_(++instance_number_counter_)
      , thread_support_(new util::thread_mapper)
      , topology_(resource::get_partitioner().get_topology())
      , state_(state_invalid)
      , on_start_func_(global_on_start_func)
      , on_stop_func_(global_on_stop_func)
      , on_error_func_(global_on_error_func)
      , result_(0)
      , main_pool_notifier_()
      , main_pool_(1, main_pool_notifier_, "main_pool")
#ifdef HPX_HAVE_IO_POOL
      , io_pool_notifier_(runtime::get_notification_policy("io-thread"))
      , io_pool_(
            rtcfg.get_thread_pool_size("io_pool"), io_pool_notifier_, "io_pool")
#endif
#ifdef HPX_HAVE_TIMER_POOL
      , timer_pool_notifier_(runtime::get_notification_policy("timer-thread"))
      , timer_pool_(rtcfg.get_thread_pool_size("timer_pool"),
            timer_pool_notifier_, "timer_pool")
#endif
      , notifier_(runtime::get_notification_policy("worker-thread"))
      , thread_manager_(new hpx::threads::threadmanager(
#ifdef HPX_HAVE_TIMER_POOL
            timer_pool_,
#endif
            notifier_))
    {
        init(true);
    }

    runtime::runtime(util::runtime_configuration& rtcfg,
         notification_policy_type&& notifier,
         notification_policy_type&& main_pool_notifier,
#ifdef HPX_HAVE_IO_POOL
         notification_policy_type&& io_pool_notifier,
#endif
#ifdef HPX_HAVE_IO_POOL
         notification_policy_type&& timer_pool_notifier,
#endif
#ifdef HPX_HAVE_NETWORKING
         threads::detail::network_background_callback_type network_background_callback
#endif
          )
      : ini_(rtcfg)
      , instance_number_(++instance_number_counter_)
      , thread_support_(new util::thread_mapper)
      , topology_(resource::get_partitioner().get_topology())
      , state_(state_invalid)
      , on_start_func_(global_on_start_func)
      , on_stop_func_(global_on_stop_func)
      , on_error_func_(global_on_error_func)
      , result_(0)
      , main_pool_notifier_()
      , main_pool_(1, main_pool_notifier_, "main_pool")
#ifdef HPX_HAVE_IO_POOL
      , io_pool_notifier_(io_pool_notifier)
      , io_pool_(
            rtcfg.get_thread_pool_size("io_pool"), io_pool_notifier_, "io_pool")
#endif
#ifdef HPX_HAVE_TIMER_POOL
      , timer_pool_notifier_(timer_pool_notifier)
      , timer_pool_(rtcfg.get_thread_pool_size("timer_pool"),
            timer_pool_notifier_, "timer_pool")
#endif
      , notifier_(notifier)
      , thread_manager_(new hpx::threads::threadmanager(
#ifdef HPX_HAVE_TIMER_POOL
            timer_pool_,
#endif
            notifier_,
#ifdef HPX_HAVE_NETWORKING
            network_background_callback
#endif
))
    {
        init(false);
    }

    void runtime::init(bool init_threadmanager)
    {
        LPROGRESS_;

        // initialize our TSS
        runtime::init_tss();
        util::reinit_construct();    // call only after TLS was initialized

        LPROGRESS_;

        // TODO: Better way to handle delaying this?
        if (init_threadmanager)
        {
            // now create all threadmanager pools
            thread_manager_->create_pools();

            // this initializes the used_processing_units_ mask
            thread_manager_->init();
        }

        // copy over all startup functions registered so far
        for (startup_function_type& f : detail::global_pre_startup_functions)
        {
            add_pre_startup_function(std::move(f));
        }
        detail::global_pre_startup_functions.clear();

        for (startup_function_type& f : detail::global_startup_functions)
        {
            add_startup_function(std::move(f));
        }
        detail::global_startup_functions.clear();

        for (shutdown_function_type& f : detail::global_pre_shutdown_functions)
        {
            add_pre_shutdown_function(std::move(f));
        }
        detail::global_pre_shutdown_functions.clear();

        for (shutdown_function_type& f : detail::global_shutdown_functions)
        {
            add_shutdown_function(std::move(f));
        }
        detail::global_shutdown_functions.clear();

        // set state to initialized
        set_state(state_initialized);
    }

    runtime::~runtime()
    {
        LRT_(debug) << "~runtime_local(entering)";

        // stop all services
        thread_manager_->stop();    // stops timer_pool_ as well
#ifdef HPX_HAVE_IO_POOL
        io_pool_.stop();
#endif
        LRT_(debug) << "~runtime_local(finished)";

        LPROGRESS_;

        // allow to reuse instance number if this was the only instance
        if (0 == instance_number_counter_)
            --instance_number_counter_;

        util::reinit_destruct();
        resource::detail::delete_partitioner();
    }

    void runtime::set_state(state s)
    {
        LPROGRESS_ << get_runtime_state_name(s);
        state_.store(s);
    }

    ///////////////////////////////////////////////////////////////////////////
    std::atomic<int> runtime::instance_number_counter_(-1);

    ///////////////////////////////////////////////////////////////////////////
    namespace {
        std::uint64_t& runtime_uptime()
        {
            static HPX_NATIVE_TLS std::uint64_t uptime;
            return uptime;
        }
    }    // namespace

    void runtime::init_tss()
    {
        // initialize our TSS
        runtime*& runtime_ = get_runtime_ptr();
        if (nullptr == runtime_)
        {
            HPX_ASSERT(nullptr == threads::thread_self::get_self());

            runtime_ = this;
            runtime_uptime() = util::high_resolution_clock::now();
        }
    }

    void runtime::deinit_tss()
    {
        // reset our TSS
        runtime_uptime() = 0;
        get_runtime_ptr() = nullptr;
        threads::reset_continuation_recursion_count();
    }

    std::uint64_t runtime::get_system_uptime()
    {
        std::int64_t diff =
            util::high_resolution_clock::now() - runtime_uptime();
        return diff < 0LL ? 0ULL : static_cast<std::uint64_t>(diff);
    }

    ///////////////////////////////////////////////////////////////////////////
    threads::policies::callback_notifier::on_startstop_type
    runtime::on_start_func() const
    {
        return on_start_func_;
    }

    threads::policies::callback_notifier::on_startstop_type
    runtime::on_stop_func() const
    {
        return on_stop_func_;
    }

    threads::policies::callback_notifier::on_error_type runtime::on_error_func()
        const
    {
        return on_error_func_;
    }

    threads::policies::callback_notifier::on_startstop_type
    runtime::on_start_func(
        threads::policies::callback_notifier::on_startstop_type&& f)
    {
        threads::policies::callback_notifier::on_startstop_type newf =
            std::move(f);
        std::swap(on_start_func_, newf);
        return newf;
    }

    threads::policies::callback_notifier::on_startstop_type
    runtime::on_stop_func(
        threads::policies::callback_notifier::on_startstop_type&& f)
    {
        threads::policies::callback_notifier::on_startstop_type newf =
            std::move(f);
        std::swap(on_stop_func_, newf);
        return newf;
    }

    threads::policies::callback_notifier::on_error_type runtime::on_error_func(
        threads::policies::callback_notifier::on_error_type&& f)
    {
        threads::policies::callback_notifier::on_error_type newf = std::move(f);
        std::swap(on_error_func_, newf);
        return newf;
    }

    ///////////////////////////////////////////////////////////////////////////
    threads::policies::callback_notifier::on_startstop_type
    get_thread_on_start_func()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            return rt->on_start_func();
        }
        else
        {
            return global_on_start_func;
        }
    }

    threads::policies::callback_notifier::on_startstop_type
    get_thread_on_stop_func()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            return rt->on_stop_func();
        }
        else
        {
            return global_on_stop_func;
        }
    }

    threads::policies::callback_notifier::on_error_type
    get_thread_on_error_func()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            return rt->on_error_func();
        }
        else
        {
            return global_on_error_func;
        }
    }

    threads::policies::callback_notifier::on_startstop_type
    register_thread_on_start_func(
        threads::policies::callback_notifier::on_startstop_type&& f)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            return rt->on_start_func(std::move(f));
        }

        threads::policies::callback_notifier::on_startstop_type newf =
            std::move(f);
        std::swap(global_on_start_func, newf);
        return newf;
    }

    threads::policies::callback_notifier::on_startstop_type
    register_thread_on_stop_func(
        threads::policies::callback_notifier::on_startstop_type&& f)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            return rt->on_stop_func(std::move(f));
        }

        threads::policies::callback_notifier::on_startstop_type newf =
            std::move(f);
        std::swap(global_on_stop_func, newf);
        return newf;
    }

    threads::policies::callback_notifier::on_error_type
    register_thread_on_error_func(
        threads::policies::callback_notifier::on_error_type&& f)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            return rt->on_error_func(std::move(f));
        }

        threads::policies::callback_notifier::on_error_type newf = std::move(f);
        std::swap(global_on_error_func, newf);
        return newf;
    }

    ///////////////////////////////////////////////////////////////////////////
    runtime& get_runtime()
    {
        HPX_ASSERT(get_runtime_ptr() != nullptr);
        return *get_runtime_ptr();
    }

    runtime*& get_runtime_ptr()
    {
        static HPX_NATIVE_TLS runtime* runtime_;
        return runtime_;
    }

    std::string get_thread_name()
    {
        std::string& thread_name = detail::thread_name();
        if (thread_name.empty())
            return "<unkown>";
        return thread_name;
    }

    /// Register the current kernel thread with HPX, this should be done once
    /// for each external OS-thread intended to invoke HPX functionality.
    /// Calling this function more than once will silently fail
    /// (will return false).
    bool register_thread(runtime* rt, char const* name, error_code& ec)
    {
        HPX_ASSERT(rt);
        return rt->register_thread(name, 0, true, ec);
    }

    /// Unregister the thread from HPX, this should be done once in
    /// the end before the external thread exists.
    void unregister_thread(runtime* rt)
    {
        HPX_ASSERT(rt);
        rt->unregister_thread();
    }

    void report_error(std::size_t num_thread, std::exception_ptr const& e)
    {
        // Early and late exceptions
        if (!threads::threadmanager_is(state_running))
        {
            hpx::runtime* rt = hpx::get_runtime_ptr();
            if (rt)
                rt->report_error(num_thread, e);
            else
                detail::report_exception_and_terminate(e);
            return;
        }

        get_runtime().get_thread_manager().report_error(num_thread, e);
    }

    void report_error(std::exception_ptr const& e)
    {
        // Early and late exceptions
        if (!threads::threadmanager_is(state_running))
        {
            hpx::runtime* rt = hpx::get_runtime_ptr();
            if (rt)
                rt->report_error(std::size_t(-1), e);
            else
                detail::report_exception_and_terminate(e);
            return;
        }

        std::size_t num_thread = hpx::get_worker_thread_num();
        get_runtime().get_thread_manager().report_error(num_thread, e);
    }

    bool register_on_exit(util::function_nonser<void()> const& f)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr == rt)
            return false;

        rt->on_exit(f);
        return true;
    }

    std::size_t get_runtime_instance_number()
    {
        runtime* rt = get_runtime_ptr();
        return (nullptr == rt) ? 0 : rt->get_instance_number();
    }

    ///////////////////////////////////////////////////////////////////////////
    std::string get_config_entry(
        std::string const& key, std::string const& dflt)
    {
        //! FIXME runtime_configuration should probs be a member of
        // hpx::runtime only, not command_line_handling
        //! FIXME change functions in this section accordingly
        if (get_runtime_ptr() != nullptr)
        {
            return get_runtime().get_config().get_entry(key, dflt);
        }
        if (!resource::is_partitioner_valid())
        {
            return dflt;
        }
        return resource::get_partitioner()
            .get_command_line_switches()
            .rtcfg_.get_entry(key, dflt);
    }

    std::string get_config_entry(std::string const& key, std::size_t dflt)
    {
        if (get_runtime_ptr() != nullptr)
        {
            return get_runtime().get_config().get_entry(key, dflt);
        }
        if (!resource::is_partitioner_valid())
        {
            return std::to_string(dflt);
        }
        return resource::get_partitioner()
            .get_command_line_switches()
            .rtcfg_.get_entry(key, dflt);
    }

    // set entries
    void set_config_entry(std::string const& key, std::string const& value)
    {
        if (get_runtime_ptr() != nullptr)
        {
            get_runtime_ptr()->get_config().add_entry(key, value);
            return;
        }
        if (resource::is_partitioner_valid())
        {
            resource::get_partitioner()
                .get_command_line_switches()
                .rtcfg_.add_entry(key, value);
            return;
        }
    }

    void set_config_entry(std::string const& key, std::size_t value)
    {
        if (get_runtime_ptr() != nullptr)
        {
            get_runtime_ptr()->get_config().add_entry(
                key, std::to_string(value));
            return;
        }
        if (resource::is_partitioner_valid())
        {
            resource::get_partitioner()
                .get_command_line_switches()
                .rtcfg_.add_entry(key, std::to_string(value));
            return;
        }
    }

    void set_config_entry_callback(std::string const& key,
        util::function_nonser<void(
            std::string const&, std::string const&)> const& callback)
    {
        if (get_runtime_ptr() != nullptr)
        {
            get_runtime_ptr()->get_config().add_notification_callback(
                key, callback);
            return;
        }
        if (resource::is_partitioner_valid())
        {
            resource::get_partitioner()
                .get_command_line_switches()
                .rtcfg_.add_notification_callback(key, callback);
            return;
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    std::size_t get_os_thread_count()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr == rt)
        {
            HPX_THROW_EXCEPTION(invalid_status, "hpx::get_os_thread_count()",
                "the runtime system has not been initialized yet");
            return std::size_t(0);
        }
        return rt->get_config().get_os_thread_count();
    }

    std::size_t get_os_thread_count(threads::executor const& exec)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr == rt)
        {
            HPX_THROW_EXCEPTION(invalid_status,
                "hpx::get_os_thread_count(exec)",
                "the runtime system has not been initialized yet");
            return std::size_t(0);
        }

        if (!exec)
            return rt->get_config().get_os_thread_count();

        error_code ec(lightweight);
        return exec.executor_data_->get_policy_element(
            threads::detail::current_concurrency, ec);
    }

    std::size_t get_worker_thread_num()
    {
        return get_worker_thread_num(throws);
    }

    std::size_t get_worker_thread_num(error_code& ec)
    {
        return threads::detail::get_thread_num_tss();
    }

    bool is_scheduler_numa_sensitive()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr == rt)
        {
            HPX_THROW_EXCEPTION(invalid_status,
                "hpx::is_scheduler_numa_sensitive",
                "the runtime system has not been initialized yet");
            return false;
        }

        bool numa_sensitive = false;
        if (std::size_t(-1) != get_worker_thread_num())
            return numa_sensitive;
        return false;
    }

    ///////////////////////////////////////////////////////////////////////////
    bool is_running()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
            return rt->get_state() == state_running;
        return false;
    }

    bool is_stopped()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
            return rt->get_state() == state_stopped;
        return true;    // assume stopped
    }

    bool is_stopped_or_shutting_down()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            state st = rt->get_state();
            return st >= state_shutdown;
        }
        return true;    // assume stopped
    }

    bool HPX_EXPORT tolerate_node_faults()
    {
#ifdef HPX_HAVE_FAULT_TOLERANCE
        return true;
#else
        return false;
#endif
    }

    bool HPX_EXPORT is_starting()
    {
        runtime* rt = get_runtime_ptr();
        return nullptr != rt ? rt->get_state() <= state_startup : true;
    }

    bool HPX_EXPORT is_pre_startup()
    {
        runtime* rt = get_runtime_ptr();
        return nullptr != rt ? rt->get_state() < state_startup : true;
    }
}    // namespace hpx

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace util {
    std::string expand(std::string const& in)
    {
        return get_runtime().get_config().expand(in);
    }

    void expand(std::string& in)
    {
        get_runtime().get_config().expand(in, std::string::size_type(-1));
    }
}}    // namespace hpx::util

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads {
    threadmanager& get_thread_manager()
    {
        return get_runtime().get_thread_manager();
    }

    // shortcut for runtime_configuration::get_default_stack_size
    std::ptrdiff_t get_default_stack_size()
    {
        return get_runtime().get_config().get_default_stack_size();
    }

    // shortcut for runtime_configuration::get_stack_size
    std::ptrdiff_t get_stack_size(threads::thread_stacksize stacksize)
    {
        if (stacksize == threads::thread_stacksize_current)
            return threads::get_self_stacksize();

        return get_runtime().get_config().get_stack_size(stacksize);
    }

    HPX_API_EXPORT void reset_thread_distribution()
    {
        get_runtime().get_thread_manager().reset_thread_distribution();
    }

    HPX_API_EXPORT void set_scheduler_mode(threads::policies::scheduler_mode m)
    {
        get_runtime().get_thread_manager().set_scheduler_mode(m);
    }

    HPX_API_EXPORT void add_scheduler_mode(threads::policies::scheduler_mode m)
    {
        get_runtime().get_thread_manager().add_scheduler_mode(m);
    }

    HPX_API_EXPORT void add_remove_scheduler_mode(
        threads::policies::scheduler_mode to_add_mode,
        threads::policies::scheduler_mode to_remove_mode)
    {
        get_runtime().get_thread_manager().add_remove_scheduler_mode(
            to_add_mode, to_remove_mode);
    }

    HPX_API_EXPORT void remove_scheduler_mode(
        threads::policies::scheduler_mode m)
    {
        get_runtime().get_thread_manager().remove_scheduler_mode(m);
    }

    HPX_API_EXPORT topology const& get_topology()
    {
        hpx::runtime* rt = hpx::get_runtime_ptr();
        if (rt == nullptr)
        {
            HPX_THROW_EXCEPTION(invalid_status, "hpx::threads::get_topology",
                "the hpx runtime system has not been initialized yet");
        }
        return rt->get_topology();
    }
}}    // namespace hpx::threads

///////////////////////////////////////////////////////////////////////////////
namespace hpx {
    std::uint64_t get_system_uptime()
    {
        return runtime::get_system_uptime();
    }

    util::runtime_configuration const& get_config()
    {
        return get_runtime().get_config();
    }

    hpx::util::io_service_pool* get_thread_pool(
        char const* name, char const* name_suffix)
    {
        std::string full_name(name);
        full_name += name_suffix;
        return get_runtime().get_thread_pool(full_name.c_str());
    }

    ///////////////////////////////////////////////////////////////////////////
    /// Return true if networking is enabled.
    bool is_networking_enabled()
    {
#if defined(HPX_HAVE_NETWORKING)
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            return rt->is_networking_enabled();
        }
        return true;    // be on the safe side, enable networking
#else
        return false;
#endif
    }
}    // namespace hpx

#if defined(_WIN64) && defined(_DEBUG) &&                                      \
    !defined(HPX_HAVE_FIBER_BASED_COROUTINES)
#include <io.h>
#endif

namespace hpx {
    namespace detail {
        ///////////////////////////////////////////////////////////////////////
        // There is no need to protect these global from thread concurrent
        // access as they are access during early startup only.
        std::list<startup_function_type> global_pre_startup_functions;
        std::list<startup_function_type> global_startup_functions;
        std::list<shutdown_function_type> global_pre_shutdown_functions;
        std::list<shutdown_function_type> global_shutdown_functions;
    }    // namespace detail

    ///////////////////////////////////////////////////////////////////////////
    void register_pre_startup_function(startup_function_type f)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            if (rt->get_state() > state_pre_startup)
            {
                HPX_THROW_EXCEPTION(invalid_status,
                    "register_pre_startup_function",
                    "Too late to register a new pre-startup function.");
                return;
            }
            rt->add_pre_startup_function(std::move(f));
        }
        else
        {
            detail::global_pre_startup_functions.push_back(std::move(f));
        }
    }

    void register_startup_function(startup_function_type f)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            if (rt->get_state() > state_startup)
            {
                HPX_THROW_EXCEPTION(invalid_status, "register_startup_function",
                    "Too late to register a new startup function.");
                return;
            }
            rt->add_startup_function(std::move(f));
        }
        else
        {
            detail::global_startup_functions.push_back(std::move(f));
        }
    }

    void register_pre_shutdown_function(shutdown_function_type f)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            if (rt->get_state() > state_pre_shutdown)
            {
                HPX_THROW_EXCEPTION(invalid_status,
                    "register_pre_shutdown_function",
                    "Too late to register a new pre-shutdown function.");
                return;
            }
            rt->add_pre_shutdown_function(std::move(f));
        }
        else
        {
            detail::global_pre_shutdown_functions.push_back(std::move(f));
        }
    }

    void register_shutdown_function(shutdown_function_type f)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr != rt)
        {
            if (rt->get_state() > state_shutdown)
            {
                HPX_THROW_EXCEPTION(invalid_status,
                    "register_shutdown_function",
                    "Too late to register a new shutdown function.");
                return;
            }
            rt->add_shutdown_function(std::move(f));
        }
        else
        {
            detail::global_shutdown_functions.push_back(std::move(f));
        }
    }

    threads::thread_result_type runtime::run_helper(
        util::function_nonser<runtime::hpx_main_function_type> const& func,
        int& result)
    {
        lbt_ << "(4th stage) runtime::run_helper: bootstrap complete";
        set_state(state_running);

        // Now, execute the user supplied thread function (hpx_main)
        if (!!func)
        {
            lbt_ << "(last stage) runtime::run_helper: about to "
                    "invoke hpx_main";

            // Change our thread description, as we're about to call hpx_main
            threads::set_thread_description(threads::get_self_id(), "hpx_main");

            // Call hpx_main
            result = func();
        }

        return threads::thread_result_type(
            threads::terminated, threads::invalid_thread_id);
    }

    int runtime::start(
        util::function_nonser<hpx_main_function_type> const& func,
        bool blocking)
    {
#if defined(_WIN64) && defined(_DEBUG) &&                                      \
    !defined(HPX_HAVE_FIBER_BASED_COROUTINES)
        // needs to be called to avoid problems at system startup
        // see: http://connect.microsoft.com/VisualStudio/feedback/ViewFeedback.aspx?FeedbackID=100319
        _isatty(0);
#endif
        // {{{ early startup code - local

        // initialize instrumentation system
        util::apex_init();

        LRT_(info) << "cmd_line: " << get_config().get_cmd_line();

        lbt_ << "(1st stage) runtime::start: booting locality " << here();

#ifdef HPX_HAVE_IO_POOL
        // start the io pool
        io_pool_.run(false);
        lbt_ << "(1st stage) runtime::start: started the application "
                "I/O service pool";
#endif
        // start the thread manager
        thread_manager_->run();
        lbt_ << "(1st stage) runtime::start: started threadmanager";
        // }}}

        // {{{ launch main
        // register the given main function with the thread manager
        lbt_ << "(1st stage) runtime::start: launching run_helper "
                "HPX thread";

        threads::thread_init_data data(
            util::bind(&runtime::run_helper, this, func, std::ref(result_)),
            "run_helper", threads::thread_priority_normal,
            threads::thread_schedule_hint(0),
            threads::get_stack_size(threads::thread_stacksize_large));

        this->runtime::starting();
        threads::thread_id_type id = threads::invalid_thread_id;
        thread_manager_->register_thread(data, id);

        // }}}

        // block if required
        if (blocking)
            return wait();    // wait for the shutdown_action to be executed

        // Register this thread with the runtime system to allow calling certain
        // HPX functionality from the main thread.
        init_tss_helper("main-thread", 0, 0, "", "", false);

        return 0;    // return zero as we don't know the outcome of hpx_main yet
    }

    int runtime::start(bool blocking)
    {
        util::function_nonser<hpx_main_function_type> empty_main;
        return start(empty_main, blocking);
    }

    ///////////////////////////////////////////////////////////////////////////
    std::string locality_prefix(util::runtime_configuration const& cfg)
    {
        std::string localities = cfg.get_entry("hpx.localities", "1");
        std::size_t num_localities =
            util::safe_lexical_cast<std::size_t>(localities, 1);
        if (num_localities > 1)
        {
            std::string locality = cfg.get_entry("hpx.locality", "");
            if (!locality.empty())
            {
                locality = "locality#" + locality;
            }
            return locality;
        }
        return "";
    }

    ///////////////////////////////////////////////////////////////////////////
    void runtime::wait_helper(
        std::mutex& mtx, std::condition_variable& cond, bool& running)
    {
        // signal successful initialization
        {
            std::lock_guard<std::mutex> lk(mtx);
            running = true;
            cond.notify_all();
        }

        // prefix thread name with locality number, if needed
        std::string locality = locality_prefix(get_config());

        // register this thread with any possibly active Intel tool
        std::string thread_name(locality + "main-thread#wait_helper");
        HPX_ITT_THREAD_SET_NAME(thread_name.c_str());

        // set thread name as shown in Visual Studio
        util::set_thread_name(thread_name.c_str());

        // stop main thread pool
        main_pool_.stop();
    }

    int runtime::wait()
    {
        LRT_(info) << "runtime_local: about to enter wait state";

        // start the wait_helper in a separate thread
        std::mutex mtx;
        std::condition_variable cond;
        bool running = false;

        std::thread t(util::bind(&runtime::wait_helper, this, std::ref(mtx),
            std::ref(cond), std::ref(running)));

        // wait for the thread to run
        {
            std::unique_lock<std::mutex> lk(mtx);
            while (!running)
                cond.wait(lk);
        }

        // use main thread to drive main thread pool
        main_pool_.thread_run(0);

        // block main thread
        t.join();

        util::yield_while(
            [this]() {
                return thread_manager_->get_thread_count() >
                    thread_manager_->get_background_thread_count();
            },
            "runtime::wait");

        LRT_(info) << "runtime_local: exiting wait state";
        return result_;
    }

    ///////////////////////////////////////////////////////////////////////////
    // First half of termination process: stop thread manager,
    // schedule a task managed by timer_pool to initiate second part
    void runtime::stop(bool blocking)
    {
        LRT_(warning) << "runtime_local: about to stop services";

        // execute all on_exit functions whenever the first thread calls this
        this->runtime::stopping();

        // stop runtime_local services (threads)
        thread_manager_->stop(false);    // just initiate shutdown

        if (threads::get_self_ptr())
        {
            // schedule task on separate thread to execute stopped() below
            // this is necessary as this function (stop()) might have been called
            // from a HPX thread, so it would deadlock by waiting for the thread
            // manager
            std::mutex mtx;
            std::condition_variable cond;
            std::unique_lock<std::mutex> l(mtx);

            std::thread t(util::bind(&runtime::stop_helper, this, blocking,
                std::ref(cond), std::ref(mtx)));
            cond.wait(l);

            t.join();
        }
        else
        {
            thread_manager_->stop(blocking);    // wait for thread manager

            // this disables all logging from the main thread
            deinit_tss_helper("main-thread", 0);

            LRT_(info) << "runtime_local: stopped all services";
        }

#ifdef HPX_HAVE_TIMER_POOL
        LTM_(info) << "stop: stopping timer pool";
        timer_pool_.stop();    // stop timer pool as well
        if (blocking)
        {
            timer_pool_.join();
            timer_pool_.clear();
        }
#endif
#ifdef HPX_HAVE_IO_POOL
        io_pool_.stop();    // stops io_pool_ as well
#endif
        //         deinit_tss();
    }

    // Second step in termination: shut down all services.
    // This gets executed as a task in the timer_pool io_service and not as
    // a HPX thread!
    void runtime::stop_helper(
        bool blocking, std::condition_variable& cond, std::mutex& mtx)
    {
        // wait for thread manager to exit
        thread_manager_->stop(blocking);    // wait for thread manager

        // this disables all logging from the main thread
        deinit_tss_helper("main-thread", 0);

        LRT_(info) << "runtime_local: stopped all services";

        std::lock_guard<std::mutex> l(mtx);
        cond.notify_all();    // we're done now
    }

    int runtime::suspend()
    {
        LRT_(info) << "runtime_local: about to suspend runtime";

        if (state_.load() == state_sleeping)
        {
            return 0;
        }

        if (state_.load() != state_running)
        {
            HPX_THROW_EXCEPTION(invalid_status, "runtime::suspend",
                "Can only suspend runtime from running state");
            return -1;
        }

        util::yield_while(
            [this]() {
                return thread_manager_->get_thread_count() >
                    thread_manager_->get_background_thread_count();
            },
            "runtime::suspend");

        thread_manager_->suspend();

#ifdef HPX_HAVE_TIMER_POOL
        timer_pool_.wait();
#endif
#ifdef HPX_HAVE_IO_POOL
        io_pool_.wait();
#endif

        set_state(state_sleeping);

        return 0;
    }

    int runtime::resume()
    {
        std::uint32_t initial_num_localities = get_initial_num_localities();
        if (initial_num_localities > 1)
        {
            HPX_THROW_EXCEPTION(invalid_status, "runtime::resume",
                "Can only suspend runtime when number of localities is 1");
            return -1;
        }

        LRT_(info) << "runtime_local: about to resume runtime";

        if (state_.load() == state_running)
        {
            return 0;
        }

        if (state_.load() != state_sleeping)
        {
            HPX_THROW_EXCEPTION(invalid_status, "runtime::resume",
                "Can only resume runtime from suspended state");
            return -1;
        }

        thread_manager_->resume();

        set_state(state_running);

        return 0;
    }

    ///////////////////////////////////////////////////////////////////////////
    bool runtime::report_error(
        std::size_t num_thread, std::exception_ptr const& e)
    {
        // call thread-specific user-supplied on_error handler
        bool report_exception = true;
        if (on_error_func_)
        {
            report_exception = on_error_func_(num_thread, e);
        }

        // Early and late exceptions, errors outside of HPX-threads
        if (!threads::get_self_ptr() ||
            !threads::threadmanager_is(state_running))
        {
            // report the error to the local console
            if (report_exception)
            {
                detail::report_exception_and_continue(e);
            }

            // store the exception to be able to rethrow it later
            {
                std::lock_guard<std::mutex> l(mtx_);
                exception_ = e;
            }

            stop(false);

            return report_exception;
        }

        return report_exception;
    }

    bool runtime::report_error(std::exception_ptr const& e)
    {
        return report_error(hpx::get_worker_thread_num(), e);
    }

    void runtime::rethrow_exception()
    {
        if (state_.load() > state_running)
        {
            std::lock_guard<std::mutex> l(mtx_);
            if (exception_)
            {
                std::exception_ptr e = exception_;
                exception_ = std::exception_ptr();
                std::rethrow_exception(e);
            }
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    int runtime::run(util::function_nonser<hpx_main_function_type> const& func)
    {
        // start the main thread function
        start(func);

        // now wait for everything to finish
        wait();
        stop();

        rethrow_exception();
        return result_;
    }

    ///////////////////////////////////////////////////////////////////////////
    int runtime::run()
    {
        // start the main thread function
        start();

        // now wait for everything to finish
        int result = wait();
        stop();

        rethrow_exception();
        return result;
    }

    util::thread_mapper& runtime::get_thread_mapper()
    {
        return *thread_support_;
    }

    ///////////////////////////////////////////////////////////////////////////
    void runtime::default_errorsink(std::string const& msg)
    {
        // log the exception information in any case
        LERR_(always) << "default_errorsink: unhandled exception: " << msg;

        std::cerr << msg << std::endl;
    }

    ///////////////////////////////////////////////////////////////////////////
    threads::policies::callback_notifier runtime::get_notification_policy(
        char const* prefix)
    {
        typedef bool (runtime::*report_error_t)(
            std::size_t, std::exception_ptr const&);

        using util::placeholders::_1;
        using util::placeholders::_2;
        using util::placeholders::_3;
        using util::placeholders::_4;

        notification_policy_type notifier;

        notifier.add_on_start_thread_callback(util::bind(
            &runtime::init_tss_helper, This(), prefix, _1, _2, _3, _4, false));
        notifier.add_on_stop_thread_callback(
            util::bind(&runtime::deinit_tss_helper, This(), prefix, _1));
        notifier.set_on_error_callback(
            util::bind(static_cast<report_error_t>(&runtime::report_error),
                This(), _1, _2));

        return notifier;
    }

    void runtime::init_tss_helper(char const* context,
        std::size_t local_thread_num, std::size_t global_thread_num,
        char const* pool_name, char const* postfix, bool service_thread)
    {
        error_code ec(lightweight);
        return init_tss_ex(context, local_thread_num, global_thread_num,
            pool_name, postfix, service_thread, ec);
    }

    void runtime::init_tss_ex(char const* context, std::size_t local_thread_num,
        std::size_t global_thread_num, char const* pool_name,
        char const* postfix, bool service_thread, error_code& ec)
    {
        // initialize our TSS
        this->runtime::init_tss();

        // set the thread's name, if it's not already set
        HPX_ASSERT(detail::thread_name().empty());

        std::string fullname;
        fullname += context;
        if (postfix && *postfix)
            fullname += postfix;
        fullname += "#" + std::to_string(global_thread_num);
        detail::thread_name() = std::move(fullname);

        char const* name = detail::thread_name().c_str();

        // initialize thread mapping for external libraries (i.e. PAPI)
        thread_support_->register_thread(name, ec);

        // register this thread with any possibly active Intel tool
        HPX_ITT_THREAD_SET_NAME(name);

        // set thread name as shown in Visual Studio
        util::set_thread_name(name);

#if defined(HPX_HAVE_APEX)
        if (std::strstr(name, "worker") != nullptr)
            apex::register_thread(name);
#endif

        // call thread-specific user-supplied on_start handler
        if (on_start_func_)
        {
            on_start_func_(
                local_thread_num, global_thread_num, pool_name, context);
        }

        // if this is a service thread, set its service affinity
        if (service_thread)
        {
            // FIXME: We don't set the affinity of the service threads on BG/Q,
            // as this is causing a hang (needs to be investigated)
#if !defined(__bgq__)
            threads::mask_cref_type used_processing_units =
                thread_manager_->get_used_processing_units();

            // --hpx:bind=none  should disable all affinity definitions
            if (threads::any(used_processing_units))
            {
                error_code ec;

                this->topology_.set_thread_affinity_mask(
                    this->topology_.get_service_affinity_mask(
                        used_processing_units),
                    ec);

                // comment this out for now as on CIrcleCI this is causing unending grief
                //                 if (ec)
                //                 {
                //                     HPX_THROW_EXCEPTION(kernel_error
                //                         , "runtime::init_tss_ex"
                //                         , hpx::util::format(
                //                             "failed to set thread affinity mask ("
                //                             HPX_CPU_MASK_PREFIX "{:x}) for service thread: {}",
                //                             used_processing_units, detail::thread_name()));
                //                 }
            }
#endif
        }
    }

    void runtime::deinit_tss_helper(
        char const* context, std::size_t global_thread_num)
    {
        // call thread-specific user-supplied on_stop handler
        if (on_stop_func_)
        {
            on_stop_func_(global_thread_num, global_thread_num, "", context);
        }

        // reset our TSS
        deinit_tss();

        // reset PAPI support
        thread_support_->unregister_thread();

        // reset thread local storage
        detail::thread_name().clear();
    }

    void runtime::add_pre_startup_function(startup_function_type f)
    {
        if (!f.empty())
        {
            std::lock_guard<std::mutex> l(mtx_);
            pre_startup_functions_.push_back(std::move(f));
        }
    }

    void runtime::add_startup_function(startup_function_type f)
    {
        if (!f.empty())
        {
            std::lock_guard<std::mutex> l(mtx_);
            startup_functions_.push_back(std::move(f));
        }
    }

    void runtime::add_pre_shutdown_function(shutdown_function_type f)
    {
        if (!f.empty())
        {
            std::lock_guard<std::mutex> l(mtx_);
            pre_shutdown_functions_.push_back(std::move(f));
        }
    }

    void runtime::add_shutdown_function(shutdown_function_type f)
    {
        if (!f.empty())
        {
            std::lock_guard<std::mutex> l(mtx_);
            shutdown_functions_.push_back(std::move(f));
        }
    }

    hpx::util::io_service_pool* runtime::get_thread_pool(char const* name)
    {
        HPX_ASSERT(name != nullptr);
#ifdef HPX_HAVE_IO_POOL
        if (0 == std::strncmp(name, "io", 2))
            return &io_pool_;
#endif
#ifdef HPX_HAVE_TIMER_POOL
        if (0 == std::strncmp(name, "timer", 5))
            return &timer_pool_;
#endif
        if (0 == std::strncmp(name, "main", 4))    //-V112
            return &main_pool_;

        HPX_THROW_EXCEPTION(bad_parameter, "runtime::get_thread_pool",
            std::string("unknown thread pool requested: ") + name);
        return nullptr;
    }

    /// Register an external OS-thread with HPX
    bool runtime::register_thread(char const* name,
        std::size_t global_thread_num, bool service_thread, error_code& ec)
    {
        if (nullptr != get_runtime_ptr())
            return false;    // already registered

        std::string thread_name(name);
        thread_name += "-thread";

        init_tss_ex(thread_name.c_str(), global_thread_num, global_thread_num,
            "", nullptr, service_thread, ec);

        return !ec ? true : false;
    }

    /// Unregister an external OS-thread with HPX
    bool runtime::unregister_thread()
    {
        if (nullptr != get_runtime_ptr())
            return false;    // never registered

        deinit_tss_helper(
            detail::thread_name().c_str(), hpx::get_worker_thread_num());
        return true;
    }

    ///////////////////////////////////////////////////////////////////////////
    threads::policies::callback_notifier get_notification_policy(
        char const* prefix)
    {
        return get_runtime().get_notification_policy(prefix);
    }

    std::uint32_t get_locality_id(error_code& ec)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr == rt || rt->get_state() == state_invalid)
        {
            return naming::invalid_locality_id;
        }

        return rt->get_locality_id(ec);
    }

    std::size_t get_num_worker_threads()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr == rt)
        {
            HPX_THROW_EXCEPTION(invalid_status, "hpx::get_num_worker_threads",
                "the runtime system has not been initialized yet");
            return std::size_t(0);
        }

        return rt->get_num_worker_threads();
    }

    /// \brief Return the number of localities which are currently registered
    ///        for the running application.
    std::uint32_t get_num_localities(hpx::launch::sync_policy, error_code& ec)
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr == rt)
        {
            HPX_THROW_EXCEPTION(invalid_status, "hpx::get_num_localities",
                "the runtime system has not been initialized yet");
            return std::size_t(0);
        }

        return rt->get_num_localities(hpx::launch::sync, ec);
    }

    std::uint32_t get_initial_num_localities()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr == rt)
        {
            HPX_THROW_EXCEPTION(invalid_status,
                "hpx::get_initial_num_localities",
                "the runtime system has not been initialized yet");
            return std::size_t(0);
        }

        return rt->get_initial_num_localities();
    }

    lcos::future<std::uint32_t> get_num_localities()
    {
        runtime* rt = get_runtime_ptr();
        if (nullptr == rt)
        {
            HPX_THROW_EXCEPTION(invalid_status, "hpx::get_num_localities",
                "the runtime system has not been initialized yet");
            return make_ready_future(std::uint32_t(0));
        }

        return rt->get_num_localities();
    }
}    // namespace hpx
