#include <errno.h>
#include <stdarg.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "drm-uapi/virtgpu_drm.h"
#include "drm-uapi/panthor_drm.h"
#include "drm-uapi/mali_kbase_ioctl.h"
#include "mali_kbase_virtio.h"
#include "kbase_kmod.h"

struct kbv_req {
   uint32_t cmd, len, seqno, rsp_off;
   uint32_t handle, offset, size, reserved;
   uint8_t data[1024];
};
struct kbv_rsp {
   uint32_t len;
   int32_t status;
   uint32_t handle, size;
   uint64_t value;
   uint8_t data[1024];
};
struct kbv_ctx {
   int fd;
   int error;
   uint32_t gem, seqno;
   void *shm;
   struct { uint32_t handle; void *ptr; uint64_t size; } bo[1024];
   struct { uint32_t gem; uint64_t va; void *ptr; } queue[32];
};
static struct kbv_ctx contexts[16];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct kbv_ctx *lookup(int fd)
{
   for (unsigned i = 0; i < 16; i++)
      if (contexts[i].shm && contexts[i].fd == fd) return &contexts[i];
   return NULL;
}
bool kbv_has(int fd)
{
   pthread_mutex_lock(&lock);
   bool found = lookup(fd) != NULL;
   pthread_mutex_unlock(&lock);
   return found;
}
bool kbv_has_error(int fd)
{
   pthread_mutex_lock(&lock);
   struct kbv_ctx *ctx = lookup(fd);
   int error = ctx ? ctx->error : 0;
   pthread_mutex_unlock(&lock);
   if (error) errno = error;
   return error != 0;
}
static int latch_error(int fd, int ret)
{
   if (ret >= 0) return ret;
   int saved = errno ? errno : EIO;
   pthread_mutex_lock(&lock);
   struct kbv_ctx *ctx = lookup(fd);
   if (ctx && !ctx->error) {
      ctx->error = saved;
      fprintf(stderr, "kbase virtio: context fd=%d failed: %s\n", fd, strerror(saved));
   }
   pthread_mutex_unlock(&lock);
   errno = saved;
   return ret;
}
int kbv_init(int fd, uint16_t *major, uint16_t *minor)
{
   if (!getenv("PANVK_KBASE_VIRTIO")) return 0;
   uint32_t caps[6] = {0};
   struct drm_virtgpu_get_caps get = {.cap_set_id = 6, .addr = (uintptr_t)caps, .size = sizeof(caps)};
   if (ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &get) || caps[0] != 2 || caps[4] != 0x4b424153) {
      errno = ENODEV; return -1;
   }
   pthread_mutex_lock(&lock);
   struct kbv_ctx *ctx = NULL;
   for (unsigned i = 0; i < 16; i++) if (!contexts[i].shm) { ctx = &contexts[i]; break; }
   if (!ctx) { errno = EMFILE; goto fail; }
   struct drm_virtgpu_context_set_param params[2] = {{1, 6}, {2, 1}};
   struct drm_virtgpu_context_init init = {.num_params = 2, .ctx_set_params = (uintptr_t)params};
   if (ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init)) goto fail;
   struct drm_virtgpu_resource_create_blob blob = {
      .blob_mem = VIRTGPU_BLOB_MEM_HOST3D, .blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE, .size = 4096,
   };
   if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &blob)) goto fail;
   struct drm_virtgpu_map map = {.handle = blob.bo_handle};
   void *shm = MAP_FAILED;
   if (!ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &map))
      shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
   if (shm == MAP_FAILED) {
      int saved = errno;
      struct drm_gem_close close = {.handle = blob.bo_handle};
      ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close); errno = saved; goto fail;
   }
   *ctx = (struct kbv_ctx){.fd = fd, .gem = blob.bo_handle, .shm = shm};
   *major = caps[1]; *minor = caps[2];
   pthread_mutex_unlock(&lock); return 1;
