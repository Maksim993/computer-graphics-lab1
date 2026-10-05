#include "application.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <imgui.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace application {

namespace {

// ------------------------ Геометрия куба ------------------------
// Вершина хранит только позицию. Цвет вычисляется процедурно
// в vertex shader по локальным координатам вершины.
constexpr glm::vec3 cube_vertices[] = {
    {-1.0f, -1.0f, -1.0f},
    { 1.0f, -1.0f, -1.0f},
    { 1.0f,  1.0f, -1.0f},
    {-1.0f,  1.0f, -1.0f},
    {-1.0f, -1.0f,  1.0f},
    { 1.0f, -1.0f,  1.0f},
    { 1.0f,  1.0f,  1.0f},
    {-1.0f,  1.0f,  1.0f},
};

constexpr uint16_t cube_indices[] = {
    0, 2, 1,  0, 3, 2, // -Z
    4, 5, 6,  4, 6, 7, // +Z
    0, 4, 7,  0, 7, 3, // -X
    1, 2, 6,  1, 6, 5, // +X
    0, 1, 5,  0, 5, 4, // -Y
    3, 7, 6,  3, 6, 2, // +Y
};

struct PushConstants {
    glm::mat4 mvp;
    glm::vec4 tint;
};

// ------------------------ Vulkan ресурсы ------------------------
VkBuffer vertex_buffer = VK_NULL_HANDLE;
VmaAllocation vertex_buffer_allocation = VK_NULL_HANDLE;

VkBuffer index_buffer = VK_NULL_HANDLE;
VmaAllocation index_buffer_allocation = VK_NULL_HANDLE;

VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;

// ------------------------ Состояние сцены ------------------------
glm::vec3 object_position{0.0f, 0.0f, 0.0f};
glm::vec3 object_rotation{0.0f, 0.0f, 0.0f};
glm::vec3 object_scale{1.0f, 1.0f, 1.0f};

glm::vec4 object_tint{1.0f, 1.0f, 1.0f, 1.0f};

// Проекция.
int projection_mode = 0; // 0 = Perspective, 1 = Orthographic
float fov_deg = 60.0f;
float ortho_size = 3.0f;
float near_plane = 0.1f;
float far_plane = 100.0f;

// Анимация.
bool animation_playing = true;
float animation_speed = 1.0f;
float animation_time = 0.0f;

// Сложная 3D-траектория: меняющийся по радиусу винтовой путь.
bool trajectory_enabled = true;
float trajectory_radius = 1.25f;
float trajectory_height = 1.25f;
float trajectory_angular_frequency = 1.0f;
float trajectory_vertical_frequency = 1.5f;
float trajectory_phase = 0.0f;

// Автоматическое вращение.
bool auto_rotate = true;
glm::vec3 rotation_speed{20.0f, 45.0f, 25.0f};

// Камера.
glm::vec3 camera_position{0.0f, 0.0f, 5.0f};
float camera_yaw = 0.0f;
float camera_pitch = 0.0f;

constexpr float pi = 3.14159265358979323846f;

// ------------------------ Работа с файлами ------------------------
std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file) {
        std::cerr << "Failed to open shader file: " << path << '\n';
        return {};
    }

    const std::streamsize size = file.tellg();

    if (size <= 0) {
        std::cerr << "Shader file is empty: " << path << '\n';
        return {};
    }

    std::vector<char> buffer(static_cast<size_t>(size));
    file.seekg(0);
    file.read(buffer.data(), size);

    if (!file) {
        std::cerr << "Failed to read shader file: " << path << '\n';
        return {};
    }

    return buffer;
}

VkShaderModule createShaderModule(VkDevice device, const std::string& path) {
    const auto code = readFile(path);

    if (code.empty() || code.size() % sizeof(uint32_t) != 0) {
        std::cerr << "Invalid SPIR-V file: " << path << '\n';
        return VK_NULL_HANDLE;
    }

    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size(),
        .pCode = reinterpret_cast<const uint32_t*>(code.data()),
    };

    VkShaderModule module = VK_NULL_HANDLE;

    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        std::cerr << "Failed to create shader module: " << path << '\n';
        return VK_NULL_HANDLE;
    }

    return module;
}

// ------------------------ Буферы ------------------------
bool createBufferWithData(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    const void* data,
    VkBuffer& out_buffer,
    VmaAllocation& out_allocation
) {
    auto& ctx = graphics::internal::context;

    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    const VmaAllocationCreateInfo alloc_info{
        .flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };

    VmaAllocationInfo mapped{};

    if (vmaCreateBuffer(
            ctx.allocator,
            &buffer_info,
            &alloc_info,
            &out_buffer,
            &out_allocation,
            &mapped) != VK_SUCCESS) {
        return false;
    }

    std::memcpy(mapped.pMappedData, data, static_cast<size_t>(size));
    vmaFlushAllocation(ctx.allocator, out_allocation, 0, size);

    return true;
}

