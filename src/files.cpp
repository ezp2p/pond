/* Files members send (docs/design.md, "Files"). A file is chunks of FILE_CHUNK bytes, each with a SHA-256, and its id is the
 * SHA-256 of those hashes in order: the same id a Puddle gives it. A member begins one with its size and chunk hashes, sends the
 * chunks (each checked against its hash as it comes), and once all are here it is whole and a message may carry it. Members
 * fetch it the same way: the hashes, then the chunks.
 *
 *   data_dir/files/<id>            a whole file
 *   data_dir/files/partial/<id>    one arriving, each chunk written where it goes
 *
 * What is known of each (its size, hashes, which chunks are here, whether it is whole) is in the `files` table, and which
 * messages carry it in `message_files`. */

#include "pond.h"

#include "encoding.h"
#include "json.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pond {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;

static Answer server_error(const DrogonDbException &e)
{
    LOG_ERROR << "database: " << e.base().what();
    return fail(500, "database");
}

static bool id_ok(const std::string &id)
{
    if (id.size() != 64)
        return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

static int64_t chunk_count(int64_t size)
{
    return (size + FILE_CHUNK - 1) / FILE_CHUNK;
}

static int64_t chunk_len(int64_t size, int64_t n)
{
    return std::min(FILE_CHUNK, size - n * FILE_CHUNK);
}

std::string Service::file_path(const std::string &id, bool partial) const
{
    return files_dir_ + (partial ? "/partial/" : "/") + id;
}

/* {"missing": [chunk numbers not here yet]} from a `have` column */
static std::string missing_json(const std::string &have)
{
    std::string out = "{\"missing\":[";
    bool first = true;
    for (size_t i = 0; i < have.size(); i++)
        if (have[i] != '1') {
            out += (first ? "" : ",") + std::to_string(i);
            first = false;
        }
    return out + "]}";
}

Task<int64_t> Service::files_max()
{
    std::string v = co_await setting("files.max_size");
    co_return v.empty() ? FILES_DEFAULT_MAX : strtoll(v.c_str(), nullptr, 10);
}

Task<Answer> Service::file_begin(std::string member, std::string id, int64_t size, std::string hashes)
{
    if (!id_ok(id))
        co_return fail(400, "bad_file");
    try {
        int64_t max = co_await files_max();
        if (max == 0)
            co_return fail(409, "files_off");
        if (size < 1 || size > max)
            co_return Answer{413, "{\"error\":\"too_large\",\"max\":" + std::to_string(max) + "}"};
        int64_t count = chunk_count(size);
        if ((int64_t)hashes.size() != count * 32 || sha256_hex(hashes) != id)
            co_return fail(400, "bad_file"); /* these are not the hashes of this file */
        mkdir(files_dir_.c_str(), 0700);
        mkdir((files_dir_ + "/partial").c_str(), 0700);
        Result r = co_await db_->execSqlCoro("SELECT have, whole, size FROM files WHERE id = ?", id);
        if (!r.empty() && num(r[0][1]))
            co_return Answer{200, "{\"missing\":[]}"}; /* here already: nothing to send */
        if (r.empty()) {
            int fd = open(file_path(id, true).c_str(), O_WRONLY | O_CREAT, 0600);
            bool ok = fd >= 0 && ftruncate(fd, size) == 0;
            if (fd >= 0)
                close(fd);
            if (!ok) {
                LOG_ERROR << "cannot make " << file_path(id, true) << ": " << strerror(errno);
                co_return fail(500, "disk");
            }
            co_await db_->execSqlCoro("INSERT IGNORE INTO files (id, size, chunks, have, uploader, created_ms) VALUES (?, ?, ?, REPEAT('0', ?), ?, ?)",
                                      id, size, hashes, count, member, now_ms());
            r = co_await db_->execSqlCoro("SELECT have, whole, size FROM files WHERE id = ?", id);
        }
        if (r.empty())
            co_return fail(500, "database");
        if (num(r[0][2]) != size)
            co_return fail(409, "other_size"); /* someone began it with another size, which its last chunk will not fit */
        co_return Answer{200, missing_json(text(r[0][0]))};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::file_put(std::string member, std::string id, int64_t n, std::string bytes)
{
    if (!id_ok(id))
        co_return fail(400, "bad_file");
    if (!chunk_limit_.take(member, now_ms()))
        co_return fail(429, "slow_down");
    try {
        Result r = co_await db_->execSqlCoro("SELECT size, SUBSTRING(chunks, ? * 32 + 1, 32), whole FROM files WHERE id = ?", n, id);
        if (r.empty())
            co_return fail(404, "no_file"); /* begin it first */
        int64_t size = num(r[0][0]);
        if (num(r[0][2]))
            co_return Answer{204, ""};
        if (n < 0 || n >= chunk_count(size) || (int64_t)bytes.size() != chunk_len(size, n))
            co_return fail(400, "bad_chunk");
        unsigned char got[32];
        sha256(bytes.data(), bytes.size(), got);
        std::string want = r[0][1].as<std::string>();
        if (want.size() != 32 || memcmp(want.data(), got, 32) != 0)
            co_return fail(400, "bad_chunk");
        std::string partial = file_path(id, true);
        int fd = open(partial.c_str(), O_WRONLY);
        bool ok = fd >= 0 && pwrite(fd, bytes.data(), bytes.size(), n * FILE_CHUNK) == (ssize_t)bytes.size();
        if (fd >= 0)
            close(fd);
        if (!ok) {
            LOG_ERROR << "cannot write " << partial << ": " << strerror(errno);
            co_return fail(500, "disk");
        }
        co_await db_->execSqlCoro("UPDATE files SET have = INSERT(have, ? + 1, 1, '1') WHERE id = ? AND whole = 0", n, id);
        Result left = co_await db_->execSqlCoro("SELECT LOCATE('0', have) FROM files WHERE id = ? AND whole = 0", id);
        if (!left.empty() && num(left[0][0]) == 0) {
            /* the last chunk: the file is whole. Two last chunks at once both get here; the second finds it moved. */
            fd = open(partial.c_str(), O_RDONLY);
            if (fd >= 0) {
                fsync(fd);
                close(fd);
            }
            std::string whole = file_path(id, false);
            if (rename(partial.c_str(), whole.c_str()) != 0 && access(whole.c_str(), F_OK) != 0) {
                LOG_ERROR << "cannot move " << partial << ": " << strerror(errno);
                co_return fail(500, "disk");
            }
            co_await db_->execSqlCoro("UPDATE files SET whole = 1 WHERE id = ?", id);
            LOG_INFO << member << " sent file " << id.substr(0, 12) << "... (" << size << " bytes)";
        }
        co_return Answer{204, ""};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

/* A member may fetch a whole file that a message in a live channel carries, or that they sent themselves. */
Task<bool> Service::may_read(const std::string &member, const std::string &id)
{
    Result r = co_await db_->execSqlCoro(
        "SELECT 1 FROM files f WHERE f.id = ? AND f.whole = 1 AND (f.uploader = ? OR EXISTS (SELECT 1 FROM message_files mf"
        " JOIN messages m ON m.id = mf.message JOIN channels c ON c.id = m.channel WHERE mf.file = f.id AND c.deleted = 0))",
        id, member);
    co_return !r.empty();
}

Task<Answer> Service::file_chunks(std::string member, std::string id)
{
    if (!id_ok(id))
        co_return fail(404, "no_file");
    try {
        if (!co_await may_read(member, id))
            co_return fail(404, "no_file");
        Result r = co_await db_->execSqlCoro("SELECT chunks FROM files WHERE id = ?", id);
        if (r.empty())
            co_return fail(404, "no_file");
        co_return Answer{200, r[0][0].as<std::string>(), true};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::file_chunk(std::string member, std::string id, int64_t n)
{
    if (!id_ok(id))
        co_return fail(404, "no_file");
    if (!chunk_limit_.take(member, now_ms()))
        co_return fail(429, "slow_down");
    try {
        if (!co_await may_read(member, id))
            co_return fail(404, "no_file");
        Result r = co_await db_->execSqlCoro("SELECT size FROM files WHERE id = ?", id);
        int64_t size = r.empty() ? 0 : num(r[0][0]);
        if (n < 0 || n >= chunk_count(size))
            co_return fail(404, "no_chunk");
        std::string out(chunk_len(size, n), '\0');
        int fd = open(file_path(id, false).c_str(), O_RDONLY);
        bool ok = fd >= 0 && pread(fd, out.data(), out.size(), n * FILE_CHUNK) == (ssize_t)out.size();
        if (fd >= 0)
            close(fd);
        if (!ok) {
            LOG_ERROR << "cannot read " << file_path(id, false) << ": " << strerror(errno);
            co_return fail(500, "disk");
        }
        co_return Answer{200, std::move(out), true};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

/* Files no message carries, begun more than FILE_UNSENT_KEEP_MS ago: uploads never sent, or never finished. Run hourly. A send
 * holds a shared lock on each file it carries until it commits, so a file is never taken from under a message. */
Task<> Service::sweep_files()
{
    Result r = co_await db_->execSqlCoro("SELECT id FROM files f WHERE created_ms < ? AND NOT EXISTS"
                                         " (SELECT 1 FROM message_files mf WHERE mf.file = f.id) LIMIT 1000",
                                         now_ms() - FILE_UNSENT_KEEP_MS);
    int gone = 0;
    for (const auto &row : r) {
        std::string id = text(row[0]);
        Result d = co_await db_->execSqlCoro("DELETE FROM files WHERE id = ? AND NOT EXISTS (SELECT 1 FROM message_files WHERE file = ?)",
                                             id, id);
        if (d.affectedRows() == 1) {
            unlink(file_path(id, false).c_str());
            unlink(file_path(id, true).c_str());
            gone++;
        }
    }
    if (gone)
        LOG_INFO << "let go of " << gone << " file(s) no message carries";
}

Task<Outcome> Service::set_files_max(int64_t bytes)
{
    if (bytes < 0 || bytes > FILE_HARD_MAX)
        co_return "the largest file is at most " + std::to_string(FILE_HARD_MAX >> 20) + " MiB";
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        co_await tx->execSqlCoro("REPLACE INTO settings (name, value) VALUES ('files.max_size', ?)", std::to_string(bytes));
        int64_t ev = co_await add_event(tx, "pond", co_await pond_json(tx));
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

Task<std::pair<int64_t, int64_t>> Service::files_usage()
{
    Result r = co_await db_->execSqlCoro("SELECT COUNT(*), COALESCE(SUM(size), 0) FROM files WHERE whole = 1");
    co_return std::pair<int64_t, int64_t>{num(r[0][0]), num(r[0][1])};
}

} // namespace pond
