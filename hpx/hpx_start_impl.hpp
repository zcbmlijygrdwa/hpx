//  Copyright (c)      2018 Mikael Simberg
//  Copyright (c) 2007-2016 Hartmut Kaiser
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_HPX_START_IMPL_HPP
#define HPX_HPX_START_IMPL_HPP

#include <hpx/assertion.hpp>
#include <hpx/hpx_start.hpp>
#include <hpx/hpx_user_main_config.hpp>
#include <hpx/program_options.hpp>
#include <hpx/runtime_configuration/runtime_mode.hpp>
#include <hpx/runtime/shutdown_function.hpp>
#include <hpx/runtime/startup_function.hpp>
#include <hpx/functional/bind_back.hpp>
#include <hpx/prefix/find_prefix.hpp>
#include <hpx/functional/function.hpp>

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#if defined(__FreeBSD__)
extern HPX_EXPORT char** freebsd_environ;
extern char** environ;
#endif

namespace hpx
{
    /// \cond NOINTERNAL
    namespace detail
    {
        HPX_EXPORT int run_or_start(
            util::function_nonser<
                int(hpx::program_options::variables_map& vm)
            > const& f,
            hpx::program_options::options_description const& desc_cmdline,
            int argc, char** argv, std::vector<std::string>&& ini_config,
            startup_function_type startup, shutdown_function_type shutdown,
            hpx::runtime_mode mode, bool blocking);