fail:
   pthread_mutex_unlock(&lock); return -1;
}
void kbv_finish(int fd)
{
   pthread_mutex_lock(&lock);
   struct kbv_ctx *ctx = lookup(fd);
   if (ctx) {
      munmap(ctx->shm, 4096);
      struct drm_gem_close close = {.handle = ctx->gem};
      ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
      memset(ctx, 0, sizeof(*ctx));
   }
   pthread_mutex_unlock(&lock);
}
static int rpc(int fd, struct kbv_req *q, struct kbv_rsp *r)
{
   int ret = -1;
   pthread_mutex_lock(&lock);
   struct kbv_ctx *ctx = lookup(fd);
   if (!ctx) { errno = ENODEV; goto out; }
   uint32_t *shm = ctx->shm;
   if (shm[1] != 8) { errno = EPROTO; goto out; }
   q->len = sizeof(*q); q->seqno = ++ctx->seqno;
   struct kbv_rsp *shared_rsp = (void *)((uint8_t *)ctx->shm + 8);
   shared_rsp->len = sizeof(*r);
   __atomic_thread_fence(__ATOMIC_RELEASE);
   struct drm_virtgpu_execbuffer exec = {
      .flags = VIRTGPU_EXECBUF_FENCE_FD_OUT | VIRTGPU_EXECBUF_RING_IDX,
      .size = sizeof(*q), .command = (uintptr_t)q, .fence_fd = -1,
   };
   if (ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &exec)) goto out;
   struct pollfd p = {.fd = exec.fence_fd, .events = POLLIN};
   int wait = poll(&p, 1, 5000);
   int saved = errno;
   close(exec.fence_fd);
   if (wait <= 0 || !(p.revents & POLLIN)) {
      errno = wait < 0 ? saved : ETIMEDOUT; goto out;
   }
   if (__atomic_load_n(shm, __ATOMIC_ACQUIRE) != q->seqno) { errno = EPROTO; goto out; }
   memcpy(r, shared_rsp, sizeof(*r));
   if (r->len != sizeof(*r)) { errno = EPROTO; goto out; }
   if (r->status) { errno = -r->status; goto out; }
   ret = 0;
out:
   pthread_mutex_unlock(&lock); return ret;
}
uint8_t *kbv_props(int fd, size_t *size)
{
   struct kbv_req q = {.cmd = 9};
   struct kbv_rsp r;
   if (rpc(fd, &q, &r)) return NULL;
   if (!r.value || r.value > 65536) { errno = EPROTO; return NULL; }
   *size = r.value;
   uint8_t *buf = malloc(*size);
   if (!buf) return NULL;
   for (size_t off = 0; off < *size;) {
      q.offset = off; q.size = 1024;
      if (rpc(fd, &q, &r) || !r.size || r.size > 1024 || r.size > *size - off) {
         free(buf); return NULL;
      }
      memcpy(buf + off, r.data, r.size); off += r.size;
   }
   return buf;
}
int kbv_csif(int fd, struct drm_panthor_csif_info *info)
{
   struct kbv_req q = {.cmd = 10};
   struct kbv_rsp r;
   if (rpc(fd, &q, &r)) return -1;
   if (r.size != sizeof(*info)) { errno = EPROTO; return -1; }
   memcpy(info, r.data, sizeof(*info)); return 0;
}
int kbv_bo_alloc(int fd, uint64_t size, uint64_t flags, bool shared, uint32_t *handle,
                 uint32_t *gem, uint64_t *gpu_va, void **map)
{
   if (size > UINT32_MAX || flags > UINT32_MAX) { errno = EOVERFLOW; return -1; }
   struct kbv_req q = {.cmd = 11, .size = size, .handle = flags, .reserved = shared};
   struct kbv_rsp r;
   if (rpc(fd, &q, &r)) return -1;
   *handle = r.handle; *gpu_va = r.value;
   struct drm_virtgpu_resource_create_blob b = {.blob_mem = 2, .blob_flags = 1,
      .size = size, .blob_id = r.handle};
   if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &b)) goto fail;
   struct drm_virtgpu_map m = {.handle = b.bo_handle};
   if (!ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &m)) {
      *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
      if (*map != MAP_FAILED) {
         pthread_mutex_lock(&lock);
         struct kbv_ctx *ctx = lookup(fd);
         unsigned slot;
         for (slot = 0; slot < 1024 && ctx->bo[slot].ptr; slot++);
         if (slot < 1024) {
            ctx->bo[slot].handle = *handle; ctx->bo[slot].ptr = *map; ctx->bo[slot].size = size;
         }
         pthread_mutex_unlock(&lock);
         if (slot < 1024) { *gem = b.bo_handle; return 0; }
         munmap(*map, size); errno = ENOSPC;
      }
   }
   struct drm_gem_close close = {.handle = b.bo_handle};
   ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
