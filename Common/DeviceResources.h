﻿#pragma once

#include "../State/Stats.h"

namespace DX
{
	// Provides an interface for an application that owns DeviceResources to be notified of the device being lost or created.
	interface IDeviceNotify
	{
		virtual void OnDeviceLost() = 0;
		virtual void OnDeviceRestored() = 0;
	};

	// Controls all the DirectX device resources.
	class DeviceResources
	{
	public:
		DeviceResources();
		void SetSwapChainPanel(Windows::UI::Xaml::Controls::SwapChainPanel^ panel);
		void SetLogicalSize(Windows::Foundation::Size logicalSize);
		void SetCurrentOrientation(Windows::Graphics::Display::DisplayOrientations currentOrientation);
		void SetDpi(float dpi);
		void SetCompositionScale(float compositionScaleX, float compositionScaleY);
		void ValidateDevice();
		void HandleDeviceLost();
		void RegisterDeviceNotify(IDeviceNotify* deviceNotify);
		void Trim();
		void GetDXGIFrameStatistics(std::shared_ptr<moonlight_xbox_dx::Stats>& dstStats);
		void Present();
		void GetUWPPixelDimensions(uint32_t *width, uint32_t *height);
		static int uwp_get_width();
		static int uwp_get_height();

		bool                        GetEnableVsync() const                  { return m_enableVsync; }
		void                        SetEnableVsync(bool ev)                 { m_enableVsync = ev; }

		// Stats helpers
		void                        SetStats(const std::shared_ptr<moonlight_xbox_dx::Stats>& stats)  { m_stats = stats; }
		std::shared_ptr<moonlight_xbox_dx::Stats>                           GetStats()                { return m_stats; }

		// The size of the render target, in pixels.
		Windows::Foundation::Size	GetOutputSize() const					{ return m_outputSize; }

		// The size of the render target, in dips.
		Windows::Foundation::Size	GetLogicalSize() const					{ return m_logicalSize; }
		float						GetDpi() const							{ return m_effectiveDpi; }

		// D3D Accessors.
		ID3D11Device3*				GetD3DDevice() const					{ return m_d3dDevice.Get(); }
		ID3D11DeviceContext3*		GetD3DDeviceContext() const				{ return m_d3dContext.Get(); }
		IDXGISwapChain4*			GetSwapChain() const					{ return m_swapChain.Get(); }
		auto                        GetDXGIFactory() const noexcept         { return m_dxgiFactory.Get(); }
		D3D_FEATURE_LEVEL			GetDeviceFeatureLevel() const			{ return m_d3dFeatureLevel; }
		ID3D11RenderTargetView1*	GetBackBufferRenderTargetView() const	{ return m_d3dRenderTargetView.Get(); }
		ID3D11DepthStencilView*		GetDepthStencilView() const				{ return m_d3dDepthStencilView.Get(); }
		DXGI_FORMAT                 GetBackBufferFormat() const noexcept    { return m_backBufferFormat; }
		D3D11_VIEWPORT				GetScreenViewport() const				{ return m_screenViewport; }
		DirectX::XMFLOAT4X4			GetOrientationTransform3D() const		{ return m_orientationTransform3D; }

		// D2D Accessors.
		D2D1::Matrix3x2F			GetOrientationTransform2D() const		{ return m_orientationTransform2D; }
	private:
		void CreateDeviceIndependentResources();
		void CreateDeviceResources();
		void CreateWindowSizeDependentResources();
		void UpdateRenderTargetSize();
		DXGI_MODE_ROTATION ComputeDisplayRotation();

		// Direct3D objects.
		Microsoft::WRL::ComPtr<IDXGIFactory2>           m_dxgiFactory;
		Microsoft::WRL::ComPtr<ID3D11Device3>			m_d3dDevice;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext3>	m_d3dContext;
		Microsoft::WRL::ComPtr<IDXGISwapChain4>			m_swapChain;

		// Direct3D rendering objects. Required for 3D.
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView1>	m_d3dRenderTargetView;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView>	m_d3dDepthStencilView;
		D3D11_VIEWPORT									m_screenViewport;

		// Cached reference to the XAML panel.
		Windows::UI::Xaml::Controls::SwapChainPanel^	m_swapChainPanel;

		// Cached device properties.
		D3D_FEATURE_LEVEL								m_d3dFeatureLevel;
		Windows::Foundation::Size						m_d3dRenderTargetSize;
		Windows::Foundation::Size						m_outputSize;
		Windows::Foundation::Size						m_logicalSize;
		Windows::Graphics::Display::DisplayOrientations	m_nativeOrientation;
		Windows::Graphics::Display::DisplayOrientations	m_currentOrientation;
		float											m_dpi;
		float											m_compositionScaleX;
		float											m_compositionScaleY;
		DWORD                                           m_dxgiFactoryFlags;

		// Variables that take into account whether the app supports high resolution screens or not.
		float											m_effectiveDpi;
		float											m_effectiveCompositionScaleX;
		float											m_effectiveCompositionScaleY;

		// Transforms used for display orientation.
		D2D1::Matrix3x2F	                            m_orientationTransform2D;
		DirectX::XMFLOAT4X4	                            m_orientationTransform3D;

		// The IDeviceNotify can be held directly as it owns the DeviceResources.
		IDeviceNotify*                                  m_deviceNotify;

		DXGI_FORMAT                                     m_backBufferFormat;
		bool                                            m_enableVsync;
		bool                                            m_swapchainVsync;
		SyncMode                                        m_displayStatus;
		std::shared_ptr<moonlight_xbox_dx::Stats>       m_stats;
	};
}
