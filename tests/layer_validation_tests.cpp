#include <vulkan.h>
#include "vk_debug_report_lunarg.h"
#include "test_common.h"
#include "vkrenderframework.h"
#include "vk_layer_config.h"
#include "../icd/common/icd-spv.h"

#define GLM_FORCE_RADIANS
#include "glm/glm.hpp"
#include <glm/gtc/matrix_transform.hpp>

#define MEM_TRACKER_TESTS 1

#define OBJ_TRACKER_TESTS 1
#define DRAW_STATE_TESTS 1
#define THREADING_TESTS 1
#define SHADER_CHECKER_TESTS 1
#define DEVICE_LIMITS_TESTS 1
#define IMAGE_TESTS 1

//--------------------------------------------------------------------------------------
// Mesh and VertexFormat Data
//--------------------------------------------------------------------------------------
struct Vertex
{
    float posX, posY, posZ, posW;    // Position data
    float r, g, b, a;                // Color
};

#define XYZ1(_x_, _y_, _z_)         (_x_), (_y_), (_z_), 1.f

typedef enum _BsoFailSelect {
    BsoFailNone                     = 0x00000000,
    BsoFailLineWidth                = 0x00000001,
    BsoFailDepthBias                = 0x00000002,
    BsoFailViewport                 = 0x00000004,
    BsoFailScissor                  = 0x00000008,
    BsoFailBlend                    = 0x00000010,
    BsoFailDepthBounds              = 0x00000020,
    BsoFailStencilReadMask          = 0x00000040,
    BsoFailStencilWriteMask         = 0x00000080,
    BsoFailStencilReference         = 0x00000100,
} BsoFailSelect;

struct vktriangle_vs_uniform {
    // Must start with MVP
    float   mvp[4][4];
    float   position[3][4];
    float   color[3][4];
};

static const char bindStateVertShaderText[] =
        "#version 130\n"
        "vec2 vertices[3];\n"
        "void main() {\n"
        "      vertices[0] = vec2(-1.0, -1.0);\n"
        "      vertices[1] = vec2( 1.0, -1.0);\n"
        "      vertices[2] = vec2( 0.0,  1.0);\n"
        "   gl_Position = vec4(vertices[gl_VertexID % 3], 0.0, 1.0);\n"
        "}\n";

static const char bindStateFragShaderText[] =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location = 0) out vec4 uFragColor;\n"
        "void main(){\n"
        "   uFragColor = vec4(0,1,0,1);\n"
        "}\n";

static VkBool32 myDbgFunc(
    VkFlags                             msgFlags,
    VkDbgObjectType                     objType,
    uint64_t                            srcObject,
    size_t                              location,
    int32_t                             msgCode,
    const char*                         pLayerPrefix,
    const char*                         pMsg,
    void*                               pUserData);

// ********************************************************
// ErrorMonitor Usage:
//
// Call SetDesiredFailureMsg with a string to be compared against all
// encountered log messages. Passing NULL will match all log messages.
// logMsg will return true for skipCall only if msg is matched or NULL.
//
// Call DesiredMsgFound to determine if the desired failure message
// was encountered.

class ErrorMonitor {
public:
    ErrorMonitor()
    {
        test_platform_thread_create_mutex(&m_mutex);
        test_platform_thread_lock_mutex(&m_mutex);
        m_msgFlags = VK_DBG_REPORT_INFO_BIT;
        m_bailout  = NULL;
        test_platform_thread_unlock_mutex(&m_mutex);
    }

    void SetDesiredFailureMsg(VkFlags msgFlags, const char *msgString)
    {
        test_platform_thread_lock_mutex(&m_mutex);
        m_desiredMsg.clear();
        m_failureMsg.clear();
        m_otherMsgs.clear();
        m_desiredMsg = msgString;
        m_msgFound   = VK_FALSE;
        m_msgFlags   = msgFlags;
        test_platform_thread_unlock_mutex(&m_mutex);
    }

    VkBool32 CheckForDesiredMsg(VkFlags msgFlags, const char *msgString)
    {
        VkBool32 result = VK_FALSE;
        test_platform_thread_lock_mutex(&m_mutex);
        if (m_bailout != NULL) {
            *m_bailout = true;
        }
        string errorString(msgString);
        if (msgFlags & m_msgFlags) {
            if (errorString.find(m_desiredMsg) != string::npos) {
                m_failureMsg = errorString;
                m_msgFound   = VK_TRUE;
                result       = VK_TRUE;
            } else {
                m_otherMsgs.push_back(errorString);
            }
        }
        test_platform_thread_unlock_mutex(&m_mutex);
        return result;
    }

    vector<string> GetOtherFailureMsgs(void)
    {
        return m_otherMsgs;
    }

    string GetFailureMsg(void)
    {
        return m_failureMsg;
    }

    VkBool32 DesiredMsgFound(void)
    {
        return m_msgFound;
    }

    void SetBailout(bool *bailout)
    {
        m_bailout = bailout;
    }

    void DumpFailureMsgs(void)
    {
        vector<string> otherMsgs = GetOtherFailureMsgs();
        cout << "Other error messages logged for this test were:" << endl;
        for (auto iter = otherMsgs.begin(); iter != otherMsgs.end(); iter++) {
            cout << "     " << *iter << endl;
        }
    }

private:
    VkFlags                    m_msgFlags;
    string                     m_desiredMsg;
    string                     m_failureMsg;
    vector<string>             m_otherMsgs;
    test_platform_thread_mutex m_mutex;
    bool*                      m_bailout;
    VkBool32                   m_msgFound;
};

static VkBool32 myDbgFunc(
    VkFlags                    msgFlags,
    VkDbgObjectType            objType,
    uint64_t                   srcObject,
    size_t                     location,
    int32_t                    msgCode,
    const char*                pLayerPrefix,
    const char*                pMsg,
    void*                      pUserData)
{
    if (msgFlags & (VK_DBG_REPORT_WARN_BIT | VK_DBG_REPORT_ERROR_BIT)) {
        ErrorMonitor *errMonitor = (ErrorMonitor *)pUserData;
        return errMonitor->CheckForDesiredMsg(msgFlags, pMsg);
    }
    return false;
}

class VkLayerTest : public VkRenderFramework
{
public:
    VkResult BeginCommandBuffer(VkCommandBufferObj &commandBuffer);
    VkResult EndCommandBuffer(VkCommandBufferObj &commandBuffer);
    void VKTriangleTest(const char *vertShaderText, const char *fragShaderText, BsoFailSelect failMask);
    void GenericDrawPreparation(VkCommandBufferObj *commandBuffer, VkPipelineObj &pipelineobj, VkDescriptorSetObj &descriptorSet, BsoFailSelect failMask);
    void GenericDrawPreparation(VkPipelineObj &pipelineobj, VkDescriptorSetObj &descriptorSet,  BsoFailSelect failMask)
             { GenericDrawPreparation(m_commandBuffer, pipelineobj, descriptorSet, failMask); }

    /* Convenience functions that use built-in command buffer */
    VkResult BeginCommandBuffer() { return BeginCommandBuffer(*m_commandBuffer); }
    VkResult EndCommandBuffer() { return EndCommandBuffer(*m_commandBuffer); }
    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
        { m_commandBuffer->Draw(vertexCount, instanceCount, firstVertex, firstInstance); }
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
        { m_commandBuffer->DrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance); }
    void QueueCommandBuffer() { m_commandBuffer->QueueCommandBuffer(); }
    void QueueCommandBuffer(const VkFence& fence) { m_commandBuffer->QueueCommandBuffer(fence); }
    void BindVertexBuffer(VkConstantBufferObj *vertexBuffer, VkDeviceSize offset, uint32_t binding)
        { m_commandBuffer->BindVertexBuffer(vertexBuffer, offset, binding); }
    void BindIndexBuffer(VkIndexBufferObj *indexBuffer, VkDeviceSize offset)
        { m_commandBuffer->BindIndexBuffer(indexBuffer, offset); }
protected:
        ErrorMonitor               *m_errorMonitor;

    virtual void SetUp() {
        std::vector<const char *> instance_layer_names;
        std::vector<const char *> device_layer_names;
        std::vector<const char *> instance_extension_names;
        std::vector<const char *> device_extension_names;

        instance_extension_names.push_back(VK_DEBUG_REPORT_EXTENSION_NAME);
        /*
         * Since CreateDbgMsgCallback is an instance level extension call
         * any extension / layer that utilizes that feature also needs
         * to be enabled at create instance time.
         */
        // Use Threading layer first to protect others from ThreadCommandBufferCollision test
        instance_layer_names.push_back("Threading");
        instance_layer_names.push_back("ObjectTracker");
        instance_layer_names.push_back("MemTracker");
        instance_layer_names.push_back("DrawState");
        instance_layer_names.push_back("ShaderChecker");
        instance_layer_names.push_back("DeviceLimits");
        instance_layer_names.push_back("Image");

        device_layer_names.push_back("Threading");
        device_layer_names.push_back("ObjectTracker");
        device_layer_names.push_back("MemTracker");
        device_layer_names.push_back("DrawState");
        device_layer_names.push_back("ShaderChecker");
        device_layer_names.push_back("DeviceLimits");
        device_layer_names.push_back("Image");

        this->app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        this->app_info.pNext = NULL;
        this->app_info.pApplicationName = "layer_tests";
        this->app_info.applicationVersion = 1;
        this->app_info.pEngineName = "unittest";
        this->app_info.engineVersion = 1;
        this->app_info.apiVersion = VK_API_VERSION;

        m_errorMonitor = new ErrorMonitor;
        InitFramework(instance_layer_names, device_layer_names,
                      instance_extension_names, device_extension_names,
                      myDbgFunc, m_errorMonitor);
    }

    virtual void TearDown() {
        // Clean up resources before we reset
        ShutdownFramework();
        delete m_errorMonitor;
    }
};

VkResult VkLayerTest::BeginCommandBuffer(VkCommandBufferObj &commandBuffer)
{
    VkResult result;

    result = commandBuffer.BeginCommandBuffer();

    /*
     * For render test all drawing happens in a single render pass
     * on a single command buffer.
     */
    if (VK_SUCCESS == result && renderPass()) {
        commandBuffer.BeginRenderPass(renderPassBeginInfo());
    }

    return result;
}

VkResult VkLayerTest::EndCommandBuffer(VkCommandBufferObj &commandBuffer)
{
    VkResult result;

    if (renderPass()) {
        commandBuffer.EndRenderPass();
    }

    result = commandBuffer.EndCommandBuffer();

    return result;
}

void VkLayerTest::VKTriangleTest(const char *vertShaderText, const char *fragShaderText, BsoFailSelect failMask)
{
    // Create identity matrix
    int i;
    struct vktriangle_vs_uniform data;

    glm::mat4 Projection = glm::mat4(1.0f);
    glm::mat4 View       = glm::mat4(1.0f);
    glm::mat4 Model      = glm::mat4(1.0f);
    glm::mat4 MVP        = Projection * View * Model;
    const int matrixSize = sizeof(MVP);
    const int bufSize    = sizeof(vktriangle_vs_uniform) / sizeof(float);

    memcpy(&data.mvp, &MVP[0][0], matrixSize);

    static const Vertex tri_data[] =
    {
        { XYZ1( -1, -1, 0 ), XYZ1( 1.f, 0.f, 0.f ) },
        { XYZ1(  1, -1, 0 ), XYZ1( 0.f, 1.f, 0.f ) },
        { XYZ1(  0,  1, 0 ), XYZ1( 0.f, 0.f, 1.f ) },
    };

    for (i=0; i<3; i++) {
        data.position[i][0] = tri_data[i].posX;
        data.position[i][1] = tri_data[i].posY;
        data.position[i][2] = tri_data[i].posZ;
        data.position[i][3] = tri_data[i].posW;
        data.color[i][0]    = tri_data[i].r;
        data.color[i][1]    = tri_data[i].g;
        data.color[i][2]    = tri_data[i].b;
        data.color[i][3]    = tri_data[i].a;
    }

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitViewport());

    VkConstantBufferObj constantBuffer(m_device, bufSize*2, sizeof(float), (const void*) &data);

    VkShaderObj vs(m_device,vertShaderText,VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj ps(m_device,fragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipelineobj(m_device);
    pipelineobj.AddColorAttachment();
    pipelineobj.AddShader(&vs);
    pipelineobj.AddShader(&ps);
    if (failMask & BsoFailLineWidth) {
        pipelineobj.MakeDynamic(VK_DYNAMIC_STATE_LINE_WIDTH);
    }
    if (failMask & BsoFailDepthBias) {
        pipelineobj.MakeDynamic(VK_DYNAMIC_STATE_DEPTH_BIAS);
    }
    // Viewport and scissors must stay in synch or other errors will occur than the ones we want
    if (failMask & BsoFailViewport) {
        pipelineobj.MakeDynamic(VK_DYNAMIC_STATE_VIEWPORT);
        m_viewports.clear();
        m_scissors.clear();
    }
    if (failMask & BsoFailScissor) {
        pipelineobj.MakeDynamic(VK_DYNAMIC_STATE_SCISSOR);
        m_scissors.clear();
        m_viewports.clear();
    }
    if (failMask & BsoFailBlend) {
        pipelineobj.MakeDynamic(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
    }
    if (failMask & BsoFailDepthBounds) {
        pipelineobj.MakeDynamic(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
    }
    if (failMask & BsoFailStencilReadMask) {
        pipelineobj.MakeDynamic(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
    }
    if (failMask & BsoFailStencilWriteMask) {
        pipelineobj.MakeDynamic(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
    }
    if (failMask & BsoFailStencilReference) {
        pipelineobj.MakeDynamic(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
    }

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendBuffer(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, constantBuffer);

    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());
    ASSERT_VK_SUCCESS(BeginCommandBuffer());

    GenericDrawPreparation(pipelineobj, descriptorSet, failMask);

    // render triangle
    Draw(3, 1, 0, 0);

    // finalize recording of the command buffer
    EndCommandBuffer();

    QueueCommandBuffer();
}

void VkLayerTest::GenericDrawPreparation(VkCommandBufferObj *commandBuffer, VkPipelineObj &pipelineobj, VkDescriptorSetObj &descriptorSet, BsoFailSelect failMask)
{
    if (m_depthStencil->Initialized()) {
        commandBuffer->ClearAllBuffers(m_clear_color, m_depth_clear_color, m_stencil_clear_color, m_depthStencil);
    } else {
        commandBuffer->ClearAllBuffers(m_clear_color, m_depth_clear_color, m_stencil_clear_color, NULL);
    }

    commandBuffer->PrepareAttachments();
    // Make sure depthWriteEnable is set so that Depth fail test will work correctly
    // Make sure stencilTestEnable is set so that Stencil fail test will work correctly
    VkStencilOpState stencil = {};
    stencil.failOp = VK_STENCIL_OP_KEEP;
    stencil.passOp = VK_STENCIL_OP_KEEP;
    stencil.depthFailOp = VK_STENCIL_OP_KEEP;
    stencil.compareOp = VK_COMPARE_OP_NEVER;

    VkPipelineDepthStencilStateCreateInfo ds_ci = {};
    ds_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds_ci.pNext = NULL;
    ds_ci.depthTestEnable = VK_FALSE;
    ds_ci.depthWriteEnable = VK_TRUE;
    ds_ci.depthCompareOp = VK_COMPARE_OP_NEVER;
    ds_ci.depthBoundsTestEnable = VK_FALSE;
    ds_ci.stencilTestEnable = VK_TRUE;
    ds_ci.front = stencil;
    ds_ci.back = stencil;

    pipelineobj.SetDepthStencil(&ds_ci);
    pipelineobj.SetViewport(m_viewports);
    pipelineobj.SetScissor(m_scissors);
    descriptorSet.CreateVKDescriptorSet(commandBuffer);
    VkResult err = pipelineobj.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());
    ASSERT_VK_SUCCESS(err);
    commandBuffer->BindPipeline(pipelineobj);
    commandBuffer->BindDescriptorSet(descriptorSet);
}

// ********************************************************************************************************************
// ********************************************************************************************************************
// ********************************************************************************************************************
// ********************************************************************************************************************
#if MEM_TRACKER_TESTS
TEST_F(VkLayerTest, CallResetCommandBufferBeforeCompletion)
{
    vk_testing::Fence testFence;
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = NULL;
    fenceInfo.flags = 0;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT, "Resetting CB");

    ASSERT_NO_FATAL_FAILURE(InitState());

    VkMemoryPropertyFlags reqs = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    vk_testing::Buffer buffer;
    buffer.init_as_dst(*m_device, (VkDeviceSize)20, reqs);

    BeginCommandBuffer();
    m_commandBuffer->FillBuffer(buffer.handle(), 0, 4, 0x11111111);
    EndCommandBuffer();

    testFence.init(*m_device, fenceInfo);

    // Bypass framework since it does the waits automatically
    VkResult err = VK_SUCCESS;
    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = NULL;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = NULL;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_commandBuffer->handle();
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = NULL;

    err = vkQueueSubmit( m_device->m_queue, 1, &submit_info, testFence.handle());
    ASSERT_VK_SUCCESS( err );

    // Introduce failure by calling begin again before checking fence
    vkResetCommandBuffer(m_commandBuffer->handle(), 0);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Resetting CB (0xaddress) before it has completed. You must check CB flag before.'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CallBeginCommandBufferBeforeCompletion)
{
    vk_testing::Fence testFence;
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = NULL;
    fenceInfo.flags = 0;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT, "Calling vkBeginCommandBuffer() on active CB");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitViewport());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    BeginCommandBuffer();
    m_commandBuffer->ClearAllBuffers(m_clear_color, m_depth_clear_color, m_stencil_clear_color, NULL);
    EndCommandBuffer();

    testFence.init(*m_device, fenceInfo);

    // Bypass framework since it does the waits automatically
    VkResult err = VK_SUCCESS;
    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = NULL;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = NULL;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_commandBuffer->handle();
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = NULL;

    err = vkQueueSubmit( m_device->m_queue, 1, &submit_info, testFence.handle());
    ASSERT_VK_SUCCESS( err );


    VkCommandBufferBeginInfo info = {};
    info.flags       = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    info.sType       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.renderPass  = VK_NULL_HANDLE;
    info.subpass     = 0;
    info.framebuffer = VK_NULL_HANDLE;

    // Introduce failure by calling BCB again before checking fence
    vkBeginCommandBuffer(m_commandBuffer->handle(), &info);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Calling vkBeginCommandBuffer() on an active CB (0xaddress) before it has completed'";
        m_errorMonitor->DumpFailureMsgs();

    }
}

TEST_F(VkLayerTest, MapMemWithoutHostVisibleBit)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Mapping Memory without VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create an image, allocate memory, free it, and then try to bind it
    VkImage               image;
    VkDeviceMemory        mem;
    VkMemoryRequirements  mem_reqs;

    const VkFormat tex_format      = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t  tex_width       = 32;
    const int32_t  tex_height      = 32;

    VkImageCreateInfo image_create_info = {};
    image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = tex_format;
        image_create_info.extent.width = tex_width;
        image_create_info.extent.height = tex_height;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        image_create_info.flags = 0;

    VkMemoryAllocateInfo mem_alloc = {};
    mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        mem_alloc.pNext = NULL;
        mem_alloc.allocationSize = 0;
        // Introduce failure, do NOT set memProps to VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        mem_alloc.memoryTypeIndex = 1;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &image);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(),
                          image,
                          &mem_reqs);

    mem_alloc.allocationSize = mem_reqs.size;

    pass = m_device->phy().set_memory_type(mem_reqs.memoryTypeBits, &mem_alloc, 0, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if(!pass) { // If we can't find any unmappable memory this test doesn't make sense
        vkDestroyImage(m_device->device(), image, NULL);
        return;
    }

    // allocate memory
    err = vkAllocateMemory(m_device->device(), &mem_alloc, NULL, &mem);
    ASSERT_VK_SUCCESS(err);

    // Try to bind free memory that has been freed
    err = vkBindImageMemory(m_device->device(), image, mem, 0);
    ASSERT_VK_SUCCESS(err);

    // Map memory as if to initialize the image
    void *mappedAddress = NULL;
    err = vkMapMemory(m_device->device(), mem, 0, 0, 0, &mappedAddress);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Error received did not match expected error message from vkMapMemory in MemTracker'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), image, NULL);
}

