#pragma once
namespace WeChatAgentVoiceProbe {
    // Caller must be on the existing deferred worker, after shared MinHook initialization.
    void Initialize() noexcept;
}