// ------------------------ Pipeline ------------------------
bool createPipeline() {
    auto& ctx = graphics::internal::context;

    const std::string shader_dir = SHADER_DIR;
    VkShaderModule vert = createShaderModule(
        ctx.device,
        shader_dir + "/cube.vert.spv"
    );
    VkShaderModule frag = createShaderModule(
        ctx.device,
        shader_dir + "/cube.frag.spv"
    );

    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert != VK_NULL_HANDLE) {
            vkDestroyShaderModule(ctx.device, vert, nullptr);
        }
        if (frag != VK_NULL_HANDLE) {
            vkDestroyShaderModule(ctx.device, frag, nullptr);
        }
        return false;
    }

    const VkPipelineShaderStageCreateInfo stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
        },
    };

    const VkVertexInputBindingDescription vertex_binding{
        .binding = 0,
        .stride = sizeof(glm::vec3),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    const VkVertexInputAttributeDescription vertex_attribute{
        .location = 0,
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT,
        .offset = 0,
    };

    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertex_binding,
        .vertexAttributeDescriptionCount = 1,
        .pVertexAttributeDescriptions = &vertex_attribute,
    };

    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    const VkPipelineViewportStateCreateInfo viewport_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    const VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    const VkPipelineDepthStencilStateCreateInfo depth_stencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };

    const VkPipelineColorBlendAttachmentState color_blend_attachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT,
    };

    const VkPipelineColorBlendStateCreateInfo color_blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    const VkPipelineDynamicStateCreateInfo dynamic_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    const VkPushConstantRange push_range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };

    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };

    if (vkCreatePipelineLayout(
            ctx.device,
            &layout_info,
            nullptr,
            &pipeline_layout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout\n";
        vkDestroyShaderModule(ctx.device, vert, nullptr);
        vkDestroyShaderModule(ctx.device, frag, nullptr);
        return false;
    }

    const VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .layout = pipeline_layout,
        .renderPass = ctx.render_pass,
        .subpass = 0,
    };

    if (vkCreateGraphicsPipelines(
            ctx.device,
            VK_NULL_HANDLE,
            1,
            &pipeline_info,
            nullptr,
            &pipeline) != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline\n";
        vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
        vkDestroyShaderModule(ctx.device, vert, nullptr);
        vkDestroyShaderModule(ctx.device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);

    return true;
}

// ------------------------ Математика сцены ------------------------
glm::vec3 currentPosition() {
    glm::vec3 position = object_position;

    if (!trajectory_enabled) {
        return position;
    }

    // Нестандартная 3D-траектория:
    // радиус слегка меняется со временем, а Z движется с другой частотой.
    const float t = animation_time;
    const float angle = 2.0f * pi * trajectory_angular_frequency * t + trajectory_phase;
    const float changing_radius =
        trajectory_radius * (0.75f + 0.25f * std::sin(angle * 0.5f));

    position.x += changing_radius * std::cos(angle);
    position.y += changing_radius * std::sin(angle);
    position.z += trajectory_height *
                  std::sin(2.0f * pi * trajectory_vertical_frequency * t + trajectory_phase);

    return position;
}

glm::mat4 buildModelMatrix() {
    glm::mat4 model(1.0f);

    model = glm::translate(model, currentPosition());

    model = glm::rotate(
        model,
        glm::radians(object_rotation.y),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );

    model = glm::rotate(
        model,
        glm::radians(object_rotation.x),
        glm::vec3(1.0f, 0.0f, 0.0f)
    );

    model = glm::rotate(
        model,
        glm::radians(object_rotation.z),
        glm::vec3(0.0f, 0.0f, 1.0f)
    );

    model = glm::scale(model, object_scale);

    return model;
}

glm::mat4 buildViewMatrix() {
    const float yaw = glm::radians(camera_yaw);
    const float pitch = glm::radians(camera_pitch);

    glm::vec3 forward{
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch),
        -std::cos(pitch) * std::cos(yaw),
    };

    forward = glm::normalize(forward);

    const glm::vec3 target = camera_position + forward;

    return glm::lookAt(
        camera_position,
        target,
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
}

