#include "renderer.h"


#pragma warning(push, 0)        
#include <locale>
#include <codecvt>
#include <thread>
#include <chrono>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#include "d3dx12.h"
#pragma warning(pop)

void Renderer::OnInit()
{
	LoadPipeline();
	LoadAssets();
}

void Renderer::OnUpdate()
{
	angle += delta_rotation;
	eye_position += XMVECTOR{ sin(angle), 0.f, cos(angle) } * delta_forward;
	auto focus_position = eye_position + XMVECTOR{ sin(angle), 0.f, cos(angle) };
	auto up_direction = XMVECTOR{ 0.f, 1.f, 0.f };
	view = XMMatrixLookAtLH(eye_position, focus_position, up_direction);
	world_view_projection = world * view * projection;
	memcpy(constant_buffer_data_begin, &world_view_projection, sizeof(world_view_projection));
}

void Renderer::OnRender()
{
	PopulateCommandList();
	ID3D12CommandList* lists[]{ command_list.Get() };

	command_queue->ExecuteCommandLists(_countof(lists), lists);

	ThrowIfFailed(swap_chain->Present(0, 0));

	MoveToNextFrame();
}

void Renderer::OnDestroy()
{
	WaitForGPU();
	CloseHandle(fence_event);
}

void Renderer::OnKeyDown(UINT8 key)
{
	constexpr auto A_CODE = 0x41 - 'a';
	switch (key) {
	case A_CODE + 'd':
		delta_rotation = 0.0001f;
		break;
	case A_CODE + 'a':
		delta_rotation = -0.0001f;
		break;
	case A_CODE + 'w':
		delta_forward = 0.0001f;
		break;
	case A_CODE + 's':
		delta_forward = -0.0001f;
		break;
	}
}

void Renderer::OnKeyUp(UINT8 key)
{
	constexpr auto A_CODE = 0x41 - 'a';
	switch (key) {
	case A_CODE + 'd':
	case A_CODE + 'a':
		delta_rotation = 0;
		break;
	case A_CODE + 'w':
	case A_CODE + 's':
		delta_forward = 0;
		break;
	}
}

void Renderer::LoadPipeline()
{
	UINT dxgi_factory_flag = 0;
#if _DEBUG
	ComPtr<ID3D12Debug> debug_controller;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
		debug_controller->EnableDebugLayer();
		dxgi_factory_flag |= DXGI_CREATE_FACTORY_DEBUG;
	}
#endif // _DEBUG
	ComPtr<IDXGIFactory4> gi_factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgi_factory_flag, IID_PPV_ARGS(&gi_factory)));
	ComPtr<IDXGIAdapter1> hardware_adapter;
	UINT adapter_index = 0;

	while (DXGI_ERROR_NOT_FOUND != gi_factory->EnumAdapters1(adapter_index, &hardware_adapter)) {
		if (SUCCEEDED(D3D12CreateDevice(hardware_adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
			break;
		}
		++adapter_index;
	}
	ThrowIfFailed(D3D12CreateDevice(hardware_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

	for (UINT i = 0; i < kFrameNumber; i++) {
		ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators[i])));
	}

	D3D12_COMMAND_QUEUE_DESC cmd_queue_desc{};
	cmd_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	cmd_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	device->CreateCommandQueue(&cmd_queue_desc, IID_PPV_ARGS(&command_queue));

	DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
	swap_chain_desc.BufferCount = kFrameNumber;
	swap_chain_desc.Width = GetWidth();
	swap_chain_desc.Height = GetHeight();
	swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swap_chain_desc.SampleDesc.Count = 1;
	ComPtr<IDXGISwapChain1> tmp_swap_chain{};
	ThrowIfFailed(
		gi_factory->CreateSwapChainForHwnd(
			command_queue.Get(), Win32Window::GetHwnd(),
			&swap_chain_desc, nullptr, nullptr, &tmp_swap_chain)
	);
	ThrowIfFailed(
		gi_factory->MakeWindowAssociation(Win32Window::GetHwnd(), DXGI_MWA_NO_ALT_ENTER)
	);
	ThrowIfFailed(tmp_swap_chain.As(&swap_chain));
	frame_index = swap_chain->GetCurrentBackBufferIndex();

	D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{}; // render target view
	rtv_heap_desc.NumDescriptors = kFrameNumber;
	rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));
	rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_handle{ rtv_heap->GetCPUDescriptorHandleForHeapStart() };
	for (UINT i = 0; i < kFrameNumber; i++) {
		ThrowIfFailed(swap_chain->GetBuffer(i, IID_PPV_ARGS(&render_targets.at(i))));
		device->CreateRenderTargetView(render_targets.at(i).Get(), nullptr, rtv_handle);
		std::wstring rt_name = L"Render Target " + std::to_wstring(i);
		render_targets.at(i)->SetName(rt_name.c_str());
		rtv_handle.Offset(1, rtv_descriptor_size);
	}

	D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_desc{}; // constant buffer view
	cbv_heap_desc.NumDescriptors = 1;
	cbv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(device->CreateDescriptorHeap(&cbv_heap_desc, IID_PPV_ARGS(&cbv_heap)));
}

