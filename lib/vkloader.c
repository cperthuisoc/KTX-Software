/* -*- tab-width: 4; -*- */
/* vi: set sw=2 ts=4 expandtab: */

/*
 * ©2018 Mark Callow.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
/**
 * @internal
 * @file
 * @~English
 *
 * @brief Functions for instantiating Vulkan textures from KTX files.
 *
 * @author Mark Callow, Edgewise Consulting
 */

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
/* For GL format tokens */
#include "GL/glcorearb.h"
#include "GL/glext.h"

#include "ktxvulkan.h"
#include "ktxint.h"
#include "vk_format.h"

// Macro to check and display Vulkan return results.
// Use when the only possible errors are caused by invalid usage by this loader.
#if defined(_DEBUG)
#define VK_CHECK_RESULT(f)                                                  \
{                                                                           \
    VkResult res = (f);                                                     \
    if (res != VK_SUCCESS)                                                  \
    {                                                                       \
        /* XXX Find an errorString function. */                             \
        fprintf(stderr, "Fatal error in ktxLoadVkTexture*: "                \
                "VkResult is \"%d\" in %s at line %d\n",                    \
                res, __FILE__, __LINE__);                                   \
        assert(res == VK_SUCCESS);                                          \
    }                                                                       \
}
#else
#define VK_CHECK_RESULT(f) ((void)f)
#endif

#define ARRAY_LEN(a) (sizeof(a) / sizeof(a[0]))

#define DEFAULT_FENCE_TIMEOUT 100000000000
#define VK_FLAGS_NONE 0

static void
setImageLayout(
    VkCommandBuffer cmdBuffer,
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkImageSubresourceRange subresourceRange);

static void
generateMipmaps(ktxVulkanTexture* vkTexture, ktxVulkanDeviceInfo* vdi,
                VkFilter filter, VkImageLayout initialLayout);

/**
 * @defgroup ktx_vkloader Vulkan Texture Image Loader
 * @brief Create texture images on a Vulkan device.
 * @{
 */

/**
 * @example vkload.cpp
 * This shows how to create and load a Vulkan image using the Vulkan texture
 * image loading functions.
 */

/**
 * @memberof ktxVulkanDeviceInfo
 * @~English
 * @brief Create a ktxVulkanDeviceInfo object.
 * 
 * Allocates CPU memory for a ktxVulkanDeviceInfo object then calls
 * ktxVulkanDeviceInfo_construct(). See it for documentation of the
 * parameters.
 *
 * @return a pointer to the constructed ktxVulkanDeviceInfo.
 *
 * @sa ktxVulkanDeviceInfo_construct(), ktxVulkanDeviceInfo_destroy()
 */
ktxVulkanDeviceInfo*
ktxVulkanDeviceInfo_Create(VkPhysicalDevice physicalDevice, VkDevice device,
                           VkQueue queue, VkCommandPool cmdPool,
                           const VkAllocationCallbacks* pAllocator)
{
    ktxVulkanDeviceInfo* newvdi;
    newvdi = (ktxVulkanDeviceInfo*)malloc(sizeof(ktxVulkanDeviceInfo));
    if (newvdi != NULL) {
        if (ktxVulkanDeviceInfo_Construct(newvdi, physicalDevice, device,
                                    queue, cmdPool, pAllocator) != KTX_SUCCESS)
        {
            free(newvdi);
            newvdi = 0;
        }
    }
    return newvdi;
}

/**
 * @memberof ktxVulkanDeviceInfo
 * @~English
 * @brief Construct a ktxVulkanDeviceInfo object.
 *
 * Records the device information, allocates a command buffer that will be
 * used to transfer image data to the Vulkan device and retrieves the physical
 * device memory properties for ease of use when allocating device memory for
 * the images.
 *
 * Pass a valid ktxVulkanDeviceInfo* to any Vulkan KTX image loading
 * function to provide it with the information.
 *
 * @param  This            pointer to the ktxVulkanDeviceInfo object to
 *                        initialize.
 * @param  physicalDevice handle of the Vulkan physical device.
 * @param  device         handle of the Vulkan logical device.
 * @param  queue          handle of the Vulkan queue.
 * @param  cmdPool        handle of the Vulkan command pool.
 * @param  pAllocator     pointer to the allocator to use for the image
 *                        memory. If NULL, the default allocator will be used.
 *
 * @returns KTX_SUCCESS on success, KTX_OUT_OF_MEMORY if a command buffer could
 *          not be allocated.
 *
 * @sa ktxVulkanDeviceInfo_destruct()
 */
