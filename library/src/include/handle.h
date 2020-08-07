/* ************************************************************************
 * Copyright 2016-2020 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#ifndef _ROCBLAS_HANDLE_H_
#define _ROCBLAS_HANDLE_H_

#include "rocblas.h"
#include "rocblas_ostream.hpp"
#include "utility.h"
#include <array>
#include <cstddef>
#include <hip/hip_runtime.h>
#include <memory>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>

/*******************************************************************************
 * \brief rocblas_handle is a structure holding the rocblas library context.
 * It must be initialized using rocblas_create_handle() and the returned handle mus
 * It should be destroyed at the end using rocblas_destroy_handle().
 * Exactly like CUBLAS, ROCBLAS only uses one stream for one API routine
 ******************************************************************************/
struct _rocblas_handle
{
private:
    // Emulate C++17 std::conjunction
    template <class...>
    struct conjunction : std::true_type
    {
    };
    template <class T, class... Ts>
    struct conjunction<T, Ts...> : std::integral_constant<bool, T{} && conjunction<Ts...>{}>
    {
    };

public:
    _rocblas_handle();
    ~_rocblas_handle();

    int             device;
    hipDeviceProp_t device_properties;

    // rocblas by default take the system default stream 0 users cannot create
    hipStream_t rocblas_stream = 0;

    // hipEvent_t pointers (for internal use only)
    hipEvent_t startEvent = nullptr;
    hipEvent_t stopEvent  = nullptr;

    // default pointer_mode is on host
    rocblas_pointer_mode pointer_mode = rocblas_pointer_mode_host;

    // default logging_mode is no logging
    rocblas_layer_mode layer_mode = rocblas_layer_mode_none;

    // default atomics mode allows atomic operations
    rocblas_atomics_mode atomics_mode = rocblas_atomics_allowed;

    // logging streams
    std::unique_ptr<rocblas_ostream> log_trace_os;
    std::unique_ptr<rocblas_ostream> log_bench_os;
    std::unique_ptr<rocblas_ostream> log_profile_os;
    void                             init_logging();

    // C interfaces for manipulating device memory
    friend rocblas_status(::rocblas_start_device_memory_size_query)(_rocblas_handle*);
    friend rocblas_status(::rocblas_stop_device_memory_size_query)(_rocblas_handle*, size_t*);
    friend rocblas_status(::rocblas_get_device_memory_size)(_rocblas_handle*, size_t*);
    friend rocblas_status(::rocblas_set_device_memory_size)(_rocblas_handle*, size_t);
    friend bool(::rocblas_is_managing_device_memory)(_rocblas_handle*);

    // Returns whether the current kernel call is a device memory size query
    bool is_device_memory_size_query() const
    {
        return device_memory_size_query;
    }

    // Sets the optimal size(s) of device memory for a kernel call
    // Maximum size is accumulated in device_memory_query_size
    // Returns rocblas_status_size_increased or rocblas_status_size_unchanged
    template <typename... Ss,
              std::enable_if_t<sizeof...(Ss) && conjunction<std::is_constructible<size_t, Ss>...>{},
                               int> = 0>
    rocblas_status set_optimal_device_memory_size(Ss... sizes)
    {
        if(!device_memory_size_query)
            return rocblas_status_internal_error;

#if __cplusplus >= 201703L
        // Compute the total size, rounding up each size to multiples of MIN_CHUNK_SIZE
        size_t total = (roundup_device_memory_size(size_t(sizes)) + ...);
#else
        size_t total = 0;
        auto   dummy = {total += roundup_device_memory_size(size_t(sizes))...};
#endif

        if(total > device_memory_query_size)
        {
            device_memory_query_size = total;
            return rocblas_status_size_increased;
        }
        return rocblas_status_size_unchanged;
    }

    // Allocate one or more sizes
    template <typename... Ss,
              std::enable_if_t<sizeof...(Ss) && conjunction<std::is_constructible<size_t, Ss>...>{},
                               int> = 0>
    auto device_malloc(Ss... sizes)
    {
        return _device_malloc<sizeof...(Ss)>(this, size_t(sizes)...);
    }

    // Temporarily change pointer mode, returning object which restores old mode when destroyed
    auto push_pointer_mode(rocblas_pointer_mode mode)
    {
        return _pushed_state<rocblas_pointer_mode>(pointer_mode, mode);
    }

    // Whether to use any_order scheduling in Tensile calls
    bool any_order = false;

    // Temporarily change any_order flag
    auto push_any_order(bool new_any_order)
    {
        return _pushed_state<bool>(any_order, new_any_order);
    }

private:
    // device memory work buffer
    static constexpr size_t DEFAULT_DEVICE_MEMORY_SIZE = 4 * 1048576;
    static constexpr size_t MIN_CHUNK_SIZE             = 64;

    // Round up size to the nearest MIN_CHUNK_SIZE
    static constexpr size_t roundup_device_memory_size(size_t size)
    {
        static_assert(MIN_CHUNK_SIZE > 0 && !(MIN_CHUNK_SIZE & (MIN_CHUNK_SIZE - 1)),
                      "MIN_CHUNK_SIZE must be a power of two");
        return ((size - 1) | (MIN_CHUNK_SIZE - 1)) + 1;
    }

    // Variables holding state of device memory allocation
    size_t device_memory_size = 0;
    size_t device_memory_query_size;
    void*  device_memory                    = nullptr;
    bool   device_memory_is_rocblas_managed = false;
    bool   device_memory_in_use             = false;
    bool   device_memory_size_query         = false;

    // Helper for device memory allocator
    void* device_allocator(size_t size);

