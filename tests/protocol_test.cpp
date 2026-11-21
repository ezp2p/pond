/* The Puddle Protocol's test vectors (ezp2p/protocol, vectors/), made by Puddle: Pond must read request signatures and
 * receipts exactly as Puddle writes them. Reads them from the submodule (third_party/protocol), or
 * POND_PROTOCOL_DIR pointing at one; skipped (77) without. No database or pondd: these are the checks alone. */

#include "encoding.h"
#include "identity.h"
#include "json.h"
#include "reqsig.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace pond;

static int failures, checks;

static void expect(bool ok, const std::string &what)
{
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what.c_str());
        failures++;
    }
}

static bool load(const std::string &path, Json::Value &out)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return f && parse_object(ss.str(), out);
}

static const char *said(Authn a)
{
    switch (a) {
    case Authn::OK: return "ok";
    case Authn::REJECT: return "reject";
    case Authn::SKEW: return "clock_skew";
    case Authn::REPLAYED: return "replayed";
    case Authn::BUSY: return "busy";
    }
    return "?";
}

static const char *said(Receipt r)
{
    switch (r) {
    case Receipt::OK: return "ok";
    case Receipt::MISSING: return "missing";
    case Receipt::WRONG_KEY: return "wrong_key";
    case Receipt::BAD: return "bad";
    }
    return "?";
}

int main()
{
    const char *env = getenv("POND_PROTOCOL_DIR");
    std::string dir = env && *env ? std::string(env) + "/vectors" : std::string(POND_SOURCE_DIR) + "/third_party/protocol/vectors";
    if (!std::filesystem::exists(dir + "/signatures.json")) {
        printf("protocol_test: skipped, no protocol checkout at %s\n", dir.c_str());
        return 77;
    }
    Json::Value v;
    if (!load(dir + "/signatures.json", v)) {
        fprintf(stderr, "protocol_test: cannot read %s/signatures.json\n", dir.c_str());
        return 1;
    }
    int64_t now = v["now_ms"].asInt64();
    unsigned char sender[32], responder[32];
    expect(unhex(v["sender"]["public_key"].asString(), sender, 32), "the sender's key");
    expect(unhex(v["responder"]["public_key"].asString(), responder, 32), "the responder's key");
    /* ids are made from keys as the protocol says (spec/01): a device or Pond id is base32(SHA-256(key)), but a Puddle's id
     * comes from its chain, so the vectors give it */
    std::string sender_id = v["sender"]["id"].asString(), responder_id = v["responder"]["id"].asString();
    expect(id_valid(sender_id) && id_valid(responder_id), "the ids");

    for (const auto &t : v["requests"]) {
        std::string name = t["name"].asString();
        KeyLookup lookup = [&](const std::string &id, const std::string &kid, unsigned char out[32]) {
            if (id != sender_id || kid != key_id(sender))
                return false;
            memcpy(out, sender, 32);
            return true;
        };
        Replay replay; /* each vector is a first sight */
        ReqSig sig;
        std::string me = t["accepts_any"].asBool() ? "*" : t["checked_by"].asString();
        Authn a = authenticate(t["header"].asString(), t["method"].asString(), t["target"].asString(), t["body"].asString(), me, lookup,
                               replay, now, sig);
        expect(said(a) == t["expect"].asString(), "request \"" + name + "\": said " + said(a) + ", expected " + t["expect"].asString());
    }
    for (const auto &t : v["receipts"]) {
        std::string name = t["name"].asString();
        Receipt r = check_receipt(t["header"].asString(), responder_id, responder, t["nonce"].asString(), t["status"].asInt(),
                                  t["body"].asString());
        expect(said(r) == t["expect"].asString(), "receipt \"" + name + "\": said " + said(r) + ", expected " + t["expect"].asString());
    }
    expect(v["requests"].size() > 0 && v["receipts"].size() > 0, "the file has vectors");

    if (failures) {
        fprintf(stderr, "protocol_test: %d of %d failed\n", failures, checks);
        return 1;
    }
    printf("protocol_test: ok (%d checks)\n", checks);
    return 0;
}
