// Vulkan compute smoke (M6 / finding 25, Track B — as a PAYLOAD, not a native title).
//
// The question this answers: our console app already runs Mesa/zink on "RADV NAVI21" (see the
// console log), which means a RADV Vulkan driver (libvulkan.so.1) is already present next to the
// app. If a *payload* can bring up a Vulkan instance + compute device + mappable memory through
// that same driver, then on-console whisper can be ggml-vulkan linked into our payload — skipping
// both the multi-hour static-RADV build and the unsolved native-title loader (CE-107750-0).
//
// So this dlopens the shipped RADV driver by absolute path (the ELF loader on :9021 doesn't set
// LD_LIBRARY_PATH), loads the entry point, and walks the compute bring-up that ggml needs:
// instance -> physical device(s) -> a compute queue family -> logical device + queue -> a buffer
// backed by host-visible device memory, mapped, written and read back. It prints each step and
// exits; sent to the ELF loader, stdout streams back to the sender.
//
// Build/run:  PS5_HOST=192.168.0.56 scripts/ps5-build.sh vulkan-smoke         (build the .elf)
//             then send it to :9021 and capture stdout.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static void* load_driver() {
  // The RADV driver our app ships (scripts/ps5-run.sh uploads it); also try the SDK/system spots.
  const char* paths[] = {
    "/data/homebrew/PS5AgenticUI/libvulkan.so.1",
    "/data/homebrew/PS5AgenticUI/libvulkan.so",
    "libvulkan.so.1",
    "libvulkan.so",
  };
  for (const char* p : paths) {
    void* h = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
    printf("[vk] dlopen %s -> %s\n", p, h ? "ok" : dlerror());
    fflush(stdout);
    if (h) return h;
  }
  return nullptr;
}