void Renderer::LoadAssets()
{
	D3D12_FEATURE_DATA_ROOT_SIGNATURE rs_feature_data{};
	rs_feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs_feature_data, sizeof(rs_feature_data)))) {
		rs_feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}
	CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
	CD3DX12_ROOT_PARAMETER1 root_params[1];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
	root_params[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX);
	D3D12_ROOT_SIGNATURE_FLAGS rs_flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc{};
	rs_desc.Init_1_1(_countof(root_params), root_params, 0, nullptr, rs_flags);
	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;

	auto res = D3DX12SerializeVersionedRootSignature(&rs_desc, rs_feature_data.HighestVersion, &signature, &error);
	if (FAILED(res)) {
		OutputDebugStringA((char*)error->GetBufferPointer());
		ThrowIfFailed(res);
	}
	ThrowIfFailed(
		device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root_signature))
	);
	WCHAR buffer[MAX_PATH];
	GetModuleFileName(nullptr, buffer, MAX_PATH);
	std::wstring module_path{ buffer };
	auto pos = module_path.find_last_of(L"\\/");
	std::wstring shader_path = module_path.substr(0, pos + 1) + L"shaders.hlsl";
	ComPtr<ID3DBlob> vertex_shader{};
	ComPtr<ID3DBlob> fragment_shader{};
	UINT compile_flags = 0;
#ifdef _DEBUG
	compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif // _DEBUG
	res = D3DCompileFromFile(
		shader_path.c_str(), nullptr, nullptr,
		"VSMain", "vs_5_0", compile_flags, 0, &vertex_shader, &error);
	if (FAILED(res)) {
		OutputDebugStringA((char*)error->GetBufferPointer());
		ThrowIfFailed(res);
	}
	res = D3DCompileFromFile(
		shader_path.c_str(), nullptr, nullptr,
		"PSMain", "ps_5_0", compile_flags, 0, &fragment_shader, &error);
	if (FAILED(res)) {
		OutputDebugStringA((char*)error->GetBufferPointer());
		ThrowIfFailed(res);
	}
	D3D12_INPUT_ELEMENT_DESC input_element_descs[]{
		{
			.SemanticName{"POSITION"},
			.SemanticIndex{0},
			.Format{DXGI_FORMAT_R32G32B32_FLOAT},
			.InputSlot{0},
			.AlignedByteOffset{0},
			.InputSlotClass{D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA},
			.InstanceDataStepRate{0}
		},
		{
			.SemanticName{"COLOR"},
			.SemanticIndex{0},
			.Format{DXGI_FORMAT_R32G32B32A32_FLOAT},
			.InputSlot{0},
			.AlignedByteOffset{4 * 3},
			.InputSlotClass{D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA},
			.InstanceDataStepRate{0}
		},
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
	pso_desc.InputLayout.pInputElementDescs = input_element_descs;
	pso_desc.InputLayout.NumElements = 2;
	pso_desc.pRootSignature = root_signature.Get();
	pso_desc.VS = CD3DX12_SHADER_BYTECODE(vertex_shader.Get());
	pso_desc.PS = CD3DX12_SHADER_BYTECODE(fragment_shader.Get());
	pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	pso_desc.DepthStencilState.DepthEnable = false;
	pso_desc.DepthStencilState.StencilEnable = false;
	pso_desc.SampleMask = UINT_MAX;
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso_desc.SampleDesc.Count = 1;
	ThrowIfFailed(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state)));

	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocators[frame_index].Get(), pipeline_state.Get(), IID_PPV_ARGS(&command_list));
	ThrowIfFailed(command_list->Close());

	auto folder_utf8 = std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().to_bytes(module_path.substr(0, pos + 1));
	std::string inputfile = std::string(folder_utf8) + "CornellBox-Original.obj";

	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;

	std::string warn;
	std::string err;

	bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, inputfile.c_str(), std::string(folder_utf8).c_str(), true);

	if (!warn.empty()) OutputDebugStringA(warn.c_str());
	if (!err.empty()) OutputDebugStringA(err.c_str());
	if (!ret) ThrowIfFailed(-1);

	// Loop over shapes
	for (size_t s = 0; s < shapes.size(); s++) {
		// Loop over faces(polygon)
		size_t index_offset = 0;
		for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
			int fv = shapes[s].mesh.num_face_vertices[f];

			// Loop over vertices in the face.
			for (size_t v = 0; v < fv; v++) {
				// access to vertex
				tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];
				tinyobj::real_t vx = attrib.vertices[3 * idx.vertex_index + 0];
				tinyobj::real_t vy = attrib.vertices[3 * idx.vertex_index + 1];
				tinyobj::real_t vz = attrib.vertices[3 * idx.vertex_index + 2];
				int material_id = shapes[s].mesh.material_ids[f];

				auto& m = materials[material_id];
				vertices.push_back({ {vx, vy, vz}, {m.diffuse[0], m.diffuse[1], m.diffuse[2], 1.f} });
			}
			index_offset += fv;
		}
	}
	
	const UINT vb_size = static_cast<UINT>(sizeof(ColorVertex) * vertices.size());
	CD3DX12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC res_buf{ CD3DX12_RESOURCE_DESC::Buffer(vb_size) };

	ThrowIfFailed(device->CreateCommittedResource(
		&heap_props,
		D3D12_HEAP_FLAG_NONE,
		&res_buf,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&vertex_buffer)
	));
	vertex_buffer->SetName(L"Our Vertex Buffer");

	UINT8* vertex_data_begin{};
	CD3DX12_RANGE read_range{ 0, 0 };
	ThrowIfFailed(vertex_buffer->Map(0, &read_range, reinterpret_cast<void**>(&vertex_data_begin)));
	memcpy(vertex_data_begin, vertices.data(), vb_size);
	vertex_buffer->Unmap(0, nullptr);

	vertex_buffer_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
	vertex_buffer_view.StrideInBytes = sizeof(ColorVertex); // size of one record
	vertex_buffer_view.SizeInBytes = vb_size; // size of the whole buffer
	
	CD3DX12_HEAP_PROPERTIES cb_heap_props{ D3D12_HEAP_TYPE_UPLOAD };
	auto cb_buf = CD3DX12_RESOURCE_DESC::Buffer(64 * 1024);

	ThrowIfFailed(device->CreateCommittedResource(
		&cb_heap_props,
		D3D12_HEAP_FLAG_NONE,
		&cb_buf,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&constant_buffer)
	));
	constant_buffer->SetName(L"Our Constant Buffer");

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc{};
	cbv_desc.SizeInBytes = (sizeof(world_view_projection) + 255) & ~255;
	cbv_desc.BufferLocation = constant_buffer->GetGPUVirtualAddress();

	device->CreateConstantBufferView(&cbv_desc, cbv_heap->GetCPUDescriptorHandleForHeapStart());
	ThrowIfFailed(constant_buffer->Map(0, &read_range, reinterpret_cast<void**>(&constant_buffer_data_begin)));
	memcpy(constant_buffer_data_begin, &world_view_projection, sizeof(world_view_projection));
	// won't unmap because have to update this buffer often

	ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	fence_event = CreateEvent(nullptr, false, false, nullptr);
	if (fence_event == nullptr) {
		ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
	}
	WaitForGPU();
}

