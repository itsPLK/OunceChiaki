// Shares the hardware decoder's output images with Vulkan, so the renderer
// reads decoded pixels in place (no copy).
// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): Switch2ImageInterop.hpp.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
extern "C"
{
#include <switch2/services/gsv.h>
}
inline thread_local bool switch2PreimportDecoderImages = false;
struct Switch2ImportedImage
{
    VkBuffer buffer   = VK_NULL_HANDLE;
    uint64_t size     = 0;
    VkBufferView luma = VK_NULL_HANDLE, chroma = VK_NULL_HANDLE;
};
bool switch2PreimportImage(void* backing, uint64_t size);
void switch2WakeVideoFrame();
Switch2ImportedImage switch2FindImportedImage(void* backing);
void switch2UnimportImage(void* backing);
void switch2RecordGpuRange(uint64_t address, uint64_t size, uint32_t handle);
bool switch2CreateSharedNativeMemory(GsvNativeMemory* out, void* address, uint64_t size, Result* result);