KTX_error_code
ktxVulkanDeviceInfo_Construct(ktxVulkanDeviceInfo* This,
                              VkPhysicalDevice physicalDevice, VkDevice device,
                              VkQueue queue, VkCommandPool cmdPool,
                              const VkAllocationCallbacks* pAllocator)
{
    VkCommandBufferAllocateInfo cmdBufInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    VkResult result;

    This->physicalDevice = physicalDevice;
    This->device = device;
    This->queue = queue;
    This->cmdPool = cmdPool;
    This->pAllocator = pAllocator;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice,
                                        &This->deviceMemoryProperties);

    // Use a separate command buffer for texture loading. Needed for
    // submitting image barriers and converting tilings.
    cmdBufInfo.commandPool = cmdPool;
    cmdBufInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdBufInfo.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &cmdBufInfo, &This->cmdBuffer);
    if (result != VK_SUCCESS) {
        return KTX_OUT_OF_MEMORY; // XXX Consider an equivalent to pGlError
    }
    return KTX_SUCCESS;
}

/**
 * @memberof ktxVulkanDeviceInfo
 * @~English
 * @brief Destruct a ktxVulkanDeviceInfo object.
 *
 * Frees the command buffer.
 *
 * @param This pointer to the ktxVulkanDeviceInfo to destruct.
 */
void
ktxVulkanDeviceInfo_Destruct(ktxVulkanDeviceInfo* This)
{
    vkFreeCommandBuffers(This->device, This->cmdPool, 1,
                         &This->cmdBuffer);
}

/**
 * @memberof ktxVulkanDeviceInfo
 * @~English
 * @brief Destroy a ktxVulkanDeviceInfo object.
 *
 * Calls ktxVulkanDeviceInfo_destruct() then frees the ktxVulkanDeviceInfo.
 *
 * @param This pointer to the ktxVulkanDeviceInfo to destroy.
 */
void
ktxVulkanDeviceInfo_Destroy(ktxVulkanDeviceInfo* This)
{
    assert(This != NULL);
    ktxVulkanDeviceInfo_Destruct(This);
    free(This);
}

