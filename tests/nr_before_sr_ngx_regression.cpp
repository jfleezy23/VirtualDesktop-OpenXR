#include "../virtualdesktop-openxr/NrBeforeSrColor.h"
#include "../virtualdesktop-openxr/NrBeforeSrModel.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>

namespace {
    using namespace virtualdesktop_openxr::nr_before_sr;

    void Require(bool condition, const char* check) {
        if (!condition) {
            std::cerr << "FAIL: " << check << '\n';
            std::exit(1);
        }
    }

    struct ParameterTable final : NVSDK_NGX_Parameter {
        std::map<std::string, unsigned int> unsignedValues;
        std::map<std::string, int> integerValues;
        std::map<std::string, float> floatValues;
        std::map<std::string, ID3D12Resource*> textures;
        ID3D12Resource* rejectedColor{};
        bool rejectNonzeroColorX{};

        void Set(const char*, unsigned long long) override {
        }
        void Set(const char* name, float value) override {
            floatValues[name] = value;
        }
        void Set(const char*, double) override {
        }
        void Set(const char* name, unsigned int value) override {
            if (!rejectNonzeroColorX || !value ||
                std::string_view(name) != NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X)
                unsignedValues[name] = value;
        }
        void Set(const char* name, int value) override {
            integerValues[name] = value;
        }
        void Set(const char*, ID3D11Resource*) override {
        }
        void Set(const char* name, ID3D12Resource* value) override {
            if (value != rejectedColor)
                textures[name] = value;
        }
        void Set(const char*, void*) override {
        }
        NVSDK_NGX_Result Get(const char*, unsigned long long*) const override {
            return NVSDK_NGX_Result_Fail;
        }
        NVSDK_NGX_Result Get(const char* name, float* value) const override {
            return GetFrom(floatValues, name, value);
        }
        NVSDK_NGX_Result Get(const char*, double*) const override {
            return NVSDK_NGX_Result_Fail;
        }
        NVSDK_NGX_Result Get(const char* name, unsigned int* value) const override {
            return GetFrom(unsignedValues, name, value);
        }
        NVSDK_NGX_Result Get(const char* name, int* value) const override {
            return GetFrom(integerValues, name, value);
        }
        NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override {
            return NVSDK_NGX_Result_Fail;
        }
        NVSDK_NGX_Result Get(const char* name, ID3D12Resource** value) const override {
            return GetFrom(textures, name, value);
        }
        NVSDK_NGX_Result Get(const char*, void**) const override {
            return NVSDK_NGX_Result_Fail;
        }
        void Reset() override {
            std::abort();
        }

      private:
        template <typename T>
        static NVSDK_NGX_Result GetFrom(const std::map<std::string, T>& table, const char* name, T* result) {
            const auto it = table.find(name);
            if (it == table.end())
                return NVSDK_NGX_Result_Fail;
            *result = it->second;
            return NVSDK_NGX_Result_Success;
        }
    };

    auto FakeTexture(std::uintptr_t token) {
        return reinterpret_cast<ID3D12Resource*>(token);
    }

