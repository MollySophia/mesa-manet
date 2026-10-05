#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct drm_panthor_csif_info;
int kbv_init(int fd, uint16_t *major, uint16_t *minor);
bool kbv_has(int fd);
bool kbv_has_error(int fd);
void kbv_finish(int fd);
uint8_t *kbv_props(int fd, size_t *size);
int kbv_csif(int fd, struct drm_panthor_csif_info *info);
int kbv_bo_alloc(int fd, uint64_t size, uint64_t flags, bool shared, uint32_t *handle,
                 uint32_t *gem, uint64_t *gpu_va, void **map);
void kbv_bo_free(int fd, uint32_t handle, uint32_t gem, void *map, uint64_t size);
int kbv_bo_sync(int fd, uint32_t handle, uint64_t offset, uint64_t size, uint8_t type);
int kbv_ioctl(int fd, unsigned long command, ...);
struct kbase_kmod_csf_user_io;
bool kbv_queue_bind(int fd, uint32_t group, uint32_t csi, uint64_t va,
                     uint32_t size, struct kbase_kmod_csf_user_io *io);
void kbv_queue_term(int fd, uint64_t va, struct kbase_kmod_csf_user_io *io);
int kbv_queue_kick(int fd, uint64_t va);
int kbv_wait_event(int fd, int64_t timeout_ns);
int kbv_cpu_sync(const void *ptr, size_t size, uint8_t type);
uint64_t kbv_alias_create(int fd, uint64_t va, uint64_t size, uint32_t nents);
void kbv_alias_destroy(int fd, uint64_t va);
