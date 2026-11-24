/*
 * VulkanMemory.h - toda a memória de GPU dos renderizadores nativos, pelo VMA.
 *
 * Regra: nenhum vkAllocateMemory / vkBindBufferMemory / vkBindImageMemory fora do VMA. Buffers e
 * imagens nascem em vmaCreateBuffer / vmaCreateImage e morrem em vmaDestroyBuffer /
 * vmaDestroyImage.
 *
 * Vazamentos e uso-após-liberar: Buffer e Image são donos únicos (só movem). Destruir um recurso
 * que a GPU ainda pode ler não o libera na hora: retire() o põe na fila do serial do envio
 * corrente, e collect(serialConcluído) libera o que a GPU já terminou. Ao fim, o destrutor de
 * VulkanMemory confere que tudo voltou e aborta listando o que sobrou com o nome de depuração
 * (D8: vazamento vira erro com nome, não silêncio).
 *
 * Uso típico no RE5:
 *   - malhas estáticas (modelos, SBC1 do detector de colisão estático quando desenhado em
 *     depuração): uploadStatic() -> DEVICE_LOCAL, cópia por staging no próximo envio;
 *   - vertex/index buffers D3DUSAGE_DYNAMIC: createBuffer(..., MemoryUsage::Stream) -> HOST_VISIBLE,
 *     mapeado persistente (em Adreno/Mali a memória é unificada: costuma ser também DEVICE_LOCAL);
 *   - texturas: createImage() + uploadImage();
 *   - profundidade/MSAA que nunca saem do tile: MemoryUsage::Transient -> LAZILY_ALLOCATED
 *     (sem RAM de verdade quando o passe usa STORE_OP_DONT_CARE).
 */
#ifndef RE5_VKN_VULKAN_MEMORY_H
#define RE5_VKN_VULKAN_MEMORY_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

/* Sem libvulkan ligado: o VMA busca tudo pelos vkGet*ProcAddr do contexto. */
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif
#include "vk_mem_alloc.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace re5::vkn {

class VulkanContext;
class VulkanMemory;
struct DeviceFns;

/* Intenção de uso -> VmaMemoryUsage + flags (a tradução fica no .cpp, num só lugar). */
enum class MemoryUsage : uint8_t {
    GpuOnly,   /* DEVICE_LOCAL; escrito só por cópia (malhas estáticas, texturas) */
    Stream,    /* HOST_VISIBLE, mapeado persistente, escrita sequencial da CPU (dinâmicos, UBO) */
    Readback,  /* HOST_VISIBLE | HOST_CACHED (GetRenderTargetData, consultas) */
    Staging,   /* origem de cópia; vida curta */
    Transient  /* LAZILY_ALLOCATED quando existe; senão GpuOnly (anexos que não saem do tile) */
};

/* Buffer dono único. Movível, não copiável; o destrutor chama VulkanMemory::retire(). */
class Buffer {
public:
    Buffer() = default;
    ~Buffer();
    Buffer(Buffer &&o) noexcept;
    Buffer &operator=(Buffer &&o) noexcept;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    VkBuffer handle() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    void *mapped() const { return mapped_; } /* nullptr se não é host-visible */
    bool hostCoherent() const { return coherent_; }
    explicit operator bool() const { return buffer_ != VK_NULL_HANDLE; }

    /* Para memória não coerente: vmaFlushAllocation / vmaInvalidateAllocation. */
    void flush(VkDeviceSize offset, VkDeviceSize size) const;
    void invalidate(VkDeviceSize offset, VkDeviceSize size) const;

private:
    friend class VulkanMemory;
    VulkanMemory *owner_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation alloc_ = nullptr;
    VkDeviceSize size_ = 0;
    void *mapped_ = nullptr;
    bool coherent_ = false;
};

/* Imagem dona única. A view padrão (todas as mips/camadas) é criada junto. */
class Image {
public:
    Image() = default;
    ~Image();
    Image(Image &&o) noexcept;
    Image &operator=(Image &&o) noexcept;
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;

    VkImage handle() const { return image_; }
    VkImageView view() const { return view_; }
    VkFormat format() const { return format_; }
    VkExtent3D extent() const { return extent_; }
    uint32_t mipLevels() const { return mips_; }
    uint32_t layers() const { return layers_; }
    VkSampleCountFlagBits samples() const { return samples_; }
    bool transient() const { return transient_; } /* caiu em LAZILY_ALLOCATED */
    VkImageUsageFlags usage() const { return usage_; }
    VkImageAspectFlags aspect() const { return aspect_; }
    uint64_t id() const { return id_; } /* único e monotônico (oráculo TBDR, caches) */
    explicit operator bool() const { return image_ != VK_NULL_HANDLE; }