// TODO : Is this test still valid. Not sure it is with updates to memory binding model
//  Verify and delete the test of fix the check
//TEST_F(VkLayerTest, FreeBoundMemory)
//{
//    VkResult        err;
//
//    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_WARN_BIT,
//        "Freeing memory object while it still has references");
//
//    ASSERT_NO_FATAL_FAILURE(InitState());

//    // Create an image, allocate memory, free it, and then try to bind it
//    VkImage               image;
//    VkDeviceMemory        mem;
//    VkMemoryRequirements  mem_reqs;
//
//    const VkFormat tex_format      = VK_FORMAT_B8G8R8A8_UNORM;
//    const int32_t  tex_width       = 32;
//    const int32_t  tex_height      = 32;
//
//    const VkImageCreateInfo image_create_info = {
//        .sType           = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
//        .pNext           = NULL,
//        .imageType       = VK_IMAGE_TYPE_2D,
//        .format          = tex_format,
//        .extent          = { tex_width, tex_height, 1 },
//        .mipLevels       = 1,
//        .arraySize       = 1,
//        .samples         = 1,
//        .tiling          = VK_IMAGE_TILING_LINEAR,
//        .usage           = VK_IMAGE_USAGE_SAMPLED_BIT,
//        .flags           = 0,
//    };
//    VkMemoryAllocateInfo mem_alloc = {
//        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
//        .pNext           = NULL,
//        .allocationSize  = 0,
//        .memoryTypeIndex = 0,
//    };
//
//    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &image);
//    ASSERT_VK_SUCCESS(err);
//
//    err = vkGetImageMemoryRequirements(m_device->device(),
//                          image,
//                          &mem_reqs);
//    ASSERT_VK_SUCCESS(err);
//
//    mem_alloc.allocationSize = mem_reqs.size;
//
//    err = m_device->phy().set_memory_type(mem_reqs.memoryTypeBits, &mem_alloc, 0);
//    ASSERT_VK_SUCCESS(err);
//
//    // allocate memory
//    err = vkAllocateMemory(m_device->device(), &mem_alloc, NULL, &mem);
//    ASSERT_VK_SUCCESS(err);
//
//    // Bind memory to Image object
//    err = vkBindImageMemory(m_device->device(), image, mem, 0);
//    ASSERT_VK_SUCCESS(err);
//
//    // Introduce validation failure, free memory while still bound to object
//    vkFreeMemory(m_device->device(), mem, NULL);
//
//    if (!m_errorMonitor->DesiredMsgFound()) {
//        FAIL() << "Did not receive Warning 'Freeing memory object while it still has references'");
//        m_errorMonitor->DumpFailureMsgs();
//    }
//}

TEST_F(VkLayerTest, RebindMemory)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "which has already been bound to mem object");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create an image, allocate memory, free it, and then try to bind it
    VkImage               image;
    VkDeviceMemory        mem1;
    VkDeviceMemory        mem2;
    VkMemoryRequirements  mem_reqs;

    const VkFormat tex_format      = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t  tex_width       = 32;
    const int32_t  tex_height      = 32;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = tex_format;
        image_create_info.extent.width = tex_width;
        image_create_info.extent.height = tex_height;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        image_create_info.flags = 0;

    VkMemoryAllocateInfo mem_alloc = {};
        mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        mem_alloc.pNext = NULL;
        mem_alloc.allocationSize = 0;
        mem_alloc.memoryTypeIndex = 0;

    // Introduce failure, do NOT set memProps to VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    mem_alloc.memoryTypeIndex = 1;
    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &image);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(),
                          image,
                          &mem_reqs);

    mem_alloc.allocationSize = mem_reqs.size;
    pass = m_device->phy().set_memory_type(mem_reqs.memoryTypeBits, &mem_alloc, 0);
    ASSERT_TRUE(pass);

    // allocate 2 memory objects
    err = vkAllocateMemory(m_device->device(), &mem_alloc, NULL, &mem1);
    ASSERT_VK_SUCCESS(err);
    err = vkAllocateMemory(m_device->device(), &mem_alloc, NULL, &mem2);
    ASSERT_VK_SUCCESS(err);

    // Bind first memory object to Image object
    err = vkBindImageMemory(m_device->device(), image, mem1, 0);
    ASSERT_VK_SUCCESS(err);

    // Introduce validation failure, try to bind a different memory object to the same image object
    err = vkBindImageMemory(m_device->device(), image, mem2, 0);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error when rebinding memory to an object";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), image, NULL);
    vkFreeMemory(m_device->device(), mem1, NULL);
    vkFreeMemory(m_device->device(), mem2, NULL);
}

TEST_F(VkLayerTest, SubmitSignaledFence)
{
    vk_testing::Fence testFence;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "submitted in SIGNALED state.  Fences must be reset before being submitted");

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = NULL;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitViewport());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    BeginCommandBuffer();
    m_commandBuffer->ClearAllBuffers(m_clear_color, m_depth_clear_color, m_stencil_clear_color, NULL);
    EndCommandBuffer();

    testFence.init(*m_device, fenceInfo);

    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = NULL;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = NULL;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_commandBuffer->handle();
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = NULL;

    vkQueueSubmit(m_device->m_queue, 1, &submit_info, testFence.handle());
    vkQueueWaitIdle(m_device->m_queue );

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'VkQueueSubmit with fence in SIGNALED_STATE'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, ResetUnsignaledFence)
{
    vk_testing::Fence testFence;
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = NULL;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_WARN_BIT,
        "submitted to VkResetFences in UNSIGNALED STATE");

    ASSERT_NO_FATAL_FAILURE(InitState());
    testFence.init(*m_device, fenceInfo);
    VkFence fences[1] = {testFence.handle()};
    vkResetFences(m_device->device(), 1, fences);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'VkResetFences with fence in UNSIGNALED_STATE'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

/* TODO: Update for changes due to bug-14075 tiling across render passes */
#if 0
TEST_F(VkLayerTest, InvalidUsageBits)
{
    // Initiate Draw w/o a PSO bound

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Invalid usage flag for image ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    VkCommandBufferObj commandBuffer(m_device);
    BeginCommandBuffer();

    const VkExtent3D e3d = {
        .width = 128,
        .height = 128,
        .depth = 1,
    };
    const VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = NULL,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT_S8_UINT,
        .extent = e3d,
        .mipLevels = 1,
        .arraySize = 1,
        .samples = 1,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = 0, // Not setting VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        .flags = 0,
    };

    VkImage dsi;
    vkCreateImage(m_device->device(), &ici, NULL, &dsi);
    VkDepthStencilView dsv;
    const VkDepthStencilViewCreateInfo dsvci = {
        .sType = VK_STRUCTURE_TYPE_DEPTH_STENCIL_VIEW_CREATE_INFO,
        .pNext = NULL,
        .image = dsi,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .arraySize = 1,
        .flags = 0,
    };
    vkCreateDepthStencilView(m_device->device(), &dsvci, NULL, &dsv);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Error received was not 'Invalid usage flag for image...'";
        m_errorMonitor->DumpFailureMsgs();
    }
}
#endif // 0
#endif // MEM_TRACKER_TESTS

#if OBJ_TRACKER_TESTS
TEST_F(VkLayerTest, PipelineNotBound)
{
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Invalid VkPipeline Object ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.pNext = NULL;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkPipeline badPipeline = (VkPipeline)0xbaadb1be;

    BeginCommandBuffer();
    vkCmdBindPipeline(m_commandBuffer->GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, badPipeline);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Error received was not 'Invalid VkPipeline Object 0xbaadb1be'" << endl;
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, BindInvalidMemory)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Invalid VkDeviceMemory Object ");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create an image, allocate memory, free it, and then try to bind it
    VkImage               image;
    VkDeviceMemory        mem;
    VkMemoryRequirements  mem_reqs;

    const VkFormat tex_format      = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t  tex_width       = 32;
    const int32_t  tex_height      = 32;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = tex_format;
        image_create_info.extent.width = tex_width;
        image_create_info.extent.height = tex_height;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        image_create_info.flags = 0;

    VkMemoryAllocateInfo mem_alloc = {};
        mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        mem_alloc.pNext = NULL;
        mem_alloc.allocationSize = 0;
        mem_alloc.memoryTypeIndex = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &image);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(),
                          image,
                          &mem_reqs);

    mem_alloc.allocationSize = mem_reqs.size;

    pass = m_device->phy().set_memory_type(mem_reqs.memoryTypeBits, &mem_alloc, 0);
    ASSERT_TRUE(pass);

    // allocate memory
    err = vkAllocateMemory(m_device->device(), &mem_alloc, NULL, &mem);
    ASSERT_VK_SUCCESS(err);

    // Introduce validation failure, free memory before binding
    vkFreeMemory(m_device->device(), mem, NULL);

    // Try to bind free memory that has been freed
    err = vkBindImageMemory(m_device->device(), image, mem, 0);
    // This may very well return an error.
    (void)err;

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Invalid VkDeviceMemory Object 0x<handle>'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), image, NULL);
}

TEST_F(VkLayerTest, BindMemoryToDestroyedObject)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT, "Invalid VkImage Object ");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create an image object, allocate memory, destroy the object and then try to bind it
    VkImage               image;
    VkDeviceMemory        mem;
    VkMemoryRequirements  mem_reqs;

    const VkFormat tex_format      = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t  tex_width       = 32;
    const int32_t  tex_height      = 32;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = tex_format;
        image_create_info.extent.width = tex_width;
        image_create_info.extent.height = tex_height;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        image_create_info.flags = 0;

    VkMemoryAllocateInfo mem_alloc = {};
        mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        mem_alloc.pNext = NULL;
        mem_alloc.allocationSize = 0;
        mem_alloc.memoryTypeIndex = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &image);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(),
                          image,
                          &mem_reqs);

    mem_alloc.allocationSize = mem_reqs.size;
    pass = m_device->phy().set_memory_type(mem_reqs.memoryTypeBits, &mem_alloc, 0);
    ASSERT_TRUE(pass);

    // Allocate memory
    err = vkAllocateMemory(m_device->device(), &mem_alloc, NULL, &mem);
    ASSERT_VK_SUCCESS(err);

    // Introduce validation failure, destroy Image object before binding
    vkDestroyImage(m_device->device(), image, NULL);
    ASSERT_VK_SUCCESS(err);

    // Now Try to bind memory to this destroyed object
    err = vkBindImageMemory(m_device->device(), image, mem, 0);
    // This may very well return an error.
    (void) err;

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Invalid VkImage Object 0x<handle>'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkFreeMemory(m_device->device(), mem, NULL);
}

TEST_F(VkLayerTest, InvalidBufferViewObject)
{
    // Create a single TEXEL_BUFFER descriptor and send it an invalid bufferView
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Invalid VkBufferView Object 0xbaadbeef");

    ASSERT_NO_FATAL_FAILURE(InitState());
    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;
    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkBufferView view = (VkBufferView) 0xbaadbeef; // invalid bufferView object

    VkWriteDescriptorSet descriptor_write;
    memset(&descriptor_write, 0, sizeof(descriptor_write));
    descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.dstBinding = 0;
    descriptor_write.descriptorCount = 1;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    descriptor_write.pTexelBufferView = &view;

    vkUpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did nto receive Error 'Invalid VkBufferView Object 0xbaadbeef'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}
#endif // OBJ_TRACKER_TESTS

#if DRAW_STATE_TESTS
TEST_F(VkLayerTest, LineWidthStateNotBound)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic line width state not set for this command buffer");

    TEST_DESCRIPTION("Simple Draw Call that validates failure when a line width state object is not bound beforehand");

    VKTriangleTest(bindStateVertShaderText, bindStateFragShaderText, BsoFailLineWidth);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Dynamic line width state not set for this command buffer'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, DepthBiasStateNotBound)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic depth bias state not set for this command buffer");

    TEST_DESCRIPTION("Simple Draw Call that validates failure when a depth bias state object is not bound beforehand");

    VKTriangleTest(bindStateVertShaderText, bindStateFragShaderText, BsoFailDepthBias);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Dynamic depth bias state not set for this command buffer'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

