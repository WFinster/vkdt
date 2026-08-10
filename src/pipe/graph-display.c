// double buffering / display queue interfacing

// wiring:
// TODO wire dt_graph_display_image_cmd_copy just before submitting to the graph queue
// TODO render_{darkroom,nodes}.c when rendering dt_image(), pass the dset of the dt_graph_display_image_t, not of the dt_node_t
// TODO acquire display during rendering

// cleanup:
// TODO only do mipmaps here, not in graph (don't assign s_conn_mipmap and s_conn_concurrent)
// TODO don't double buffer displays in the graph (don't assign s_conn_double_buffer and don't propagate)

// this almost works like s_conn_double_buffer, only that it destroys/inits the resources
// for the double buffer in question only when the command buffer is recorded. the current
// method frees all memory (both double buffers) in case the graph topology is changed.
// this out-of-band buffer here allows the render thread to display the output images
// for as long as it has to.

// the render thread can only lock one of the buffers (only one submit in flight)
// the other should be free for us to delete/overwrite.
// we need a triple buffer for the worst case:
//  (a) one buffer in use by render dispatch (from older graph config, different size, say)
//  (b) one buffer in use by graph dispatch  (new graph config)
//  (c) one buffer needs to be allocated by the main thread when recording the command buffer
// note that we don't have to keep them around indefinitely and can clean up when appropriate.
// for instance, once the render dispatch finished, (a) can be cleaned out in favour of (c)
// and regular operation requires only two buffers.

typedef enum dt_graph_display_t
{
  s_graph_display_main  = 0,
  s_graph_display_hist,
  s_graph_display_dspy,
  s_graph_display_view0,
  s_graph_display_view1,
  s_graph_display_cnt,
}
dt_graph_display_t;

typedef struct dt_graph_display_image_t
{
  dt_conector_image_t con;    // fake connector image which holds image + views and mipmap info
  VkDescriptorSet     dset;
  VkDeviceMemory      mem;
  dt_token_t          format; // e.g. f16
  dt_token_t          chan;   // e.g. rgba
  uint64_t            val;    // global timeline semaphore value.
}
dt_graph_display_image_t;

// TODO clarify lifetime of this. probably global/gui not graph
typedef struct dt_graph_display_images_t
{
  uint64_t timeline_display; // this is the image currently used by display/ui dispatches
  uint64_t timeline_render;  // this is the image currently in flight for graph dispatch (>=timeline_display, == if done and not rendering)
  dt_graph_display_image_t display_image[s_graph_display_end][3];
}
dt_graph_display_images_t;

int // return 1 if image with this timeline value is ready for display
dt_graph_display_image_ready_for_display(
    uint64_t val)
{
  uint64_t value;
  VkResult res = vkGetSemaphoreCounterValue(qvk.device, vkdt.graph_dev.semaphore_process, &value);
  return val <= value;
}

// do this all the time, potentially not incrementing image timeline values:
uint64_t 
dt_graph_display_acquire_for_display(
    dt_graph_display_images_t *dspy)
{
  // image is done processing (signalled by graph dispatch)
  // query semaphore value of graph dispatch signal. don't ever wait, just use what we have:
  uint64_t value;
  VkResult res = vkGetSemaphoreCounterValue(qvk.device, vkdt.graph_dev.semaphore_process, &value);
  if(res == VK_SUCCESS && value > dspy.timeline_display)
    dspy->timeline_display = value;
  // there is always a buffer with the given value
  return dspy->timeline_display;
}

