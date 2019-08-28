//  Copyright (c) 2007-2017 Hartmut Kaiser
//  Copyright (c)      2011 Bryce Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/config.hpp>
#include <hpx/performance_counters/counters.hpp>

#include <hpx/assertion.hpp>
#include <hpx/concurrency/thread_name.hpp>
#include <hpx/custom_exception_info.hpp>
#include <hpx/errors.hpp>
#include <hpx/functional.hpp>
#include <hpx/lcos/barrier.hpp>
#include <hpx/lcos/latch.hpp>
#include <hpx/logging.hpp>
#include <hpx/runtime/agas/big_boot_barrier.hpp>
#include <hpx/runtime/agas/interface.hpp>
#include <hpx/runtime/agas/addressing_service.hpp>
#include <hpx/runtime/applier/applier.hpp>
#include <hpx/runtime/components/console_error_sink.hpp>
#include <hpx/runtime/components/runtime_support.hpp>
#include <hpx/runtime/components/server/console_error_sink.hpp>
#include <hpx/runtime/components/server/memory.hpp>
#include <hpx/runtime/components/server/runtime_support.hpp>
#include <hpx/runtime/components/server/simple_component_base.hpp>    // EXPORTS get_next_id
#include <hpx/runtime/parcelset/parcelhandler.hpp>
#include <hpx/runtime/config_entry.hpp>
#include <hpx/runtime/parcelset_fwd.hpp>
#include <hpx/runtime/shutdown_function.hpp>
#include <hpx/runtime/startup_function.hpp>
#include <hpx/runtime/threads/coroutines/detail/context_impl.hpp>
#include <hpx/runtime/threads/scoped_background_timer.hpp>
#include <hpx/runtime/threads/threadmanager.hpp>
#include <hpx/runtime_impl_distributed.hpp>
#include <hpx/runtime_impl_local.hpp>
#include <hpx/state.hpp>
#include <hpx/thread_support/set_thread_name.hpp>
#include <hpx/util/apex.hpp>
#include <hpx/util/safe_lexical_cast.hpp>
#include <hpx/util/thread_mapper.hpp>
#include <hpx/util/yield_while.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN64) && defined(_DEBUG) && !defined(HPX_HAVE_FIBER_BASED_COROUTINES)
#include <io.h>
#endif

namespace hpx
{
    namespace detail
    {
        naming::gid_type get_next_id(std::size_t count)
        {
            if (nullptr == get_runtime_ptr())
                return naming::invalid_gid;

            return get_runtime_distributed().get_next_id(count);
        }

        ///////////////////////////////////////////////////////////////////////////
        void dijkstra_make_black()
        {
            get_runtime_support_ptr()->dijkstra_make_black();
        }

#if defined(HPX_HAVE_BACKGROUND_THREAD_COUNTERS) &&                            \
    defined(HPX_HAVE_THREAD_IDLE_RATES)
        bool network_background_callback(std::size_t num_thread,
            std::int64_t& background_work_exec_time_send,
            std::int64_t& background_work_exec_time_receive)
        {
            bool result = false;
            // count background work duration
            {
                threads::background_work_duration_counter bg_send_duration(
                    background_work_exec_time_send);
                threads::background_exec_time_wrapper bg_exec_time(
                    bg_send_duration);

                if (hpx::parcelset::do_background_work(
                        num_thread, parcelset::parcelport_background_mode_send))
                {
                    result = true;
                }
            }

            {
                threads::background_work_duration_counter bg_receive_duration(
                    background_work_exec_time_receive);
                threads::background_exec_time_wrapper bg_exec_time(
                    bg_receive_duration);

                if (hpx::parcelset::do_background_work(num_thread,
                        parcelset::parcelport_background_mode_receive))
                {
                    result = true;
                }
            }

            if (0 == num_thread)
                hpx::agas::garbage_collect_non_blocking();
            return result;
        }
#else
        bool network_background_callback(std::size_t num_thread)
        {
            bool result = false;

            if (hpx::parcelset::do_background_work(
                    num_thread, parcelset::parcelport_background_mode_all))
            {
                result = true;
            }

            if (0 == num_thread)
                hpx::agas::garbage_collect_non_blocking();
            return result;
        }
#endif
    }

    ///////////////////////////////////////////////////////////////////////////
    components::server::runtime_support* get_runtime_support_ptr()
    {
        // TODO
        return reinterpret_cast<components::server::runtime_support*>(
            get_runtime_distributed().get_runtime_support_lva());
    }