// Disable these two tests until we can sort out how to track multiple layer errors
TEST_F(VkLayerTest, ViewportStateNotBound)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic viewport state not set for this command buffer");

    TEST_DESCRIPTION("Simple Draw Call that validates failure when a viewport state object is not bound beforehand");

    VKTriangleTest(bindStateVertShaderText, bindStateFragShaderText, BsoFailViewport);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieve Error 'Dynamic scissor state not set for this command buffer'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, ScissorStateNotBound)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic scissor state not set for this command buffer");

    TEST_DESCRIPTION("Simple Draw Call that validates failure when a viewport state object is not bound beforehand");

    VKTriangleTest(bindStateVertShaderText, bindStateFragShaderText, BsoFailScissor);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieve Error ' Expected: 'Dynamic scissor state not set for this command buffer'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, BlendStateNotBound)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic blend object state not set for this command buffer");

    TEST_DESCRIPTION("Simple Draw Call that validates failure when a blend state object is not bound beforehand");

    VKTriangleTest(bindStateVertShaderText, bindStateFragShaderText, BsoFailBlend);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieve Error 'Dynamic blend object state not set for this command buffer'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, DepthBoundsStateNotBound)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic depth bounds state not set for this command buffer");

    TEST_DESCRIPTION("Simple Draw Call that validates failure when a depth bounds state object is not bound beforehand");

    VKTriangleTest(bindStateVertShaderText, bindStateFragShaderText, BsoFailDepthBounds);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Dynamic depth bounds state not set for this command buffer'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, StencilReadMaskNotSet)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic stencil read mask state not set for this command buffer");

    ASSERT_NO_FATAL_FAILURE(InitState());

    TEST_DESCRIPTION("Simple Draw Call that validates failure when a stencil read mask is not set beforehand");

    VKTriangleTest(bindStateVertShaderText, bindStateFragShaderText, BsoFailStencilReadMask);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Dynamic stencil read mask state not set for this command buffer'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, StencilWriteMaskNotSet)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic stencil write mask state not set for this command buffer");

    ASSERT_NO_FATAL_FAILURE(InitState());

    TEST_DESCRIPTION("Simple Draw Call that validates failure when a stencil write mask is not set beforehand");

    VKTriangleTest(bindStateVertShaderText, bindStateFragShaderText, BsoFailStencilWriteMask);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Dynamic stencil write mask state not set for this command buffer'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, StencilReferenceNotSet)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic stencil reference state not set for this command buffer");

    TEST_DESCRIPTION("Simple Draw Call that validates failure when a stencil reference is not set beforehand");

    VKTriangleTest(bindStateVertShaderText, bindStateFragShaderText, BsoFailStencilReference);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Dynamic stencil reference state not set for this command buffer'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CommandBufferTwoSubmits)
{
    vk_testing::Fence testFence;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "was begun w/ VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT set, but has been submitted");

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = NULL;
    fenceInfo.flags = 0;

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitViewport());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    // We luck out b/c by default the framework creates CB w/ the VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT set
    BeginCommandBuffer();
    m_commandBuffer->ClearAllBuffers(m_clear_color, m_depth_clear_color, m_stencil_clear_color, NULL);
    EndCommandBuffer();

    testFence.init(*m_device, fenceInfo);

    // Bypass framework since it does the waits automatically
    VkResult err = VK_SUCCESS;
    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = NULL;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = NULL;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_commandBuffer->handle();
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = NULL;

    err = vkQueueSubmit( m_device->m_queue, 1, &submit_info, testFence.handle());
    ASSERT_VK_SUCCESS( err );

    // Cause validation error by re-submitting cmd buffer that should only be submitted once
    err = vkQueueSubmit( m_device->m_queue, 1, &submit_info, testFence.handle());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'CB (0xaddress) was created w/ VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT set...'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, BindPipelineNoRenderPass)
{
    // Initiate Draw w/o a PSO bound
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Incorrectly binding graphics pipeline ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);
    VkPipelineMultisampleStateCreateInfo pipe_ms_state_ci = {};
        pipe_ms_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        pipe_ms_state_ci.pNext = NULL;
        pipe_ms_state_ci.rasterizationSamples = 1;
        pipe_ms_state_ci.sampleShadingEnable = 0;
        pipe_ms_state_ci.minSampleShading = 1.0;
        pipe_ms_state_ci.pSampleMask = NULL;

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.pNext = NULL;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;
        VkPipelineLayout pipeline_layout;

    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkShaderObj vs(m_device, bindStateVertShaderText, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, bindStateFragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this); //  TODO - We shouldn't need a fragment shader
                                                                                       // but add it to be able to run on more devices
    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);
    pipe.SetMSAA(&pipe_ms_state_ci);
    pipe.CreateVKPipeline(pipeline_layout, renderPass());

    // Calls AllocateCommandBuffers
    VkCommandBufferObj commandBuffer(m_device, m_commandPool);
    VkCommandBufferBeginInfo cmd_buf_info = {};
    memset(&cmd_buf_info, 0, sizeof(VkCommandBufferBeginInfo));
    cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_buf_info.pNext = NULL;
    cmd_buf_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer.GetBufferHandle(), &cmd_buf_info);
    vkCmdBindPipeline(commandBuffer.GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.handle());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Incorrectly binding graphics pipeline (0x<handle>) without an active RenderPass'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, AllocDescriptorFromEmptyPool)
{
    // Initiate Draw w/o a PSO bound
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Unable to allocate 1 descriptors of type VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    // Create Pool w/ 1 Sampler descriptor, but try to alloc Uniform Buffer descriptor from it
    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.flags = 0;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Unable to allocate 1 descriptors of type VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, FreeDescriptorFromOneShotPool)
{
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "It is invalid to call vkFreeDescriptorSets() with a pool created without setting VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT.");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.flags = 0;
        // Not specifying VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT means
        // app can only call vkResetDescriptorPool on this pool.;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    err = vkFreeDescriptorSets(m_device->device(), ds_pool, 1, &descriptorSet);
    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'It is invalid to call vkFreeDescriptorSets() with a pool created with...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, InvalidDescriptorPool)
{
    // TODO : Simple check for bad object should be added to ObjectTracker to catch this case
    //   The DS check for this is after driver has been called to validate DS internal data struct
    // Attempt to clear DS Pool with bad object
/*
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Unable to find pool node for pool 0xbaad6001 specified in vkResetDescriptorPool() call");

    VkDescriptorPool badPool = (VkDescriptorPool)0xbaad6001;
    vkResetDescriptorPool(device(), badPool);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Unable to find pool node for pool 0xbaad6001 specified in vkResetDescriptorPool() call'";
        m_errorMonitor->DumpFailureMsgs();
    }*/
}

TEST_F(VkLayerTest, InvalidDescriptorSet)
{
    // TODO : Simple check for bad object should be added to ObjectTracker to catch this case
    //   The DS check for this is after driver has been called to validate DS internal data struct
    // Create a valid cmd buffer
    // call vkCmdBindDescriptorSets w/ false DS
}

TEST_F(VkLayerTest, InvalidDescriptorSetLayout)
{
    // TODO : Simple check for bad object should be added to ObjectTracker to catch this case
    //   The DS check for this is after driver has been called to validate DS internal data struct
}

TEST_F(VkLayerTest, InvalidPipeline)
{
    // TODO : Simple check for bad object should be added to ObjectTracker to catch this case
    //   The DS check for this is after driver has been called to validate DS internal data struct
    // Create a valid cmd buffer
    // call vkCmdBindPipeline w/ false Pipeline
//
//    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
//        "Attempt to bind Pipeline ");
//
//    ASSERT_NO_FATAL_FAILURE(InitState());
//    VkCommandBufferObj commandBuffer(m_device);
//    BeginCommandBuffer();
//    VkPipeline badPipeline = (VkPipeline)0xbaadb1be;
//    vkCmdBindPipeline(commandBuffer.GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, badPipeline);
//
//    if (!m_errorMonitor->DesiredMsgFound()) {
//        FAIL() << "Did not receive Error 'Attempt to bind Pipeline 0xbaadb1be that doesn't exist!'";
//        m_errorMonitor->DumpFailureMsgs();
//    }
}

TEST_F(VkLayerTest, DescriptorSetNotUpdated)
{
    // Create and update CommandBuffer then call QueueSubmit w/o calling End on CommandBuffer
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_WARN_BIT,
        " bound but it was never updated. ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitViewport());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());
    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;
    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.pNext = NULL;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkShaderObj vs(m_device, bindStateVertShaderText, VK_SHADER_STAGE_VERTEX_BIT, this);
    //  TODO - We shouldn't need a fragment shader but add it to be able to run on more devices
    VkShaderObj fs(m_device, bindStateFragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);
    pipe.CreateVKPipeline(pipeline_layout, renderPass());

    BeginCommandBuffer();
    vkCmdBindPipeline(m_commandBuffer->GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.handle());
    vkCmdBindDescriptorSets(m_commandBuffer->GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &descriptorSet, 0, NULL);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieve Warning 'DS <blah> bound but it was never updated. You may want to either update it or not bind it.'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, NoBeginCommandBuffer)
{

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "You must call vkBeginCommandBuffer() before this call to ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    VkCommandBufferObj commandBuffer(m_device, m_commandPool);
    // Call EndCommandBuffer() w/o calling BeginCommandBuffer()
    vkEndCommandBuffer(commandBuffer.GetBufferHandle());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieve Error 'You must call vkBeginCommandBuffer() before this call to vkEndCommandBuffer()'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, PrimaryCommandBufferFramebufferAndRenderpass)
{

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "may not specify framebuffer or renderpass parameters");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Calls AllocateCommandBuffers
    VkCommandBufferObj commandBuffer(m_device, m_commandPool);

    // Force the failure by setting the Renderpass and Framebuffer fields with (fake) data
    VkCommandBufferBeginInfo cmd_buf_info = {};
    cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_buf_info.pNext = NULL;
    cmd_buf_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    cmd_buf_info.renderPass = (VkRenderPass)0xcadecade;
    cmd_buf_info.framebuffer = (VkFramebuffer)0xcadecade;


    // The error should be caught by validation of the BeginCommandBuffer call
    vkBeginCommandBuffer(commandBuffer.GetBufferHandle(), &cmd_buf_info);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkAllocateCommandBuffers():  Primary Command Buffer may not specify framebuffer or renderpass parameters'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, SecondaryCommandBufferFramebufferAndRenderpass)
{
    VkResult        err;
    VkCommandBuffer     draw_cmd;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "must specify framebuffer and renderpass parameters");

    ASSERT_NO_FATAL_FAILURE(InitState());

    VkCommandBufferAllocateInfo cmd = {};
    cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOC_INFO;
    cmd.pNext = NULL;
    cmd.commandPool = m_commandPool;
    cmd.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    cmd.bufferCount = 1;

    err = vkAllocateCommandBuffers(m_device->device(), &cmd, &draw_cmd);
    ASSERT_VK_SUCCESS(err);

    // Force the failure by not setting the Renderpass and Framebuffer fields
    VkCommandBufferBeginInfo cmd_buf_info = {};
    cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_buf_info.pNext = NULL;
    cmd_buf_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    // The error should be caught by validation of the BeginCommandBuffer call
    vkBeginCommandBuffer(draw_cmd, &cmd_buf_info);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkAllocateCommandBuffers():  Secondary Command Buffer must specify framebuffer and renderpass parameters'";
        m_errorMonitor->DumpFailureMsgs();
    }
    vkFreeCommandBuffers(m_device->device(), m_commandPool, 1, &draw_cmd);
}

TEST_F(VkLayerTest, InvalidPipelineCreateState)
{
    // Attempt to Create Gfx Pipeline w/o a VS
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Invalid Pipeline CreateInfo State: Vtx Shader required");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkViewport vp = {}; // Just need dummy vp to point to
    VkRect2D sc = {}; // dummy scissor to point to

    VkPipelineViewportStateCreateInfo vp_state_ci = {};
        vp_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp_state_ci.scissorCount = 1;
        vp_state_ci.pScissors = &sc;
        vp_state_ci.viewportCount = 1;
        vp_state_ci.pViewports = &vp;

    VkGraphicsPipelineCreateInfo gp_ci = {};
        gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp_ci.pViewportState = &vp_state_ci;
        gp_ci.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        gp_ci.layout = pipeline_layout;
        gp_ci.renderPass = renderPass();

    VkPipelineCacheCreateInfo pc_ci = {};
        pc_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        pc_ci.initialDataSize = 0;
        pc_ci.pInitialData = 0;

    VkPipeline pipeline;
    VkPipelineCache pipelineCache;

    err = vkCreatePipelineCache(m_device->device(), &pc_ci, NULL, &pipelineCache);
    ASSERT_VK_SUCCESS(err);
    err = vkCreateGraphicsPipelines(m_device->device(), pipelineCache, 1, &gp_ci, NULL, &pipeline);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Invalid Pipeline CreateInfo State: Vtx Shader required'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineCache(m_device->device(), pipelineCache, NULL);
    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}
/*// TODO : This test should be good, but needs Tess support in compiler to run
TEST_F(VkLayerTest, InvalidPatchControlPoints)
{
    // Attempt to Create Gfx Pipeline w/o a VS
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Invalid Pipeline CreateInfo State: VK_PRIMITIVE_TOPOLOGY_PATCH primitive ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), VK_DESCRIPTOR_POOL_USAGE_NON_FREE, 1, &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    err = vkAllocateDescriptorSets(m_device->device(), ds_pool, VK_DESCRIPTOR_SET_USAGE_NON_FREE, 1, &ds_layout, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.pNext = NULL;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkPipelineShaderStageCreateInfo shaderStages[3];
    memset(&shaderStages, 0, 3 * sizeof(VkPipelineShaderStageCreateInfo));

    VkShaderObj vs(m_device,bindStateVertShaderText,VK_SHADER_STAGE_VERTEX_BIT, this);
    // Just using VS txt for Tess shaders as we don't care about functionality
    VkShaderObj tc(m_device,bindStateVertShaderText,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, this);
    VkShaderObj te(m_device,bindStateVertShaderText,VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, this);

    shaderStages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].shader = vs.handle();
    shaderStages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    shaderStages[1].shader = tc.handle();
    shaderStages[2].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[2].stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    shaderStages[2].shader = te.handle();

    VkPipelineInputAssemblyStateCreateInfo iaCI = {};
        iaCI.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        iaCI.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;

    VkPipelineTessellationStateCreateInfo tsCI = {};
        tsCI.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
        tsCI.patchControlPoints = 0; // This will cause an error

    VkGraphicsPipelineCreateInfo gp_ci = {};
        gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp_ci.pNext = NULL;
        gp_ci.stageCount = 3;
        gp_ci.pStages = shaderStages;
        gp_ci.pVertexInputState = NULL;
        gp_ci.pInputAssemblyState = &iaCI;
        gp_ci.pTessellationState = &tsCI;
        gp_ci.pViewportState = NULL;
        gp_ci.pRasterizationState = NULL;
        gp_ci.pMultisampleState = NULL;
        gp_ci.pDepthStencilState = NULL;
        gp_ci.pColorBlendState = NULL;
        gp_ci.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        gp_ci.layout = pipeline_layout;
        gp_ci.renderPass = renderPass();

    VkPipelineCacheCreateInfo pc_ci = {};
        pc_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        pc_ci.pNext = NULL;
        pc_ci.initialSize = 0;
        pc_ci.initialData = 0;
        pc_ci.maxSize = 0;

    VkPipeline pipeline;
    VkPipelineCache pipelineCache;

    err = vkCreatePipelineCache(m_device->device(), &pc_ci, NULL, &pipelineCache);
    ASSERT_VK_SUCCESS(err);
    err = vkCreateGraphicsPipelines(m_device->device(), pipelineCache, 1, &gp_ci, NULL, &pipeline);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Invalid Pipeline CreateInfo State: VK_PRIMITIVE_TOPOLOGY_PATCH primitive...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineCache(m_device->device(), pipelineCache, NULL);
    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}
*/
// Set scissor and viewport counts to different numbers
TEST_F(VkLayerTest, PSOViewportScissorCountMismatch)
{
    // Attempt to Create Gfx Pipeline w/o a VS
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Gfx Pipeline viewport count (1) must match scissor count (0).");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkViewport vp = {}; // Just need dummy vp to point to

    VkPipelineViewportStateCreateInfo vp_state_ci = {};
        vp_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp_state_ci.scissorCount = 0;
        vp_state_ci.viewportCount = 1; // Count mismatch should cause error
        vp_state_ci.pViewports = &vp;

    VkPipelineShaderStageCreateInfo shaderStages[2];
	memset(&shaderStages, 0, 2 * sizeof(VkPipelineShaderStageCreateInfo));

    VkShaderObj vs(m_device,bindStateVertShaderText,VK_SHADER_STAGE_VERTEX_BIT, this);
	VkShaderObj fs(m_device, bindStateFragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this); // TODO - We shouldn't need a fragment shader
	                                                                                   // but add it to be able to run on more devices
    shaderStages[0] = vs.GetStageCreateInfo();
    shaderStages[1] = fs.GetStageCreateInfo();

    VkGraphicsPipelineCreateInfo gp_ci = {};
        gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp_ci.stageCount = 2;
        gp_ci.pStages = shaderStages;
        gp_ci.pViewportState = &vp_state_ci;
        gp_ci.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        gp_ci.layout = pipeline_layout;
        gp_ci.renderPass = renderPass();

    VkPipelineCacheCreateInfo pc_ci = {};
        pc_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    VkPipeline pipeline;
    VkPipelineCache pipelineCache;

    err = vkCreatePipelineCache(m_device->device(), &pc_ci, NULL, &pipelineCache);
    ASSERT_VK_SUCCESS(err);
    err = vkCreateGraphicsPipelines(m_device->device(), pipelineCache, 1, &gp_ci, NULL, &pipeline);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Gfx Pipeline viewport count (1) must match scissor count (0).'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineCache(m_device->device(), pipelineCache, NULL);
    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}
