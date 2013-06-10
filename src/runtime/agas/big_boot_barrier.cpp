////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2011 Bryce Lelbach & Katelyn Kufahl
//  Copyright (c) 2007-2012 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

#include <hpx/version.hpp>
#include <hpx/exception.hpp>
#include <hpx/config.hpp>
#include <hpx/hpx.hpp>
#include <hpx/runtime/actions/plain_action.hpp>
#include <hpx/runtime/components/plain_component_factory.hpp>
#include <hpx/runtime/components/server/managed_component_base.hpp>
#include <hpx/util/portable_binary_iarchive.hpp>
#include <hpx/util/stringstream.hpp>
#include <hpx/util/reinitializable_static.hpp>
#include <hpx/runtime/actions/action_support.hpp>
#include <hpx/runtime/parcelset/parcel.hpp>
#include <hpx/runtime/parcelset/parcelport.hpp>
#include <hpx/runtime/parcelset/tcp/parcelport_connection.hpp>
#include <hpx/runtime/naming/resolver_client.hpp>
#include <hpx/runtime/agas/interface.hpp>
#include <hpx/runtime/agas/big_boot_barrier.hpp>

#if defined(HPX_HAVE_SECURITY)
#include <hpx/components/security/certificate.hpp>
#include <hpx/components/security/signed_type.hpp>
#endif

#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/assert.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/lexical_cast.hpp>

namespace hpx { namespace agas
{

typedef components::detail::heap_factory<
    lcos::detail::promise<
        response
      , response
    >
  , components::managed_component<
        lcos::detail::promise<
            response
          , response
        >
    >
> response_heap_type;

// TODO: Make assertions exceptions
void early_parcel_sink(
    parcelset::parcel const& p
    )
{ // {{{
    // De-serialize the parcel data
//     util::portable_binary_iarchive archive(*parcel_data, boost::archive::no_header);
//
//     std::size_t parcel_count = 0;
//     archive >> parcel_count;
//     for(std::size_t i = 0; i < parcel_count; ++i)
//     {
//         parcelset::parcel p;
//         archive >> p;

        // decode the action-type in the parcel
        actions::action_type act = p.get_action();

        // early parcels should only be plain actions
        BOOST_ASSERT(actions::base_action::plain_action == act->get_action_type());

        // early parcels can't have continuations
        BOOST_ASSERT(!p.get_continuation());

        // We should not allow any exceptions to escape the execution of the
        // action as this would bring down the ASIO thread we execute in.
        try {
            act->get_thread_function(0)
                (threads::thread_state_ex(threads::wait_signaled));
        }
        catch (...) {
            std::cerr << hpx::diagnostic_information(boost::current_exception())
                      << std::endl;
            std::abort();
        }
//     }
} // }}}

// This structure is used when a locality registers with node zero
struct registration_header
{
    registration_header() {}

    // TODO: pass head address as a GVA
    registration_header(
        naming::locality const& locality_
      , boost::uint64_t parcelport_allocation_
      , boost::uint64_t response_allocation_
      , boost::uint64_t component_runtime_support_ptr_
      , boost::uint64_t component_memory_ptr_
      , boost::uint64_t primary_ns_ptr_
      , boost::uint32_t num_threads_
    ) :
        locality(locality_)
      , parcelport_allocation(parcelport_allocation_)
      , response_allocation(response_allocation_)
      , component_runtime_support_ptr(component_runtime_support_ptr_)
      , component_memory_ptr(component_memory_ptr_)
      , primary_ns_ptr(primary_ns_ptr_)
      , num_threads(num_threads_)
    {}

    naming::locality locality;
    boost::uint64_t parcelport_allocation;
    boost::uint64_t response_allocation;

    boost::uint64_t component_runtime_support_ptr;
    boost::uint64_t component_memory_ptr;
    boost::uint64_t primary_ns_ptr;

    boost::uint32_t num_threads;

    template <typename Archive>
    void serialize(Archive & ar, const unsigned int)
    {
        ar & locality;
        ar & parcelport_allocation;
        ar & response_allocation;
        ar & component_runtime_support_ptr;
        ar & component_memory_ptr;
        ar & primary_ns_ptr;
        ar & num_threads;
    }
};

// This structure is used in the response from node zero to the locality which
// is trying to register.

// TODO: We don't need to send the full gid for the lower and upper bound of
// each range, we can just send the lower gid and offsets into it.
struct notification_header
{
    notification_header() {}

