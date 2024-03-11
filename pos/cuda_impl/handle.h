#pragma once

#include <iostream>
#include <string>
#include <cstdlib>

#include <sys/resource.h>
#include <stdint.h>
#include <cuda.h>
#include <cuda_runtime_api.h>

#include "pos/include/common.h"
#include "pos/include/handle.h"

/*!
 *  \brief  idx of CUDA resource types
 */
enum : pos_resource_typeid_t {
    kPOS_ResourceTypeId_CUDA_Context = kPOS_ResourceTypeId_Num_Base_Type,
    kPOS_ResourceTypeId_CUDA_Module,
    kPOS_ResourceTypeId_CUDA_Function,
    kPOS_ResourceTypeId_CUDA_Var,
    kPOS_ResourceTypeId_CUDA_Device,
    kPOS_ResourceTypeId_CUDA_Memory,
    kPOS_ResourceTypeId_CUDA_Stream,
    kPOS_ResourceTypeId_CUDA_Event,

    /*! \note   library handle types, define in pos/cuda_impl/handle/xxx.h */
    kPOS_ResourceTypeId_cuBLAS_Context,
};

// declarations of CUDA handles
class POSHandle_CUDA_Context;
class POSHandle_CUDA_Device;
class POSHandle_CUDA_Event;
class POSHandle_CUDA_Function;
class POSHandle_CUDA_Memory;
class POSHandle_CUDA_Module;
class POSHandle_CUDA_Stream;
class POSHandle_CUDA_Var;
class POSHandle_cuBLAS_Context;

// declarations of managers for CUDA handles
class POSHandleManager_CUDA_Context;
class POSHandleManager_CUDA_Device;
class POSHandleManager_CUDA_Event;
class POSHandleManager_CUDA_Function;
class POSHandleManager_CUDA_Memory;
class POSHandleManager_CUDA_Module;
class POSHandleManager_CUDA_Stream;
class POSHandleManager_CUDA_Var;
class POSHandleManager_cuBLAS_Context;

// definitions
#include "pos/cuda_impl/handle/context.h"
#include "pos/cuda_impl/handle/device.h"
#include "pos/cuda_impl/handle/event.h"
#include "pos/cuda_impl/handle/function.h"
#include "pos/cuda_impl/handle/memory.h"
#include "pos/cuda_impl/handle/module.h"
#include "pos/cuda_impl/handle/stream.h"
#include "pos/cuda_impl/handle/var.h"
#include "pos/cuda_impl/handle/cublas.h"
