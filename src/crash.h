/* When pondd dies of a crash (a bad pointer, or glibc's "free(): ..." abort on a heap that is no longer sound), it says what it
 * was doing first: the requests it was answering at that moment, and where in pondd it was. It still dies, since nothing is
 * safe to trust after that, and Docker or the service manager starts it again; but the log then says which request to look
 * at instead of only "free(): invalid pointer". */
#pragma once

#include <string>

namespace pond {

/* once, early in main */
void crash_report_install();

/* held for as long as a request is being answered; a crash lists every one held at that moment */
class InFlight {
public:
    InFlight(const std::string &method, const std::string &path, const std::string &peer);
    ~InFlight();
    InFlight(const InFlight &) = delete;
    InFlight &operator=(const InFlight &) = delete;

private:
    int slot_;
};

} // namespace pond
