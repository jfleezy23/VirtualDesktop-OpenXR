// Enforce bounded native commits: only End's actual color/depth references consume a chain.
// Use real vendor NR, images and readback; OVRNull's permissive index wrapping is not the oracle.
#define VDXR_NR_TEST_ENTRYPOINT retainedInputMain
#include "nr_input_toggle_regression.cpp"
#undef VDXR_NR_TEST_ENTRYPOINT

namespace {
    struct StrictCompositor {
        std::map<ovrTextureSwapChain, unsigned> pending, lengths;
        unsigned commits{}, ends{}, maxLength{}, exhausted{};

        ovrResult commit(ovrSession session, ovrTextureSwapChain chain) {
            int length{};
            checkOvr(ovr_GetTextureSwapChainLength(session, chain, &length));
            require(length > 0, "Strict compositor requires a bounded native chain");
            lengths[chain] = length;
            maxLength = std::max(maxLength, static_cast<unsigned>(length));
            if (pending[chain] >= static_cast<unsigned>(length)) {
                ++exhausted;
                std::cerr << "strict compositor: unconsumed native chain exhausted after " << length << " commits\n";
                return ovrError_TextureSwapChainFull;
            }
            ++pending[chain];
            ++commits;
            return ovrSuccess;
        }
        void end(const ovrLayerHeader* const* layers, unsigned count) {
            std::set<ovrTextureSwapChain> consumed;
            for (unsigned i = 0; i < count; ++i) {
                if (!layers[i])
                    continue;
                require(layers[i]->Type == ovrLayerType_EyeFov || layers[i]->Type == ovrLayerType_EyeFovDepth,
                        "Strict fixture only submits projection layers");
                const auto& color = *reinterpret_cast<const ovrLayerEyeFov*>(layers[i]);
                consumed.insert(color.ColorTexture[0]);
                consumed.insert(color.ColorTexture[1]);
                if (layers[i]->Type == ovrLayerType_EyeFovDepth) {
                    const auto& depth = *reinterpret_cast<const ovrLayerEyeFovDepth*>(layers[i]);
                    consumed.insert(depth.DepthTexture[0]);
                    consumed.insert(depth.DepthTexture[1]);
                }
            }
            for (auto chain : consumed) {
                require(chain && written.count(chain), "Every submitted chain must have a real committed image");
                pending[chain] = 0;
            }
            ++ends;
        }
    };
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4,
                "Usage: nr_intermediate_commit_regression <backend> <state> color|depth|ignored|reuse|upscaled");
        const std::string mode = argv[3];
        require(mode == "color" || mode == "depth" || mode == "ignored" || mode == "reuse" || mode == "upscaled" ||
                    mode == "direct" || mode == "static" || mode == "static-array",
                "Unknown strict mode");
        singleDepthInput = mode == "direct" || mode == "static";
        staticDepthInput = mode == "static" || mode == "static-array";
        staticDepthReuse = mode == "static-array";
        startWithNr = singleDepthInput;
        sameDepthForBothEyes = singleDepthInput;
        includeDepthInput = mode != "color";
        nrDepthSubmissionSetting = mode == "ignored" ? 0 : 1;
        nrSharpenSetting = mode == "color" ? 35 : 0;
        if (mode == "upscaled") {
            nrSharpenSetting = 35;
            nrUpscalingSetting = 75;
        }
        additionalSteadyFrames = 16;
        colourTolerance = 1; // BGRA8 postprocessing quantizes the shader output back to eight bits.
        expectedOffColour = {{{192, 32, 48, 255}, {24, 144, 208, 255}}};
        StrictCompositor strict;
        guardNativeCommit = [&](auto session, auto chain) { return strict.commit(session, chain); };
        observeNativeLayers = [&](auto layers, auto count) { strict.end(layers, count); };
        observeProjection = [&](const auto&) {
            unsigned difference{};
            for (unsigned c = 0; c < 3; ++c)
                difference += std::abs(int(observedColour[0][c]) - int(observedColour[1][c]));
            require(difference > 64, "Distinct submitted eyes must retain different color content");
            require(observedColour[0][0] > observedColour[0][2] + 32 &&
                        observedColour[1][2] > observedColour[1][0] + 32,
                    "Submitted left must remain red and right blue; swapped eyes are invalid");
        };
        XrCompositionLayerProjection second{};
        std::array<const XrCompositionLayerBaseHeader*, 2> layers{};
        if (mode == "reuse") {
            adjustFrameLayers = [&](auto& end, const auto& first) {
                second = first;
                layers = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&first),
                          reinterpret_cast<const XrCompositionLayerBaseHeader*>(&second)};
                end.layers = layers.data();
                end.layerCount = 2;
            };
        }
        char toggle[] = "toggle", full[] = "100";
        char* arguments[]{argv[0], argv[1], argv[2], toggle, full};
        const auto result = retainedInputMain(5, arguments);
        guardNativeCommit = {};
        observeNativeLayers = {};
        observeProjection = {};
        adjustFrameLayers = {};
        require(result == 0, "Strict bounded-compositor public NR path failed");
        require(!strict.exhausted && strict.ends > 2 * strict.maxLength + 2,
                "Strict run must exceed twice the native ring length without exhaustion");
        for (const auto& [chain, outstanding] : strict.pending)
            require(outstanding == 0, "No unused native commit may remain after final submission");
        std::cout << "PASS: strict bounded native commits, all final layers consumed; mode=" << mode
                  << " ends=" << strict.ends << " commits=" << strict.commits << " maxLength=" << strict.maxLength
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