        HPX_EXPORT int run_or_start(resource::partitioner& rp,
            startup_function_type startup, shutdown_function_type shutdown,
            bool blocking);

#if defined(HPX_WINDOWS)
        void init_winsocket();
#endif
    }
    /// \endcond

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is the main, non-blocking entry point for any HPX application.
    /// This function (or one of its overloads below) should be called from the
    /// users `main()` function. It will set up the HPX runtime environment and
    /// schedule the function given by \p f as an HPX thread. It will return
    /// immediately after that. Use `hpx::wait` and `hpx::stop` to synchronize
    /// with the runtime system's execution.
    inline bool start(
        util::function_nonser<
            int(hpx::program_options::variables_map& vm)
        > const& f,
        hpx::program_options::options_description const& desc_cmdline,
        int argc, char** argv, std::vector<std::string> const& cfg,
        startup_function_type startup, shutdown_function_type shutdown,
        hpx::runtime_mode mode)
    {
#if defined(HPX_WINDOWS)
        detail::init_winsocket();
#endif
        util::set_hpx_prefix(HPX_PREFIX);
#if defined(__FreeBSD__)
        freebsd_environ = environ;
#endif
        // set a handler for std::abort, std::at_quick_exit, and std::atexit
        std::signal(SIGABRT, detail::on_abort);
        std::atexit(detail::on_exit);
#if defined(HPX_HAVE_CXX11_STD_QUICK_EXIT)
        std::at_quick_exit(detail::on_exit);
#endif

        return 0 == detail::run_or_start(f, desc_cmdline, argc, argv,
            hpx_startup::user_main_config(cfg),
            std::move(startup), std::move(shutdown), mode, false);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is the main, non-blocking entry point for any HPX application.
    /// This function (or one of its overloads below) should be called from the
    /// users `main()` function. It will set up the HPX runtime environment and
    /// schedule the function given by \p f as an HPX thread. It will return
    /// immediately after that. Use `hpx::wait` and `hpx::stop` to synchronize
    /// with the runtime system's execution.
    inline bool
    start(int (*f)(hpx::program_options::variables_map& vm),
        hpx::program_options::options_description const& desc_cmdline,
        int argc, char** argv, startup_function_type startup,
        shutdown_function_type shutdown, hpx::runtime_mode mode)
    {
        std::vector<std::string> cfg;
        return start(f, desc_cmdline, argc, argv, cfg, std::move(startup),
            std::move(shutdown), mode);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is a simplified main, non-blocking entry point, which can be used
    /// to set up the runtime for an HPX application (the runtime system will be
    /// set up in console mode or worker mode depending on the command line
    /// settings). It will return immediately after that. Use `hpx::wait` and
    /// `hpx::stop` to synchronize with the runtime system's execution.
    inline bool
    start(hpx::program_options::options_description const& desc_cmdline,
        int argc, char** argv, startup_function_type startup,
        shutdown_function_type shutdown, hpx::runtime_mode mode)
    {
        return start(static_cast<hpx_main_type>(::hpx_main), desc_cmdline,
            argc, argv, std::move(startup), std::move(shutdown), mode);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is a simplified main, non-blocking entry point, which can be used
    /// to set up the runtime for an HPX application (the runtime system will
    /// be set up in console mode or worker mode depending on the command line
    /// settings). It will return immediately after that. Use `hpx::wait` and
    /// `hpx::stop` to synchronize with the runtime system's execution.
    inline bool
    start(hpx::program_options::options_description const& desc_cmdline,
        int argc, char** argv, std::vector<std::string> const& cfg,
        startup_function_type startup, shutdown_function_type shutdown,
        hpx::runtime_mode mode)
    {
        return start(static_cast<hpx_main_type>(::hpx_main), desc_cmdline,
            argc, argv, cfg, std::move(startup), std::move(shutdown), mode);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is a simplified main, non-blocking entry point, which can be used
    /// to set up the runtime for an HPX application (the runtime system will
    /// be set up in console mode or worker mode depending on the command line
    /// settings). It will return immediately after that. Use `hpx::wait` and
    /// `hpx::stop` to synchronize with the runtime system's execution.
    inline bool
    start(int argc, char** argv, std::vector<std::string> const& cfg,
        hpx::runtime_mode mode)
    {
        using hpx::program_options::options_description;

        options_description desc_commandline(
            "Usage: " HPX_APPLICATION_STRING " [options]");

        return start(desc_commandline, argc, argv, cfg, startup_function_type(),
            shutdown_function_type(), mode);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is a simplified main, non-blocking entry point, which can be used
    /// to set up the runtime for an HPX application (the runtime system will
    /// be set up in console mode or worker mode depending on the command line
    /// settings). It will return immediately after that. Use `hpx::wait` and
    /// `hpx::stop` to synchronize with the runtime system's execution.
    inline bool
    start(hpx::program_options::options_description const& desc_cmdline,
        int argc, char** argv, hpx::runtime_mode mode)
    {
        return start(static_cast<hpx_main_type>(::hpx_main), desc_cmdline,
            argc, argv, startup_function_type(), shutdown_function_type(), mode);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is a simplified main, non-blocking entry point, which can be used
    /// to set up the runtime for an HPX application (the runtime system will
    /// be set up in console mode or worker mode depending on the command line
    /// settings). It will return immediately after that. Use `hpx::wait` and
    /// `hpx::stop` to synchronize with the runtime system's execution.
    inline bool
    start(hpx::program_options::options_description const& desc_cmdline,
        int argc, char** argv, std::vector<std::string> const& cfg,
        hpx::runtime_mode mode)
    {
        return start(desc_cmdline, argc, argv, cfg, startup_function_type(),
            shutdown_function_type(), mode);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is a simplified main, non-blocking entry point, which can be used
    /// to set up the runtime for an HPX application (the runtime system will
    /// be set up in console mode or worker mode depending on the command line
    /// settings). It will return immediately after that. Use `hpx::wait` and
    /// `hpx::stop` to synchronize with the runtime system's execution.
    inline bool
    start(std::string const& app_name, int argc, char** argv,
        hpx::runtime_mode mode)
    {
        return start(static_cast<hpx_main_type>(::hpx_main), app_name, argc,
            argv, startup_function_type(), shutdown_function_type(), mode);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is a simplified main, non-blocking entry point, which can be used
    /// to set up the runtime for an HPX application (the runtime system will
    /// be set up in console mode or worker mode depending on the command line
    /// settings). It will return immediately after that. Use `hpx::wait` and
    /// `hpx::stop` to synchronize with the runtime system's execution.
    inline bool start(int argc, char** argv, hpx::runtime_mode mode)
    {
        return start(static_cast<hpx_main_type>(::hpx_main),
            HPX_APPLICATION_STRING, argc, argv, mode);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is a simplified main, non-blocking entry point, which can be used
    /// to set up the runtime for an HPX application (the runtime system will
    /// be set up in console mode or worker mode depending on the command line
    /// settings). It will return immediately after that. Use `hpx::wait` and
    /// `hpx::stop` to synchronize with the runtime system's execution.
    inline bool start(std::vector<std::string> const& cfg,
        hpx::runtime_mode mode)
    {
        using hpx::program_options::options_description;

        options_description desc_commandline(
            std::string("Usage: ") + HPX_APPLICATION_STRING +  " [options]");

        char *dummy_argv[2] = { const_cast<char*>(HPX_APPLICATION_STRING), nullptr };

        return start(static_cast<hpx_main_type>(::hpx_main), desc_commandline,
            1, dummy_argv, cfg, startup_function_type(),
            shutdown_function_type(), mode);
    }

    /// \brief Main non-blocking entry point for launching the HPX runtime system.
    ///
    /// This is a simplified main, non-blocking entry point, which can be used
    /// to set up the runtime for an HPX application (the runtime system will
    /// be set up in console mode or worker mode depending on the command line
    /// settings). It will return immediately after that. Use `hpx::wait` and
    /// `hpx::stop` to synchronize with the runtime system's execution.
    inline bool start(int (*f)(hpx::program_options::variables_map& vm),
        std::string const& app_name, int argc, char** argv,
        hpx::runtime_mode mode)
    {
        using hpx::program_options::options_description;

        options_description desc_commandline(
            "Usage: " + app_name +  " [options]");

        if (argc == 0 || argv == nullptr)
        {
            char *dummy_argv[2] = { const_cast<char*>(app_name.c_str()), nullptr };
            return start(desc_commandline, 1, dummy_argv, mode);
        }

        return start(f, desc_commandline, argc, argv, startup_function_type(),
            shutdown_function_type(), mode);
    }

    // Main non-blocking entry point for launching the HPX runtime system.
    inline bool start(int (*f)(hpx::program_options::variables_map& vm),
        int argc, char** argv, hpx::runtime_mode mode)
    {
        std::string app_name(HPX_APPLICATION_STRING);
        return start(f, app_name, argc, argv, mode);
    }

    /// \cond NOINTERNAL
    namespace detail
    {
        HPX_EXPORT int init_helper(
            hpx::program_options::variables_map&,
            util::function_nonser<int(int, char**)> const&);
    }
    /// \endcond

    // Main non-blocking entry point for launching the HPX runtime system.
    inline bool start(util::function_nonser<int(int, char**)> const& f,
        std::string const& app_name, int argc, char** argv,
        hpx::runtime_mode mode)
    {
        using hpx::program_options::options_description;

        options_description desc_commandline(
            "Usage: " + app_name +  " [options]");

        util::function_nonser<int(hpx::program_options::variables_map& vm)>
            main_f = util::bind_back(detail::init_helper, f);
        std::vector<std::string> cfg;

        HPX_ASSERT(argc != 0 && argv != nullptr);

        return start(main_f, desc_commandline, argc, argv, cfg,
            startup_function_type(), shutdown_function_type(), mode);
    }

    // Main non-blocking entry point for launching the HPX runtime system.
    inline bool start(util::function_nonser<int(int, char**)> const& f,
        int argc, char** argv, hpx::runtime_mode mode)
    {
        std::string app_name(HPX_APPLICATION_STRING);
        return start(f, app_name, argc, argv, mode);
    }

    inline bool start(util::function_nonser<int(int, char**)> const& f,
        int argc, char** argv, std::vector<std::string> const& cfg,
        hpx::runtime_mode mode)
    {
        std::string app_name(HPX_APPLICATION_STRING);
        using hpx::program_options::options_description;

        options_description desc_commandline(
            "Usage: " + app_name +  " [options]");

        util::function_nonser<int(hpx::program_options::variables_map& vm)>
            main_f = util::bind_back(detail::init_helper, f);

        HPX_ASSERT(argc != 0 && argv != nullptr);

        return start(main_f, desc_commandline, argc, argv, cfg,
            startup_function_type(), shutdown_function_type(), mode);
    }

    inline bool start(util::function_nonser<int(int, char**)> const& f,
        std::vector<std::string> const& cfg,
        hpx::runtime_mode mode)
    {
        char *dummy_argv[2] = { const_cast<char*>(HPX_APPLICATION_STRING), nullptr };

        return start(f, 1, dummy_argv, cfg, mode);
    }

    inline bool start(std::nullptr_t f, std::string const& app_name, int argc,
        char** argv, hpx::runtime_mode mode)
    {
        using hpx::program_options::options_description;

        options_description desc_commandline(
            "Usage: " + app_name +  " [options]");

        util::function_nonser<int(hpx::program_options::variables_map& vm)>
            main_f;
        std::vector<std::string> cfg;

        HPX_ASSERT(argc != 0 && argv != nullptr);

        return start(main_f, desc_commandline, argc, argv, cfg,
            startup_function_type(), shutdown_function_type(), mode);
    }

    inline bool start(std::nullptr_t f, int argc, char** argv,
        hpx::runtime_mode mode)
    {
        std::string app_name(HPX_APPLICATION_STRING);
        return start(f, app_name, argc, argv, mode);
    }

    inline bool start(std::nullptr_t f, int argc, char** argv,
        std::vector<std::string> const& cfg, hpx::runtime_mode mode)
    {
        std::string app_name(HPX_APPLICATION_STRING);
        using hpx::program_options::options_description;

        options_description desc_commandline(
            "Usage: " + app_name +  " [options]");

        util::function_nonser<int(hpx::program_options::variables_map& vm)>
            main_f;

        HPX_ASSERT(argc != 0 && argv != nullptr);

        return start(main_f, desc_commandline, argc, argv, cfg,
            startup_function_type(), shutdown_function_type(), mode);
    }

    inline bool start(std::nullptr_t f, std::vector<std::string> const& cfg,
        hpx::runtime_mode mode)
    {
        char* dummy_argv[2] = {
            const_cast<char*>(HPX_APPLICATION_STRING), nullptr};

        return start(nullptr, 1, dummy_argv, cfg, mode);
    }

    ////////////////////////////////////////////////////////////////////////////
    inline bool start(resource::partitioner& rp, startup_function_type startup,
        shutdown_function_type shutdown)
    {
#if defined(HPX_WINDOWS)
        detail::init_winsocket();
#endif
        util::set_hpx_prefix(HPX_PREFIX);
#if defined(__FreeBSD__)
        freebsd_environ = environ;
#endif
        // set a handler for std::abort
        std::signal(SIGABRT, detail::on_abort);
        std::atexit(detail::on_exit);
#if defined(HPX_HAVE_CXX11_STD_QUICK_EXIT)
        std::at_quick_exit(detail::on_exit);
#endif

        return 0 == detail::run_or_start(
            rp, std::move(startup), std::move(shutdown), false);
    }
}

#endif /*HPX_HPX_START_IMPL_HPP*/