glm::mat4 buildProjectionMatrix(float aspect) {
    glm::mat4 projection;

    if (projection_mode == 0) {
        projection = glm::perspective(
            glm::radians(fov_deg),
            aspect,
            near_plane,
            far_plane
        );
    } else {
        projection = glm::ortho(
            -ortho_size * aspect,
             ortho_size * aspect,
            -ortho_size,
             ortho_size,
            near_plane,
            far_plane
        );
    }

    // Y-flip for Vulkan clip space.
    projection[1][1] *= -1.0f;

    return projection;
}

void resetScene() {
    projection_mode = 0;
    fov_deg = 60.0f;
    ortho_size = 3.0f;
    near_plane = 0.1f;
    far_plane = 100.0f;

    object_position = {0.0f, 0.0f, 0.0f};
    object_rotation = {0.0f, 0.0f, 0.0f};
    object_scale = {1.0f, 1.0f, 1.0f};
    object_tint = {1.0f, 1.0f, 1.0f, 1.0f};

    animation_playing = true;
    animation_speed = 1.0f;
    animation_time = 0.0f;

    trajectory_enabled = true;
    trajectory_radius = 1.25f;
    trajectory_height = 1.25f;
    trajectory_angular_frequency = 1.0f;
    trajectory_vertical_frequency = 1.5f;
    trajectory_phase = 0.0f;

    auto_rotate = true;
    rotation_speed = {20.0f, 45.0f, 25.0f};

    camera_position = {0.0f, 0.0f, 5.0f};
    camera_yaw = 0.0f;
    camera_pitch = 0.0f;
}

} // namespace

bool initialize() {
    if (!createBufferWithData(
            sizeof(cube_vertices),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            cube_vertices,
            vertex_buffer,
            vertex_buffer_allocation)) {
        std::cerr << "Failed to create cube vertex buffer\n";
        return false;
    }

    if (!createBufferWithData(
            sizeof(cube_indices),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            cube_indices,
            index_buffer,
            index_buffer_allocation)) {
        std::cerr << "Failed to create cube index buffer\n";
        auto& ctx = graphics::internal::context;
        vmaDestroyBuffer(ctx.allocator, vertex_buffer, vertex_buffer_allocation);
        vertex_buffer = VK_NULL_HANDLE;
        vertex_buffer_allocation = VK_NULL_HANDLE;
        return false;
    }

    if (!createPipeline()) {
        auto& ctx = graphics::internal::context;
        vmaDestroyBuffer(ctx.allocator, index_buffer, index_buffer_allocation);
        vmaDestroyBuffer(ctx.allocator, vertex_buffer, vertex_buffer_allocation);
        index_buffer = VK_NULL_HANDLE;
        index_buffer_allocation = VK_NULL_HANDLE;
        vertex_buffer = VK_NULL_HANDLE;
        vertex_buffer_allocation = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void shutdown() {
    auto& ctx = graphics::internal::context;
    vkQueueWaitIdle(ctx.graphics_queue);

    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(ctx.device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }

    if (pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
    }

    if (index_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx.allocator, index_buffer, index_buffer_allocation);
        index_buffer = VK_NULL_HANDLE;
        index_buffer_allocation = VK_NULL_HANDLE;
    }

    if (vertex_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx.allocator, vertex_buffer, vertex_buffer_allocation);
        vertex_buffer = VK_NULL_HANDLE;
        vertex_buffer_allocation = VK_NULL_HANDLE;
    }
}