// Don't set viewport state in PSO. This is an error b/c we always need this state
//  for the counts even if the data is going to be set dynamically.
TEST_F(VkLayerTest, PSOViewportStateNotSet)
{
    // Attempt to Create Gfx Pipeline w/o a VS
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Gfx Pipeline pViewportState is null. Even if ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkDynamicState sc_state = VK_DYNAMIC_STATE_SCISSOR;
    // Set scissor as dynamic to avoid second error
    VkPipelineDynamicStateCreateInfo dyn_state_ci = {};
        dyn_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn_state_ci.dynamicStateCount = 1;
        dyn_state_ci.pDynamicStates = &sc_state;

    VkPipelineShaderStageCreateInfo shaderStages[2];
	memset(&shaderStages, 0, 2 * sizeof(VkPipelineShaderStageCreateInfo));

    VkShaderObj vs(m_device,bindStateVertShaderText,VK_SHADER_STAGE_VERTEX_BIT, this);
	VkShaderObj fs(m_device, bindStateFragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this); // TODO - We shouldn't need a fragment shader
	                                                                                   // but add it to be able to run on more devices
    shaderStages[0] = vs.GetStageCreateInfo();
    shaderStages[1] = fs.GetStageCreateInfo();

    VkGraphicsPipelineCreateInfo gp_ci = {};
        gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp_ci.stageCount = 2;
        gp_ci.pStages = shaderStages;
        gp_ci.pViewportState = NULL; // Not setting VP state w/o dynamic vp state should cause validation error
        gp_ci.pDynamicState = &dyn_state_ci;
        gp_ci.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        gp_ci.layout = pipeline_layout;
        gp_ci.renderPass = renderPass();

    VkPipelineCacheCreateInfo pc_ci = {};
        pc_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    VkPipeline pipeline;
    VkPipelineCache pipelineCache;

    err = vkCreatePipelineCache(m_device->device(), &pc_ci, NULL, &pipelineCache);
    ASSERT_VK_SUCCESS(err);
    err = vkCreateGraphicsPipelines(m_device->device(), pipelineCache, 1, &gp_ci, NULL, &pipeline);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Gfx Pipeline pViewportState is null. Even if...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineCache(m_device->device(), pipelineCache, NULL);
    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}
// Create PSO w/o non-zero viewportCount but no viewport data
// Then run second test where dynamic scissor count doesn't match PSO scissor count
TEST_F(VkLayerTest, PSOViewportCountWithoutDataAndDynScissorMismatch)
{
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Gfx Pipeline viewportCount is 1, but pViewports is NULL. ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkPipelineViewportStateCreateInfo vp_state_ci = {};
        vp_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp_state_ci.viewportCount = 1;
        vp_state_ci.pViewports = NULL; // Null vp w/ count of 1 should cause error
        vp_state_ci.scissorCount = 1;
        vp_state_ci.pScissors = NULL; // Scissor is dynamic (below) so this won't cause error

    VkDynamicState sc_state = VK_DYNAMIC_STATE_SCISSOR;
    // Set scissor as dynamic to avoid that error
    VkPipelineDynamicStateCreateInfo dyn_state_ci = {};
        dyn_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn_state_ci.dynamicStateCount = 1;
        dyn_state_ci.pDynamicStates = &sc_state;

    VkPipelineShaderStageCreateInfo shaderStages[2];
	memset(&shaderStages, 0, 2 * sizeof(VkPipelineShaderStageCreateInfo));

    VkShaderObj vs(m_device,bindStateVertShaderText,VK_SHADER_STAGE_VERTEX_BIT, this);
	VkShaderObj fs(m_device, bindStateFragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this); // TODO - We shouldn't need a fragment shader
	                                                                                   // but add it to be able to run on more devices
    shaderStages[0] = vs.GetStageCreateInfo();
    shaderStages[1] = fs.GetStageCreateInfo();

	VkPipelineVertexInputStateCreateInfo vi_ci = {};
	vi_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi_ci.pNext = nullptr;
	vi_ci.vertexBindingDescriptionCount = 0;
	vi_ci.pVertexBindingDescriptions = nullptr;
	vi_ci.vertexAttributeDescriptionCount = 0;
	vi_ci.pVertexAttributeDescriptions = nullptr;

	VkPipelineInputAssemblyStateCreateInfo ia_ci = {};
	ia_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia_ci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

	VkPipelineRasterizationStateCreateInfo rs_ci = {};
	rs_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs_ci.pNext = nullptr;

	VkPipelineColorBlendStateCreateInfo cb_ci = {};
	cb_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb_ci.pNext = nullptr;

    VkGraphicsPipelineCreateInfo gp_ci = {};
        gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp_ci.stageCount = 2;
        gp_ci.pStages = shaderStages;
        gp_ci.pVertexInputState = &vi_ci;
        gp_ci.pInputAssemblyState = &ia_ci;
        gp_ci.pViewportState = &vp_state_ci;
        gp_ci.pRasterizationState = &rs_ci;
        gp_ci.pColorBlendState = &cb_ci;
        gp_ci.pDynamicState = &dyn_state_ci;
        gp_ci.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        gp_ci.layout = pipeline_layout;
        gp_ci.renderPass = renderPass();

    VkPipelineCacheCreateInfo pc_ci = {};
        pc_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    VkPipeline pipeline;
    VkPipelineCache pipelineCache;

    err = vkCreatePipelineCache(m_device->device(), &pc_ci, NULL, &pipelineCache);
    ASSERT_VK_SUCCESS(err);
    err = vkCreateGraphicsPipelines(m_device->device(), pipelineCache, 1, &gp_ci, NULL, &pipeline);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieve Error 'Gfx Pipeline viewportCount is 1, but pViewports is NULL...'";
        m_errorMonitor->DumpFailureMsgs();
    }


    // Now hit second fail case where we set scissor w/ different count than PSO
    // First need to successfully create the PSO from above by setting pViewports
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic scissorCount from vkCmdSetScissor() is 2, but PSO scissorCount is 1. These counts must match.");

    VkViewport vp = {}; // Just need dummy vp to point to
    vp_state_ci.pViewports = &vp;
    err = vkCreateGraphicsPipelines(m_device->device(), pipelineCache, 1, &gp_ci, NULL, &pipeline);
    ASSERT_VK_SUCCESS(err);
    BeginCommandBuffer();
    vkCmdBindPipeline(m_commandBuffer->GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkRect2D scissors[2] = {}; // don't care about data
    // Count of 2 doesn't match PSO count of 1
    vkCmdSetScissor(m_commandBuffer->GetBufferHandle(), 2, scissors);
    Draw(1, 0, 0, 0);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Dynamic scissorCount from vkCmdSetScissor() is 2, but PSO scissorCount is 1...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineCache(m_device->device(), pipelineCache, NULL);
    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}
// Create PSO w/o non-zero scissorCount but no scissor data
// Then run second test where dynamic viewportCount doesn't match PSO viewportCount
TEST_F(VkLayerTest, PSOScissorCountWithoutDataAndDynViewportMismatch)
{
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Gfx Pipeline scissorCount is 1, but pScissors is NULL. ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkPipelineViewportStateCreateInfo vp_state_ci = {};
        vp_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp_state_ci.scissorCount = 1;
        vp_state_ci.pScissors = NULL; // Null scissor w/ count of 1 should cause error
        vp_state_ci.viewportCount = 1;
        vp_state_ci.pViewports = NULL; // vp is dynamic (below) so this won't cause error

    VkDynamicState vp_state = VK_DYNAMIC_STATE_VIEWPORT;
    // Set scissor as dynamic to avoid that error
    VkPipelineDynamicStateCreateInfo dyn_state_ci = {};
        dyn_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn_state_ci.dynamicStateCount = 1;
        dyn_state_ci.pDynamicStates = &vp_state;

    VkPipelineShaderStageCreateInfo shaderStages[2];
	memset(&shaderStages, 0, 2 * sizeof(VkPipelineShaderStageCreateInfo));

    VkShaderObj vs(m_device,bindStateVertShaderText,VK_SHADER_STAGE_VERTEX_BIT, this);
	VkShaderObj fs(m_device, bindStateFragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this); // TODO - We shouldn't need a fragment shader
	                                                                                   // but add it to be able to run on more devices
    shaderStages[0] = vs.GetStageCreateInfo();
    shaderStages[1] = fs.GetStageCreateInfo();

    VkPipelineVertexInputStateCreateInfo vi_ci = {};
    vi_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi_ci.pNext = nullptr;
    vi_ci.vertexBindingDescriptionCount = 0;
    vi_ci.pVertexBindingDescriptions = nullptr;
    vi_ci.vertexAttributeDescriptionCount = 0;
    vi_ci.pVertexAttributeDescriptions = nullptr;

    VkPipelineInputAssemblyStateCreateInfo ia_ci = {};
    ia_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia_ci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineRasterizationStateCreateInfo rs_ci = {};
    rs_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs_ci.pNext = nullptr;

    VkPipelineColorBlendStateCreateInfo cb_ci = {};
    cb_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb_ci.pNext = nullptr;

    VkGraphicsPipelineCreateInfo gp_ci = {};
        gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp_ci.stageCount = 2;
        gp_ci.pStages = shaderStages;
        gp_ci.pVertexInputState = &vi_ci;
        gp_ci.pInputAssemblyState = &ia_ci;
        gp_ci.pViewportState = &vp_state_ci;
        gp_ci.pRasterizationState = &rs_ci;
        gp_ci.pColorBlendState = &cb_ci;
        gp_ci.pDynamicState = &dyn_state_ci;
        gp_ci.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        gp_ci.layout = pipeline_layout;
        gp_ci.renderPass = renderPass();

    VkPipelineCacheCreateInfo pc_ci = {};
        pc_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    VkPipeline pipeline;
    VkPipelineCache pipelineCache;

    err = vkCreatePipelineCache(m_device->device(), &pc_ci, NULL, &pipelineCache);
    ASSERT_VK_SUCCESS(err);
    err = vkCreateGraphicsPipelines(m_device->device(), pipelineCache, 1, &gp_ci, NULL, &pipeline);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieve Error 'Gfx Pipeline scissorCount is 1, but pScissors is NULL...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    // Now hit second fail case where we set scissor w/ different count than PSO
    // First need to successfully create the PSO from above by setting pViewports
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Dynamic viewportCount from vkCmdSetViewport() is 2, but PSO viewportCount is 1. These counts must match.");

    VkRect2D sc = {}; // Just need dummy vp to point to
    vp_state_ci.pScissors = &sc;
    err = vkCreateGraphicsPipelines(m_device->device(), pipelineCache, 1, &gp_ci, NULL, &pipeline);
    ASSERT_VK_SUCCESS(err);
    BeginCommandBuffer();
    vkCmdBindPipeline(m_commandBuffer->GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewports[2] = {}; // don't care about data
    // Count of 2 doesn't match PSO count of 1
    vkCmdSetViewport(m_commandBuffer->GetBufferHandle(), 2, viewports);
    Draw(1, 0, 0, 0);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Dynamic viewportCount from vkCmdSetViewport() is 2, but PSO viewportCount is 1...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineCache(m_device->device(), pipelineCache, NULL);
    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, NullRenderPass)
{
    // Bind a NULL RenderPass
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "You cannot use a NULL RenderPass object in vkCmdBeginRenderPass()");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    BeginCommandBuffer();
    // Don't care about RenderPass handle b/c error should be flagged before that
    vkCmdBeginRenderPass(m_commandBuffer->GetBufferHandle(), NULL, VK_SUBPASS_CONTENTS_INLINE);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'You cannot use a NULL RenderPass object in vkCmdBeginRenderPass()'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, RenderPassWithinRenderPass)
{
    // Bind a BeginRenderPass within an active RenderPass
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "It is invalid to issue this call inside an active render pass");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    BeginCommandBuffer();
    // Just create a dummy Renderpass that's non-NULL so we can get to the proper error
    VkRenderPassBeginInfo rp_begin = {};
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.pNext = NULL;
        rp_begin.renderPass = renderPass();
        rp_begin.framebuffer = framebuffer();

    vkCmdBeginRenderPass(m_commandBuffer->GetBufferHandle(), &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'It is invalid to issue this call inside an active render pass...'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, FillBufferWithinRenderPass)
{
    // Call CmdFillBuffer within an active renderpass
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "It is invalid to issue this call inside an active render pass");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    // Renderpass is started here
    BeginCommandBuffer();

    VkMemoryPropertyFlags reqs = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    vk_testing::Buffer dstBuffer;
    dstBuffer.init_as_dst(*m_device, (VkDeviceSize)1024, reqs);

    m_commandBuffer->FillBuffer(dstBuffer.handle(), 0, 4, 0x11111111);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'It is invalid to issue this call inside an active render pass...'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, UpdateBufferWithinRenderPass)
{
    // Call CmdUpdateBuffer within an active renderpass
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "It is invalid to issue this call inside an active render pass");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    // Renderpass is started here
    BeginCommandBuffer();

    VkMemoryPropertyFlags reqs = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    vk_testing::Buffer dstBuffer;
    dstBuffer.init_as_dst(*m_device, (VkDeviceSize)1024, reqs);

    VkDeviceSize    dstOffset = 0;
    VkDeviceSize    dataSize   = 1024;
    const uint32_t *pData      = NULL;

    vkCmdUpdateBuffer(m_commandBuffer->GetBufferHandle(), dstBuffer.handle(), dstOffset, dataSize, pData);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'It is invalid to issue this call inside an active render pass...'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, ClearColorImageWithinRenderPass)
{
    // Call CmdClearColorImage within an active RenderPass
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "It is invalid to issue this call inside an active render pass");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    // Renderpass is started here
    BeginCommandBuffer();

    VkClearColorValue     clear_color       = {0};
    VkMemoryPropertyFlags reqs              = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkFormat        tex_format        = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t         tex_width         = 32;
    const int32_t         tex_height        = 32;
    VkImageCreateInfo     image_create_info = {};
    image_create_info.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.pNext                 = NULL;
    image_create_info.imageType             = VK_IMAGE_TYPE_2D;
    image_create_info.format                = tex_format;
    image_create_info.extent.width          = tex_width;
    image_create_info.extent.height         = tex_height;
    image_create_info.extent.depth          = 1;
    image_create_info.mipLevels             = 1;
    image_create_info.arrayLayers             = 1;
    image_create_info.samples               = 1;
    image_create_info.tiling                = VK_IMAGE_TILING_LINEAR;
    image_create_info.usage                 = VK_IMAGE_USAGE_SAMPLED_BIT;

    vk_testing::Image dstImage;
    dstImage.init(*m_device, (const VkImageCreateInfo&)image_create_info, reqs);

    const VkImageSubresourceRange range =
        vk_testing::Image::subresource_range(image_create_info, VK_IMAGE_ASPECT_COLOR_BIT);

    vkCmdClearColorImage(m_commandBuffer->GetBufferHandle(),
                         dstImage.handle(),
                         VK_IMAGE_LAYOUT_GENERAL,
                         &clear_color,
                         1,
                         &range);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'It is invalid to issue this call inside an active render pass...'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, ClearDepthStencilImageWithinRenderPass)
{
    // Call CmdClearDepthStencilImage within an active RenderPass
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "It is invalid to issue this call inside an active render pass");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    // Renderpass is started here
    BeginCommandBuffer();

    VkClearDepthStencilValue clear_value = {0};
    VkMemoryPropertyFlags    reqs        = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VkImageCreateInfo image_create_info  = vk_testing::Image::create_info();
    image_create_info.imageType          = VK_IMAGE_TYPE_2D;
    image_create_info.format             = VK_FORMAT_D24_UNORM_S8_UINT;
    image_create_info.extent.width       = 64;
    image_create_info.extent.height      = 64;
    image_create_info.tiling             = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.usage              = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    vk_testing::Image dstImage;
    dstImage.init(*m_device, (const VkImageCreateInfo&)image_create_info, reqs);

    const VkImageSubresourceRange range =
        vk_testing::Image::subresource_range(image_create_info, VK_IMAGE_ASPECT_DEPTH_BIT);

    vkCmdClearDepthStencilImage(m_commandBuffer->GetBufferHandle(),
                                dstImage.handle(),
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                &clear_value,
                                1,
                                &range);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'It is invalid to issue this call inside an active render pass...'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, ClearColorAttachmentsOutsideRenderPass)
{
    // Call CmdClearAttachmentss outside of an active RenderPass
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
         "vkCmdClearAttachments: This call must be issued inside an active render pass");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    // Start no RenderPass
    err = m_commandBuffer->BeginCommandBuffer();
    ASSERT_VK_SUCCESS(err);

    VkClearAttachment color_attachment;
    color_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    color_attachment.clearValue.color.float32[0] = 0;
    color_attachment.clearValue.color.float32[1] = 0;
    color_attachment.clearValue.color.float32[2] = 0;
    color_attachment.clearValue.color.float32[3] = 0;
    color_attachment.colorAttachment = 0;
    VkClearRect clear_rect  = { { { 0, 0 }, { 32, 32 } } };
    vkCmdClearAttachments(m_commandBuffer->GetBufferHandle(),
                          1, &color_attachment,
                          1, &clear_rect);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCmdClearAttachments: This call must be issued inside an active render pass.'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, InvalidDynamicStateObject)
{
    // Create a valid cmd buffer
    // call vkCmdBindDynamicStateObject w/ false DS Obj
    // TODO : Simple check for bad object should be added to ObjectTracker to catch this case
    //   The DS check for this is after driver has been called to validate DS internal data struct
}