// only do this if we're actually processing anything new:
uint64_t 
dt_graph_display_acquire_for_processing(
    dt_graph_t *graph)
{
  dt_graph_display_images_t *dspy = graph->dspy;
  if(!dspy) return graph->frame; // no display/ui interaction, return good semaphore value, don't double buffer the output

  uint64_t value_process = 0; // what processing semaphore finished
  VkResult res = vkGetSemaphoreCounterValue(qvk.device, vkdt.graph_dev.semaphore_process, &value_process);
  if(res != VK_SUCCESS) value_process = dspy->timeline_process - 1; // well, fuck

  assert(value <= dspy->timeline_process);
  assert(dspy->timeline_display <= dspy->timeline_process);

  // display dispatch in flight might still be timeline_display - 1 if the cpu thread just picked up timeline_display
  uint64_t value_display = 0; // what processing semaphore finished
  VkResult res = vkGetSemaphoreCounterValue(qvk.device, vkdt.graph_dev.semaphore_display, &value_display);
  if(res != VK_SUCCESS) value_display = dspy->timeline_display - 1; // well, fuck

  int slot = -1;
  for(int i=0;i<3;i++)
  { // find display image triple buffer slot for us
    uint64_t tv = dspy->display_image[0][i].val;   // run on main output, the others follow the same logic
    int free = 0;                         // assume still locked
    if(tv <= value_process) free = 1;     // all buffers that finished processing are kinda free
    if(tv >= value_display) free = 0;     // unless they are used by the current display dispatch
    if(free && slot >= 0) // already have what we need, locked buffer for display + new buffer for processing. the other one can go:
      dt_graph_display_image_cleanup(dspy->display_image[0]+i, graph);
    else if(free && slot < 0) slot = i;
  }

  assert(slot >= 0);

  // this is what we want to stamp on the new image buffer:
  dspy->timeline_process++;

  dt_token_t dn[] = {
    dt_token("main"),
    dt_token("hist"),
    dt_token("dspy"),
    dt_token("view0"),
    dt_token("view1"),
  };

  int cnt = 0;
  dt_node_t *const arr = graph->node;
  const int arr_cnt = graph->num_nodes;
  uint32_t nodeid[2000];
  if(sizeof(nodeid)/sizeof(nodeid[0]) < graph->num_nodes)
  {
    dt_log(s_log_pipe|s_log_err, "too many nodes in graph!");
    return 0;
  }
#define TRAVERSE_POST {\
    nodeid[cnt++] = curr;\
  }
#include "graph-traverse.inc"
  // TODO cache display nodes after create nodes?
  for(int n=0;n<cnt;n++)
  {
    dt_node_t *node = graph->node + nodeid[n];
    if(node->module->name == dt_token("display"))
    {
      for(int k=0;k<s_graph_display_cnt;k++)
      {
        if(node->module->inst == dn[k])
        { // init/copy buffer:
          VkResult res = dt_graph_display_image_cmd_copy(
              dspy->display_image[k] + slot, dspy->timeline_process, graph, node);
        }
      }
    }
  }
  return dspy->timeline_process;
}

static inline void
dt_graph_display_image_cleanup(
    dt_graph_display_image_t *d,
    dt_graph_t *graph)
{
  if(d->con.image)      vkDestroyImage      (qvk.device, d->img,  0);
  if(d->con.image_view) vkDestroyImageView  (qvk.device, d->view, 0);
  if(d->mem)            vkFreeMemory        (qvk.device, d->mem,  0);
  if(d->dset)           vkFreeDescriptorSets(qvk.device, graph->dset_pool, 1, &d->dset);
  for(int m=0;m<LENGTH(d->con.mipmap_views);m++)
    if(d->con.mipmap_views[m]) vkDestroyImageView(qvk.device, d->con.mipmap_views[m], 0);
  memset(d, 0, sizeof(*d));
}

static inline int // return 1 if display image is compatible with given display node
dt_graph_display_image_check_compat(
    dt_graph_display_image_t *d,
    dt_graph_t               *graph, // for the dset_pool
    dt_node_t                *node)  // the display node
{
  int wd = node->connector[0].roi.wd;
  int ht = node->connector[0].roi.wd;
  // number of mip levels is determined by resolution, not checking explicitly
  return d->wd == wd && d->ht == ht && d->format == node->connector[0].format && d->chan = node->connector[0].chan;
}

