//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

/// \file hpx/runtime/threads/thread_id_type.hpp

#ifndef HPX_THREADS_THREAD_ID_TYPE_HPP
#define HPX_THREADS_THREAD_ID_TYPE_HPP

#include <cstddef>
#include <functional>

namespace hpx { namespace threads
{
    class HPX_EXPORT thread_data;

    struct thread_id_type
    {
        constexpr thread_id_type()
          : thrd_(nullptr) {}
        constexpr thread_id_type(thread_data* thrd)
          : thrd_(thrd) {}

        constexpr thread_id_type(std::nullptr_t)
          : thrd_(nullptr) {}

        thread_id_type(thread_id_type const&) = default;
        thread_id_type& operator=(thread_id_type const&) = default;

        constexpr thread_id_type& operator=(thread_data* thrd)
        {
            thrd_ = thrd;

            return *this;
        }

        constexpr thread_id_type& operator=(std::nullptr_t)
        {
            thrd_ = nullptr;

            return *this;
        }

        thread_data* operator->() const
        {
            HPX_ASSERT(thrd_);
            return thrd_;
        }

        thread_data* operator->()
        {
            HPX_ASSERT(thrd_);
            return thrd_;
        }

        thread_data& operator*()
        {
            HPX_ASSERT(thrd_);
            return *thrd_;
        }

        thread_data const& operator*() const
        {
            HPX_ASSERT(thrd_);
            return *thrd_;
        }

        explicit constexpr operator bool() const
        {
            return nullptr != thrd_;
        }

        constexpr operator thread_data*()
        {
            return thrd_;
        }

        constexpr operator thread_data*() const
        {
            return thrd_;
        }

        thread_data* thrd_;
    };

    constexpr bool operator==(std::nullptr_t, thread_id_type const& rhs)
    {
        return nullptr == rhs.thrd_;
    }

    constexpr bool operator!=(std::nullptr_t, thread_id_type const& rhs)
    {
        return nullptr != rhs.thrd_;
    }

    constexpr bool operator==(thread_id_type const& rhs, std::nullptr_t)
    {
        return nullptr == rhs.thrd_;
    }

    constexpr bool operator!=(thread_id_type const& rhs, std::nullptr_t)
    {
        return nullptr != rhs.thrd_;
    }

    constexpr bool operator==(thread_id_type const& lhs, thread_id_type const& rhs)
    {
        return lhs.thrd_ == rhs.thrd_;
    }

    constexpr bool operator!=(thread_id_type const& lhs, thread_id_type const& rhs)
    {
        return lhs.thrd_ != rhs.thrd_;
    }

    constexpr bool operator<(thread_id_type const& lhs, thread_id_type const& rhs)
    {
        return std::less<void const*>{}(lhs.thrd_, rhs.thrd_);
    }

    constexpr bool operator>(thread_id_type const& lhs, thread_id_type const& rhs)
    {
        return std::less<void const*>{}(rhs.thrd_, lhs.thrd_);
    }

    constexpr bool operator<=(thread_id_type const& lhs, thread_id_type const& rhs)
    {
        return !(rhs > lhs);
    }

    constexpr bool operator>=(thread_id_type const& lhs, thread_id_type const& rhs)
    {
        return !(rhs < lhs);
    }
}}

namespace std
{
    template <>
    struct hash< ::hpx::threads::thread_id_type>
    {
        typedef ::hpx::threads::thread_id_type argument_type;
        typedef std::size_t result_type;

        std::size_t operator()(::hpx::threads::thread_id_type const& v) const noexcept
        {
            std::hash<const ::hpx::threads::thread_data*> hasher_;
            return hasher_(v.thrd_);
        }
    };
}

#endif