TEST_F(VkLayerTest, IdxBufferAlignmentError)
{
    // Bind a BeginRenderPass within an active RenderPass
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCmdBindIndexBuffer() offset (0x7) does not fall on ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());
    uint32_t qfi = 0;
    VkBufferCreateInfo buffCI = {};
        buffCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffCI.size = 1024;
        buffCI.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        buffCI.queueFamilyIndexCount = 1;
        buffCI.pQueueFamilyIndices = &qfi;

    VkBuffer ib;
    err = vkCreateBuffer(m_device->device(), &buffCI, NULL, &ib);
    ASSERT_VK_SUCCESS(err);

    BeginCommandBuffer();
    ASSERT_VK_SUCCESS(err);
    //vkCmdBindPipeline(m_commandBuffer->GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.handle());
    // Should error before calling to driver so don't care about actual data
    vkCmdBindIndexBuffer(m_commandBuffer->GetBufferHandle(), ib, 7, VK_INDEX_TYPE_UINT16);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCmdBindIndexBuffer() offset (0x7) does not fall on ...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyBuffer(m_device->device(), ib, NULL);
}

TEST_F(VkLayerTest, ExecuteCommandsPrimaryCB)
{
    // Attempt vkCmdExecuteCommands w/ a primary cmd buffer (should only be secondary)

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCmdExecuteCommands() called w/ Primary Cmd Buffer ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    BeginCommandBuffer();
    //ASSERT_VK_SUCCESS(err);
    VkCommandBuffer primCB = m_commandBuffer->GetBufferHandle();
    vkCmdExecuteCommands(m_commandBuffer->GetBufferHandle(), 1, &primCB);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCmdExecuteCommands() called w/ Primary Cmd Buffer '";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, DSTypeMismatch)
{
    // Create DS w/ layout of one type and attempt Update w/ mis-matched type
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Write descriptor update has descriptor type VK_DESCRIPTOR_TYPE_SAMPLER that does not match ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    //VkDescriptorSetObj descriptorSet(m_device);
    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);
    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkSamplerCreateInfo sampler_ci = {};
        sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.pNext = NULL;
        sampler_ci.magFilter = VK_FILTER_NEAREST;
        sampler_ci.minFilter = VK_FILTER_NEAREST;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_BASE;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.mipLodBias = 1.0;
        sampler_ci.maxAnisotropy = 1;
        sampler_ci.compareEnable = VK_FALSE;
        sampler_ci.compareOp = VK_COMPARE_OP_NEVER;
        sampler_ci.minLod = 1.0;
        sampler_ci.maxLod = 1.0;
        sampler_ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_ci.unnormalizedCoordinates = VK_FALSE;

    VkSampler sampler;
    err = vkCreateSampler(m_device->device(), &sampler_ci, NULL, &sampler);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorImageInfo info = {};
    info.sampler = sampler;

    VkWriteDescriptorSet descriptor_write;
    memset(&descriptor_write, 0, sizeof(descriptor_write));
    descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.descriptorCount = 1;
    // This is a mismatched type for the layout which expects BUFFER
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_write.pImageInfo = &info;

    vkUpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Write descriptor update has descriptor type VK_DESCRIPTOR_TYPE_SAMPLER that does not match...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroySampler(m_device->device(), sampler, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, DSUpdateOutOfBounds)
{
    // For overlapping Update, have arrayIndex exceed that of layout
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Descriptor update type of VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET is out of bounds for matching binding");

    ASSERT_NO_FATAL_FAILURE(InitState());
    //VkDescriptorSetObj descriptorSet(m_device);
    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkSamplerCreateInfo sampler_ci = {};
        sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.pNext = NULL;
        sampler_ci.magFilter = VK_FILTER_NEAREST;
        sampler_ci.minFilter = VK_FILTER_NEAREST;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_BASE;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.mipLodBias = 1.0;
        sampler_ci.maxAnisotropy = 1;
        sampler_ci.compareEnable = VK_FALSE;
        sampler_ci.compareOp = VK_COMPARE_OP_NEVER;
        sampler_ci.minLod = 1.0;
        sampler_ci.maxLod = 1.0;
        sampler_ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_ci.unnormalizedCoordinates = VK_FALSE;

    VkSampler sampler;
    err = vkCreateSampler(m_device->device(), &sampler_ci, NULL, &sampler);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorImageInfo info = {};
    info.sampler = sampler;

    VkWriteDescriptorSet descriptor_write;
    memset(&descriptor_write, 0, sizeof(descriptor_write));
    descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.dstArrayElement = 1; /* This index out of bounds for the update */
    descriptor_write.descriptorCount = 1;
    // This is the wrong type, but out of bounds will be flagged first
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_write.pImageInfo = &info;

    vkUpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Descriptor update type of VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET is out of bounds for matching binding...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroySampler(m_device->device(), sampler, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, InvalidDSUpdateIndex)
{
    // Create layout w/ count of 1 and attempt update to that layout w/ binding index 2
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        " does not have binding to match update binding ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    //VkDescriptorSetObj descriptorSet(m_device);
    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;
    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkSamplerCreateInfo sampler_ci = {};
        sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.pNext = NULL;
        sampler_ci.magFilter = VK_FILTER_NEAREST;
        sampler_ci.minFilter = VK_FILTER_NEAREST;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_BASE;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.mipLodBias = 1.0;
        sampler_ci.maxAnisotropy = 1;
        sampler_ci.compareEnable = VK_FALSE;
        sampler_ci.compareOp = VK_COMPARE_OP_NEVER;
        sampler_ci.minLod = 1.0;
        sampler_ci.maxLod = 1.0;
        sampler_ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_ci.unnormalizedCoordinates = VK_FALSE;

    VkSampler sampler;
    err = vkCreateSampler(m_device->device(), &sampler_ci, NULL, &sampler);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorImageInfo info = {};
    info.sampler = sampler;

    VkWriteDescriptorSet descriptor_write;
    memset(&descriptor_write, 0, sizeof(descriptor_write));
    descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.dstBinding = 2;
    descriptor_write.descriptorCount = 1;
    // This is the wrong type, but out of bounds will be flagged first
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_write.pImageInfo = &info;

    vkUpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Descriptor Set <blah> does not have binding to match update binding '";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroySampler(m_device->device(), sampler, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, InvalidDSUpdateStruct)
{
    // Call UpdateDS w/ struct type other than valid VK_STRUCTUR_TYPE_UPDATE_* types
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Unexpected UPDATE struct of type ");

    ASSERT_NO_FATAL_FAILURE(InitState());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);
    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkSamplerCreateInfo sampler_ci = {};
        sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.pNext = NULL;
        sampler_ci.magFilter = VK_FILTER_NEAREST;
        sampler_ci.minFilter = VK_FILTER_NEAREST;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_BASE;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.mipLodBias = 1.0;
        sampler_ci.maxAnisotropy = 1;
        sampler_ci.compareEnable = VK_FALSE;
        sampler_ci.compareOp = VK_COMPARE_OP_NEVER;
        sampler_ci.minLod = 1.0;
        sampler_ci.maxLod = 1.0;
        sampler_ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_ci.unnormalizedCoordinates = VK_FALSE;
    VkSampler sampler;
    err = vkCreateSampler(m_device->device(), &sampler_ci, NULL, &sampler);
    ASSERT_VK_SUCCESS(err);


    VkDescriptorImageInfo info = {};
    info.sampler = sampler;

    VkWriteDescriptorSet descriptor_write;
    memset(&descriptor_write, 0, sizeof(descriptor_write));
    descriptor_write.sType = (VkStructureType)0x99999999; /* Intentionally broken struct type */
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.descriptorCount = 1;
    // This is the wrong type, but out of bounds will be flagged first
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_write.pImageInfo = &info;

    vkUpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Unexpected UPDATE struct of type '";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroySampler(m_device->device(), sampler, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, SampleDescriptorUpdateError)
{
    // Create a single Sampler descriptor and send it an invalid Sampler
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Attempt to update descriptor with invalid sampler 0xbaadbeef");

    ASSERT_NO_FATAL_FAILURE(InitState());
    // TODO : Farm Descriptor setup code to helper function(s) to reduce copied code
    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;
    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkSampler sampler = (VkSampler) 0xbaadbeef; // Sampler with invalid handle

    VkDescriptorImageInfo descriptor_info;
    memset(&descriptor_info, 0, sizeof(VkDescriptorImageInfo));
    descriptor_info.sampler = sampler;

    VkWriteDescriptorSet descriptor_write;
    memset(&descriptor_write, 0, sizeof(descriptor_write));
    descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.dstBinding = 0;
    descriptor_write.descriptorCount = 1;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_write.pImageInfo = &descriptor_info;

    vkUpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Attempt to update descriptor with invalid sampler...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, ImageViewDescriptorUpdateError)
{
    // Create a single combined Image/Sampler descriptor and send it an invalid imageView
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Attempt to update descriptor with invalid imageView 0xbaadbeef");

    ASSERT_NO_FATAL_FAILURE(InitState());
    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;
    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkSamplerCreateInfo sampler_ci = {};
        sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.pNext = NULL;
        sampler_ci.magFilter = VK_FILTER_NEAREST;
        sampler_ci.minFilter = VK_FILTER_NEAREST;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_BASE;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.mipLodBias = 1.0;
        sampler_ci.maxAnisotropy = 1;
        sampler_ci.compareEnable = VK_FALSE;
        sampler_ci.compareOp = VK_COMPARE_OP_NEVER;
        sampler_ci.minLod = 1.0;
        sampler_ci.maxLod = 1.0;
        sampler_ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_ci.unnormalizedCoordinates = VK_FALSE;

    VkSampler sampler;
    err = vkCreateSampler(m_device->device(), &sampler_ci, NULL, &sampler);
    ASSERT_VK_SUCCESS(err);

    VkImageView view = (VkImageView) 0xbaadbeef; // invalid imageView object

    VkDescriptorImageInfo descriptor_info;
    memset(&descriptor_info, 0, sizeof(VkDescriptorImageInfo));
    descriptor_info.sampler = sampler;
    descriptor_info.imageView = view;

    VkWriteDescriptorSet descriptor_write;
    memset(&descriptor_write, 0, sizeof(descriptor_write));
    descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.dstBinding = 0;
    descriptor_write.descriptorCount = 1;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_write.pImageInfo = &descriptor_info;

    vkUpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Attempt to update descriptor with invalid imageView...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroySampler(m_device->device(), sampler, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, CopyDescriptorUpdateErrors)
{
    // Create DS w/ layout of 2 types, write update 1 and attempt to copy-update into the other
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Copy descriptor update index 0, update count #1, has src update descriptor type VK_DESCRIPTOR_TYPE_SAMPLER ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    //VkDescriptorSetObj descriptorSet(m_device);
    VkDescriptorPoolSize ds_type_count[2] = {};
        ds_type_count[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count[0].descriptorCount = 1;
        ds_type_count[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        ds_type_count[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 2;
        ds_pool_ci.pPoolSizes = ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);
    VkDescriptorSetLayoutBinding dsl_binding[2] = {};
        dsl_binding[0].binding = 0;
        dsl_binding[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding[0].arraySize = 1;
        dsl_binding[0].stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding[0].pImmutableSamplers = NULL;
        dsl_binding[1].binding = 1;
        dsl_binding[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        dsl_binding[1].arraySize = 1;
        dsl_binding[1].stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding[1].pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 2;
        ds_layout_ci.pBinding = dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkSamplerCreateInfo sampler_ci = {};
        sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.pNext = NULL;
        sampler_ci.magFilter = VK_FILTER_NEAREST;
        sampler_ci.minFilter = VK_FILTER_NEAREST;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_BASE;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.mipLodBias = 1.0;
        sampler_ci.maxAnisotropy = 1;
        sampler_ci.compareEnable = VK_FALSE;
        sampler_ci.compareOp = VK_COMPARE_OP_NEVER;
        sampler_ci.minLod = 1.0;
        sampler_ci.maxLod = 1.0;
        sampler_ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_ci.unnormalizedCoordinates = VK_FALSE;

    VkSampler sampler;
    err = vkCreateSampler(m_device->device(), &sampler_ci, NULL, &sampler);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorImageInfo info = {};
    info.sampler = sampler;

    VkWriteDescriptorSet descriptor_write;
    memset(&descriptor_write, 0, sizeof(VkWriteDescriptorSet));
    descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.dstBinding = 1; // SAMPLER binding from layout above
    descriptor_write.descriptorCount = 1;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_write.pImageInfo = &info;
    // This write update should succeed
    vkUpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);
    // Now perform a copy update that fails due to type mismatch
    VkCopyDescriptorSet copy_ds_update;
    memset(&copy_ds_update, 0, sizeof(VkCopyDescriptorSet));
    copy_ds_update.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
    copy_ds_update.srcSet = descriptorSet;
    copy_ds_update.srcBinding = 1; // copy from SAMPLER binding
    copy_ds_update.dstSet = descriptorSet;
    copy_ds_update.dstBinding = 0; // ERROR : copy to UNIFORM binding
    copy_ds_update.descriptorCount = 1; // copy 1 descriptor
    vkUpdateDescriptorSets(m_device->device(), 0, NULL, 1, &copy_ds_update);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Copy descriptor update index 0, update count #1, has src update descriptor type_DESCRIPTOR_TYPE_SAMPLER'";
        m_errorMonitor->DumpFailureMsgs();
    }
    // Now perform a copy update that fails due to binding out of bounds
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Copy descriptor update 0 has srcBinding 3 which is out of bounds ");
    memset(&copy_ds_update, 0, sizeof(VkCopyDescriptorSet));
    copy_ds_update.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
    copy_ds_update.srcSet = descriptorSet;
    copy_ds_update.srcBinding = 3; // ERROR : Invalid binding for matching layout
    copy_ds_update.dstSet = descriptorSet;
    copy_ds_update.dstBinding = 0;
    copy_ds_update.descriptorCount = 1; // copy 1 descriptor
    vkUpdateDescriptorSets(m_device->device(), 0, NULL, 1, &copy_ds_update);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Copy descriptor update 0 has srcBinding 3 which is out of bounds...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    // Now perform a copy update that fails due to binding out of bounds
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Copy descriptor src update is out of bounds for matching binding 1 ");

    memset(&copy_ds_update, 0, sizeof(VkCopyDescriptorSet));
    copy_ds_update.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
    copy_ds_update.srcSet = descriptorSet;
    copy_ds_update.srcBinding = 1;
    copy_ds_update.dstSet = descriptorSet;
    copy_ds_update.dstBinding = 0;
    copy_ds_update.descriptorCount = 5; // ERROR copy 5 descriptors (out of bounds for layout)
    vkUpdateDescriptorSets(m_device->device(), 0, NULL, 1, &copy_ds_update);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Copy descriptor src update is out of bounds for matching binding 1...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroySampler(m_device->device(), sampler, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, NumSamplesMismatch)
{
    // Create CommandBuffer where MSAA samples doesn't match RenderPass sampleCount
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Num samples mismatch! ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());
    VkDescriptorPoolSize ds_type_count = {};
    ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
    dsl_binding.binding = 0;
    dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dsl_binding.arraySize = 1;
    dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
    dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
    ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ds_layout_ci.pNext = NULL;
    ds_layout_ci.bindingCount = 1;
    ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineMultisampleStateCreateInfo pipe_ms_state_ci = {};
        pipe_ms_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        pipe_ms_state_ci.pNext = NULL;
        pipe_ms_state_ci.rasterizationSamples = 4;
        pipe_ms_state_ci.sampleShadingEnable = 0;
        pipe_ms_state_ci.minSampleShading = 1.0;
        pipe_ms_state_ci.pSampleMask = NULL;

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.pNext = NULL;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkShaderObj vs(m_device, bindStateVertShaderText, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, bindStateFragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this); //  TODO - We shouldn't need a fragment shader
                                                                                       // but add it to be able to run on more devices
    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);
    pipe.SetMSAA(&pipe_ms_state_ci);
    pipe.CreateVKPipeline(pipeline_layout, renderPass());

    BeginCommandBuffer();
    vkCmdBindPipeline(m_commandBuffer->GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.handle());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieve Error 'Num samples mismatch!...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, ClearCmdNoDraw)
{
    // Create CommandBuffer where we add ClearCmd for FB Color attachment prior to issuing a Draw
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_WARN_BIT,
        "vkCmdClearAttachments() issued on CB object ");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineMultisampleStateCreateInfo pipe_ms_state_ci = {};
        pipe_ms_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        pipe_ms_state_ci.pNext = NULL;
        pipe_ms_state_ci.rasterizationSamples = 4;
        pipe_ms_state_ci.sampleShadingEnable = 0;
        pipe_ms_state_ci.minSampleShading = 1.0;
        pipe_ms_state_ci.pSampleMask = NULL;

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.pNext = NULL;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;

    VkPipelineLayout pipeline_layout;
    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkShaderObj vs(m_device, bindStateVertShaderText, VK_SHADER_STAGE_VERTEX_BIT, this);
    //  TODO - We shouldn't need a fragment shader but add it to be able to run on more devices
    VkShaderObj fs(m_device, bindStateFragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);
    pipe.SetMSAA(&pipe_ms_state_ci);
    pipe.CreateVKPipeline(pipeline_layout, renderPass());

    BeginCommandBuffer();

    // Main thing we care about for this test is that the VkImage obj we're clearing matches Color Attachment of FB
    //  Also pass down other dummy params to keep driver and paramchecker happy
    VkClearAttachment color_attachment;
    color_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    color_attachment.clearValue.color.float32[0] = 1.0;
    color_attachment.clearValue.color.float32[1] = 1.0;
    color_attachment.clearValue.color.float32[2] = 1.0;
    color_attachment.clearValue.color.float32[3] = 1.0;
    color_attachment.colorAttachment = 0;
    VkClearRect clear_rect = { { { 0, 0 }, { (int)m_width, (int)m_height } } };

    vkCmdClearAttachments(m_commandBuffer->GetBufferHandle(), 1, &color_attachment, 1, &clear_rect);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCommandClearAttachments() issued on CB object...'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}

TEST_F(VkLayerTest, VtxBufferBadIndex)
{
    // Create CommandBuffer where MSAA samples doesn't match RenderPass sampleCount
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Vtx Buffer Index 1 was bound, but no vtx buffers are attached to PSO.");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitViewport());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;

    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkPipelineMultisampleStateCreateInfo pipe_ms_state_ci = {};
        pipe_ms_state_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        pipe_ms_state_ci.pNext = NULL;
        pipe_ms_state_ci.rasterizationSamples = 1;
        pipe_ms_state_ci.sampleShadingEnable = 0;
        pipe_ms_state_ci.minSampleShading = 1.0;
        pipe_ms_state_ci.pSampleMask = NULL;

    VkPipelineLayoutCreateInfo pipeline_layout_ci = {};
        pipeline_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_ci.pNext = NULL;
        pipeline_layout_ci.setLayoutCount = 1;
        pipeline_layout_ci.pSetLayouts = &ds_layout;
        VkPipelineLayout pipeline_layout;

    err = vkCreatePipelineLayout(m_device->device(), &pipeline_layout_ci, NULL, &pipeline_layout);
    ASSERT_VK_SUCCESS(err);

    VkShaderObj vs(m_device, bindStateVertShaderText, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, bindStateFragShaderText, VK_SHADER_STAGE_FRAGMENT_BIT, this); //  TODO - We shouldn't need a fragment shader
                                                                                       // but add it to be able to run on more devices
    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);
    pipe.SetMSAA(&pipe_ms_state_ci);
    pipe.SetViewport(m_viewports);
    pipe.SetScissor(m_scissors);
    pipe.CreateVKPipeline(pipeline_layout, renderPass());

    BeginCommandBuffer();
    vkCmdBindPipeline(m_commandBuffer->GetBufferHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.handle());
    // Don't care about actual data, just need to get to draw to flag error
    static const float vbo_data[3] = {1.f, 0.f, 1.f};
    VkConstantBufferObj vbo(m_device, sizeof(vbo_data), sizeof(float), (const void*) &vbo_data);
    BindVertexBuffer(&vbo, (VkDeviceSize)0, 1); // VBO idx 1, but no VBO in PSO
    Draw(1, 0, 0, 0);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Vtx Buffer Index 0 was bound, but no vtx buffers are attached to PSO.'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyPipelineLayout(m_device->device(), pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}
#endif // DRAW_STATE_TESTS

#if THREADING_TESTS
#if GTEST_IS_THREADSAFE
struct thread_data_struct {
    VkCommandBuffer commandBuffer;
    VkEvent event;
    bool bailout;
};

extern "C" void *AddToCommandBuffer(void *arg)
{
    struct thread_data_struct *data = (struct thread_data_struct *) arg;

    for (int i = 0; i<10000; i++) {
        vkCmdSetEvent(data->commandBuffer, data->event, VK_PIPELINE_STAGE_ALL_GPU_COMMANDS);
        if (data->bailout) {
            break;
        }
    }
    return NULL;
}

TEST_F(VkLayerTest, ThreadCommandBufferCollision)
{
    test_platform_thread thread;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT, "THREADING ERROR");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitViewport());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    // Calls AllocateCommandBuffers
    VkCommandBufferObj commandBuffer(m_device, m_commandPool);

    // Avoid creating RenderPass
    commandBuffer.BeginCommandBuffer();

    VkEventCreateInfo event_info;
    VkEvent event;
    VkResult err;

    memset(&event_info, 0, sizeof(event_info));
    event_info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;

    err = vkCreateEvent(device(), &event_info, NULL, &event);
    ASSERT_VK_SUCCESS(err);

    err = vkResetEvent(device(), event);
    ASSERT_VK_SUCCESS(err);

    struct thread_data_struct data;
    data.commandBuffer = commandBuffer.GetBufferHandle();
    data.event = event;
    data.bailout = false;
    m_errorMonitor->SetBailout(&data.bailout);
    // Add many entries to command buffer from another thread.
    test_platform_thread_create(&thread, AddToCommandBuffer, (void *)&data);
    // Add many entries to command buffer from this thread at the same time.
    AddToCommandBuffer(&data);

    test_platform_thread_join(thread, NULL);
    commandBuffer.EndCommandBuffer();

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'THREADING ERROR' from using one VkCommandBufferObj in two threads";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyEvent(device(), event, NULL);
}
#endif // GTEST_IS_THREADSAFE
#endif // THREADING_TESTS

#if SHADER_CHECKER_TESTS
TEST_F(VkLayerTest, InvalidSPIRVCodeSize)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Shader is not SPIR-V");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkShaderModule module;
    VkShaderModuleCreateInfo moduleCreateInfo;
    struct icd_spv_header spv;

    spv.magic = ICD_SPV_MAGIC;
    spv.version = ICD_SPV_VERSION;
    spv.gen_magic = 0;

    moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleCreateInfo.pNext = NULL;
    moduleCreateInfo.pCode = (const uint32_t *) &spv;
    moduleCreateInfo.codeSize = 4;
    moduleCreateInfo.flags = 0;
    vkCreateShaderModule(m_device->device(), &moduleCreateInfo, NULL, &module);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieive Error 'Shader is not SPIR-V'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, InvalidSPIRVMagic)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Shader is not SPIR-V");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkShaderModule module;
    VkShaderModuleCreateInfo moduleCreateInfo;
    struct icd_spv_header spv;

    spv.magic = ~ICD_SPV_MAGIC;
    spv.version = ICD_SPV_VERSION;
    spv.gen_magic = 0;

    moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleCreateInfo.pNext = NULL;
    moduleCreateInfo.pCode = (const uint32_t *) &spv;
    moduleCreateInfo.codeSize = sizeof(spv) + 10;
    moduleCreateInfo.flags = 0;
    vkCreateShaderModule(m_device->device(), &moduleCreateInfo, NULL, &module);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieive Error 'Shader is not SPIR-V'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, InvalidSPIRVVersion)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Shader is not SPIR-V");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkShaderModule module;
    VkShaderModuleCreateInfo moduleCreateInfo;
    struct icd_spv_header spv;

    spv.magic = ICD_SPV_MAGIC;
    spv.version = ~ICD_SPV_VERSION;
    spv.gen_magic = 0;

    moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleCreateInfo.pNext = NULL;

    moduleCreateInfo.pCode = (const uint32_t *) &spv;
    moduleCreateInfo.codeSize = sizeof(spv) + 10;
    moduleCreateInfo.flags = 0;
    vkCreateShaderModule(m_device->device(), &moduleCreateInfo, NULL, &module);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not recieive Error 'Shader is not SPIR-V'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineVertexOutputNotConsumed)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_WARN_BIT,
        "not consumed by fragment shader");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out float x;\n"
        "void main(){\n"
        "   gl_Position = vec4(1);\n"
        "   x = 0;\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out vec4 color;\n"
        "void main(){\n"
        "   color = vec4(1);\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddColorAttachment();
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Warning 'not consumed by fragment shader'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineFragmentInputNotProvided)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "not written by vertex shader");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "void main(){\n"
        "   gl_Position = vec4(1);\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) in float x;\n"
        "layout(location=0) out vec4 color;\n"
        "void main(){\n"
        "   color = vec4(x);\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddColorAttachment();
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'not written by vertex shader'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineVsFsTypeMismatch)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Type mismatch on location 0");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out int x;\n"
        "void main(){\n"
        "   x = 0;\n"
        "   gl_Position = vec4(1);\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) in float x;\n"  /* VS writes int */
        "layout(location=0) out vec4 color;\n"
        "void main(){\n"
        "   color = vec4(x);\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddColorAttachment();
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Type mismatch on location 0'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineAttribNotConsumed)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_WARN_BIT,
        "location 0 not consumed by VS");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkVertexInputBindingDescription input_binding;
    memset(&input_binding, 0, sizeof(input_binding));

    VkVertexInputAttributeDescription input_attrib;
    memset(&input_attrib, 0, sizeof(input_attrib));
    input_attrib.format = VK_FORMAT_R32_SFLOAT;

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "void main(){\n"
        "   gl_Position = vec4(1);\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out vec4 color;\n"
        "void main(){\n"
        "   color = vec4(1);\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddColorAttachment();
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    pipe.AddVertexInputBindings(&input_binding, 1);
    pipe.AddVertexInputAttribs(&input_attrib, 1);

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Warning 'location 0 not consumed by VS'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineAttribNotProvided)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "VS consumes input at location 0 but not provided");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) in vec4 x;\n"       /* not provided */
        "void main(){\n"
        "   gl_Position = x;\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out vec4 color;\n"
        "void main(){\n"
        "   color = vec4(1);\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddColorAttachment();
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'VS consumes input at location 0 but not provided'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineAttribTypeMismatch)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "location 0 does not match VS input type");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkVertexInputBindingDescription input_binding;
    memset(&input_binding, 0, sizeof(input_binding));

    VkVertexInputAttributeDescription input_attrib;
    memset(&input_attrib, 0, sizeof(input_attrib));
    input_attrib.format = VK_FORMAT_R32_SFLOAT;

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) in int x;\n"    /* attrib provided float */
        "void main(){\n"
        "   gl_Position = vec4(x);\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out vec4 color;\n"
        "void main(){\n"
        "   color = vec4(1);\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddColorAttachment();
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    pipe.AddVertexInputBindings(&input_binding, 1);
    pipe.AddVertexInputAttribs(&input_attrib, 1);

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'location 0 does not match VS input type'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineAttribBindingConflict)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Duplicate vertex input binding descriptions for binding 0");

    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    /* Two binding descriptions for binding 0 */
    VkVertexInputBindingDescription input_bindings[2];
    memset(input_bindings, 0, sizeof(input_bindings));

    VkVertexInputAttributeDescription input_attrib;
    memset(&input_attrib, 0, sizeof(input_attrib));
    input_attrib.format = VK_FORMAT_R32_SFLOAT;

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) in float x;\n"    /* attrib provided float */
        "void main(){\n"
        "   gl_Position = vec4(x);\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out vec4 color;\n"
        "void main(){\n"
        "   color = vec4(1);\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddColorAttachment();
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    pipe.AddVertexInputBindings(input_bindings, 2);
    pipe.AddVertexInputAttribs(&input_attrib, 1);

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Duplicate vertex input binding descriptions for binding 0'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