    void CheckColorGuides() {
        D3D12_RESOURCE_DESC color{};
        color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        color.Width = 2880;
        color.Height = 1544;
        color.MipLevels = color.DepthOrArraySize = 1;
        color.SampleDesc.Count = 1;
        color.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        auto depth = color;
        depth.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        const Rect left{0, 0, 1439, 1541}, right{1440, 0, 1439, 1541};
        for (auto eye : {left, right}) {
            const auto plan = PlanColorViews(color, eye, depth, eye);
            Require(plan && plan->resolvedFormat == color.Format &&
                        plan->depthPlaneSrv == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
                    "preserve packed SR color and read the typed depth plane for each eye");
        }
        const Rect depthCrop{1438, 2, right.width, right.height};
        Require(bool(PlanColorViews(color, right, depth, depthCrop)), "independent color and depth origins");
        color.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        Require(bool(PlanColorViews(color, right, depth, depthCrop)),
                "allow shader-readable simultaneous-access color");
        Require(!PlanColorViews(color, right, depth, {1442, 0, right.width, right.height}) &&
                    !PlanColorViews(color, right, depth, {0, 0, right.width, right.height - 1}) &&
                    !PlanColorViews(color, {UINT32_MAX, 0, right.width, right.height}, depth, right),
                "reject out-of-bounds, mismatched and overflowing rectangles");
        depth.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        Require(!PlanColorViews(color, right, depth, right), "deny non-readable depth");
        depth.Flags = D3D12_RESOURCE_FLAG_NONE;
        depth.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        Require(!PlanColorViews(color, right, depth, right),
                "typed DSV resource cannot be reinterpreted as a plane SRV");
        depth.Format = DXGI_FORMAT_R32_FLOAT;
        color.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const auto direct = PlanColorViews(color, right, depth, depthCrop);
        Require(direct && direct->resolvedFormat == color.Format && direct->depthPlaneSrv == DXGI_FORMAT_UNKNOWN,
                "keep FP16 color and direct R32_FLOAT depth");

        NgxSrEvaluation frame;
        frame.color = {FakeTexture(1), right};
        frame.depth = {FakeTexture(2), depthCrop};
        frame.motion = {FakeTexture(3), {0, 0, 2480, 2656}};
        frame.output.active = {0, 0, 2480, 2656};
        frame.motionScaleX = -1.5f;
        frame.motionScaleY = 2.f;
        Require(ModelMotionExtentEligible(frame, DXGI_FORMAT_R16G16_FLOAT),
                "accept qualified full output-sized RG16 at zero origin");
        for (auto extracted : {static_cast<ID3D12Resource*>(nullptr), FakeTexture(4)}) {
            ParameterTable model;
            ConfigureModelEvaluate(model, frame, FakeTexture(5), FakeTexture(6), extracted, true, false, {});
            Require(model.textures.at("DLSSNR.Depth") == (extracted ? extracted : frame.depth.texture) &&
                        model.unsignedValues.at("DLSSNR.DepthSubrectBaseX") == (extracted ? 0u : depthCrop.x) &&
                        model.unsignedValues.at("DLSSNR.DepthSubrectBaseY") == (extracted ? 0u : depthCrop.y) &&
                        model.unsignedValues.at("DLSSNR.DepthSubrectWidth") == right.width &&
                        model.unsignedValues.at("DLSSNR.DepthSubrectHeight") == right.height &&
                        model.integerValues.at("DLSSNR.DepthInverted") == 1,
                    "bind direct depth at native origin or private inverted depth at zero origin");
            Require(model.textures.at("DLSSNR.MVec") == frame.motion.texture &&
                        model.unsignedValues.at("DLSSNR.MVecSubrectBaseX") == 0 &&
                        model.unsignedValues.at("DLSSNR.MVecSubrectBaseY") == 0 &&
                        model.unsignedValues.at("DLSSNR.MVecSubrectWidth") == 2480 &&
                        model.unsignedValues.at("DLSSNR.MVecSubrectHeight") == 2656 &&
                        model.floatValues.at("DLSSNR.MVecScaleX") == -1.5f &&
                        model.floatValues.at("DLSSNR.MVecScaleY") == 2.f,
                    "preserve output-grid resource, zero origin, full extent and signed pixel scales");
        }
        frame.motion.active.x = 1;
        Require(!ModelMotionExtentEligible(frame, DXGI_FORMAT_R16G16_FLOAT), "reject unqualified offset output-grid X");
        frame.motion.active.x = 0;
        frame.motion.active.y = 1;
        Require(!ModelMotionExtentEligible(frame, DXGI_FORMAT_R16G16_FLOAT), "reject unqualified offset output-grid Y");
        frame.motion.active.y = 0;
        Require(!ModelMotionExtentEligible(frame, DXGI_FORMAT_R32G32_FLOAT),
                "output-sized admission requires qualified RG16 format");
        ++frame.output.active.width;
        Require(!ModelMotionExtentEligible(frame, DXGI_FORMAT_R16G16_FLOAT),
                "reject vectors with an unrecognized extent");
        --frame.output.active.width;
        for (float invalid : {0.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            frame.motionScaleY = invalid;
            Require(!ModelMotionExtentEligible(frame, DXGI_FORMAT_R16G16_FLOAT), "reject invalid native scale");
        }
        frame.motionScaleY = 2.f;
        frame.motion.active.x = UINT32_MAX;
        Require(!ModelMotionExtentEligible(frame, DXGI_FORMAT_R16G16_FLOAT), "reject overflowing native motion region");
        frame.motion.active = {44, 19, right.width, right.height};
        for (auto format : {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT}) {
            Require(ModelMotionExtentEligible(frame, format), "retain direct render-sized motion format");
            ParameterTable model;
            ConfigureModelEvaluate(model, frame, FakeTexture(5), FakeTexture(6), nullptr, true, false, {});
            Require(model.textures.at("DLSSNR.MVec") == frame.motion.texture &&
                        model.unsignedValues.at("DLSSNR.MVecSubrectBaseX") == 44 &&
                        model.unsignedValues.at("DLSSNR.MVecSubrectBaseY") == 19 &&
                        model.unsignedValues.at("DLSSNR.MVecSubrectWidth") == right.width &&
                        model.unsignedValues.at("DLSSNR.MVecSubrectHeight") == right.height &&
                        model.floatValues.at("DLSSNR.MVecScaleX") == -1.5f &&
                        model.floatValues.at("DLSSNR.MVecScaleY") == 2.f,
                    "pass through independently cropped render-grid motion");
        }
        Require(!ModelMotionExtentEligible(frame, DXGI_FORMAT_R16_FLOAT), "reject single-channel vector binding");
    }

    void CheckColorSubstitution(const ParameterTable& baseline, const NgxSrEvaluation& frame) {
        for (int scenario = 0; scenario != 8; ++scenario) {
            auto table = baseline;
            table.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 17u);
            auto original = frame.color;
            original.active.y = 17;
            const auto resolved = FakeTexture(5);
            ParameterTable other;
            NgxColorSubstitution color{&table, original, resolved};
            switch (scenario) {
            case 0: // Successful native evaluation.
            case 1: // Failing native evaluation.
            case 2: // Exceptional native evaluation.
                break;
            case 3: // Missing offset must remain absent.
                table.unsignedValues.erase(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y);
                break;
            case 4: // Wrong-type offset must remain the original type.
                table.unsignedValues.erase(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X);
                table.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 1600);
                break;
            case 5: // The color pointer changed after preparing model inputs.
                table.Set(NVSDK_NGX_Parameter_Color, FakeTexture(99));
                break;
            case 6: // Setter ignored the substitution after both offsets had changed.
                table.rejectedColor = resolved;
                break;
            case 7: // The read-only caller argument is not the proven mutable table.
                color.exclusiveTable = &other;
                break;
            }
            const auto saved = table;
            const bool shouldBind = scenario < 3;
            unsigned calls{};
            const auto native = [&] {
                ++calls;
                auto expected = saved;
                if (shouldBind) {
                    expected.Set(NVSDK_NGX_Parameter_Color, resolved);
                    expected.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
                    expected.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
                }
                Require(table.textures == expected.textures && table.unsignedValues == expected.unsignedValues &&
                            table.integerValues == expected.integerValues && table.floatValues == expected.floatValues,
                        "native SR receives exact color and origins with independent depth, motion and output intact");
                if (scenario == 2)
                    throw 7;
                return scenario == 1 ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success;
            };
            try {
                const auto result = EvaluateWithResolvedColor(&table, color, native);
                Require(color.restored && result == (scenario == 1 ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success),
                        "native result preserved and restoration verified");
            } catch (int failure) {
                Require(scenario == 2 && failure == 7, "native exception preserved");
            }
            Require(calls == 1 && color.restored && color.substituted == shouldBind &&
                        table.textures == saved.textures && table.unsignedValues == saved.unsignedValues &&
                        table.integerValues == saved.integerValues && table.floatValues == saved.floatValues,
                    "one original call and exact map restoration across success, fallback, failure and unwind");
        }

