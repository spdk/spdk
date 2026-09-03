#include <cuda_runtime.h>
#include <stdio.h>
#include "gpu_fill.h"

__global__ void gpu_fill_kernel(char *dest, const char* src, size_t len){
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < len) {
        dest[i] = src[i];
    }
}

extern "C" int
gpu_copy_buffer(char *host_dst, char* host_src,size_t len){
    cudaError_t err;
    unsigned int ok = 0;

    err = cudaHostRegister(host_dst, len, cudaHostRegisterMapped);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaHostRegister failed: %s\n", cudaGetErrorString(err));
        return -1;
    }

    err = cudaHostRegister(host_src, len, cudaHostRegisterMapped);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaHostRegister failed: %s\n", cudaGetErrorString(err));
        return -1;
    }

    char *dev_dst = NULL, *dev_src = NULL;
    ok = 1;
    if(ok){
        err = cudaHostGetDevicePointer((void **)&dev_dst, host_dst, 0);
        if (err != cudaSuccess) {
            fprintf(stderr, "cudaHostGetDevicePointer failed: %s\n", cudaGetErrorString(err));
            cudaHostUnregister(host_dst);
            return -1;
            ok = 0;
        }
    }

    if(ok){
        err = cudaHostGetDevicePointer((void **)&dev_src, host_dst, 0);
        if (err != cudaSuccess) {
            fprintf(stderr, "cudaHostGetDevicePointer failed: %s\n", cudaGetErrorString(err));
            cudaHostUnregister(host_src);
            return -1;
            ok = 0;
        }
    }

    if(ok){
        int threads = 256;
        int blocks = (len + threads - 1) / threads;
        gpu_fill_kernel<<<blocks, threads>>>(dev_dst, dev_src, len);
        cudaDeviceSynchronize();

        err = cudaGetLastError();
        if (err != cudaSuccess) {
            fprintf(stderr, "kernel launch failed: %s\n", cudaGetErrorString(err));
            cudaHostUnregister(host_dst);
            return -1;
        }
    }
    cudaHostUnregister(host_dst);
    cudaHostUnregister(host_src);
    return ok ? 0:-1;
}