    /*
     * Layout corrente (a imagem inteira num layout só). Quem grava a transição no command
     * buffer atualiza: VulkanMemory nos uploads, o renderizador nos passes.
     */
    VkImageLayout layout() const { return layout_; }
    void setLayout(VkImageLayout l) { layout_ = l; }

private:
    friend class VulkanMemory;
    VulkanMemory *owner_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    std::vector<std::pair<uint32_t, VkImageView>> subViews_; /* (layer << 16 | mip) -> view de anexo */
    uint64_t id_ = 0;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags usage_ = 0;
    VkImageAspectFlags aspect_ = 0;
    VmaAllocation alloc_ = nullptr;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent3D extent_{};
    uint32_t mips_ = 1, layers_ = 1;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    bool transient_ = false;
};

struct BufferDesc {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    MemoryUsage memory = MemoryUsage::GpuOnly;
    VkMemoryPropertyFlags requiredFlags = 0; /* ex.: HOST_COHERENT para quem escreve sem flush */
    const char *debugName = nullptr; /* vmaSetAllocationName + VK_EXT_debug_utils quando houver */
};

struct ImageDesc {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{1, 1, 1};
    uint32_t mipLevels = 1;
    uint32_t layers = 1; /* 6 para cubo */
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0; /* Transient soma VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT */
    VkImageCreateFlags flags = 0;
    MemoryUsage memory = MemoryUsage::GpuOnly;
    const char *debugName = nullptr;
};

/* Uma região de upload de textura: mip/camada de destino e dados já no layout do Vulkan. */
struct ImageUploadRegion {
    uint32_t mipLevel = 0, layer = 0;
    VkOffset3D offset{};
    VkExtent3D extent{};
    const void *data = nullptr;
    size_t bytes = 0;
    uint32_t rowLengthTexels = 0; /* 0 = compacto (BC/ETC usam blocos) */
};

/*
 * API "crua" para o device D3D9 em C (vkn_bridge.h): handles soltos em vez de Buffer/Image,
 * porque a destruição adiada já é do device (lixo por envio). Continua tudo pelo VMA e contado
 * no relatório de vazamento.
 */
struct HostBlock; /* memória do host importada (VK_EXT_external_memory_host) e subdividida */
struct RawAlloc {
    VmaAllocation alloc = nullptr; /* nullptr numa subdivisão de HostBlock */
    void *map = nullptr;           /* host-visible mapeado, ou nullptr */
    HostBlock *block = nullptr;    /* != nullptr: subdivisão de bloco do host */
    VmaVirtualAllocation sub = 0;
};

/* Totais para o overlay de desempenho e para os testes de vazamento. */
struct MemoryStats {
    uint64_t liveBuffers = 0, liveImages = 0;
    uint64_t bytesDeviceLocal = 0, bytesHostVisible = 0, bytesLazy = 0;
    uint64_t pendingRetire = 0;  /* recursos esperando a GPU */
    uint64_t stagingBytesThisFrame = 0;
};

/*
 * Dono do VmaAllocator e da fila de destruição adiada. Thread-safe: o VMA tem seu próprio
 * lock; a fila de retire e o anel de staging usam mutex_ (a ponte D3D9 chama de várias threads
 * do guest).
 *
 * As cópias de upload são gravadas no command buffer que o renderizador passa em
 * recordUploads(), antes do primeiro passe do envio; não há vkQueueSubmit aqui.
 */
class VulkanMemory {
public:
    VulkanMemory(); /* fora da linha: PendingCopy só é completo no .cpp */
    ~VulkanMemory(); /* drena tudo (exige device ocioso), loga sobras, vmaDestroyAllocator */

    VulkanMemory(const VulkanMemory &) = delete;
    VulkanMemory &operator=(const VulkanMemory &) = delete;

    /* vmaCreateAllocator com as funções do contexto (mesmo driver/AdrenoTools). */
    bool create(const VulkanContext &ctx, VkDeviceSize stagingRingBytes = VkDeviceSize(32) << 20);
    void destroy();

    Buffer createBuffer(const BufferDesc &desc);
    Image createImage(const ImageDesc &desc);

    /*
     * Buffer sobre memória do guest (VK_EXT_external_memory_host), substitui o chunk -2 do re5vk.
     * O VMA só importa ponteiro de host com alocação dedicada recebendo pNext; se a versão do VMA
     * vendorizada não oferecer isso, a chamada aborta com o motivo (nada de cópia silenciosa).
     */
    Buffer importHostBuffer(void *hostPtr, VkDeviceSize size, VkBufferUsageFlags usage, const char *debugName);

    /* Malha estática: cria GpuOnly e enfileira a cópia de 'data' via staging. */
    Buffer uploadStatic(const void *data, VkDeviceSize size, VkBufferUsageFlags usage, const char *debugName);

    /* Atualiza um trecho de buffer GpuOnly (via staging) ou escreve direto se mapeado. */
    void updateBuffer(const Buffer &dst, VkDeviceSize offset, const void *data, VkDeviceSize size);

