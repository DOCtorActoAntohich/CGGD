#include "dx12_renderer.h"

#include "utils/com_error_handler.h"
#include "utils/window.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <filesystem>


void cg::renderer::dx12_renderer::init()
{
    model = std::make_shared<cg::world::model>();
    model->load_obj(settings->model_path);

    camera = std::make_shared<cg::world::camera>();
    camera->set_width(static_cast<float>(settings->width));
    camera->set_height(static_cast<float>(settings->height));
    camera->set_position(float3{
        settings->camera_position[0],
        settings->camera_position[1],
        settings->camera_position[2],
    });
    camera->set_theta(settings->camera_theta);
    camera->set_phi(settings->camera_phi);
    camera->set_angle_of_view(settings->camera_angle_of_view);
    camera->set_z_near(settings->camera_z_near);
    camera->set_z_far(settings->camera_z_far);

    view_port = CD3DX12_VIEWPORT(
        0.0f, 0.0f,
        static_cast<float>(settings->width),
        static_cast<float>(settings->height)
    );
    scissor_rect = CD3DX12_RECT(
        0, 0,
        static_cast<LONG>(settings->width),
        static_cast<LONG>(settings->height)
    );

    world_view_projection =
        camera->get_dxm_view_matrix() *
        camera->get_dxm_projection_matrix();

    load_pipeline();
    load_assets();
}

void cg::renderer::dx12_renderer::destroy()
{
    wait_for_gpu();
    CloseHandle(fence_event);
}

void cg::renderer::dx12_renderer::update()
{
    world_view_projection =
        camera->get_dxm_view_matrix() *
        camera->get_dxm_projection_matrix();

    // Update constant buffer cos it depends on volatile data.
    memcpy(
        constant_buffer_data_begin,
        &world_view_projection,
        sizeof(world_view_projection)
    );
}

void cg::renderer::dx12_renderer::render()
{
    populate_command_list();

    ID3D12CommandList* command_lists[] = { command_list.Get() };
    command_queue->ExecuteCommandLists(
        _countof(command_lists), command_lists
    );

    THROW_IF_FAILED(swap_chain->Present(0, 0));

    move_to_next_frame();
}

void cg::renderer::dx12_renderer::load_pipeline()
{
    // Enable validation layer.
    UINT dxgi_factory_flags = 0;
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug_controller;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
        debug_controller->EnableDebugLayer();
        dxgi_factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    // Enumerate hardware adapters.
    ComPtr<IDXGIFactory4> dxgi_factory;
    THROW_IF_FAILED(
        CreateDXGIFactory2(dxgi_factory_flags, IID_PPV_ARGS(&dxgi_factory))
    );

    ComPtr<IDXGIAdapter1> hardware_adapter;
    dxgi_factory->EnumAdapters1(0, &hardware_adapter);

#ifdef _DEBUG
    DXGI_ADAPTER_DESC adapter_desc = {};
    hardware_adapter->GetDesc(&adapter_desc);
    OutputDebugString(adapter_desc.Description);
    OutputDebugStringA("\n");
#endif

    // Create a device object.
    THROW_IF_FAILED(D3D12CreateDevice(
        hardware_adapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&device)
    ));

    // Create a command queue.
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    THROW_IF_FAILED(
        device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue))
    );

    // Create a swap chain and bind it to the window.
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    swap_chain_desc.BufferCount = frame_number;
    swap_chain_desc.Width  = settings->width;
    swap_chain_desc.Height = settings->height;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swap_chain_desc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> temp_swap_chain;
    THROW_IF_FAILED(dxgi_factory->CreateSwapChainForHwnd(
        command_queue.Get(),
        cg::utils::window::get_hwnd(),
        &swap_chain_desc,
        nullptr,
        nullptr,
        &temp_swap_chain
    ));

    THROW_IF_FAILED(dxgi_factory->MakeWindowAssociation(
        cg::utils::window::get_hwnd(),
        DXGI_MWA_NO_ALT_ENTER
    ));

    THROW_IF_FAILED(temp_swap_chain.As(&swap_chain));
    frame_index = swap_chain->GetCurrentBackBufferIndex();

    // Create descriptor heap for render target views.
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = frame_number;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    THROW_IF_FAILED(device->CreateDescriptorHeap(
        &rtv_heap_desc,
        IID_PPV_ARGS(&rtv_heap)
    ));
    rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV
    );

    // Create render target views for render target.
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_handle(rtv_heap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < frame_number; ++i) {
        THROW_IF_FAILED(
            swap_chain->GetBuffer(i, IID_PPV_ARGS(&render_targets[i]))
        );
        device->CreateRenderTargetView(render_targets[i].Get(), nullptr, rtv_handle);
        rtv_handle.Offset(1, rtv_descriptor_size);

        auto name = std::wstring(L"Render target ") + std::to_wstring(i);
        render_targets[i]->SetName(name.c_str());
    }

    // Create descriptor heap for constant buffer.
    D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_desc = {};
    cbv_heap_desc.NumDescriptors = 1;
    cbv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    THROW_IF_FAILED(device->CreateDescriptorHeap(
        &cbv_heap_desc,
        IID_PPV_ARGS(&cbv_heap)
    ));

    // Create command allocators.
    for (size_t i = 0; i < frame_number; ++i) {
        THROW_IF_FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&command_allocators[i])
        ));
    }
}

