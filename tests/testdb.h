#pragma once

/* A MariaDB of a test's own: made in a fresh folder under /tmp, reached by its Unix socket only, with no passwords, and thrown
 * away when the test ends. Needs mariadb-install-db and mariadbd on the PATH (brew install mariadb). */

#include "config.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ftw.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

struct TestDb {
    std::string dir; /* short: a socket path must fit in about 100 bytes */
    pid_t pid = 0;
    pond::DbConfig cfg;

    bool start()
    {
        char tmpl[] = "/tmp/pondt.XXXXXX";
        if (!mkdtemp(tmpl))
            return false;
        dir = tmpl;
        std::string cmd = "mariadb-install-db --no-defaults --datadir=" + dir + "/data --auth-root-authentication-method=normal"
                          " --skip-test-db > " + dir + "/install.log 2>&1";
        if (system(cmd.c_str()) != 0) {
            fprintf(stderr, "mariadb-install-db failed: see %s/install.log\n", dir.c_str());
            return false;
        }
        std::string sock = dir + "/sock";
        pid = fork();
        if (pid == 0) {
            std::string data = "--datadir=" + dir + "/data", s = "--socket=" + sock, log = "--log-error=" + dir + "/server.log",
                        pidf = "--pid-file=" + dir + "/pid";
            execlp("mariadbd", "mariadbd", "--no-defaults", data.c_str(), s.c_str(), log.c_str(), pidf.c_str(), "--skip-networking",
                   "--skip-grant-tables", "--innodb-buffer-pool-size=16M", (char *)nullptr);
            _exit(127);
        }
        for (int i = 0; i < 200; i++) {
            struct stat st;
            if (stat(sock.c_str(), &st) == 0)
                break;
            usleep(50 * 1000);
        }
        cmd = "mariadb --no-defaults -S " + sock + " -u root -e 'CREATE DATABASE pond' > /dev/null 2>&1";
        for (int i = 0; i < 40 && system(cmd.c_str()) != 0; i++)
            usleep(100 * 1000);
        cfg.socket = sock;
        cfg.user = "root";
        cfg.name = "pond";
        return true;
    }

    ~TestDb()
    {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
        if (!dir.empty() && !getenv("POND_KEEP_TEST_DB"))
            nftw(dir.c_str(), [](const char *p, const struct stat *, int, struct FTW *) { return remove(p); }, 16, FTW_DEPTH | FTW_PHYS);
    }
};
