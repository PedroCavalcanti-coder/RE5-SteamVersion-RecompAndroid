"""Patch do present da DXVK 1.9.2b no Android: medida e quadros em voo (re5, 2026-11-11).

O que o Perfetto mostrou no A54 (Mali-G68, DXVK): a principal do jogo dorme ~38 ms por quadro
dentro do Present, esperando o present do quadro anterior (SynchronizePresent ->
waitForSubmission); a dxvk-submit, que faz o vkQueuePresentKHR e já pede a imagem seguinte,
fica 95% do tempo esperando o driver do Mali. CPU e GPU se revezam: a GPU fica ~60% ocupada, o
governador a deixa em 221 MHz e o jogo faz ~15 fps com nenhum recurso cheio.

1) Medida (com RE5_FPS_LOG=1): ms por segundo em vkQueuePresentKHR, no acquire antecipado da
   dxvk-submit, no acquire da thread do jogo, no vkQueueSubmit e nas duas esperas da thread do
   jogo no Present (present anterior; latência de quadros). Uma linha "re5 present:" a cada 5 s.
2) DXVK_RE5_PRESENT_DEPTH=<n> (1 a 3; padrão 1 = o código original): a thread do jogo só espera
   o present de n quadros atrás. Cada quadro leva o próprio semáforo e o índice da imagem até o
   vkQueuePresentKHR (o Presenter original guarda os dois num estado só, que andava no present:
   com quadros em voo o acquire seguinte reusaria o semáforo e a imagem do anterior), e o acquire
   passa a ser feito só na thread do jogo, sem o acquire antecipado.

apply(patch) recebe a função patch(rel, old, new) do prepare.py. Também roda sozinho sobre uma
árvore já preparada: python android/dxvk/re5_present.py build/dxvk-android/src
"""
import pathlib
import sys

STATS_DECL = '''
  /* re5: medida do present (RE5_FPS_LOG=1); ver android/dxvk/re5_present.py */
  enum Re5PresentStat { Re5QueuePresent, Re5PreAcquire, Re5AppAcquire, Re5QueueSubmit,
                        Re5WaitPresent, Re5WaitLatency, Re5StatCount };
  bool     re5StatsOn();
  bool     re5PresentQueueWanted();   /* DXVK_RE5_PRESENT_QUEUE=1 */
  uint64_t re5NowNs();
  void     re5StatAdd(Re5PresentStat k, uint64_t t0);
  void     re5StatReport();
'''

STATS_IMPL = '''
  /* re5: medida do present (RE5_FPS_LOG=1); ver android/dxvk/re5_present.py */
  static std::atomic<uint64_t> g_re5Ns[Re5StatCount];
  static std::atomic<uint32_t> g_re5N[Re5StatCount];
  static std::atomic<uint64_t> g_re5T0 = { 0 };

  bool re5StatsOn() {
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("RE5_FPS_LOG"); on = e && e[0] && e[0] != '0'; }
    return on != 0;
  }

  bool re5PresentQueueWanted() {
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("DXVK_RE5_PRESENT_QUEUE"); on = e && e[0] == '1'; }
    return on != 0;
  }

  uint64_t re5NowNs() {
    if (!re5StatsOn()) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
  }

  void re5StatAdd(Re5PresentStat k, uint64_t t0) {
    if (!t0) return;
    g_re5Ns[k] += re5NowNs() - t0;
    g_re5N[k] += 1;
  }

  void re5StatReport() {
    uint64_t now = re5NowNs();
    if (!now) return;
    uint64_t t0 = g_re5T0.load();
    if (!t0) { g_re5T0 = now; return; }
    if (now - t0 < 5000000000ull) return;
    if (!g_re5T0.compare_exchange_strong(t0, now)) return;
    double s = double(now - t0) / 1e9;
    static const char* names[Re5StatCount] = { "queue_present", "acquire_antecipado", "acquire_jogo",
                                               "queue_submit", "espera_present", "espera_latencia" };
    std::string line = "re5 present (ms por s, chamadas por s):";
    for (int k = 0; k < Re5StatCount; k++) {
      uint64_t ns = g_re5Ns[k].exchange(0);
      uint32_t n = g_re5N[k].exchange(0);
      line += str::format(" ", names[k], " ", uint32_t(double(ns) / 1e6 / s + 0.5), " (", uint32_t(n / s + 0.5), ")");
    }
    Logger::info(line);
  }
'''