fail:
   q = (struct kbv_req){.cmd = 4, .handle = *handle}; rpc(fd, &q, &r);
   return -1;
}
void kbv_bo_free(int fd, uint32_t handle, uint32_t gem, void *map, uint64_t size)
{
   pthread_mutex_lock(&lock);
   struct kbv_ctx *ctx = lookup(fd);
   if (ctx) for (unsigned i = 0; i < 1024; i++)
      if (ctx->bo[i].ptr == map) memset(&ctx->bo[i], 0, sizeof(ctx->bo[i]));
   pthread_mutex_unlock(&lock);
   munmap(map, size);
   struct drm_gem_close close = {.handle = gem};
   ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
   struct kbv_req q = {.cmd = 4, .handle = handle};
   struct kbv_rsp r;
   rpc(fd, &q, &r);
}
int kbv_bo_sync(int fd, uint32_t handle, uint64_t offset, uint64_t size, uint8_t type)
{
   if (offset > UINT32_MAX || size > UINT32_MAX) { errno = EOVERFLOW; return -1; }
   struct kbv_req q = {.cmd = 12, .handle = handle, .offset = offset, .size = size};
   struct kbv_rsp r;
   q.data[0] = type;
   return latch_error(fd, rpc(fd, &q, &r));
}
int kbv_ioctl(int fd, unsigned long command, ...)
{
   va_list args;
   void *arg = NULL;
   va_start(args, command);
   if (_IOC_SIZE(command)) arg = va_arg(args, void *);
   va_end(args);
   if (!kbv_has(fd)) return ioctl(fd, command, arg);
   if (_IOC_SIZE(command) > 1024) { errno = EINVAL; return -1; }
   struct kbv_req q = {.cmd = 13, .handle = command, .size = _IOC_SIZE(command)};
   struct kbv_rsp r;
   if (q.size) memcpy(q.data, arg, q.size);
   if (rpc(fd, &q, &r)) return -1;
   if (r.size != q.size) { errno = EPROTO; return -1; }
   if (q.size) memcpy(arg, r.data, q.size);
   return 0;
}
int kbv_cpu_sync(const void *ptr, size_t size, uint8_t type)
{
   uintptr_t addr = (uintptr_t)ptr;
   int fd = -1;
   uint32_t handle = 0;
   uint64_t offset = 0;
   pthread_mutex_lock(&lock);
   for (unsigned c = 0; c < 16 && fd < 0; c++) {
      if (!contexts[c].shm) continue;
      for (unsigned b = 0; b < 1024; b++) {
         uintptr_t start = (uintptr_t)contexts[c].bo[b].ptr;
         uint64_t len = contexts[c].bo[b].size;
         if (start && addr >= start && addr - start < len && size <= len - (addr - start)) {
            fd = contexts[c].fd; handle = contexts[c].bo[b].handle; offset = addr - start; break;
         }
      }
   }
   pthread_mutex_unlock(&lock);
   if (fd < 0) return 0;
   return kbv_bo_sync(fd, handle, offset, size, type) ? -1 : 1;
}
static int queue_rpc(int fd, uint32_t op, uint64_t va, uint32_t csi,
                      uint32_t size, uint32_t group, struct kbv_rsp *r)
{
   struct kbv_req q = {.cmd = 14, .handle = op, .offset = csi, .size = size};
   memcpy(q.data, &va, 8); memcpy(q.data + 8, &group, 4);
   return rpc(fd, &q, r);
}
bool kbv_queue_bind(int fd, uint32_t group, uint32_t csi, uint64_t va,
                     uint32_t size, struct kbase_kmod_csf_user_io *io)
{
   struct kbv_rsp r;
   if (queue_rpc(fd, 0, va, csi, size, group, &r)) return false;
   struct drm_virtgpu_resource_create_blob b = {.blob_mem = 2, .blob_flags = 1,
      .size = 12288, .blob_id = r.handle};
   if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &b)) goto fail;
   struct drm_virtgpu_map m = {.handle = b.bo_handle};
   void *ptr = MAP_FAILED;
   if (!ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &m))
      ptr = mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
   if (ptr != MAP_FAILED) {
      pthread_mutex_lock(&lock);
      struct kbv_ctx *ctx = lookup(fd);
      unsigned slot;
      for (slot = 0; slot < 32 && ctx->queue[slot].ptr; slot++);
      if (slot < 32) {
         ctx->queue[slot].ptr = ptr; ctx->queue[slot].va = va; ctx->queue[slot].gem = b.bo_handle;
      }
      pthread_mutex_unlock(&lock);
      if (slot < 32) {
         *io = (struct kbase_kmod_csf_user_io){.map = ptr, .doorbell = ptr,
            .input = (uint8_t *)ptr + 4096 + 16 * csi,
            .output = (uint8_t *)ptr + 8192 + 16 * csi};
         return true;
      }
      munmap(ptr, 12288);
   }
   struct drm_gem_close close = {.handle = b.bo_handle};
   ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
