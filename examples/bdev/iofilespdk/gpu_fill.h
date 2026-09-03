#ifndef GPU_FILL_H
#define GPU_FILL_H

#ifdef __cplusplus
extern "C" {
#endif

int gpu_copy_buffer(char *dst, char *src, size_t len);

#ifdef __cplusplus
}
#endif

#endif