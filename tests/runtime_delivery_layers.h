#pragma once

// Public static mipmapped quad source shared by two layers. Only the
// controlled test uses the extra GPU readbacks; no runtime instrumentation.
struct DeliveryLayerFixture {
    XrSwapchain quad{};
    XrCompositionLayerQuad quadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    ComPtr<ID3D11Texture2D> quadSnapshot;
    unsigned captures{};
    double sample{};
    ovrTextureSwapChain written{};

    DeliveryLayerFixture(XrSession session, XrSpace space, ID3D11DeviceContext* context) {
        XrSwapchainCreateInfo desc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        desc.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
        desc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.width = desc.height = 16;
        desc.arraySize = desc.sampleCount = desc.faceCount = 1;
        desc.mipCount = 2;
        XR_CHECK(xrCreateSwapchain(session, &desc, &quad));
        uint32_t count{};
        XR_CHECK(xrEnumerateSwapchainImages(quad, 0, &count, nullptr));
        std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(
            quad, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index{};
        XR_CHECK(xrAcquireSwapchainImage(quad, &acquire, &index));
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        XR_CHECK(xrWaitSwapchainImage(quad, &wait));
        for (unsigned mip = 0; mip < 2; ++mip) {
            const unsigned size = 16 >> mip;
            std::vector<uint32_t> pixels(size * size, 0x803264c8u);
            context->UpdateSubresource(images[index].texture, mip, nullptr, pixels.data(), size * 4, 0);
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XR_CHECK(xrReleaseSwapchainImage(quad, &release));
        quadLayer.space = cylinder.space = space;
        quadLayer.layerFlags =
            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quadLayer.pose.orientation.w = 1.f;
        quadLayer.pose.position.z = -2.f;
        quadLayer.size = {1.f, 1.f};
        quadLayer.subImage = {quad, {{0, 0}, {16, 16}}, 0};
        cylinder.layerFlags = quadLayer.layerFlags;
        cylinder.eyeVisibility = quadLayer.eyeVisibility;
        cylinder.pose = quadLayer.pose;
        cylinder.pose.position.x = .5f;
        cylinder.subImage = quadLayer.subImage;
        cylinder.radius = 2.f;
        cylinder.centralAngle = 1.f;
        cylinder.aspectRatio = 1.f;
    }
    static ComPtr<ID3D11Texture2D> snapshot(ovrSession session, ovrTextureSwapChain chain) {
        int count{}, current{};
        require(OVR_SUCCESS(ovr_GetTextureSwapChainLength(session, chain, &count)) && count > 0 &&
                    OVR_SUCCESS(ovr_GetTextureSwapChainCurrentIndex(session, chain, &current)),
                "actual native chain indexing");
        ComPtr<ID3D11Texture2D> image, staging;
        require(OVR_SUCCESS(ovr_GetTextureSwapChainBufferDX(
                    session, chain, (current + count - 1) % count, IID_PPV_ARGS(&image))),
                "actual committed native image");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        image->GetDevice(&device);
        device->GetImmediateContext(&context);
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags = desc.MiscFlags = 0;
        checkHr(device->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), image.Get());
        return staging;
    }
    void capture(ovrSession session, const ovrLayerHeader* const* layers, unsigned count) {
        if (count == 1)
            return;
        require(count == 3 && layers[0]->Type == ovrLayerType_EyeFov && layers[1]->Type == ovrLayerType_Quad &&
                    layers[2]->Type == ovrLayerType_Cylinder,
                "ordinary fallback preserves every layer and its order");
        const auto& q = *reinterpret_cast<const ovrLayerQuad*>(layers[1]);
        const auto& other = *reinterpret_cast<const ovrLayerCylinder*>(layers[2]);
        require(q.ColorTexture == other.ColorTexture, "quad and cylinder share one finalized chain");
        const auto currentSample = reinterpret_cast<const ovrLayerEyeFov*>(layers[0])->SensorSampleTime;
        if (captures++) {
            require(sample == currentSample && written == q.ColorTexture,
                    "padding repeats preserve the same mixed-layer input and chain");
            return;
        }
        sample = currentSample;
        written = q.ColorTexture;
        quadSnapshot = snapshot(session, q.ColorTexture);
    }
    void verify() {
        require(captures == 3 && quadSnapshot, "one mixed-layer input fills its three actual native slots");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        quadSnapshot->GetDevice(&device);
        device->GetImmediateContext(&context);
        D3D11_TEXTURE2D_DESC desc{};
        quadSnapshot->GetDesc(&desc);
        require(desc.ArraySize == 1 && desc.MipLevels == 2, "native fallback retains both mip levels");
        for (UINT mip = 0; mip < 2; ++mip) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checkHr(context->Map(quadSnapshot.Get(), mip, D3D11_MAP_READ, 0, &mapped));
            bool exact = true;
            // Existing alpha policy operates on mip0. Higher levels retain the
            // app's pixels, matching the previous direct-image contract.
            const std::array<unsigned, 4> expected =
                mip ? std::array<unsigned, 4>{200, 100, 50, 128} : std::array<unsigned, 4>{100, 50, 25, 128};
            for (UINT y = 0; y < 16u >> mip; ++y)
                for (UINT x = 0; x < 16u >> mip; ++x)
                    for (unsigned c = 0; c < 4; ++c)
                        exact &=
                            static_cast<const uint8_t*>(mapped.pData)[y * mapped.RowPitch + x * 4 + c] == expected[c];
            context->Unmap(quadSnapshot.Get(), mip);
            require(exact,
                    mip ? "native higher mip retains every app pixel"
                        : "native alpha is corrected once after source receipt");
        }
        std::cout << "PASS: native mixed fallback retains shared-quad late alpha and complete mip pixels\n";
    }
};
