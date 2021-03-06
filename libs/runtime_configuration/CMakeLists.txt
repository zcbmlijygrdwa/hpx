# Copyright (c) 2019 The STE||AR-Group
#
# SPDX-License-Identifier: BSL-1.0
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

cmake_minimum_required(VERSION 3.3.2 FATAL_ERROR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(runtime_configuration_headers
  hpx/runtime_configuration/agas_service_mode.hpp
  hpx/runtime_configuration/component_registry_base.hpp
  hpx/runtime_configuration/ini.hpp
  hpx/runtime_configuration/plugin_registry_base.hpp
  hpx/runtime_configuration/runtime_configuration.hpp
  hpx/runtime_configuration/runtime_mode.hpp
  hpx/runtime_configuration/static_factory_data.hpp
)

set(runtime_configuration_compat_headers
  hpx/util/ini.hpp
  hpx/util/runtime_configuration.hpp
  hpx/plugins/plugin_registry_base.hpp
  hpx/runtime/components/component_registry_base.hpp
  hpx/runtime/components/static_factory_data.hpp
  hpx/runtime/runtime_mode.hpp
)

set(runtime_configuration_sources
  ini.cpp
  init_ini_data.cpp
  runtime_configuration.cpp
  runtime_mode.cpp
)

include(HPX_AddModule)
add_hpx_module(runtime_configuration
  COMPATIBILITY_HEADERS ON
  DEPRECATION_WARNINGS
  FORCE_LINKING_GEN
  GLOBAL_HEADER_GEN ON
  SOURCES ${runtime_configuration_sources}
  HEADERS ${runtime_configuration_headers}
  COMPAT_HEADERS ${runtime_configuration_compat_headers}
  DEPENDENCIES
    hpx_assertion
    hpx_concurrency
    hpx_config
    hpx_coroutines
    hpx_errors
    hpx_functional
    hpx_plugin
    hpx_prefix
    hpx_serialization
    hpx_thread_support
    hpx_version
  CMAKE_SUBDIRS examples tests
)