        auto table = baseline;
        table.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
        NgxColorSubstitution color{&table, frame.color, FakeTexture(5)};
        unsigned calls{};
        EvaluateWithResolvedColor(&table, color, [&] {
            ++calls;
            table.rejectedColor = frame.color.texture;
            return NVSDK_NGX_Result_Success;
        });
        Require(calls == 1 && color.substituted && !color.restored &&
                    table.unsignedValues.at(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X) ==
                        frame.color.active.x,
                "an SDK refusing restoration is reported after attempting all keys");

        table = baseline;
        table.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
        table.rejectNonzeroColorX = true;
        color = {&table, frame.color, FakeTexture(5)};
        table.rejectedColor = color.resolved;
        calls = 0;
        const auto result = EvaluateWithResolvedColor(&table, color, [&] {
            ++calls;
            return NVSDK_NGX_Result_Success;
        });
        Require(!color.substituted && !color.restored && calls == 0 && result == NVSDK_NGX_Result_FAIL_InvalidParameter,
                "a failed substitution with an unrestored offset never reaches native SR");
    }
} // namespace

int main() {
    CheckColorGuides();
    ParameterTable parameters;
    parameters.Set(NVSDK_NGX_Parameter_Width, 1600u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 1280u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 2400u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 1920u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 3);
    const auto feature = ReadNgxSrFeature(parameters);
    Require(feature && feature->maxRender.width == 1600 && feature->output.height == 1920 &&
                feature->createFlags == 3 && !feature->outputSubrects,
            "capture native SR creation contract");
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1u);
    Require(ReadNgxSrFeature(parameters)->outputSubrects, "honor older unsigned output subrects permission");
    parameters.unsignedValues.erase(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects);
    // The native D3D12 SDK helper writes this create flag through SetI.
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);
    const auto subrectFeature = ReadNgxSrFeature(parameters);
    Require(subrectFeature && subrectFeature->outputSubrects, "honor signed SDK output subrects permission");

    parameters.Set(NVSDK_NGX_Parameter_Color, FakeTexture(1));
    parameters.Set(NVSDK_NGX_Parameter_Depth, FakeTexture(2));
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, FakeTexture(3));
    parameters.Set(NVSDK_NGX_Parameter_Output, FakeTexture(4));
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 1200u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 960u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 1600u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 1680u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 1728u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 2432u);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, -1.25f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.5f);
    parameters.Set(NVSDK_NGX_Parameter_Reset, 1);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 2.5f);
    const auto eval = ReadNgxSrEvaluation(parameters, *subrectFeature);
    Require(eval && eval->color.texture == FakeTexture(1) && eval->color.active.x == 1600 &&
                eval->depth.active.x == 1680 && eval->motion.active.x == 1728 && eval->motion.active.width == 1200 &&
                eval->output.active.x == 2432 && eval->output.active.width == 2400 && eval->motionScaleX == -1.25f &&
                eval->motionScaleY == 1.5f && eval->reset && eval->preExposure == 2.5f,
            "preserve independent substrate origins and motion scales");
    CheckColorSubstitution(parameters, *eval);

    parameters.unsignedValues.erase(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height);
    Require(!ReadNgxSrEvaluation(parameters, *feature), "reject partial dynamic dimensions");
    parameters.unsignedValues.erase(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width);
    Require(ReadNgxSrEvaluation(parameters, *feature)->color.active.width == 1600,
            "use creation extent for the whole image when both dynamic dimensions are absent");
    parameters.textures.erase(NVSDK_NGX_Parameter_Depth);
    Require(!ReadNgxSrEvaluation(parameters, *feature), "missing depth passes through");
    parameters.unsignedValues.erase(NVSDK_NGX_Parameter_OutHeight);
    Require(!ReadNgxSrFeature(parameters), "incomplete creation params cannot be registered");
    std::cout << "PASS: before-SR NGX input binding\n";
}