    notification_header(
        naming::gid_type const& prefix_
      , naming::gid_type const& response_lower_gid_
      , naming::gid_type const& response_upper_gid_
      , naming::gid_type const& parcelport_lower_gid_
      , naming::gid_type const& parcelport_upper_gid_
      , naming::address const& locality_ns_address_
      , naming::address const& primary_ns_address_
      , naming::address const& component_ns_address_
      , naming::address const& symbol_ns_address_
    ) :
        prefix(prefix_)
      , response_lower_gid(response_lower_gid_)
      , response_upper_gid(response_upper_gid_)
      , parcelport_lower_gid(parcelport_lower_gid_)
      , parcelport_upper_gid(parcelport_upper_gid_)
      , locality_ns_address(locality_ns_address_)
      , primary_ns_address(primary_ns_address_)
      , component_ns_address(component_ns_address_)
      , symbol_ns_address(symbol_ns_address_)
    {}

    naming::gid_type prefix;
    naming::gid_type response_lower_gid;
    naming::gid_type response_upper_gid;
    naming::gid_type parcelport_lower_gid;
    naming::gid_type parcelport_upper_gid;
    naming::address locality_ns_address;
    naming::address primary_ns_address;
    naming::address component_ns_address;
    naming::address symbol_ns_address;
#if defined(HPX_HAVE_SECURITY)
    components::security::signed_certificate root_certificate;
#endif

    template <typename Archive>
    void serialize(Archive & ar, const unsigned int)
    {
        ar & prefix;
        ar & response_lower_gid;
        ar & response_upper_gid;
        ar & parcelport_lower_gid;
        ar & parcelport_upper_gid;
        ar & locality_ns_address;
        ar & primary_ns_address;
        ar & component_ns_address;
        ar & symbol_ns_address;
#if defined(HPX_HAVE_SECURITY)
        ar & root_certificate;
#endif
    }
};

// {{{ early action forwards
void register_console(registration_header const& header);

void notify_console(notification_header const& header);

void register_worker(registration_header const& header);

void notify_worker(notification_header const& header);
// }}}

// {{{ early action types
typedef actions::plain_action1<
    registration_header const&
  , register_console
> register_console_action;

typedef actions::plain_action1<
    notification_header const&
  , notify_console
> notify_console_action;

typedef actions::plain_action1<
    registration_header const&
  , register_worker
> register_worker_action;

typedef actions::plain_action1<
    notification_header const&
  , notify_worker
> notify_worker_action;
// }}}

}}

using hpx::agas::register_console_action;
using hpx::agas::notify_console_action;
using hpx::agas::register_worker_action;
using hpx::agas::notify_worker_action;

HPX_ACTION_HAS_CRITICAL_PRIORITY(register_console_action);
HPX_ACTION_HAS_CRITICAL_PRIORITY(notify_console_action);
HPX_ACTION_HAS_CRITICAL_PRIORITY(register_worker_action);
HPX_ACTION_HAS_CRITICAL_PRIORITY(notify_worker_action);

HPX_REGISTER_PLAIN_ACTION(register_console_action,
    register_console_action, hpx::components::factory_enabled)
HPX_REGISTER_PLAIN_ACTION(notify_console_action,
    notify_console_action, hpx::components::factory_enabled)
HPX_REGISTER_PLAIN_ACTION(register_worker_action,
    register_worker_action, hpx::components::factory_enabled)
HPX_REGISTER_PLAIN_ACTION(notify_worker_action,
    notify_worker_action, hpx::components::factory_enabled)