    /*
     * Enfileira cópias de textura. Em recordUploads() a imagem vai a TRANSFER_DST e depois a
     * SHADER_READ_ONLY_OPTIMAL (Image::layout() acompanha).
     */
    void uploadImage(Image &dst, const ImageUploadRegion *regions, uint32_t count);

    /* View de um único mip/camada (anexo de framebuffer); criada uma vez, morre com a imagem. */
    VkImageView attachmentView(Image &img, uint32_t mipLevel, uint32_t layer);

    /*
     * Avisado quando uma imagem é aposentada (fora do lock): o renderizador descarta
     * framebuffers que usavam as views dela. Chamado na thread de quem destruiu a imagem.
     */
    using ImageRetiredFn = void (*)(uint64_t imageId, void *user);
    void setImageRetiredCallback(ImageRetiredFn fn, void *user);

    /* Grava as cópias pendentes em 'cmd' (com as barreiras TRANSFER -> uso). Devolve quantas. */
    uint32_t recordUploads(VkCommandBuffer cmd, uint64_t submitSerial);

    /* Destruição adiada: chamadas pelos destrutores de Buffer/Image. */
    void retire(Buffer &b);
    void retire(Image &img);

    /* Serial do envio que está sendo gravado (retire usa) e do último que a GPU concluiu. */
    void setRecordingSerial(uint64_t serial);
    void collect(uint64_t completedSerial);

    MemoryStats stats() const;
    VmaAllocator allocator() const { return allocator_; }

    /* ---- API crua (vkn_bridge.h). Falha de alocação aborta com o nome (D8). ---- */
    VkBuffer createRawBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage, VkMemoryPropertyFlags required,
                             const char *name, RawAlloc *out);
    VkImage createRawImage(const VkImageCreateInfo &ci, MemoryUsage usage, const char *name, RawAlloc *out);
    /* Destruição imediata: quem chama já esperou a GPU (lixo do envio concluído). */
    void destroyRawBuffer(VkBuffer b, RawAlloc &a);
    void destroyRawImage(VkImage img, RawAlloc &a);
    /* Importa [p, p+size) do host como um bloco; nullptr se o driver recusar (quem chama volta ao
     * caminho comum). O bloco vive até destroy(); a memória do host é de quem chamou. */
    HostBlock *importHostBlock(void *p, VkDeviceSize size, const char *name);
    /* Buffer dentro do bloco; VK_NULL_HANDLE se não couber ou o tipo de memória não servir. */
    VkBuffer createRawHostBuffer(HostBlock *block, const VkBufferCreateInfo &ci, RawAlloc *out);
    /* Solta um bloco sem buffers vivos (aborta se houver). A memória do host é de quem chamou. */
    void freeHostBlock(HostBlock *block);

private:
    struct Retired {
        uint64_t serial;
        VkBuffer buffer;
        VkImage image;
        std::vector<VkImageView> views;
        VmaAllocation alloc;
    };
    struct PendingCopy; /* buffer->buffer ou buffer->imagem, definido no .cpp */
    struct LiveInfo {
        std::string name;
        VkDeviceSize size;
        uint8_t cls; /* 0 device-local, 1 host-visible, 2 lazy */
        bool buffer;
    };

    /* Anel de staging; com mutex_ preso. Sem espaço no anel: buffer temporário aposentado no envio. */
    VkBuffer stageAllocLocked(VkDeviceSize size, VkDeviceSize align, VkDeviceSize *outOffset, void **outPtr);
    void trackLocked(VmaAllocation a, const char *name, bool buffer);
    void untrackLocked(VmaAllocation a);
    void freeRetiredLocked(Retired &r);

    VkDevice device_ = VK_NULL_HANDLE;
    const DeviceFns *fns_ = nullptr;
    VmaAllocator allocator_ = nullptr;
    bool hasLazy_ = false;
    bool externalHost_ = false;
    VkDeviceSize hostPtrAlign_ = 0;
    VkDeviceSize nonCoherentAtom_ = 1;
    uint64_t nextImageId_ = 1;
    ImageRetiredFn imageRetired_ = nullptr;
    void *imageRetiredUser_ = nullptr;

    mutable std::mutex mutex_;
    uint64_t recordingSerial_ = 0;
    std::deque<Retired> retired_;
    std::vector<PendingCopy> pending_;
    std::unordered_map<VmaAllocation, LiveInfo> live_;
    std::vector<HostBlock *> hostBlocks_;
    uint32_t seenBufferTypes_ = 0, seenImageTypes_ = 0; /* tipos já anunciados no log */
    uint64_t liveHostSubs_ = 0;

    Buffer stagingRing_;
    VkDeviceSize stagingHead_ = 0, stagingTail_ = 0;
    std::deque<std::pair<uint64_t, VkDeviceSize>> stagingMarks_; /* serial -> head no fim do envio */

    MemoryStats stats_{};
};

} // namespace re5::vkn

#endif /* RE5_VKN_VULKAN_MEMORY_H */
