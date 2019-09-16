//  Copyright (c) 2007-2018 Hartmut Kaiser
//  Copyright (c)      2011 Bryce Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_RUNTIME_RUNTIME_JUN_10_2008_1012AM)
#define HPX_RUNTIME_RUNTIME_JUN_10_2008_1012AM

#include <hpx/config.hpp>
#include <hpx/runtime.hpp>
#include <hpx/runtime/runtime_mode.hpp>
#include <hpx/runtime/shutdown_function.hpp>
#include <hpx/runtime/startup_function.hpp>
#include <hpx/runtime/thread_hooks.hpp>
#include <hpx/runtime/threads/policies/callback_notifier.hpp>
#include <hpx/runtime/threads/threadmanager.hpp>
#include <hpx/runtime_fwd.hpp>
#include <hpx/state.hpp>
#include <hpx/topology.hpp>
#include <hpx/util/io_service_pool.hpp>
#include <hpx/util/runtime_configuration.hpp>
#include <hpx/util/thread_mapper.hpp>
#include <hpx/util_fwd.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <hpx/config/warnings_prefix.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace hpx {
    namespace detail {
        ///////////////////////////////////////////////////////////////////////
        // There is no need to protect these global from thread concurrent
        // access as they are access during early startup only.
        extern std::list<startup_function_type> global_pre_startup_functions;
        extern std::list<startup_function_type> global_startup_functions;
        extern std::list<shutdown_function_type> global_pre_shutdown_functions;
        extern std::list<shutdown_function_type> global_shutdown_functions;
    }    // namespace detail

    ///////////////////////////////////////////////////////////////////////////
    class HPX_EXPORT runtime
    {
    public:
        state get_state() const
        {
            return state_.load();
        }

        /// The \a hpx_main_function_type is the default function type usable
        /// as the main HPX thread function.
        typedef int hpx_main_function_type();

        ///
        typedef void hpx_errorsink_function_type(
            std::uint32_t, std::string const&);

        /// Construct a new HPX runtime instance
        ///
        /// TODO: This is not correct...
        ///  \param locality_mode  [in] This is the mode the given runtime
        ///                       instance should be executed in.
        explicit runtime(util::runtime_configuration& rtcfg);

        /// \brief The destructor makes sure all HPX runtime services are
        ///        properly shut down before exiting.
        virtual ~runtime();

        /// \brief Manage list of functions to call on exit
        void on_exit(util::function_nonser<void()> const& f)
        {
            std::lock_guard<std::mutex> l(mtx_);
            on_exit_functions_.push_back(f);
        }

        /// \brief Manage runtime 'stopped' state
        void starting()
        {
            state_.store(state_pre_main);
        }

        /// \brief Call all registered on_exit functions
        void stopping()
        {
            state_.store(state_stopped);

            typedef util::function_nonser<void()> value_type;

            std::lock_guard<std::mutex> l(mtx_);
            for (value_type const& f : on_exit_functions_)
                f();
        }

        /// This accessor returns whether the runtime instance has been stopped
        bool stopped() const
        {
            return state_.load() == state_stopped;
        }

        ///  \brief Stop the runtime system, wait for termination
        ///
        /// \param blocking   [in] This allows to control whether this
        ///                   call blocks until the runtime system has been
        ///                   fully stopped. If this parameter is \a false then
        ///                   this call will initiate the stop action but will
        ///                   return immediately. Use a second call to stop
        ///                   with this parameter set to \a true to wait for
        ///                   all internal work to be completed.
        // TODO: Rename
        void stopped_2(
            bool blocking, std::condition_variable& cond, std::mutex& mtx);

        /// \brief access configuration information
        util::runtime_configuration& get_config()
        {
            return ini_;
        }
        util::runtime_configuration const& get_config() const
        {
            return ini_;
        }

        std::size_t get_instance_number() const
        {
            return static_cast<std::size_t>(instance_number_);
        }

        /// \brief Return the system uptime measure on the thread executing this call
        static std::uint64_t get_system_uptime();

        /// \brief Return a reference to the internal PAPI thread manager
        util::thread_mapper& get_thread_mapper();

        threads::topology const& get_topology() const
        {
            return topology_;
        }

        std::uint32_t assign_cores(
            std::string const& locality_basename, std::uint32_t num_threads);

        std::uint32_t assign_cores();

        /// \brief Run the HPX runtime system, use the given function for the
        ///        main \a thread and block waiting for all threads to
        ///        finish
        ///
        /// \param func       [in] This is the main function of an HPX
        ///                   application. It will be scheduled for execution
        ///                   by the thread manager as soon as the runtime has
        ///                   been initialized. This function is expected to
        ///                   expose an interface as defined by the typedef
        ///                   \a hpx_main_function_type. This parameter is
        ///                   optional and defaults to none main thread
        ///                   function, in which case all threads have to be
        ///                   scheduled explicitly.
        ///
        /// \note             The parameter \a func is optional. If no function
        ///                   is supplied, the runtime system will simply wait
        ///                   for the shutdown action without explicitly
        ///                   executing any main thread.
        ///
        /// \returns          This function will return the value as returned
        ///                   as the result of the invocation of the function
        ///                   object given by the parameter \p func.
        virtual int run(
            util::function_nonser<hpx_main_function_type> const& func);

        /// \brief Run the HPX runtime system, initially use the given number
        ///        of (OS) threads in the thread-manager and block waiting for
        ///        all threads to finish.
        ///
        /// \returns          This function will always return 0 (zero).
        virtual int run();

        /// Rethrow any stored exception (to be called after stop())
        virtual void rethrow_exception();

        /// \brief Start the runtime system
        ///
        /// \param func       [in] This is the main function of an HPX
        ///                   application. It will be scheduled for execution
        ///                   by the thread manager as soon as the runtime has
        ///                   been initialized. This function is expected to
        ///                   expose an interface as defined by the typedef
        ///                   \a hpx_main_function_type.
        /// \param blocking   [in] This allows to control whether this
        ///                   call blocks until the runtime system has been
        ///                   stopped. If this parameter is \a true the
        ///                   function \a runtime#start will call
        ///                   \a runtime#wait internally.
        ///
        /// \returns          If a blocking is a true, this function will
        ///                   return the value as returned as the result of the
        ///                   invocation of the function object given by the
        ///                   parameter \p func. Otherwise it will return zero.
        virtual int start(
            util::function_nonser<hpx_main_function_type> const& func,
            bool blocking = false);

        /// \brief Start the runtime system
        ///
        /// \param blocking   [in] This allows to control whether this
        ///                   call blocks until the runtime system has been
        ///                   stopped. If this parameter is \a true the
        ///                   function \a runtime#start will call
        ///                   \a runtime#wait internally .
        ///
        /// \returns          If a blocking is a true, this function will
        ///                   return the value as returned as the result of the
        ///                   invocation of the function object given by the
        ///                   parameter \p func. Otherwise it will return zero.
        virtual int start(bool blocking = false);

        /// \brief Wait for the shutdown action to be executed
        ///
        /// \returns          This function will return the value as returned
        ///                   as the result of the invocation of the function
        ///                   object given by the parameter \p func.
        virtual int wait();

        /// \brief Initiate termination of the runtime system
        ///
        /// \param blocking   [in] This allows to control whether this
        ///                   call blocks until the runtime system has been
        ///                   fully stopped. If this parameter is \a false then
        ///                   this call will initiate the stop action but will
        ///                   return immediately. Use a second call to stop
        ///                   with this parameter set to \a true to wait for
        ///                   all internal work to be completed.
        virtual void stop(bool blocking = true);

        /// \brief Suspend the runtime system
        virtual int suspend();

        ///    \brief Resume the runtime system
        virtual int resume();

        virtual int finalize(double shutdown_timeout)
        {
            return 0;
        }

        ///  \brief Return true if networking is enabled.
        virtual bool is_networking_enabled()
        {
            return false;
        }

        /// \brief Allow access to the thread manager instance used by the HPX
        ///        runtime.
        virtual hpx::threads::threadmanager& get_thread_manager()
        {
            return *thread_manager_;
        }

        /// \brief Returns a string of the locality endpoints (usable in debug output)
        virtual std::string here() const
        {
            return "127.0.0.1";
        }

        /// \brief Report a non-recoverable error to the runtime system
        ///
        /// \param num_thread [in] The number of the operating system thread
        ///                   the error has been detected in.
        /// \param e          [in] This is an instance encapsulating an
        ///                   exception which lead to this function call.
        virtual bool report_error(
            std::size_t num_thread, std::exception_ptr const& e);

        /// \brief Report a non-recoverable error to the runtime system
        ///
        /// \param e          [in] This is an instance encapsulating an
        ///                   exception which lead to this function call.
        ///
        /// \note This function will retrieve the number of the current
        ///       shepherd thread and forward to the report_error function
        ///       above.
        virtual bool report_error(std::exception_ptr const& e);

        /// Add a function to be executed inside a HPX thread before hpx_main
        /// but guaranteed to be executed before any startup function registered
        /// with \a add_startup_function.
        ///
        /// \param  f   The function 'f' will be called from inside a HPX
        ///             thread before hpx_main is executed. This is very useful
        ///             to setup the runtime environment of the application
        ///             (install performance counters, etc.)
        ///
        /// \note       The difference to a startup function is that all
        ///             pre-startup functions will be (system-wide) executed
        ///             before any startup function.
        virtual void add_pre_startup_function(startup_function_type f);

        /// Add a function to be executed inside a HPX thread before hpx_main
        ///
        /// \param  f   The function 'f' will be called from inside a HPX
        ///             thread before hpx_main is executed. This is very useful
        ///             to setup the runtime environment of the application
        ///             (install performance counters, etc.)
        virtual void add_startup_function(startup_function_type f);

        /// Add a function to be executed inside a HPX thread during
        /// hpx::finalize, but guaranteed before any of teh shutdown functions
        /// is executed.
        ///
        /// \param  f   The function 'f' will be called from inside a HPX
        ///             thread while hpx::finalize is executed. This is very
        ///             useful to tear down the runtime environment of the
        ///             application (uninstall performance counters, etc.)
        ///
        /// \note       The difference to a shutdown function is that all
        ///             pre-shutdown functions will be (system-wide) executed
        ///             before any shutdown function.
        virtual void add_pre_shutdown_function(shutdown_function_type f);

        /// Add a function to be executed inside a HPX thread during hpx::finalize
        ///
        /// \param  f   The function 'f' will be called from inside a HPX
        ///             thread while hpx::finalize is executed. This is very
        ///             useful to tear down the runtime environment of the
        ///             application (uninstall performance counters, etc.)
        virtual void add_shutdown_function(shutdown_function_type f);

        /// Access one of the internal thread pools (io_service instances)
        /// HPX is using to perform specific tasks. The three possible values
        /// for the argument \p name are "main_pool", "io_pool", "parcel_pool",
        /// and "timer_pool". For any other argument value the function will
        /// return zero.
        virtual hpx::util::io_service_pool* get_thread_pool(char const* name);

        /// \brief Register an external OS-thread with HPX
        ///
        /// This function should be called from any OS-thread which is external to
        /// HPX (not created by HPX), but which needs to access HPX functionality,
        /// such as setting a value on a promise or similar.
        ///
        /// \param name             [in] The name to use for thread registration.
        /// \param num              [in] The sequence number to use for thread
        ///                         registration. The default for this parameter
        ///                         is zero.
        /// \param service_thread   [in] The thread should be registered as a
        ///                         service thread. The default for this parameter
        ///                         is 'true'. Any service threads will be pinned
        ///                         to cores not currently used by any of the HPX
        ///                         worker threads.
        ///
        /// \note The function will compose a thread name of the form
        ///       '<name>-thread#<num>' which is used to register the thread. It
        ///       is the user's responsibility to ensure that each (composed)
        ///       thread name is unique. HPX internally uses the following names
        ///       for the threads it creates, do not reuse those:
        ///
        ///         'main', 'io', 'timer', 'parcel', 'worker'
        ///
        /// \note This function should be called for each thread exactly once. It
        ///       will fail if it is called more than once.
        ///
        /// \returns This function will return whether th erequested operation
        ///          succeeded or not.
        ///
        virtual bool register_thread(char const* name, std::size_t num = 0,
            bool service_thread = true, error_code& ec = throws);

        /// \brief Unregister an external OS-thread with HPX
        ///
        /// This function will unregister any external OS-thread from HPX.
        ///
        /// \note This function should be called for each thread exactly once. It
        ///       will fail if it is called more than once. It will fail as well
        ///       if the thread has not been registered before (see
        ///       \a register_thread).
        ///
        /// \returns This function will return whether th erequested operation
        ///          succeeded or not.
        ///
        virtual bool unregister_thread();

        /// Generate a new notification policy instance for the given thread
        /// name prefix
        typedef threads::policies::callback_notifier notification_policy_type;
        virtual notification_policy_type get_notification_policy(
            char const* prefix);

        notification_policy_type::on_startstop_type on_start_func() const;
        notification_policy_type::on_startstop_type on_stop_func() const;
        notification_policy_type::on_error_type on_error_func() const;

        notification_policy_type::on_startstop_type on_start_func(
            notification_policy_type::on_startstop_type&&);
        notification_policy_type::on_startstop_type on_stop_func(
            notification_policy_type::on_startstop_type&&);
        notification_policy_type::on_error_type on_error_func(
            notification_policy_type::on_error_type&&);

    protected:
        void init_tss();
        void deinit_tss();

    private:
        // TODO: Rename
        void deinit_tss_2(char const* context, std::size_t num);

        void init_tss_ex(char const* context, std::size_t local_thread_num,
            std::size_t global_thread_num, char const* pool_name,
            char const* postfix, bool service_thread, error_code& ec);

        // TODO: Rename
        void init_tss_2(char const* context, std::size_t local_thread_num,
            std::size_t global_thread_num, char const* pool_name,
            char const* postfix, bool service_thread);

    public:
        void set_state(state s);

    private:
        // avoid warnings about usage of this in member initializer list
        runtime* This()
        {
            return this;
        }

        //
    protected:
        static void default_errorsink(std::string const&);

        //
    protected:
        threads::thread_result_type run_helper(
            util::function_nonser<runtime::hpx_main_function_type> const& func,
            int& result);

        void wait_helper(
            std::mutex& mtx, std::condition_variable& cond, bool& running);

    protected:
        // list of functions to call on exit
        typedef std::vector<util::function_nonser<void()>> on_exit_type;
        on_exit_type on_exit_functions_;
        mutable std::mutex mtx_;

        util::runtime_configuration ini_;

        long instance_number_;
        static std::atomic<int> instance_number_counter_;

        // certain components (such as PAPI) require all threads to be
        // registered with the library
        std::unique_ptr<util::thread_mapper> thread_support_;

        // topology and affinity data
        threads::topology& topology_;

        // locality basename -> used cores
        typedef std::map<std::string, std::uint32_t> used_cores_map_type;
        used_cores_map_type used_cores_map_;

        std::atomic<state> state_;

        // support tieing in external functions to be called for thread events
        notification_policy_type::on_startstop_type on_start_func_;
        notification_policy_type::on_startstop_type on_stop_func_;
        notification_policy_type::on_error_type on_error_func_;

    private:
        int result_;
        notification_policy_type main_pool_notifier_;
        util::io_service_pool main_pool_;
#ifdef HPX_HAVE_IO_POOL
        notification_policy_type io_pool_notifier_;
        util::io_service_pool io_pool_;
#endif
#ifdef HPX_HAVE_TIMER_POOL
        notification_policy_type timer_pool_notifier_;
        util::io_service_pool timer_pool_;
#endif
        notification_policy_type notifier_;
        std::unique_ptr<hpx::threads::threadmanager> thread_manager_;

        std::exception_ptr exception_;

        std::list<startup_function_type> pre_startup_functions_;
        std::list<startup_function_type> startup_functions_;
        std::list<shutdown_function_type> pre_shutdown_functions_;
        std::list<shutdown_function_type> shutdown_functions_;
    };
}    // namespace hpx

#include <hpx/config/warnings_suffix.hpp>

#endif