def apply(patch):
    # ---- Presenter: medida, acquire/present por quadro ----
    patch("src/vulkan/vulkan_presenter.h", "    VkResult presentImage();\n",
          "    VkResult presentImage();\n\n"
          "    /* re5: quadros em voo (DXVK_RE5_PRESENT_DEPTH > 1): cada quadro leva o próprio semáforo\n"
          "     * e a imagem até o present; o acquire é feito só aqui, sem o antecipado da dxvk-submit. */\n"
          "    VkResult acquireNextImageRe5(PresenterSync& sync, uint32_t& index);\n"
          "    VkResult presentImageRe5(VkSemaphore present, uint32_t index);\n")
    patch("src/vulkan/vulkan_presenter.h", "    uint32_t m_frameIndex = 0;\n",
          "    uint32_t m_frameIndex = 0;\n    uint32_t m_re5AcquireIndex = 0;\n")
    patch("src/vulkan/vulkan_presenter.h", "namespace dxvk::vk {\n", "namespace dxvk::vk {\n" + STATS_DECL, )
    patch("src/vulkan/vulkan_presenter.cpp", '#include "vulkan_presenter.h"\n',
          '#include "vulkan_presenter.h"\n\n#include <atomic>\n#include <cstdlib>\n#include <ctime>\n')
    patch("src/vulkan/vulkan_presenter.cpp", "namespace dxvk::vk {\n", "namespace dxvk::vk {\n" + STATS_IMPL)
    patch("src/vulkan/vulkan_presenter.cpp",
          "    if (m_acquireStatus == VK_NOT_READY) {\n"
          "      m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),\n"
          "        m_swapchain, std::numeric_limits<uint64_t>::max(),\n"
          "        sync.acquire, VK_NULL_HANDLE, &m_imageIndex);\n"
          "    }",
          "    if (m_acquireStatus == VK_NOT_READY) {\n"
          "      uint64_t re5t0 = re5NowNs();\n"
          "      m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),\n"
          "        m_swapchain, std::numeric_limits<uint64_t>::max(),\n"
          "        sync.acquire, VK_NULL_HANDLE, &m_imageIndex);\n"
          "      re5StatAdd(Re5AppAcquire, re5t0);\n"
          "    }")
    patch("src/vulkan/vulkan_presenter.cpp",
          "    VkResult status = m_vkd->vkQueuePresentKHR(m_device.queue, &info);\n\n"
          "    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)\n      return status;\n",
          "    uint64_t re5t0 = re5NowNs();\n"
          "    VkResult status = m_vkd->vkQueuePresentKHR(m_device.queue, &info);\n"
          "    re5StatAdd(Re5QueuePresent, re5t0);\n\n"
          "    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)\n      return status;\n")
    patch("src/vulkan/vulkan_presenter.cpp",
          "    m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),\n"
          "      m_swapchain, std::numeric_limits<uint64_t>::max(),\n"
          "      sync.acquire, VK_NULL_HANDLE, &m_imageIndex);\n\n"
          "    bool vsync",
          "    re5t0 = re5NowNs();\n"
          "    m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),\n"
          "      m_swapchain, std::numeric_limits<uint64_t>::max(),\n"
          "      sync.acquire, VK_NULL_HANDLE, &m_imageIndex);\n"
          "    re5StatAdd(Re5PreAcquire, re5t0);\n\n"
          "    bool vsync")
    patch("src/vulkan/vulkan_presenter.cpp", "  VkResult Presenter::recreateSwapChain(",
          "  VkResult Presenter::acquireNextImageRe5(PresenterSync& sync, uint32_t& index) {\n"
          "    sync = m_semaphores.at(m_re5AcquireIndex);\n"
          "    m_re5AcquireIndex = (m_re5AcquireIndex + 1) % uint32_t(m_semaphores.size());\n"
          "    uint64_t re5t0 = re5NowNs();\n"
          "    VkResult status = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),\n"
          "      m_swapchain, std::numeric_limits<uint64_t>::max(),\n"
          "      sync.acquire, VK_NULL_HANDLE, &index);\n"
          "    re5StatAdd(Re5AppAcquire, re5t0);\n"
          "    return status;\n"
          "  }\n\n\n"
          "  VkResult Presenter::presentImageRe5(VkSemaphore present, uint32_t index) {\n"
          "    VkPresentInfoKHR info;\n"
          "    info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;\n"
          "    info.pNext              = nullptr;\n"
          "    info.waitSemaphoreCount = 1;\n"
          "    info.pWaitSemaphores    = &present;\n"
          "    info.swapchainCount     = 1;\n"
          "    info.pSwapchains        = &m_swapchain;\n"
          "    info.pImageIndices      = &index;\n"
          "    info.pResults           = nullptr;\n"
          "    uint64_t re5t0 = re5NowNs();\n"
          "    VkResult status = m_vkd->vkQueuePresentKHR(m_device.queue, &info);\n"
          "    re5StatAdd(Re5QueuePresent, re5t0);\n"
          "    bool vsync = m_info.presentMode == VK_PRESENT_MODE_FIFO_KHR\n"
          "              || m_info.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;\n"
          "    m_fpsLimiter.delay(vsync);\n"
          "    return status;\n"
          "  }\n\n\n"
          "  VkResult Presenter::recreateSwapChain(")
    patch("src/vulkan/vulkan_presenter.cpp", "    m_frameIndex = 0;\n",
          "    m_frameIndex = 0;\n    m_re5AcquireIndex = 0;\n")

    # ---- quantas filas cada família tem (um present numa fila própria precisa de 2 na gráfica) ----
    patch("src/dxvk/dxvk_adapter.cpp",
          "  DxvkAdapterQueueIndices DxvkAdapter::findQueueFamilies() const {\n",
          "  DxvkAdapterQueueIndices DxvkAdapter::findQueueFamilies() const {\n"
          "    for (uint32_t i = 0; i < m_queueFamilies.size(); i++)\n"
          "      Logger::info(str::format(\"re5: família \", i, \": \", m_queueFamilies[i].queueCount,\n"
          "        \" filas, flags \", uint32_t(m_queueFamilies[i].queueFlags)));\n")

    # ---- DXVK_RE5_PRESENT_QUEUE=1: present numa 2ª fila, numa thread própria ----
    # O driver do Mali do A54 segura o vkQueuePresentKHR ~um quadro inteiro; na DXVK 1.9 o present
    # roda na dxvk-submit, e os envios do quadro seguinte ficam presos atrás dele (GPU ~58% ocupada,
    # governador em 221 MHz). Com a família gráfica tendo 2 filas, o device nasce com as duas, o
    # Presenter usa a 2ª, e a dxvk-submit entrega o present a uma thread "dxvk-present" e segue
    # enviando. A thread do jogo continua esperando o present anterior antes do acquire (como no
    # original): o acquire antecipado e o present moram na thread de present, sem acesso
    # simultâneo à swapchain.
    patch("src/dxvk/dxvk_adapter.h", "  class DxvkAdapter : public RcObject {\n    \n  public:\n",
          "  class DxvkAdapter : public RcObject {\n    \n  public:\n"
          "    bool re5PresentQueue = false;   /* re5: o device foi criado com a 2ª fila gráfica */\n")
    patch("src/dxvk/dxvk_adapter.cpp", '#include "dxvk_adapter.h"\n',
          '#include "dxvk_adapter.h"\n#include "../vulkan/vulkan_presenter.h"\n')
    patch("src/dxvk/dxvk_adapter.cpp", "    float queuePriority = 1.0f;\n",
          "    float queuePriority = 1.0f;\n    float re5QueuePriorities[2] = { 1.0f, 1.0f };\n")
    patch("src/dxvk/dxvk_adapter.cpp",
          "      graphicsQueue.queueCount        = 1;\n"
          "      graphicsQueue.pQueuePriorities  = &queuePriority;\n",
          "      graphicsQueue.queueCount        = 1;\n"
          "      graphicsQueue.pQueuePriorities  = &queuePriority;\n"
          "      if (family == queueFamilies.graphics && vk::re5PresentQueueWanted()\n"
          "       && m_queueFamilies.at(family).queueCount >= 2) {\n"
          "        graphicsQueue.queueCount       = 2;\n"
          "        graphicsQueue.pQueuePriorities = re5QueuePriorities;\n"
          "        re5PresentQueue = true;\n"
          "        Logger::info(\"re5: present na 2ª fila gráfica (DXVK_RE5_PRESENT_QUEUE=1)\");\n"
          "      }\n")
    patch("src/dxvk/dxvk_device.h", "    DxvkDeviceQueue transfer;\n  };",
          "    DxvkDeviceQueue transfer;\n    DxvkDeviceQueue re5Present;   /* re5: 2ª fila gráfica, só para o present */\n  };")
    patch("src/dxvk/dxvk_device.cpp", "    m_queues.transfer = getQueue(queueFamilies.transfer, 0);\n",
          "    m_queues.transfer = getQueue(queueFamilies.transfer, 0);\n"
          "    if (m_adapter->re5PresentQueue)\n"
          "      m_queues.re5Present = getQueue(queueFamilies.graphics, 1);\n")
    patch("src/d3d9/d3d9_swapchain.cpp",
          "    DxvkDeviceQueue graphicsQueue = m_device->queues().graphics;\n",
          "    DxvkDeviceQueue graphicsQueue = m_device->queues().re5Present.queueHandle\n"
          "      ? m_device->queues().re5Present : m_device->queues().graphics;\n")
    patch("src/dxvk/dxvk_queue.h", "    dxvk::thread                m_finishThread;\n",
          "    dxvk::thread                m_finishThread;\n\n"
          "    /* re5: presents da 2ª fila (DXVK_RE5_PRESENT_QUEUE=1) */\n"
          "    dxvk::mutex                 m_re5PresentMutex;\n"
          "    dxvk::condition_variable    m_re5PresentCond;\n"
          "    std::queue<DxvkSubmitEntry> m_re5PresentQueue;\n"
          "    dxvk::thread                m_re5PresentThread;\n"
          "    void re5PresentLoop();\n")
    patch("src/dxvk/dxvk_queue.cpp",
          "    m_finishThread([this] () { finishCmdLists(); }) {\n",
          "    m_finishThread([this] () { finishCmdLists(); }),\n"
          "    m_re5PresentThread([this] () { re5PresentLoop(); }) {\n")
    patch("src/dxvk/dxvk_queue.cpp",
          "    m_submitThread.join();\n    m_finishThread.join();\n",
          "    { std::lock_guard<dxvk::mutex> plock(m_re5PresentMutex); }\n"
          "    m_re5PresentCond.notify_all();\n"
          "    m_submitThread.join();\n    m_finishThread.join();\n    m_re5PresentThread.join();\n")
    patch("src/dxvk/dxvk_queue.cpp",
          "      DxvkSubmitEntry entry = std::move(m_submitQueue.front());\n      lock.unlock();\n",
          "      DxvkSubmitEntry entry = std::move(m_submitQueue.front());\n      lock.unlock();\n\n"
          "      // re5: present na 2ª fila -> thread de present; esta segue com os envios\n"
          "      if (entry.present.presenter != nullptr && m_device->queues().re5Present.queueHandle) {\n"
          "        { std::lock_guard<dxvk::mutex> plock(m_re5PresentMutex);\n"
          "          m_re5PresentQueue.push(std::move(entry)); }\n"
          "        m_re5PresentCond.notify_one();\n"
          "        lock = std::unique_lock<dxvk::mutex>(m_mutex);\n"
          "        m_submitQueue.pop();\n"
          "        m_submitCond.notify_all();\n"
          "        continue;\n"
          "      }\n")
    patch("src/dxvk/dxvk_queue.cpp", "  void DxvkSubmissionQueue::finishCmdLists() {\n",
          "  void DxvkSubmissionQueue::re5PresentLoop() {\n"
          "    env::setThreadName(\"dxvk-present\");\n"
          "    std::unique_lock<dxvk::mutex> lock(m_re5PresentMutex);\n"
          "    while (!m_stopped.load()) {\n"
          "      m_re5PresentCond.wait(lock, [this] {\n"
          "        return m_stopped.load() || !m_re5PresentQueue.empty();\n"
          "      });\n"
          "      if (m_stopped.load())\n"
          "        return;\n"
          "      DxvkSubmitEntry entry = std::move(m_re5PresentQueue.front());\n"
          "      m_re5PresentQueue.pop();\n"
          "      lock.unlock();\n"
          "      VkResult status = entry.present.re5Semaphore\n"
          "        ? entry.present.presenter->presentImageRe5(entry.present.re5Semaphore, entry.present.re5Image)\n"
          "        : entry.present.presenter->presentImage();\n"
          "      if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)\n"
          "        Logger::warn(str::format(\"re5: present na 2ª fila -> \", status));\n"
          "      // quem espera o status (waitForSubmission) dorme em m_submitCond, sob m_mutex\n"
          "      { std::lock_guard<dxvk::mutex> slock(m_mutex);\n"
          "        if (entry.status)\n"
          "          entry.status->result = status; }\n"
          "      m_submitCond.notify_all();\n"
          "      lock.lock();\n"
          "    }\n"
          "  }\n\n\n"
          "  void DxvkSubmissionQueue::finishCmdLists() {\n")

    # ---- fila de envio: present com semáforo/imagem do quadro; medida do vkQueueSubmit ----
    patch("src/dxvk/dxvk_queue.h", "    Rc<vk::Presenter>   presenter;\n  };",
          "    Rc<vk::Presenter>   presenter;\n"
          "    VkSemaphore         re5Semaphore = VK_NULL_HANDLE;   /* re5: != 0 -> presentImageRe5 */\n"
          "    uint32_t            re5Image     = 0;\n  };")
    patch("src/dxvk/dxvk_queue.cpp",
          "          status = entry.submit.cmdList->submit(\n"
          "            entry.submit.waitSync,\n"
          "            entry.submit.wakeSync);",
          "          uint64_t re5t0 = vk::re5NowNs();\n"
          "          status = entry.submit.cmdList->submit(\n"
          "            entry.submit.waitSync,\n"
          "            entry.submit.wakeSync);\n"
          "          vk::re5StatAdd(vk::Re5QueueSubmit, re5t0);")
    patch("src/dxvk/dxvk_queue.cpp",
          "          status = entry.present.presenter->presentImage();",
          "          status = entry.present.re5Semaphore\n"
          "            ? entry.present.presenter->presentImageRe5(entry.present.re5Semaphore, entry.present.re5Image)\n"
          "            : entry.present.presenter->presentImage();")
    patch("src/dxvk/dxvk_device.h",
          "    void presentImage(\n"
          "      const Rc<vk::Presenter>&        presenter,\n"
          "            DxvkSubmitStatus*         status);",
          "    void presentImage(\n"
          "      const Rc<vk::Presenter>&        presenter,\n"
          "            DxvkSubmitStatus*         status,\n"
          "            VkSemaphore               re5Semaphore = VK_NULL_HANDLE,\n"
          "            uint32_t                  re5Image = 0);")
    patch("src/dxvk/dxvk_device.cpp",
          "  void DxvkDevice::presentImage(\n"
          "    const Rc<vk::Presenter>&        presenter,\n"
          "          DxvkSubmitStatus*         status) {\n"
          "    status->result = VK_NOT_READY;\n\n"
          "    DxvkPresentInfo presentInfo;\n"
          "    presentInfo.presenter = presenter;\n",
          "  void DxvkDevice::presentImage(\n"
          "    const Rc<vk::Presenter>&        presenter,\n"
          "          DxvkSubmitStatus*         status,\n"
          "          VkSemaphore               re5Semaphore,\n"
          "          uint32_t                  re5Image) {\n"
          "    status->result = VK_NOT_READY;\n\n"
          "    DxvkPresentInfo presentInfo;\n"
          "    presentInfo.presenter = presenter;\n"
          "    presentInfo.re5Semaphore = re5Semaphore;\n"
          "    presentInfo.re5Image = re5Image;\n")

    # ---- swapchain do D3D9: quadros em voo e as esperas medidas ----
    patch("src/d3d9/d3d9_swapchain.h", "    DxvkSubmitStatus          m_presentStatus;\n",
          "    DxvkSubmitStatus          m_presentStatus;\n"
          "    DxvkSubmitStatus          m_re5Status[4];   /* re5: um por present em voo (DEPTH > 1) */\n"
          "    uint32_t                  m_re5Next  = 0;\n"
          "    uint32_t                  m_re5Image = 0;\n"
          "    void Re5WaitAllPresents();\n"
          "    VkResult Re5Acquire(vk::PresenterSync& sync, uint32_t& index);\n")
    patch("src/d3d9/d3d9_swapchain.cpp", '#include "d3d9_swapchain.h"\n',
          '#include "d3d9_swapchain.h"\n\n#include <cstdlib>\n')
    patch("src/d3d9/d3d9_swapchain.cpp", "namespace dxvk {\n",
          "namespace dxvk {\n\n"
          "  /* re5: presents em voo antes de a thread do jogo esperar (android/dxvk/re5_present.py). */\n"
          "  static uint32_t re5PresentDepth() {\n"
          "    static int d = -1;\n"
          "    if (d < 0) {\n"
          "      const char* e = std::getenv(\"DXVK_RE5_PRESENT_DEPTH\");\n"
          "      d = e && e[0] ? std::atoi(e) : 1;\n"
          "      if (d < 1) d = 1;\n"
          "      if (d > 3) d = 3;\n"
          "      if (vk::re5PresentQueueWanted()) d = 1;   /* a 2ª fila usa o caminho original */\n"
          "      Logger::info(str::format(\"re5: DXVK_RE5_PRESENT_DEPTH=\", d, d > 1 ? \" (quadros em voo)\" : \" (original)\"));\n"
          "    }\n"
          "    return uint32_t(d);\n"
          "  }\n\n\n"
          "  void D3D9SwapChainEx::Re5WaitAllPresents() {\n"
          "    m_device->waitForSubmission(&this->m_presentStatus);\n"
          "    for (auto& s : m_re5Status) {\n"
          "      m_device->waitForSubmission(&s);\n"
          "      s.result = VK_SUCCESS;\n"
          "    }\n"
          "  }\n\n\n"
          "  VkResult D3D9SwapChainEx::Re5Acquire(vk::PresenterSync& sync, uint32_t& index) {\n"
          "    VkResult r = re5PresentDepth() > 1 ? m_presenter->acquireNextImageRe5(sync, index)\n"
          "                                       : m_presenter->acquireNextImage(sync, index);\n"
          "    m_re5Image = index;\n"
          "    return r;\n"
          "  }\n")
    for _ in range(4):   # destrutor, Invalidate, RecreateSwapChain, CreatePresenter
        patch("src/d3d9/d3d9_swapchain.cpp", "    m_device->waitForSubmission(&m_presentStatus);\n",
              "    Re5WaitAllPresents();\n")
    patch("src/d3d9/d3d9_swapchain.cpp",
          "      VkResult status = m_presenter->acquireNextImage(sync, imageIndex);",
          "      VkResult status = Re5Acquire(sync, imageIndex);")
    patch("src/d3d9/d3d9_swapchain.cpp",
          "        status = m_presenter->acquireNextImage(sync, imageIndex);",
          "        status = Re5Acquire(sync, imageIndex);")
    patch("src/d3d9/d3d9_swapchain.cpp",
          "    m_presentStatus.result = VK_NOT_READY;\n\n    m_parent->EmitCs([this,\n",
          "    DxvkSubmitStatus* re5Status = &m_presentStatus;\n"
          "    bool re5Depth = re5PresentDepth() > 1;\n"
          "    if (re5Depth) re5Status = &m_re5Status[m_re5Next++ % 4];\n"
          "    re5Status->result = VK_NOT_READY;\n\n"
          "    m_parent->EmitCs([this,\n"
          "      cStatus      = re5Status,\n"
          "      cRe5         = re5Depth,\n"
          "      cImage       = m_re5Image,\n")
    patch("src/d3d9/d3d9_swapchain.cpp",
          "      m_device->presentImage(m_presenter, &m_presentStatus);",
          "      m_device->presentImage(m_presenter, cStatus, cRe5 ? cSync.present : VK_NULL_HANDLE, cImage);")
    patch("src/d3d9/d3d9_swapchain.cpp",
          "    VkResult status = m_device->waitForSubmission(&m_presentStatus);\n",
          "    uint64_t re5t0 = vk::re5NowNs();\n"
          "    uint32_t re5Depth = re5PresentDepth();\n"
          "    VkResult status = VK_SUCCESS;\n"
          "    if (re5Depth <= 1)\n"
          "      status = m_device->waitForSubmission(&m_presentStatus);\n"
          "    else if (m_re5Next >= re5Depth)\n"
          "      status = m_device->waitForSubmission(&m_re5Status[(m_re5Next - re5Depth) % 4]);\n"
          "    vk::re5StatAdd(vk::Re5WaitPresent, re5t0);\n")
    patch("src/d3d9/d3d9_swapchain.cpp",
          "    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());\n",
          "    uint64_t re5t0 = vk::re5NowNs();\n"
          "    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());\n"
          "    vk::re5StatAdd(vk::Re5WaitLatency, re5t0);\n"
          "    vk::re5StatReport();\n")


if __name__ == "__main__":
    root = pathlib.Path(sys.argv[1])

    marker = root / "src/d3d9/d3d9_swapchain.cpp"
    if "re5PresentDepth" in marker.read_text(encoding="utf-8"):
        raise SystemExit(f"re5_present: já aplicado em {root}")

    def patch(rel, old, new):
        path = root / rel
        text = path.read_text(encoding="utf-8")
        if old not in text:
            raise SystemExit(f"re5_present: trecho não encontrado em {rel}: {old[:70]!r}")
        path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="")

    apply(patch)
    print(f"re5_present: aplicado em {root}")
