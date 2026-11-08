// An expected SDK failure is a PASS only after both real ticket owners are
// witnessed before cleanup and the real public session cleanup releases both.
#define VDXR_PREBEGIN_FAILURE_REGRESSION 1
#define VDXR_PREBEGIN_ENTRY_NAME retainedPrebeginFailureMain
#include "nr_fg_prebegin_overlap_regression.cpp"
int main(int argc, char** argv) {
    const auto result = retainedPrebeginFailureMain(argc, argv);
    const auto& state = virtualdesktop_openxr::RuntimeFrameGenerationRegression::prebegin;
    if (result == 1 && state.failureRetained && state.cleanupVerified) {
        std::cout << "PASS: before-first-Begin failure retains current and future tickets through joined cleanup\n";
        return 0;
    }
    return 1;
}
