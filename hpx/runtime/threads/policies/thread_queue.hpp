//  Copyright (c) 2007-2017 Hartmut Kaiser
//  Copyright (c) 2018      Thomas Heller
//  Copyright (c) 2011      Bryce Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_THREADMANAGER_THREAD_QUEUE_AUG_25_2009_0132PM)
#define HPX_THREADMANAGER_THREAD_QUEUE_AUG_25_2009_0132PM

#include <hpx/config.hpp>
#include <hpx/compat/mutex.hpp>
#include <hpx/error_code.hpp>
#include <hpx/runtime/config_entry.hpp>
#include <hpx/runtime/threads/policies/lockfree_queue_backends.hpp>
#include <hpx/runtime/threads/policies/queue_helpers.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/runtime/threads/thread_stack.hpp>
#include <hpx/throw_exception.hpp>
#include <hpx/util/assert.hpp>
#include <hpx/util/block_profiler.hpp>
#include <hpx/util/function.hpp>
#include <hpx/util/get_and_reset_value.hpp>
#include <hpx/util/high_resolution_clock.hpp>
#include <hpx/util/unlock_guard.hpp>

#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
#   include <hpx/util/tick_counter.hpp>
#endif

#include <boost/lexical_cast.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies
{
#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
    ///////////////////////////////////////////////////////////////////////////
    // We control whether to collect queue wait times using this global bool.
    // It will be set by any of the related performance counters. Once set it
    // stays set, thus no race conditions will occur.
    extern HPX_EXPORT bool maintain_queue_wait_times;
#endif
#ifdef HPX_HAVE_THREAD_MINIMAL_DEADLOCK_DETECTION
    ///////////////////////////////////////////////////////////////////////////
    // We globally control whether to do minimal deadlock detection using this
    // global bool variable. It will be set once by the runtime configuration
    // startup code
    extern bool minimal_deadlock_detection;
#endif

    namespace detail
    {
        inline int get_min_tasks_to_steal_pending()
        {
            static int min_tasks_to_steal_pending =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.min_tasks_to_steal_pending", "0"));
            return min_tasks_to_steal_pending;
        }

        inline int get_min_tasks_to_steal_staged()
        {
            static int min_tasks_to_steal_staged =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.min_tasks_to_steal_staged", "10"));
            return min_tasks_to_steal_staged;
        }

        inline int get_min_add_new_count()
        {
            static int min_add_new_count =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.min_add_new_count", "10"));
            return min_add_new_count;
        }

        inline int get_max_add_new_count()
        {
            static int max_add_new_count =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.max_add_new_count", "10"));
            return max_add_new_count;
        }

        inline int get_max_delete_count()
        {
            static int max_delete_count =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.max_delete_count", "1000"));
            return max_delete_count;
        }

        inline int get_max_terminated_threads()
        {
            static int max_terminated_threads =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.max_terminated_threads",
                    std::to_string(HPX_SCHEDULER_MAX_TERMINATED_THREADS)));
            return max_terminated_threads;
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    // // Queue back-end interface:
    //
    // template <typename T>
    // struct queue_backend
    // {
    //     typedef ... container_type;
    //     typedef ... value_type;
    //     typedef ... reference;
    //     typedef ... const_reference;
    //     typedef ... size_type;
    //
    //     queue_backend(
    //         size_type initial_size = ...
    //       , size_type num_thread = ...
    //         );
    //
    //     bool push(const_reference val);
    //
    //     bool pop(reference val, bool steal = true);
    //
    //     bool empty();
    // };
    //
    // struct queue_policy
    // {
    //     template <typename T>
    //     struct apply
    //     {
    //         typedef ... type;
    //     };
    // };
    template <typename Mutex = compat::mutex,
        typename PendingQueuing = lockfree_lifo,
        typename StagedQueuing = lockfree_lifo,
        typename TerminatedQueuing = lockfree_fifo>
    class thread_queue
    {
    private:
        // we use a simple mutex to protect the data members for now
        typedef Mutex mutex_type;

        // don't steal if less than this amount of tasks are left
        int const min_tasks_to_steal_pending;
        int const min_tasks_to_steal_staged;

        // create at least this amount of threads from tasks
        int const min_add_new_count;

        // create not more than this amount of threads from tasks
        int const max_add_new_count;

        // number of terminated threads to discard
        int const max_delete_count;

        // number of terminated threads to collect before cleaning them up
//         int const max_terminated_threads;

        // this is the type of a map holding all threads (except depleted ones)
//         typedef std::unordered_set<thread_id_type> thread_map_type;

        typedef thread_stack work_items_type;
        typedef thread_stack heap_items_type;

    protected:
        void create_thread_object(threads::thread_id_type& thrd_id,
            threads::thread_init_data& data, thread_state_enum state)
        {
            HPX_ASSERT(data.stacksize != 0);

            std::ptrdiff_t stacksize = data.stacksize;

            heap_items_type* heap = nullptr;

            if (stacksize == get_stack_size(thread_stacksize_small))
            {
                heap = &thread_heap_small_;
            }
            else if (stacksize == get_stack_size(thread_stacksize_medium))
            {
                heap = &thread_heap_medium_;
            }
            else if (stacksize == get_stack_size(thread_stacksize_large))
            {
                heap = &thread_heap_large_;
            }
            else if (stacksize == get_stack_size(thread_stacksize_huge))
            {
                heap = &thread_heap_huge_;
            }
            else {
                switch(stacksize) {
                case thread_stacksize_small:
                    heap = &thread_heap_small_;
                    break;

                case thread_stacksize_medium:
                    heap = &thread_heap_medium_;
                    break;

                case thread_stacksize_large:
                    heap = &thread_heap_large_;
                    break;

                case thread_stacksize_huge:
                    heap = &thread_heap_huge_;
                    break;

                default:
                    break;
                }
            }
            HPX_ASSERT(heap);

            if (state == pending_do_not_schedule || state == pending_boost)
            {
                state = pending;
            }

            thread_data* thrd = heap->pop();
            // Try to pop an item from the associated heap...
            if (thrd)
            {
                // rebind it...
                thrd->rebind(data, state);
            }
            else
            {
                // Allocate a new thread object.
                thrd = threads::thread_data::create(data, this, state);
            }

            thrd_id = thrd;
        }

        ///////////////////////////////////////////////////////////////////////
    public:
        void recycle_thread(thread_id_type thrd)
        {
            std::ptrdiff_t stacksize = thrd->get_stack_size();
            if (stacksize == get_stack_size(thread_stacksize_small))
            {
                thread_heap_small_.push(thrd);
            }
            else if (stacksize == get_stack_size(thread_stacksize_medium))
            {
                thread_heap_medium_.push(thrd);
            }
            else if (stacksize == get_stack_size(thread_stacksize_large))
            {
                thread_heap_large_.push(thrd);
            }
            else if (stacksize == get_stack_size(thread_stacksize_huge))
            {
                thread_heap_huge_.push(thrd);
            }
            else
            {
                switch(stacksize) {
                case thread_stacksize_small:
                    thread_heap_small_.push(thrd);
                    break;

                case thread_stacksize_medium:
                    thread_heap_medium_.push(thrd);
                    break;

                case thread_stacksize_large:
                    thread_heap_large_.push(thrd);
                    break;

                case thread_stacksize_huge:
                    thread_heap_huge_.push(thrd);
                    break;

                default:
                    HPX_ASSERT(false);
                    break;
                }
            }
        }

        /// This function makes sure all threads which are marked for deletion
        /// (state is terminated) are properly destroyed.
        ///
        /// This returns 'true' if there are no more terminated threads waiting
        /// to be deleted.
        bool cleanup_terminated_locked(bool delete_all = false)
        {
            return true;
        }

    public:
        bool cleanup_terminated(bool delete_all = false)
        {
            return true;
        }

        // The maximum number of active threads this thread manager should
        // create. This number will be a constraint only as long as the work
        // items queue is not empty. Otherwise the number of active threads
        // will be incremented in steps equal to the \a min_add_new_count
        // specified above.
        enum { max_thread_count = 1000 };

        thread_queue(std::size_t queue_num = std::size_t(-1),
                std::size_t max_count = max_thread_count)
          : min_tasks_to_steal_pending(detail::get_min_tasks_to_steal_pending()),
            min_tasks_to_steal_staged(detail::get_min_tasks_to_steal_staged()),
            min_add_new_count(detail::get_min_add_new_count()),
            max_add_new_count(detail::get_max_add_new_count()),
            max_delete_count(detail::get_max_delete_count()),
            thread_map_count_(0),
            work_items_count_(0),
#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
            work_items_wait_(0),
            work_items_wait_count_(0),
#endif
            max_count_((0 == max_count)
                      ? static_cast<std::size_t>(max_thread_count)
                      : max_count),
#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
            add_new_time_(0),
#endif
#ifdef HPX_HAVE_THREAD_STEALING_COUNTS
            pending_misses_(0),
            pending_accesses_(0),
            stolen_from_pending_(0),
            stolen_from_staged_(0),
            stolen_to_pending_(0),
            stolen_to_staged_(0),
#endif
            add_new_logger_("thread_queue::add_new")
        {}

        ~thread_queue()
        {
            while (true)
            {
                thread_data* thrd = thread_heap_small_.pop();
                if (!thrd) break;
                delete thrd;
            }
            while (true)
            {
                thread_data* thrd = thread_heap_medium_.pop();
                if (!thrd) break;
                delete thrd;
            }
            while (true)
            {
                thread_data* thrd = thread_heap_large_.pop();
                if (!thrd) break;
                delete thrd;
            }
            while (true)
            {
                thread_data* thrd = thread_heap_huge_.pop();
                if (!thrd) break;
                delete thrd;
            }
        }

        void set_max_count(std::size_t max_count = max_thread_count)
        {
            max_count_ = (0 == max_count) ? max_thread_count : max_count; //-V105
        }

#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
        std::uint64_t get_creation_time(bool reset)
        {
            return util::get_and_reset_value(add_new_time_, reset);
        }

        std::uint64_t get_cleanup_time(bool reset)
        {
            return 0;//util::get_and_reset_value(cleanup_terminated_time_, reset);
        }
#endif

        ///////////////////////////////////////////////////////////////////////
        // This returns the current length of the queues (work items and new items)
        std::int64_t get_queue_length() const
        {
            return work_items_count_;
        }

        // This returns the current length of the pending queue
        std::int64_t get_pending_queue_length() const
        {
            return work_items_count_;
        }

        // This returns the current length of the staged queue
        std::int64_t get_staged_queue_length(
            std::memory_order order = std::memory_order_seq_cst) const
        {
            return 0;
        }

#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
        std::uint64_t get_average_task_wait_time() const
        {
            return 0;
        }

        std::uint64_t get_average_thread_wait_time() const
        {
            std::uint64_t count = work_items_wait_count_;
            if (count == 0)
                return 0;
            return work_items_wait_ / count;
        }
#endif

#ifdef HPX_HAVE_THREAD_STEALING_COUNTS
        std::int64_t get_num_pending_misses(bool reset)
        {
            return util::get_and_reset_value(pending_misses_, reset);
        }

        void increment_num_pending_misses(std::size_t num = 1)
        {
            pending_misses_ += num;
        }

        std::int64_t get_num_pending_accesses(bool reset)
        {
            return util::get_and_reset_value(pending_accesses_, reset);
        }

        void increment_num_pending_accesses(std::size_t num = 1)
        {
            pending_accesses_ += num;
        }

        std::int64_t get_num_stolen_from_pending(bool reset)
        {
            return util::get_and_reset_value(stolen_from_pending_, reset);
        }

        void increment_num_stolen_from_pending(std::size_t num = 1)
        {
            stolen_from_pending_ += num;
        }

        std::int64_t get_num_stolen_from_staged(bool reset)
        {
            return util::get_and_reset_value(stolen_from_staged_, reset);
        }

        void increment_num_stolen_from_staged(std::size_t num = 1)
        {
            stolen_from_staged_ += num;
        }

        std::int64_t get_num_stolen_to_pending(bool reset)
        {
            return util::get_and_reset_value(stolen_to_pending_, reset);
        }

        void increment_num_stolen_to_pending(std::size_t num = 1)
        {
            stolen_to_pending_ += num;
        }

        std::int64_t get_num_stolen_to_staged(bool reset)
        {
            return util::get_and_reset_value(stolen_to_staged_, reset);
        }

        void increment_num_stolen_to_staged(std::size_t num = 1)
        {
            stolen_to_staged_ += num;
        }
#else
        void increment_num_pending_misses(std::size_t num = 1) {}
        void increment_num_pending_accesses(std::size_t num = 1) {}
        void increment_num_stolen_from_pending(std::size_t num = 1) {}
        void increment_num_stolen_from_staged(std::size_t num = 1) {}
        void increment_num_stolen_to_pending(std::size_t num = 1) {}
        void increment_num_stolen_to_staged(std::size_t num = 1) {}
#endif

        ///////////////////////////////////////////////////////////////////////
        // create a new thread and schedule it if the initial state is equal to
        // pending
        void create_thread(thread_init_data& data, thread_id_type* id,
            thread_state_enum initial_state, bool run_now, error_code& ec)
        {
            // thread has not been created yet
            if (id) *id = invalid_thread_id;

//             if (run_now)
            {
                threads::thread_id_type thrd = nullptr;

                // The mutex can not be locked while a new thread is getting
                // created, as it might have that the current HPX thread gets
                // suspended.
                create_thread_object(thrd, data, initial_state);

                ++thread_map_count_;

                // this thread has to be in the map now
                HPX_ASSERT(thrd->get_queue() == this);

                // push the new thread in the pending queue thread
                if (initial_state == pending)
                    schedule_thread(thrd);

                // return the thread_id of the newly created thread
                if (id) *id = std::move(thrd);

                if (&ec != &throws)
                    ec = make_success_code();
                return;
            }

            if (&ec != &throws)
                ec = make_success_code();
        }

        void move_work_items_from(thread_queue *src, std::int64_t count)
        {
//             thread_description* trd;
//             while (src->work_items_.pop(trd))
//             {
//                 --src->work_items_count_;
//
// #ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
//                 if (maintain_queue_wait_times) {
//                     std::uint64_t now = util::high_resolution_clock::now();
//                     src->work_items_wait_ += now - util::get<1>(*trd);
//                     ++src->work_items_wait_count_;
//                     util::get<1>(*trd) = now;
//                 }
// #endif
//
//                 bool finished = count == ++work_items_count_;
//                 work_items_.push(trd);
//                 if (finished)
//                     break;
//             }
        }

        void move_task_items_from(thread_queue *src,
            std::int64_t count)
        {
        }

        /// Return the next thread to be executed, return false if none is
        /// available
        bool get_next_thread(threads::thread_data*& thrd, bool steal = false) HPX_HOT
        {
            thrd = work_items_.pop(steal);
            if (thrd)
            {
#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
                if (maintain_queue_wait_times) {
                    work_items_wait_ += util::high_resolution_clock::now() -
                        thrd->list_.time_;
                    ++work_items_wait_count_;
                }
#endif
                thread_queue* queue
                    = reinterpret_cast<thread_queue*>(thrd->get_queue());
                --queue->work_items_count_;
                return true;
            }
            return false;
        }

        /// Schedule the passed thread
        void schedule_thread(threads::thread_data* thrd, bool other_end = false)
        {
            thread_queue* queue
                = reinterpret_cast<thread_queue*>(thrd->get_queue());
            ++queue->work_items_count_;
#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
            thrd->list_.time_ = util::high_resolution_clock::now();
#endif
            work_items_.push(thrd);
        }

        /// Destroy the passed thread as it has been terminated
        bool destroy_thread(threads::thread_data* thrd, std::int64_t& busy_count)
        {
            HPX_ASSERT(thrd->get_queue() == this);
            recycle_thread(thrd);
            --thread_map_count_;

            return true;
        }

        ///////////////////////////////////////////////////////////////////////
        /// Return the number of existing threads with the given state.
        std::int64_t get_thread_count(thread_state_enum state = unknown) const
        {
            if (terminated == state)
                return 0;//terminated_items_count_;

            if (staged == state)
                return 0;

            if (unknown == state)
                return thread_map_count_;

            if (pending == state)
                return work_items_count_;

            if (suspended == state)
                return thread_map_count_ - work_items_count_;

            return 0;
// FIXME
//             // acquire lock only if absolutely necessary
//             std::lock_guard<mutex_type> lk(mtx_);
//
//             std::int64_t num_threads = 0;
//             thread_map_type::const_iterator end = thread_map_.end();
//             for (thread_map_type::const_iterator it = thread_map_.begin();
//                  it != end; ++it)
//             {
//                 if ((*it)->get_state().state() == state)
//                     ++num_threads;
//             }
//             return num_threads;
        }

        ///////////////////////////////////////////////////////////////////////
        void abort_all_suspended_threads()
        {

// FIXME
//             std::lock_guard<mutex_type> lk(mtx_);
//             thread_map_type::iterator end =  thread_map_.end();
//             for (thread_map_type::iterator it = thread_map_.begin();
//                  it != end; ++it)
//             {
//                 if ((*it)->get_state().state() == suspended)
//                 {
//                     (*it)->set_state(pending, wait_abort);
//                     schedule_thread((*it));
//                 }
//             }
        }

        bool enumerate_threads(
            util::function_nonser<bool(thread_id_type)> const& f,
            thread_state_enum state = unknown) const
        {
            std::uint64_t count = thread_map_count_;
            if (state == terminated)
            {
                count = 0;//terminated_items_count_;
            }
            else if (state == staged)
            {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "thread_queue::iterate_threads",
                    "can't iterate over thread ids of staged threads");
                return false;
            }

            std::vector<thread_id_type> ids;
            ids.reserve(static_cast<std::size_t>(count));

// FIXME
//             if (state == unknown)
//             {
//                 std::lock_guard<mutex_type> lk(mtx_);
//                 thread_map_type::const_iterator end =  thread_map_.end();
//                 for (thread_map_type::const_iterator it = thread_map_.begin();
//                      it != end; ++it)
//                 {
//                     ids.push_back(*it);
//                 }
//             }
//             else
//             {
//                 std::lock_guard<mutex_type> lk(mtx_);
//                 thread_map_type::const_iterator end =  thread_map_.end();
//                 for (thread_map_type::const_iterator it = thread_map_.begin();
//                      it != end; ++it)
//                 {
//                     if ((*it)->get_state().state() == state)
//                         ids.push_back(*it);
//                 }
//             }

            // now invoke callback function for all matching threads
            for (thread_id_type const& id : ids)
            {
                if (!f(id))
                    return false;       // stop iteration
            }

            return true;
        }

        ///////////////////////////////////////////////////////////////////////
        bool dump_suspended_threads(std::size_t num_thread
          , std::int64_t& idle_loop_count, bool running)
        {
#ifndef HPX_HAVE_THREAD_MINIMAL_DEADLOCK_DETECTION
            return false;
#else
            if (minimal_deadlock_detection) {
                return false;
//                 std::lock_guard<mutex_type> lk(mtx_);
//                 return detail::dump_suspended_threads(num_thread, thread_map_
//                   , idle_loop_count, running);
            }
            return false;
#endif
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t num_thread) {}
        void on_stop_thread(std::size_t num_thread) {}
        void on_error(std::size_t num_thread, std::exception_ptr const& e) {}

        std::unique_lock<mutex_type> lock()
        {
            return std::unique_lock<mutex_type>(mtx_);
        }

        std::unique_lock<mutex_type> try_lock()
        {
            return std::unique_lock<mutex_type>(mtx_, std::try_to_lock);
        }

    private:
        mutable mutex_type mtx_;                    ///< mutex protecting the members

//         thread_map_type thread_map_;
        ///< mapping of thread id's to HPX-threads
        std::atomic<std::int64_t> thread_map_count_;
        ///< overall count of work items

        work_items_type work_items_;
        ///< list of active work items
        std::atomic<std::int64_t> work_items_count_;
        ///< count of active work items

#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
        std::atomic<std::int64_t> work_items_wait_;
        ///< overall wait time of work items
        std::atomic<std::int64_t> work_items_wait_count_;
        ///< overall number of work items in queue
#endif
        std::size_t max_count_;
        ///< maximum number of existing HPX-threads

        heap_items_type thread_heap_small_;
        heap_items_type thread_heap_medium_;
        heap_items_type thread_heap_large_;
        heap_items_type thread_heap_huge_;

#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
        std::uint64_t add_new_time_;
#endif

#ifdef HPX_HAVE_THREAD_STEALING_COUNTS
        // # of times our associated worker-thread couldn't find work in work_items
        std::atomic<std::int64_t> pending_misses_;

        // # of times our associated worker-thread looked for work in work_items
        std::atomic<std::int64_t> pending_accesses_;

        std::atomic<std::int64_t> stolen_from_pending_;
        ///< count of work_items stolen from this queue
        std::atomic<std::int64_t> stolen_from_staged_;
        ///< count of new_tasks stolen from this queue
        std::atomic<std::int64_t> stolen_to_pending_;
        ///< count of work_items stolen to this queue from other queues
        std::atomic<std::int64_t> stolen_to_staged_;
        ///< count of new_tasks stolen to this queue from other queues
#endif

        util::block_profiler<add_new_tag> add_new_logger_;
    };
}}}

#endif