void update(double time) {
    static double previous_time = time;
    float delta_time = static_cast<float>(time - previous_time);
    previous_time = time;

    // Защита от большого скачка времени после перетягивания окна/отладки.
    delta_time = glm::clamp(delta_time, 0.0f, 0.1f);

    if (animation_playing) {
        const float dt = delta_time * animation_speed;
        animation_time += dt;

        if (auto_rotate) {
            object_rotation += rotation_speed * dt;
        }
    }

    ImGui::Begin("CG Lab 1 - Maxim");

    ImGui::Text("Variant 1: Cube");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Projection", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* projection_items[] = {
            "Perspective",
            "Orthographic"
        };

        ImGui::Combo(
            "Type",
            &projection_mode,
            projection_items,
            2
        );

        if (projection_mode == 0) {
            ImGui::SliderFloat(
                "FOV",
                &fov_deg,
                20.0f,
                120.0f,
                "%.1f deg"
            );
        } else {
            ImGui::SliderFloat(
                "Ortho size",
                &ortho_size,
                0.5f,
                15.0f
            );
        }

        ImGui::SliderFloat(
            "Near",
            &near_plane,
            0.01f,
            5.0f
        );

        ImGui::SliderFloat(
            "Far",
            &far_plane,
            10.0f,
            200.0f
        );
    }

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3(
            "Position",
            &object_position.x,
            0.01f
        );

        ImGui::DragFloat3(
            "Rotation (deg)",
            &object_rotation.x,
            1.0f,
            -360.0f,
            360.0f
        );

        ImGui::DragFloat3(
            "Scale",
            &object_scale.x,
            0.01f,
            0.05f,
            10.0f
        );
    }

    if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3(
            "Tint",
            &object_tint.x
        );

        ImGui::TextWrapped(
            "Tint is multiplied by a procedural color "
            "computed from the local cube position."
        );
    }

    if (ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button(animation_playing ? "Pause" : "Play")) {
            animation_playing = !animation_playing;
        }

        ImGui::SameLine();

        if (ImGui::Button("Reset time")) {
            animation_time = 0.0f;
        }

        ImGui::Text("Time: %.2f s", animation_time);

        ImGui::SliderFloat(
            "Speed",
            &animation_speed,
            0.0f,
            3.0f,
            "%.2fx"
        );

        ImGui::Separator();

        ImGui::Checkbox(
            "Trajectory",
            &trajectory_enabled
        );

        ImGui::SliderFloat(
            "Radius",
            &trajectory_radius,
            0.0f,
            4.0f
        );

        ImGui::SliderFloat(
            "Height",
            &trajectory_height,
            0.0f,
            4.0f
        );

        ImGui::SliderFloat(
            "Angular freq",
            &trajectory_angular_frequency,
            0.1f,
            3.0f
        );

        ImGui::SliderFloat(
            "Vertical freq",
            &trajectory_vertical_frequency,
            0.1f,
            4.0f
        );

        ImGui::SliderFloat(
            "Phase",
            &trajectory_phase,
            -pi,
            pi
        );

        ImGui::Separator();

        ImGui::Checkbox(
            "Auto rotation",
            &auto_rotate
        );

        ImGui::SliderFloat3(
            "Rotation speed",
            &rotation_speed.x,
            -180.0f,
            180.0f,
            "%.1f deg/s"
        );
    }

    if (ImGui::CollapsingHeader("Camera")) {
        ImGui::DragFloat3(
            "Camera position",
            &camera_position.x,
            0.05f
        );

        ImGui::SliderFloat(
            "Yaw",
            &camera_yaw,
            -180.0f,
            180.0f
        );

        ImGui::SliderFloat(
            "Pitch",
            &camera_pitch,
            -89.0f,
            89.0f
        );
    }

    if (ImGui::Button("Reset all")) {
        resetScene();
    }

    ImGui::End();
}

void render(const graphics::internal::FrameData& fd) {
    auto& ctx = graphics::internal::context;
    const VkCommandBuffer cmd = fd.command_buffer;

    vkResetCommandBuffer(cmd, 0);

    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    vkBeginCommandBuffer(cmd, &begin_info);

    const VkClearValue clear_values[] = {
        {
            .color = {{0.035f, 0.045f, 0.065f, 1.0f}}
        },
        {
            .depthStencil = {1.0f, 0}
        },
    };

    const VkRenderPassBeginInfo render_pass_begin{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = ctx.render_pass,
        .framebuffer = fd.framebuffer,
        .renderArea = {
            .offset = {0, 0},
            .extent = ctx.swapchain_extent,
        },
        .clearValueCount = 2,
        .pClearValues = clear_values,
    };

    vkCmdBeginRenderPass(
        cmd,
        &render_pass_begin,
        VK_SUBPASS_CONTENTS_INLINE
    );

    const VkViewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(ctx.swapchain_extent.width),
        .height = static_cast<float>(ctx.swapchain_extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const VkRect2D scissor{
        .offset = {0, 0},
        .extent = ctx.swapchain_extent,
    };

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline
    );

    const VkDeviceSize vertex_offset = 0;

    vkCmdBindVertexBuffers(
        cmd,
        0,
        1,
        &vertex_buffer,
        &vertex_offset
    );

    vkCmdBindIndexBuffer(
        cmd,
        index_buffer,
        0,
        VK_INDEX_TYPE_UINT16
    );

    const float aspect =
        static_cast<float>(ctx.swapchain_extent.width) /
        static_cast<float>(ctx.swapchain_extent.height);

    const PushConstants push_constants{
        .mvp =
            buildProjectionMatrix(aspect) *
            buildViewMatrix() *
            buildModelMatrix(),
        .tint = object_tint,
    };

    vkCmdPushConstants(
        cmd,
        pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(PushConstants),
        &push_constants
    );

    vkCmdDrawIndexed(
        cmd,
        static_cast<uint32_t>(sizeof(cube_indices) / sizeof(cube_indices[0])),
        1,
        0,
        0,
        0
    );

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

} // namespace application
