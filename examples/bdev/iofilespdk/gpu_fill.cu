#include <cuda_runtime.h>
#include <stdio.h>
#include <string.h>
#include "gpu_fill.h"

__global__ void gpu_copy_kernel(char *dst, const char *src, size_t len)
{
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < len) {
        dst[i] = src[i];
    }
}

extern "C" int
gpu_copy_buffer(char *host_dst, char *host_src, size_t len)
{
    cudaError_t err;
    int ok = 1;

    err = cudaHostRegister(host_dst, len, cudaHostRegisterMapped);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaHostRegister(dst) failed: %s\n", cudaGetErrorString(err));
        return -1;
    }



    char *dev_dst = NULL, *dev_src = NULL;
    err = cudaHostGetDevicePointer((void **)&dev_dst, host_dst, 0);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaHostGetDevicePointer(dst) failed: %s\n", cudaGetErrorString(err));
        ok = 0;
    }
    if (ok) {
        err = cudaHostGetDevicePointer((void **)&dev_src, host_src, 0);
        if (err != cudaSuccess) {
            fprintf(stderr, "cudaHostGetDevicePointer(src) failed: %s\n", cudaGetErrorString(err));
            ok = 0;
        }
    }
    if (ok) {
        int threads = 256;
        int blocks = (len + threads - 1) / threads;
        gpu_copy_kernel<<<blocks, threads>>>(dev_dst, dev_src, len);
        cudaDeviceSynchronize();

        err = cudaGetLastError();
        if (err != cudaSuccess) {
            fprintf(stderr, "kernel launch failed: %s\n", cudaGetErrorString(err));
            ok = 0;
        }
    }

    cudaHostUnregister(host_dst);

    return ok ? 0 : -1;
}

extern "C" int
gpu_alloc_buffer(char **ptr, size_t len){
    cudaError_t err = cudaHostAlloc((void**)ptr, len, cudaHostAllocMapped);
    if(err != cudaSuccess){
        fprintf(stderr, "Coulndt allocate buffer in GPU\n");
        return -1;
    }
    return 0;
}

extern "C" void 
gpu_free_buffer(char *ptr){
    cudaFreeHost(ptr);
}
