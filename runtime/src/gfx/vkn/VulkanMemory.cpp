/*
 * VulkanMemory.cpp - memória dos renderizadores nativos pelo VMA (ver VulkanMemory.h).
 *
 * Esta é a única unidade de tradução com VMA_IMPLEMENTATION.
 */
#define VMA_IMPLEMENTATION
#include "VulkanMemory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "VulkanContext.h"

extern "C" {
#include "re5/log.h"
}

namespace re5::vkn {

namespace {

constexpr const char *kCh = "vkn-mem";

/*
 * RE5_VKN_VMA_AUTO=1: deixa o VMA pontuar os tipos (VMA_MEMORY_USAGE_AUTO*). Padrão: o primeiro
 * tipo com as propriedades pedidas, como o alocador do re5vk fazia e como a especificação manda
 * ordenar (o mais rápido primeiro). No Mali todo tipo é DEVICE_LOCAL|HOST_VISIBLE e o AUTO pode
 * cair num tipo HOST_CACHED (coerência de cache em toda leitura/escrita da GPU).
 */
bool vmaAuto()
{
    static int v = -1;
    if (v < 0) {
        const char *e = std::getenv("RE5_VKN_VMA_AUTO");
        v = e && *e == '1';
    }
    return v == 1;
}

VmaAllocationCreateInfo allocInfoFor(MemoryUsage u, bool hasLazy)
{
    VmaAllocationCreateInfo ai{};
    switch (u) {
    case MemoryUsage::GpuOnly:
        if (vmaAuto()) {
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        } else {
            ai.usage = VMA_MEMORY_USAGE_UNKNOWN; /* custo igual: o VMA fica com o menor índice */
            ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
        break;
    case MemoryUsage::Stream:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Readback:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Staging:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Transient:
        if (hasLazy) {
            ai.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;
            ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        } else {
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        }
        break;
    }
    return ai;
}

VkImageAspectFlags aspectOf(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a)
{
    return a > 1 ? (v + a - 1) / a * a : v;
}

} // namespace

/* ------------------------------------------------------------------ cópias ---- */

struct HostBlock {
    VmaAllocation alloc = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t memoryType = 0;
    uint8_t *base = nullptr;
    VkDeviceSize size = 0;
    VmaVirtualBlock vblock = nullptr;
    uint32_t live = 0;
};

struct VulkanMemory::PendingCopy {
    bool toImage = false;
    VkBuffer src = VK_NULL_HANDLE;
    VkBuffer dstBuffer = VK_NULL_HANDLE;
    VkBufferCopy bufferRegion{};
    Image *dstImage = nullptr;
    VkImage image = VK_NULL_HANDLE;
    uint64_t imageId = 0;
    VkBufferImageCopy imageRegion{};
};

/* ---------------------------------------------------------------- Buffer ---- */

Buffer::~Buffer()
{
    if (owner_ && buffer_ != VK_NULL_HANDLE) owner_->retire(*this);
}

Buffer::Buffer(Buffer &&o) noexcept
    : owner_(o.owner_), buffer_(o.buffer_), alloc_(o.alloc_), size_(o.size_), mapped_(o.mapped_),
      coherent_(o.coherent_)
{
    o.owner_ = nullptr;
    o.buffer_ = VK_NULL_HANDLE;
    o.alloc_ = nullptr;
    o.size_ = 0;
    o.mapped_ = nullptr;
}

Buffer &Buffer::operator=(Buffer &&o) noexcept
{
    if (this != &o) {
        if (owner_ && buffer_ != VK_NULL_HANDLE) owner_->retire(*this);
        owner_ = o.owner_;
        buffer_ = o.buffer_;
        alloc_ = o.alloc_;
        size_ = o.size_;
        mapped_ = o.mapped_;
        coherent_ = o.coherent_;
        o.owner_ = nullptr;
        o.buffer_ = VK_NULL_HANDLE;
        o.alloc_ = nullptr;
        o.size_ = 0;
        o.mapped_ = nullptr;
    }
    return *this;
}

void Buffer::flush(VkDeviceSize offset, VkDeviceSize size) const
{
    if (!coherent_ && owner_ && alloc_) vmaFlushAllocation(owner_->allocator(), alloc_, offset, size);
}

void Buffer::invalidate(VkDeviceSize offset, VkDeviceSize size) const
{
    if (!coherent_ && owner_ && alloc_) vmaInvalidateAllocation(owner_->allocator(), alloc_, offset, size);
}

/* ----------------------------------------------------------------- Image ---- */

Image::~Image()
{
    if (owner_ && image_ != VK_NULL_HANDLE) owner_->retire(*this);
}

Image::Image(Image &&o) noexcept
{
    *this = std::move(o);
}

Image &Image::operator=(Image &&o) noexcept
{
    if (this != &o) {
        if (owner_ && image_ != VK_NULL_HANDLE) owner_->retire(*this);
        owner_ = o.owner_;
        image_ = o.image_;
        view_ = o.view_;
        subViews_ = std::move(o.subViews_);
        id_ = o.id_;
        layout_ = o.layout_;
        usage_ = o.usage_;
        aspect_ = o.aspect_;
        alloc_ = o.alloc_;
        format_ = o.format_;
        extent_ = o.extent_;
        mips_ = o.mips_;
        layers_ = o.layers_;
        samples_ = o.samples_;
        transient_ = o.transient_;
        o.owner_ = nullptr;
        o.image_ = VK_NULL_HANDLE;
        o.view_ = VK_NULL_HANDLE;
        o.subViews_.clear();
        o.alloc_ = nullptr;
        o.id_ = 0;
    }
    return *this;
}

/* ---------------------------------------------------------- VulkanMemory ---- */

VulkanMemory::VulkanMemory() = default;

VulkanMemory::~VulkanMemory()
{
    destroy();
}

bool VulkanMemory::create(const VulkanContext &ctx, VkDeviceSize stagingRingBytes)
{
    destroy();
    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = ctx.getInstanceProcAddr();
    vf.vkGetDeviceProcAddr = ctx.getDeviceProcAddr();

    VmaAllocatorCreateInfo ci{};
    ci.physicalDevice = ctx.physicalDevice();
    ci.device = ctx.device();
    ci.instance = ctx.instance();
    /* O VMA não pode usar versão acima da instância nem do device. */
    uint32_t api = std::min(ctx.caps().instanceApiVersion ? ctx.caps().instanceApiVersion : VK_API_VERSION_1_1,
                            ctx.properties().apiVersion);
    ci.vulkanApiVersion = std::max<uint32_t>(VK_API_VERSION_1_1, std::min<uint32_t>(api, VK_API_VERSION_1_3));
    ci.pVulkanFunctions = &vf;
    VkResult r = vmaCreateAllocator(&ci, &allocator_);
    if (r != VK_SUCCESS) {
        RE5_ERROR(kCh, "vmaCreateAllocator -> %d", (int)r);
        allocator_ = nullptr;
        return false;
    }
    device_ = ctx.device();
    fns_ = &ctx.fns();
    hasLazy_ = ctx.caps().lazilyAllocatedMemory;
    externalHost_ = ctx.caps().externalMemoryHost && ctx.fns().vkGetMemoryHostPointerPropertiesEXT;
    hostPtrAlign_ = ctx.caps().minImportedHostPointerAlignment;
    nonCoherentAtom_ = ctx.caps().nonCoherentAtomSize;
    stats_ = MemoryStats{};
    seenBufferTypes_ = seenImageTypes_ = 0;
    {
        const VkPhysicalDeviceMemoryProperties &mp = ctx.memoryProperties();
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            RE5_INFO(kCh, "tipo de memória %u: flags 0x%X, heap %u", i, (unsigned)mp.memoryTypes[i].propertyFlags,
                     mp.memoryTypes[i].heapIndex);
    }

    if (stagingRingBytes) {
        BufferDesc d;
        d.size = stagingRingBytes;
        d.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        d.memory = MemoryUsage::Staging;
        d.debugName = "re5vkn.staging_ring";
        stagingRing_ = createBuffer(d);
        if (!stagingRing_.mapped()) re5_fatal(kCh, "anel de staging sem mapeamento");
    }
    stagingHead_ = stagingTail_ = 0;
    return true;
}

void VulkanMemory::destroy()
{
    if (!allocator_) return;
    /* O chamador garante device ocioso: tudo aposentado pode sair agora. */
    if (stagingRing_) retire(stagingRing_);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_.clear();
        for (auto &r : retired_) freeRetiredLocked(r);
        retired_.clear();
        stagingMarks_.clear();
        if (liveHostSubs_)
            re5_fatal(kCh, "%llu buffer(s) em blocos do host ainda vivos ao destruir a memória",
                      (unsigned long long)liveHostSubs_);
        for (HostBlock *hb : hostBlocks_) {
            vmaDestroyVirtualBlock(hb->vblock);
            untrackLocked(hb->alloc);
            vmaFreeMemory(allocator_, hb->alloc);
            delete hb;
        }
        hostBlocks_.clear();
        if (!live_.empty()) {
            for (const auto &kv : live_)
                RE5_ERROR(kCh, "vazamento: %s '%s' (%llu bytes) ainda vivo ao destruir a memória",
                          kv.second.buffer ? "buffer" : "imagem", kv.second.name.c_str(),
                          (unsigned long long)kv.second.size);
            re5_fatal(kCh, "%llu recurso(s) de GPU vazaram (lista acima); destrua-os antes do renderizador",
                      (unsigned long long)live_.size());
        }
    }
    vmaDestroyAllocator(allocator_);
    allocator_ = nullptr;
    device_ = VK_NULL_HANDLE;
    fns_ = nullptr;
    stats_ = MemoryStats{};
}

void VulkanMemory::trackLocked(VmaAllocation a, const char *name, bool buffer)
{
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator_, a, &info);
    VkMemoryPropertyFlags flags = 0;
    vmaGetAllocationMemoryProperties(allocator_, a, &flags);
    LiveInfo li;
    li.name = name ? name : "(sem nome)";
    li.size = info.size;
    li.cls = (flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? 2 : (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? 1 : 0;
    li.buffer = buffer;
    if (name) vmaSetAllocationName(allocator_, a, name);
    /* Uma linha por tipo de memória novo em uso (buffer/imagem): mostra a escolha no aparelho. */
    {
        const uint32_t bit = 1u << (info.memoryType & 31u);
        uint32_t &seen = buffer ? seenBufferTypes_ : seenImageTypes_;
        if (!(seen & bit)) {
            seen |= bit;
            RE5_INFO(kCh, "%s '%s': tipo de memória %u (flags 0x%X)%s", buffer ? "buffer" : "imagem", li.name.c_str(),
                     info.memoryType, (unsigned)flags, vmaAuto() ? " [RE5_VKN_VMA_AUTO=1]" : "");
        }
    }
    uint64_t &bytes = li.cls == 2 ? stats_.bytesLazy : li.cls == 1 ? stats_.bytesHostVisible : stats_.bytesDeviceLocal;
    bytes += li.size;
    (buffer ? stats_.liveBuffers : stats_.liveImages)++;
    live_.emplace(a, std::move(li));
}

void VulkanMemory::untrackLocked(VmaAllocation a)
{
    auto it = live_.find(a);
    if (it == live_.end()) re5_fatal(kCh, "alocação desconhecida liberada (dupla liberação?)");
    const LiveInfo &li = it->second;
    uint64_t &bytes = li.cls == 2 ? stats_.bytesLazy : li.cls == 1 ? stats_.bytesHostVisible : stats_.bytesDeviceLocal;
    bytes -= li.size;
    (li.buffer ? stats_.liveBuffers : stats_.liveImages)--;
    live_.erase(it);
}

Buffer VulkanMemory::createBuffer(const BufferDesc &desc)
{
    if (!allocator_) re5_fatal(kCh, "createBuffer antes de create()");
    if (desc.memory == MemoryUsage::Transient) re5_fatal(kCh, "MemoryUsage::Transient é só para imagens (%s)",
                                                         desc.debugName ? desc.debugName : "?");
    if (desc.size == 0) re5_fatal(kCh, "buffer de tamanho 0 (%s)", desc.debugName ? desc.debugName : "?");

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = desc.size;
    bi.usage = desc.usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai = allocInfoFor(desc.memory, hasLazy_);
    ai.requiredFlags |= desc.requiredFlags;
    Buffer b;
    VmaAllocationInfo info{};
    VkResult r = vmaCreateBuffer(allocator_, &bi, &ai, &b.buffer_, &b.alloc_, &info);
    if (r != VK_SUCCESS)
        re5_fatal(kCh, "vmaCreateBuffer(%s, %llu bytes) -> %d", desc.debugName ? desc.debugName : "?",
                  (unsigned long long)desc.size, (int)r);
    VkMemoryPropertyFlags flags = 0;
    vmaGetAllocationMemoryProperties(allocator_, b.alloc_, &flags);
    b.owner_ = this;
    b.size_ = desc.size;
    b.mapped_ = info.pMappedData;
    b.coherent_ = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    std::lock_guard<std::mutex> lk(mutex_);
    trackLocked(b.alloc_, desc.debugName, true);
    return b;
}

Image VulkanMemory::createImage(const ImageDesc &desc)
{
    if (!allocator_) re5_fatal(kCh, "createImage antes de create()");
    const bool lazy = desc.memory == MemoryUsage::Transient && hasLazy_;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.flags = desc.flags;
    if (desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE || desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ii.imageType = desc.type;
    ii.format = desc.format;
    ii.extent = desc.extent;
    ii.mipLevels = desc.mipLevels;
    ii.arrayLayers = desc.layers;
    ii.samples = desc.samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = desc.usage;
    if (lazy) {
        /* Só anexos podem morar em LAZILY_ALLOCATED: nada de cópia nem amostragem. */
        const VkImageUsageFlags bad = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        if (ii.usage & bad)
            re5_fatal(kCh, "imagem transitória '%s' com uso 0x%X que lê/copia (só anexos podem ser lazy)",
                      desc.debugName ? desc.debugName : "?", (unsigned)ii.usage);
        ii.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    }
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai = allocInfoFor(desc.memory, hasLazy_);
    Image img;
    VkResult r = vmaCreateImage(allocator_, &ii, &ai, &img.image_, &img.alloc_, nullptr);
    if (r != VK_SUCCESS)
        re5_fatal(kCh, "vmaCreateImage(%s, %ux%ux%u fmt %d) -> %d", desc.debugName ? desc.debugName : "?",
                  desc.extent.width, desc.extent.height, desc.extent.depth, (int)desc.format, (int)r);

    img.owner_ = this;
    img.format_ = desc.format;
    img.extent_ = desc.extent;
    img.mips_ = desc.mipLevels;
    img.layers_ = desc.layers;
    img.samples_ = desc.samples;
    img.transient_ = lazy;
    img.usage_ = ii.usage;
    img.aspect_ = aspectOf(desc.format);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image_;
    vi.viewType = desc.viewType;
    vi.format = desc.format;
    /* View de amostragem de profundidade+estêncil: só o aspecto de profundidade. */
    vi.subresourceRange.aspectMask =
        (img.aspect_ & VK_IMAGE_ASPECT_DEPTH_BIT) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : img.aspect_;
    vi.subresourceRange.levelCount = desc.mipLevels;
    vi.subresourceRange.layerCount = desc.layers;
    r = fns_->vkCreateImageView(device_, &vi, nullptr, &img.view_);
    if (r != VK_SUCCESS)
        re5_fatal(kCh, "vkCreateImageView(%s) -> %d", desc.debugName ? desc.debugName : "?", (int)r);

    std::lock_guard<std::mutex> lk(mutex_);
    img.id_ = nextImageId_++;
    trackLocked(img.alloc_, desc.debugName, false);
    return img;
}

VkImageView VulkanMemory::attachmentView(Image &img, uint32_t mipLevel, uint32_t layer)
{
    if (!img) re5_fatal(kCh, "attachmentView em imagem vazia");
    if (mipLevel >= img.mips_ || layer >= img.layers_)
        re5_fatal(kCh, "attachmentView fora da imagem (mip %u/%u, camada %u/%u)", mipLevel, img.mips_, layer,
                  img.layers_);
    /* Imagem 2D sem mips nem camadas: a view padrão já serve de anexo. */
    if (img.mips_ == 1 && img.layers_ == 1 && img.extent_.depth == 1) return img.view_;
    const uint32_t key = layer << 16 | mipLevel;
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto &sv : img.subViews_)
        if (sv.first == key) return sv.second;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = img.format_;
    vi.subresourceRange.aspectMask = img.aspect_;
    vi.subresourceRange.baseMipLevel = mipLevel;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = layer;
    vi.subresourceRange.layerCount = 1;
    VkImageView v = VK_NULL_HANDLE;
    VkResult r = fns_->vkCreateImageView(device_, &vi, nullptr, &v);
    if (r != VK_SUCCESS) re5_fatal(kCh, "vkCreateImageView (anexo mip %u camada %u) -> %d", mipLevel, layer, (int)r);
    img.subViews_.emplace_back(key, v);
    return v;
}

Buffer VulkanMemory::importHostBuffer(void *hostPtr, VkDeviceSize size, VkBufferUsageFlags usage,
                                      const char *debugName)
{
    if (!externalHost_)
        re5_fatal(kCh, "importHostBuffer(%s): o device não tem VK_EXT_external_memory_host",
                  debugName ? debugName : "?");
    const VkDeviceSize al = hostPtrAlign_ ? hostPtrAlign_ : 1;
    if (reinterpret_cast<uintptr_t>(hostPtr) % al || size % al)
        re5_fatal(kCh, "importHostBuffer(%s): ponteiro %p / tamanho %llu fora do alinhamento %llu",
                  debugName ? debugName : "?", hostPtr, (unsigned long long)size, (unsigned long long)al);

    VkMemoryHostPointerPropertiesEXT hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    VkResult r = fns_->vkGetMemoryHostPointerPropertiesEXT(
        device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, hostPtr, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits)
        re5_fatal(kCh, "vkGetMemoryHostPointerPropertiesEXT(%s) -> %d, tipos 0x%X", debugName ? debugName : "?",
                  (int)r, hp.memoryTypeBits);

    VkExternalMemoryBufferCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.pNext = &ext;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImportMemoryHostPointerInfoEXT imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = hostPtr;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_UNKNOWN;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    ai.memoryTypeBits = hp.memoryTypeBits;

    Buffer b;
    r = vmaCreateDedicatedBuffer(allocator_, &bi, &ai, &imp, &b.buffer_, &b.alloc_, nullptr);
    if (r != VK_SUCCESS)
        re5_fatal(kCh, "vmaCreateDedicatedBuffer (importação %s, %llu bytes) -> %d", debugName ? debugName : "?",
                  (unsigned long long)size, (int)r);
    VkMemoryPropertyFlags flags = 0;
    vmaGetAllocationMemoryProperties(allocator_, b.alloc_, &flags);
    b.owner_ = this;
    b.size_ = size;
    b.mapped_ = hostPtr; /* a memória é o próprio ponteiro do guest */
    b.coherent_ = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    std::lock_guard<std::mutex> lk(mutex_);
    trackLocked(b.alloc_, debugName, true);
    return b;
}

VkBuffer VulkanMemory::stageAllocLocked(VkDeviceSize size, VkDeviceSize align, VkDeviceSize *outOffset,
                                        void **outPtr)
{
    const VkDeviceSize cap = stagingRing_ ? stagingRing_.size() : 0;
    align = std::max<VkDeviceSize>(align, nonCoherentAtom_);
    VkDeviceSize start = alignUp(stagingHead_, align);
    bool fits = false;
    if (cap && size < cap) {
        if (stagingHead_ >= stagingTail_) {
            if (start + size <= cap) {
                fits = true;
            } else if (size < stagingTail_) { /* volta ao começo; '<' mantém head != tail */
                start = 0;
                fits = true;
            }
        } else if (start + size < stagingTail_) {
            fits = true;
        }
    }
    if (fits) {
        stagingHead_ = start + size;
        stats_.stagingBytesThisFrame += size;
        *outOffset = start;
        *outPtr = static_cast<uint8_t *>(stagingRing_.mapped()) + start;
        return stagingRing_.handle();
    }

    /* Não cabe (textura grande ou anel cheio): buffer próprio, aposentado no envio corrente. */
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai = allocInfoFor(MemoryUsage::Staging, hasLazy_);
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    VkResult r = vmaCreateBuffer(allocator_, &bi, &ai, &buf, &a, &info);
    if (r != VK_SUCCESS) re5_fatal(kCh, "staging temporário de %llu bytes -> %d", (unsigned long long)size, (int)r);
    trackLocked(a, "re5vkn.staging_temp", true);
    /* recordingSerial_ ainda vai ser enviado: a liberação espera ele terminar. */
    retired_.push_back(Retired{recordingSerial_, buf, VK_NULL_HANDLE, {}, a});
    stats_.pendingRetire = retired_.size();
    stats_.stagingBytesThisFrame += size;
    *outOffset = 0;
    *outPtr = info.pMappedData;
    return buf;
}

void VulkanMemory::updateBuffer(const Buffer &dst, VkDeviceSize offset, const void *data, VkDeviceSize size)
{
    if (!dst || offset + size > dst.size())
        re5_fatal(kCh, "updateBuffer fora do buffer (%llu + %llu > %llu)", (unsigned long long)offset,
                  (unsigned long long)size, (unsigned long long)dst.size());
    if (size == 0) return;
    if (dst.mapped()) {
        std::memcpy(static_cast<uint8_t *>(dst.mapped()) + offset, data, size);
        dst.flush(offset, size);
        return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    VkDeviceSize off = 0;
    void *p = nullptr;
    VkBuffer src = stageAllocLocked(size, 4, &off, &p);
    std::memcpy(p, data, size);
    if (src == stagingRing_.handle()) stagingRing_.flush(off, size);
    else vmaFlushAllocation(allocator_, retired_.back().alloc, 0, VK_WHOLE_SIZE);
    PendingCopy c;
    c.src = src;
    c.dstBuffer = dst.handle();
    c.bufferRegion = VkBufferCopy{off, offset, size};
    pending_.push_back(c);
}

Buffer VulkanMemory::uploadStatic(const void *data, VkDeviceSize size, VkBufferUsageFlags usage,
                                  const char *debugName)
{
    BufferDesc d;
    d.size = size;
    d.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    d.memory = MemoryUsage::GpuOnly;
    d.debugName = debugName;
    Buffer b = createBuffer(d);
    updateBuffer(b, 0, data, size);
    return b;
}

void VulkanMemory::uploadImage(Image &dst, const ImageUploadRegion *regions, uint32_t count)
{
    if (!dst) re5_fatal(kCh, "uploadImage em imagem vazia");
    if (dst.transient_) re5_fatal(kCh, "uploadImage em imagem transitória (lazy)");
    if (!(dst.usage_ & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        re5_fatal(kCh, "uploadImage: imagem sem VK_IMAGE_USAGE_TRANSFER_DST_BIT");
    std::lock_guard<std::mutex> lk(mutex_);
    for (uint32_t i = 0; i < count; i++) {
        const ImageUploadRegion &rg = regions[i];
        if (rg.mipLevel >= dst.mips_ || rg.layer >= dst.layers_)
            re5_fatal(kCh, "uploadImage: região fora da imagem (mip %u camada %u)", rg.mipLevel, rg.layer);
        VkDeviceSize off = 0;
        void *p = nullptr;
        VkBuffer src = stageAllocLocked(rg.bytes, 16, &off, &p);
        std::memcpy(p, rg.data, rg.bytes);
        if (src == stagingRing_.handle()) stagingRing_.flush(off, rg.bytes);
        else vmaFlushAllocation(allocator_, retired_.back().alloc, 0, VK_WHOLE_SIZE);
        PendingCopy c;
        c.toImage = true;
        c.src = src;
        c.dstImage = &dst;
        c.image = dst.image_;
        c.imageId = dst.id_;
        c.imageRegion.bufferOffset = off;
        c.imageRegion.bufferRowLength = rg.rowLengthTexels;
        c.imageRegion.imageSubresource.aspectMask =
            (dst.aspect_ & VK_IMAGE_ASPECT_DEPTH_BIT) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : dst.aspect_;
        c.imageRegion.imageSubresource.mipLevel = rg.mipLevel;
        c.imageRegion.imageSubresource.baseArrayLayer = rg.layer;
        c.imageRegion.imageSubresource.layerCount = 1;
        c.imageRegion.imageOffset = rg.offset;
        c.imageRegion.imageExtent = rg.extent;
        pending_.push_back(c);
    }
}

uint32_t VulkanMemory::recordUploads(VkCommandBuffer cmd, uint64_t submitSerial)
{
    std::vector<PendingCopy> work;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        work.swap(pending_);
        /* Tudo o que foi alocado no anel até aqui é lido por este envio. */
        if (stagingRing_ && (stagingMarks_.empty() || stagingMarks_.back().second != stagingHead_))
            stagingMarks_.emplace_back(submitSerial, stagingHead_);
    }
    if (work.empty()) return 0;

    /* WAR: o envio anterior pode estar lendo o destino; só dependência de execução. */
    fns_->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);

    /* Imagens: cada uma vai a TRANSFER_DST uma vez (dedup por id). */
    std::vector<Image *> images;
    for (const auto &c : work)
        if (c.toImage && std::find(images.begin(), images.end(), c.dstImage) == images.end())
            images.push_back(c.dstImage);
    std::vector<VkImageMemoryBarrier> barriers;
    for (Image *img : images) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = img->layout_; /* UNDEFINED na primeira carga: o conteúdo anterior não importa */
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img->image_;
        b.subresourceRange = {img->aspect_, 0, img->mips_, 0, img->layers_};
        barriers.push_back(b);
    }
    if (!barriers.empty())
        fns_->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                   nullptr, 0, nullptr, (uint32_t)barriers.size(), barriers.data());

    for (const auto &c : work) {
        if (c.toImage)
            fns_->vkCmdCopyBufferToImage(cmd, c.src, c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c.imageRegion);
        else
            fns_->vkCmdCopyBuffer(cmd, c.src, c.dstBuffer, 1, &c.bufferRegion);
    }

    for (size_t i = 0; i < barriers.size(); i++) {
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        images[i]->layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                       VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    fns_->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               0, 1, &mb, 0, nullptr, (uint32_t)barriers.size(), barriers.data());
    return (uint32_t)work.size();
}

void VulkanMemory::freeRetiredLocked(Retired &r)
{
    for (VkImageView v : r.views) fns_->vkDestroyImageView(device_, v, nullptr);
    untrackLocked(r.alloc);
    if (r.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator_, r.buffer, r.alloc);
    else vmaDestroyImage(allocator_, r.image, r.alloc);
}

void VulkanMemory::retire(Buffer &b)
{
    if (b.buffer_ == VK_NULL_HANDLE) return;
    if (b.owner_ != this) re5_fatal(kCh, "buffer aposentado no VulkanMemory errado");
    {
        std::lock_guard<std::mutex> lk(mutex_);
        /* Cópias pendentes para ele morrem com ele. */
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&](const PendingCopy &c) { return !c.toImage && c.dstBuffer == b.buffer_; }),
                       pending_.end());
        retired_.push_back(Retired{recordingSerial_, b.buffer_, VK_NULL_HANDLE, {}, b.alloc_});
        stats_.pendingRetire = retired_.size();
    }
    b.owner_ = nullptr;
    b.buffer_ = VK_NULL_HANDLE;
    b.alloc_ = nullptr;
    b.mapped_ = nullptr;
    b.size_ = 0;
}

