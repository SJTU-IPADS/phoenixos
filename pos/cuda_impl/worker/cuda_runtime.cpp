#include <iostream>

#include "pos/include/common.h"
#include "pos/cuda_impl/worker.h"

#include <cuda.h>
#include <cuda_runtime_api.h>

namespace wk_functions {


/*!
 *  \related    cudaMalloc
 *  \brief      allocate a memory area
 */
namespace cuda_malloc {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *memory_handle;
        size_t allocate_size;
        void *ptr;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);
        
        // execute the actual cuda_malloc
        allocate_size = pos_api_param_value(wqe, 0, size_t);
        wqe->api_cxt->return_code = cudaMalloc(&ptr, allocate_size);

        // record server address
        if(likely(cudaSuccess == wqe->api_cxt->return_code)){
            memory_handle = pos_api_create_handle(wqe, 0);
            POS_CHECK_POINTER(memory_handle);
            
            retval = memory_handle->set_passthrough_addr(ptr, memory_handle);
            if(unlikely(POS_SUCCESS != retval)){ 
                POS_WARN_DETAIL("failed to set passthrough address for the memory handle: %p", ptr);
                goto exit;
            }

            memory_handle->mark_status(kPOS_HandleStatus_Active);
            memcpy(wqe->api_cxt->ret_data, &(memory_handle->client_addr), sizeof(uint64_t));
        } else {
            memset(wqe->api_cxt->ret_data, 0, sizeof(uint64_t));
        }

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_malloc


/*!
 *  \related    cudaFree
 *  \brief      release a CUDA memory area
 */
namespace cuda_free {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *memory_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        memory_handle = pos_api_delete_handle(wqe, 0);
        POS_CHECK_POINTER(memory_handle);

        wqe->api_cxt->return_code = cudaFree(
            /* devPtr */ memory_handle->server_addr
        );

        if(likely(cudaSuccess == wqe->api_cxt->return_code)){
            memory_handle->mark_status(kPOS_HandleStatus_Deleted);
        }

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_free


/*!
 *  \related    cudaLaunchKernel
 *  \brief      launch a user-define computation kernel
 */
namespace cuda_launch_kernel {
#define POS_CUDA_LAUNCH_KERNEL_MAX_NB_PARAMS    64

    static void* cuda_args[POS_CUDA_LAUNCH_KERNEL_MAX_NB_PARAMS] = {0};

    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle_CUDA_Function *function_handle;
        POSHandle_CUDA_Stream *stream_handle;
        POSHandle *memory_handle;
        uint64_t i, j, nb_involved_memory;
        // void **cuda_args = nullptr;
        void *args, *args_values, *arg_addr;
        uint64_t *addr_list;
        cudaStream_t worker_stream;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        function_handle = (POSHandle_CUDA_Function*)(pos_api_input_handle(wqe, 0));
        POS_CHECK_POINTER(function_handle);

        // stream_handle = pos_api_typed_handle(wqe, kPOS_ResourceTypeId_CUDA_Stream, POSHandle_CUDA_Stream, 0);
        // POS_CHECK_POINTER(stream_handle);

        if(unlikely(ws->worker->worker_stream == nullptr)){
            POS_ASSERT(cudaSuccess == cudaStreamCreate(&worker_stream));
            ws->worker->worker_stream = worker_stream;
        }

        // the 3rd parameter of the API call contains parameter to launch the kernel
        args = pos_api_param_addr(wqe, 3);
        POS_CHECK_POINTER(args);

        // [Cricket Adapt] skip the metadata used by cricket
        args += (sizeof(size_t) + sizeof(uint16_t) * function_handle->nb_params);

        /*!
         *  \note   the actual kernel parameter list passed to the cuLaunchKernel is 
         *          an array of pointers, so we allocate a new array here to store
         *          these pointers
         */
        // TODO: pre-allocated!
        // if(likely(function_handle->nb_params > 0)){
        //     POS_CHECK_POINTER(cuda_args = malloc(function_handle->nb_params * sizeof(void*)));
        // }

        for(i=0; i<function_handle->nb_params; i++){
            cuda_args[i] = args + function_handle->param_offsets[i];
            POS_CHECK_POINTER(cuda_args[i]);
        }
        typedef struct __dim3 { uint32_t x; uint32_t y; uint32_t z; } __dim3_t;