int main() {
  printf("[vk] vulkan-smoke: bringing up a compute device as a payload\n");
  // The app runs from its folder with LD_LIBRARY_PATH set to it (see homebrew.js); RADV may need
  // that to find/stage its bits. Replicate it: chdir there and set the search path before dlopen.
  const char* appdir = "/data/homebrew/PS5AgenticUI";
  int cd = chdir(appdir);
  setenv("LD_LIBRARY_PATH", appdir, 1);
  printf("[vk] chdir(%s)=%d, LD_LIBRARY_PATH=%s\n", appdir, cd, getenv("LD_LIBRARY_PATH"));
  fflush(stdout);

  void* drv = load_driver();
  if (!drv) { printf("[vk] FAIL: no Vulkan driver could be loaded\n"); return 1; }

  // libvulkan.so.1 here is the RADV *ICD*, not the Khronos loader (there is no separate loader in
  // the SDK). An ICD expects the loader to negotiate the loader–ICD interface version first; skip
  // that and some physical-device dispatch (e.g. memory properties) comes back empty. Do the
  // handshake ourselves and use the ICD entry points the loader would use.
  using NegotiateFn = VkResult (*)(uint32_t*);
  auto negotiate = reinterpret_cast<NegotiateFn>(dlsym(drv, "vk_icdNegotiateLoaderICDInterfaceVersion"));
  auto icd_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(drv, "vk_icdGetInstanceProcAddr"));
  auto icd_gpdpa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(drv, "vk_icdGetPhysicalDeviceProcAddr"));
  if (negotiate) {
    uint32_t ver = 5;  // current loader–ICD interface version
    VkResult nr = negotiate(&ver);
    printf("[vk] vk_icdNegotiateLoaderICDInterfaceVersion -> %d, agreed version %u\n", int(nr), ver);
  } else {
    printf("[vk] WARN: no vk_icdNegotiateLoaderICDInterfaceVersion\n");
  }

  auto gipa = icd_gipa ? icd_gipa : reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(drv, "vkGetInstanceProcAddr"));
  if (!gipa) { printf("[vk] FAIL: no vkGetInstanceProcAddr\n"); return 1; }

  auto load = [&](VkInstance inst, const char* name) -> PFN_vkVoidFunction {
    PFN_vkVoidFunction f = gipa(inst, name);
    if (!f) printf("[vk] WARN: missing %s\n", name);
    return f;
  };

  auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(load(nullptr, "vkCreateInstance"));
  if (!vkCreateInstance) { printf("[vk] FAIL: no vkCreateInstance\n"); return 1; }

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "vulkan-smoke";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, nullptr, &instance);
  printf("[vk] vkCreateInstance -> %d\n", int(r));
  fflush(stdout);
  if (r != VK_SUCCESS) { printf("[vk] FAIL: instance\n"); return 1; }

  auto vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(load(instance, "vkEnumeratePhysicalDevices"));
  auto vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(load(instance, "vkGetPhysicalDeviceProperties"));
  auto vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(load(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
  auto vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(load(instance, "vkGetPhysicalDeviceMemoryProperties"));
  auto vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(load(instance, "vkCreateDevice"));
  auto vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(load(instance, "vkGetDeviceProcAddr"));
  if (!vkEnumeratePhysicalDevices || !vkCreateDevice || !vkGetDeviceProcAddr) { printf("[vk] FAIL: instance fns\n"); return 1; }

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance, &n, nullptr);
  printf("[vk] physical devices: %u\n", n);
  if (n == 0) { printf("[vk] FAIL: no physical devices (RADV didn't enumerate the GPU)\n"); return 1; }
  VkPhysicalDevice devs[8];
  if (n > 8) n = 8;
  vkEnumeratePhysicalDevices(instance, &n, devs);

  VkPhysicalDevice phys = devs[0];
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(phys, &props);
  printf("[vk] device 0: '%s' type=%d apiVersion=%u.%u.%u driverVersion=0x%x vendor=0x%x device=0x%x\n",
         props.deviceName, int(props.deviceType),
         VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
         props.driverVersion, props.vendorID, props.deviceID);
  fflush(stdout);

  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
  VkQueueFamilyProperties qf[16];
  if (qn > 16) qn = 16;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf);
  int computeFamily = -1;
  for (uint32_t i = 0; i < qn; ++i) {
    bool compute = qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT;
    bool graphics = qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
    printf("[vk]   queue family %u: count=%u flags=0x%x%s%s\n", i, qf[i].queueCount, qf[i].queueFlags,
           compute ? " COMPUTE" : "", graphics ? " GRAPHICS" : "");
    if (compute && computeFamily < 0) computeFamily = int(i);
  }
  if (computeFamily < 0) { printf("[vk] FAIL: no compute queue family\n"); return 1; }
  printf("[vk] using compute queue family %d\n", computeFamily);
  fflush(stdout);

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = uint32_t(computeFamily);
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;

  VkDevice device = VK_NULL_HANDLE;
  r = vkCreateDevice(phys, &dci, nullptr, &device);
  printf("[vk] vkCreateDevice -> %d\n", int(r));
  fflush(stdout);
  if (r != VK_SUCCESS) { printf("[vk] FAIL: logical device\n"); return 1; }

  auto dpa = [&](const char* name) { return vkGetDeviceProcAddr(device, name); };
  auto vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(dpa("vkGetDeviceQueue"));
  auto vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(dpa("vkCreateBuffer"));
  auto vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(dpa("vkGetBufferMemoryRequirements"));
  auto vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(dpa("vkAllocateMemory"));
  auto vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(dpa("vkBindBufferMemory"));
  auto vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(dpa("vkMapMemory"));
  if (!vkGetDeviceQueue || !vkCreateBuffer || !vkAllocateMemory || !vkMapMemory) { printf("[vk] FAIL: device fns\n"); return 1; }

  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, uint32_t(computeFamily), 0, &queue);
  printf("[vk] got compute queue: %s\n", queue ? "ok" : "null");

  // A small storage buffer backed by host-visible memory: the path ggml uses to stage data.
  const VkDeviceSize bytes = 4096;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  r = vkCreateBuffer(device, &bci, nullptr, &buffer);
  printf("[vk] vkCreateBuffer -> %d\n", int(r));
  if (r != VK_SUCCESS) { printf("[vk] FAIL: buffer\n"); return 1; }

  VkMemoryRequirements mr{};
  vkGetBufferMemoryRequirements(device, buffer, &mr);

  // Query memory via both the 1.0 and the 1.1 (…2) entry points: some RADV builds populate only
  // one. Use whichever reports memory types.
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  printf("[vk] vkGetPhysicalDeviceMemoryProperties (gipa): %u types, %u heaps\n", mp.memoryTypeCount, mp.memoryHeapCount);
  if (mp.memoryTypeCount == 0 && icd_gpdpa) {
    // Physical-device functions come from the ICD via vk_icdGetPhysicalDeviceProcAddr.
    auto pdmp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(icd_gpdpa(instance, "vkGetPhysicalDeviceMemoryProperties"));
    printf("[vk] GetPhysicalDeviceMemoryProperties via icdGetPhysicalDeviceProcAddr: %s\n", pdmp ? "found" : "missing");
    if (pdmp) {
      VkPhysicalDeviceMemoryProperties rmp{};
      pdmp(phys, &rmp);
      printf("[vk] …returned %u types, %u heaps\n", rmp.memoryTypeCount, rmp.memoryHeapCount);
      if (rmp.memoryTypeCount) mp = rmp;
    }
  }
  if (mp.memoryTypeCount == 0) {
    auto p2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties2"));
    if (!p2) p2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties2KHR"));
    if (p2) {
      VkPhysicalDeviceMemoryProperties2 mp2{};
      mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
      p2(phys, &mp2);
      mp = mp2.memoryProperties;
      printf("[vk] vkGetPhysicalDeviceMemoryProperties2: %u types, %u heaps\n", mp.memoryTypeCount, mp.memoryHeapCount);
    } else {
      printf("[vk] WARN: no …MemoryProperties2 either\n");
    }
  }
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    printf("[vk]   mem type %u: heap=%u flags=0x%x\n", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags);

  // Prefer host-visible+coherent (ggml's staging path); accept host-visible alone otherwise.
  int memType = -1;
  const uint32_t pref = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & pref) == pref) { memType = int(i); break; }
  if (memType < 0)
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { memType = int(i); break; }
  printf("[vk] chosen host-visible memory type: %d (of %u), buffer needs %llu bytes\n",
         memType, mp.memoryTypeCount, (unsigned long long)mr.size);
  if (memType < 0) { printf("[vk] FAIL: no host-visible memory\n"); return 1; }

  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = uint32_t(memType);
  VkDeviceMemory mem = VK_NULL_HANDLE;
  r = vkAllocateMemory(device, &mai, nullptr, &mem);
  printf("[vk] vkAllocateMemory -> %d\n", int(r));
  if (r != VK_SUCCESS) { printf("[vk] FAIL: allocate\n"); return 1; }
  vkBindBufferMemory(device, buffer, mem, 0);

  void* mapped = nullptr;
  r = vkMapMemory(device, mem, 0, bytes, 0, &mapped);
  printf("[vk] vkMapMemory -> %d ptr=%p\n", int(r), mapped);
  if (r != VK_SUCCESS || !mapped) { printf("[vk] FAIL: map\n"); return 1; }

  // Write a pattern and read it back: proves mapped device memory is actually coherent.
  uint32_t* u = static_cast<uint32_t*>(mapped);
  for (int i = 0; i < 1024; ++i) u[i] = uint32_t(i * 3 + 1);
  bool good = true;
  for (int i = 0; i < 1024; ++i) if (u[i] != uint32_t(i * 3 + 1)) { good = false; break; }
  printf("[vk] mapped memory write/read-back: %s\n", good ? "ok" : "MISMATCH");

  printf("[vk] RESULT: %s — Vulkan compute device is usable from a payload on this console\n",
         good ? "PASS" : "FAIL");
  fflush(stdout);
  return good ? 0 : 1;
}