/* TODO: would be nice to test the mixed broadcast & custom case, but the GLSL->SPV compiler
 * rejects it. */

TEST_F(VkLayerTest, CreatePipelineFragmentOutputNotWritten)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Attachment 0 not written by FS");

    ASSERT_NO_FATAL_FAILURE(InitState());

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "void main(){\n"
        "   gl_Position = vec4(1);\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "void main(){\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    /* set up CB 0, not written */
    pipe.AddColorAttachment();
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Attachment 0 not written by FS'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineFragmentOutputNotConsumed)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_WARN_BIT,
        "FS writes to output location 1 with no matching attachment");

    ASSERT_NO_FATAL_FAILURE(InitState());

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "void main(){\n"
        "   gl_Position = vec4(1);\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out vec4 x;\n"
        "layout(location=1) out vec4 y;\n"  /* no matching attachment for this */
        "void main(){\n"
        "   x = vec4(1);\n"
        "   y = vec4(1);\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    /* set up CB 0, not written */
    pipe.AddColorAttachment();
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());
    /* FS writes CB 1, but we don't configure it */

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'FS writes to output location 1 with no matching attachment'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineFragmentOutputTypeMismatch)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "does not match FS output type");

    ASSERT_NO_FATAL_FAILURE(InitState());

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "void main(){\n"
        "   gl_Position = vec4(1);\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out ivec4 x;\n"     /* not UNORM */
        "void main(){\n"
        "   x = ivec4(1);\n"
        "}\n";

    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    /* set up CB 0; type is UNORM by default */
    pipe.AddColorAttachment();
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.AppendDummy();
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'does not match FS output type'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreatePipelineUniformBlockNotProvided)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "not declared in pipeline layout");

    ASSERT_NO_FATAL_FAILURE(InitState());

    char const *vsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "void main(){\n"
        "   gl_Position = vec4(1);\n"
        "}\n";
    char const *fsSource =
        "#version 140\n"
        "#extension GL_ARB_separate_shader_objects: require\n"
        "#extension GL_ARB_shading_language_420pack: require\n"
        "\n"
        "layout(location=0) out vec4 x;\n"
        "layout(set=0) layout(binding=0) uniform foo { int x; int y; } bar;\n"
        "void main(){\n"
        "   x = vec4(bar.y);\n"
        "}\n";


    VkShaderObj vs(m_device, vsSource, VK_SHADER_STAGE_VERTEX_BIT, this);
    VkShaderObj fs(m_device, fsSource, VK_SHADER_STAGE_FRAGMENT_BIT, this);

    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);

    /* set up CB 0; type is UNORM by default */
    pipe.AddColorAttachment();
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkDescriptorSetObj descriptorSet(m_device);
    descriptorSet.CreateVKDescriptorSet(m_commandBuffer);

    pipe.CreateVKPipeline(descriptorSet.GetPipelineLayout(), renderPass());

    /* should have generated an error -- pipeline layout does not
     * provide a uniform buffer in 0.0
     */
    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'not declared in pipeline layout'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

