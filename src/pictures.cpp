/* Pictures (docs/design.md, "Pictures"): the Pond's icon, and the pictures in members' profiles. A picture is an image of at most
 * PICTURE_MAX bytes, kept once by its SHA-256 in data_dir/pictures/<hash>: the same hash a Puddle names it by in a profile, so a
 * member's Puddle sends its picture here and names it in the profile it shows the Pond, as it does for a friend, and every other
 * member's Puddle fetches it from here. Nothing about pictures is in the database: the icon is a setting, a profile is a member's,
 * and the file's kind is read from its first bytes. */

#include "pond.h"

#include "encoding.h"
#include "json.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pond {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;

const char *picture_kind(std::string_view b)
{
    auto starts = [&](const char *magic, size_t n, size_t at = 0) { return b.size() >= at + n && !memcmp(b.data() + at, magic, n); };
    if (starts("\x89PNG\r\n\x1a\n", 8))
        return "image/png";
    if (starts("\xff\xd8\xff", 3))
        return "image/jpeg";
    if (starts("GIF87a", 6) || starts("GIF89a", 6))
        return "image/gif";
    if (starts("RIFF", 4) && starts("WEBP", 4, 8))
        return "image/webp";
    return nullptr;
}

bool picture_hash_ok(const std::string &h)
{
    if (h.size() != 64)
        return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

std::string Service::picture_path(const std::string &hash) const
{
    return pictures_dir_ + "/" + hash;
}

/* The picture's bytes, if it is here: false if not. */
static bool read_all(const std::string &path, std::string &out, size_t max)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size <= max;
    if (ok) {
        out.resize((size_t)st.st_size);
        ok = pread(fd, out.data(), out.size(), 0) == (ssize_t)out.size();
    }
    close(fd);
    return ok;
}

bool Service::picture_ref(const std::string &hash, std::string &json) const
{
    std::string head;
    struct stat st;
    if (!picture_hash_ok(hash) || stat(picture_path(hash).c_str(), &st) != 0 || st.st_size <= 0)
        return false;
    int fd = open(picture_path(hash).c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    head.resize(16);
    ssize_t n = pread(fd, head.data(), head.size(), 0);
    close(fd);
    head.resize(n > 0 ? (size_t)n : 0);
    const char *kind = picture_kind(head);
    if (!kind)
        return false;
    json = "{\"hash\":" + jstr(hash) + ",\"mime\":" + jstr(kind) + ",\"size\":" + std::to_string((int64_t)st.st_size) + "}";
    return true;
}

Outcome Service::store_picture(const std::string &bytes, std::string &hash)
{
    if (bytes.empty() || bytes.size() > PICTURE_MAX)
        return "a picture is at most " + std::to_string(PICTURE_MAX >> 10) + " KiB";
    if (!picture_kind(bytes))
        return "a picture is a PNG, JPEG, GIF or WebP image";
    hash = sha256_hex(bytes);
    std::string path = picture_path(hash);
    if (access(path.c_str(), F_OK) == 0) {
        utimes(path.c_str(), nullptr); /* sent again: it is wanted, so the sweep counts from now */
        return "";
    }
    mkdir(pictures_dir_.c_str(), 0755);
    std::string part = path + ".part" + std::to_string(now_ms());
    FILE *f = fopen(part.c_str(), "wb");
    bool ok = f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (f)
        ok = (fclose(f) == 0) && ok;
    if (!ok || rename(part.c_str(), path.c_str()) != 0) {
        LOG_ERROR << "cannot write " << path << ": " << strerror(errno);
        unlink(part.c_str());
        return "the Pond could not keep it";
    }
    return "";
}

Task<Answer> Service::picture_put(std::string member, std::string bytes)
{
    if (!chunk_limit_.take(member, now_ms()))
        co_return fail(429, "slow_down");
    if (bytes.size() > PICTURE_MAX)
        co_return Answer{413, "{\"error\":\"too_large\",\"max\":" + std::to_string(PICTURE_MAX) + "}"};
    std::string hash, ref;
    if (!picture_kind(bytes))
        co_return fail(400, "bad_picture");
    Outcome o = store_picture(bytes, hash);
    if (!o.empty() || !picture_ref(hash, ref))
        co_return fail(500, "disk");
    co_return Answer{200, ref};
}

/* Is the picture shown to the members: the Pond's icon or banner, or in the profile of someone who is a member? */
Task<bool> Service::picture_shown(std::string hash)
{
    std::string icon = co_await setting("icon");
    std::string banner = co_await setting("banner");
    if (icon == hash || banner == hash)
        co_return true;
    /* the hash is 64 hex characters (checked), so it can only match where a profile names it */
    Result r = co_await db_->execSqlCoro("SELECT 1 FROM members WHERE left_ms IS NULL AND profile LIKE CONCAT('%', ?, '%') LIMIT 1", hash);
    co_return !r.empty();
}

Task<Answer> Service::picture_get(std::string member, std::string hash)
{
    if (!picture_hash_ok(hash))
        co_return fail(404, "no_picture");
    if (!chunk_limit_.take(member, now_ms()))
        co_return fail(429, "slow_down");
    try {
        bool shown = co_await picture_shown(hash);
        std::string bytes;
        if (!shown || !read_all(picture_path(hash), bytes, PICTURE_MAX))
            co_return fail(404, "no_picture");
        co_return Answer{200, std::move(bytes), true};
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "database: " << e.base().what();
        co_return fail(500, "database");
    }
}

Task<Outcome> Service::set_icon(std::optional<std::string> hash, std::string which)
{
    std::string ref;
    if (hash && !picture_ref(*hash, ref))
        co_return "no such picture: send it first";
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        if (hash)
            co_await tx->execSqlCoro("REPLACE INTO settings (name, value) VALUES (?, ?)", which, *hash);
        else
            co_await tx->execSqlCoro("DELETE FROM settings WHERE name = ?", which);
        std::string pj = co_await pond_json(tx);
        int64_t ev = co_await add_event(tx, "pond", pj);
        if (!co_await Committed(std::move(tx)))
            co_return "the database did not commit it";
        bump(ev);
        co_return "";
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return std::string("the database refused it: ") + e.base().what();
    }
}

