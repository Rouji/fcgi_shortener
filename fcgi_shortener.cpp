#include <fcgio.h>
#include <stdlib.h>
#include <thread>
#include <vector>
#include <string>
#include <filesystem>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>

#include "lmdbpp/lmdbpp.h"

#define EOL "\r\n"
#define DOUBLE_EOL EOL EOL

#ifndef NUM_TRIES
#define NUM_TRIES 3
#endif

#ifndef LINK_ALPHABET
#define LINK_ALPHABET "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz01234567890_-"
#endif

std::hash<std::string_view> svhash{};

std::string rnd_link(int length)
{
    static constexpr std::string_view link_alphabet{LINK_ALPHABET};
    std::string rnd{};
    for (auto i = 0; i < length; ++i)
        rnd += link_alphabet[rand()%link_alphabet.length()];
    return rnd;
}

//major jank
const std::string_view multipart_formdata_first_value(const std::string_view& body)
{
    auto first_value_idx = body.find(DOUBLE_EOL) + strlen(DOUBLE_EOL);
    return body.substr(first_value_idx, body.find(EOL, first_value_idx) - first_value_idx);
}

class ShortenerThread
{
public:
    struct Context
    {
        lmdbpp::Env& env;
        lmdbpp::Dbi dbi_shorts;
        lmdbpp::Dbi dbi_reverse;
        int short_len;
    };

    ShortenerThread(Context& context) : _context(context) {}

    void run()
    {
        FCGX_InitRequest(&_req, 0, 0);
        while (true)
        {
            if (FCGX_Accept_r(&_req) < 0)
                break;

            auto method = FCGX_GetParam("REQUEST_METHOD", _req.envp);
            if (std::strcmp(method, "POST") == 0)
            {
                _store_link();
            }
            else
            {
                auto uri = std::string_view{FCGX_GetParam("DOCUMENT_URI", _req.envp)};
                if (uri == "/")
                {
                    _get_index();
                }
                else
                {
                    _get_link(uri);
                }
            }
            FCGX_Finish_r(&_req);
        }
    }
private:
    FCGX_Request _req;
    Context& _context;
    char _buf[1024];

    void _print_err(FCGX_Stream* stream, int status, const char* msg)
    {
        FCGX_FPrintF(
            stream,
            "Content-type: text/plain; charset=utf-8" EOL
            "Status: %d" EOL
            EOL
            "Error %d: %s",
            status, status, msg
        );
    }
    void _get_index()
    {
        FCGX_FPrintF(
            _req.out,
            "Content-type: text/html; charset=utf-8" EOL
            EOL
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
)EOF",
            FCGX_GetParam("REQUEST_SCHEME", _req.envp),
            FCGX_GetParam("HOST", _req.envp)
        );
    }

    void _print_short(const char* shrt)
    {
        FCGX_FPrintF(
            _req.out,
            "Content-type: text/plain; charset=utf-8" EOL
            EOL
            "%s://%s/%s",
            FCGX_GetParam("REQUEST_SCHEME", _req.envp),
            FCGX_GetParam("HOST", _req.envp),
            shrt
        );
    }

    void _store_link()
    {
        std::string_view content_type{FCGX_GetParam("CONTENT_TYPE", _req.envp)};
        if (content_type.find("multipart/form-data") == std::string_view::npos)
        {
            _print_err(_req.out, 400, "Unsupported request format");
            return;
        }

        size_t len = FCGX_GetStr(_buf, sizeof(_buf), _req.in);
        const std::string_view body{_buf, len};
        auto link = multipart_formdata_first_value(body);

        if (link.rfind("https://", 0) != 0 && link.rfind("http://", 0) != 0)
        {
            _print_err(_req.out, 400, "Invalid URL");
            return;
        }

        auto link_hash = svhash(link);
        lmdbpp::Txn txn{_context.env};
        try
        {
            auto reverse = txn.get<decltype(link_hash), const char>(_context.dbi_reverse, lmdbpp::Val{&link_hash});
            _print_short(reverse.to_str().c_str());
            return;
        }
        catch(lmdbpp::NotFoundError& e)
        {}

        auto short_len = _context.short_len;
        auto tries = NUM_TRIES;
        while (true)
        {
            try
            {
                auto rnd = rnd_link(short_len);
                txn.put(_context.dbi_shorts, lmdbpp::Val{rnd}, lmdbpp::Val{link});
                txn.put(_context.dbi_reverse, lmdbpp::Val{&link_hash}, lmdbpp::Val{rnd});
                _print_short(rnd.c_str());
                return;
            }
            catch (lmdbpp::KeyExistsError &e)
            {
                if (--tries == 0)
                {
                    tries = NUM_TRIES;
                    ++short_len;
                }
            }
        }
    }

    void _get_link(const std::string_view& uri)
    {
        auto key = uri.substr(uri.find_first_not_of('/'));
        try
        {
            lmdbpp::Txn txn{_context.env, MDB_RDONLY};
            auto link = txn.get<const char, char>(_context.dbi_shorts, lmdbpp::Val{key}).to_str();
            FCGX_FPrintF(
                _req.out,
                "Status: 301\r\n"
                "Content-type: text/plain; charset=utf-8\r\n"
                "Location: %s\r\n"
                "\r\n"
                "%s",
                link.c_str(),
                link.c_str()
            );
        }
        catch (lmdbpp::NotFoundError& e)
        {
            _print_err(_req.out, 404, "Not Found");
        }
    }
};

int main(int argc, char* argv[])
{
    if (argc != 4)
    {
        std::cerr<<"Usage: "<<argv[0]<<" DB_PATH THREADS MIN_LENGTH" << std::endl;
        return 1;
    }

    auto db = argv[1];
    auto num_threads = std::atoi(argv[2]);
    auto short_len = std::atoi(argv[3]);
    srand(time(0));

    FCGX_Init();
    std::filesystem::create_directory(db);
    lmdbpp::Env env{db, lmdbpp::EnvArgs{.mapsize = (size_t)std::pow(1024, 4), .maxdbs = 2}};
    ShortenerThread::Context context{env};
    context.short_len = short_len;
    {
        lmdbpp::Txn txn{env};
        context.dbi_shorts = txn.open_dbi("shorts", lmdbpp::DbiFlags::CREATE);
        context.dbi_reverse = txn.open_dbi("reverse", lmdbpp::DbiFlags::CREATE | lmdbpp::DbiFlags::INTEGERKEY);
    }

    std::vector<std::thread> threads{};
    for (auto i = 0; i < num_threads; ++i)
        threads.emplace_back([&context]{ShortenerThread(context).run();});
    for (auto& t : threads)
        t.join();
    return 0;
}