#endif // SHADER_CHECKER_TESTS

#if DEVICE_LIMITS_TESTS
TEST_F(VkLayerTest, CreateImageLimitsViolationWidth)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "CreateImage extents exceed allowable limits for format");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create an image
    VkImage image;

    const VkFormat tex_format      = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t  tex_width       = 32;
    const int32_t  tex_height      = 32;

    VkImageCreateInfo image_create_info = {};
    image_create_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.pNext         = NULL;
    image_create_info.imageType     = VK_IMAGE_TYPE_2D;
    image_create_info.format        = tex_format;
    image_create_info.extent.width  = tex_width;
    image_create_info.extent.height = tex_height;
    image_create_info.extent.depth  = 1;
    image_create_info.mipLevels     = 1;
    image_create_info.arrayLayers     = 1;
    image_create_info.samples       = 1;
    image_create_info.tiling        = VK_IMAGE_TILING_LINEAR;
    image_create_info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT;
    image_create_info.flags         = 0;

    // Introduce error by sending down a bogus width extent
    image_create_info.extent.width = 65536;
    vkCreateImage(m_device->device(), &image_create_info, NULL, &image);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'CreateImage extents exceed allowable limits for format'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CreateImageResourceSizeViolation)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "CreateImage resource size exceeds allowable maximum");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create an image
    VkImage image;

    const VkFormat tex_format      = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t  tex_width       = 32;
    const int32_t  tex_height      = 32;

    VkImageCreateInfo image_create_info = {};
    image_create_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.pNext         = NULL;
    image_create_info.imageType     = VK_IMAGE_TYPE_2D;
    image_create_info.format        = tex_format;
    image_create_info.extent.width  = tex_width;
    image_create_info.extent.height = tex_height;
    image_create_info.extent.depth  = 1;
    image_create_info.mipLevels     = 1;
    image_create_info.arrayLayers     = 1;
    image_create_info.samples       = 1;
    image_create_info.tiling        = VK_IMAGE_TILING_LINEAR;
    image_create_info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT;
    image_create_info.flags         = 0;

    // Introduce error by sending down individually allowable values that result in a surface size
    // exceeding the device maximum
    image_create_info.extent.width  = 8192;
    image_create_info.extent.height = 8192;
    image_create_info.extent.depth  = 16;
    image_create_info.arrayLayers     = 4;
    image_create_info.samples       = 2;
    image_create_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    vkCreateImage(m_device->device(), &image_create_info, NULL, &image);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'CreateImage resource size exceeds allowable maximum'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, UpdateBufferAlignment)
{
    uint32_t updateData[] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "dstOffset, is not a multiple of 4");

    ASSERT_NO_FATAL_FAILURE(InitState());

    VkMemoryPropertyFlags reqs = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    vk_testing::Buffer buffer;
    buffer.init_as_dst(*m_device, (VkDeviceSize)20, reqs);

    BeginCommandBuffer();
    // Introduce failure by using offset that is not multiple of 4
    m_commandBuffer->UpdateBuffer(buffer.handle(), 1, 4, updateData);
    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCommandUpdateBuffer parameter, VkDeviceSize dstOffset, is not a multiple of 4'";
        m_errorMonitor->DumpFailureMsgs();
    }

    // Introduce failure by using size that is not multiple of 4
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "dataSize, is not a multiple of 4");

    m_commandBuffer->UpdateBuffer(buffer.handle(), 0, 6, updateData);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCommandUpdateBuffer parameter, VkDeviceSize dataSize, is not a multiple of 4'";
        m_errorMonitor->DumpFailureMsgs();
    }
    EndCommandBuffer();
}

TEST_F(VkLayerTest, FillBufferAlignment)
{
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "dstOffset, is not a multiple of 4");

    ASSERT_NO_FATAL_FAILURE(InitState());

    VkMemoryPropertyFlags reqs = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    vk_testing::Buffer buffer;
    buffer.init_as_dst(*m_device, (VkDeviceSize)20, reqs);

    BeginCommandBuffer();
    // Introduce failure by using offset that is not multiple of 4
    m_commandBuffer->FillBuffer(buffer.handle(), 1, 4, 0x11111111);
    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCommandFillBuffer parameter, VkDeviceSize dstOffset, is not a multiple of 4'";
        m_errorMonitor->DumpFailureMsgs();
    }

    // Introduce failure by using size that is not multiple of 4
    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "size, is not a multiple of 4");

    m_commandBuffer->FillBuffer(buffer.handle(), 0, 6, 0x11111111);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCommandFillBuffer parameter, VkDeviceSize size, is not a multiple of 4'";
        m_errorMonitor->DumpFailureMsgs();
    }
    EndCommandBuffer();
}

#endif // DEVICE_LIMITS_TESTS

#if IMAGE_TESTS
TEST_F(VkLayerTest, InvalidImageView)
{
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCreateImageView called with baseMipLevel 10 ");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create an image and try to create a view with bad baseMipLevel
    VkImage               image;

    const VkFormat tex_format      = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t  tex_width       = 32;
    const int32_t  tex_height      = 32;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = tex_format;
        image_create_info.extent.width = tex_width;
        image_create_info.extent.height = tex_height;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        image_create_info.flags = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &image);
    ASSERT_VK_SUCCESS(err);

    VkImageViewCreateInfo image_view_create_info = {};
        image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_view_create_info.image = image;
        image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        image_view_create_info.format = tex_format;
        image_view_create_info.subresourceRange.layerCount = 1;
        image_view_create_info.subresourceRange.baseMipLevel = 10; // cause an error
        image_view_create_info.subresourceRange.levelCount = 1;
        image_view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageView view;
    err = vkCreateImageView(m_device->device(), &image_view_create_info, NULL, &view);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCreateImageView called with baseMipLevel 10...'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, InvalidImageViewAspect)
{
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCreateImageView: Color image formats must have ONLY the VK_IMAGE_ASPECT_COLOR_BIT set");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create an image and try to create a view with an invalid aspectMask
    VkImage               image;

    const VkFormat tex_format      = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t  tex_width       = 32;
    const int32_t  tex_height      = 32;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = tex_format;
        image_create_info.extent.width = tex_width;
        image_create_info.extent.height = tex_height;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        image_create_info.flags = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &image);
    ASSERT_VK_SUCCESS(err);

    VkImageViewCreateInfo image_view_create_info = {};
        image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_view_create_info.image = image;
        image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        image_view_create_info.format = tex_format;
        image_view_create_info.subresourceRange.baseMipLevel = 0;
        image_view_create_info.subresourceRange.levelCount = 1;
        // Cause an error by setting an invalid image aspect
        image_view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_METADATA_BIT;

    VkImageView view;
    err = vkCreateImageView(m_device->device(), &image_view_create_info, NULL, &view);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'VkCreateImageView: Color image formats must have ...'";
        m_errorMonitor->DumpFailureMsgs();
    }
}

TEST_F(VkLayerTest, CopyImageTypeMismatch)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCmdCopyImage called with unmatched source and dest image types");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create two images of different types and try to copy between them
    VkImage               srcImage;
    VkImage               dstImage;
    VkDeviceMemory        srcMem;
    VkDeviceMemory        destMem;
    VkMemoryRequirements  memReqs;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_create_info.extent.width = 32;
        image_create_info.extent.height = 32;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_create_info.flags = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &srcImage);
    ASSERT_VK_SUCCESS(err);

        image_create_info.imageType = VK_IMAGE_TYPE_1D;
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &dstImage);
    ASSERT_VK_SUCCESS(err);

    // Allocate memory
    VkMemoryAllocateInfo memAlloc = {};
        memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        memAlloc.pNext = NULL;
        memAlloc.allocationSize = 0;
        memAlloc.memoryTypeIndex = 0;

    vkGetImageMemoryRequirements(m_device->device(), srcImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &srcMem);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(), dstImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_VK_SUCCESS(err);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &destMem);
    ASSERT_VK_SUCCESS(err);

    err = vkBindImageMemory(m_device->device(), srcImage, srcMem, 0);
    ASSERT_VK_SUCCESS(err);
    err = vkBindImageMemory(m_device->device(), dstImage, destMem, 0);
    ASSERT_VK_SUCCESS(err);

    BeginCommandBuffer();
    VkImageCopy copyRegion;
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.mipLevel = 0;
    copyRegion.srcSubresource.baseArrayLayer = 0;
    copyRegion.srcSubresource.layerCount = 0;
    copyRegion.srcOffset.x = 0;
    copyRegion.srcOffset.y = 0;
    copyRegion.srcOffset.z = 0;
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.mipLevel = 0;
    copyRegion.dstSubresource.baseArrayLayer = 0;
    copyRegion.dstSubresource.layerCount = 0;
    copyRegion.dstOffset.x = 0;
    copyRegion.dstOffset.y = 0;
    copyRegion.dstOffset.z = 0;
    copyRegion.extent.width = 1;
    copyRegion.extent.height = 1;
    copyRegion.extent.depth = 1;
    m_commandBuffer->CopyImage(srcImage, VK_IMAGE_LAYOUT_GENERAL, dstImage, VK_IMAGE_LAYOUT_GENERAL, 1, &copyRegion);
    EndCommandBuffer();

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCmdCopyImage called with unmatched source and dest image types'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), srcImage, NULL);
    vkDestroyImage(m_device->device(), dstImage, NULL);
    vkFreeMemory(m_device->device(), srcMem, NULL);
    vkFreeMemory(m_device->device(), destMem, NULL);
}

TEST_F(VkLayerTest, CopyImageFormatSizeMismatch)
{
    // TODO : Create two images with different format sizes and vkCmdCopyImage between them
}

TEST_F(VkLayerTest, CopyImageDepthStencilFormatMismatch)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCmdCopyImage called with unmatched source and dest image types");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create two images of different types and try to copy between them
    VkImage               srcImage;
    VkImage               dstImage;
    VkDeviceMemory        srcMem;
    VkDeviceMemory        destMem;
    VkMemoryRequirements  memReqs;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_create_info.extent.width = 32;
        image_create_info.extent.height = 32;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_create_info.flags = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &srcImage);
    ASSERT_VK_SUCCESS(err);

        image_create_info.imageType = VK_IMAGE_TYPE_1D;
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &dstImage);
    ASSERT_VK_SUCCESS(err);

    // Allocate memory
    VkMemoryAllocateInfo memAlloc = {};
        memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        memAlloc.pNext = NULL;
        memAlloc.allocationSize = 0;
        memAlloc.memoryTypeIndex = 0;

    vkGetImageMemoryRequirements(m_device->device(), srcImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &srcMem);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(), dstImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &destMem);
    ASSERT_VK_SUCCESS(err);

    err = vkBindImageMemory(m_device->device(), srcImage, srcMem, 0);
    ASSERT_VK_SUCCESS(err);
    err = vkBindImageMemory(m_device->device(), dstImage, destMem, 0);
    ASSERT_VK_SUCCESS(err);

    BeginCommandBuffer();
    VkImageCopy copyRegion;
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.mipLevel = 0;
    copyRegion.srcSubresource.baseArrayLayer = 0;
    copyRegion.srcSubresource.layerCount = 0;
    copyRegion.srcOffset.x = 0;
    copyRegion.srcOffset.y = 0;
    copyRegion.srcOffset.z = 0;
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.mipLevel = 0;
    copyRegion.dstSubresource.baseArrayLayer = 0;
    copyRegion.dstSubresource.layerCount = 0;
    copyRegion.dstOffset.x = 0;
    copyRegion.dstOffset.y = 0;
    copyRegion.dstOffset.z = 0;
    copyRegion.extent.width = 1;
    copyRegion.extent.height = 1;
    copyRegion.extent.depth = 1;
    m_commandBuffer->CopyImage(srcImage, VK_IMAGE_LAYOUT_GENERAL, dstImage, VK_IMAGE_LAYOUT_GENERAL, 1, &copyRegion);
    EndCommandBuffer();

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCmdCopyImage called with unmatched source and dest image types'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), srcImage, NULL);
    vkDestroyImage(m_device->device(), dstImage, NULL);
    vkFreeMemory(m_device->device(), srcMem, NULL);
    vkFreeMemory(m_device->device(), destMem, NULL);
}