void cg::renderer::dx12_renderer::load_assets()
{
    // Create descriptor table.
    CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
    CD3DX12_ROOT_PARAMETER1 root_parameters[1];
    ranges[0].Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
        1, 0, 0,
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC
    );
    root_parameters[0].InitAsDescriptorTable(
        1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX
    );

    // Create a root signature.
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rs_feature_data = {};
    rs_feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(device->CheckFeatureSupport(
        D3D12_FEATURE_ROOT_SIGNATURE, &rs_feature_data, sizeof(rs_feature_data)))) {
        rs_feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }
    D3D12_ROOT_SIGNATURE_FLAGS rs_flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_descriptor;
    rs_descriptor.Init_1_1(
        _countof(root_parameters),
        root_parameters,
        0,
        nullptr,
        rs_flags
    );
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;

    HRESULT rs_result = D3DX12SerializeVersionedRootSignature(
        &rs_descriptor,
        rs_feature_data.HighestVersion,
        &signature,
        &error
    );
    if (FAILED(rs_result)) {
        OutputDebugStringA((char*)error->GetBufferPointer());
        THROW_IF_FAILED(rs_result);
    }
    THROW_IF_FAILED(device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&root_signature)
    ));

    // Shader compilation.
    WCHAR path_buffer[MAX_PATH] = {0};
    GetModuleFileName(NULL, path_buffer, MAX_PATH);
    auto shader_path = std::filesystem::path(path_buffer).parent_path() / "shaders.hlsl";
    if (!std::filesystem::exists(shader_path)) {
        auto msg = std::string("File does not exist: ") + shader_path.string();
        OutputDebugStringA(msg.c_str());
        THROW_IF_FAILED(E_FAIL);
    }

    ComPtr<ID3D10Blob> vertex_shader;
    ComPtr<ID3D10Blob> pixel_shader;
    UINT compile_flags = 0;