void VulkanMemory::retire(Image &img)
{
    if (img.image_ == VK_NULL_HANDLE) return;
    if (img.owner_ != this) re5_fatal(kCh, "imagem aposentada no VulkanMemory errado");
    const uint64_t id = img.id_;
    ImageRetiredFn cb;
    void *cbUser;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&](const PendingCopy &c) { return c.toImage && c.imageId == id; }),
                       pending_.end());
        Retired r{recordingSerial_, VK_NULL_HANDLE, img.image_, {}, img.alloc_};
        if (img.view_ != VK_NULL_HANDLE) r.views.push_back(img.view_);
        for (auto &sv : img.subViews_) r.views.push_back(sv.second);
        retired_.push_back(std::move(r));
        stats_.pendingRetire = retired_.size();
        cb = imageRetired_;
        cbUser = imageRetiredUser_;
    }
    img.owner_ = nullptr;
    img.image_ = VK_NULL_HANDLE;
    img.view_ = VK_NULL_HANDLE;
    img.subViews_.clear();
    img.alloc_ = nullptr;
    img.id_ = 0;
    if (cb) cb(id, cbUser);
}

void VulkanMemory::setImageRetiredCallback(ImageRetiredFn fn, void *user)
{
    std::lock_guard<std::mutex> lk(mutex_);
    imageRetired_ = fn;
    imageRetiredUser_ = user;
}

