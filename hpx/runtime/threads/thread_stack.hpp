//  Copyright (c)      2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// This code was inspired by this blog post:
// http://moodycamel.com/blog/2014/solving-the-aba-problem-for-lock-free-free-lists
// The original code example presented has been released under the simplified BSD
// license. Due to sufficient changes, we re-release this under the BSL
//
// The intent is to have a intrusive, lockfree list for thread_data items. All
// other solutions (Boost.Lockfree etc.) require additional memory for the node
// data structure. Since thread_data is supposed to be dynamically allocated by
// the scheduler, an additional dynamic allocation seems unnecessary...

#ifndef HPX_RUNTIME_THREADS_THREAD_STACK_HPP
#define HPX_RUNTIME_THREADS_THREAD_STACK_HPP

#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/util/assert.hpp>
#include <hpx/util/detail/yield_k.hpp>

#include <atomic>

namespace hpx { namespace threads
{
    struct thread_stack
    {
        thread_stack() : head_(nullptr)
        {}

        void push(thread_data* thrd)
        {
            if (thrd->list_.refcount_.fetch_add(should_be_on_freelist, std::memory_order_release) == 0)
            {
                add_knowing_refcount_is_zero(thrd);
            }
        }

        thread_data* pop(bool steal = false)
        {
            thread_data* head = head_.load(std::memory_order_acquire);
            for(std::size_t k = 0; head != nullptr; ++k)
            {
                thread_data* prev_head = head;
                std::uint32_t refcount = head->list_.refcount_.load(std::memory_order_relaxed);
                if ((refcount & refcount_mask) == 0 ||
                    !head->list_.refcount_.compare_exchange_strong(refcount, refcount + 1,
                        std::memory_order_acquire, std::memory_order_relaxed))
                {
                    hpx::util::detail::yield_k(k, "thread_stack::pop");
                    // If the above condition is false, someone else changed head_
//                     if (steal) return nullptr;
                    head = head_.load(std::memory_order_acquire);
                    continue;
                }

                thread_data* next = head->list_.next_.load(std::memory_order_relaxed);
                if (head_.compare_exchange_strong(head, next,
                    std::memory_order_acquire, std::memory_order_relaxed))
                {
                    HPX_ASSERT((head->list_.refcount_.load(std::memory_order_relaxed) &
                        should_be_on_freelist) == 0);

                    head->list_.refcount_.fetch_add(-2, std::memory_order_relaxed);

                    return head;
                }

                refcount = prev_head->list_.refcount_.fetch_add(-1, std::memory_order_acq_rel);
                if (refcount == should_be_on_freelist + 1)
                {
                    add_knowing_refcount_is_zero(prev_head);
                }
            }

            return nullptr;
        }

    private:
        void add_knowing_refcount_is_zero(thread_data *thrd)
        {
            thread_data* head = head_.load(std::memory_order_relaxed);
            for(std::size_t k = 0; true; ++k)
            {

                thrd->list_.next_.store(head, std::memory_order_relaxed);
                thrd->list_.refcount_.store(1, std::memory_order_release);

                if (!head_.compare_exchange_strong(head, thrd,
                    std::memory_order_release, std::memory_order_relaxed))
                {
                    if (thrd->list_.refcount_.fetch_add(should_be_on_freelist - 1,
                        std::memory_order_release) == 1)
                    {
                        hpx::util::detail::yield_k(k, "thread_stack::pop");
                        continue;
                    }
                }
                return;
            }
        }

        static constexpr std::uint32_t refcount_mask = 0x7FFFFFFF;
        static constexpr std::uint32_t should_be_on_freelist = 0x80000000;

        std::atomic<thread_data*> head_;
    };
}}

#endif
