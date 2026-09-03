#include <cuda_runtime.h>
#include <stdio.h>
#include "gpu_fill.h"

__global__ void gpu_fill_kernel(char *buf, size_t len)
{
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < len) {
        buf[i] = 'A' + (i % 26);
    }
}

extern "C" int
gpu_fill_buffer(char *host_buf, size_t len)
{
    cudaError_t err;

    err = cudaHostRegister(host_buf, len, cudaHostRegisterMapped);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaHostRegister failed: %s\n", cudaGetErrorString(err));
        return -1;
    }

    char *dev_buf = NULL;
    err = cudaHostGetDevicePointer((void **)&dev_buf, host_buf, 0);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaHostGetDevicePointer failed: %s\n", cudaGetErrorString(err));
        cudaHostUnregister(host_buf);
        return -1;
    }

    int threads = 256;
    int blocks = (len + threads - 1) / threads;
    gpu_fill_kernel<<<blocks, threads>>>(dev_buf, len);
    cudaDeviceSynchronize();

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        fprintf(stderr, "kernel launch failed: %s\n", cudaGetErrorString(err));
        cudaHostUnregister(host_buf);
        return -1;
    }

    cudaHostUnregister(host_buf);
    return 0;
}