void Renderer::PopulateCommandList()
{
	ThrowIfFailed(command_allocators[frame_index]->Reset());
	ThrowIfFailed(command_list->Reset(command_allocators[frame_index].Get(), pipeline_state.Get()));

	command_list->SetGraphicsRootSignature(root_signature.Get());
	ID3D12DescriptorHeap* heaps[] {cbv_heap.Get()};
	command_list->SetDescriptorHeaps(_countof(heaps), heaps); // very expensive operation btw
	command_list->SetGraphicsRootDescriptorTable(0, cbv_heap->GetGPUDescriptorHandleForHeapStart());

	command_list->RSSetViewports(1, &view_port);
	command_list->RSSetScissorRects(1, &scissor_rect);

	auto present_to_render_transition = CD3DX12_RESOURCE_BARRIER::Transition(
		render_targets[frame_index].Get(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET
	);
	command_list->ResourceBarrier(1, &present_to_render_transition);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_handle(rtv_heap->GetCPUDescriptorHandleForHeapStart(), frame_index, rtv_descriptor_size);
	command_list->OMSetRenderTargets(1, &rtv_handle, false, nullptr);
	float color [] { 0.f, 0.f, 0.f, 1.f };
	command_list->ClearRenderTargetView(rtv_handle, color, 0, nullptr);
	command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	command_list->IASetVertexBuffers(0, 1, &vertex_buffer_view);
	command_list->DrawInstanced(vertices.size(), 1, 0, 0);

	auto render_to_present_transition = CD3DX12_RESOURCE_BARRIER::Transition(
		render_targets[frame_index].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT
	);
	command_list->ResourceBarrier(1, &render_to_present_transition);

	ThrowIfFailed(command_list->Close());
}

void Renderer::WaitForGPU()
{
	ThrowIfFailed(command_queue->Signal(fence.Get(), fence_values[frame_index]));
	ThrowIfFailed(fence->SetEventOnCompletion(fence_values[frame_index], fence_event));
	WaitForSingleObjectEx(fence_event, INFINITE, false);
	fence_values[frame_index]++;
}

void Renderer::MoveToNextFrame()
{
	const UINT64 current_fence_value = fence_values[frame_index];
	ThrowIfFailed(command_queue->Signal(fence.Get(), current_fence_value));

	frame_index = swap_chain->GetCurrentBackBufferIndex();

	if (fence->GetCompletedValue() < fence_values[frame_index]) {
		ThrowIfFailed(fence->SetEventOnCompletion(fence_values[frame_index], fence_event));
		WaitForSingleObjectEx(fence_event, INFINITE, false);
	}
	fence_values[frame_index] = current_fence_value + 1;
}
