/* The shop (docs/design.md, "Shop"): a shop page lists what the Pond sells, or takes donations, and the Pond takes the payment
 * through Stripe with the key its admins gave it. Everything but the card form happens here: the Pond keeps the items, makes
 * each order, makes a Stripe Payment Link for that order alone (one payment, then it closes; Stripe shows its own
 * confirmation, so nothing of ours need be public), checks with Stripe until it is paid, and records it. Stripe does the
 * card, the checks on the buyer and the money. The key never leaves the Pond: /info says only whether payments are on, and
 * whether they are tests. */

#include "pond.h"

#include "json.h"
#include "text.h"

#include <drogon/HttpClient.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>

namespace pond {

using drogon::orm::DrogonDbException;
using drogon::orm::Result;

static Answer server_error(const DrogonDbException &e)
{
    LOG_ERROR << "database: " << e.base().what();
    return fail(500, "database");
}

static Answer bad(const std::string &message)
{
    return {400, "{\"error\":\"bad_request\",\"message\":" + jstr(message) + "}"};
}

/* ---- Stripe ---- */

struct StripeReply {
    int status = 0; /* 0: no answer */
    Json::Value json;
};

/* One call to Stripe's API: form fields for a POST, none for a GET */
static Task<StripeReply> stripe(std::string api, std::string key, drogon::HttpMethod method, std::string path,
                                std::vector<std::pair<std::string, std::string>> fields)
{
    StripeReply out;
    try {
        auto client = drogon::HttpClient::newHttpClient(api);
        drogon::HttpRequestPtr req;
        if (method == drogon::Post) {
            req = drogon::HttpRequest::newHttpFormPostRequest();
            for (auto &[k, v] : fields)
                req->setParameter(k, v);
        } else {
            req = drogon::HttpRequest::newHttpRequest();
            std::string q;
            for (auto &[k, v] : fields)
                q += (q.empty() ? "" : "&") + drogon::utils::urlEncodeComponent(k) + "=" + drogon::utils::urlEncodeComponent(v);
            if (!q.empty())
                path += "?" + q;
        }
        req->setMethod(method);
        req->setPath(path);
        req->addHeader("Authorization", "Bearer " + key);
        auto resp = co_await client->sendRequestCoro(req, 20);
        out.status = (int)resp->statusCode();
        parse_object(resp->body(), out.json);
    } catch (const std::exception &e) {
        LOG_WARN << "shop: Stripe did not answer: " << e.what();
        out.status = 0;
    }
    co_return out;
}

static std::string stripe_error(const StripeReply &r)
{
    if (!r.status)
        return "Stripe did not answer";
    const std::string m = r.json["error"]["message"].isString() ? r.json["error"]["message"].asString() : "";
    return m.empty() ? "Stripe refused it (" + std::to_string(r.status) + ")" : "Stripe: " + m;
}

Task<Answer> Service::admin_stripe(std::string by, std::optional<std::string> key)
{
    std::string detail = "payments off";
    if (key) {
        const std::string &k = *key;
        if (k.size() < 20 || k.size() > 255 || (k.rfind("sk_", 0) != 0 && k.rfind("rk_", 0) != 0) ||
            !std::all_of(k.begin(), k.end(), [](char c) { return isalnum((unsigned char)c) || c == '_'; }))
            co_return bad("that is not a Stripe secret key (sk_...) or restricted key (rk_...)");
        /* ask Stripe once, so a mistyped key is said now, not when someone buys */
        std::vector<std::pair<std::string, std::string>> fields;
        fields.emplace_back("limit", "1");
        const std::string api = shop_settings.stripe_api;
        const StripeReply r = co_await stripe(api, k, drogon::Get, "/v1/payment_links", fields);
        if (r.status != 200)
            co_return Answer{409, "{\"error\":\"stripe_refused\",\"message\":" + jstr(stripe_error(r)) + "}"};
        detail = k.find("_live_") != std::string::npos ? "payments on (live)" : "payments on (test)";
    }
    TxPtr tx;
    try {
        tx = co_await db_->newTransactionCoro();
        if (key)
            co_await tx->execSqlCoro("REPLACE INTO settings (name, value) VALUES ('stripe_key', ?)", *key);
        else
            co_await tx->execSqlCoro("DELETE FROM settings WHERE name = 'stripe_key'");
        const std::string pj = co_await pond_json(tx);
        const int64_t ev = co_await add_event(tx, "pond", pj);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &e) {
        if (tx)
            tx->rollback();
        co_return server_error(e);
    }
    co_await log_admin(by, "set_stripe", "", detail);
    co_return Answer{204, ""};
}

/* ---- items ---- */

static std::string item_json(const drogon::orm::Row &r) /* id, title, description, image, price, custom, stock, active */
{
    return "{\"id\":" + std::to_string(num(r[0])) + ",\"title\":" + jstr(text(r[1])) + ",\"description\":" + jstr(text(r[2])) +
           ",\"image\":" + jstr(text(r[3])) + ",\"price\":" + std::to_string(num(r[4])) + ",\"custom\":" + (num(r[5]) ? "true" : "false") +
           ",\"stock\":" + (r[6].isNull() ? std::string("null") : std::to_string(num(r[6]))) + ",\"active\":" + (num(r[7]) ? "true" : "false") + "}";
}

static const char ITEM_COLS[] = "id, title, description, image, price, custom, stock, active";

Task<std::string> Service::shop_items_json(int64_t page, std::string member)
{
    const Powers p = co_await powers(member);
    const bool manages = p.has(MANAGE_SHOP);
    Result r = co_await db_->execSqlCoro(std::string("SELECT ") + ITEM_COLS + " FROM shop_items WHERE page = ? ORDER BY position, id", page);
    std::string out = ",\"items\":[";
    bool first = true;
    for (const auto &row : r) {
        if (!num(row[7]) && !manages) /* one taken off sale is for those who keep the shop */
            continue;
        out += std::string(first ? "" : ",") + item_json(row);
        first = false;
    }
    const std::string key = co_await setting("stripe_key");
    out += "],\"payments\":";
    out += key.empty() ? "false" : "true";
    co_return out;
}

/* `e` checked: "" if it may be set, else why not */
static std::string check_item(ItemEdit &e, bool creating)
{
    std::string t;
    if (creating && (!e.title || !e.price))
        return "an item has a title and a price";
    if (e.title) {
        if (!clean_text(*e.title, ITEM_TITLE_MAX, false, t) || t.empty())
            return "an item's title is 1 to 100 characters on one line";
        e.title = t;
    }
    if (e.description) {
        if (!clean_text(*e.description, ITEM_TEXT_MAX, true, t))
            return "an item's description is at most 4000 characters";
        e.description = t;
    }
    if (e.image) {
        if (!clean_text(*e.image, FILE_NAME_MAX, false, t))
            return "an item's picture is the name of one of the page's media";
        e.image = t;
    }
    if (e.price && (*e.price < 0 || *e.price > PRICE_MAX))
        return "a price is 0 to 99999999, in the currency's smallest unit (cents)";
    if (e.stock && *e.stock && (**e.stock < 0 || **e.stock > 1000000000))
        return "stock is a number, or none for no limit";
    return "";
}

Task<Answer> Service::admin_item_create(std::string by, int64_t page, ItemEdit e)
{
    const std::string why = check_item(e, true);
    if (!why.empty())
        co_return bad(why);
    TxPtr tx;
    int64_t id = 0;
    try {
        tx = co_await db_->newTransactionCoro();
        Result k = co_await tx->execSqlCoro("SELECT kind FROM pages WHERE id = ? FOR UPDATE", page);
        if (k.empty() || text(k[0][0]) != "shop") {
            tx->rollback();
            co_return fail(404, "no_page");
        }
        Result c = co_await tx->execSqlCoro("SELECT COUNT(*), COALESCE(MAX(position) + 1, 0) FROM shop_items WHERE page = ?", page);
        if (num(c[0][0]) >= ITEMS_PER_PAGE_MAX) {
            tx->rollback();
            co_return bad("a shop page has at most 200 items");
        }
        const std::optional<int64_t> stock = e.stock ? *e.stock : std::nullopt;
        const int64_t custom = e.custom.value_or(false) ? 1 : 0, active = e.active.value_or(true) ? 1 : 0;
        Result r = co_await tx->execSqlCoro("INSERT INTO shop_items (page, title, description, image, price, custom, stock, active, position)"
                                            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                            page, *e.title, e.description.value_or(""), e.image.value_or(""), *e.price, custom, stock, active,
                                            num(c[0][1]));
        id = (int64_t)r.insertId();
        const int64_t ev = co_await page_touched(tx, page, by);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &ex) {
        if (tx)
            tx->rollback();
        co_return server_error(ex);
    }
    co_await log_admin(by, "item_create", *e.title, std::to_string(*e.price));
    co_return Answer{200, "{\"id\":" + std::to_string(id) + "}"};
}

Task<Answer> Service::admin_item_edit(std::string by, int64_t page, int64_t item, ItemEdit e)
{
    const std::string why = check_item(e, false);
    if (!why.empty())
        co_return bad(why);
    TxPtr tx;
    std::string title;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro(std::string("SELECT ") + ITEM_COLS + " FROM shop_items WHERE id = ? AND page = ? FOR UPDATE", item, page);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "no_item");
        }
        title = e.title.value_or(text(r[0][1]));
        std::optional<int64_t> stock = r[0][6].isNull() ? std::nullopt : std::optional<int64_t>(num(r[0][6]));
        if (e.stock)
            stock = *e.stock;
        const int64_t custom = e.custom.value_or(num(r[0][5]) != 0) ? 1 : 0, active = e.active.value_or(num(r[0][7]) != 0) ? 1 : 0;
        co_await tx->execSqlCoro("UPDATE shop_items SET title = ?, description = ?, image = ?, price = ?, custom = ?, stock = ?, active = ? WHERE id = ?",
                                 title, e.description.value_or(text(r[0][2])), e.image.value_or(text(r[0][3])), e.price.value_or(num(r[0][4])),
                                 custom, stock, active, item);
        const int64_t ev = co_await page_touched(tx, page, by);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &ex) {
        if (tx)
            tx->rollback();
        co_return server_error(ex);
    }
    co_await log_admin(by, "item_edit", title, "");
    co_return Answer{204, ""};
}