namespace hpx { namespace agas
{

// {{{ early action definitions
// remote call to AGAS
// TODO: pass data members from the notification header to the client API instead
// of using temporaries which cause copying
// TODO: merge with register_worker which is all but identical
void register_console(registration_header const& header)
{
    // This lock acquires the bbb mutex on creation. When it goes out of scope,
    // it's dtor calls big_boot_barrier::notify().
    big_boot_barrier::scoped_lock lock(get_big_boot_barrier());

    runtime& rt = get_runtime();
    naming::resolver_client& agas_client = rt.get_agas_client();

    if (HPX_UNLIKELY(!agas_client.is_bootstrap()))
    {
        HPX_THROW_EXCEPTION(internal_server_error
            , "agas::register_console"
            , "registration parcel received by non-bootstrap locality");
    }

    naming::gid_type prefix;
    if (!agas_client.register_locality(header.locality, prefix, header.num_threads))
    {
        HPX_THROW_EXCEPTION(internal_server_error
            , "agas::register_console"
            , boost::str(boost::format(
                "attempt to register locality %s more than once") %
                    header.locality));
        return;
    }

    naming::gid_type parcel_lower, parcel_upper;
    agas_client.get_id_range(header.locality, header.parcelport_allocation
      , parcel_lower, parcel_upper);

    naming::gid_type heap_lower, heap_upper;
    agas_client.get_id_range(header.locality, header.response_allocation
      , heap_lower, heap_upper);

    naming::gid_type runtime_support_gid(prefix.get_msb()
      , header.component_runtime_support_ptr);
    naming::address runtime_support_address(header.locality
      , components::get_component_type<components::server::runtime_support>()
      , header.component_runtime_support_ptr);
    agas_client.bind(runtime_support_gid, runtime_support_address);

    naming::gid_type memory_gid(prefix.get_msb()
      , header.component_memory_ptr);
    naming::address memory_address(header.locality
      , components::get_component_type<components::server::memory>()
      , header.component_memory_ptr);
    agas_client.bind(memory_gid, memory_address);

    naming::gid_type primary_ns_gid(
        stubs::primary_namespace::get_service_instance(prefix));
    naming::address primary_ns_address(header.locality
      , components::get_component_type<agas::server::primary_namespace>()
      , header.primary_ns_ptr);
    agas_client.bind(primary_ns_gid, primary_ns_address);

    agas::register_name("/locality(console)", prefix);

    naming::address locality_addr(get_runtime().here(),
        server::locality_namespace::get_component_type(),
            static_cast<void*>(&agas_client.bootstrap->locality_ns_server_));
    naming::address primary_addr(get_runtime().here(),
        server::primary_namespace::get_component_type(),
            static_cast<void*>(&agas_client.bootstrap->primary_ns_server_));
    naming::address component_addr(get_runtime().here(),
        server::component_namespace::get_component_type(),
            static_cast<void*>(&agas_client.bootstrap->component_ns_server_));
    naming::address symbol_addr(get_runtime().here(),
        server::symbol_namespace::get_component_type(),
            static_cast<void*>(&agas_client.bootstrap->symbol_ns_server_));

    notification_header hdr (
        prefix, heap_lower, heap_upper, parcel_lower, parcel_upper,
        locality_addr, primary_addr, component_addr, symbol_addr);

#if defined(HPX_HAVE_SECURITY)
    // wait for the root certificate to be available
    bool got_root_certificate = false;
    for (std::size_t i = 0; i != HPX_MAX_NETWORK_RETRIES; ++i)
    {
        error_code ec(lightweight);
        hdr.root_certificate = rt.get_root_certificate(ec);
        if (!ec)
        {
            got_root_certificate = true;
            break;
        }
        boost::this_thread::sleep(boost::get_system_time() +
            boost::posix_time::milliseconds(HPX_NETWORK_RETRIES_SLEEP));
    }

    if (!got_root_certificate)
    {
        HPX_THROW_EXCEPTION(internal_server_error
          , "agas::register_console"
          , "could not obtain root certificate");
        return;
    }
#endif

    actions::base_action* p =
        new actions::transfer_action<notify_console_action>(hdr);

    HPX_STD_FUNCTION<void()>* thunk = new HPX_STD_FUNCTION<void()>(
        boost::bind(
            &big_boot_barrier::apply
          , boost::ref(get_big_boot_barrier())
          , naming::get_locality_id_from_gid(prefix)
          , naming::address(header.locality)
          , p));

    get_big_boot_barrier().add_thunk(thunk);
}

// TODO: merge with notify_worker which is all but identical
// TODO: callback must finishing installing new heap
// TODO: callback must set up future pool
// AGAS callback to client
void notify_console(notification_header const& header)
{
    // This lock acquires the bbb mutex on creation. When it goes out of scope,
    // it's dtor calls big_boot_barrier::notify().
    big_boot_barrier::scoped_lock lock(get_big_boot_barrier());

    runtime& rt = get_runtime();
    naming::resolver_client& agas_client = rt.get_agas_client();

    if (HPX_UNLIKELY(agas_client.get_status() != starting))
    {
        hpx::util::osstream strm;
        strm << "locality "
             << rt.here()
             << " has launched early";
        HPX_THROW_EXCEPTION(internal_server_error,
            "agas::notify_console",
            hpx::util::osstream_get_string(strm));
    }

    // set our prefix
    agas_client.set_local_locality(header.prefix);
    rt.get_config().parse("assigned locality",
        boost::str(boost::format("hpx.locality!=%1%")
                  % naming::get_locality_id_from_gid(header.prefix)));

    // store the full addresses of the agas servers in our local router
    agas_client.locality_ns_addr_ = header.locality_ns_address;
    agas_client.primary_ns_addr_ = header.primary_ns_address;
    agas_client.component_ns_addr_ = header.component_ns_address;
    agas_client.symbol_ns_addr_ = header.symbol_ns_address;

    // register runtime support component
    naming::gid_type const runtime_support_gid(header.prefix.get_msb(), 0);
    naming::address runtime_support_address(rt.here()
      , components::get_component_type<components::server::runtime_support>()
      , rt.get_runtime_support_lva());
    agas_client.bind(runtime_support_gid, runtime_support_address);

    // register local primary namespace component
    naming::gid_type const primary_gid =
        stubs::primary_namespace::get_service_instance(
            agas_client.get_local_locality());
    naming::address primary_addr(rt.here()
      , server::primary_namespace::get_component_type(),
        agas_client.get_hosted_primary_ns_ptr());
    agas_client.bind(primary_gid, primary_addr);

#if defined(HPX_HAVE_SECURITY)
    rt.store_root_certificate(header.root_certificate);
#endif

    // Assign the initial parcel gid range to the parcelport. Note that we can't
    // get the parcelport through the parcelhandler because it isn't up yet.
    rt.get_id_pool().set_range(
        header.parcelport_lower_gid
      , header.parcelport_upper_gid);

    // assign the initial gid range to the unique id range allocator that our
    // response heap is using
    response_heap_type::get_heap().set_range(
        header.response_lower_gid
      , header.response_upper_gid);

    // allocate our first heap
    response_heap_type::block_type* p = response_heap_type::alloc_heap();

    // set the base gid that we bound to this heap
    p->set_gid(header.response_lower_gid);

    // push the heap onto the OSHL
    response_heap_type::get_heap().add_heap(p);

    // bind range of GIDs to head addresses
    agas_client.bind_range(
        header.response_lower_gid
      , response_heap_type::block_type::heap_step
      , p->get_address()
      , response_heap_type::block_type::heap_size);

    // set up the future pools
    naming::resolver_client::locality_promise_pool_type& locality_promise_pool
        = agas_client.hosted->locality_promise_pool_;

    util::runtime_configuration const& ini_ = rt.get_config();
    const std::size_t pool_size = ini_.get_agas_promise_pool_size();

    for (std::size_t i = 0; i < pool_size; ++i)
    {
        locality_promise_pool.enqueue(
            new lcos::packaged_action<server::locality_namespace::service_action>);
    }
}

// remote call to AGAS
void register_worker(registration_header const& header)
{
    // This lock acquires the bbb mutex on creation. When it goes out of scope,
    // it's dtor calls big_boot_barrier::notify().
    big_boot_barrier::scoped_lock lock(get_big_boot_barrier());

    runtime& rt = get_runtime();
    naming::resolver_client& agas_client = rt.get_agas_client();

    if (HPX_UNLIKELY(agas_client.is_connecting()))
    {
        HPX_THROW_EXCEPTION(
            internal_server_error
          , "agas::register_worker"
          , "runtime_mode_connect can't find running application.");
    }

    if (HPX_UNLIKELY(!agas_client.is_bootstrap()))
    {
        HPX_THROW_EXCEPTION(
            internal_server_error
          , "agas::register_worker"
          , "registration parcel received by non-bootstrap locality.");
    }

    naming::gid_type prefix;
    if (!agas_client.register_locality(header.locality, prefix, header.num_threads))
    {
        HPX_THROW_EXCEPTION(internal_server_error
            , "agas::register_worker"
            , boost::str(boost::format(
                "attempt to register locality %s more than once") %
                    header.locality));
        return;
    }

    naming::gid_type parcel_lower, parcel_upper;
    agas_client.get_id_range(header.locality, header.parcelport_allocation
      , parcel_lower, parcel_upper);

    naming::gid_type heap_lower, heap_upper;
    agas_client.get_id_range(header.locality, header.response_allocation
      , heap_lower, heap_upper);

    naming::gid_type runtime_support_gid(prefix.get_msb()
      , header.component_runtime_support_ptr);
    naming::address runtime_support_address(header.locality
      , components::get_component_type<components::server::runtime_support>()
      , header.component_runtime_support_ptr);
    agas_client.bind(runtime_support_gid, runtime_support_address);

    naming::gid_type memory_gid(prefix.get_msb()
      , header.component_memory_ptr);
    naming::address memory_address(header.locality
      , components::get_component_type<components::server::memory>()
      , header.component_memory_ptr);
    agas_client.bind(memory_gid, memory_address);

    naming::gid_type primary_ns_gid(
        stubs::primary_namespace::get_service_instance(prefix));
    naming::address primary_ns_address(header.locality
      , components::get_component_type<agas::server::primary_namespace>()
      , header.primary_ns_ptr);
    agas_client.bind(primary_ns_gid, primary_ns_address);

    naming::address locality_addr(rt.here(),
        server::locality_namespace::get_component_type(),
            static_cast<void*>(&agas_client.bootstrap->locality_ns_server_));
    naming::address primary_addr(rt.here(),
        server::primary_namespace::get_component_type(),
            static_cast<void*>(&agas_client.bootstrap->primary_ns_server_));
    naming::address component_addr(rt.here(),
        server::component_namespace::get_component_type(),
            static_cast<void*>(&agas_client.bootstrap->component_ns_server_));
    naming::address symbol_addr(rt.here(),
        server::symbol_namespace::get_component_type(),
            static_cast<void*>(&agas_client.bootstrap->symbol_ns_server_));

    notification_header hdr (
        prefix, heap_lower, heap_upper, parcel_lower, parcel_upper,
        locality_addr, primary_addr, component_addr, symbol_addr);

#if defined(HPX_HAVE_SECURITY)
    // wait for the root certificate to be available
    bool got_root_certificate = false;
    for (std::size_t i = 0; i != HPX_MAX_NETWORK_RETRIES; ++i)
    {
        error_code ec(lightweight);
        hdr.root_certificate = rt.get_root_certificate(ec);
        if (!ec)
        {
            got_root_certificate = true;
            break;
        }
        boost::this_thread::sleep(boost::get_system_time() +
            boost::posix_time::milliseconds(HPX_NETWORK_RETRIES_SLEEP));
    }

    if (!got_root_certificate)
    {
        HPX_THROW_EXCEPTION(internal_server_error
          , "agas::register_console"
          , "could not obtain root certificate");
        return;
    }
#endif

    actions::base_action* p =
        new actions::transfer_action<notify_console_action>(hdr);

    // FIXME: This could screw with startup.

    // TODO: Handle cases where localities try to connect to AGAS while it's
    // shutting down.
    if (agas_client.get_status() != starting)
    {
        // We can just send the parcel now, the connecting locality isn't a part
        // of startup synchronization.
        get_big_boot_barrier().apply(naming::get_locality_id_from_gid(prefix)
          , naming::address(header.locality), p);
    }

    else // AGAS is starting up; this locality is participating in startup
    {    // synchronization.
        HPX_STD_FUNCTION<void()>* thunk = new HPX_STD_FUNCTION<void()>(
            boost::bind(
                &big_boot_barrier::apply
              , boost::ref(get_big_boot_barrier())
              , naming::get_locality_id_from_gid(prefix)
              , naming::address(header.locality)
              , p));

        get_big_boot_barrier().add_thunk(thunk);
    }
}

// TODO: callback must finish installing new heap
// TODO: callback must set up future pool
// AGAS callback to client
void notify_worker(notification_header const& header)
{
    // This lock acquires the bbb mutex on creation. When it goes out of scope,
    // it's dtor calls big_boot_barrier::notify().
    big_boot_barrier::scoped_lock lock(get_big_boot_barrier());

    runtime& rt = get_runtime();
    naming::resolver_client& agas_client = rt.get_agas_client();

    // set our prefix
    agas_client.set_local_locality(header.prefix);
    rt.get_config().parse("assigned locality",
        boost::str(boost::format("hpx.locality!=%1%")
                  % naming::get_locality_id_from_gid(header.prefix)));

    // store the full addresses of the agas servers in our local service
    agas_client.locality_ns_addr_ = header.locality_ns_address;
    agas_client.primary_ns_addr_ = header.primary_ns_address;
    agas_client.component_ns_addr_ = header.component_ns_address;
    agas_client.symbol_ns_addr_ = header.symbol_ns_address;

    // register runtime support component
    naming::gid_type const runtime_support_gid(header.prefix.get_msb()
      , rt.get_runtime_support_lva());
    naming::address runtime_support_address(rt.here()
      , components::get_component_type<components::server::runtime_support>()
      , rt.get_runtime_support_lva());
    agas_client.bind(runtime_support_gid, runtime_support_address);

    // register local primary namespace component
    naming::gid_type const primary_gid =
        stubs::primary_namespace::get_service_instance(
            agas_client.get_local_locality());
    naming::address primary_addr(rt.here()
      , server::primary_namespace::get_component_type(),
        agas_client.get_hosted_primary_ns_ptr());
    agas_client.bind(primary_gid, primary_addr);

#if defined(HPX_HAVE_SECURITY)
    rt.store_root_certificate(header.root_certificate);
#endif

    // Assign the initial parcel gid range to the parcelport. Note that we can't
    // get the parcelport through the parcelhandler because it isn't up yet.
    rt.get_id_pool().set_range(
        header.parcelport_lower_gid
      , header.parcelport_upper_gid);

    // assign the initial gid range to the unique id range allocator that our
    // response heap is using
    response_heap_type::get_heap().set_range(
        header.response_lower_gid
      , header.response_upper_gid);

    // allocate our first heap
    response_heap_type::block_type* p = response_heap_type::alloc_heap();

    // set the base gid that we bound to this heap
    p->set_gid(header.response_lower_gid);

    // push the heap onto the OSHL
    response_heap_type::get_heap().add_heap(p);

    // bind range of GIDs to head addresses
    agas_client.bind_range(
        header.response_lower_gid
      , response_heap_type::block_type::heap_step
      , p->get_address()
      , response_heap_type::block_type::heap_size);

    // set up the future pools
    naming::resolver_client::locality_promise_pool_type& locality_promise_pool
        = agas_client.hosted->locality_promise_pool_;

    util::runtime_configuration const& ini_ = rt.get_config();

    const std::size_t pool_size = ini_.get_agas_promise_pool_size();

    for (std::size_t i = 0; i < pool_size; ++i)
    {
        locality_promise_pool.enqueue(
            new lcos::packaged_action<server::locality_namespace::service_action>);
    }
}
// }}}

void big_boot_barrier::spin()
{
    boost::mutex::scoped_lock lock(mtx);
    while (connected)
        cond.wait(lock);
}

big_boot_barrier::big_boot_barrier(
    parcelset::parcelport& pp_
  , util::runtime_configuration const& ini_
  , runtime_mode runtime_type_
):
    pp(pp_)
  , service_type(ini_.get_agas_service_mode())
  , runtime_type(runtime_type_)
  , bootstrap_agas(ini_.get_agas_locality())
  , cond()
  , mtx()
  , connected( (service_mode_bootstrap == service_type)
             ? ( ini_.get_num_localities()
               ? (ini_.get_num_localities() - 1)
               : 0)
             : 1)
  , thunks(16)
{
    pp_.register_event_handler(&early_parcel_sink);
}

void big_boot_barrier::apply(
    boost::uint32_t locality_id
  , naming::address const& addr
  , actions::base_action* act
) { // {{{
    parcelset::parcel p(naming::get_gid_from_locality_id(locality_id), addr, act);
    pp.send_early_parcel(p);
} // }}}

void big_boot_barrier::wait(void* primary_ns_server)
{ // {{{
    if (service_mode_bootstrap == service_type)
        spin();

    else
    {
        BOOST_ASSERT(0 != primary_ns_server);

        runtime& rt = get_runtime();
        boost::uint32_t num_threads = boost::lexical_cast<boost::uint32_t>(
            rt.get_config().get_entry("hpx.os_threads", boost::uint32_t(1)));

        // hosted, console
        if (runtime_mode_console == runtime_type)
        {
            // We need to contact the bootstrap AGAS node, and then wait
            // for it to signal us. We do this by executing register_console
            // on the bootstrap AGAS node, and sleeping on this node. We'll
            // be woken up by notify_console.

            apply(0, bootstrap_agas,
                new actions::transfer_action<register_console_action>(
                    registration_header(
                        rt.here()
                      , 20*HPX_INITIAL_GID_RANGE
                      , response_heap_type::block_type::heap_step
                      , rt.get_runtime_support_lva()
                      , rt.get_memory_lva()
                      , reinterpret_cast<boost::uint64_t>(primary_ns_server)
                      , num_threads)
                ));
            spin();
        }

        // hosted, worker or connected
        else
        {
            // we need to contact the bootstrap AGAS node, and then wait
            // for it to signal us.

            apply(0, bootstrap_agas,
                new actions::transfer_action<register_worker_action>(
                    registration_header(
                        rt.here()
                      , 20*HPX_INITIAL_GID_RANGE
                      , response_heap_type::block_type::heap_step
                      , rt.get_runtime_support_lva()
                      , rt.get_memory_lva()
                      , reinterpret_cast<boost::uint64_t>(primary_ns_server)
                      , num_threads)
                ));
            spin();
        }
    }
} // }}}

void big_boot_barrier::notify()
{
    boost::mutex::scoped_lock lk(mtx, boost::adopt_lock);
    --connected;
    cond.notify_all();
}

// This is triggered in runtime_impl::start, after the early action handler
// has been replaced by the parcelhandler. We have to delay the notifications
// until this point so that the AGAS locality can come up.
void big_boot_barrier::trigger()
{
    if (service_mode_bootstrap == service_type)
    {
        HPX_STD_FUNCTION<void()>* p;

        while (thunks.dequeue(p))
            (*p)();
    }
}

///////////////////////////////////////////////////////////////////////////////
struct bbb_tag;

void create_big_boot_barrier(
    parcelset::parcelport& pp_
  , util::runtime_configuration const& ini_
  , runtime_mode runtime_type_
) {
    util::reinitializable_static<boost::shared_ptr<big_boot_barrier>, bbb_tag> bbb;
    if (bbb.get())
    {
        HPX_THROW_EXCEPTION(internal_server_error,
            "create_big_boot_barrier",
            "create_big_boot_barrier was called more than once");
    }
    bbb.get().reset(new big_boot_barrier(pp_, ini_, runtime_type_));
}

void destroy_big_boot_barrier()
{
    util::reinitializable_static<boost::shared_ptr<big_boot_barrier>, bbb_tag> bbb;
    if (!bbb.get())
    {
        HPX_THROW_EXCEPTION(internal_server_error,
            "destroy_big_boot_barrier",
            "big_boot_barrier has not been created yet");
    }
    bbb.get().reset();
}

big_boot_barrier& get_big_boot_barrier()
{
    util::reinitializable_static<boost::shared_ptr<big_boot_barrier>, bbb_tag> bbb;
    if (!bbb.get())
    {
        HPX_THROW_EXCEPTION(internal_server_error,
            "get_big_boot_barrier",
            "big_boot_barrier has not been created yet");
    }
    return *(bbb.get());
}

}}

