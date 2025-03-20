#include "XeSS.h"

#include "DX12SwapChain.h"
#include "Upscaling.h"

void XeSS::Init()
{
	auto dx12SwapChain = DX12SwapChain::GetSingleton();
	auto d3d12Device = dx12SwapChain->d3d12Device.get();

	m_desiredOutputResolution.x = (float)dx12SwapChain->swapChainDesc.Width;
	m_desiredOutputResolution.y = (float)dx12SwapChain->swapChainDesc.Height;

	if (FAILED(xessD3D12CreateContext(d3d12Device, &m_xessContext)))
		logger::error("Unable to create XeSS context");

	if (XESS_RESULT_WARNING_OLD_DRIVER == xessIsOptimalDriver(m_xessContext)) {
		MessageBox(NULL, L"Please install the latest graphics driver from your vendor for optimal Intel(R) XeSS performance and visual quality", L"Important notice", MB_OK | MB_TOPMOST | MB_ICONINFORMATION);
	}

	xess_d3d12_init_params_t params = {
		m_desiredOutputResolution,
		XESS_QUALITY_SETTING_AA,
		XESS_INIT_FLAG_LDR_INPUT_COLOR | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE,
		0,
		0,
		nullptr,
		0,
		nullptr,
		0,
		NULL
	};

	if (FAILED(xessD3D12Init(m_xessContext, &params)))
		logger::error("Unable to initialize XeSS context");
		
	DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dx12SwapChain->commandAllocators[dx12SwapChain->frameIndex].get(), nullptr, IID_PPV_ARGS(&commandList)));
	DX::ThrowIfFailed(commandList->Close());
}

void XeSS::Upscale(Texture2D* a_color, Texture2D* a_alphaMask, float2 a_jitter)
{
	auto dx12SwapChain = DX12SwapChain::GetSingleton();
	auto& commandAllocator = dx12SwapChain->commandAllocators[dx12SwapChain->frameIndex];

	auto upscaling = Upscaling::GetSingleton();

	DX::ThrowIfFailed(commandAllocator->Reset());
	DX::ThrowIfFailed(commandList->Reset(commandAllocator.get(), nullptr));

	xess_d3d12_execute_params_t exec_params{};
	exec_params.inputWidth = m_desiredOutputResolution.x;
	exec_params.inputHeight = m_desiredOutputResolution.y;
	exec_params.jitterOffsetX = -a_jitter.x;
	exec_params.jitterOffsetY = -a_jitter.y;
	exec_params.exposureScale = 1.0f;

	exec_params.pColorTexture = upscaling->HUDLessBufferShared12.get();
	exec_params.pVelocityTexture = upscaling->motionVectorBufferShared12.get();
	exec_params.pDepthTexture = upscaling->depthBufferShared12.get();
	exec_params.pExposureScaleTexture = 0;

	exec_params.pOutputTexture = upscaling->upscaleBufferShared12.get();

	if (FAILED(xessD3D12Execute(m_xessContext, commandList.get(), &exec_params)))
		logger::error("Unable to run XeSS");

	DX::ThrowIfFailed(commandList->Close());

	// Execute the command list.
	ID3D12CommandList* ppCommandLists[] = { commandList.get() };
	dx12SwapChain->commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
}