    ///////////////////////////////////////////////////////////////////////////
    runtime_impl_distributed::runtime_impl_distributed(
        util::runtime_configuration& rtcfg)
      : runtime_impl_local(rtcfg)
      , mode_(rtcfg.mode_)
      , result_(0)
      , main_pool_notifier_()
      , main_pool_(1, main_pool_notifier_, "main_pool")
#ifdef HPX_HAVE_IO_POOL
      , io_pool_notifier_(
            runtime_impl_distributed::get_notification_policy("io-thread"))
      , io_pool_(
            rtcfg.get_thread_pool_size("io_pool"), io_pool_notifier_, "io_pool")
#endif
#ifdef HPX_HAVE_TIMER_POOL
      , timer_pool_notifier_(
            runtime_impl_distributed::get_notification_policy("timer-thread"))
      , timer_pool_(rtcfg.get_thread_pool_size("timer_pool"),
            timer_pool_notifier_, "timer_pool")
#endif
      , notifier_(
            runtime_impl_distributed::get_notification_policy("worker-thread"))
      , thread_manager_(new hpx::threads::threadmanager(
#ifdef HPX_HAVE_TIMER_POOL
            timer_pool_,
#endif
            notifier_
#ifdef HPX_HAVE_NETWORKING
            ,
            &detail::network_background_callback
#endif
            ))
      , parcel_handler_notifier_(
            runtime_impl_distributed::get_notification_policy("parcel-thread"))
      , parcel_handler_(rtcfg, thread_manager_.get(), parcel_handler_notifier_)
      , agas_client_(ini_, rtcfg.mode_)
      , applier_(parcel_handler_, *thread_manager_)
      , memory_(new components::server::memory)
      , runtime_support_(new components::server::runtime_support(ini_))
    {
        LPROGRESS_;

        runtime_distributed*& runtime_distributed_ = get_runtime_distributed_ptr();
        if (nullptr == runtime_distributed_)
        {
            HPX_ASSERT(nullptr == threads::thread_self::get_self());

            runtime_distributed_ = this;
        }

        agas_client_.bootstrap(parcel_handler_, ini_);

        components::server::get_error_dispatcher().
            set_error_sink(&runtime_impl_distributed::default_errorsink);

        // now create all threadmanager pools
        thread_manager_->create_pools();

        // this initializes the used_processing_units_ mask
        thread_manager_->init();

        // now, launch AGAS and register all nodes, launch all other components
        agas_client_.initialize(
            parcel_handler_, std::uint64_t(runtime_support_.get()),
            std::uint64_t(memory_.get()));

        parcel_handler_.initialize(agas_client_, &applier_);

        applier_.initialize(std::uint64_t(runtime_support_.get()),
            std::uint64_t(memory_.get()));

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

    ///////////////////////////////////////////////////////////////////////////
    runtime_impl_distributed::~runtime_impl_distributed()
    {
        LRT_(debug) << "~runtime_impl_distributed(entering)";

        runtime_support_->delete_function_lists();

        // stop all services
        parcel_handler_.stop();     // stops parcel pools as well
        thread_manager_->stop();    // stops timer_pool_ as well
#ifdef HPX_HAVE_IO_POOL
        io_pool_.stop();
#endif
        // unload libraries
        runtime_support_->tidy();

        LRT_(debug) << "~runtime_impl_distributed(finished)";

        LPROGRESS_;
    }

    int pre_main(hpx::runtime_mode);

    threads::thread_result_type
    runtime_impl_distributed::run_helper(
        util::function_nonser<runtime::hpx_main_function_type> const& func,
        int& result)
    {
        lbt_ << "(2nd stage) runtime_impl_distributed::run_helper: launching pre_main";

        // Change our thread description, as we're about to call pre_main
        threads::set_thread_description(threads::get_self_id(), "pre_main");

        // Finish the bootstrap
        result = hpx::pre_main(mode_);
        if (result) {
            lbt_ << "runtime_impl_distributed::run_helper: bootstrap "
                    "aborted, bailing out";
            return threads::thread_result_type(threads::terminated,
                threads::invalid_thread_id);
        }

        lbt_ << "(4th stage) runtime_impl_distributed::run_helper: bootstrap complete";
        set_state(state_running);

        parcel_handler_.enable_alternative_parcelports();

        // reset all counters right before running main, if requested
        if (get_config_entry("hpx.print_counter.startup", "0") == "1")
        {
            bool reset = false;
            if (get_config_entry("hpx.print_counter.reset", "0") == "1")
                reset = true;

            error_code ec(lightweight);     // ignore errors
            evaluate_active_counters(reset, "startup", ec);
        }

        // Connect back to given latch if specified
        std::string connect_back_to(
            get_config_entry("hpx.on_startup.wait_on_latch", ""));
        if (!connect_back_to.empty())
        {
            lbt_ << "(5th stage) runtime_impl_distributed::run_helper: about to "
                    "synchronize with latch: "
                 << connect_back_to;

            // inform launching process that this locality is up and running
            hpx::lcos::latch l;
            l.connect_to(connect_back_to);
            l.count_down_and_wait();

            lbt_ << "(5th stage) runtime_impl_distributed::run_helper: "
                    "synchronized with latch: "
                 << connect_back_to;
        }

        // Now, execute the user supplied thread function (hpx_main)
        if (!!func)
        {
            lbt_ << "(last stage) runtime_impl_distributed::run_helper: about to "
                    "invoke hpx_main";

            // Change our thread description, as we're about to call hpx_main
            threads::set_thread_description(threads::get_self_id(), "hpx_main");

            // Call hpx_main
            result = func();
        }
        return threads::thread_result_type(threads::terminated,
            threads::invalid_thread_id);
    }

    int runtime_impl_distributed::start(
        util::function_nonser<hpx_main_function_type> const& func, bool blocking)
    {
#if defined(_WIN64) && defined(_DEBUG) && !defined(HPX_HAVE_FIBER_BASED_COROUTINES)
        // needs to be called to avoid problems at system startup
        // see: http://connect.microsoft.com/VisualStudio/feedback/ViewFeedback.aspx?FeedbackID=100319
        _isatty(0);
#endif
        // {{{ early startup code - local

        // initialize instrumentation system
        util::apex_init();

        LRT_(info) << "cmd_line: " << get_config().get_cmd_line();

        lbt_ << "(1st stage) runtime_impl_distributed::start: booting locality " << here();

        // start runtime_support services
        runtime_support_->run();
        lbt_ << "(1st stage) runtime_impl_distributed::start: started "
                      "runtime_support component";

#ifdef HPX_HAVE_IO_POOL
        // start the io pool
        io_pool_.run(false);
        lbt_ << "(1st stage) runtime_impl_distributed::start: started the application "
                      "I/O service pool";
#endif
        // start the thread manager
        thread_manager_->run();
        lbt_ << "(1st stage) runtime_impl_distributed::start: started threadmanager";
        // }}}

        // invoke the AGAS v2 notifications
        agas::get_big_boot_barrier().trigger();

        // {{{ launch main
        // register the given main function with the thread manager
        lbt_ << "(1st stage) runtime_impl_distributed::start: launching run_helper "
                      "HPX thread";

        threads::thread_init_data data(
            util::bind(&runtime_impl_distributed::run_helper, this, func,
                std::ref(result_)),
            "run_helper",
            threads::thread_priority_normal,
            threads::thread_schedule_hint(0),
            threads::get_stack_size(threads::thread_stacksize_large));

        this->runtime::starting();
        threads::thread_id_type id = threads:: invalid_thread_id;
        thread_manager_->register_thread(data, id);

        // }}}

        // block if required
        if (blocking)
            return wait();     // wait for the shutdown_action to be executed

        // Register this thread with the runtime system to allow calling certain
        // HPX functionality from the main thread.
        init_tss("main-thread", 0, 0, "", "", false);

        return 0;   // return zero as we don't know the outcome of hpx_main yet
    }

    int runtime_impl_distributed::start(bool blocking)
    {
        util::function_nonser<hpx_main_function_type> empty_main;
        return start(empty_main, blocking);
    }

    ///////////////////////////////////////////////////////////////////////////
    void runtime_impl_distributed::wait_helper(
        std::mutex& mtx, std::condition_variable& cond, bool& running)
    {
        // signal successful initialization
        {
            std::lock_guard<std::mutex> lk(mtx);
            running = true;
            cond.notify_all();
        }

        // prefix thread name with locality number, if needed
        std::string locality = ""; //locality_prefix(get_config());

        // register this thread with any possibly active Intel tool
        std::string thread_name (locality + "main-thread#wait_helper");
        HPX_ITT_THREAD_SET_NAME(thread_name.c_str());

        // set thread name as shown in Visual Studio
        util::set_thread_name(thread_name.c_str());

#if defined(HPX_HAVE_APEX)
        // not registering helper threads - for now
        //apex::register_thread(thread_name.c_str());
#endif

        // wait for termination
        runtime_support_->wait();

        // stop main thread pool
        main_pool_.stop();
    }

    int runtime_impl_distributed::wait()
    {
        LRT_(info) << "runtime_impl_distributed: about to enter wait state";

        // start the wait_helper in a separate thread
        std::mutex mtx;
        std::condition_variable cond;
        bool running = false;

        std::thread t (util::bind(
                &runtime_impl_distributed::wait_helper,
                this, std::ref(mtx), std::ref(cond), std::ref(running)
            ));

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

        LRT_(info) << "runtime_impl_distributed: exiting wait state";
        return result_;
    }

    ///////////////////////////////////////////////////////////////////////////
    // First half of termination process: stop thread manager,
    // schedule a task managed by timer_pool to initiate second part
    void runtime_impl_distributed::stop(bool blocking)
    {
        LRT_(warning) << "runtime_impl_distributed: about to stop services";

        // flush all parcel buffers, stop buffering parcels at this point
        //parcel_handler_.do_background_work(true, parcelport_background_mode_all);

        // execute all on_exit functions whenever the first thread calls this
        this->runtime::stopping();

        // stop runtime_impl_distributed services (threads)
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

            std::thread t(util::bind(&runtime_impl_distributed::stopped, this, blocking,
                std::ref(cond), std::ref(mtx)));
            cond.wait(l);

            t.join();
        }
        else
        {
            runtime_support_->stopped();         // re-activate shutdown HPX-thread
            thread_manager_->stop(blocking);     // wait for thread manager

            // this disables all logging from the main thread
            deinit_tss("main-thread", 0);

            LRT_(info) << "runtime_impl_distributed: stopped all services";
        }

        // stop the rest of the system
        parcel_handler_.stop(blocking);     // stops parcel pools as well
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
        io_pool_.stop();                    // stops io_pool_ as well
#endif
//         deinit_tss();
    }

