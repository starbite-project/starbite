/*
 * vktest: the first Vulkan on the S9's Mali-G72 under iOS. PanVK
 * (Mesa's Vulkan driver, /usr/local/starbite/lib/libvulkan_panfrost.dylib,
 * built by tools/mesa-ios/mk-mesa-ios.sh's Vulkan build) is called directly
 * through its ICD entry point, no loader: an instance, the GPU, a device, an
 * image cleared on the GPU to a known colour and copied into a buffer the CPU
 * reads back. Every step goes to /private/var/tmp/vktest.log.
 * Runs at boot as launchd job com.starbite.vktest (tools/vk/install-vktest.py).
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vulkan/vulkan_core.h>

static FILE *out;

static void
say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fflush(out);
}

#define CHECK(what, call) do { VkResult r_ = (call); say("%s: %d\n", what, r_); if (r_ != VK_SUCCESS) return 1; } while (0)

static PFN_vkGetInstanceProcAddr gipa;
#define LOAD(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)

int
main(void)
{
	out = fopen("/private/var/tmp/vktest.log", "w");
	if (out == NULL) {
		return 1;
	}
	setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
	setenv("PANVK_DEBUG", "startup", 1);
	setenv("MESA_LOG_FILE", "/private/var/tmp/vktest.mesa.log", 1);
	setenv("STARBITE_DRM_TRACE", "1", 1);
	say("vktest: pid %d, waiting 20 s for the GPU\n", getpid());
	sleep(20);

	void *lib = dlopen("/usr/local/starbite/lib/libvulkan_panfrost.dylib", RTLD_NOW);
	if (lib == NULL) {
		say("dlopen: %s\n", dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
	say("vk_icdGetInstanceProcAddr %p\n", (void *)gipa);
	if (gipa == NULL) {
		return 1;
	}
	LOAD(NULL, vkCreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0,
		                  .pApplicationName = "vktest" };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CHECK("vkCreateInstance", vkCreateInstance(&ici, NULL, &inst));

	LOAD(inst, vkEnumeratePhysicalDevices);
	LOAD(inst, vkGetPhysicalDeviceProperties);
	LOAD(inst, vkGetPhysicalDeviceMemoryProperties);
	LOAD(inst, vkGetPhysicalDeviceQueueFamilyProperties);
	LOAD(inst, vkCreateDevice);
	LOAD(inst, vkGetDeviceProcAddr);
	uint32_t n = 1;
	VkPhysicalDevice pd;
	CHECK("vkEnumeratePhysicalDevices", vkEnumeratePhysicalDevices(inst, &n, &pd));
	if (n == 0) {
		say("no physical device\n");
		return 1;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	say("device: %s, Vulkan %u.%u.%u, driver %#x\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
	    VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
	say("queue families: %u\n", nq);

	float prio = 1;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
		                        .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		                   .pQueueCreateInfos = &qci };
	VkDevice dev;
	CHECK("vkCreateDevice", vkCreateDevice(pd, &dci, NULL, &dev));
#define DLOAD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
	DLOAD(vkGetDeviceQueue);
	DLOAD(vkCreateImage);
	DLOAD(vkGetImageMemoryRequirements);
	DLOAD(vkAllocateMemory);
	DLOAD(vkBindImageMemory);
	DLOAD(vkCreateBuffer);
	DLOAD(vkGetBufferMemoryRequirements);
	DLOAD(vkBindBufferMemory);
	DLOAD(vkMapMemory);
	DLOAD(vkCreateCommandPool);
	DLOAD(vkAllocateCommandBuffers);
	DLOAD(vkBeginCommandBuffer);
	DLOAD(vkCmdPipelineBarrier);
	DLOAD(vkCmdClearColorImage);
	DLOAD(vkCmdCopyImageToBuffer);
	DLOAD(vkEndCommandBuffer);
	DLOAD(vkQueueSubmit);
	DLOAD(vkQueueWaitIdle);
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t host_type = 0;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		say("memory type %u: flags %#x heap %u\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);
		if ((mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
			host_type = i;
		}
	}

	const uint32_t W = 64, H = 64;
	VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		                 .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		                 .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		                 .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage img;
	CHECK("vkCreateImage", vkCreateImage(dev, &ii, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	uint32_t img_type = 0;
	while (img_type < mp.memoryTypeCount && !(mr.memoryTypeBits & (1u << img_type))) {
		img_type++;
	}
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		                     .memoryTypeIndex = img_type };
	VkDeviceMemory imem;
	CHECK("vkAllocateMemory (image)", vkAllocateMemory(dev, &mai, NULL, &imem));
	CHECK("vkBindImageMemory", vkBindImageMemory(dev, img, imem, 0));

	VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4,
		                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf;
	CHECK("vkCreateBuffer", vkCreateBuffer(dev, &bi, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = host_type;
	VkDeviceMemory bmem;
	CHECK("vkAllocateMemory (buffer)", vkAllocateMemory(dev, &mai, NULL, &bmem));
	CHECK("vkBindBufferMemory", vkBindBufferMemory(dev, buf, bmem, 0));
	uint32_t *pixels;
	CHECK("vkMapMemory", vkMapMemory(dev, bmem, 0, W * H * 4, 0, (void **)&pixels));
	memset(pixels, 0xee, W * H * 4);

	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CHECK("vkCreateCommandPool", vkCreateCommandPool(dev, &cpi, NULL, &pool));
	VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK("vkAllocateCommandBuffers", vkAllocateCommandBuffers(dev, &cbi, &cb));
	VkCommandBufferBeginInfo bgi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK("vkBeginCommandBuffer", vkBeginCommandBuffer(cb, &bgi));
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		                    .image = img, .subresourceRange = range };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
	VkClearColorValue colour = { .float32 = { 1.0f, 0.5f, 0.25f, 1.0f } };   /* -> ff 80 40 ff */
	vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
	VkImageMemoryBarrier b2 = b1;
	b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
	VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
	CHECK("vkEndCommandBuffer", vkEndCommandBuffer(cb));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	CHECK("vkQueueSubmit", vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CHECK("vkQueueWaitIdle", vkQueueWaitIdle(q));

	unsigned good = 0;
	for (uint32_t i = 0; i < W * H; i++) {
		good += pixels[i] == 0xff4080ffu;      /* R ff, G 80, B 40, A ff, little endian */
	}
	say("pixel 0 %#010x, pixel %u %#010x: %u of %u pixels are the clear colour -> %s\n", pixels[0], W * H - 1,
	    pixels[W * H - 1], good, W * H, good == W * H ? "VULKAN WORKS ON THE MALI-G72" : "not right");
	return good == W * H ? 0 : 2;
}