TEST_F(VkLayerTest, ResolveImageLowSampleCount)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCmdResolveImage called with source sample count less than 2.");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create two images of sample count 1 and try to Resolve between them
    VkImage               srcImage;
    VkImage               dstImage;
    VkDeviceMemory        srcMem;
    VkDeviceMemory        destMem;
    VkMemoryRequirements  memReqs;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_create_info.extent.width = 32;
        image_create_info.extent.height = 1;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_create_info.flags = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &srcImage);
    ASSERT_VK_SUCCESS(err);

        image_create_info.imageType = VK_IMAGE_TYPE_1D;
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &dstImage);
    ASSERT_VK_SUCCESS(err);

    // Allocate memory
    VkMemoryAllocateInfo memAlloc = {};
        memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        memAlloc.pNext = NULL;
        memAlloc.allocationSize = 0;
        memAlloc.memoryTypeIndex = 0;

    vkGetImageMemoryRequirements(m_device->device(), srcImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &srcMem);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(), dstImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &destMem);
    ASSERT_VK_SUCCESS(err);

    err = vkBindImageMemory(m_device->device(), srcImage, srcMem, 0);
    ASSERT_VK_SUCCESS(err);
    err = vkBindImageMemory(m_device->device(), dstImage, destMem, 0);
    ASSERT_VK_SUCCESS(err);

    BeginCommandBuffer();
    // Need memory barrier to VK_IMAGE_LAYOUT_GENERAL for source and dest?
    //VK_IMAGE_LAYOUT_UNDEFINED = 0,
    //VK_IMAGE_LAYOUT_GENERAL = 1,
    VkImageResolve resolveRegion;
    resolveRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveRegion.srcSubresource.mipLevel = 0;
    resolveRegion.srcSubresource.baseArrayLayer = 0;
    resolveRegion.srcSubresource.layerCount = 0;
    resolveRegion.srcOffset.x = 0;
    resolveRegion.srcOffset.y = 0;
    resolveRegion.srcOffset.z = 0;
    resolveRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveRegion.dstSubresource.mipLevel = 0;
    resolveRegion.dstSubresource.baseArrayLayer = 0;
    resolveRegion.dstSubresource.layerCount = 0;
    resolveRegion.dstOffset.x = 0;
    resolveRegion.dstOffset.y = 0;
    resolveRegion.dstOffset.z = 0;
    resolveRegion.extent.width = 1;
    resolveRegion.extent.height = 1;
    resolveRegion.extent.depth = 1;
    m_commandBuffer->ResolveImage(srcImage, VK_IMAGE_LAYOUT_GENERAL, dstImage, VK_IMAGE_LAYOUT_GENERAL, 1, &resolveRegion);
    EndCommandBuffer();

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCmdResolveImage called with source sample count less than 2.'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), srcImage, NULL);
    vkDestroyImage(m_device->device(), dstImage, NULL);
    vkFreeMemory(m_device->device(), srcMem, NULL);
    vkFreeMemory(m_device->device(), destMem, NULL);
}

TEST_F(VkLayerTest, ResolveImageHighSampleCount)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCmdResolveImage called with dest sample count greater than 1.");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create two images of sample count 2 and try to Resolve between them
    VkImage               srcImage;
    VkImage               dstImage;
    VkDeviceMemory        srcMem;
    VkDeviceMemory        destMem;
    VkMemoryRequirements  memReqs;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_create_info.extent.width = 32;
        image_create_info.extent.height = 1;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 2;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        // Note: Some implementations expect color attachment usage for any multisample surface
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image_create_info.flags = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &srcImage);
    ASSERT_VK_SUCCESS(err);

        image_create_info.imageType = VK_IMAGE_TYPE_1D;
        // Note: Some implementations expect color attachment usage for any multisample surface
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &dstImage);
    ASSERT_VK_SUCCESS(err);

    // Allocate memory
    VkMemoryAllocateInfo memAlloc = {};
        memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        memAlloc.pNext = NULL;
        memAlloc.allocationSize = 0;
        memAlloc.memoryTypeIndex = 0;

    vkGetImageMemoryRequirements(m_device->device(), srcImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &srcMem);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(), dstImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &destMem);
    ASSERT_VK_SUCCESS(err);

    err = vkBindImageMemory(m_device->device(), srcImage, srcMem, 0);
    ASSERT_VK_SUCCESS(err);
    err = vkBindImageMemory(m_device->device(), dstImage, destMem, 0);
    ASSERT_VK_SUCCESS(err);

    BeginCommandBuffer();
    // Need memory barrier to VK_IMAGE_LAYOUT_GENERAL for source and dest?
    //VK_IMAGE_LAYOUT_UNDEFINED = 0,
    //VK_IMAGE_LAYOUT_GENERAL = 1,
    VkImageResolve resolveRegion;
    resolveRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveRegion.srcSubresource.mipLevel = 0;
    resolveRegion.srcSubresource.baseArrayLayer = 0;
    resolveRegion.srcSubresource.layerCount = 0;
    resolveRegion.srcOffset.x = 0;
    resolveRegion.srcOffset.y = 0;
    resolveRegion.srcOffset.z = 0;
    resolveRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveRegion.dstSubresource.mipLevel = 0;
    resolveRegion.dstSubresource.baseArrayLayer = 0;
    resolveRegion.dstSubresource.layerCount = 0;
    resolveRegion.dstOffset.x = 0;
    resolveRegion.dstOffset.y = 0;
    resolveRegion.dstOffset.z = 0;
    resolveRegion.extent.width = 1;
    resolveRegion.extent.height = 1;
    resolveRegion.extent.depth = 1;
    m_commandBuffer->ResolveImage(srcImage, VK_IMAGE_LAYOUT_GENERAL, dstImage, VK_IMAGE_LAYOUT_GENERAL, 1, &resolveRegion);
    EndCommandBuffer();

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCmdResolveImage called with dest sample count greater than 1.'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), srcImage, NULL);
    vkDestroyImage(m_device->device(), dstImage, NULL);
    vkFreeMemory(m_device->device(), srcMem, NULL);
    vkFreeMemory(m_device->device(), destMem, NULL);
}

TEST_F(VkLayerTest, ResolveImageFormatMismatch)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCmdResolveImage called with unmatched source and dest formats.");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create two images of different types and try to copy between them
    VkImage               srcImage;
    VkImage               dstImage;
    VkDeviceMemory        srcMem;
    VkDeviceMemory        destMem;
    VkMemoryRequirements  memReqs;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_create_info.extent.width = 32;
        image_create_info.extent.height = 1;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 2;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        // Note: Some implementations expect color attachment usage for any multisample surface
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image_create_info.flags = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &srcImage);
    ASSERT_VK_SUCCESS(err);

        // Set format to something other than source image
        image_create_info.format = VK_FORMAT_R32_SFLOAT;
        // Note: Some implementations expect color attachment usage for any multisample surface
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image_create_info.samples = 1;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &dstImage);
    ASSERT_VK_SUCCESS(err);

    // Allocate memory
    VkMemoryAllocateInfo memAlloc = {};
        memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        memAlloc.pNext = NULL;
        memAlloc.allocationSize = 0;
        memAlloc.memoryTypeIndex = 0;

    vkGetImageMemoryRequirements(m_device->device(), srcImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &srcMem);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(), dstImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &destMem);
    ASSERT_VK_SUCCESS(err);

    err = vkBindImageMemory(m_device->device(), srcImage, srcMem, 0);
    ASSERT_VK_SUCCESS(err);
    err = vkBindImageMemory(m_device->device(), dstImage, destMem, 0);
    ASSERT_VK_SUCCESS(err);

    BeginCommandBuffer();
    // Need memory barrier to VK_IMAGE_LAYOUT_GENERAL for source and dest?
    //VK_IMAGE_LAYOUT_UNDEFINED = 0,
    //VK_IMAGE_LAYOUT_GENERAL = 1,
    VkImageResolve resolveRegion;
    resolveRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveRegion.srcSubresource.mipLevel = 0;
    resolveRegion.srcSubresource.baseArrayLayer = 0;
    resolveRegion.srcSubresource.layerCount = 0;
    resolveRegion.srcOffset.x = 0;
    resolveRegion.srcOffset.y = 0;
    resolveRegion.srcOffset.z = 0;
    resolveRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveRegion.dstSubresource.mipLevel = 0;
    resolveRegion.dstSubresource.baseArrayLayer = 0;
    resolveRegion.dstSubresource.layerCount = 0;
    resolveRegion.dstOffset.x = 0;
    resolveRegion.dstOffset.y = 0;
    resolveRegion.dstOffset.z = 0;
    resolveRegion.extent.width = 1;
    resolveRegion.extent.height = 1;
    resolveRegion.extent.depth = 1;
    m_commandBuffer->ResolveImage(srcImage, VK_IMAGE_LAYOUT_GENERAL, dstImage, VK_IMAGE_LAYOUT_GENERAL, 1, &resolveRegion);
    EndCommandBuffer();

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCmdResolveImage called with unmatched source and dest formats.'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), srcImage, NULL);
    vkDestroyImage(m_device->device(), dstImage, NULL);
    vkFreeMemory(m_device->device(), srcMem, NULL);
    vkFreeMemory(m_device->device(), destMem, NULL);
}

TEST_F(VkLayerTest, ResolveImageTypeMismatch)
{
    VkResult        err;
    bool            pass;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "vkCmdResolveImage called with unmatched source and dest image types.");

    ASSERT_NO_FATAL_FAILURE(InitState());

    // Create two images of different types and try to copy between them
    VkImage               srcImage;
    VkImage               dstImage;
    VkDeviceMemory        srcMem;
    VkDeviceMemory        destMem;
    VkMemoryRequirements  memReqs;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_create_info.extent.width = 32;
        image_create_info.extent.height = 1;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 2;
        image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
        // Note: Some implementations expect color attachment usage for any multisample surface
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image_create_info.flags = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &srcImage);
    ASSERT_VK_SUCCESS(err);

        image_create_info.imageType = VK_IMAGE_TYPE_1D;
        // Note: Some implementations expect color attachment usage for any multisample surface
        image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image_create_info.samples = 1;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &dstImage);
    ASSERT_VK_SUCCESS(err);

    // Allocate memory
    VkMemoryAllocateInfo memAlloc = {};
        memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO;
        memAlloc.pNext = NULL;
        memAlloc.allocationSize = 0;
        memAlloc.memoryTypeIndex = 0;

    vkGetImageMemoryRequirements(m_device->device(), srcImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &srcMem);
    ASSERT_VK_SUCCESS(err);

    vkGetImageMemoryRequirements(m_device->device(), dstImage, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    pass = m_device->phy().set_memory_type(memReqs.memoryTypeBits, &memAlloc, 0);
    ASSERT_TRUE(pass);
    err = vkAllocateMemory(m_device->device(), &memAlloc, NULL, &destMem);
    ASSERT_VK_SUCCESS(err);

    err = vkBindImageMemory(m_device->device(), srcImage, srcMem, 0);
    ASSERT_VK_SUCCESS(err);
    err = vkBindImageMemory(m_device->device(), dstImage, destMem, 0);
    ASSERT_VK_SUCCESS(err);

    BeginCommandBuffer();
    // Need memory barrier to VK_IMAGE_LAYOUT_GENERAL for source and dest?
    //VK_IMAGE_LAYOUT_UNDEFINED = 0,
    //VK_IMAGE_LAYOUT_GENERAL = 1,
    VkImageResolve resolveRegion;
    resolveRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveRegion.srcSubresource.mipLevel = 0;
    resolveRegion.srcSubresource.baseArrayLayer = 0;
    resolveRegion.srcSubresource.layerCount = 0;
    resolveRegion.srcOffset.x = 0;
    resolveRegion.srcOffset.y = 0;
    resolveRegion.srcOffset.z = 0;
    resolveRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveRegion.dstSubresource.mipLevel = 0;
    resolveRegion.dstSubresource.baseArrayLayer = 0;
    resolveRegion.dstSubresource.layerCount = 0;
    resolveRegion.dstOffset.x = 0;
    resolveRegion.dstOffset.y = 0;
    resolveRegion.dstOffset.z = 0;
    resolveRegion.extent.width = 1;
    resolveRegion.extent.height = 1;
    resolveRegion.extent.depth = 1;
    m_commandBuffer->ResolveImage(srcImage, VK_IMAGE_LAYOUT_GENERAL, dstImage, VK_IMAGE_LAYOUT_GENERAL, 1, &resolveRegion);
    EndCommandBuffer();

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'vkCmdResolveImage called with unmatched source and dest image types.'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), srcImage, NULL);
    vkDestroyImage(m_device->device(), dstImage, NULL);
    vkFreeMemory(m_device->device(), srcMem, NULL);
    vkFreeMemory(m_device->device(), destMem, NULL);
}

TEST_F(VkLayerTest, DepthStencilImageViewWithColorAspectBitError)
{
    // Create a single Image descriptor and cause it to first hit an error due
    //  to using a DS format, then cause it to hit error due to COLOR_BIT not set in aspect
    // The image format check comes 2nd in validation so we trigger it first,
    //  then when we cause aspect fail next, bad format check will be preempted
    VkResult        err;

    m_errorMonitor->SetDesiredFailureMsg(VK_DBG_REPORT_ERROR_BIT,
        "Combination depth/stencil image formats can have only the ");

    ASSERT_NO_FATAL_FAILURE(InitState());

    VkDescriptorPoolSize ds_type_count = {};
        ds_type_count.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        ds_type_count.descriptorCount = 1;

    VkDescriptorPoolCreateInfo ds_pool_ci = {};
        ds_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ds_pool_ci.pNext = NULL;
        ds_pool_ci.maxSets = 1;
        ds_pool_ci.poolSizeCount = 1;
        ds_pool_ci.pPoolSizes = &ds_type_count;

    VkDescriptorPool ds_pool;
    err = vkCreateDescriptorPool(m_device->device(), &ds_pool_ci, NULL, &ds_pool);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        dsl_binding.arraySize = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_ALL;
        dsl_binding.pImmutableSamplers = NULL;

    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.pNext = NULL;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBinding = &dsl_binding;
    VkDescriptorSetLayout ds_layout;
    err = vkCreateDescriptorSetLayout(m_device->device(), &ds_layout_ci, NULL, &ds_layout);
    ASSERT_VK_SUCCESS(err);

    VkDescriptorSet descriptorSet;
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOC_INFO;
    alloc_info.setLayoutCount = 1;
    alloc_info.descriptorPool = ds_pool;
    alloc_info.pSetLayouts = &ds_layout;
    err = vkAllocateDescriptorSets(m_device->device(), &alloc_info, &descriptorSet);
    ASSERT_VK_SUCCESS(err);

    VkImage               image_bad;
    VkImage               image_good;
    // One bad format and one good format for Color attachment
    const VkFormat tex_format_bad  = VK_FORMAT_D32_SFLOAT_S8_UINT;
    const VkFormat tex_format_good = VK_FORMAT_B8G8R8A8_UNORM;
    const int32_t  tex_width       = 32;
    const int32_t  tex_height      = 32;

    VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.pNext = NULL;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.format = tex_format_bad;
        image_create_info.extent.width = tex_width;
        image_create_info.extent.height = tex_height;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.samples = 1;
        image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        image_create_info.flags = 0;

    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &image_bad);
    ASSERT_VK_SUCCESS(err);
    image_create_info.format = tex_format_good;
    image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    err = vkCreateImage(m_device->device(), &image_create_info, NULL, &image_good);
    ASSERT_VK_SUCCESS(err);

    VkImageViewCreateInfo image_view_create_info = {};
        image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_view_create_info.image = image_bad;
        image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        image_view_create_info.format = tex_format_bad;
        image_view_create_info.subresourceRange.baseArrayLayer = 0;
        image_view_create_info.subresourceRange.baseMipLevel = 0;
        image_view_create_info.subresourceRange.layerCount = 1;
        image_view_create_info.subresourceRange.levelCount = 1;
        image_view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageView view;
    err = vkCreateImageView(m_device->device(), &image_view_create_info, NULL, &view);

    if (!m_errorMonitor->DesiredMsgFound()) {
        FAIL() << "Did not receive Error 'Combination depth-stencil image formats can have only the....'";
        m_errorMonitor->DumpFailureMsgs();
    }

    vkDestroyImage(m_device->device(), image_bad, NULL);
    vkDestroyImage(m_device->device(), image_good, NULL);
    vkDestroyDescriptorSetLayout(m_device->device(), ds_layout, NULL);
    vkDestroyDescriptorPool(m_device->device(), ds_pool, NULL);
}
#endif // IMAGE_TESTS

int main(int argc, char **argv) {
    int result;

    ::testing::InitGoogleTest(&argc, argv);
    VkTestFramework::InitArgs(&argc, argv);

    ::testing::AddGlobalTestEnvironment(new TestEnvironment);

    result = RUN_ALL_TESTS();

    VkTestFramework::Finish();
    return result;
}
