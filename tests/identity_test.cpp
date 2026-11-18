/* The parts of the Pond that must agree with Puddle byte for byte: base32 and base64, key chains (with rolls, and every way a
 * forged or damaged one fails), and request signatures. The chains here are made from Puddle's documented formats
 * (harness.h); e2e_test checks them against a real Puddle. */

#include "harness.h"

#include "identity.h"
#include "reqsig.h"
#include "seal.h"
#include "text.h"

using namespace pond;

static bool chain_ok(const std::string &text, ChainHead *head = nullptr)
{
    ChainHead h;
    std::string why;
    bool ok = verify_chain(text, h, why);
    if (head)
        *head = h;
    return ok;
}

/* `chain` with its `n`th line (0-based) changed by `edit`. */
static std::string with_line(const std::string &chain, size_t n, const std::function<void(std::string &)> &edit)
{
    std::vector<std::string> lines;
    size_t p = 0;
    while (p < chain.size()) {
        size_t nl = chain.find('\n', p);
        lines.push_back(chain.substr(p, nl - p));
        p = nl + 1;
    }
    edit(lines[n]);
    std::string out;
    for (auto &l : lines)
        out += l + "\n";
    return out;
}

static void replace(std::string &s, const std::string &from, const std::string &to)
{
    size_t at = s.find(from);
    if (at != std::string::npos)
        s.replace(at, from.size(), to);
}

/* Sealing (spec/09) is HPKE, and RFC 9180's own vectors for the suite (A.2.1) are what keep this and Puddle's agreeing. */
static std::string bytes_of(const char *hexs)
{
    std::string out(strlen(hexs) / 2, '\0');
    unhex(hexs, (unsigned char *)out.data(), out.size());
    return out;
}

static void sealing()
{
    std::string ikm_e = bytes_of("909a9b35d3dc4713a5e72a4da274b55d3d3821a37e5d099e74a647db583a904b"),
                ikm_r = bytes_of("1ac01f181fdf9f352797655161c58b75c656a6cc2716dcb66372da835542e1df"),
                info = bytes_of("4f6465206f6e2061204772656369616e2055726e"),
                pt = bytes_of("4265617574792069732074727574682c20747275746820626561757479");
    unsigned char sk_e[32], pk_e[32], sk_r[32], pk_r[32], enc[32];
    CHECK(x25519_derive(ikm_e.data(), 32, sk_e, pk_e) && hex(pk_e, 32) == "1afa08d3dec047a643885163f1180476fa7ddb54c6a8029ea33f95796bf2ac4a");
    CHECK(x25519_derive(ikm_r.data(), 32, sk_r, pk_r) && hex(sk_r, 32) == "8057991eef8f1f1af18f4a9491d16a1ce333f695d4db8e38da75975c4478e0fb");
    Hpke s, r;
    CHECK(hpke_sender_with(sk_e, pk_r, info, enc, s) && hpke_receiver(enc, sk_r, info, r));
    CHECK(hex(s.key, 32) == "ad2744de8e17f4ebba575b3f5f5a8fa1f69c2a07f6e7500bc60ca6e3e3ec1c91" && hex(s.base_nonce, 12) == "5c4d98150661b848853b547f");
    std::string ct, back;
    s.seq = r.seq = 1;
    CHECK(hpke_seal(s, bytes_of("436f756e742d31"), pt, ct) &&
          hex((const unsigned char *)ct.data(), ct.size()) ==
              "6b53c051e4199c518de79594e1c4ab18b96f081549d45ce015be002090bb119e85285337cc95ba5f59992dc98c");
    CHECK(hpke_open(r, bytes_of("436f756e742d31"), ct, back) && back == pt);
    unsigned char out[32];
    CHECK(hpke_export(s, bytes_of("54657374436f6e74657874"), out, 32) &&
          hex(out, 32) == "5acb09211139c43b3090489a9da433e8a30ee7188ba8b0a9a1ccf0c229283e53");
    /* answers: records bound to their status and place, and the last one marked */
    unsigned char key[32];
    CHECK(answer_key(s, key));
    std::string big(3 * SEAL_RECORD + 5, 'x'), sealed, opened;
    CHECK(seal_answer(key, 200, big, sealed) && sealed.size() == big.size() + 4 * SEAL_TAG);
    CHECK(open_answer(key, 200, sealed, opened) && opened == big);
    CHECK(!open_answer(key, 404, sealed, opened));
    CHECK(!open_answer(key, 200, sealed.substr(0, SEAL_RECORD + SEAL_TAG), opened)); /* cut short after a whole record */
    CHECK(seal_answer(key, 202, "", sealed) && sealed.size() == SEAL_TAG && open_answer(key, 202, sealed, opened) && opened.empty());
}

