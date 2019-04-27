#include "UpdateChecker.h"
#include <Exception.h>
#include <version.h>
#include <HttpRequest.h>
#include <cstring>

#define THROW_EXCEPTION_WITH_WIN32_ERROR() THROW_EXCEPTION("win32 error %lu", GetLastError())

#define UPDATE_CHECK_ENDPOINT_URL "https://ravin.tk/api/rf/dashfaction/checkupdate.php"

void UpdateChecker::checkAsync(std::function<void()> callback)
{
    m_abort = false;
    m_callback = callback;
    std::thread th(&UpdateChecker::threadProc, this);
    m_thread = std::move(th);
}

void UpdateChecker::abort()
{
    m_abort = true;
}

bool UpdateChecker::check()
{
    HttpSession session{"DashFaction"};
    auto url = UPDATE_CHECK_ENDPOINT_URL "?version=" VERSION_STR;

    HttpRequest req{url, "GET", session};
    req.send();

    if (m_abort)
        return false;

    char buf[4096];
    size_t bytes_read = req.read(buf, sizeof(buf) - 1);
    buf[bytes_read] = 0;

    if (m_abort)
        return false;

    if (!buf[0])
        m_new_version = false;
    else {
        // printf("%s\n", buf);x
        char* url = buf;
        char* msg_text = std::strchr(buf, '\n');
        if (msg_text) {
            *msg_text = 0;
            ++msg_text;
        }
        m_new_version = true;
        m_url = url;
        m_message = msg_text;
    }

    return m_new_version;
}

void UpdateChecker::threadProc()
{
    try {
        check();
    }
    catch (const std::exception& e) {
        m_error = e.what();
    }

    if (!m_abort)
        m_callback();
}