    // Opaque smart allocator class to perform device memory allocations
    template <size_t N = 1>
    class _device_malloc
    {
    protected:
        rocblas_handle       handle;
        bool                 success = true;
        size_t               total   = 0;
        std::array<void*, N> pointers; // Important: must come after handle, success and total

        // Allocate one or more pointers to buffers of different sizes
        template <typename... Ss>
        decltype(pointers) allocate_pointers(Ss... sizes)
        {
            // This creates a list of partial sums which are the offsets of each of the allocated
            // arrays. The sizes are rounded up to the next multiple of MIN_CHUNK_SIZE.
            // total contains the total of all sizes at the end of the calculation of offsets.
            size_t old, offsets[] = {(old = total, total += roundup_device_memory_size(sizes), old)...};

            // If total size is 0, return an array of nullptr's, but leave it marked as successful
            if(!total)
                return {};

            // We allocate the total amount needed. This is a constant-time operation if the space is
            // already available, or if an explicit size has been allocated.
            void* ptr = handle->device_allocator(total);

            // If allocation failed, return an array of nullptr's and mark allocation as failed
            if(!ptr)
            {
                success = false;
                return {};
            }

            // An array of pointers to all of the allocated arrays is formed.
            // If a size is 0, the corresponding pointer is nullptr
            size_t i = 0;
            return {!sizes ? i++, nullptr : (char*)ptr + offsets[i++]...};
        }

        // Create a tuple of references to the pointers, to be assigned to std::tie(...)
        template <size_t... Is>
        const auto tie_pointers(std::index_sequence<Is...>)
        {
            return std::tie(pointers[Is]...);
        }

    public:
        // Constructor
        template <typename... Ss>
        explicit _device_malloc(rocblas_handle handle, Ss... sizes)
            : handle(handle)
            , pointers(allocate_pointers(sizes...))
        {
        }

        // Conversion to bool to tell if the allocation succeeded
        explicit operator bool() const
        {
            return success;
        }

        // Conversion to std::tuple<void*&...> to be assigned to std::tie(ptr1, ptr2, ...)
        operator auto()
        {
            return tie_pointers(std::make_index_sequence<N>{});
        }

        // Conversion to any pointer type, but only if N == 1
        template <typename T, typename = std::enable_if_t<std::is_pointer<T>{} && N == 1>>
        operator T() const
        {
            return T(pointers[0]);
        }

        // Total size of allocation
        size_t total_size() const
        {
            return total;
        }

        // The destructor marks the device memory as no longer in use
        ~_device_malloc()
        {
            handle->device_memory_in_use = false;
        }

        _device_malloc(const _device_malloc&) = delete;
        _device_malloc(_device_malloc&&)      = default;
        _device_malloc& operator=(const _device_malloc&) = delete;
        _device_malloc& operator=(_device_malloc&&) = default;
    };

    // Class for temporarily modifying a state, restoring it on destruction
    template <typename STATE>
    class _pushed_state
    {
        STATE&      state;
        const STATE old_state;

    public:
        // Constructor
        _pushed_state(STATE& state, STATE new_state)
            : state(state)
            , old_state(state)
        {
            state = new_state;
        }

        // Temporary object implicitly converts to old state
        operator STATE() const
        {
            return old_state;
        }

        // Old state is restored on destruction
        ~_pushed_state()
        {
            state = old_state;
        }

        _pushed_state(const _pushed_state&) = delete;
        _pushed_state(_pushed_state&&)      = default;
        _pushed_state& operator=(const _pushed_state&) = delete;
        _pushed_state& operator=(_pushed_state&&) = delete;
    };

    // For HPA kernel calls, all available device memory is allocated and passed to Tensile
    struct _gsu_malloc : _device_malloc<>
    {
        _gsu_malloc(rocblas_handle handle)
            : _device_malloc<>(handle, handle->device_memory_size)
        {
            handle->gsu_workspace_size = *this ? total_size() : 0;
            handle->gsu_workspace      = *this;
        }

        ~_gsu_malloc()
        {
            handle->gsu_workspace_size = 0;
            handle->gsu_workspace      = nullptr;
        }

        _gsu_malloc(const _gsu_malloc&) = delete;
        _gsu_malloc(_gsu_malloc&&)      = default;
        _gsu_malloc& operator=(const _gsu_malloc&) = delete;
        _gsu_malloc& operator=(_gsu_malloc&&) = default;
    };

public:
    // gsu_malloc() returns a proxy object which manages GSU memory
    _gsu_malloc gsu_malloc()
    {
        return this;
    };

    size_t gsu_workspace_size = 0;
    void*  gsu_workspace      = nullptr;
};

// For functions which don't use temporary device memory, and won't be likely
// to use them in the future, the RETURN_ZERO_DEVICE_MEMORY_SIZE_IF_QUERIED(handle)
// macro can be used to return from a rocblas function with a requested size of 0.
#define RETURN_ZERO_DEVICE_MEMORY_SIZE_IF_QUERIED(h) \
    do                                               \
    {                                                \
        if((h)->is_device_memory_size_query())       \
            return rocblas_status_size_unchanged;    \
    } while(0)

// Warn about potentially unsafe and synchronizing uses of hipMalloc and hipFree
#define hipMalloc(ptr, size)                                                                     \
    _Pragma(                                                                                     \
        "GCC warning \"Direct use of hipMalloc in rocBLAS is deprecated; see CONTRIBUTING.md\"") \
        hipMalloc(ptr, size)
#define hipFree(ptr)                                                                               \
    _Pragma("GCC warning \"Direct use of hipFree in rocBLAS is deprecated; see CONTRIBUTING.md\"") \
        hipFree(ptr)

#endif
