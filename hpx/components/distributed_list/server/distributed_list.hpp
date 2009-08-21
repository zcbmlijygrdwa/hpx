//  Copyright (c) 2009-2010 Dylan Stark
// 
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_COMPONENTS_SERVER_DISTRIBUTED_LIST_AUG_14_2009_1129AM)
#define HPX_COMPONENTS_SERVER_DISTRIBUTED_LIST_AUG_14_2009_1129AM

#include <iostream>

#include <hpx/hpx_fwd.hpp>
#include <hpx/runtime/applier/applier.hpp>
#include <hpx/runtime/threads/thread.hpp>
#include <hpx/runtime/components/component_type.hpp>
#include <hpx/runtime/components/server/simple_component_base.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace components { namespace server
{
    ///////////////////////////////////////////////////////////////////////////
    /// The distributed_list is an HPX component.
    ///
    template <typename List>
    class HPX_COMPONENT_EXPORT distributed_list
      : public simple_component_base<distributed_list<List> >
    {
    private:
        typedef simple_component_base<distributed_list> base_type;
        
    public:
        distributed_list();
        
        typedef hpx::components::server::distributed_list<List> wrapping_type;
        
        enum actions
        {
            distributed_list_get_local = 0
        };
        
        ///////////////////////////////////////////////////////////////////////
        // exposed functionality of this component
        typedef List list_type;

        naming::id_type get_local(naming::id_type);

        ///////////////////////////////////////////////////////////////////////
        // Each of the exposed functions needs to be encapsulated into an action
        // type, allowing to generate all required boilerplate code for threads,
        // serialization, etc.
        typedef hpx::actions::result_action1<
            distributed_list, naming::id_type, distributed_list_get_local,
            naming::id_type,
            &distributed_list::get_local
        > get_local_action;

    private:
        // Map from locale to its local_list
        std::map<naming::id_type,naming::id_type> map_;
    };

}}}

#endif