void VulkanMemory::setRecordingSerial(uint64_t serial)
{
    std::lock_guard<std::mutex> lk(mutex_);
    recordingSerial_ = serial;
    stats_.stagingBytesThisFrame = 0;
}

void VulkanMemory::collect(uint64_t completedSerial)
{
    std::lock_guard<std::mutex> lk(mutex_);
    /* A ordem de aposentadoria segue os seriais (não decrescentes): para no primeiro em uso. */
    while (!retired_.empty() && retired_.front().serial <= completedSerial) {
        freeRetiredLocked(retired_.front());
        retired_.pop_front();
    }
    while (!stagingMarks_.empty() && stagingMarks_.front().first <= completedSerial) {
        stagingTail_ = stagingMarks_.front().second;
        stagingMarks_.pop_front();
    }
    if (stagingMarks_.empty() && pending_.empty()) stagingHead_ = stagingTail_ = 0; /* anel vazio */
    stats_.pendingRetire = retired_.size();
}

/* ------------------------------------------------------------- API crua ---- */

VkBuffer VulkanMemory::createRawBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage, VkMemoryPropertyFlags required,
                                       const char *name, RawAlloc *out)
{
    if (!allocator_) re5_fatal(kCh, "createRawBuffer antes de create()");
    VmaAllocationCreateInfo ai = allocInfoFor(usage, hasLazy_);
    if ((required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !vmaAuto()) {
        /* o primeiro tipo com HOST_VISIBLE|HOST_COHERENT, como o re5vk */
        ai = VmaAllocationCreateInfo{};
        ai.usage = VMA_MEMORY_USAGE_UNKNOWN;
        ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    ai.requiredFlags |= required;
    if ((required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && vmaAuto())
        ai.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer b = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    VkResult r = vmaCreateBuffer(allocator_, &ci, &ai, &b, &out->alloc, &info);
    if (r != VK_SUCCESS)
        re5_fatal(kCh, "vmaCreateBuffer(%s, %llu bytes) -> %d", name ? name : "?", (unsigned long long)ci.size, (int)r);
    out->map = info.pMappedData;
    out->block = nullptr;
    std::lock_guard<std::mutex> lk(mutex_);
    trackLocked(out->alloc, name, true);
    return b;
}

VkImage VulkanMemory::createRawImage(const VkImageCreateInfo &ci, MemoryUsage usage, const char *name, RawAlloc *out)
{
    if (!allocator_) re5_fatal(kCh, "createRawImage antes de create()");
    VmaAllocationCreateInfo ai = allocInfoFor(usage, hasLazy_);
    VkImage img = VK_NULL_HANDLE;
    VkResult r = vmaCreateImage(allocator_, &ci, &ai, &img, &out->alloc, nullptr);
    if (r != VK_SUCCESS)
        re5_fatal(kCh, "vmaCreateImage(%s, %ux%ux%u fmt %d) -> %d", name ? name : "?", ci.extent.width,
                  ci.extent.height, ci.extent.depth, (int)ci.format, (int)r);
    out->map = nullptr;
    out->block = nullptr;
    std::lock_guard<std::mutex> lk(mutex_);
    trackLocked(out->alloc, name, false);
    return img;
}

void VulkanMemory::destroyRawBuffer(VkBuffer b, RawAlloc &a)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (a.block) {
        fns_->vkDestroyBuffer(device_, b, nullptr);
        vmaVirtualFree(a.block->vblock, a.sub);
        a.block->live--;
        liveHostSubs_--;
    } else if (a.alloc) {
        untrackLocked(a.alloc);
        vmaDestroyBuffer(allocator_, b, a.alloc);
    }
    a = RawAlloc{};
}

void VulkanMemory::destroyRawImage(VkImage img, RawAlloc &a)
{
    if (!a.alloc) return;
    std::lock_guard<std::mutex> lk(mutex_);
    untrackLocked(a.alloc);
    vmaDestroyImage(allocator_, img, a.alloc);
    a = RawAlloc{};
}

HostBlock *VulkanMemory::importHostBlock(void *p, VkDeviceSize size, const char *name)
{
    if (!externalHost_) return nullptr;
    VkMemoryHostPointerPropertiesEXT hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (fns_->vkGetMemoryHostPointerPropertiesEXT(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p,
                                                  &hp) != VK_SUCCESS || !hp.memoryTypeBits)
        return nullptr;
    VkImportMemoryHostPointerInfoEXT imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = p;
    VkMemoryRequirements req{};
    req.size = size;
    req.alignment = 1;
    req.memoryTypeBits = hp.memoryTypeBits;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_UNKNOWN;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocation alloc = nullptr;
    VmaAllocationInfo info{};
    if (vmaAllocateDedicatedMemory(allocator_, &req, &ai, &imp, &alloc, &info) != VK_SUCCESS) return nullptr;
    VmaVirtualBlockCreateInfo vci{};
    vci.size = size;
    VmaVirtualBlock vb = nullptr;
    if (vmaCreateVirtualBlock(&vci, &vb) != VK_SUCCESS) re5_fatal(kCh, "vmaCreateVirtualBlock falhou");
    auto *hb = new HostBlock;
    hb->alloc = alloc;
    hb->memory = info.deviceMemory;
    hb->memoryType = info.memoryType;
    hb->base = static_cast<uint8_t *>(p);
    hb->size = size;
    hb->vblock = vb;
    std::lock_guard<std::mutex> lk(mutex_);
    trackLocked(alloc, name ? name : "re5vkn.host_block", true);
    hostBlocks_.push_back(hb);
    return hb;
}

VkBuffer VulkanMemory::createRawHostBuffer(HostBlock *hb, const VkBufferCreateInfo &ci, RawAlloc *out)
{
    VkBuffer b = VK_NULL_HANDLE;
    if (fns_->vkCreateBuffer(device_, &ci, nullptr, &b) != VK_SUCCESS)
        re5_fatal(kCh, "vkCreateBuffer (bloco do host, %llu bytes)", (unsigned long long)ci.size);
    VkMemoryRequirements req{};
    fns_->vkGetBufferMemoryRequirements(device_, b, &req);
    std::lock_guard<std::mutex> lk(mutex_);
    VmaVirtualAllocationCreateInfo vai{};
    vai.size = req.size;
    vai.alignment = req.alignment ? req.alignment : 1;
    VkDeviceSize off = 0;
    VmaVirtualAllocation sub = 0;
    if (!(req.memoryTypeBits >> hb->memoryType & 1u) || vmaVirtualAllocate(hb->vblock, &vai, &sub, &off) != VK_SUCCESS) {
        fns_->vkDestroyBuffer(device_, b, nullptr);
        return VK_NULL_HANDLE;
    }
    VkResult r = vmaBindBufferMemory2(allocator_, hb->alloc, off, b, nullptr);
    if (r != VK_SUCCESS) re5_fatal(kCh, "vmaBindBufferMemory2 (bloco do host) -> %d", (int)r);
    out->alloc = nullptr;
    out->block = hb;
    out->sub = sub;
    out->map = hb->base + off;
    hb->live++;
    liveHostSubs_++;
    return b;
}

void VulkanMemory::freeHostBlock(HostBlock *hb)
{
    if (!hb) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (hb->live) re5_fatal(kCh, "bloco do host solto com %u buffer(s) vivos", hb->live);
    auto it = std::find(hostBlocks_.begin(), hostBlocks_.end(), hb);
    if (it == hostBlocks_.end()) re5_fatal(kCh, "bloco do host desconhecido");
    hostBlocks_.erase(it);
    vmaDestroyVirtualBlock(hb->vblock);
    untrackLocked(hb->alloc);
    vmaFreeMemory(allocator_, hb->alloc);
    delete hb;
}

MemoryStats VulkanMemory::stats() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return stats_;
}

} // namespace re5::vkn
