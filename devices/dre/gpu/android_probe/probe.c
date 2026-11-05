/* SPDX-License-Identifier: MIT
 * Execute qualified project-MSL→metallib→SPIR-V shaders using Android's real
 * Vulkan driver. This does NOT execute Mesa IR3 or provide native XNU/Metal. */
#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "qualified_shaders.h"

#define COUNT 257u
#define BUFFER_BYTES 2048u
#define WAIT_NS UINT64_C(5000000000)
struct buffer { VkBuffer object;VkDeviceMemory memory;VkDeviceSize bytes,allocated;void *cpu;VkMemoryPropertyFlags flags; };
struct context {
    VkInstance instance;VkPhysicalDevice physical;VkDevice device;VkQueue queue;
    VkPhysicalDeviceProperties properties;VkPhysicalDeviceMemoryProperties memory;
    VkQueueFamilyProperties family;uint32_t family_index,subgroup;
    VkCommandPool pool;VkCommandBuffer commands;VkFence fence;VkQueryPool queries;
    struct buffer buffers[6];unsigned buffer_count;
    VkImage image;VkDeviceMemory image_memory;VkImageView image_view;
    VkRenderPass renderpass;VkFramebuffer framebuffer;
    VkDescriptorSetLayout descriptors;VkDescriptorPool descriptor_pool;VkDescriptorSet descriptor_set;
    VkPipelineLayout compute_layout,render_layout;VkPipeline compute,render;
    VkShaderModule shaders[3];
    const char *stage;VkResult error;
    uint64_t ticks[4],delta[2];int compute_pass,render_pass,counters_pass;
    uint32_t compute_checked,pixels_checked,input_guards,output_guards;
    const char *mismatch_kind;uint32_t mismatch_index,mismatch_actual,mismatch_expected;
};
static struct context c;
static void timeout_handler(int signal_number)
{
    (void)signal_number;
    static const char msg[]="{\"schema\":1,\"status\":\"TIMEOUT\",\"stage\":\"process_deadline\",\"native_xnu_metal_proven\":false}\n";
    (void)write(STDOUT_FILENO,msg,sizeof(msg)-1);_exit(124);
}
static void json_string(FILE *out,const char *value)
{
    fputc('"',out);
    for (const unsigned char *p=(const unsigned char *)value;*p;p++) {
        if (*p=='"' || *p=='\\')fputc('\\',out);
        if (*p<32)fprintf(out,"\\u%04x",*p);else fputc(*p,out);
    }
    fputc('"',out);
}
static void report(FILE *out,int pass)
{
    fprintf(out,"{\"schema\":1,\"operation\":\"android-vulkan-project-metal-shader-test\",\"status\":\"%s\",\"uid\":%u,\"stage\":",pass?"PASS":"FAIL",(unsigned)getuid());
    json_string(out,c.stage?c.stage:"startup");fprintf(out,",\"vk_result\":%d,\"device_name\":",c.error);
    json_string(out,c.properties.deviceName);
    fprintf(out,",\"vendor_id\":%u,\"device_id\":%u,\"device_type\":%u,\"api_version\":%u,\"driver_version\":%u,\"subgroup_size\":%u,",
        c.properties.vendorID,c.properties.deviceID,c.properties.deviceType,c.properties.apiVersion,c.properties.driverVersion,c.subgroup);
    fprintf(out,"\"compute_exact_pass\":%s,\"compute_values_checked\":%u,\"input_guard_words_checked\":%u,\"output_guard_words_checked\":%u,",
        c.compute_pass?"true":"false",c.compute_checked,c.input_guards,c.output_guards);
    fprintf(out,"\"offscreen_bgra_pass\":%s,\"pixels_checked\":%u,\"gpu_timestamp_activity\":%s,\"timestamp_valid_bits\":%u,\"timestamp_period_ns\":%.9g,",
        c.render_pass?"true":"false",c.pixels_checked,c.counters_pass?"true":"false",c.family.timestampValidBits,
        isfinite(c.properties.limits.timestampPeriod)?c.properties.limits.timestampPeriod:0.0);
    fprintf(out,"\"gpu_timestamp_ticks\":[%llu,%llu,%llu,%llu],\"gpu_elapsed_ticks\":[%llu,%llu],\"device_input_memory_flags\":%u,\"device_output_memory_flags\":%u,",
        (unsigned long long)c.ticks[0],(unsigned long long)c.ticks[1],(unsigned long long)c.ticks[2],(unsigned long long)c.ticks[3],
        (unsigned long long)c.delta[0],(unsigned long long)c.delta[1],c.buffers[1].flags,c.buffers[2].flags);
    fprintf(out,"\"mismatch_kind\":");json_string(out,c.mismatch_kind?c.mismatch_kind:"");
    fprintf(out,",\"mismatch_index\":%u,\"mismatch_actual\":%u,\"mismatch_expected\":%u,",c.mismatch_index,c.mismatch_actual,c.mismatch_expected);
    fprintf(out,"\"driver_path\":\"existing Android Vulkan/KGSL\",\"mesa_ir3_execution_proven\":false,\"native_xnu_execution_proven\":false,\"metal_runtime_proven\":false,\"native_xnu_metal_proven\":false,\"shader_qualification\":%s}\n",dre_shader_qualification);
}
static int mismatch(const char *kind,uint32_t index,uint32_t actual,uint32_t expected)
{ c.mismatch_kind=kind;c.mismatch_index=index;c.mismatch_actual=actual;c.mismatch_expected=expected;return 0; }
static int finish(const char *path,int pass)
{
    if (path) { FILE *f=fopen(path,"wx");if (!f)pass=0;else { report(f,pass);if (fclose(f))pass=0; } }
    report(stdout,pass);fflush(stdout);return pass?0:1;
}
#define VK(call) do { c.error=(call);if (c.error!=VK_SUCCESS)return 0; } while (0)
static int initialize(void)
{
    c.stage="uid2000";if (getuid()!=2000)return 0;
    c.stage="instance";
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="dre-project-shader-probe",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VK(vkCreateInstance(&info,0,&c.instance));
    VkPhysicalDevice devices[16];uint32_t n=16;VK(vkEnumeratePhysicalDevices(c.instance,&n,devices));
    c.stage="physical_A619";
    for (uint32_t i=0;i<n;i++) {
        VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(devices[i],&p);
        if (p.vendorID==0x5143 && p.deviceID==0x06010900 && p.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
            strstr(p.deviceName,"Adreno") && strstr(p.deviceName,"619")) { c.physical=devices[i];c.properties=p;break; }
    }
    if (!c.physical || c.properties.apiVersion<VK_API_VERSION_1_1 || c.properties.limits.maxPushConstantsSize<40 ||
        c.properties.limits.maxComputeWorkGroupInvocations<64 || c.properties.limits.maxComputeWorkGroupSize[0]<64 ||
        c.properties.limits.maxComputeWorkGroupCount[0]<5)return 0;
    PFN_vkGetPhysicalDeviceProperties2 get_properties=(PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(c.instance,"vkGetPhysicalDeviceProperties2");
    if (!get_properties)return 0;
    VkPhysicalDeviceSubgroupProperties subgroup={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&subgroup};
    get_properties(c.physical,&properties);c.subgroup=subgroup.subgroupSize;
    if (c.subgroup!=64)return 0;
    c.stage="spirv14_extensions";
    VkExtensionProperties extensions[512];n=512;VK(vkEnumerateDeviceExtensionProperties(c.physical,0,&n,extensions));
    const char *required[]={VK_KHR_SPIRV_1_4_EXTENSION_NAME,VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME};
    for (unsigned i=0;i<2;i++) {
        int found=0;for (uint32_t j=0;j<n;j++)if (!strcmp(required[i],extensions[j].extensionName))found=1;
        if (!found)return 0;
    }
    c.stage="queue_timestamp_capabilities";
    VkQueueFamilyProperties families[32];n=32;vkGetPhysicalDeviceQueueFamilyProperties(c.physical,&n,families);
    int found=0;
    for (uint32_t i=0;i<n;i++)if ((families[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT) &&
        families[i].queueCount && families[i].timestampValidBits>=32 && families[i].timestampValidBits<=64) { c.family_index=i;c.family=families[i];found=1;break; }
    if (!found || !isfinite(c.properties.limits.timestampPeriod) || c.properties.limits.timestampPeriod<=0)return 0;
    c.stage="logical_device";float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=c.family_index,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
        .enabledExtensionCount=2,.ppEnabledExtensionNames=required};
    VK(vkCreateDevice(c.physical,&di,0,&c.device));vkGetDeviceQueue(c.device,c.family_index,0,&c.queue);
    vkGetPhysicalDeviceMemoryProperties(c.physical,&c.memory);
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=c.family_index};
    VK(vkCreateCommandPool(c.device,&pi,0,&c.pool));
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=c.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VK(vkAllocateCommandBuffers(c.device,&ai,&c.commands));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK(vkCreateFence(c.device,&fi,0,&c.fence));
    VkQueryPoolCreateInfo query={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=4};
    VK(vkCreateQueryPool(c.device,&query,0,&c.queries));return 1;
}
static int memory_type(uint32_t bits,VkMemoryPropertyFlags required,uint32_t *index)
{
    int found=0;unsigned best=0;
    for (uint32_t i=0;i<c.memory.memoryTypeCount;i++) {
        VkMemoryPropertyFlags f=c.memory.memoryTypes[i].propertyFlags;
        if (!(bits&(1u<<i)) || (f&required)!=required)continue;
        unsigned score=(f&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT?2:0)+(f&VK_MEMORY_PROPERTY_HOST_CACHED_BIT?1:0);
        if (!found || score>best) { found=1;best=score;*index=i; }
    }
    return found;
}
static int buffer(VkDeviceSize bytes,VkBufferUsageFlags usage,VkMemoryPropertyFlags flags,struct buffer **out)
{
    if (c.buffer_count>=6)return 0;
    struct buffer *b=&c.buffers[c.buffer_count++];b->bytes=bytes;
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VK(vkCreateBuffer(c.device,&info,0,&b->object));
    VkMemoryRequirements r;vkGetBufferMemoryRequirements(c.device,b->object,&r);
    uint32_t type=0;if (r.size>8u*1024u*1024u || !memory_type(r.memoryTypeBits,flags,&type))return 0;
    b->allocated=r.size;b->flags=c.memory.memoryTypes[type].propertyFlags;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=type};
    VK(vkAllocateMemory(c.device,&ai,0,&b->memory));VK(vkBindBufferMemory(c.device,b->object,b->memory,0));
    if (flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)VK(vkMapMemory(c.device,b->memory,0,VK_WHOLE_SIZE,0,&b->cpu));
    *out=b;return 1;
}
static int cache(struct buffer *b,int invalidate)
{
    if (b->flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)return 1;
    VkMappedMemoryRange r={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=b->memory,.offset=0,.size=VK_WHOLE_SIZE};
    if (invalidate)VK(vkInvalidateMappedMemoryRanges(c.device,1,&r));else VK(vkFlushMappedMemoryRanges(c.device,1,&r));
    return 1;
}
static int shader(const uint32_t *words,size_t bytes,VkShaderModule *module)
{
    if (bytes<20 || bytes>65536 || bytes%4 || words[0]!=0x07230203 || words[1]!=0x10400)return 0;
    VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=words};
    VK(vkCreateShaderModule(c.device,&info,0,module));return 1;
}
static int begin(unsigned query)
{
    VK(vkResetCommandBuffer(c.commands,0));
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK(vkBeginCommandBuffer(c.commands,&bi));vkCmdResetQueryPool(c.commands,c.queries,query,2);
    vkCmdWriteTimestamp(c.commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,c.queries,query);return 1;
}
static int submit(unsigned query)
{
    vkCmdWriteTimestamp(c.commands,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,c.queries,query+1);
    VK(vkEndCommandBuffer(c.commands));VK(vkResetFences(c.device,1,&c.fence));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&c.commands};
    VK(vkQueueSubmit(c.queue,1,&si,c.fence));VK(vkWaitForFences(c.device,1,&c.fence,VK_TRUE,WAIT_NS));
    uint64_t data[4]={0};
    VK(vkGetQueryPoolResults(c.device,c.queries,query,2,sizeof(data),data,16,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
    if (!data[1] || !data[3])return 0;
    c.ticks[query]=data[0];c.ticks[query+1]=data[2];
    uint64_t mask=c.family.timestampValidBits==64?UINT64_MAX:(UINT64_C(1)<<c.family.timestampValidBits)-1;
    c.delta[query/2]=(data[2]-data[0])&mask;return c.delta[query/2]!=0;
}
static VkBufferMemoryBarrier barrier_for(struct buffer *b,VkAccessFlags src,VkAccessFlags dst)
{
    return (VkBufferMemoryBarrier){.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=src,.dstAccessMask=dst,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=b->object,.offset=0,.size=VK_WHOLE_SIZE};
}
static uint32_t input_value(uint32_t i) { return 0x9e3779b9u*(i+1)^0xa51eu; }
static uint32_t expected_value(uint32_t i)
{
    uint32_t shift=i&31,v=input_value(i),rotated=shift?(v<<shift)|(v>>(32-shift)):v;
    return (rotated^0xa619d22au)+i*0x10203u;
}
static int compute(void)
{
    c.stage="compute_buffers";
    struct buffer *upload,*in,*out,*readback,*input_readback;
    if (!buffer(COUNT*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,&upload) ||
        !buffer(BUFFER_BYTES,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&in) ||
        !buffer(BUFFER_BYTES,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&out) ||
        !buffer(BUFFER_BYTES,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,&readback) ||
        !buffer(BUFFER_BYTES,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,&input_readback))return 0;
    for (uint32_t i=0;i<COUNT;i++)((uint32_t *)upload->cpu)[i]=input_value(i);
    if (!cache(upload,0))return 0;
    c.stage="compute_pipeline";
    VkDescriptorSetLayoutBinding bindings[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VK(vkCreateDescriptorSetLayout(c.device,&li,0,&c.descriptors));
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=40};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&c.descriptors,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    VK(vkCreatePipelineLayout(c.device,&pl,0,&c.compute_layout));
    if (!shader(dre_compute_spv,sizeof(dre_compute_spv),&c.shaders[0]))return 0;
    const uint32_t spec_data[4]={64,1,1,0};
    VkSpecializationMapEntry entries[4];for (uint32_t i=0;i<4;i++)entries[i]=(VkSpecializationMapEntry){i,i*4,4};
    VkSpecializationInfo spec={.mapEntryCount=4,.pMapEntries=entries,.dataSize=sizeof(spec_data),.pData=spec_data};
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=c.shaders[0],.pName="main",.pSpecializationInfo=&spec},.layout=c.compute_layout};
    VK(vkCreateComputePipelines(c.device,0,1,&ci,0,&c.compute));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    VK(vkCreateDescriptorPool(c.device,&dpi,0,&c.descriptor_pool));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=c.descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&c.descriptors};
    VK(vkAllocateDescriptorSets(c.device,&dai,&c.descriptor_set));
    VkDescriptorBufferInfo db[2]={{in->object,0,BUFFER_BYTES},{out->object,0,BUFFER_BYTES}};
    VkWriteDescriptorSet writes[2];for (uint32_t i=0;i<2;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=c.descriptor_set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=db+i};
    vkUpdateDescriptorSets(c.device,2,writes,0,0);
    c.stage="compute_submit";if (!begin(0))return 0;
    vkCmdFillBuffer(c.commands,in->object,0,BUFFER_BYTES,0x13579bdf);vkCmdFillBuffer(c.commands,out->object,0,BUFFER_BYTES,0xdeadbeef);
    VkBufferMemoryBarrier fill=barrier_for(in,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(c.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,1,&fill,0,0);
    VkBufferCopy copy={0,256,COUNT*4};vkCmdCopyBuffer(c.commands,upload->object,in->object,1,&copy);
    VkBufferMemoryBarrier before[2]={barrier_for(in,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT),barrier_for(out,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT)};
    vkCmdPipelineBarrier(c.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,0,2,before,0,0);
    vkCmdBindPipeline(c.commands,VK_PIPELINE_BIND_POINT_COMPUTE,c.compute);
    vkCmdBindDescriptorSets(c.commands,VK_PIPELINE_BIND_POINT_COMPUTE,c.compute_layout,0,1,&c.descriptor_set,0,0);
    uint32_t constants[10]={0};constants[8]=256;constants[9]=512;
    vkCmdPushConstants(c.commands,c.compute_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,40,constants);vkCmdDispatch(c.commands,5,1,1);
    VkBufferMemoryBarrier after[2]={barrier_for(out,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT),barrier_for(in,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT)};
    vkCmdPipelineBarrier(c.commands,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,2,after,0,0);
    copy=(VkBufferCopy){0,0,BUFFER_BYTES};vkCmdCopyBuffer(c.commands,out->object,readback->object,1,&copy);vkCmdCopyBuffer(c.commands,in->object,input_readback->object,1,&copy);
    VkBufferMemoryBarrier host[2]={barrier_for(readback,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT),barrier_for(input_readback,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT)};
    vkCmdPipelineBarrier(c.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,0,2,host,0,0);
    if (!submit(0) || !cache(readback,1) || !cache(input_readback,1))return 0;
    c.stage="compute_exact_values_and_guards";
    for (uint32_t i=0;i<BUFFER_BYTES/4;i++) {
        int output_data=i>=128 && i<128+COUNT,input_data=i>=64 && i<64+COUNT;
        uint32_t expected=output_data?expected_value(i-128):0xdeadbeef;
        if (((uint32_t *)readback->cpu)[i]!=expected)return mismatch(output_data?"compute_value":"output_guard",i,((uint32_t *)readback->cpu)[i],expected);
        expected=input_data?input_value(i-64):0x13579bdf;
        if (((uint32_t *)input_readback->cpu)[i]!=expected)return mismatch(input_data?"input_value":"input_guard",i,((uint32_t *)input_readback->cpu)[i],expected);
        if (output_data)c.compute_checked++;else c.output_guards++;
        if (!input_data)c.input_guards++;
    }
    c.compute_pass=1;return 1;
}
static int render(void)
{
    c.stage="render_image";
    VkFormatProperties format;vkGetPhysicalDeviceFormatProperties(c.physical,VK_FORMAT_B8G8R8A8_UNORM,&format);
    if ((format.optimalTilingFeatures&(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))!=(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))return 0;
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_B8G8R8A8_UNORM,
        .extent={64,64,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VK(vkCreateImage(c.device,&ii,0,&c.image));
    VkMemoryRequirements r;vkGetImageMemoryRequirements(c.device,c.image,&r);uint32_t type=0;
    if (r.size>8u*1024u*1024u || !memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&type))return 0;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=type};
    VK(vkAllocateMemory(c.device,&ai,0,&c.image_memory));VK(vkBindImageMemory(c.device,c.image,c.image_memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=c.image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VK(vkCreateImageView(c.device,&vi,0,&c.image_view));
    VkAttachmentDescription attachment={.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&reference};
    VkSubpassDependency dependency={.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=1,.pDependencies=&dependency};
    VK(vkCreateRenderPass(c.device,&ri,0,&c.renderpass));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=c.renderpass,.attachmentCount=1,.pAttachments=&c.image_view,.width=64,.height=64,.layers=1};
    VK(vkCreateFramebuffer(c.device,&fi,0,&c.framebuffer));
    c.stage="render_pipeline";
    if (!shader(dre_vertex_spv,sizeof(dre_vertex_spv),&c.shaders[1]) || !shader(dre_fragment_spv,sizeof(dre_fragment_spv),&c.shaders[2]))return 0;
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};VK(vkCreatePipelineLayout(c.device,&pl,0,&c.render_layout));
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=c.shaders[1],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=c.shaders[2],.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,64,64,0,1};VkRect2D scissor={{0,0},{64,64}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState color={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&color};
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vertex,.pInputAssemblyState=&input,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,
        .pColorBlendState=&blend,.layout=c.render_layout,.renderPass=c.renderpass};
    VK(vkCreateGraphicsPipelines(c.device,0,1,&gp,0,&c.render));
    struct buffer *readback;if (!buffer(64*64*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,&readback))return 0;
    c.stage="render_submit";if (!begin(2))return 0;
    VkClearValue clear={.color={{0,0,0,0}}};
    VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=c.renderpass,.framebuffer=c.framebuffer,
        .renderArea={{0,0},{64,64}},.clearValueCount=1,.pClearValues=&clear};
    vkCmdBeginRenderPass(c.commands,&bi,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(c.commands,VK_PIPELINE_BIND_POINT_GRAPHICS,c.render);
    vkCmdDraw(c.commands,3,1,0,0);vkCmdEndRenderPass(c.commands);
    VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=c.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(c.commands,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&ib);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={64,64,1}};
    vkCmdCopyImageToBuffer(c.commands,c.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback->object,1,&copy);
    VkBufferMemoryBarrier host=barrier_for(readback,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    vkCmdPipelineBarrier(c.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,0,1,&host,0,0);
    if (!submit(2) || !cache(readback,1))return 0;
    c.stage="render_exact_bgra_pixels";
    for (unsigned y=0;y<64;y++)for (unsigned x=0;x<64;x++) {
        const uint8_t *p=(const uint8_t *)readback->cpu+y*256+x*4;
        if (p[0]!=((x*3+y*5)&255) || p[1]!=(x^y) || p[2]!=165 || p[3]!=255)
            return mismatch("bgra_pixel",y*64+x,(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24,
                ((x*3+y*5)&255)|((x^y)<<8)|(165u<<16)|(255u<<24));
        c.pixels_checked++;
    }
    c.render_pass=1;c.counters_pass=c.delta[0]!=0 && c.delta[1]!=0;return c.counters_pass;
}
static void cleanup(void)
{
    /* Reached only after both submitted batches completed their fences. Never
     * wait indefinitely or free in-flight objects on a failed/expired wait. */
    vkDestroyPipeline(c.device,c.compute,0);vkDestroyPipeline(c.device,c.render,0);
    vkDestroyPipelineLayout(c.device,c.compute_layout,0);vkDestroyPipelineLayout(c.device,c.render_layout,0);
    for (unsigned i=0;i<3;i++)vkDestroyShaderModule(c.device,c.shaders[i],0);
    vkDestroyDescriptorPool(c.device,c.descriptor_pool,0);vkDestroyDescriptorSetLayout(c.device,c.descriptors,0);
    vkDestroyFramebuffer(c.device,c.framebuffer,0);vkDestroyRenderPass(c.device,c.renderpass,0);
    vkDestroyImageView(c.device,c.image_view,0);vkDestroyImage(c.device,c.image,0);vkFreeMemory(c.device,c.image_memory,0);
    for (unsigned i=0;i<c.buffer_count;i++) {
        if (c.buffers[i].cpu)vkUnmapMemory(c.device,c.buffers[i].memory);
        vkDestroyBuffer(c.device,c.buffers[i].object,0);vkFreeMemory(c.device,c.buffers[i].memory,0);
    }
    vkDestroyQueryPool(c.device,c.queries,0);vkDestroyFence(c.device,c.fence,0);vkDestroyCommandPool(c.device,c.pool,0);
    vkDestroyDevice(c.device,0);vkDestroyInstance(c.instance,0);
}
int main(int argc,char **argv)
{
    if (argc>2)return 2;
    signal(SIGALRM,timeout_handler);alarm(30);
    if (!initialize() || !compute() || !render()) { int status=finish(argc==2?argv[1]:0,0);_exit(status); }
    c.stage="cleanup_completed_work";cleanup();c.stage="complete";alarm(0);return finish(argc==2?argv[1]:0,1);
}