/* Taken off the page; orders keep its title */
Task<Answer> Service::admin_item_delete(std::string by, int64_t page, int64_t item)
{
    TxPtr tx;
    std::string title;
    try {
        tx = co_await db_->newTransactionCoro();
        Result r = co_await tx->execSqlCoro("SELECT title FROM shop_items WHERE id = ? AND page = ? FOR UPDATE", item, page);
        if (r.empty()) {
            tx->rollback();
            co_return fail(404, "no_item");
        }
        title = text(r[0][0]);
        co_await tx->execSqlCoro("UPDATE shop_items SET page = 0, active = 0 WHERE id = ?", item);
        const int64_t ev = co_await page_touched(tx, page, by);
        if (!co_await Committed(std::move(tx)))
            co_return fail(500, "database");
        bump(ev);
    } catch (const DrogonDbException &ex) {
        if (tx)
            tx->rollback();
        co_return server_error(ex);
    }
    co_await log_admin(by, "item_delete", title, "");
    co_return Answer{204, ""};
}

/* ---- orders ---- */

static std::string order_json(const drogon::orm::Row &r, bool all) /* ORDER_COLS */
{
    const std::string status = text(r[6]);
    std::string out = "{\"id\":" + std::to_string(num(r[0])) + ",\"item\":" + std::to_string(num(r[1])) + ",\"title\":" + jstr(text(r[2])) +
                      ",\"quantity\":" + std::to_string(num(r[3])) + ",\"amount\":" + std::to_string(num(r[4])) + ",\"currency\":" + jstr(text(r[5])) +
                      ",\"status\":" + jstr(status) + ",\"live\":" + (num(r[8]) ? "true" : "false") + ",\"created\":" + std::to_string(num(r[9])) +
                      ",\"paid\":" + (r[10].isNull() ? std::string("null") : std::to_string(num(r[10])));
    if (status == "open") /* where to pay it: the buyer's alone */
        out += ",\"url\":" + jstr(text(r[7]));
    if (all)
        out += ",\"member\":" + jstr(text(r[11]));
    return out + "}";
}

