#pragma once
#include <vulkan/vulkan.h>
#include <stdint.h>
// convenience wrapper of vk buffers that aren't managed (directly) by the
// graph resource allocation. these are used for temporary things (acceleration
// structure construction) or to manage buffer references on structs that are
// resource-managed.

typedef enum dt_buffer_flags_t
{
  s_buffer_flag_none         = 0,
  s_buffer_flag_host_visible = 1,
  s_buffer_flag_device_only  = 2,
  s_buffer_flag_mapped       = 4,
}
dt_buffer_flags_t;

typedef struct dt_buffer_t
{
  VkBuffer       buf;
  uint64_t       adr;
  void          *map;
  VkDeviceMemory mem;
  VkDeviceSize   siz;
  char           dbg[128];
}
dt_buffer_t;

VkResult dt_check_device_allocation(uint64_t size, int heap_index);
VkResult dt_buffer_alloc_debug(dt_buffer_t *b, int line, const char *file, uint64_t size, VkBufferUsageFlags usage, dt_buffer_flags_t flags);
// VkResult dt_buffer_alloc(dt_buffer_t *b, uint64_t size, VkBufferUsageFlags usage, dt_buffer_flags_t flags);
#define dt_buffer_alloc(b, s, u, f) dt_buffer_alloc_debug(b, __LINE__, __FILE__, s, u, f)
void dt_buffer_init(dt_buffer_t *b);
void dt_buffer_free(dt_buffer_t *b);
