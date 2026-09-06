#ifndef GPU_FILL_H
#define GPU_FILL_H

#ifdef __cplusplus
extern "C" {
#endif

int gpu_copy_buffer(char *dst, char *src, size_t len);
int gpu_alloc_buffer(char **ptr, size_t len);
void gpu_free_buffer(char *ptr);

#ifdef __cplusplus
}
#endif

#endif