#if _DEBUG
    compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT vertex_shaders_compilation_result = D3DCompileFromFile(
        shader_path.wstring().c_str(),
        nullptr,
        nullptr,
        "VSMain",
        "vs_5_0",
        compile_flags,
        0,
        &vertex_shader,
        &error
    );
    if (FAILED(vertex_shaders_compilation_result)) {
        OutputDebugStringA((char*)error->GetBufferPointer());
        THROW_IF_FAILED(vertex_shaders_compilation_result);
    }

    HRESULT pixel_shaders_compilation_result = D3DCompileFromFile(
        shader_path.wstring().c_str(),
        nullptr,
        nullptr,
        "PSMain",
        "ps_5_0",
        compile_flags,
        0,
        &pixel_shader,
        &error
    );
    if (FAILED(pixel_shaders_compilation_result)) {
        OutputDebugStringA((char*)error->GetBufferPointer());
        THROW_IF_FAILED(pixel_shaders_compilation_result);
    }

    // Create a Pipeline State Object.
    D3D12_INPUT_ELEMENT_DESC inputs_descriptors[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,
            0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT,
            0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT,
            0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 1, DXGI_FORMAT_R32G32B32_FLOAT,
            0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 2, DXGI_FORMAT_R32G32B32_FLOAT,
            0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,
            0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.InputLayout            = { inputs_descriptors, _countof(inputs_descriptors) };
    pso_desc.pRootSignature         = root_signature.Get();
    pso_desc.VS                     = CD3DX12_SHADER_BYTECODE(vertex_shader.Get());
    pso_desc.PS                     = CD3DX12_SHADER_BYTECODE(pixel_shader.Get());
    pso_desc.RasterizerState        = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso_desc.BlendState             = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso_desc.SampleMask             = UINT_MAX;
    pso_desc.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets       = 1;
    pso_desc.RTVFormats[0]          = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count       = 1;
    pso_desc.DepthStencilState.DepthEnable   = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.RasterizerState.FrontCounterClockwise = TRUE;

    THROW_IF_FAILED(device->CreateGraphicsPipelineState(
        &pso_desc,
        IID_PPV_ARGS(&pipeline_state)
    ));

    // Create a command list.
    THROW_IF_FAILED(device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        command_allocators[0].Get(),
        pipeline_state.Get(),
        IID_PPV_ARGS(&command_list)
    ));


    // Vertex buffers.
    vertex_buffers.resize(model->get_vertex_buffers().size());
    upload_vertex_buffers.resize(model->get_vertex_buffers().size());
    vertex_buffer_views.resize(model->get_vertex_buffers().size());
    for (size_t s = 0; s < model->get_vertex_buffers().size(); ++s) {
        auto vertex_buffer_data = model->get_vertex_buffers()[s];
        const UINT vertex_buffer_size = static_cast<UINT>(
            vertex_buffer_data->get_size_in_bytes()
        );

        THROW_IF_FAILED(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(
                vertex_buffer_size
            ),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&upload_vertex_buffers[s])
        ));
        THROW_IF_FAILED(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(
                vertex_buffer_size
            ),
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&vertex_buffers[s])
        ));

        auto vertex_buffer_name = std::wstring(L"Vertex buffer ") + std::to_wstring(s);
        vertex_buffers[s]->SetName(vertex_buffer_name.c_str());

        // Copy vertex data. DELETED
        /*UINT8* vertex_data_begin = nullptr;
        CD3DX12_RANGE read_range(0, 0);
        THROW_IF_FAILED(vertex_buffers[s]->Map(
            0,
            &read_range,
            reinterpret_cast<void**>(&vertex_data_begin)
        ));
        memcpy(
            vertex_data_begin,
            vertex_buffer_data->get_data(),
            vertex_buffer_size
        );
        vertex_buffers[s]->Unmap(0, nullptr);*/

        // Copy vertex data.
        D3D12_SUBRESOURCE_DATA vertex_data{};
        vertex_data.pData      = vertex_buffer_data->get_data();
        vertex_data.RowPitch   = vertex_buffer_size;
        vertex_data.SlicePitch = vertex_buffer_size;
        UpdateSubresources(
            command_list.Get(),
            vertex_buffers[s].Get(),
            upload_vertex_buffers[s].Get(),
            0, 0, 1, &vertex_data
        );
        command_list->ResourceBarrier(
            1, &CD3DX12_RESOURCE_BARRIER::Transition(
                vertex_buffers[s].Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
            )
        );

        // Vertex buffer view definition.
        vertex_buffer_views[s].BufferLocation = vertex_buffers[s]->GetGPUVirtualAddress();
        vertex_buffer_views[s].SizeInBytes    = vertex_buffer_size;
        vertex_buffer_views[s].StrideInBytes  = sizeof(cg::vertex);
    }


    // Index buffers.
    index_buffers.resize(model->get_index_buffers().size());
    upload_index_buffers.resize(model->get_index_buffers().size());
    index_buffer_views.resize(model->get_index_buffers().size());
    for (size_t s = 0; s < model->get_index_buffers().size(); ++s) {
        auto index_buffer_data = model->get_index_buffers()[s];
        const UINT index_buffer_size = static_cast<UINT>(
            index_buffer_data->get_size_in_bytes()
        );

        THROW_IF_FAILED(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(
                index_buffer_size
            ),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&upload_index_buffers[s])
        ));
        THROW_IF_FAILED(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(
                index_buffer_size
            ),
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&index_buffers[s])
        ));

        auto index_buffer_name = std::wstring(L"Index buffer ") + std::to_wstring(s);
        vertex_buffers[s]->SetName(index_buffer_name.c_str());

        // Copy index data. DELETED
        /*UINT8* index_data_begin;
        CD3DX12_RANGE read_range(0, 0);
        THROW_IF_FAILED(index_buffers[s]->Map(
            0,
            &read_range,
            reinterpret_cast<void**>(&index_data_begin)
        ));
        memcpy(
            index_data_begin,
            index_buffer_data->get_data(),
            index_buffer_size
        );
        index_buffers[s]->Unmap(0, nullptr);*/

        // Copy index data.
        D3D12_SUBRESOURCE_DATA index_data{};
        index_data.pData      = index_buffer_data->get_data();
        index_data.RowPitch   = index_buffer_size;
        index_data.SlicePitch = index_buffer_size;

        UpdateSubresources(
            command_list.Get(),
            index_buffers[s].Get(),
            upload_index_buffers[s].Get(),
            0, 0, 1, &index_data
        );
        command_list->ResourceBarrier(
            1, &CD3DX12_RESOURCE_BARRIER::Transition(
                index_buffers[s].Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_INDEX_BUFFER
            )
        );

        // Index buffer view definition.
        index_buffer_views[s].BufferLocation = index_buffers[s]->GetGPUVirtualAddress();
        index_buffer_views[s].SizeInBytes    = index_buffer_size;
        index_buffer_views[s].Format         = DXGI_FORMAT_R32_UINT;
    }


    // Constant buffer.
    THROW_IF_FAILED(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(
            64 * 1024 // "Strict rules, should be aligned, 64 KB"
        ),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&constant_buffer)
    ));

    std::wstring constant_buffer_name(L"Constant buffer");
    constant_buffer->SetName(constant_buffer_name.c_str());

    CD3DX12_RANGE constant_buffer_read_range(0, 0);
    THROW_IF_FAILED(constant_buffer->Map(
        0,
        &constant_buffer_read_range,
        reinterpret_cast<void**>(&constant_buffer_data_begin)
    ));
    memcpy(
        constant_buffer_data_begin,
        &world_view_projection,
        sizeof(world_view_projection)
    );

    // Create a constant buffer view.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
    cbv_desc.SizeInBytes    = (sizeof(world_view_projection) + 255) & ~255; // To align.
    cbv_desc.BufferLocation = constant_buffer->GetGPUVirtualAddress();
    device->CreateConstantBufferView(
        &cbv_desc,
        cbv_heap->GetCPUDescriptorHandleForHeapStart()
    );


    // Execute a command list to upload resources.
    THROW_IF_FAILED(command_list->Close());
    ID3D12CommandList* command_lists[] = { command_list.Get() };
    command_queue->ExecuteCommandLists(
        _countof(command_lists), command_lists
    );

    // Create a fence.
    THROW_IF_FAILED(device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)
    ));
    fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fence_event == nullptr) {
        THROW_IF_FAILED(HRESULT_FROM_WIN32(GetLastError()));
    }

    wait_for_gpu();
}

