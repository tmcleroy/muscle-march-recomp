#include "wp/big_stack.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace wp {

namespace {

#ifdef _WIN32
DWORD WINAPI run(LPVOID parameter) {
    (*static_cast<const std::function<void()>*>(parameter))();
    return 0;
}
#else
void* run(void* parameter) {
    (*static_cast<const std::function<void()>*>(parameter))();
    return nullptr;
}
#endif

}

void run_with_stack(size_t stack_size, const std::function<void()>& work) {
#ifdef _WIN32
    HANDLE thread = CreateThread(nullptr, stack_size, run, const_cast<std::function<void()>*>(&work), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, stack_size);
    pthread_t thread;
    pthread_create(&thread, &attributes, run, const_cast<std::function<void()>*>(&work));
    pthread_attr_destroy(&attributes);
    pthread_join(thread, nullptr);
#endif
}

}