static const char ORDER_COLS[] = "id, item, title, quantity, amount, currency, status, link_url, live, created_ms, paid_ms, member";

Task<Answer> Service::orders(std::string member, bool all)
{
    try {
        /* everyone's for those who keep the shop; their own for a member */
        const std::string who = all ? std::string("%") : member;
        Result r = co_await db_->execSqlCoro(std::string("SELECT ") + ORDER_COLS + " FROM shop_orders WHERE member LIKE ? ORDER BY id DESC LIMIT 200", who);
        std::string out = "{\"orders\":[";
        for (size_t i = 0; i < r.size(); i++)
            out += std::string(i ? "," : "") + order_json(r[i], all);
        co_return Answer{200, out + "]}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

Task<Answer> Service::shop_buy(std::string member, int64_t page, int64_t item, int64_t quantity, int64_t amount)
{
    if (quantity < 1 || quantity > 100)
        co_return bad("buy 1 to 100 at once");
    if (!send_limit_.take(member, now_ms()))
        co_return fail(429, "slow_down");
    std::string title, currency, key, pond_name;
    int64_t order = 0, unit = 0;
    try {
        const bool seen = co_await may_see_page(member, page);
        if (!seen)
            co_return fail(404, "no_page");
        key = co_await setting("stripe_key");
        if (key.empty())
            co_return fail(409, "no_payments");
        pond_name = co_await setting("name");
        Result pg = co_await db_->execSqlCoro("SELECT kind, COALESCE(data, '{}') FROM pages WHERE id = ?", page);
        if (pg.empty() || text(pg[0][0]) != "shop")
            co_return fail(404, "no_page");
        Json::Value d;
        parse_object(text(pg[0][1]), d);
        currency = d.get("currency", "usd").asString();
        Result r = co_await db_->execSqlCoro(std::string("SELECT ") + ITEM_COLS + " FROM shop_items WHERE id = ? AND page = ? AND active = 1", item, page);
        if (r.empty())
            co_return fail(404, "no_item");
        title = text(r[0][1]);
        const int64_t price = num(r[0][4]);
        const bool custom = num(r[0][5]) != 0;
        unit = custom ? amount : price;
        if (custom && (amount < price || amount < 1)) /* paid as wished: at least the price */
            co_return Answer{409, "{\"error\":\"too_little\",\"least\":" + std::to_string(std::max<int64_t>(price, 1)) + "}"};
        if (unit < 1 || unit * quantity > PRICE_MAX)
            co_return bad("that is not an amount Stripe takes");
        if (!r[0][6].isNull() && num(r[0][6]) < quantity)
            co_return fail(409, "sold_out");
        const int64_t live = key.find("_live_") != std::string::npos ? 1 : 0;
        Result ins = co_await db_->execSqlCoro("INSERT INTO shop_orders (item, member, title, quantity, amount, currency, status, live, created_ms)"
                                               " VALUES (?, ?, ?, ?, ?, ?, 'creating', ?, ?)",
                                               item, member, title, quantity, unit * quantity, currency, live, now_ms());
        order = (int64_t)ins.insertId();
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
    /* a price for this order, and a link that takes one payment for it */
    const std::string api = shop_settings.stripe_api;
    using Fields = std::vector<std::pair<std::string, std::string>>;
    Fields price_fields;
    price_fields.emplace_back("currency", currency);
    price_fields.emplace_back("unit_amount", std::to_string(unit));
    price_fields.emplace_back("product_data[name]", title);
    StripeReply price = co_await stripe(api, key, drogon::Post, "/v1/prices", price_fields);
    StripeReply link;
    if (price.status == 200 && price.json["id"].isString()) {
        const std::string pond_id = key_.id;
        Fields link_fields;
        link_fields.emplace_back("line_items[0][price]", price.json["id"].asString());
        link_fields.emplace_back("line_items[0][quantity]", std::to_string(quantity));
        link_fields.emplace_back("metadata[pond_order]", std::to_string(order));
        link_fields.emplace_back("metadata[pond]", pond_id);
        link_fields.emplace_back("after_completion[type]", "hosted_confirmation");
        link_fields.emplace_back("after_completion[hosted_confirmation][custom_message]",
                                 "Paid, thank you. You can close this page: " + pond_name + " sees the payment in a moment.");
        link_fields.emplace_back("restrictions[completed_sessions][limit]", "1");
        link = co_await stripe(api, key, drogon::Post, "/v1/payment_links", link_fields);
    }
    const bool made = link.status == 200 && link.json["id"].isString() && link.json["url"].isString();
    try {
        if (!made) {
            co_await db_->execSqlCoro("UPDATE shop_orders SET status = 'failed' WHERE id = ?", order);
            const std::string why = stripe_error(price.status == 200 ? link : price);
            LOG_WARN << "shop: order " << order << ": " << why;
            co_return Answer{502, "{\"error\":\"stripe_failed\",\"message\":" + jstr(why) + "}"};
        }
        const std::string url = link.json["url"].asString(), id = link.json["id"].asString();
        co_await db_->execSqlCoro("UPDATE shop_orders SET status = 'open', link_id = ?, link_url = ? WHERE id = ?", id, url, order);
        co_return Answer{200, "{\"order\":" + std::to_string(order) + ",\"url\":" + jstr(url) + "}"};
    } catch (const DrogonDbException &e) {
        co_return server_error(e);
    }
}

/* Orders waiting on Stripe: paid ones recorded (and their link closed), those open past a day let go. Run from the tick. */
Task<> Service::check_orders()
{
    try {
        const std::string key = co_await setting("stripe_key");
        if (key.empty())
            co_return;
        Result r = co_await db_->execSqlCoro("SELECT id, link_id, created_ms, item, quantity, member FROM shop_orders WHERE status = 'open' ORDER BY id LIMIT 50");
        const std::string api = shop_settings.stripe_api;
        for (const auto &row : r) {
            const int64_t id = num(row[0]), item = num(row[3]), quantity = num(row[4]);
            const std::string link = text(row[1]), buyer = text(row[5]);
            std::vector<std::pair<std::string, std::string>> q;
            q.emplace_back("payment_link", link);
            q.emplace_back("limit", "10");
            const StripeReply s = co_await stripe(api, key, drogon::Get, "/v1/checkout/sessions", q);
            if (s.status != 200)
                continue;
            bool paid = false;
            for (const auto &session : s.json["data"])
                if (session["payment_status"].asString() == "paid" && session["metadata"]["pond_order"].asString() == std::to_string(id))
                    paid = true;
            const bool lapsed = !paid && now_ms() - num(row[2]) > ORDER_KEEP_OPEN_MS;
            if (!paid && !lapsed)
                continue;
            TxPtr tx = co_await db_->newTransactionCoro();
            const std::string status = paid ? "paid" : "lapsed";
            const std::optional<int64_t> when = paid ? std::optional<int64_t>(now_ms()) : std::nullopt;
            Result u = co_await tx->execSqlCoro("UPDATE shop_orders SET status = ?, paid_ms = ? WHERE id = ? AND status = 'open'", status, when, id);
            int64_t ev = 0;
            if (u.affectedRows()) {
                std::string sold = paid ? "paid" : "lapsed";
                if (paid) {
                    co_await tx->execSqlCoro("UPDATE shop_items SET stock = GREATEST(stock - ?, 0) WHERE id = ? AND stock IS NOT NULL", quantity, item);
                    Result p = co_await tx->execSqlCoro("SELECT page FROM shop_items WHERE id = ?", item);
                    if (!p.empty() && num(p[0][0]) > 0)
                        co_await page_touched(tx, num(p[0][0]), "");
                }
                co_await add_event(tx, "order", "{\"id\":" + std::to_string(id) + ",\"status\":" + jstr(sold) + "}", buyer);
                ev = co_await add_event(tx, "orders", "{}"); /* for those who keep the shop */
            }
            if (!co_await Committed(std::move(tx)))
                continue;
            bump(ev);
            if (u.affectedRows())
                LOG_INFO << "shop: order " << id << (paid ? " paid" : " lapsed unpaid");
            /* the link takes no more payments */
            std::vector<std::pair<std::string, std::string>> off;
            off.emplace_back("active", "false");
            const std::string path = "/v1/payment_links/" + link;
            co_await stripe(api, key, drogon::Post, path, off);
        }
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "database: " << e.base().what();
    }
}

} // namespace pond
