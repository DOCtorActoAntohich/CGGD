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

    // Create depth buffer.
    CD3DX12_RESOURCE_DESC depth_buffer_desc(
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        0,
        settings->width, settings->height,
        1, 1,
        DXGI_FORMAT_D32_FLOAT,
        1, 0,
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
            D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
    );
    D3D12_CLEAR_VALUE depth_clear_value{}; // For optimization.
    depth_clear_value.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear_value.DepthStencil.Depth   = 1.0f;
    depth_clear_value.DepthStencil.Stencil = 0;

    THROW_IF_FAILED(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &depth_buffer_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depth_clear_value,
        IID_PPV_ARGS(&depth_buffer)
    ));

    depth_buffer->SetName(L"Depth buffer");


    // Create a descriptor heap for depth-stencil view.
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    THROW_IF_FAILED(device->CreateDescriptorHeap(
        &dsv_heap_desc,
        IID_PPV_ARGS(&dsv_heap)
    ));

    // Create a depth-stencil descriptor.
    device->CreateDepthStencilView(
        depth_buffer.Get(),
        nullptr,
        dsv_heap->GetCPUDescriptorHandleForHeapStart()
    );


    // Create descriptor heap for constant buffer.
    D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_desc = {};
    cbv_heap_desc.NumDescriptors =
        1 + static_cast<UINT>(model->get_per_shape_texture_files().size());
    cbv_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    THROW_IF_FAILED(device->CreateDescriptorHeap(
        &cbv_heap_desc,
        IID_PPV_ARGS(&cbv_srv_heap)
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
    constexpr int size = 2;
    CD3DX12_DESCRIPTOR_RANGE1 ranges[size];
    CD3DX12_ROOT_PARAMETER1 root_parameters[size];
    ranges[0].Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
        1, 0, 0,
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC
    );
    root_parameters[0].InitAsDescriptorTable(
        1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX
    );

    ranges[1].Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        1, 0, 0,
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC
    );
    root_parameters[1].InitAsDescriptorTable(
        1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL
    );


    // Create a root signature.
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rs_feature_data = {};
    rs_feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(device->CheckFeatureSupport(
        D3D12_FEATURE_ROOT_SIGNATURE, &rs_feature_data, sizeof(rs_feature_data)))) {
        rs_feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }
    D3D12_ROOT_SIGNATURE_FLAGS rs_flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_STATIC_SAMPLER_DESC sampler_desc{};
    sampler_desc.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler_desc.MinLOD           = 0;
    sampler_desc.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler_desc.MipLODBias       = 0;
    sampler_desc.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler_desc.Filter           = D3D12_FILTER_ANISOTROPIC;
    sampler_desc.MaxAnisotropy    = 16;
    sampler_desc.ShaderRegister   = 0;
    sampler_desc.RegisterSpace    = 0;
    sampler_desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_descriptor;
    rs_descriptor.Init_1_1(
        _countof(root_parameters),
        root_parameters,
        1,
        &sampler_desc,
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
    ComPtr<ID3D10Blob> pixel_shader_texture;

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

    HRESULT pixel_shader_texture_compilation_result = D3DCompileFromFile(
        shader_path.wstring().c_str(),
        nullptr,
        nullptr,
        "PSMain_texture",
        "ps_5_0",
        compile_flags,
        0,
        &pixel_shader_texture,
        &error
    );
    if (FAILED(pixel_shader_texture_compilation_result)) {
        OutputDebugStringA((char*)error->GetBufferPointer());
        THROW_IF_FAILED(pixel_shader_texture_compilation_result);
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
    pso_desc.DSVFormat              = DXGI_FORMAT_D32_FLOAT;
    pso_desc.SampleDesc.Count       = 1;

    pso_desc.DepthStencilState.DepthEnable    = TRUE;
    pso_desc.DepthStencilState.StencilEnable  = FALSE;
    pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

    pso_desc.RasterizerState.FrontCounterClockwise = TRUE;

    // Solid is default.
    // Wireframe mode shows only borders.
    // pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

    THROW_IF_FAILED(device->CreateGraphicsPipelineState(
        &pso_desc,
        IID_PPV_ARGS(&pipeline_state)
    ));

    // Another pipeline state for texture geometry.
    pso_desc.PS = CD3DX12_SHADER_BYTECODE(pixel_shader_texture.Get());
    THROW_IF_FAILED(device->CreateGraphicsPipelineState(
        &pso_desc,
        IID_PPV_ARGS(&pipeline_state_texture)
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

    // Texture buffers.
    textures.resize(model->get_per_shape_texture_files().size());
    upload_textures.resize(model->get_per_shape_texture_files().size());

    const UINT cbv_srv_descriptor_size = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
    );
    CD3DX12_CPU_DESCRIPTOR_HANDLE cbv_srv_handle(
        cbv_srv_heap->GetCPUDescriptorHandleForHeapStart(),
        1, cbv_srv_descriptor_size
    );

    // Upload textures.
    // todo if error use model->get_index_buffers().size()
    for (size_t s = 0; s < model->get_per_shape_texture_files().size(); ++s) {
        if (model->get_per_shape_texture_files()[s].empty()) {
            cbv_srv_handle.Offset(1, cbv_srv_descriptor_size);
            continue;
        }

        auto full_name = std::filesystem::absolute(
            model->get_per_shape_texture_files()[s]
        ).string();

        int tex_width;
        int tex_height;
        int tex_channels;
        unsigned char* image = stbi_load(
            full_name.c_str(),
            &tex_width, &tex_height,
            &tex_channels,
            STBI_rgb_alpha
        );
        if (image == nullptr) {
            throw std::runtime_error("Cannot load texture");
        }

        D3D12_RESOURCE_DESC texture_desc{};
        texture_desc.MipLevels          = 1;
        texture_desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.Width              = tex_width;
        texture_desc.Height             = tex_height;
        texture_desc.DepthOrArraySize   = 1;
        texture_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Flags              = D3D12_RESOURCE_FLAG_NONE;
        texture_desc.SampleDesc.Count   = 1;
        texture_desc.SampleDesc.Quality = 0;

        THROW_IF_FAILED(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &texture_desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&textures[s])
        ));
        const UINT64 upload_buffer_size =
            GetRequiredIntermediateSize(textures[s].Get(), 0, 1);
        THROW_IF_FAILED(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(upload_buffer_size),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&upload_textures[s])
        ));

        D3D12_SUBRESOURCE_DATA texture_data{};
        texture_data.pData      = image;
        texture_data.RowPitch   = tex_width * STBI_rgb_alpha;
        texture_data.SlicePitch = tex_width * tex_height * STBI_rgb_alpha;

        UpdateSubresources(
            command_list.Get(),
            textures[s].Get(),
            upload_textures[s].Get(),
            0, 0, 1,
            &texture_data
        );
        command_list->ResourceBarrier(
            1,
            &CD3DX12_RESOURCE_BARRIER::Transition(
                textures[s].Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            )
        );

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Format = texture_desc.Format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            textures[s].Get(),
            &srv_desc,
            cbv_srv_handle
        );
        cbv_srv_handle.Offset(1, cbv_srv_descriptor_size);
    }


    // Create a constant buffer.
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
        cbv_srv_heap->GetCPUDescriptorHandleForHeapStart()
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
    ID3D12DescriptorHeap* heaps[] = { cbv_srv_heap.Get() };
    command_list->SetDescriptorHeaps(_countof(heaps), heaps);
    command_list->SetGraphicsRootDescriptorTable(
        0, cbv_srv_heap->GetGPUDescriptorHandleForHeapStart()
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
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv_handle(
        dsv_heap->GetCPUDescriptorHandleForHeapStart()
    );
    command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);

    const float clear_color[] = { 0.392f, 0.584f, 0.929f, 1.0f };
    command_list->ClearRenderTargetView(
        rtv_handle, clear_color, 0, nullptr
    );
    command_list->ClearDepthStencilView(
        dsv_handle, D3D12_CLEAR_FLAG_DEPTH,
        1.0f, 0, 0, nullptr
    );
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const UINT cbv_srv_descriptor_size = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
    );
    CD3DX12_GPU_DESCRIPTOR_HANDLE cbv_srv_handle(
        cbv_srv_heap->GetGPUDescriptorHandleForHeapStart(),
        1,
        cbv_srv_descriptor_size
    );

    bool is_plain_color_mode = true;
    for (size_t s = 0; s < model->get_vertex_buffers().size(); ++s) {
        bool texture_exists = !model->get_per_shape_texture_files()[s].empty();
        if (texture_exists) {
            if (is_plain_color_mode) {
                command_list->SetPipelineState(pipeline_state_texture.Get());
                is_plain_color_mode = false;
            }
            cbv_srv_handle.InitOffsetted(
                cbv_srv_heap->GetGPUDescriptorHandleForHeapStart(),
                static_cast<INT>(s + 1),
                cbv_srv_descriptor_size
            );
            command_list->SetGraphicsRootDescriptorTable(1, cbv_srv_handle);
        }
        else if (!is_plain_color_mode) {
            command_list->SetPipelineState(pipeline_state.Get());
            is_plain_color_mode = true;
        }
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