    // Second step in termination: shut down all services.
    // This gets executed as a task in the timer_pool io_service and not as
    // a HPX thread!
    void runtime_impl_distributed::stopped(
        bool blocking, std::condition_variable& cond, std::mutex& mtx)
    {
        // wait for thread manager to exit
        runtime_support_->stopped();         // re-activate shutdown HPX-thread
        thread_manager_->stop(blocking);     // wait for thread manager

        // this disables all logging from the main thread
        deinit_tss("main-thread", 0);

        LRT_(info) << "runtime_impl_distributed: stopped all services";

        std::lock_guard<std::mutex> l(mtx);
        cond.notify_all();                  // we're done now
    }

    int runtime_impl_distributed::suspend()
    {
        std::uint32_t initial_num_localities = get_initial_num_localities();
        if (initial_num_localities > 1)
        {
            HPX_THROW_EXCEPTION(invalid_status,
                "runtime_impl_distributed::suspend",
                "Can only suspend runtime when number of localities is 1");
            return -1;
        }

        LRT_(info) << "runtime_impl_distributed: about to suspend runtime";

        if (state_.load() == state_sleeping)
        {
            return 0;
        }

        if (state_.load() != state_running)
        {
            HPX_THROW_EXCEPTION(invalid_status,
                "runtime_impl_distributed::suspend",
                "Can only suspend runtime from running state");
            return -1;
        }

        util::yield_while(
            [this]()
            {
                return thread_manager_->get_thread_count() >
                    thread_manager_->get_background_thread_count();
            }, "runtime_impl_distributed::suspend");

        thread_manager_->suspend();

        // Ignore parcel pools because suspension can only be done with one
        // locality
#ifdef HPX_HAVE_TIMER_POOL
        timer_pool_.wait();
#endif
#ifdef HPX_HAVE_IO_POOL
        io_pool_.wait();
#endif

        set_state(state_sleeping);

        return 0;
    }