int main()
{
    sealing();
    /* ---- encodings (RFC 4648 test vectors) ---- */
    const unsigned char foobar[] = "foobar";
    CHECK(base32(foobar, 6) == "mzxw6ytboi");
    CHECK(base64(foobar, 6) == "Zm9vYmFy" && base64(foobar, 5) == "Zm9vYmE=" && base64(foobar, 4) == "Zm9vYg==");
    unsigned char out[6];
    CHECK(unbase64("Zm9vYg==", out, 4) && !memcmp(out, "foob", 4));
    CHECK(!unbase64("Zm9vYh==", out, 4)); /* stray bits */
    CHECK(!unbase64("Zm9vYg=", out, 4) && !unbase64("Zm9v Yg==", out, 4) && !unbase64("Zm9vYmE=", out, 4));
    CHECK(unhex("00ff10", out, 3) && out[1] == 0xff && !unhex("00FF10", out, 3));

    /* ---- text ---- */
    std::string t;
    CHECK(clean_text("  hi\r\nthere\x01  ", 100, true, t) && t == "hi\nthere");
    CHECK(clean_text("a\nb\tc", 100, false, t) && t == "a b c");
    CHECK(clean_text("\xff\xfe", 100, false, t) && t == "\xef\xbf\xbd\xef\xbf\xbd"); /* bad bytes become U+FFFD */
    CHECK(clean_text("\xc0\xaf", 100, false, t) && t == "\xef\xbf\xbd\xef\xbf\xbd");  /* overlong */
    CHECK(clean_text(std::string(4000, 'x') + "   ", 4000, false, t) && !clean_text(std::string(4001, 'x'), 4000, false, t));
    CHECK(clean_text("é", 1, false, t) && t == "é"); /* characters, not bytes */

    /* ---- key chains ---- */
    Identity ann;
    ChainHead h;
    CHECK(chain_ok(ann.chain, &h) && h.id == ann.id && h.seq == 0 && !memcmp(h.sign_pub, ann.sign.pub, 32));
    CHECK(id_valid(ann.id) && key_id(h.sign_pub) == ann.kid());
    std::string first = ann.chain;
    ann.roll_keys();
    ann.roll_keys();
    CHECK(chain_ok(ann.chain, &h) && h.id == ann.id && h.seq == 2 && !memcmp(h.sign_pub, ann.sign.pub, 32)); /* the id stays */
    CHECK(chain_ok(ann.chain + "\n\n")); /* blank lines are skipped */

    Identity ben;
    CHECK(!chain_ok(""));
    CHECK(!chain_ok("not json\n"));
    CHECK(!chain_ok(with_line(first, 0, [](std::string &l) { replace(l, "\"seq\":0", "\"seq\":1"); }))); /* the signature covers it */
    CHECK(!chain_ok(with_line(first, 0, [](std::string &l) { replace(l, "\"ts\":", "\"ts\": "); })));    /* exact bytes */
    CHECK(!chain_ok(with_line(ann.chain, 1, [&](std::string &l) { replace(l, "\"sig_roll\":\"", "\"sig_roll\":\"A"); })));
    CHECK(!chain_ok(first + ben.chain)); /* someone else's record does not follow on */
    {
        /* records out of order, or one left out */
        std::string a = with_line(ann.chain, 1, [](std::string &l) { l = ""; });
        CHECK(!chain_ok(a));
    }
    {
        /* a roll signed by a roll key nobody committed to: what a thief with only the daily keys could make */
        Identity thief = ben;
        thief.roll.make();
        thief.roll_keys();
        CHECK(!chain_ok(thief.chain));
    }

    /* ---- request signatures ---- */
    Identity cat;
    const std::string pond_id = ben.id, target = "/pond/v1/channels/1/send", body = "{\"text\":\"hi\"}";
    Replay replay;
    KeyLookup lookup = [&](const std::string &id, const std::string &kid, unsigned char pub[32]) {
        if (id != cat.id || kid != cat.kid())
            return false;
        memcpy(pub, cat.sign.pub, 32);
        return true;
    };
    ReqSig r;
    std::string header = cat.signature("POST", target, body, pond_id);
    CHECK(parse_reqsig(header, r) && r.id == cat.id && r.kid == cat.kid());
    CHECK(authenticate(header, "POST", target, body, pond_id, lookup, replay, wall_ms(), r) == Authn::OK);
    CHECK(authenticate(header, "POST", target, body, pond_id, lookup, replay, wall_ms(), r) == Authn::REPLAYED);
    header = cat.signature("POST", target, body, pond_id);
    CHECK(authenticate(header, "POST", target, body + " ", pond_id, lookup, replay, wall_ms(), r) == Authn::REJECT); /* the body */
    CHECK(authenticate(header, "POST", target + "?x=1", body, pond_id, lookup, replay, wall_ms(), r) == Authn::REJECT);
    CHECK(authenticate(header, "GET", target, body, pond_id, lookup, replay, wall_ms(), r) == Authn::REJECT);
    CHECK(authenticate(header, "POST", target, body, ann.id, lookup, replay, wall_ms(), r) == Authn::REJECT); /* someone else's */
    CHECK(authenticate(header, "POST", target, body, pond_id, lookup, replay, wall_ms() + 6 * 60 * 1000, r) == Authn::SKEW);
    header = ben.signature("POST", target, body, pond_id);
    CHECK(authenticate(header, "POST", target, body, pond_id, lookup, replay, wall_ms(), r) == Authn::REJECT); /* unknown sender */
    CHECK(!parse_reqsig("v1 id=" + cat.id + "; ts=1; kid=x; nonce=x; sig=x", r));                             /* out of order */
    header = cat.signature("GET", "/pond/v1/info", "", pond_id);
    CHECK(!parse_reqsig(header + " ", r) && parse_reqsig(header, r));

    return finish("identity_test");
}
