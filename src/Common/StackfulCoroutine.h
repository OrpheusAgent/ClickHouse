#pragma once
/// BOOST_USE_ASAN, BOOST_USE_MSAN, BOOST_USE_TSAN and BOOST_USE_UCONTEXT are defined via CMake for sanitizer builds.
#include <base/defines.h>
#include <boost/context/fiber.hpp>
#include <map>

/// Class wrapper for boost::context::fiber.
/// It tracks current executing coroutine for thread and
/// supports storing coroutine-specific data
/// that will be destroyed on coroutine destructor.
class StackfulCoroutine
{
private:
    using Impl = boost::context::fiber;
    using CoroutinePtr = StackfulCoroutine *;
    template <typename T> friend class CoroutineLocal;

public:
    template <typename StackAlloc, typename Fn>
    StackfulCoroutine(StackAlloc && salloc, Fn && fn) : impl(std::allocator_arg_t(), std::forward<StackAlloc>(salloc), RoutineImpl<Fn>(std::forward<Fn>(fn)))
    {
    }

    StackfulCoroutine() = default;

    StackfulCoroutine(StackfulCoroutine && other) = default;
    StackfulCoroutine & operator=(StackfulCoroutine && other) = default;

    StackfulCoroutine(const StackfulCoroutine &) = delete;
    StackfulCoroutine & operator =(const StackfulCoroutine &) = delete;

    explicit operator bool() const
    {
        return impl.operator bool();
    }

    void resume()
    {
        /// Update information about current executing coroutine.
        CoroutinePtr & current_coroutine = getCurrentCoroutine();
        CoroutinePtr parent_coroutine = current_coroutine;
        current_coroutine = this;
        impl = std::move(impl).resume();
        /// Restore parent coroutine.
        current_coroutine = parent_coroutine;
    }

    static CoroutinePtr & getCurrentCoroutine()
    {
        thread_local static CoroutinePtr current_coroutine;
        return current_coroutine;
    }

private:
    template <typename Fn>
    struct RoutineImpl
    {
        struct SuspendCallback
        {
            Impl & impl;

            void operator()()
            {
                impl = std::move(impl).resume();
            }
        };

        explicit RoutineImpl(Fn && fn_) : fn(std::move(fn_))
        {
        }

        Impl operator()(Impl && sink)
        {
            SuspendCallback suspend_callback{sink};
            fn(suspend_callback);
            return std::move(sink);
        }

        Fn fn;
    };

    /// Special wrapper to store data in uniquer_ptr.
    struct DataWrapper
    {
        virtual ~DataWrapper() = default;
    };

    using DataPtr = std::unique_ptr<DataWrapper>;

    /// Get reference to coroutine-specific data by key
    /// (the pointer to the structure that uses this data).
    DataPtr & getLocalData(void * key)
    {
        return local_data[key];
    }

    Impl && release()
    {
        return std::move(impl);
    }

    Impl impl;
    std::map<void *, DataPtr> local_data;
};

/// Implementation for coroutine local variable.
/// If we are in coroutine, it returns coroutine local data,
/// otherwise it returns it's single field.
/// Coroutine local data is destroyed in StackfulCoroutine destructor.
/// Implementation is similar to boost::fiber::fiber_specific_ptr
/// (we cannot use it because we don't use boost::fiber API.
template <typename T>
class CoroutineLocal
{
public:
    T & operator*()
    {
        return get();
    }

    T * operator->()
    {
        return &get();
    }

private:
    struct DataWrapperImpl : public StackfulCoroutine::DataWrapper
    {
        T impl;
    };

    T & get()
    {
        StackfulCoroutine * current_coroutine = StackfulCoroutine::getCurrentCoroutine();
        if (!current_coroutine)
            return main_instance;

        StackfulCoroutine::DataPtr & ptr = current_coroutine->getLocalData(this);
        /// Initialize instance on first request.
        if (!ptr)
            ptr = std::make_unique<DataWrapperImpl>();

        return dynamic_cast<DataWrapperImpl *>(ptr.get())->impl;
    }

    T main_instance;
};
