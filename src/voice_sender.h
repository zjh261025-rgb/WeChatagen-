#pragma once
#include <mutex>
#include "httplib.h"

namespace WeChatAgentVoice {
    // Live shares the existing send lock; dry-run never acquires it.
    void Handle(const httplib::Request& request,httplib::Response& response,std::timed_mutex& nativeSendMutex);
}
