/* pond.yaml's ports: chat (TCP) and voice (UDP), both 443 unless said, and the older names still read (docs/design.md,
 * "pond.yaml"). No database or pondd: only load_config. */

#include "harness.h"

#include "config.h"

#include <cstdlib>
#include <fstream>
#include <unistd.h>

using namespace pond;

static std::string dir;

/* load `yaml` (data_dir is added); false if it was refused, with the reason in `err` */
static bool load(const std::string &yaml, Config &c, std::string &err)
{
    std::string path = dir + "/pond.yaml";
    std::ofstream(path) << "data_dir: /nowhere\n" << yaml;
    c = Config{};
    return load_config(path, c, err);
}

int main()
{
    char tmpl[] = "/tmp/pond-config-XXXXXX";
    dir = mkdtemp(tmpl);
    Config c;
    std::string err;

    /* nothing said: 443 for both */
    CHECK(load("", c, err) && c.chat_port == 443 && c.voice_port == 443 && c.notes.empty());
    CHECK(link_address(c) == "127.0.0.1");

    /* chat alone: voice follows it */
    CHECK(load("ports:\n  chat: 8443\n", c, err) && c.chat_port == 8443 && c.voice_port == 8443);
    CHECK(link_address(c) == "127.0.0.1:8443");

    /* each its own */
    CHECK(load("ports:\n  chat: 8443\n  voice: 42069\n", c, err) && c.chat_port == 8443 && c.voice_port == 42069);
    CHECK(load("ports:\n  voice: 42069\n", c, err) && c.chat_port == 443 && c.voice_port == 42069);

    /* the older names still work, with a note to rename them */
    CHECK(load("port: 8443\n", c, err) && c.chat_port == 8443 && c.voice_port == 8443 && c.notes.size() == 1);
    CHECK(load("port: 8443\nvoice:\n  port: 42069\n", c, err) && c.chat_port == 8443 && c.voice_port == 42069 && c.notes.size() == 2);
    CHECK(load("ports:\n  chat: 8443\nvoice:\n  port: 42069\n  max_in_channel: 5\n", c, err) && c.voice_port == 42069 &&
          c.voice.max_in_channel == 5);

    /* the same setting twice, a typo, a bad number, or a single value */
    CHECK(!load("port: 8443\nports:\n  chat: 8443\n", c, err));
    CHECK(!load("ports:\n  voice: 1\nvoice:\n  port: 2\n", c, err));
    CHECK(!load("ports:\n  web: 8443\n", c, err) && err.find("ports.web") != std::string::npos);
    CHECK(!load("ports:\n  voice: 70000\n", c, err));
    CHECK(!load("ports: 443\n", c, err));

    /* voice: false still turns voice off, and the ports stay as they are */
    CHECK(load("voice: false\n", c, err) && !c.voice.on && c.chat_port == 443);

    unlink((dir + "/pond.yaml").c_str());
    rmdir(dir.c_str());
    return finish("config_test");
}
