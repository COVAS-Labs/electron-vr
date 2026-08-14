#include <libdrm/drm_fourcc.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "../openxr_api_layer_protocol_linux.h"

namespace {
using namespace electron_vr::openxr_layer_linux;

constexpr uint32_t kAmdVendorId = 0x1002;
constexpr uint32_t kDefaultFrames = 120;
constexpr uint32_t kMinimumChanges = 3;

struct ScopedFd {
  int value = -1;
  ScopedFd() = default;
  explicit ScopedFd(int fd) : value(fd) {}
  ~ScopedFd() { if (value >= 0) close(value); }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&& other) noexcept : value(other.value) { other.value = -1; }
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      if (value >= 0) close(value);
      value = other.value;
      other.value = -1;
    }
    return *this;
  }
};

struct VulkanState {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memory_properties{};
  VkPhysicalDeviceDrmPropertiesEXT drm_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
  PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;

  ~VulkanState() {
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
      if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
  }
};

bool HasExtension(VkPhysicalDevice device, const char* name) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) return false;
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
  return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

bool InitializeVulkan(VulkanState* state, std::string* error) {
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.pApplicationName = "electron-vr Vulkan DMA-BUF probe";
  application.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &application;
  if (vkCreateInstance(&instance_info, nullptr, &state->instance) != VK_SUCCESS) {
    *error = "vkCreateInstance failed";
    return false;
  }

  uint32_t device_count = 0;
  if (vkEnumeratePhysicalDevices(state->instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
    *error = "no Vulkan physical devices found";
    return false;
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  if (vkEnumeratePhysicalDevices(state->instance, &device_count, devices.data()) != VK_SUCCESS) {
    *error = "failed to enumerate Vulkan physical devices";
    return false;
  }

  const char* required_extensions[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
  };
  for (VkPhysicalDevice candidate : devices) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(candidate, &properties);
    if (properties.vendorID != kAmdVendorId ||
        (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
         properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)) continue;
    bool extensions_available = true;
    for (const char* extension : required_extensions) extensions_available &= HasExtension(candidate, extension);
    if (!extensions_available) continue;
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
    for (uint32_t index = 0; index < family_count; ++index) {
      if (families[index].queueCount > 0 && (families[index].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0) {
        state->physical_device = candidate;
        state->properties = properties;
        state->queue_family = index;
        break;
      }
    }
    if (state->physical_device != VK_NULL_HANDLE) break;
  }
  if (state->physical_device == VK_NULL_HANDLE) {
    *error = "no hardware AMD Vulkan device with the required DMA-BUF extensions found (CPU devices are rejected)";
    return false;
  }

  VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties2.pNext = &state->drm_properties;
  vkGetPhysicalDeviceProperties2(state->physical_device, &properties2);
  vkGetPhysicalDeviceMemoryProperties(state->physical_device, &state->memory_properties);

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = state->queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = static_cast<uint32_t>(std::size(required_extensions));
  device_info.ppEnabledExtensionNames = required_extensions;
  if (vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device) != VK_SUCCESS) {
    *error = "vkCreateDevice failed";
    return false;
  }
  vkGetDeviceQueue(state->device, state->queue_family, 0, &state->queue);
  state->get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
      vkGetDeviceProcAddr(state->device, "vkGetMemoryFdPropertiesKHR"));
  if (state->get_memory_fd_properties == nullptr) {
    *error = "vkGetMemoryFdPropertiesKHR is unavailable";
    return false;
  }
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = state->queue_family;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  if (vkCreateCommandPool(state->device, &pool_info, nullptr, &state->command_pool) != VK_SUCCESS) {
    *error = "vkCreateCommandPool failed";
    return false;
  }
  return true;
}

bool FormatForSnapshot(const OverlaySnapshot& snapshot, VkFormat* format, std::string* error) {
  if (snapshot.drm_format == DRM_FORMAT_ARGB8888) {
    *format = VK_FORMAT_B8G8R8A8_UNORM;
    return true;
  }
  if (snapshot.drm_format == DRM_FORMAT_ABGR8888) {
    *format = VK_FORMAT_R8G8B8A8_UNORM;
    return true;
  }
  *error = "unsupported DRM format " + std::to_string(snapshot.drm_format);
  return false;
}