    int runtime_impl_distributed::resume()
    {
        std::uint32_t initial_num_localities = get_initial_num_localities();
        if (initial_num_localities > 1)
        {
            HPX_THROW_EXCEPTION(invalid_status,
                "runtime_impl_distributed::resume",
                "Can only suspend runtime when number of localities is 1");
            return -1;
        }

        LRT_(info) << "runtime_impl_distributed: about to resume runtime";

        if (state_.load() == state_running)
        {
            return 0;
        }

        if (state_.load() != state_sleeping)
        {
            HPX_THROW_EXCEPTION(invalid_status,
                "runtime_impl_distributed::resume",
                "Can only resume runtime from suspended state");
            return -1;
        }

        thread_manager_->resume();

        set_state(state_running);

        return 0;
    }

    ///////////////////////////////////////////////////////////////////////////
    bool runtime_impl_distributed::report_error(
        std::size_t num_thread, std::exception_ptr const& e)
    {
        // call thread-specific user-supplied on_error handler
        bool report_exception = true;
        if (on_error_func_)
        {
            report_exception = on_error_func_(num_thread, e);
        }

        // Early and late exceptions, errors outside of HPX-threads
        if (!threads::get_self_ptr() || !threads::threadmanager_is(state_running))
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

            lcos::barrier::get_global_barrier().detach();

            // initiate stopping the runtime system
            runtime_support_->notify_waiting_main();
            stop(false);

            return report_exception;
        }