/* Get appropriate memory type index for a memory allocation. */
static uint32_t
ktxVulkanDeviceInfo_getMemoryType(ktxVulkanDeviceInfo* This,
                                  uint32_t typeBits, VkFlags properties)
{
    for (uint32_t i = 0; i < 32; i++)
    {
        if ((typeBits & 1) == 1)
        {
            if ((This->deviceMemoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }
        typeBits >>= 1;
    }

    // XXX : throw error
    return 0;
}

//======================================================================
//  ReadImages callbacks
//======================================================================

typedef struct user_cbdata_optimal {
    VkBufferImageCopy* region; // specify destination region in final image.
    VkDeviceSize offset;       // Offset of current level in staging buffer
    ktx_uint32_t numFaces;
    ktx_uint32_t numLayers;
#if defined(_DEBUG)
    VkBufferImageCopy* regionsArrayEnd;   //  "
#endif
} user_cbdata_optimal;

/*
 * Callback for an optimally tiled texture. Set up a copy region for
 * the miplevel.
 */
KTX_error_code KTXAPIENTRY
optimalTilingCallback(int miplevel, int face,
                      int width, int height, int depth,
                      ktx_uint32_t faceLodSize,
                      void* pixels, void* userdata)
{
    user_cbdata_optimal* ud = (user_cbdata_optimal*)userdata;

    // Set up copy to destination region in final image
    assert(ud->region < ud->regionsArrayEnd);
    ud->region->bufferOffset = ud->offset;
    ud->offset += faceLodSize;
    // XXX Handle row padding for uncompressed textures. KTX specifies
    // GL_UNPACK_ALIGNMENT of 4 so need to pad this from actual width.
    // That means I need the element size and group size for the format
    // to calculate bufferRowLength.
    ud->region->bufferRowLength = 0;
    ud->region->bufferImageHeight = 0;
    ud->region->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ud->region->imageSubresource.mipLevel = miplevel;
    ud->region->imageSubresource.baseArrayLayer = face;
    ud->region->imageSubresource.layerCount = ud->numLayers * ud->numFaces;
    ud->region->imageOffset.x = 0;
    ud->region->imageOffset.y = 0;
    ud->region->imageOffset.z = 0;
    ud->region->imageExtent.width = width;
    ud->region->imageExtent.height = height;
    ud->region->imageExtent.depth = depth;

    ud->region += 1; // XXX Probably need some check of the array length.

    return KTX_SUCCESS;
}

typedef struct user_cbdata_linear {
    VkImage destImage;
    VkDevice device;
    uint8_t* dest;   // Pointer to mapped Image memory
} user_cbdata_linear;


/*
 * Callback for linear tiled textures.
 * Copy the image data into the Vulkan image.
 */
KTX_error_code KTXAPIENTRY
linearTilingCallback(int miplevel, int face,
                      int width, int height, int depth,
                      ktx_uint32_t faceLodSize,
                      void* pixels, void* userdata)
{
    user_cbdata_linear* ud = (user_cbdata_linear*)userdata;
    VkSubresourceLayout subResLayout;
    VkImageSubresource subRes = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = miplevel,
      .arrayLayer = face
    };

    // Get sub resources layout
    // Includes row pitch, size offsets, etc.
    vkGetImageSubresourceLayout(ud->device, ud->destImage, &subRes, &subResLayout);

    // Copy image data to destImage via its mapped memory.
    // XXX How to handle subResLayout.{array,depth,row}Pitch?
    //     Problem if rowPitch is not a multiple of 4. Really don't want
    //     copy a row at a time. For now
    if ((subResLayout.rowPitch & 0x3) != 0)
        return KTX_INVALID_OPERATION;
    // XXX We receive all the array levels in one lump. Will this work?
    memcpy(ud->dest + subResLayout.offset, pixels, faceLodSize);
    return KTX_SUCCESS;
}

/**
 * @memberof ktxTexture
 * @~English
 * @brief Create a Vulkan image object from a ktxTexture object.
 *
 * Creates a VkImage with @c VkFormat etc. matching the KTX data and uploads
 * the images. Also creates a VkImageView object for accessing the image.
 * Mipmaps will be generated if the @c ktxTexture's @c generateMipmaps
 * flag is set. Returns the handles of the created objects and information
 * about the texture in the @c ktxVulkanTexture pointed at by @p vkTexture.
 *
 * @p usageFlags and thus acceptable usage of the created image may be
 * augmented as follows:
 * - with @c VK_IMAGE_USAGE_TRANSFER_DST_BIT if @p tiling is
 *   @c VK_IMAGE_TILING_OPTIMAL
 * - with <code>VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT</code>
 *   if @c generateMipmaps is set in the @c ktxTexture.
 *
 * Most Vulkan implementations support VK_IMAGE_TILING_LINEAR only for a very
 * limited number of formats and features. Generally VK_IMAGE_TILING_OPTIMAL is
 * preferred. The latter requires a staging buffer so will use more memory
 * during loading.
 *
 * @param[in] This          pointer to the ktxTexture from which to upload.
 * @param [in] vdi          pointer to a ktxVulkanDeviceInfo structure providing
 *                          information about the Vulkan device onto which to
 *                          load the texture.
 * @param [in,out] vkTexture pointer to a ktxVulkanTexture structure into which
 *                           the function writes information about the created
 *                           VkImage.
 * @param [in] tiling       type of tiling to use in the destination image
 *                          on the Vulkan device.
 * @param [in] usageFlags   a set of VkImageUsageFlags bits indicating the
 *                          intended usage of the destination image.
 * @param [in] finalLayout  a VkImageLayout value indicating the desired
 *                          final layout of the created image.
 *
 * @return  KTX_SUCCESS on success, other KTX_* enum values on error.
 *
 * @exception KTX_INVALID_VALUE @p This, @p vdi or @p vkTexture is @c NULL.
 * @exception KTX_INVALID_OPERATION The ktxTexture contains neither images nor
 *                                  an active stream from which to read them.
 * @exception KTX_INVALID_OPERATION The combination of the ktxTexture's format,
 *                                  @p tiling and @p usageFlags is not supported
 *                                  by the physical device.
 * @exception KTX_INVALID_OPERATION Requested mipmap generation is not supported
 *                                  by the physical device for the combination
 *                                  of the ktxTexture's format and @p tiling.
 * @exception KTX_OUT_OF_MEMORY Sufficient memory could not be allocated
 *                              on either the CPU or the Vulkan device.
 */
KTX_error_code
ktxTexture_VkUploadEx(ktxTexture* This, ktxVulkanDeviceInfo* vdi,
                      ktxVulkanTexture* vkTexture,
                      VkImageTiling tiling,
                      VkImageUsageFlags usageFlags,
                      VkImageLayout finalLayout)
{
    KTX_error_code           kResult;
    VkFilter                 blitFilter;
    VkFormat                 vkFormat;
    VkImageType              imageType;
    VkImageViewType          viewType;
    VkImageCreateFlags       createFlags = 0;
    VkImageFormatProperties  imageFormatProperties;
    VkResult                 vResult;
    VkCommandBufferBeginInfo cmdBufBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = NULL
    };
    VkImageCreateInfo        imageCreateInfo = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .pNext = NULL
    };
    VkMemoryAllocateInfo     memAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = NULL,
        .allocationSize = 0,
        .memoryTypeIndex = 0
    };
    VkMemoryRequirements     memReqs;
    uint32_t                 numImageLayers, numImageLevels;

    if (!vdi || !This || !vkTexture) {
        return KTX_INVALID_VALUE;
    }

    if (!This->pData && !ktxTexture_isActiveStream(This)) {
        /* Nothing to upload. */
        return KTX_INVALID_OPERATION;
    }

    /* _ktxCheckHeader should have caught this. */
    assert(This->numFaces == 6 ? This->numDimensions == 2 : VK_TRUE);

    numImageLayers = This->numLayers;
    if (This->isCubemap) {
        numImageLayers *= 6;
        createFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    switch (This->numDimensions) {
      case 1:
        imageType = VK_IMAGE_TYPE_1D;
        viewType = This->isArray ?
                        VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        break;
      case 2:
        imageType = VK_IMAGE_TYPE_2D;
        if (This->isCubemap)
            viewType = This->isArray ?
                        VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        else
            viewType = This->isArray ?
                        VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        break;
      case 3:
        imageType = VK_IMAGE_TYPE_3D;
        /* 3D array textures not supported in Vulkan. Attempts to create or
         * load them should have been trapped long before this.
         */
        assert(!This->isArray);
        viewType = VK_IMAGE_VIEW_TYPE_3D;
        break;
    }

    vkFormat = vkGetFormatFromOpenGLInternalFormat(This->glInternalformat);
    if (vkFormat == VK_FORMAT_UNDEFINED)
        vkFormat = vkGetFormatFromOpenGLFormat(This->glFormat, This->glType);
    if (vkFormat == VK_FORMAT_UNDEFINED) {
        return KTX_INVALID_OPERATION;
    }

    /* Get device properties for the requested image format */
    if (tiling == VK_IMAGE_TILING_OPTIMAL) {
        // Ensure we can copy from staging buffer to image.
        usageFlags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (This->generateMipmaps) {
        // Ensure we can blit between levels.
        usageFlags |= (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    vResult = vkGetPhysicalDeviceImageFormatProperties(vdi->physicalDevice,
                                                      vkFormat,
                                                      imageType,
                                                      tiling,
                                                      usageFlags,
                                                      createFlags,
                                                      &imageFormatProperties);
    if (vResult == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        return KTX_INVALID_OPERATION;
    }

    if (This->generateMipmaps) {
        uint32_t max_dim;
        VkFormatProperties    formatProperties;
        VkFormatFeatureFlags  formatFeatureFlags;
        VkFormatFeatureFlags  neededFeatures
            = VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        vkGetPhysicalDeviceFormatProperties(vdi->physicalDevice,
                                            vkFormat,
                                            &formatProperties);
        assert(vResult == VK_SUCCESS);
        if (tiling == VK_IMAGE_TILING_OPTIMAL)
            formatFeatureFlags = formatProperties.optimalTilingFeatures;
        else
            formatFeatureFlags = formatProperties.linearTilingFeatures;

        if ((formatFeatureFlags & neededFeatures) != neededFeatures)
            return KTX_INVALID_OPERATION;

        if (formatFeatureFlags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            blitFilter = VK_FILTER_LINEAR;
        else
            blitFilter = VK_FILTER_NEAREST; // XXX INVALID_OP?

        max_dim = MAX(MAX(This->baseWidth, This->baseHeight), This->baseDepth);
        numImageLevels = (uint32_t)floor(log2(max_dim)) + 1;
    } else {
        numImageLevels = This->numLevels;
    }

    vkTexture->width = This->baseWidth;
    vkTexture->height = This->baseHeight;
    vkTexture->depth = This->baseDepth;
    vkTexture->imageLayout = finalLayout;
    vkTexture->imageFormat = vkFormat;
    vkTexture->levelCount = numImageLevels;
    vkTexture->layerCount = numImageLayers;
    vkTexture->viewType = viewType;

    VK_CHECK_RESULT(vkBeginCommandBuffer(vdi->cmdBuffer, &cmdBufBeginInfo));

    if (tiling == VK_IMAGE_TILING_OPTIMAL)
    {
        // Create a host-visible staging buffer that contains the raw image data
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        VkBufferImageCopy* copyRegions;
        VkDeviceSize textureSize;
        VkBufferCreateInfo bufferCreateInfo = {
          .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
          .pNext = NULL
        };
        VkImageSubresourceRange subresourceRange;
        VkFence copyFence;
        VkFenceCreateInfo fenceCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = NULL,
            .flags = VK_FLAGS_NONE
        };
        VkSubmitInfo submitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = NULL
        };
        ktx_uint8_t* pMappedStagingBuffer;

        /*
         * Because all array layers and faces are the same size they can be
         * copied in a single operation so there'll be 1 copy per mip level.
         */
        uint32_t numCopyRegions = This->numLevels;
        user_cbdata_optimal cbData;

        textureSize = ktxTexture_GetSize(This);

        copyRegions = (VkBufferImageCopy*)malloc(sizeof(VkBufferImageCopy)
                                                   * numCopyRegions);
        if (copyRegions == NULL) {
            return KTX_OUT_OF_MEMORY;
        }

        bufferCreateInfo.size = textureSize;
        // This buffer is used as a transfer source for the buffer copy
        bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK_CHECK_RESULT(vkCreateBuffer(vdi->device, &bufferCreateInfo,
                                       vdi->pAllocator, &stagingBuffer));

        // Get memory requirements for the staging buffer (alignment,
        // memory type bits)
        vkGetBufferMemoryRequirements(vdi->device, stagingBuffer, &memReqs);

        memAllocInfo.allocationSize = memReqs.size;
        // Get memory type index for a host visible buffer
        memAllocInfo.memoryTypeIndex = ktxVulkanDeviceInfo_getMemoryType(
                vdi,
                memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
              | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );

        vResult = vkAllocateMemory(vdi->device, &memAllocInfo,
                                  vdi->pAllocator, &stagingMemory);
        if (vResult != VK_SUCCESS) {
            return KTX_OUT_OF_MEMORY;
        }
        VK_CHECK_RESULT(vkBindBufferMemory(vdi->device, stagingBuffer,
                                           stagingMemory, 0));

        VK_CHECK_RESULT(vkMapMemory(vdi->device, stagingMemory, 0,
                                    memReqs.size, 0,
                                    (void **)&pMappedStagingBuffer));

        cbData.offset = 0;
        cbData.region = copyRegions;
        cbData.numFaces = This->numFaces;
        cbData.numLayers = This->numLayers;
#if defined(_DEBUG)
        cbData.regionsArrayEnd = copyRegions + numCopyRegions;
#endif

        if (This->pData) {
            /* Image data has already been loaded. Copy to staging buffer. */
            assert(This->dataSize <= memAllocInfo.allocationSize);
            memcpy(pMappedStagingBuffer, This->pData, This->dataSize);
        } else {
            /* Load the image data directly into the staging buffer. */
            /* The strange cast quiets an Xcode warning when building for
             * Generic iOS Device where size_t is 32-bit even when building
             * for arm64. */
            kResult = ktxTexture_LoadImageData(This,
                                      pMappedStagingBuffer,
                                      (ktx_size_t)memAllocInfo.allocationSize);
            if (kResult != KTX_SUCCESS)
                return kResult;
        }
        
        // Iterate over mip levels to set up the copy regions.
        kResult = ktxTexture_IterateLevels(This,
                                           optimalTilingCallback,
                                           &cbData);
        // XXX Check for possible errors

        vkUnmapMemory(vdi->device, stagingMemory);

        // Create optimal tiled target image
        imageCreateInfo.imageType = imageType;
        imageCreateInfo.flags = createFlags;
        imageCreateInfo.format = vkFormat;
        // numImageLevels ensures enough levels for generateMipmaps.
        imageCreateInfo.mipLevels = numImageLevels;
        imageCreateInfo.arrayLayers = numImageLayers;
        imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageCreateInfo.usage = usageFlags;
        imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageCreateInfo.extent.width = vkTexture->width;
        imageCreateInfo.extent.height = vkTexture->height;
        imageCreateInfo.extent.depth = vkTexture->depth;

        VK_CHECK_RESULT(vkCreateImage(vdi->device, &imageCreateInfo,
                                      vdi->pAllocator, &vkTexture->image));

        vkGetImageMemoryRequirements(vdi->device, vkTexture->image, &memReqs);

        memAllocInfo.allocationSize = memReqs.size;

        memAllocInfo.memoryTypeIndex = ktxVulkanDeviceInfo_getMemoryType(
                                          vdi, memReqs.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK_RESULT(vkAllocateMemory(vdi->device, &memAllocInfo,
                                         vdi->pAllocator,
                                         &vkTexture->deviceMemory));
        VK_CHECK_RESULT(vkBindImageMemory(vdi->device, vkTexture->image,
                                          vkTexture->deviceMemory, 0));

        subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        subresourceRange.baseMipLevel = 0;
        subresourceRange.levelCount = This->numLevels;
        subresourceRange.baseArrayLayer = 0;
        subresourceRange.layerCount = numImageLayers;

        // Image barrier to transition, possibly only the base level, image
        // layout to TRANSFER_DST_OPTIMAL so it can be used as the copy
        // destination.
        setImageLayout(
            vdi->cmdBuffer,
            vkTexture->image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            subresourceRange);

        // Copy mip levels from staging buffer
        vkCmdCopyBufferToImage(
            vdi->cmdBuffer, stagingBuffer,
            vkTexture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            numCopyRegions, copyRegions
            );

        if (This->generateMipmaps) {
            generateMipmaps(vkTexture, vdi,
                            blitFilter, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        } else {
            // Transition image layout to finalLayout after all mip levels
            // have been copied.
            // In this case numImageLevels == This->numLevels
            //subresourceRange.levelCount = numImageLevels;
            setImageLayout(
                vdi->cmdBuffer,
                vkTexture->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                //currentLayout,
                finalLayout,
                subresourceRange);
        }

        // Submit command buffer containing copy and image layout commands
        VK_CHECK_RESULT(vkEndCommandBuffer(vdi->cmdBuffer));

        // Create a fence to make sure that the copies have finished before
        // continuing
        VK_CHECK_RESULT(vkCreateFence(vdi->device, &fenceCreateInfo,
                                      vdi->pAllocator, &copyFence));

        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &vdi->cmdBuffer;

        VK_CHECK_RESULT(vkQueueSubmit(vdi->queue, 1, &submitInfo, copyFence));

        VK_CHECK_RESULT(vkWaitForFences(vdi->device, 1, &copyFence,
                                        VK_TRUE, DEFAULT_FENCE_TIMEOUT));

        vkDestroyFence(vdi->device, copyFence, vdi->pAllocator);

        // Clean up staging resources
        vkFreeMemory(vdi->device, stagingMemory, vdi->pAllocator);
        vkDestroyBuffer(vdi->device, stagingBuffer, vdi->pAllocator);
    }
    else
    {
        VkImage mappableImage;
        VkDeviceMemory mappableMemory;
        VkFence nullFence = { VK_NULL_HANDLE };
        VkSubmitInfo submitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = NULL
        };
        //VkImageSubresourceRange subresourceRange;
        user_cbdata_linear cbData;

        imageCreateInfo.imageType = imageType;
        imageCreateInfo.flags = createFlags;
        imageCreateInfo.format = vkFormat;
        imageCreateInfo.extent.width = vkTexture->width;
        imageCreateInfo.extent.height = vkTexture->height;
        imageCreateInfo.extent.depth = vkTexture->depth;
        // numImageLevels ensures enough levels for generateMipmaps.
        imageCreateInfo.mipLevels = numImageLevels;
        imageCreateInfo.arrayLayers = numImageLayers;
        imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
        imageCreateInfo.usage = usageFlags;
        imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

        // Load mip map level 0 to linear tiling image
        VK_CHECK_RESULT(vkCreateImage(vdi->device, &imageCreateInfo,
                                      vdi->pAllocator, &mappableImage));

        // Get memory requirements for this image
        // like size and alignment
        vkGetImageMemoryRequirements(vdi->device, mappableImage, &memReqs);
        // Set memory allocation size to required memory size
        memAllocInfo.allocationSize = memReqs.size;

        // Get memory type that can be mapped to host memory
        memAllocInfo.memoryTypeIndex = ktxVulkanDeviceInfo_getMemoryType(
                vdi,
                memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        // Allocate host memory
        vResult = vkAllocateMemory(vdi->device, &memAllocInfo, vdi->pAllocator,
                                  &mappableMemory);
        if (vResult != VK_SUCCESS) {
            return KTX_OUT_OF_MEMORY;
        }
        VK_CHECK_RESULT(vkBindImageMemory(vdi->device, mappableImage,
                                          mappableMemory, 0));

        cbData.destImage = mappableImage;
        cbData.device = vdi->device;

        // Map image memory
        VK_CHECK_RESULT(vkMapMemory(vdi->device, mappableMemory, 0,
                        memReqs.size, 0, (void **)&cbData.dest));

        // Iterate over images to copy texture data into mapped image memory.
        if (ktxTexture_isActiveStream(This)) {
            kResult = ktxTexture_IterateLoadLevelFaces(This,
                                                       linearTilingCallback,
                                                       &cbData);
        } else {
            kResult = ktxTexture_IterateLevelFaces(This,
                                                   linearTilingCallback,
                                                   &cbData);
        }
        // XXX Check for possible errors

        vkUnmapMemory(vdi->device, mappableMemory);

        // Linear tiled images can be directly used as textures.
        vkTexture->image = mappableImage;
        vkTexture->deviceMemory = mappableMemory;

        if (This->generateMipmaps) {
            generateMipmaps(vkTexture, vdi,
                            blitFilter,
                            VK_IMAGE_LAYOUT_PREINITIALIZED);
        } else {
            VkImageSubresourceRange subresourceRange;
            subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            subresourceRange.baseMipLevel = 0;
            subresourceRange.levelCount = numImageLevels;
            subresourceRange.baseArrayLayer = 0;
            subresourceRange.layerCount = numImageLayers;

           // Transition image layout to finalLayout.
            setImageLayout(
                vdi->cmdBuffer,
                vkTexture->image,
                VK_IMAGE_LAYOUT_PREINITIALIZED,
                finalLayout,
                subresourceRange);
        }

        // Submit command buffer containing image layout commands
        VK_CHECK_RESULT(vkEndCommandBuffer(vdi->cmdBuffer));

        submitInfo.waitSemaphoreCount = 0;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &vdi->cmdBuffer;

        VK_CHECK_RESULT(vkQueueSubmit(vdi->queue, 1, &submitInfo, nullFence));
        VK_CHECK_RESULT(vkQueueWaitIdle(vdi->queue));
    }
    return KTX_SUCCESS;
}

/** @memberof ktxTexture
 * @~English
 * @brief Create a Vulkan image object from a ktxTexture object.
 *
 * Calls ktxTexture_VkUploadEx() with the most commonly used options:
 * VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT and
 * VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
 * 
 * @sa ktxTexture_VkUploadEx() for details and use that for complete
 *     control.
 */
KTX_error_code
ktxTexture_VkUpload(ktxTexture* texture, ktxVulkanDeviceInfo* vdi,
                    ktxVulkanTexture *vkTexture)
{
    return ktxTexture_VkUploadEx(texture, vdi, vkTexture,
                                 VK_IMAGE_TILING_OPTIMAL,
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/** @memberof ktxTexture
 * @~English
 * @brief Return the VkFormat enum of a ktxTexture object.
 *
 * @return The VkFormat of the ktxTexture. May return VK_FORMAT_UNDEFINED if
 *         there is no mapping from the GL internalformat and format.
 */
VkFormat
ktxTexture_GetVkFormat(ktxTexture* This)
{
    VkFormat vkFormat;

    vkFormat = vkGetFormatFromOpenGLInternalFormat(This->glInternalformat);
    if (vkFormat == VK_FORMAT_UNDEFINED)
        vkFormat = vkGetFormatFromOpenGLFormat(This->glFormat, This->glType);
    return vkFormat;
}

//======================================================================
//  Utilities
//======================================================================

/**
 * @internal
 * @~English
 * @brief Create an image memory barrier for changing the layout of an image.
 *
 * The barrier is placed in the passed command buffer. See the Vulkan spec.
 * chapter 11.4 "Image Layout" for details.
 */
static void
setImageLayout(
    VkCommandBuffer cmdBuffer,
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkImageSubresourceRange subresourceRange)
{
    // Create an image barrier object
    VkImageMemoryBarrier imageMemoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = NULL,
         // Some default values
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED
    };

    imageMemoryBarrier.oldLayout = oldLayout;
    imageMemoryBarrier.newLayout = newLayout;
    imageMemoryBarrier.image = image;
    imageMemoryBarrier.subresourceRange = subresourceRange;

    // Source layouts (old)
    // The source access mask controls actions to be finished on the old
    // layout before it will be transitioned to the new layout.
    switch (oldLayout)
    {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        // Image layout is undefined (or does not matter).
        // Only valid as initial layout. No flags required.
        imageMemoryBarrier.srcAccessMask = 0;
        break;

    case VK_IMAGE_LAYOUT_PREINITIALIZED:
        // Image is preinitialized.
        // Only valid as initial layout for linear images; preserves memory
        // contents. Make sure host writes have finished.
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        // Image is a color attachment.
        // Make sure writes to the color buffer have finished
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        // Image is a depth/stencil attachment.
        // Make sure any writes to the depth/stencil buffer have finished.
        imageMemoryBarrier.srcAccessMask
                                = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        // Image is a transfer source.
        // Make sure any reads from the image have finished
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        break;

    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        // Image is a transfer destination.
        // Make sure any writes to the image have finished.
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        // Image is read by a shader.
        // Make sure any shader reads from the image have finished
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        break;

    default:
        /* Value not used by callers, so not supported. */
        assert(KTX_FALSE);
    }

    // Target layouts (new)
    // The destination access mask controls the dependency for the new image
    // layout.
    switch (newLayout)
    {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        // Image will be used as a transfer destination.
        // Make sure any writes to the image have finished.
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        // Image will be used as a transfer source.
        // Make sure any reads from and writes to the image have finished.
        imageMemoryBarrier.srcAccessMask |= VK_ACCESS_TRANSFER_READ_BIT;
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        break;

    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        // Image will be used as a color attachment.
        // Make sure any writes to the color buffer have finished.
        imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        // Image layout will be used as a depth/stencil attachment.
        // Make sure any writes to depth/stencil buffer have finished.
        imageMemoryBarrier.dstAccessMask
                                = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;

    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        // Image will be read in a shader (sampler, input attachment).
        // Make sure any writes to the image have finished.
        if (imageMemoryBarrier.srcAccessMask == 0)
        {
            imageMemoryBarrier.srcAccessMask
                    = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        imageMemoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        break;
    default:
        /* Value not used by callers, so not supported. */
        assert(KTX_FALSE);
    }

    // Put barrier on top of pipeline.
    VkPipelineStageFlags srcStageFlags = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags destStageFlags = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    // Add the barrier to the passed command buffer
    vkCmdPipelineBarrier(
        cmdBuffer,
        srcStageFlags,
        destStageFlags,
        0,
        0, NULL,
        0, NULL,
        1, &imageMemoryBarrier);
}

/** @internal
 * @~English
 * @brief Generate mipmaps from base using @c VkCmdBlitImage.
 *
 * Mipmaps are generated by blitting level n from level n-1 as it should
 * be faster than the alternative of blitting all levels from the base level.
 *
 * After generation, the image is transitioned to the layout indicated by
 * @c vkTexture->imageLayout.
 *
 * @param[in] vkTexture     pointer to an object with information about the
 *                          image for which to generate mipmaps.
 * @param[in] vdi           pointer to an object with information about the
 *                          Vulkan device and command buffer to use.
 * @param[in] blitFilter    the type of filter to use in the @c VkCmdBlitImage.
 * @param[in] initialLayout the layout of the image on entry to the function.
 */
static void
generateMipmaps(ktxVulkanTexture* vkTexture, ktxVulkanDeviceInfo* vdi,
                VkFilter blitFilter, VkImageLayout initialLayout)
{
    VkImageSubresourceRange subresourceRange;
    memset(&subresourceRange, 0, sizeof(subresourceRange));
    subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresourceRange.baseMipLevel = 0;
    subresourceRange.levelCount = 1;
    subresourceRange.baseArrayLayer = 0;
    subresourceRange.layerCount = vkTexture->layerCount;

    // Transition base level to SRC_OPTIMAL for blitting.
    setImageLayout(
        vdi->cmdBuffer,
        vkTexture->image,
        initialLayout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        subresourceRange);

    // Generate the mip chain
    // ----------------------
    // Blit level n from level n-1.
    for (uint32_t i = 1; i < vkTexture->levelCount; i++)
    {
        VkImageBlit imageBlit;
        memset(&imageBlit, 0, sizeof(imageBlit));

        // Source
        imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlit.srcSubresource.layerCount = vkTexture->layerCount;
        imageBlit.srcSubresource.mipLevel = i-1;
        imageBlit.srcOffsets[1].x = MAX(1, vkTexture->width >> (i - 1));
        imageBlit.srcOffsets[1].y = MAX(1, vkTexture->height >> (i - 1));
        imageBlit.srcOffsets[1].z = MAX(1, vkTexture->depth >> (i - 1));;

        // Destination
        imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBlit.dstSubresource.layerCount = 1;
        imageBlit.dstSubresource.mipLevel = i;
        imageBlit.dstOffsets[1].x = MAX(1, vkTexture->width >> i);
        imageBlit.dstOffsets[1].y = MAX(1, vkTexture->height >> i);
        imageBlit.dstOffsets[1].z = MAX(1, vkTexture->depth >> i);

        VkImageSubresourceRange mipSubRange;
        memset(&mipSubRange, 0, sizeof(mipSubRange));

        mipSubRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mipSubRange.baseMipLevel = i;
        mipSubRange.levelCount = 1;
        mipSubRange.layerCount = vkTexture->layerCount;

        // Transiton current mip level to transfer dest
        setImageLayout(
            vdi->cmdBuffer,
            vkTexture->image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            mipSubRange);

        // Blit from previous level
        vkCmdBlitImage(
            vdi->cmdBuffer,
            vkTexture->image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            vkTexture->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &imageBlit,
            blitFilter);

        // Transiton current mip level to transfer source for read in
        // next iteration.
        setImageLayout(
            vdi->cmdBuffer,
            vkTexture->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            mipSubRange);
    }

    // After the loop, all mip layers are in TRANSFER_SRC layout.
    // Transition all to final layout.
    subresourceRange.levelCount = vkTexture->levelCount;
    setImageLayout(
        vdi->cmdBuffer,
        vkTexture->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        vkTexture->imageLayout,
        subresourceRange);
}

//======================================================================
//  ktxVulkanTexture utilities
//======================================================================

/**
 * @memberof ktxVulkanTexture
 * @~English
 * @brief Destructor for the object returned when loading a texture image.
 *
 * Frees the Vulkan resources created when the texture image was loaded.
 *
 * @param vkTexture  pointer to the ktxVulkanTexture to be destructed.
 * @param device     handle to the Vulkan logical device to which the texture was
 *                   loaded.
 * @param pAllocator pointer to the allocator used during loading.
 */
void
ktxVulkanTexture_Destruct(ktxVulkanTexture* vkTexture, VkDevice device,
                          const VkAllocationCallbacks* pAllocator)
{
    vkDestroyImage(device, vkTexture->image, pAllocator);
    vkFreeMemory(device, vkTexture->deviceMemory, pAllocator);
}

/** @} */