static inline VkResult
dt_graph_display_image_init(
    dt_graph_display_image_t *d,
    dt_graph_t               *graph, // for the dset_pool
    dt_node_t                *node)  // the display node
{
  int wd = node->connector[0].roi.wd;
  int ht = node->connector[0].roi.wd;
  d->con.wd = wd;
  d->con.ht = ht;
  d->format = node->connector[0].format;
  d->chan   = node->connector[0].chan;
  d->con.mip_levels = MIN(4, (uint32_t)(log2f(MAX(wd, ht))) + 1);
  uint32_t queue_indices[] = { qvk.queue_family_graphics, qvk.queue_family_compute };
  VkFormat format = dt_connector_vkformat(node->connector);
  dt_connector_image_t *con_node = dt_graph_connector_image(graph, node - graph->node, 0, 0, 0);
  VkImageCreateInfo images_create_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = format,
    .extent = {
      .width  = wd,
      .height = ht,
      .depth  = 1
    },
    .mipLevels             = d->con.mip_levels,
    .arrayLayers           = 1,
    .samples               = VK_SAMPLE_COUNT_1_BIT,
    .tiling                = VK_IMAGE_TILING_OPTIMAL,
    .usage                 = 
      VK_IMAGE_USAGE_STORAGE_BIT
      | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
      | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      | VK_IMAGE_USAGE_SAMPLED_BIT
    .sharingMode           = VK_SHARING_MODE_CONCURRENT,
    .queueFamilyIndexCount = 2,
    .pQueueFamilyIndices   = queue_indices,
    .initialLayout         = VK_IMAGE_LAYOUT_GENERAL,
  };
  QVKR(vkCreateImage(qvk.device, &images_create_info, 0, &d->con.image));
  VkMemoryRequirements mem_req;
  vkGetImageMemoryRequirements(qvk.device, d->con.image, mem_req);
  dt_check_device_allocation(mem_req.size, 0);
  VkMemoryAllocateInfo mem_alloc_info = {
    .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize  = mem_req.size,
    .memoryTypeIndex = qvk_memory_get_device(),
  };
  QVKR(vkAllocateMemory(qvk.device, &mem_alloc_info, 0, &d->mem));
  QVKR(vkBindImageMemory(qvk.device, d->con.image, d->mem, 0));
  VkImageViewCreateInfo images_view_create_info = {
    .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .viewType   = VK_IMAGE_VIEW_TYPE_2D,
    .format     = format,
    .image      = d->con.image,
    .subresourceRange = {
      .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel   = 0,
      .levelCount     = d->con.mip_levels,
      .baseArrayLayer = 0,
      .layerCount     = 1
    },
  };
  QVKR(vkCreateImageView(qvk.device, &images_view_create_info, NULL, &d->con.image_view));
  for (uint32_t m = 0; m < d->con.mip_levels; m++)
  {
    images_view_create_info.subresourceRange.baseMipLevel = m;
    images_view_create_info.subresourceRange.levelCount = 1;
    if(d->con.mipmap_views[m]) vkDestroyImageView(qvk.device, d->con.mipmap_views[m], 0);
    QVKR(vkCreateImageView(qvk.device, &images_view_create_info, NULL, &d->con.mipmap_views[m]));
  }

  VkDescriptorSetAllocateInfo dset_info = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = graph->dset_pool,
    .descriptorSetCount = 1,
    .pSetLayouts = node->dset_layout,
  };
  QVKR(vkAllocateDescriptorSets(qvk.device, &dset_info, &d->dset));
  VkDescriptorImageInfo img_info = {
    .sampler     = VK_NULL_HANDLE,
    .imageView   = d->view,
    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  VkWriteDescriptorSet img_dset = {
    .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
    .dstSet          = d->dset,
    .dstBinding      = 0,
    .descriptorCount = 1,
    .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .pImageInfo      = img_info,
  };
  vkUpdateDescriptorSets(qvk.device, 1, &img_dset, 0, NULL);
  return VK_SUCCESS;
}

static inline VkResult
dt_graph_display_image_cmd_copy(
    dt_graph_display_image_t *d,     // pointing to the display image to write to
    uint64_t                  val,   // timeline value to set on d
    dt_graph_t               *graph, // the graph with the descriptor set pool
    dt_node_t                *node)  // the display node
{
  if(!dt_graph_display_image_check_compat(d, graph, node))
  { // cleanup and allocate now
    dt_graph_display_image_cleanup(d, graph);
    dt_graph_display_image_init(d, graph, node);
  }
  d->val = val;

  VkCommandBuffer cmd_buf = dt_graph_cmd_buf(graph);
  dt_connector_image_t *img = dt_graph_connector_image(graph, node - graph->node, 0, 0, graph->double_buffer);
  VkImageCopy cpy = {
    .srcSubresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .baseArrayLayer = 0,
      .layerCount = 1,
    },
    .dstSubresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .baseArrayLayer = 0,
      .layerCount = 1,
    },
    .extent = { d->con.wd, d->con.ht },
  };
  // this can apparently run in compute only, if we only copy colour:
  vkCmdCopyImage(cmd_buf, img->image, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1 &cpy);
  dt_graph_generate_mipmaps(graph, 0, 0, &d->con);
}