        // The components::console_error_sink is only applied at the console,
        // so the default error sink never gets called on the locality, meaning
        // that the user never sees errors that kill the system before the
        // error parcel gets sent out. So, before we try to send the error
        // parcel (which might cause a double fault), print local diagnostics.
        components::server::console_error_sink(e);

        // Report this error to the console.
        naming::gid_type console_id;
        if (agas_client_.get_console_locality(console_id))
        {
            if (agas_client_.get_local_locality() != console_id) {
                components::console_error_sink(
                    naming::id_type(console_id, naming::id_type::unmanaged), e);
            }
        }

        components::stubs::runtime_support::terminate_all(
            naming::get_id_from_locality_id(HPX_AGAS_BOOTSTRAP_PREFIX));

        return report_exception;
    }

    bool runtime_impl_distributed::report_error(std::exception_ptr const& e)
    {
        return report_error(hpx::get_worker_thread_num(), e);
    }

    void runtime_impl_distributed::rethrow_exception()
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
    int runtime_impl_distributed::run(
        util::function_nonser<hpx_main_function_type> const& func)
    {
        // start the main thread function
        start(func);

        // now wait for everything to finish
        wait();
        stop();

        parcel_handler_.stop();      // stops parcelport for sure

        rethrow_exception();
        return result_;
    }

    ///////////////////////////////////////////////////////////////////////////
    int runtime_impl_distributed::run()
    {
        // start the main thread function
        start();

        // now wait for everything to finish
        int result = wait();
        stop();

        parcel_handler_.stop();      // stops parcelport for sure

        rethrow_exception();
        return result;
    }

    ///////////////////////////////////////////////////////////////////////////
    void runtime_impl_distributed::default_errorsink(
        std::string const& msg)
    {
        // log the exception information in any case
        LERR_(always) << "default_errorsink: unhandled exception: " << msg;

        std::cerr << msg << std::endl;
    }

    ///////////////////////////////////////////////////////////////////////////
    threads::policies::callback_notifier runtime_impl_distributed::get_notification_policy(
        char const* prefix)
    {
        typedef bool (runtime_impl_distributed::*report_error_t)(
            std::size_t, std::exception_ptr const&);

        using util::placeholders::_1;
        using util::placeholders::_2;
        using util::placeholders::_3;
        using util::placeholders::_4;

        notification_policy_type notifier;

        notifier.add_on_start_thread_callback(util::bind(
            &runtime_impl_distributed::init_tss, This(), prefix, _1, _2, _3, _4, false));
        notifier.add_on_stop_thread_callback(
            util::bind(&runtime_impl_distributed::deinit_tss, This(), prefix, _1));
        notifier.set_on_error_callback(
            util::bind(static_cast<report_error_t>(&runtime_impl_distributed::report_error),
                This(), _1, _2));

        return notifier;
    }

    void runtime_impl_distributed::init_tss(char const* context,
        std::size_t local_thread_num, std::size_t global_thread_num,
        char const* pool_name, char const* postfix, bool service_thread)
    {
        // prefix thread name with locality number, if needed
        std::string locality = "";//locality_prefix(get_config());

        error_code ec(lightweight);
        return init_tss_ex(locality, context, local_thread_num,
            global_thread_num, pool_name, postfix, service_thread, ec);
    }

    void runtime_impl_distributed::init_tss_ex(std::string const& locality,
        char const* context, std::size_t local_thread_num,
        std::size_t global_thread_num, char const* pool_name,
        char const* postfix, bool service_thread, error_code& ec)
    {
        // initialize our TSS
        this->runtime::init_tss();
        runtime_distributed*& runtime_distributed_ = get_runtime_distributed_ptr();
        if (nullptr == runtime_distributed_)
        {
            HPX_ASSERT(nullptr == threads::thread_self::get_self());

            runtime_distributed_ = this;
        }

        // set the thread's name, if it's not already set
        HPX_ASSERT(detail::thread_name().empty());

        std::string fullname = std::string(locality);
        if (!locality.empty())
            fullname += "/";
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
                        used_processing_units), ec);

