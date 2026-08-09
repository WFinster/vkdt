// double buffering / display queue interfacing
// TODO in graph, allocate more descriptor sets in pool (for s_graph_display_end many sampled images)
// TODO put dt_graph_display_images_t directly on graph
// TODO wire dt_graph_display_image_cmd_copy just before submitting to the graph queue

// TODO can we use the current set of semaphores?
// TODO render_{darkroom,nodes}.c when rendering dt_image(), pass the dset of the dt_graph_display_image_t, not of the dt_node_t
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
  uint64_t            val;    // global timeline semaphore value associated with this buffer
}
dt_graph_display_image_t;

typedef struct dt_graph_display_images_t
{
  dt_graph_display_image_t display_image[s_graph_display_end][3];
}
dt_graph_display_images_t;

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
    dt_graph_display_image_t *d,
    dt_graph_t               *graph,
    dt_node_t                *node) // the display node
{
  // TODO make sure the slot d is not in use by any active render dispatch!
  // TODO check timeline semaphore value of renderer against the value stored in d
  if(!dt_graph_display_image_check_compat(d, graph, node))
  { // cleanup and allocate now
    // TODO check all triple buffers for appropriate slot, clean up all those that aren't needed any more
    dt_graph_display_image_cleanup(d, graph);
    dt_graph_display_image_init(d, graph, node);
  }
  // TODO
  // copy timestamp/semaphore value!
  d->timeline_val = XXX;
  // this is the value that will be picked up by the next render dispatch and signaled when they are done!

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
