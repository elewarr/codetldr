#pragma once

namespace codetldr {

/// Model loading status for the embedding model.
/// Phase 15+ will implement the full ModelManager with ONNX Runtime loading.
enum class ModelStatus {
    not_configured,     // No model path configured
    loaded,             // Model loaded and ready
    model_not_installed, // Model file missing
    load_failed,        // Model file present but failed to load
};

/// ModelManager: manages the lifecycle of the ONNX embedding model.
/// Phase 15+ will implement the full ModelManager.
/// Phase 22 uses only the status() method for observability.
class ModelManager {
public:
    ModelManager() = default;
    ~ModelManager() = default;

    /// Return the current model loading status.
    ModelStatus status() const noexcept { return status_; }

private:
    ModelStatus status_ = ModelStatus::not_configured;
};

} // namespace codetldr
