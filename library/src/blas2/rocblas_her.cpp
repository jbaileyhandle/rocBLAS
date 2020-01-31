/* ************************************************************************
 * Copyright 2016-2020 Advanced Micro Devices, Inc.
 * ************************************************************************ */
#include "rocblas_her.hpp"
#include "logging.h"
#include "utility.h"

namespace
{
    template <typename>
    constexpr char rocblas_her_name[] = "unknown";
    template <>
    constexpr char rocblas_her_name<rocblas_float_complex>[] = "rocblas_cher";
    template <>
    constexpr char rocblas_her_name<rocblas_double_complex>[] = "rocblas_zher";

    template <typename T, typename U>
    rocblas_status rocblas_her_impl(rocblas_handle handle,
                                    rocblas_fill   uplo,
                                    rocblas_int    n,
                                    const U*       alpha,
                                    const T*       x,
                                    rocblas_int    incx,
                                    T*             A,
                                    rocblas_int    lda)
    {
        if(!handle)
            return rocblas_status_invalid_handle;

        RETURN_ZERO_DEVICE_MEMORY_SIZE_IF_QUERIED(handle);

        auto layer_mode = handle->layer_mode;
        if(layer_mode
           & (rocblas_layer_mode_log_trace | rocblas_layer_mode_log_bench
              | rocblas_layer_mode_log_profile))
        {
            auto uplo_letter = rocblas_fill_letter(uplo);

            if(handle->pointer_mode == rocblas_pointer_mode_host)
            {
                if(layer_mode & rocblas_layer_mode_log_trace)
                    log_trace(handle,
                              rocblas_her_name<T>,
                              uplo,
                              n,
                              log_trace_scalar_value(alpha),
                              x,
                              incx,
                              A,
                              lda);

                if(layer_mode & rocblas_layer_mode_log_bench)
                    log_bench(handle,
                              "./rocblas-bench -f her -r",
                              rocblas_precision_string<T>,
                              "--uplo",
                              uplo_letter,
                              "-n",
                              n,
                              LOG_BENCH_SCALAR_VALUE(alpha),
                              "--incx",
                              incx,
                              "--lda",
                              lda);
            }
            else
            {
                if(layer_mode & rocblas_layer_mode_log_trace)
                    log_trace(handle,
                              rocblas_her_name<T>,
                              uplo,
                              n,
                              log_trace_scalar_value(alpha),
                              x,
                              incx,
                              A,
                              lda);
            }

            if(layer_mode & rocblas_layer_mode_log_profile)
                log_profile(handle,
                            rocblas_her_name<T>,
                            "uplo",
                            uplo_letter,
                            "N",
                            n,
                            "incx",
                            incx,
                            "lda",
                            lda);
        }

        if(uplo != rocblas_fill_lower && uplo != rocblas_fill_upper)
            return rocblas_status_invalid_value;
        if(n < 0 || !incx || lda < n || lda < 1)
            return rocblas_status_invalid_size;
        if(!n)
            return rocblas_status_success;
        if(!x || !A || !alpha)
            return rocblas_status_invalid_pointer;

        static constexpr rocblas_int    offset_x = 0, offset_A = 0, batch_count = 1;
        static constexpr rocblas_stride stride_x = 0, stride_A = 0;
        return rocblas_her_template(handle,
                                    uplo,
                                    n,
                                    alpha,
                                    x,
                                    offset_x,
                                    incx,
                                    stride_x,
                                    A,
                                    lda,
                                    offset_A,
                                    stride_A,
                                    batch_count);
    }

}
/*
 * ===========================================================================
 *    C wrapper
 * ===========================================================================
 */

extern "C" {

rocblas_status rocblas_cher(rocblas_handle               handle,
                            rocblas_fill                 uplo,
                            rocblas_int                  n,
                            const float*                 alpha,
                            const rocblas_float_complex* x,
                            rocblas_int                  incx,
                            rocblas_float_complex*       A,
                            rocblas_int                  lda)
try
{
    return rocblas_her_impl(handle, uplo, n, alpha, x, incx, A, lda);
}
catch(...)
{
    return exception_to_rocblas_status();
}

rocblas_status rocblas_zher(rocblas_handle                handle,
                            rocblas_fill                  uplo,
                            rocblas_int                   n,
                            const double*                 alpha,
                            const rocblas_double_complex* x,
                            rocblas_int                   incx,
                            rocblas_double_complex*       A,
                            rocblas_int                   lda)
try
{
    return rocblas_her_impl(handle, uplo, n, alpha, x, incx, A, lda);
}
catch(...)
{
    return exception_to_rocblas_status();
}

} // extern "C"
