#define VDXR_PREBEGIN_CANCEL_REGRESSION 1
#define VDXR_PREBEGIN_ENTRY_NAME retainedPrebeginCancelMain
#include "nr_fg_prebegin_overlap_regression.cpp"
int main(int argc, char** argv) {
    const auto result = retainedPrebeginCancelMain(argc, argv);
    const auto& state = virtualdesktop_openxr::RuntimeFrameGenerationRegression::prebegin;
    if (result == 1 && state.cancelRetained && state.cleanupVerified) {
        std::cout << "PASS: cancellation after early publication settles both actual ticket owners\n";
        return 0;
    }
    return 1;
}