// comment this out for now as on CIrcleCI this is causing unending grief
//                 if (ec)
//                 {
//                     HPX_THROW_EXCEPTION(kernel_error
//                         , "runtime_impl_distributed::init_tss_ex"
//                         , hpx::util::format(
//                             "failed to set thread affinity mask ("
//                             HPX_CPU_MASK_PREFIX "{:x}) for service thread: {}",
//                             used_processing_units, detail::thread_name()));
//                 }
            }
#endif
        }
    }

    void runtime_impl_distributed::deinit_tss(char const* context,
        std::size_t global_thread_num)
    {
        // call thread-specific user-supplied on_stop handler
        if (on_stop_func_)
        {
            on_stop_func_(
                global_thread_num, global_thread_num, "", context);
        }

        // reset our TSS
        this->runtime::deinit_tss();
        get_runtime_distributed_ptr() = nullptr;

        // reset PAPI support
        thread_support_->unregister_thread();

        // reset thread local storage
        detail::thread_name().clear();
    }

    naming::gid_type
    runtime_impl_distributed::get_next_id(std::size_t count)
    {
        return id_pool_.get_id(count);
    }

    void runtime_impl_distributed::
        add_pre_startup_function(startup_function_type f)
    {
        runtime_support_->add_pre_startup_function(std::move(f));
    }

    void runtime_impl_distributed::
        add_startup_function(startup_function_type f)
    {
        runtime_support_->add_startup_function(std::move(f));
    }

    void runtime_impl_distributed::
        add_pre_shutdown_function(shutdown_function_type f)
    {
        runtime_support_->add_pre_shutdown_function(std::move(f));
    }

    void runtime_impl_distributed::
        add_shutdown_function(shutdown_function_type f)
    {
        runtime_support_->add_shutdown_function(std::move(f));
    }

    hpx::util::io_service_pool* runtime_impl_distributed::
        get_thread_pool(char const* name)
    {
        HPX_ASSERT(name != nullptr);
#ifdef HPX_HAVE_IO_POOL
        if (0 == std::strncmp(name, "io", 2))
            return &io_pool_;
#endif
        if (0 == std::strncmp(name, "parcel", 6))
            return parcel_handler_.get_thread_pool(name);
#ifdef HPX_HAVE_TIMER_POOL
        if (0 == std::strncmp(name, "timer", 5))
            return &timer_pool_;
#endif
        if (0 == std::strncmp(name, "main", 4)) //-V112
            return &main_pool_;

        HPX_THROW_EXCEPTION(bad_parameter,
            "runtime_impl_distributed::get_thread_pool",
            std::string("unknown thread pool requested: ") + name);
        return nullptr;
    }


    /// Register an external OS-thread with HPX
    bool runtime_impl_distributed::register_thread(char const* name,
        std::size_t global_thread_num, bool service_thread, error_code& ec)
    {
        if (nullptr != get_runtime_ptr())
            return false;       // already registered

        // prefix thread name with locality number, if needed
        std::string locality = "";//locality_prefix(get_config());

        std::string thread_name(name);
        thread_name += "-thread";

        init_tss_ex(locality, thread_name.c_str(), global_thread_num,
            global_thread_num, "", nullptr, service_thread, ec);

        return !ec ? true : false;
    }

    /// Unregister an external OS-thread with HPX
    bool runtime_impl_distributed::unregister_thread()
    {
        if (nullptr != get_runtime_ptr())
            return false;    // never registered

        deinit_tss(detail::thread_name().c_str(), hpx::get_worker_thread_num());
        return true;
    }

    void runtime_impl_distributed::register_message_handler(char const* message_handler_type,
        char const* action, error_code& ec)
    {
        return runtime_support_->register_message_handler(
            message_handler_type, action, ec);
    }

    parcelset::policies::message_handler* runtime_impl_distributed::create_message_handler(
        char const* message_handler_type, char const* action,
        parcelset::parcelport* pp, std::size_t num_messages,
        std::size_t interval, error_code& ec)
    {
        return runtime_support_->create_message_handler(message_handler_type,
            action, pp, num_messages, interval, ec);
    }

    serialization::binary_filter* runtime_impl_distributed::create_binary_filter(
        char const* binary_filter_type, bool compress,
        serialization::binary_filter* next_filter, error_code& ec)
    {
        return runtime_support_->create_binary_filter(binary_filter_type,
            compress, next_filter, ec);
    }

    ///////////////////////////////////////////////////////////////////////////
    // There is no need to protect these global from thread concurrent access
    // as they are access during early startup only.
    std::vector<hpx::util::tuple<char const*, char const*> >
        message_handler_registrations;

    ///////////////////////////////////////////////////////////////////////////
    // Create an instance of a message handler plugin
    void register_message_handler(char const* message_handler_type,
        char const* action, error_code& ec)
    {
        runtime_distributed* rtd = get_runtime_distributed_ptr();
        if (nullptr != rtd) {
            return rtd->register_message_handler(message_handler_type, action, ec);
        }

        // store the request for later
        message_handler_registrations.push_back(
            hpx::util::make_tuple(message_handler_type, action));
    }

    parcelset::policies::message_handler* create_message_handler(
        char const* message_handler_type, char const* action,
        parcelset::parcelport* pp, std::size_t num_messages,
        std::size_t interval, error_code& ec)
    {
        runtime_distributed* rtd = get_runtime_distributed_ptr();
        if (nullptr != rtd) {
            return rtd->create_message_handler(message_handler_type, action,
                pp, num_messages, interval, ec);
        }

        HPX_THROWS_IF(ec, invalid_status, "create_message_handler",
            "the runtime system is not available at this time");
        return nullptr;
    }

    ///////////////////////////////////////////////////////////////////////////
    // Create an instance of a binary filter plugin
    serialization::binary_filter* create_binary_filter(char const* binary_filter_type,
        bool compress, serialization::binary_filter* next_filter, error_code& ec)
    {
        runtime_distributed* rtd = get_runtime_distributed_ptr();
        if (nullptr != rtd)
            return rtd->create_binary_filter
                    (binary_filter_type, compress, next_filter, ec);

        HPX_THROWS_IF(ec, invalid_status, "create_binary_filter",
            "the runtime system is not available at this time");
        return nullptr;
    }

    runtime_distributed& get_runtime_distributed()
    {
        HPX_ASSERT(get_runtime_distributed_ptr() != nullptr);
        return *get_runtime_distributed_ptr();
    }

    runtime_distributed*& get_runtime_distributed_ptr()
    {
        static HPX_NATIVE_TLS runtime_distributed* runtime_distributed_;
        return runtime_distributed_;
    }

    naming::gid_type const & get_locality()
    {
        return get_runtime_distributed().get_agas_client().get_local_locality();
    }

    ///////////////////////////////////////////////////////////////////////////
    // Helpers
    naming::id_type find_here(error_code& ec)
    {
        if (nullptr == hpx::applier::get_applier_ptr())
        {
            HPX_THROWS_IF(ec, invalid_status, "hpx::find_here",
                "the runtime system is not available at this time");
            return naming::invalid_id;
        }

        static naming::id_type here(
            hpx::applier::get_applier().get_raw_locality(ec),
            naming::id_type::unmanaged);
        return here;
    }

    naming::id_type find_root_locality(error_code& ec)
    {
        runtime_distributed* rt = hpx::get_runtime_distributed_ptr();
        if (nullptr == rt)
        {
            HPX_THROWS_IF(ec, invalid_status, "hpx::find_root_locality",
                "the runtime system is not available at this time");
            return naming::invalid_id;
        }

        naming::gid_type console_locality;
        if (!rt->get_agas_client().get_console_locality(console_locality))
        {
            HPX_THROWS_IF(ec, invalid_status, "hpx::find_root_locality",
                "the root locality is not available at this time");
            return naming::invalid_id;
        }

        if (&ec != &throws)
            ec = make_success_code();

        return naming::id_type(console_locality, naming::id_type::unmanaged);
    }

    std::vector<naming::id_type>
    find_all_localities(components::component_type type, error_code& ec)
    {
        std::vector<naming::id_type> locality_ids;
        if (nullptr == hpx::applier::get_applier_ptr())
        {
            HPX_THROWS_IF(ec, invalid_status, "hpx::find_all_localities",
                "the runtime system is not available at this time");
            return locality_ids;
        }

        hpx::applier::get_applier().get_localities(locality_ids, type, ec);
        return locality_ids;
    }

    std::vector<naming::id_type> find_all_localities(error_code& ec)
    {
        std::vector<naming::id_type> locality_ids;
        if (nullptr == hpx::applier::get_applier_ptr())
        {
            HPX_THROWS_IF(ec, invalid_status, "hpx::find_all_localities",
                "the runtime system is not available at this time");
            return locality_ids;
        }

        hpx::applier::get_applier().get_localities(locality_ids, ec);
        return locality_ids;
    }

    std::vector<naming::id_type>
    find_remote_localities(components::component_type type, error_code& ec)
    {
        std::vector<naming::id_type> locality_ids;
        if (nullptr == hpx::applier::get_applier_ptr())
        {
            HPX_THROWS_IF(ec, invalid_status, "hpx::find_remote_localities",
                "the runtime system is not available at this time");
            return locality_ids;
        }

        hpx::applier::get_applier().get_remote_localities(locality_ids, type, ec);
        return locality_ids;
    }

    std::vector<naming::id_type> find_remote_localities(error_code& ec)
    {
        std::vector<naming::id_type> locality_ids;
        if (nullptr == hpx::applier::get_applier_ptr())
        {
            HPX_THROWS_IF(ec, invalid_status, "hpx::find_remote_localities",
                "the runtime system is not available at this time");
            return locality_ids;
        }

        hpx::applier::get_applier().get_remote_localities(locality_ids,
            components::component_invalid, ec);

        return locality_ids;
    }

    // find a locality supporting the given component
    naming::id_type find_locality(components::component_type type, error_code& ec)
    {
        if (nullptr == hpx::applier::get_applier_ptr())
        {
            HPX_THROWS_IF(ec, invalid_status, "hpx::find_locality",
                "the runtime system is not available at this time");
            return naming::invalid_id;
        }

        std::vector<naming::id_type> locality_ids;
        hpx::applier::get_applier().get_localities(locality_ids, type, ec);

        if (ec || locality_ids.empty())
            return naming::invalid_id;

        // chose first locality to host the object
        return locality_ids.front();
    }

    /// \brief Return the number of localities which are currently registered
    ///        for the running application.
    std::uint32_t get_num_localities(hpx::launch::sync_policy, error_code& ec)
    {
        if (nullptr == hpx::get_runtime_distributed_ptr())
            return 0;

        return get_runtime_distributed().get_agas_client().get_num_localities(
            ec);
    }

    std::uint32_t get_initial_num_localities()
    {
        if (nullptr == hpx::get_runtime_ptr())
            return 0;

        return get_runtime().get_config().get_num_localities();
    }

    std::uint32_t get_num_localities(hpx::launch::sync_policy,
        components::component_type type, error_code& ec)
    {
        if (nullptr == hpx::get_runtime_ptr())
            return 0;

        return get_runtime_distributed().get_agas_client().get_num_localities(
            type, ec);
    }

    lcos::future<std::uint32_t> get_num_localities()
    {
        if (nullptr == hpx::get_runtime_ptr())
            return lcos::make_ready_future<std::uint32_t>(0);

        return get_runtime_distributed()
            .get_agas_client()
            .get_num_localities_async();
    }

    lcos::future<std::uint32_t> get_num_localities(
        components::component_type type)
    {
        if (nullptr == hpx::get_runtime_distributed_ptr())
            return lcos::make_ready_future<std::uint32_t>(0);

        return get_runtime_distributed()
            .get_agas_client()
            .get_num_localities_async(type);
    }

    std::size_t get_num_worker_threads()
    {
        runtime_distributed* rt = get_runtime_distributed_ptr();
        if (nullptr == rt)
        {
            HPX_THROW_EXCEPTION(
                invalid_status,
                "hpx::get_num_worker_threads",
                "the runtime system has not been initialized yet");
            return std::size_t(0);
        }

        error_code ec(lightweight);
        return static_cast<std::size_t>(
            rt->get_agas_client().get_num_overall_threads(ec));
    }

    std::uint32_t get_locality_id(error_code& ec)
    {
        return agas::get_locality_id(ec);
    }
}

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace naming
{
    // shortcut for get_runtime().get_agas_client()
    resolver_client& get_agas_client()
    {
        return get_runtime_distributed().get_agas_client();
    }
}}

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace parcelset
{
    bool do_background_work(
        std::size_t num_thread, parcelport_background_mode mode)
    {
        return get_runtime_distributed().get_parcel_handler().do_background_work(
            num_thread, mode);
    }
}}