void cg::renderer::dx12_renderer::populate_command_list()
{
    // Reset.
    THROW_IF_FAILED(command_allocators[frame_index]->Reset());
    THROW_IF_FAILED(command_list->Reset(
        command_allocators[frame_index].Get(),
        pipeline_state.Get()
    ));


    // Initial state.
    command_list->SetGraphicsRootSignature(root_signature.Get());
    ID3D12DescriptorHeap* heaps[] = { cbv_heap.Get() };
    command_list->SetDescriptorHeaps(_countof(heaps), heaps);
    command_list->SetGraphicsRootDescriptorTable(
        0, cbv_heap->GetGPUDescriptorHandleForHeapStart()
    );
    command_list->RSSetViewports(1, &view_port);
    command_list->RSSetScissorRects(1, &scissor_rect);

    command_list->ResourceBarrier(
        1,
        &CD3DX12_RESOURCE_BARRIER::Transition(
            render_targets[frame_index].Get(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET
        )
    );

    // Drawing.
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_handle(
        rtv_heap->GetCPUDescriptorHandleForHeapStart(),
        frame_index,
        rtv_descriptor_size
    );
    command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    const float clear_color[] = { 0.392f, 0.584f, 0.929f, 1.0f };
    command_list->ClearRenderTargetView(
        rtv_handle, clear_color, 0, nullptr
    );
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (size_t s = 0; s < model->get_vertex_buffers().size(); ++s) {
        command_list->IASetVertexBuffers(0, 1, &vertex_buffer_views[s]);
        command_list->IASetIndexBuffer(&index_buffer_views[s]);
        command_list->DrawIndexedInstanced(
            model->get_index_buffers()[s]->get_number_of_elements(),
            1, 0, 0, 0
        );
    }

    command_list->ResourceBarrier(
        1,
        &CD3DX12_RESOURCE_BARRIER::Transition(
            render_targets[frame_index].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT
        )
    );

    THROW_IF_FAILED(command_list->Close());
}


void cg::renderer::dx12_renderer::move_to_next_frame()
{
    const UINT64 current_fence_value = fence_values[frame_index];
    THROW_IF_FAILED(command_queue->Signal(
        fence.Get(), current_fence_value
    ));

    frame_index = swap_chain->GetCurrentBackBufferIndex();
    if (fence->GetCompletedValue() < fence_values[frame_index]) {
        THROW_IF_FAILED(fence->SetEventOnCompletion(
            fence_values[frame_index], fence_event
        ));
        WaitForSingleObjectEx(fence_event, INFINITE, FALSE);
    }
    fence_values[frame_index] = current_fence_value + 1;
}

void cg::renderer::dx12_renderer::wait_for_gpu()
{
    THROW_IF_FAILED(command_queue->Signal(
        fence.Get(), fence_values[frame_index]
    ));

    THROW_IF_FAILED(fence->SetEventOnCompletion(
        fence_values[frame_index], fence_event
    ));
    WaitForSingleObjectEx(fence_event, INFINITE, FALSE);

    ++fence_values[frame_index];
}