fail:
   queue_rpc(fd, 1, va, 0, 0, 0, &r); return false;
}
void kbv_queue_term(int fd, uint64_t va, struct kbase_kmod_csf_user_io *io)
{
   pthread_mutex_lock(&lock);
   struct kbv_ctx *ctx = lookup(fd);
   if (ctx) for (unsigned i = 0; i < 32; i++) {
      if (!ctx->queue[i].ptr || ctx->queue[i].va != va) continue;
      munmap(ctx->queue[i].ptr, 12288);
      struct drm_gem_close close = {.handle = ctx->queue[i].gem};
      ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
      memset(&ctx->queue[i], 0, sizeof(ctx->queue[i]));
   }
   pthread_mutex_unlock(&lock);
   struct kbv_rsp r;
   queue_rpc(fd, 1, va, 0, 0, 0, &r);
   memset(io, 0, sizeof(*io));
}
int kbv_queue_kick(int fd, uint64_t va)
{
   if (kbv_has_error(fd)) return -1;
   struct kbv_rsp r;
   return latch_error(fd, queue_rpc(fd, 2, va, 0, 0, 0, &r));
}
int kbv_wait_event(int fd, int64_t timeout_ns)
{
   if (kbv_has_error(fd)) return -1;
   uint32_t ms = timeout_ns < 0 || timeout_ns > 20000000 ? 20 : (timeout_ns + 999999) / 1000000;
   struct kbv_rsp r;
   return latch_error(fd, queue_rpc(fd, 3, 0, 0, ms, 0, &r));
}
uint64_t kbv_alias_create(int fd, uint64_t va, uint64_t size, uint32_t nents)
{
   if (size > UINT32_MAX) { errno = EOVERFLOW; return 0; }
   struct kbv_req q = {.cmd = 15, .offset = nents, .size = size};
   struct kbv_rsp r;
   memcpy(q.data, &va, 8);
   return rpc(fd, &q, &r) ? 0 : r.value;
}
void kbv_alias_destroy(int fd, uint64_t va)
{
   struct kbv_req q = {.cmd = 15, .handle = 1};
   struct kbv_rsp r;
   memcpy(q.data, &va, 8);
   rpc(fd, &q, &r);
}
