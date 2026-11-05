/* SPDX-License-Identifier: MIT
 * Host-only acceptance fixture: actual Turnip SPIR-V -> NIR -> A619 IR3.
 * The selected Mesa no-op DRM frontend provides compile-time device facts.
 * It is NOT a phone driver or hardware test. Never allocates a command buffer,
 * records a dispatch, submits a queue, advertises MTLDevice or executes work.
 * Input contract: compute, set0 sequential storage buffers, one push range;
 * or a triangle-list vertex/fragment pipeline, no inputs/descriptors/pushes,
 * one unblended B8G8R8A8_UNORM attachment. No render pass is ever executed.
 * The caller must verify this contract against the translator's actual JSON.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { \
    fprintf(stderr,"%s failed: %d\n",#call,r); exit(2); } } while(0)
static unsigned number(const char *s,unsigned limit) {
    char *end;unsigned long n=strtoul(s,&end,10);
    if(!*s || *end || n>limit) { fprintf(stderr,"invalid bounded integer\n");exit(2); }
    return (unsigned)n;
}
static VkShaderModule load_module(VkDevice device,const char *path) {
    FILE *f=fopen(path,"rb");if(!f)exit(2);
    if(fseek(f,0,SEEK_END))exit(2);
    long bytes=ftell(f);
    if(bytes<20 || bytes>16*1024*1024 || bytes%4 || fseek(f,0,SEEK_SET))exit(2);
    uint32_t *words=malloc((size_t)bytes);if(!words)exit(2);
    if(fread(words,1,(size_t)bytes,f)!=(size_t)bytes || fclose(f) || words[0]!=0x07230203)exit(2);
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=words};
    VkShaderModule module;CHECK(vkCreateShaderModule(device,&mi,NULL,&module));free(words);
    return module;
}
int main(int argc,char **argv) {
    if(argc!=5) { fprintf(stderr,"usage: offline_ir3 SPIRV STORAGE_BINDINGS PUSH_BYTES OUTPUT_PREFIX\n       offline_ir3 --render VERTEX_SPIRV FRAGMENT_SPIRV OUTPUT_PREFIX\n");return 2; }
    const char *gpu=getenv("FD_GPU_ID"),*preload=getenv("LD_PRELOAD");
    if(!gpu || strcmp(gpu,"619") || !preload || !strstr(preload,"libfreedreno_noop_drm_shim.so")) {
        fprintf(stderr,"this offline fixture requires the Mesa no-op DRM frontend and explicit A619 target\n");return 2;
    }
    int render=!strcmp(argv[1],"--render");
    unsigned bindings=render?0:number(argv[2],32),push=render?0:number(argv[3],128);
    if(push%4) return 2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="DRE offline IR3 compiler only",.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance;CHECK(vkCreateInstance(&ci,NULL,&instance));
    uint32_t count=0;CHECK(vkEnumeratePhysicalDevices(instance,&count,NULL));if(count!=1)return 2;
    VkPhysicalDevice physical;CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);
    if(!strstr(properties.deviceName,"619") || properties.deviceID!=0x06010900) { fprintf(stderr,"refusing non-A619-Holi compile target\n");return 2; }
    printf("OFFLINE COMPILE ONLY; no GPU execution: %s deviceID=%x\n",properties.deviceName,properties.deviceID);
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,NULL);
    if(!families || families>32)return 2;
    VkQueueFamilyProperties family[32];vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,family);
    unsigned queue=0;while(queue<families && !(family[queue].queueFlags&VK_QUEUE_COMPUTE_BIT))queue++;
    if(queue==families)return 2;
    float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=queue,.queueCount=1,.pQueuePriorities=&priority};
    const char *extension=VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR feature={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,.pipelineExecutableInfo=VK_TRUE};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&feature,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
    VkDevice device;CHECK(vkCreateDevice(physical,&di,NULL,&device));
    VkDescriptorSetLayoutBinding binding[32]={0};
    for(unsigned i=0;i<bindings;i++)binding[i]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=bindings,.pBindings=binding};
    VkDescriptorSetLayout set;CHECK(vkCreateDescriptorSetLayout(device,&si,NULL,&set));
    VkPushConstantRange range={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=push};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set,.pushConstantRangeCount=push?1:0,.pPushConstantRanges=&range};
    VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&li,NULL,&layout));
    VkShaderModule module=load_module(device,render?argv[2]:argv[1]),fragment=VK_NULL_HANDLE;
    VkRenderPass pass=VK_NULL_HANDLE;VkPipeline pipeline;
    VkPipelineCreateFlags capture=VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR|VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    if(!render) {
        VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .flags=capture,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=layout};
        CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline));
    } else {
        fragment=load_module(device,argv[3]);
        VkAttachmentDescription attachment={.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
            .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference reference={.attachment=0,.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&reference};
        VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass};
        CHECK(vkCreateRenderPass(device,&ri,NULL,&pass));
        VkPipelineShaderStageCreateInfo stages[2]={
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=module,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
        VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo viewport={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
        VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
        VkPipelineMultisampleStateCreateInfo samples={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};
        VkPipelineColorBlendStateCreateInfo color={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
        VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_DEPTH_BOUNDS,VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
        VkPipelineDynamicStateCreateInfo dynamics={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=sizeof(dynamic)/sizeof(dynamic[0]),.pDynamicStates=dynamic};
        VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.flags=capture,
            .stageCount=2,.pStages=stages,.pVertexInputState=&vertex,.pInputAssemblyState=&assembly,
            .pViewportState=&viewport,.pRasterizationState=&raster,.pMultisampleState=&samples,
            .pColorBlendState=&color,.pDynamicState=&dynamics,.layout=layout,.renderPass=pass};
        CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline));
    }
    PFN_vkGetPipelineExecutablePropertiesKHR properties_fn=(PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(device,"vkGetPipelineExecutablePropertiesKHR");
    PFN_vkGetPipelineExecutableInternalRepresentationsKHR reps_fn=(PFN_vkGetPipelineExecutableInternalRepresentationsKHR)vkGetDeviceProcAddr(device,"vkGetPipelineExecutableInternalRepresentationsKHR");
    if(!properties_fn || !reps_fn)return 2;
    VkPipelineInfoKHR info={.sType=VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR,.pipeline=pipeline};
    uint32_t executables=0;CHECK(properties_fn(device,&info,&executables,NULL));
    if(!executables || executables>16)return 2;
    unsigned saved=0;
    for(uint32_t e=0;e<executables;e++) {
        VkPipelineExecutableInfoKHR ei={.sType=VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,.pipeline=pipeline,.executableIndex=e};
        uint32_t n=0;CHECK(reps_fn(device,&ei,&n,NULL));if(n>16)return 2;
        VkPipelineExecutableInternalRepresentationKHR reps[16]={0};
        for(uint32_t i=0;i<n;i++)reps[i].sType=VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
        CHECK(reps_fn(device,&ei,&n,reps));
        for(uint32_t i=0;i<n;i++) { if(!reps[i].dataSize || reps[i].dataSize>16*1024*1024)return 2;reps[i].pData=malloc(reps[i].dataSize);if(!reps[i].pData)return 2; }
        CHECK(reps_fn(device,&ei,&n,reps));
        for(uint32_t i=0;i<n;i++) {
            char path[4096];int length=snprintf(path,sizeof(path),"%s.%u.%u.txt",argv[4],e,i);
            if(length<0 || (size_t)length>=sizeof(path))return 2;
            if(!reps[i].isText)return 2;
            FILE *f=fopen(path,"wbx");if(!f)return 2;
            if(fwrite(reps[i].pData,1,reps[i].dataSize,f)!=reps[i].dataSize || fclose(f))return 2;
            printf("representation=%s bytes=%zu file=%s\n",reps[i].name,reps[i].dataSize,path);
            saved++;free(reps[i].pData);
        }
    }
    vkDestroyPipeline(device,pipeline,NULL);vkDestroyShaderModule(device,module,NULL);
    if(fragment)vkDestroyShaderModule(device,fragment,NULL);
    if(pass)vkDestroyRenderPass(device,pass,NULL);
    vkDestroyPipelineLayout(device,layout,NULL);vkDestroyDescriptorSetLayout(device,set,NULL);
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);
    return saved?0:2;
}