        wqe->api_cxt->return_code = cuLaunchKernel(
            /* f */ (CUfunction)(function_handle->server_addr),
            /* gridDimX */ ((__dim3_t*)pos_api_param_addr(wqe, 1))->x,
            /* gridDimY */ ((__dim3_t*)pos_api_param_addr(wqe, 1))->y,
            /* gridDimZ */ ((__dim3_t*)pos_api_param_addr(wqe, 1))->z,
            /* blockDimX */ ((__dim3_t*)pos_api_param_addr(wqe, 2))->x,
            /* blockDimY */ ((__dim3_t*)pos_api_param_addr(wqe, 2))->y,
            /* blockDimZ */ ((__dim3_t*)pos_api_param_addr(wqe, 2))->z,
            /* sharedMemBytes */ pos_api_param_value(wqe, 4, size_t),
            // /* hStream */ stream_handle->server_addr,
            /* hStream */ (CUstream)(ws->worker->worker_stream),
            /* kernelParams */ cuda_args,
            /* extra */ nullptr
        );

        // if(likely(cuda_args != nullptr)){ free(cuda_args); }

        if(unlikely(CUDA_SUCCESS != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_launch_kernel




/*!
 *  \related    cudaMemcpy (Host to Device)
 *  \brief      copy memory buffer from host to device
 */
namespace cuda_memcpy_h2d {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *memory_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        memory_handle = pos_api_inout_handle(wqe, 0);
        POS_CHECK_POINTER(memory_handle);

        wqe->api_cxt->return_code = cudaMemcpy(
            /* dst */ memory_handle->server_addr,
            /* src */ pos_api_param_addr(wqe, 1),
            /* count */ pos_api_param_size(wqe, 1),
            /* kind */ cudaMemcpyHostToDevice
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_memcpy_h2d



/*!
 *  \related    cudaMemcpy (Device to Host)
 *  \brief      copy memory buffer from device to host
 */
namespace cuda_memcpy_d2h {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *memory_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        memory_handle = pos_api_input_handle(wqe, 0);
        POS_CHECK_POINTER(memory_handle);

        wqe->api_cxt->return_code = cudaMemcpy(
            /* dst */ wqe->api_cxt->ret_data,
            /* src */ (const void*)(memory_handle->server_addr),
            /* count */ pos_api_param_value(wqe, 1, uint64_t),
            /* kind */ cudaMemcpyDeviceToHost
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_memcpy_d2h




/*!
 *  \related    cudaMemcpy (Device to Device)
 *  \brief      copy memory buffer from device to device
 */
namespace cuda_memcpy_d2d {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *dst_memory_handle, *src_memory_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        dst_memory_handle = pos_api_output_handle(wqe, 0);
        POS_CHECK_POINTER(dst_memory_handle);

        src_memory_handle = pos_api_input_handle(wqe, 0);
        POS_CHECK_POINTER(src_memory_handle);

        wqe->api_cxt->return_code = cudaMemcpy(
            /* dst */ dst_memory_handle->server_addr,
            /* src */ src_memory_handle->server_addr,
            /* count */ pos_api_param_value(wqe, 2, uint64_t),
            /* kind */ cudaMemcpyDeviceToDevice
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_memcpy_d2d




/*!
 *  \related    cudaMemcpyAsync (Host to Device)
 *  \brief      async copy memory buffer from host to device
 */
namespace cuda_memcpy_h2d_async {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *memory_handle, *stream_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        memory_handle = pos_api_inout_handle(wqe, 0);
        POS_CHECK_POINTER(memory_handle);

        stream_handle = pos_api_input_handle(wqe, 0);
        POS_CHECK_POINTER(stream_handle);

        wqe->api_cxt->return_code = cudaMemcpyAsync(
            /* dst */ memory_handle->server_addr,
            /* src */ pos_api_param_addr(wqe, 1),
            /* count */ pos_api_param_size(wqe, 1),
            /* kind */ cudaMemcpyHostToDevice,
            /* stream */ (cudaStream_t)(stream_handle->server_addr)
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_memcpy_h2d_async




/*!
 *  \related    cudaMemcpyAsync (Device to Host)
 *  \brief      async copy memory buffer from device to host
 */
namespace cuda_memcpy_d2h_async {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *memory_handle, *stream_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        memory_handle = pos_api_input_handle(wqe, 0);
        POS_CHECK_POINTER(memory_handle);

        stream_handle = pos_api_input_handle(wqe, 1);
        POS_CHECK_POINTER(stream_handle);

        wqe->api_cxt->return_code = cudaMemcpyAsync(
            /* dst */ wqe->api_cxt->ret_data,
            /* src */ memory_handle->server_addr,
            /* count */ pos_api_param_value(wqe, 1, uint64_t),
            /* kind */ cudaMemcpyDeviceToHost,
            /* stream */ (cudaStream_t)(stream_handle->server_addr)
        );

        /*! \note   we must synchronize this api under remoting */
        wqe->api_cxt->return_code = cudaStreamSynchronize(
            (cudaStream_t)(stream_handle->server_addr)
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_memcpy_d2h_async




/*!
 *  \related    cudaMemcpyAsync (Device to Device)
 *  \brief      async copy memory buffer from device to device
 */
namespace cuda_memcpy_d2d_async {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *dst_memory_handle, *src_memory_handle, *stream_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        dst_memory_handle = pos_api_output_handle(wqe, 0);
        POS_CHECK_POINTER(dst_memory_handle);

        src_memory_handle = pos_api_input_handle(wqe, 0);
        POS_CHECK_POINTER(src_memory_handle);

        stream_handle = pos_api_input_handle(wqe, 1);
        POS_CHECK_POINTER(stream_handle);

        wqe->api_cxt->return_code = cudaMemcpyAsync(
            /* dst */ dst_memory_handle->server_addr,
            /* src */ src_memory_handle->server_addr,
            /* count */ pos_api_param_value(wqe, 2, uint64_t),
            /* kind */ cudaMemcpyDeviceToDevice,
            /* stream */ (cudaStream_t)(stream_handle->server_addr)
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_memcpy_d2d_async




/*!
 *  \related    cudaSetDevice
 *  \brief      specify a CUDA device to use
 */
namespace cuda_set_device {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle_CUDA_Device *device_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        device_handle = (POSHandle_CUDA_Device*)(pos_api_input_handle(wqe, 0));
        POS_CHECK_POINTER(device_handle);

        wqe->api_cxt->return_code = cudaSetDevice(device_handle->device_id);

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

        return retval;
    }
} // namespace cuda_set_device




/*!
 *  \related    cudaGetLastError
 *  \brief      obtain the latest error within the CUDA context
 */
namespace cuda_get_last_error {
    // parser function
    POS_WK_FUNC_LAUNCH(){
        POS_ERROR_DETAIL("shouldn't be called");
        return POS_SUCCESS;
    }
} // namespace cuda_get_last_error




/*!
 *  \related    cudaGetErrorString
 *  \brief      obtain the error string from the CUDA context
 */
namespace cuda_get_error_string {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        const char* ret_string;
        ret_string = cudaGetErrorString(pos_api_param_value(wqe, 0, cudaError_t));

        if(likely(strlen(ret_string) > 0)){
            memcpy(wqe->api_cxt->ret_data, ret_string, strlen(ret_string)+1);
        }

        wqe->api_cxt->return_code = cudaSuccess;

        POSWorker::__done(ws, wqe);
        
        return POS_SUCCESS;
    }
} // namespace cuda_get_error_string




/*!
 *  \related    cudaPeekAtLastError
 *  \brief      obtain the latest error within the CUDA context
 */
namespace cuda_peek_at_last_error {
    // parser function
    POS_WK_FUNC_LAUNCH(){
        POS_ERROR_DETAIL("shouldn't be called");
        return POS_SUCCESS;
    }
} // namespace cuda_peek_at_last_error




/*!
 *  \related    cudaGetDeviceCount
 *  \brief      obtain the number of devices
 */
namespace cuda_get_device_count {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        // TODO: we launch this op for debug?
        POSWorker::__done(ws, wqe);
        return POS_SUCCESS;
    }
} // namespace cuda_get_device_count





/*!
 *  \related    cudaGetDeviceProperties
 *  \brief      obtain the properties of specified device
 */
namespace cuda_get_device_properties {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle_CUDA_Device *device_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        device_handle = (POSHandle_CUDA_Device*)(pos_api_input_handle(wqe, 0));
        POS_CHECK_POINTER(device_handle);

        wqe->api_cxt->return_code = cudaGetDeviceProperties(
            (struct cudaDeviceProp*)wqe->api_cxt->ret_data, 
            device_handle->device_id
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_get_device_properties



/*!
 *  \related    cudaDeviceGetAttribute
 *  \brief      obtain the properties of specified device
 */
namespace cuda_device_get_attribute {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle_CUDA_Device *device_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        device_handle = (POSHandle_CUDA_Device*)(pos_api_input_handle(wqe, 0));
        POS_CHECK_POINTER(device_handle);

        wqe->api_cxt->return_code = cudaDeviceGetAttribute(
            /* value */ (int*)(wqe->api_cxt->ret_data), 
            /* attr */ pos_api_param_value(wqe, 0, cudaDeviceAttr),
            /* device */ device_handle->device_id
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_device_get_attribute



/*!
 *  \related    cudaGetDevice
 *  \brief      obtain the handle of specified device
 */
namespace cuda_get_device {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        // TODO: we launch this op for debug?
        POSWorker::__done(ws, wqe);
        return POS_SUCCESS;
    }
} // namespace cuda_get_device



/*!
 *  \related    cudaFuncGetAttributes
 *  \brief      find out attributes for a given function
 */
namespace cuda_func_get_attributes {
    // launch function
    POS_WK_FUNC_LAUNCH(){

        pos_retval_t retval = POS_SUCCESS;
        POSHandle_CUDA_Function *function_handle;
        struct cudaFuncAttributes *attr = (struct cudaFuncAttributes*)wqe->api_cxt->ret_data;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        function_handle = (POSHandle_CUDA_Function*)(pos_api_input_handle(wqe, 0));
        POS_CHECK_POINTER(function_handle);

    #define GET_FUNC_ATTR(member, name)					                                    \
        do {								                                                \
            int tmp;								                                        \
            wqe->api_cxt->return_code = cuFuncGetAttribute(                                 \
                &tmp, CU_FUNC_ATTRIBUTE_##name, (CUfunction)(function_handle->server_addr)  \
            );                                                                              \
            if(unlikely(wqe->api_cxt->return_code != CUDA_SUCCESS)){                        \
                goto exit;                                                                  \
            }                                                                               \
            attr->member = tmp;						                                        \
        } while(0)
        GET_FUNC_ATTR(maxThreadsPerBlock, MAX_THREADS_PER_BLOCK);
        GET_FUNC_ATTR(sharedSizeBytes, SHARED_SIZE_BYTES);
        GET_FUNC_ATTR(constSizeBytes, CONST_SIZE_BYTES);
        GET_FUNC_ATTR(localSizeBytes, LOCAL_SIZE_BYTES);
        GET_FUNC_ATTR(numRegs, NUM_REGS);
        GET_FUNC_ATTR(ptxVersion, PTX_VERSION);
        GET_FUNC_ATTR(binaryVersion, BINARY_VERSION);
        GET_FUNC_ATTR(cacheModeCA, CACHE_MODE_CA);
        GET_FUNC_ATTR(maxDynamicSharedSizeBytes, MAX_DYNAMIC_SHARED_SIZE_BYTES);
        GET_FUNC_ATTR(preferredShmemCarveout, PREFERRED_SHARED_MEMORY_CARVEOUT);
    #undef GET_FUNC_ATTR

    exit:
        if(unlikely(CUDA_SUCCESS != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

        return retval;
    }
} // namespace cuda_func_get_attributes



/*!
 *  \related    cudaOccupancyMaxActiveBlocksPerMultiprocessorWithFlags
 *  \brief      returns occupancy for a device function with the specified flags
 */
namespace cuda_occupancy_max_active_bpm_with_flags {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle_CUDA_Function *function_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        function_handle = (POSHandle_CUDA_Function*)(pos_api_input_handle(wqe, 0));
        POS_CHECK_POINTER(function_handle);

        wqe->api_cxt->return_code = cuOccupancyMaxActiveBlocksPerMultiprocessorWithFlags(
            /* numBlock */ (int*)(wqe->api_cxt->ret_data),
            /* func */ (CUfunction)(function_handle->server_addr),
            /* blockSize */ pos_api_param_value(wqe, 1, int),
            /* dynamicSMemSize */ pos_api_param_value(wqe, 2, size_t),
            /* flags */ pos_api_param_value(wqe, 3, int)
        );

    exit:
        if(unlikely(CUDA_SUCCESS != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

        return retval;
    }
} // namespace cuda_occupancy_max_active_bpm_with_flags




/*!
 *  \related    cudaStreamSynchronize
 *  \brief      sync a specified stream
 */
namespace cuda_stream_synchronize {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *stream_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        stream_handle = pos_api_input_handle(wqe, 0);
        POS_CHECK_POINTER(stream_handle);

        wqe->api_cxt->return_code = cudaStreamSynchronize(
            (cudaStream_t)(stream_handle->server_addr)
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

        return retval;
    }
} // namespace cuda_stream_synchronize




/*!
 *  \related    cudaStreamIsCapturing
 *  \brief      obtain the stream's capturing state
 */
namespace cuda_stream_is_capturing {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        // pos_retval_t retval = POS_SUCCESS;
        // POSHandle_CUDA_Stream *stream_handle;

        // POS_CHECK_POINTER(ws);
        // POS_CHECK_POINTER(wqe);

        // stream_handle = pos_api_typed_handle(wqe, kPOS_ResourceTypeId_CUDA_Stream, POSHandle_CUDA_Stream, 0);
        // POS_CHECK_POINTER(stream_handle);

        // wqe->api_cxt->return_code = cudaStreamIsCapturing(
        //     /* stream */ stream_handle->server_addr,
        //     /* pCaptureStatus */ (cudaStreamCaptureStatus*) wqe->api_cxt->ret_data
        // );

        // return retval;

        // we launch this op just for debug
        POSWorker::__done(ws, wqe);

        return POS_SUCCESS;
    }
} // namespace cuda_stream_is_capturing




/*!
 *  \related    cuda_event_create_with_flags
 *  \brief      create a new event with flags
 */
namespace cuda_event_create_with_flags {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *event_handle;
        int flags;
        cudaEvent_t ptr;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);
        
        // execute the actual cudaEventCreateWithFlags
        flags = pos_api_param_value(wqe, 0, int);
        wqe->api_cxt->return_code = cudaEventCreateWithFlags(&ptr, flags);

        // record server address
        if(likely(cudaSuccess == wqe->api_cxt->return_code)){
            event_handle = pos_api_create_handle(wqe, 0);
            POS_CHECK_POINTER(event_handle);
            event_handle->set_server_addr(ptr);
            event_handle->mark_status(kPOS_HandleStatus_Active);
        }

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_event_create_with_flags




/*!
 *  \related    cuda_event_destory
 *  \brief      destory a CUDA event
 */
namespace cuda_event_destory {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *event_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        event_handle = pos_api_delete_handle(wqe, 0);
        POS_CHECK_POINTER(event_handle);

        wqe->api_cxt->return_code = cudaEventDestroy(
            /* event */ (cudaEvent_t)(event_handle->server_addr)
        );

        if(likely(cudaSuccess == wqe->api_cxt->return_code)){
            event_handle->mark_status(kPOS_HandleStatus_Deleted);
        }

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_event_destory




/*!
 *  \related    cuda_event_record
 *  \brief      record a CUDA event
 */
namespace cuda_event_record {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        pos_retval_t retval = POS_SUCCESS;
        POSHandle *event_handle, *stream_handle;

        POS_CHECK_POINTER(ws);
        POS_CHECK_POINTER(wqe);

        event_handle = pos_api_output_handle(wqe, 0);
        POS_CHECK_POINTER(event_handle);
        stream_handle = pos_api_input_handle(wqe, 0);
        POS_CHECK_POINTER(stream_handle);

        wqe->api_cxt->return_code = cudaEventRecord(
            /* event */ (cudaEvent_t)(event_handle->server_addr),
            /* stream */ (cudaStream_t)(stream_handle->server_addr)
        );

        if(unlikely(cudaSuccess != wqe->api_cxt->return_code)){ 
            POSWorker::__restore(ws, wqe);
        } else {
            POSWorker::__done(ws, wqe);
        }

    exit:
        return retval;
    }
} // namespace cuda_event_record




/*!
 *  \related    template_cuda
 *  \brief      template_cuda
 */
namespace template_cuda {
    // launch function
    POS_WK_FUNC_LAUNCH(){
        return POS_FAILED_NOT_IMPLEMENTED;
    }
} // namespace template_cuda




} // namespace wk_functions 