bool ValidateFormat(VulkanState& state, VkFormat format, std::string* error) {
  VkDrmFormatModifierPropertiesListEXT modifier_list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  properties.pNext = &modifier_list;
  vkGetPhysicalDeviceFormatProperties2(state.physical_device, format, &properties);
  std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(modifier_list.drmFormatModifierCount);
  modifier_list.pDrmFormatModifierProperties = modifiers.data();
  vkGetPhysicalDeviceFormatProperties2(state.physical_device, format, &properties);
  const auto linear = std::find_if(modifiers.begin(), modifiers.end(), [](const auto& modifier) {
    return modifier.drmFormatModifier == DRM_FORMAT_MOD_LINEAR && modifier.drmFormatModifierPlaneCount == 1;
  });
  if (linear == modifiers.end() || (linear->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0) {
    *error = "linear DRM modifier does not support transfer source images for the snapshot format";
    return false;
  }

  VkPhysicalDeviceExternalImageFormatInfo external_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
  modifier_info.pNext = &external_info;
  modifier_info.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
  modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkPhysicalDeviceImageFormatInfo2 image_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  image_info.pNext = &modifier_info;
  image_info.format = format;
  image_info.type = VK_IMAGE_TYPE_2D;
  image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkExternalImageFormatProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 image_properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  image_properties.pNext = &external_properties;
  if (vkGetPhysicalDeviceImageFormatProperties2(state.physical_device, &image_info, &image_properties) != VK_SUCCESS ||
      (external_properties.externalMemoryProperties.externalMemoryFeatures &
       VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0 ||
      (external_properties.externalMemoryProperties.compatibleHandleTypes &
       VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) == 0) {
    *error = "linear DRM modifier image is not importable from DMA-BUF";
    return false;
  }
  return true;
}

bool FindMemoryType(const VulkanState& state, uint32_t bits, VkMemoryPropertyFlags required,
                    uint32_t* index, VkMemoryPropertyFlags* flags = nullptr) {
  for (uint32_t i = 0; i < state.memory_properties.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags candidate = state.memory_properties.memoryTypes[i].propertyFlags;
    if ((bits & (1U << i)) != 0 && (candidate & required) == required) {
      *index = i;
      if (flags != nullptr) *flags = candidate;
      return true;
    }
  }
  return false;
}

uint64_t Checksum(const uint8_t* bytes, size_t size) {
  uint64_t checksum = 1469598103934665603ULL;
  for (size_t i = 0; i < size; ++i) {
    checksum ^= bytes[i];
    checksum *= 1099511628211ULL;
  }
  return checksum;
}

bool ImportAndChecksum(VulkanState& state, const OverlaySnapshot& snapshot, int received_fd,
                       uint64_t* checksum, std::string* error) {
  if (snapshot.width == 0 || snapshot.height == 0 || snapshot.plane_count != 1 ||
      snapshot.modifier != DRM_FORMAT_MOD_INVALID || snapshot.planes[0].stride < snapshot.width * 4ULL ||
      snapshot.width > std::numeric_limits<uint32_t>::max() / 4U ||
      snapshot.height > std::numeric_limits<VkDeviceSize>::max() / (static_cast<VkDeviceSize>(snapshot.width) * 4U) ||
      snapshot.planes[0].offset > snapshot.planes[0].size ||
      static_cast<uint64_t>(snapshot.planes[0].stride) * snapshot.height >
          snapshot.planes[0].size - snapshot.planes[0].offset) {
    *error = "snapshot does not satisfy the single-plane linear contract";
    return false;
  }
  VkFormat format = VK_FORMAT_UNDEFINED;
  if (!FormatForSnapshot(snapshot, &format, error) || !ValidateFormat(state, format, error)) return false;

  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory image_memory = VK_NULL_HANDLE;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  void* mapping = nullptr;
  int import_fd = -1;
  const auto cleanup = [&] {
    if (mapping != nullptr) vkUnmapMemory(state.device, staging_memory);
    if (fence != VK_NULL_HANDLE) vkDestroyFence(state.device, fence, nullptr);
    if (command != VK_NULL_HANDLE) vkFreeCommandBuffers(state.device, state.command_pool, 1, &command);
    if (staging != VK_NULL_HANDLE) vkDestroyBuffer(state.device, staging, nullptr);
    if (staging_memory != VK_NULL_HANDLE) vkFreeMemory(state.device, staging_memory, nullptr);
    if (image != VK_NULL_HANDLE) vkDestroyImage(state.device, image, nullptr);
    if (image_memory != VK_NULL_HANDLE) vkFreeMemory(state.device, image_memory, nullptr);
    if (import_fd >= 0) close(import_fd);
  };
  const auto fail = [&](const char* message) {
    *error = message;
    cleanup();
    return false;
  };

  VkSubresourceLayout plane{};
  plane.offset = snapshot.planes[0].offset;
  plane.rowPitch = snapshot.planes[0].stride;
  VkExternalMemoryImageCreateInfo external_image{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_modifier{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
  explicit_modifier.pNext = &external_image;
  explicit_modifier.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
  explicit_modifier.drmFormatModifierPlaneCount = 1;
  explicit_modifier.pPlaneLayouts = &plane;
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.pNext = &explicit_modifier;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {snapshot.width, snapshot.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(state.device, &image_info, nullptr, &image) != VK_SUCCESS) return fail("vkCreateImage failed");

  VkMemoryDedicatedRequirements dedicated_requirements{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
  VkMemoryRequirements2 image_requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
  image_requirements.pNext = &dedicated_requirements;
  VkImageMemoryRequirementsInfo2 requirements_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
  requirements_info.image = image;
  vkGetImageMemoryRequirements2(state.device, &requirements_info, &image_requirements);
  import_fd = dup(received_fd);
  if (import_fd < 0) return fail("failed to duplicate received DMA-BUF fd");
  VkMemoryFdPropertiesKHR fd_properties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  if (state.get_memory_fd_properties(state.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                     import_fd, &fd_properties) != VK_SUCCESS) {
    return fail("vkGetMemoryFdPropertiesKHR failed");
  }
  uint32_t image_memory_type = 0;
  if (!FindMemoryType(state, image_requirements.memoryRequirements.memoryTypeBits & fd_properties.memoryTypeBits,
                      0, &image_memory_type)) {
    return fail("DMA-BUF and image memory type bits do not intersect");
  }
  VkMemoryDedicatedAllocateInfo dedicated_info{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated_info.image = image;
  VkImportMemoryFdInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  import_info.fd = import_fd;
  import_info.pNext = &dedicated_info;
  VkMemoryAllocateInfo image_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  image_allocation.pNext = &import_info;
  image_allocation.allocationSize = image_requirements.memoryRequirements.size;
  image_allocation.memoryTypeIndex = image_memory_type;
  if (vkAllocateMemory(state.device, &image_allocation, nullptr, &image_memory) != VK_SUCCESS) {
    return fail("vkAllocateMemory failed for imported DMA-BUF");
  }
  // A successful import transfers only the duplicated descriptor to Vulkan.
  import_fd = -1;
  if (vkBindImageMemory(state.device, image, image_memory, 0) != VK_SUCCESS) return fail("vkBindImageMemory failed");

  const VkDeviceSize byte_size = static_cast<VkDeviceSize>(snapshot.width) * snapshot.height * 4U;
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = byte_size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(state.device, &buffer_info, nullptr, &staging) != VK_SUCCESS) return fail("vkCreateBuffer failed");
  VkMemoryRequirements staging_requirements{};
  vkGetBufferMemoryRequirements(state.device, staging, &staging_requirements);
  uint32_t staging_type = 0;
  VkMemoryPropertyFlags staging_flags = 0;
  if (!FindMemoryType(state, staging_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                      &staging_type, &staging_flags)) {
    return fail("no host-visible staging memory type found");
  }
  VkMemoryAllocateInfo staging_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  staging_allocation.allocationSize = staging_requirements.size;
  staging_allocation.memoryTypeIndex = staging_type;
  if (vkAllocateMemory(state.device, &staging_allocation, nullptr, &staging_memory) != VK_SUCCESS ||
      vkBindBufferMemory(state.device, staging, staging_memory, 0) != VK_SUCCESS ||
      vkMapMemory(state.device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    return fail("failed to allocate and map staging memory");
  }

  VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_info.commandPool = state.command_pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(state.device, &command_info, &command) != VK_SUCCESS) {
    return fail("vkAllocateCommandBuffers failed");
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return fail("vkBeginCommandBuffer failed");
  VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  acquire.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  acquire.dstQueueFamilyIndex = state.queue_family;
  acquire.image = image;
  acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &acquire);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {snapshot.width, snapshot.height, 1};
  vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
  VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  release.srcQueueFamilyIndex = state.queue_family;
  release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  release.image = image;
  release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &release);
  VkBufferMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host_barrier.buffer = staging;
  host_barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                       0, nullptr, 1, &host_barrier, 0, nullptr);
  if (vkEndCommandBuffer(command) != VK_SUCCESS) return fail("vkEndCommandBuffer failed");

  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(state.device, &fence_info, nullptr, &fence) != VK_SUCCESS) return fail("vkCreateFence failed");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  if (vkQueueSubmit(state.queue, 1, &submit, fence) != VK_SUCCESS ||
      vkWaitForFences(state.device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
    return fail("Vulkan copy submission failed");
  }
  if ((staging_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = staging_memory;
    range.size = VK_WHOLE_SIZE;
    if (vkInvalidateMappedMemoryRanges(state.device, 1, &range) != VK_SUCCESS) {
      return fail("vkInvalidateMappedMemoryRanges failed");
    }
  }
  *checksum = Checksum(static_cast<const uint8_t*>(mapping), static_cast<size_t>(byte_size));
  cleanup();
  return true;
}

bool Connect(ScopedFd* socket_fd, std::string* error) {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || runtime[0] == '\0') {
    *error = "XDG_RUNTIME_DIR is not set";
    return false;
  }
  const std::string path = std::string(runtime) + "/electron-vr/" + kSocketName;
  sockaddr_un address{};
  if (path.size() >= sizeof(address.sun_path)) {
    *error = "companion socket path is too long";
    return false;
  }
  socket_fd->value = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (socket_fd->value < 0) {
    *error = "socket failed: " + std::string(std::strerror(errno));
    return false;
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (connect(socket_fd->value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    *error = "connect failed: " + std::string(std::strerror(errno));
    return false;
  }
  ucred credentials{};
  socklen_t credentials_size = sizeof(credentials);
  if (getsockopt(socket_fd->value, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0 ||
      credentials.uid != geteuid()) {
    *error = "companion socket peer UID does not match";
    return false;
  }
  LayerHello hello{};
  hello.header = {kProtocolMagic, kProtocolVersion, MessageType::kHello, sizeof(hello), 0, 1};
  hello.process_id = static_cast<uint32_t>(getpid());
  std::strncpy(hello.application_name, "electron_vr_vulkan_dmabuf_probe", sizeof(hello.application_name) - 1);
  if (send(socket_fd->value, &hello, sizeof(hello), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(hello))) {
    *error = "failed to send LayerHello";
    return false;
  }
  return true;
}

bool ReceiveSnapshot(int socket_fd, OverlaySnapshot* snapshot, ScopedFd* received_fd, std::string* error) {
  iovec data{snapshot, sizeof(*snapshot)};
  char control[CMSG_SPACE(sizeof(int) * kMaxPlanes)] = {};
  msghdr message{};
  message.msg_iov = &data;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  const ssize_t received = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC);
  std::vector<int> descriptors;
  if (received >= 0) {
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
          header->cmsg_len < CMSG_LEN(0)) continue;
      const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      const int* values = reinterpret_cast<const int*>(CMSG_DATA(header));
      descriptors.insert(descriptors.end(), values, values + count);
    }
  }
  if (received != static_cast<ssize_t>(sizeof(*snapshot)) ||
      (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
    for (int descriptor : descriptors) close(descriptor);
    *error = received < 0 ? "recvmsg failed: " + std::string(std::strerror(errno)) : "truncated snapshot packet";
    return false;
  }
  if (snapshot->header.magic != kProtocolMagic || snapshot->header.version != kProtocolVersion ||
      snapshot->header.type != MessageType::kSnapshot || snapshot->header.byte_size != sizeof(*snapshot) ||
      snapshot->header.fd_count != descriptors.size() || descriptors.size() > 1) {
    for (int descriptor : descriptors) close(descriptor);
    *error = "invalid OverlaySnapshot packet";
    return false;
  }
  if (!descriptors.empty()) received_fd->value = descriptors[0];
  return true;
}

bool SendFrameAck(int socket_fd, const OverlaySnapshot& snapshot, std::string* error) {
  FrameAck ack{};
  ack.header = {kProtocolMagic, kProtocolVersion, MessageType::kFrameAck, sizeof(ack), 0, snapshot.header.sequence};
  ack.generation = snapshot.generation;
  ack.consumed = 1;
  if (send(socket_fd, &ack, sizeof(ack), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(ack))) {
    *error = "failed to acknowledge imported frame";
    return false;
  }
  return true;
}

bool ParseFrameTarget(int argc, char** argv, uint32_t* target) {
  if (argc == 1) {
    *target = kDefaultFrames;
    return true;
  }
  if (argc != 2) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long value = std::strtoul(argv[1], &end, 10);
  if (errno != 0 || end == argv[1] || *end != '\0' || value == 0 ||
      value > std::numeric_limits<uint32_t>::max()) return false;
  *target = static_cast<uint32_t>(value);
  return true;
}
}

int main(int argc, char** argv) {
  uint32_t target = 0;
  if (!ParseFrameTarget(argc, argv, &target)) {
    std::cerr << "usage: " << argv[0] << " [successful-frames]\n";
    return 2;
  }
  VulkanState vulkan;
  std::string error;
  if (!InitializeVulkan(&vulkan, &error)) {
    std::cerr << "Vulkan initialization failed: " << error << '\n';
    return 1;
  }
  std::cout << "device=" << vulkan.properties.deviceName
            << " vendor=0x" << std::hex << vulkan.properties.vendorID
            << " device=0x" << vulkan.properties.deviceID << std::dec
            << " type=" << vulkan.properties.deviceType
            << " queue_family=" << vulkan.queue_family;
  if (vulkan.drm_properties.hasRender) {
    std::cout << " render_node=" << vulkan.drm_properties.renderMajor << ':'
              << vulkan.drm_properties.renderMinor;
  } else {
    std::cout << " render_node=unavailable";
  }
  std::cout << '\n';

  ScopedFd socket_fd;
  if (!Connect(&socket_fd, &error)) {
    std::cerr << "IPC connection failed: " << error << '\n';
    return 1;
  }
  uint32_t frames = 0;
  uint32_t changed = 0;
  uint64_t previous_checksum = 0;
  bool have_previous = false;
  while (frames < target) {
    OverlaySnapshot snapshot{};
    ScopedFd frame_fd;
    if (!ReceiveSnapshot(socket_fd.value, &snapshot, &frame_fd, &error)) {
      std::cerr << "snapshot receive failed: " << error << '\n';
      return 1;
    }
    if (snapshot.header.fd_count == 0) continue;
    uint64_t checksum = 0;
    if (snapshot.generation == 0 || frame_fd.value < 0 ||
        !ImportAndChecksum(vulkan, snapshot, frame_fd.value, &checksum, &error)) {
      std::cerr << "generation=" << snapshot.generation << " import failed: " << error << '\n';
      return 1;
    }
    if (!SendFrameAck(socket_fd.value, snapshot, &error)) {
      std::cerr << error << '\n';
      return 1;
    }
    if (have_previous && checksum != previous_checksum) ++changed;
    previous_checksum = checksum;
    have_previous = true;
    ++frames;
    std::cout << "frame=" << frames << " generation=" << snapshot.generation
              << " checksum=0x" << std::hex << checksum << std::dec << '\n';
  }
  std::cout << "frames=" << frames << " changed=" << changed << '\n';
  if (frames < target || changed < kMinimumChanges) {
    std::cerr << "probe failed: expected " << target << " frames and at least "
              << kMinimumChanges << " checksum changes\n";
    return 1;
  }
  return 0;
}
