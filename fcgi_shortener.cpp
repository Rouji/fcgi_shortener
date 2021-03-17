#include <fcgio.h>
#include <stdlib.h>
#include <thread>
#include <vector>
#include <mutex>
#include <string>
#include <filesystem>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>

#include "lmdbpp/lmdbpp.h"

#define EOL "\r\n"
#define DOUBLE_EOL EOL EOL
#define NUM_TRIES 3


struct ThreadContext
{
    lmdbpp::Env& env;
    lmdbpp::Dbi dbi_shorts;
    int link_len;
    std::mutex key_mutex;
};

std::string rnd_link(int length)
{
    static constexpr std::string_view link_alphabet{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz01234567890_-"};
    constexpr auto alph_len = link_alphabet.length();

    std::string rnd{};
    for (auto i = 0; i < length; ++i)
        rnd += link_alphabet[rand()%alph_len];
    return rnd;
}

//major jank
const std::string_view multipart_formdata_first_value(const std::string_view& body)
{
    auto first_value_idx = body.find(DOUBLE_EOL) + strlen(DOUBLE_EOL);
    return body.substr(first_value_idx, body.find(EOL, first_value_idx) - first_value_idx);
}

void fcgi_thread(ThreadContext* context)
{
    FCGX_Request req;
    FCGX_InitRequest(&req, 0, 0);
    char buf[1024];
    while (true)
    {
        if (FCGX_Accept_r(&req) < 0)
            break;

        auto method = FCGX_GetParam("REQUEST_METHOD", req.envp);
        if (std::strcmp(method, "POST") == 0)
        {
            auto content_type = FCGX_GetParam("CONTENT_TYPE", req.envp);
            size_t len = FCGX_GetStr(buf, sizeof(buf), req.in);
            const std::string_view body{buf, len};
            auto link = multipart_formdata_first_value(body);
            auto link_len = context->link_len;
            auto tries = NUM_TRIES;

            while (true)
            {
                try
                {
                    auto rnd = rnd_link(link_len);

                    {
                        std::lock_guard<std::mutex> guard{context->key_mutex};
                        lmdbpp::Txn{context->env}.put(context->dbi_shorts, lmdbpp::Val{rnd}, lmdbpp::Val{link});
                    }

                    FCGX_FPrintF(
                        req.out,
                        "Content-type: text/plain; charset=utf-8\r\n"
                        "\r\n"
                        "%s://%s/%s",
                        FCGX_GetParam("REQUEST_SCHEME", req.envp),
                        FCGX_GetParam("HOST", req.envp),
                        rnd.c_str()
                    );
                    break;
                }
                catch (lmdbpp::KeyExistsError &e)
                {
                    if (--tries == 0)
                    {
                        tries = NUM_TRIES;
                        ++link_len;
                    }
                }
            }
        }
        else
        {
            auto uri = std::string_view{FCGX_GetParam("DOCUMENT_URI", req.envp)};
            if (uri == "/")
            {
                FCGX_FPrintF(
                    req.out,
                    "Content-type: text/html; charset=utf-8\r\n"
                    "\r\n"
                    R"EOF(
<!DOCTYPE html>
<html lang="en">
<head>
    <title>URL Shortener</title>
    <meta name="description" content="URL shortener. Shortens URLs." />
</head>
<body>
<pre>
Shorten a URL using curl:
curl -F "url=https://example.com/cats.jpg" %s://%s/

Or use this form:
</pre>
<form id="frm" method="post" enctype="multipart/form-data">
<input type="text" name="url" id="url" placeholder="https://example.com/cats.jpg" />
<input type="submit" value="shorten"/>
</form>
</body>
</html>
)EOF"
                    "\r\n",
                    FCGX_GetParam("REQUEST_SCHEME", req.envp),
                    FCGX_GetParam("HOST", req.envp)
                );
            }
            else
            {
                auto key = uri.substr(uri.find_first_not_of('/'));
                try
                {
                    lmdbpp::Txn txn{context->env, MDB_RDONLY};
                    auto link = txn.get<const char, char>(context->dbi_shorts, lmdbpp::Val{key}).to_str();
                    FCGX_FPrintF(
                        req.out,
                        "Status: 301\r\n"
                        "Content-type: text/plain; charset=utf-8\r\n"
                        "Location: %s\r\n"
                        "\r\n"
                        "%s\r\n",
                        link.c_str(),
                        link.c_str()
                    );
                }
                catch (lmdbpp::NotFoundError& e)
                {
                    FCGX_FPrintF(
                        req.out,
                        "Status: 404\r\n"
                        "Content-type: text/plain; charset=utf-8\r\n"
                        "\r\n"
                        "Error 404: Not found"
                    );
                }
            }
        }

        FCGX_Finish_r(&req);
    }
}

int main(int argc, char* argv[])
{
    if (argc != 4)
    {
        std::cerr<<"Usage: "<<argv[0]<<" DB_PATH THREADS LINK_LENGTH" << std::endl;
        return 1;
    }

    auto db = argv[1];
    auto num_threads = std::atoi(argv[2]);
    auto link_len = std::atoi(argv[3]);
    srand(time(0));

    FCGX_Init();
    std::filesystem::create_directory(db);
    lmdbpp::Env env{db, lmdbpp::EnvArgs{.mapsize = (size_t)std::pow(1024, 4)}};
    ThreadContext context{env, 0, link_len};
    {
        lmdbpp::Txn txn{env, MDB_RDONLY};
        context.dbi_shorts = txn.open_dbi();
    }

    std::vector<std::thread> threads{};
    for (auto i = 0; i < num_threads; ++i)
        threads.emplace_back(fcgi_thread, &context);
    for (auto& t : threads)
        t.join();
    return 0;
}
