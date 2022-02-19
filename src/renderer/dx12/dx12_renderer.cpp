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

    load_pipeline();
    load_assets();
}

void cg::renderer::dx12_renderer::destroy()
{
}

void cg::renderer::dx12_renderer::update()
{
    // Update constant buffer cos it depends on volatile data.
    memcpy(
        constant_buffer_data_begin,
        &world_view_projection,
        sizeof(world_view_projection)
    );
}

void cg::renderer::dx12_renderer::render()
{
    //THROW_ERROR("Not implemented yet")
}

void cg::renderer::dx12_renderer::load_pipeline()
{
    // mogus?
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

    // Vertex buffers.
    vertex_buffers.resize(model->get_vertex_buffers().size());
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
            IID_PPV_ARGS(&vertex_buffers[s])
        ));

        auto vertex_buffer_name = std::wstring(L"Vertex buffer ") + std::to_wstring(s);
        vertex_buffers[s]->SetName(vertex_buffer_name.c_str());

        UINT8* vertex_data_begin = nullptr;
        CD3DX12_RANGE read_range(0, 0);
        THROW_IF_FAILED(vertex_buffers[s]->Map(
            0,
            &read_range,
            reinterpret_cast<void**>(&vertex_data_begin)
        ));
        memcpy(
            vertex_data_begin,
            vertex_buffer_data.get(),
            vertex_buffer_size
        );
        vertex_buffers[s]->Unmap(0, nullptr);

        vertex_buffer_views[s].BufferLocation = vertex_buffers[s]->GetGPUVirtualAddress();
        vertex_buffer_views[s].SizeInBytes    = vertex_buffer_size;
        vertex_buffer_views[s].StrideInBytes  = sizeof(cg::vertex);
    }


    // Index buffers.
    index_buffers.resize(model->get_index_buffers().size());
    index_buffer_views.resize(model->get_index_buffers().size());
    for (size_t s = 0; s < model->get_index_buffers().size(); ++s) {
        auto index_buffer_data = model->get_vertex_buffers()[s];
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
            IID_PPV_ARGS(&index_buffers[s])
        ));

        auto index_buffer_name = std::wstring(L"Index buffer ") + std::to_wstring(s);
        vertex_buffers[s]->SetName(index_buffer_name.c_str());

        UINT8* index_data_begin;
        CD3DX12_RANGE read_range(0, 0);
        THROW_IF_FAILED(index_buffers[s]->Map(
            0,
            &read_range,
            reinterpret_cast<void**>(&index_data_begin)
        ));
        memcpy(
            index_data_begin,
            index_buffer_data.get(),
            index_buffer_size
        );
        index_buffers[s]->Unmap(0, nullptr);

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

    constant_buffer->Unmap(0, nullptr);
}

void cg::renderer::dx12_renderer::populate_command_list()
{
    THROW_ERROR("Not implemented yet")
}


void cg::renderer::dx12_renderer::move_to_next_frame()
{
    THROW_ERROR("Not implemented yet")
}

void cg::renderer::dx12_renderer::wait_for_gpu()
{
    THROW_ERROR("Not implemented yet")
}
