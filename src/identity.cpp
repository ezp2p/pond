#include "identity.h"

#include <puddle_proto.h>

#include <cstring>

/* Ids, keys and chains are libpuddle-proto's (third_party/protocol/lib): the same checks every Puddle and Levee makes. */

namespace pond {

bool id_valid(std::string_view s)
{
    return s.size() == ID_LEN && pp_id_valid(std::string(s).c_str());
}

std::string key_id(const unsigned char pub[32])
{
    char k[65];
    pp_key_id(pub, k);
    return k;
}

bool verify_signature(const unsigned char pub[32], std::string_view data, const unsigned char sig[64])
{
    return pp_ed25519_verify(pub, data.data(), data.size(), sig) == 1;
}

bool verify_chain(std::string_view text, ChainHead &out, std::string &why)
{
    pp_chain_head h;
    char err[200] = "";
    if (pp_chain_verify(text.data(), text.size(), nullptr, &h, err, sizeof err) != 0) {
        why = err;
        if (why.rfind("identity ", 0) == 0)
            why = why.substr(9);
        return false;
    }
    ChainHead c;
    c.id = h.id;
    c.seq = h.seq;
    memcpy(c.sign_pub, h.sign_pub, 32);
    out = c;
    return true;
}

} // namespace pond