Task<Answer> Service::admin_icon(std::string by, std::optional<std::string> hash, std::string which)
{
    if (hash && !picture_hash_ok(*hash))
        co_return fail(400, "bad_request");
    Outcome o = co_await set_icon(hash, which);
    if (!o.empty())
        co_return Answer{400, "{\"error\":\"bad_request\",\"message\":" + jstr(o) + "}"};
    std::string detail = hash ? *hash : std::string("removed");
    co_await log_admin(by, "set_" + which, "", detail);
    co_return Answer{204, ""};
}

/* Pictures nobody is shown and that came more than FILE_UNSENT_KEEP_MS ago: sent and never used, or replaced. Run hourly. */
Task<> Service::sweep_pictures()
{
    DIR *d = opendir(pictures_dir_.c_str());
    if (!d)
        co_return;
    std::vector<std::string> old;
    int64_t before = now_ms() - FILE_UNSENT_KEEP_MS;
    while (struct dirent *e = readdir(d)) {
        std::string name = e->d_name;
        struct stat st;
        bool part = name.find(".part") != std::string::npos;
        if ((picture_hash_ok(name) || part) && stat((pictures_dir_ + "/" + name).c_str(), &st) == 0 &&
            (int64_t)st.st_mtime * 1000 < before && old.size() < 1000)
            old.push_back(name);
    }
    closedir(d);
    int gone = 0;
    for (auto &name : old) {
        bool part = name.find(".part") != std::string::npos;
        if (!part) {
            bool shown = co_await picture_shown(name);
            if (shown)
                continue;
        }
        if (unlink((pictures_dir_ + "/" + name).c_str()) == 0)
            gone++;
    }
    if (gone)
        LOG_INFO << "let go of " << gone << " picture(s) nobody is shown";
}

} // namespace pond
