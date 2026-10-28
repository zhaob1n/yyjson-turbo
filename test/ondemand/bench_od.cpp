// On Demand benchmark: yyjson_od vs simdjson::ondemand (+ yyjson DOM and
// simdjson DOM for reference). Same process, interleaved rounds.
// usage: bench_od DATA_DIR [rounds] [task-filter]
#include "simdjson.h"
extern "C" {
#include "yyjson.h"
#include "yyjson_ondemand.h"
}
#include <x86intrin.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
using namespace simdjson;
using sv = std::string_view;
typedef uint64_t u64;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct Input { std::string name; char *buf; size_t len; };
static Input load(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb"); CHECK(f);
  fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
  char *b = (char *)aligned_alloc(64, (n + 64 + 63) / 64 * 64 + 64);
  CHECK(fread(b, 1, n, f) == n); fclose(f);
  memset(b + n, 0, 128);
  Input in; in.name = path.substr(path.rfind('/') + 1); in.buf = b; in.len = n; return in;
}

struct Ctx {
  yyjson_od_parser *odp = yyjson_od_parser_new(NULL);
  ondemand::parser sjo;
  dom::parser sjd;
  yyjson_alc *alc = yyjson_alc_dyn_new();
  const Input *in = nullptr;
  yyjson_od_doc *od() { int c; yyjson_od_doc *d = yyjson_od_parse(odp, in->buf, in->len, &c); CHECK(d); return d; }
  ondemand::document sj() { return sjo.iterate(padded_string_view(in->buf, in->len, in->len + 64)); }
  yyjson_doc *yy() { yyjson_doc *d = yyjson_read_opts(in->buf, in->len, YYJSON_READ_STAGE1 | YYJSON_READ_NO_NUL_TERM, alc, NULL); CHECK(d); return d; }
  dom::element sd() { dom::element e; CHECK(!sjd.parse(in->buf, in->len, false).get(e)); return e; }
};

// ---- small helpers for yyjson_od ----
static inline yyjson_od_val odf(yyjson_od_iter &it, const char *k) {
  yyjson_od_val v; CHECK(yyjson_od_obj_find(&it, k, strlen(k), &v)); return v;
}
static inline sv ods(yyjson_od_val v) { yyjson_od_str s; CHECK(yyjson_od_get_str(&v, &s)); return sv(s.ptr, s.len); }
static inline u64 odu(yyjson_od_val v) { u64 x; CHECK(yyjson_od_get_uint(&v, &x)); return x; }
static inline int64_t odi(yyjson_od_val v) { int64_t x; CHECK(yyjson_od_get_sint(&v, &x)); return x; }
static inline double odd(yyjson_od_val v) { double x; CHECK(yyjson_od_get_real(&v, &x)); return x; }
static inline yyjson_od_iter odo(yyjson_od_val v) { yyjson_od_iter it; CHECK(yyjson_od_get_obj(&v, &it)); return it; }
static inline yyjson_od_iter oda(yyjson_od_val v) { yyjson_od_iter it; CHECK(yyjson_od_get_arr(&v, &it)); return it; }
static inline void odend(yyjson_od_doc *d) { CHECK(yyjson_od_doc_err(d) == 0); }
// ---- yyjson DOM helpers ----
static inline yyjson_val *yg(yyjson_val *o, const char *k) { yyjson_val *v = yyjson_obj_get(o, k); CHECK(v); return v; }
static inline sv ys(yyjson_val *v) { CHECK(yyjson_is_str(v)); return sv(yyjson_get_str(v), yyjson_get_len(v)); }
static inline u64 yu(yyjson_val *v) { CHECK(yyjson_is_uint(v)); return yyjson_get_uint(v); }
static inline double yd(yyjson_val *v) { CHECK(yyjson_is_num(v)); return yyjson_get_num(v); }

// =================== tasks ===================
// 1. twitter partial_tweets (simdjson benchmark/partial_tweets)
struct Tw { sv created_at; u64 id; sv text; u64 reply; u64 uid; sv sname; u64 rt, fav;
  bool operator==(const Tw &o) const { return created_at == o.created_at && id == o.id && text == o.text && reply == o.reply && uid == o.uid && sname == o.sname && rt == o.rt && fav == o.fav; } };
struct PartialTweets {
  typedef std::vector<Tw> R;
  static void od(Ctx &c, R &r) {
    yyjson_od_doc ld; CHECK(yyjson_od_iterate(c.odp, c.in->buf, c.in->len, &ld) == 0); yyjson_od_doc *d = &ld; yyjson_od_iter root = odo(yyjson_od_doc_root(d));
    yyjson_od_iter a = oda(odf(root, "statuses")); yyjson_od_val tv;
    while (yyjson_od_arr_next(&a, &tv)) {
      yyjson_od_iter t = odo(tv); Tw w;
      w.created_at = ods(odf(t, "created_at")); w.id = odu(odf(t, "id")); w.text = ods(odf(t, "text"));
      yyjson_od_val rp = odf(t, "in_reply_to_status_id"); w.reply = yyjson_od_is_null(&rp) ? 0 : odu(rp);
      yyjson_od_iter u = odo(odf(t, "user")); w.uid = odu(odf(u, "id")); w.sname = ods(odf(u, "screen_name"));
      w.rt = odu(odf(t, "retweet_count")); w.fav = odu(odf(t, "favorite_count"));
      r.push_back(w);
    }
    odend(d);
  }
  static void sj(Ctx &c, R &r) {
    auto doc = c.sj();
    for (ondemand::object t : doc.find_field("statuses")) {
      Tw w;
      w.created_at = t.find_field("created_at"); w.id = t.find_field("id"); w.text = t.find_field("text");
      ondemand::value rp = t.find_field("in_reply_to_status_id"); w.reply = rp.is_null() ? 0 : uint64_t(rp);
      ondemand::object u = t.find_field("user"); w.uid = u.find_field("id"); w.sname = u.find_field("screen_name");
      w.rt = t.find_field("retweet_count"); w.fav = t.find_field("favorite_count");
      r.push_back(w);
    }
  }
  static void yy(Ctx &c, R &r) {
    yyjson_doc *d = c.yy(); yyjson_val *st = yg(yyjson_doc_get_root(d), "statuses"), *t; size_t i, n;
    yyjson_arr_foreach(st, i, n, t) {
      Tw w; w.created_at = ys(yg(t, "created_at")); w.id = yu(yg(t, "id")); w.text = ys(yg(t, "text"));
      yyjson_val *rp = yg(t, "in_reply_to_status_id"); w.reply = yyjson_is_null(rp) ? 0 : yu(rp);
      yyjson_val *u = yg(t, "user"); w.uid = yu(yg(u, "id")); w.sname = ys(yg(u, "screen_name"));
      w.rt = yu(yg(t, "retweet_count")); w.fav = yu(yg(t, "favorite_count"));
      r.push_back(w);
    }
    yyjson_doc_free(d);
  }
  static void sd(Ctx &c, R &r) {
    dom::element doc = c.sd();
    for (dom::object t : doc["statuses"]) {
      Tw w; w.created_at = t["created_at"]; w.id = t["id"]; w.text = t["text"];
      dom::element rp = t["in_reply_to_status_id"]; w.reply = rp.is_null() ? 0 : uint64_t(rp);
      dom::object u = t["user"]; w.uid = u["id"]; w.sname = u["screen_name"];
      w.rt = t["retweet_count"]; w.fav = t["favorite_count"];
      r.push_back(w);
    }
  }
};

// 2. twitter top_tweet (forward-only variant, max_retweet_count = 60)
struct TopR { int64_t rt = -1; sv sname, text; bool operator==(const TopR &o) const { return rt == o.rt && sname == o.sname && text == o.text; } };
struct TopTweet {
  typedef TopR R;
  static void od(Ctx &c, R &r) {
    yyjson_od_doc ld; CHECK(yyjson_od_iterate(c.odp, c.in->buf, c.in->len, &ld) == 0); yyjson_od_doc *d = &ld; yyjson_od_iter root = odo(yyjson_od_doc_root(d));
    yyjson_od_iter a = oda(odf(root, "statuses")); yyjson_od_val tv;
    while (yyjson_od_arr_next(&a, &tv)) {
      yyjson_od_iter t = odo(tv);
      yyjson_od_val tx = odf(t, "text");
      yyjson_od_iter u = odo(odf(t, "user")); yyjson_od_val sn = odf(u, "screen_name");
      int64_t rt = odi(odf(t, "retweet_count"));
      if (rt <= 60 && rt >= r.rt) { r.rt = rt; r.text = ods(tx); r.sname = ods(sn); }
    }
    odend(d);
  }
  static void sj(Ctx &c, R &r) {
    auto doc = c.sj();
    for (auto t : doc["statuses"]) {
      auto tx = t["text"]; auto sn = t["user"]["screen_name"]; int64_t rt = t["retweet_count"];
      if (rt <= 60 && rt >= r.rt) { r.rt = rt; r.text = tx; r.sname = sn; }
    }
  }
  static void yy(Ctx &c, R &r) {
    yyjson_doc *d = c.yy(); yyjson_val *st = yg(yyjson_doc_get_root(d), "statuses"), *t; size_t i, n;
    yyjson_arr_foreach(st, i, n, t) {
      yyjson_val *tx = yg(t, "text"), *sn = yg(yg(t, "user"), "screen_name"); int64_t rt = (int64_t)yu(yg(t, "retweet_count"));
      if (rt <= 60 && rt >= r.rt) { r.rt = rt; r.text = ys(tx); r.sname = ys(sn); }
    }
    yyjson_doc_free(d);
  }
  static void sd(Ctx &c, R &r) {
    dom::element doc = c.sd();
    for (dom::object t : doc["statuses"]) {
      sv tx = t["text"], sn = t["user"]["screen_name"]; int64_t rt = t["retweet_count"];
      if (rt <= 60 && rt >= r.rt) { r.rt = rt; r.text = tx; r.sname = sn; }
    }
  }
};

// 3. twitter distinct_user_id (collect user ids incl. retweeted_status.user.id)
struct DistinctUserId {
  typedef std::vector<u64> R;
  static void od(Ctx &c, R &r) {
    yyjson_od_doc ld; CHECK(yyjson_od_iterate(c.odp, c.in->buf, c.in->len, &ld) == 0); yyjson_od_doc *d = &ld; yyjson_od_iter root = odo(yyjson_od_doc_root(d));
    yyjson_od_iter a = oda(odf(root, "statuses")); yyjson_od_val tv, rv;
    while (yyjson_od_arr_next(&a, &tv)) {
      yyjson_od_iter t = odo(tv);
      { yyjson_od_iter u = odo(odf(t, "user")); r.push_back(odu(odf(u, "id"))); }
      if (yyjson_od_obj_find(&t, "retweeted_status", 16, &rv)) {
        yyjson_od_iter rt = odo(rv); yyjson_od_iter u = odo(odf(rt, "user")); r.push_back(odu(odf(u, "id")));
      }
    }
    odend(d);
  }
  static void sj(Ctx &c, R &r) {
    auto doc = c.sj();
    for (ondemand::object t : doc.find_field("statuses")) {
      r.push_back(t.find_field("user").find_field("id"));
      auto rt = t.find_field("retweeted_status");
      if (!rt.error()) r.push_back(rt.find_field("user").find_field("id"));
    }
  }
  static void yy(Ctx &c, R &r) {
    yyjson_doc *d = c.yy(); yyjson_val *st = yg(yyjson_doc_get_root(d), "statuses"), *t; size_t i, n;
    yyjson_arr_foreach(st, i, n, t) {
      r.push_back(yu(yg(yg(t, "user"), "id")));
      yyjson_val *rt = yyjson_obj_get(t, "retweeted_status"); if (rt) r.push_back(yu(yg(yg(rt, "user"), "id")));
    }
    yyjson_doc_free(d);
  }
  static void sd(Ctx &c, R &r) {
    dom::element doc = c.sd();
    for (dom::object t : doc["statuses"]) {
      r.push_back(t["user"]["id"]);
      auto rt = t["retweeted_status"]; if (!rt.error()) r.push_back(rt["user"]["id"]);
    }
  }
};

// 4. citm_catalog: areaNames (key,value) + performances sums
struct CitmR { std::vector<std::pair<sv, sv>> names; u64 perf_id = 0, event_id = 0, amount = 0, area_id = 0, n_area = 0, start = 0;
  bool operator==(const CitmR &o) const { return names == o.names && perf_id == o.perf_id && event_id == o.event_id && amount == o.amount && area_id == o.area_id && n_area == o.n_area && start == o.start; } };
struct Citm {
  typedef CitmR R;
  static void od(Ctx &c, R &r) {
    yyjson_od_doc ld; CHECK(yyjson_od_iterate(c.odp, c.in->buf, c.in->len, &ld) == 0); yyjson_od_doc *d = &ld; yyjson_od_iter root = odo(yyjson_od_doc_root(d));
    yyjson_od_iter an = odo(odf(root, "areaNames")); yyjson_od_str k; yyjson_od_val v, pv, x, y;
    while (yyjson_od_obj_next(&an, &k, &v)) r.names.push_back({sv(k.ptr, k.len), ods(v)});
    yyjson_od_iter ps = oda(odf(root, "performances"));
    while (yyjson_od_arr_next(&ps, &pv)) {
      yyjson_od_iter p = odo(pv);
      r.event_id += odu(odf(p, "eventId")); r.perf_id += odu(odf(p, "id"));
      yyjson_od_iter pr = oda(odf(p, "prices"));
      while (yyjson_od_arr_next(&pr, &x)) { yyjson_od_iter o = odo(x); r.amount += odu(odf(o, "amount")); }
      yyjson_od_iter sc = oda(odf(p, "seatCategories"));
      while (yyjson_od_arr_next(&sc, &x)) {
        yyjson_od_iter o = odo(x); yyjson_od_iter ar = oda(odf(o, "areas"));
        while (yyjson_od_arr_next(&ar, &y)) { yyjson_od_iter a = odo(y); r.area_id += odu(odf(a, "areaId")); r.n_area++; }
      }
      r.start += odu(odf(p, "start"));
    }
    odend(d);
  }
  static void sj(Ctx &c, R &r) {
    auto doc = c.sj();
    for (auto f : doc["areaNames"].get_object()) { sv key = f.unescaped_key(); r.names.push_back({key, sv(f.value())}); }
    for (ondemand::object p : doc["performances"]) {
      r.event_id += uint64_t(p["eventId"]); r.perf_id += uint64_t(p["id"]);
      for (ondemand::object o : p["prices"]) r.amount += uint64_t(o["amount"]);
      for (ondemand::object o : p["seatCategories"])
        for (ondemand::object a : o["areas"]) { r.area_id += uint64_t(a["areaId"]); r.n_area++; }
      r.start += uint64_t(p["start"]);
    }
  }
  static void yy(Ctx &c, R &r) {
    yyjson_doc *d = c.yy(); yyjson_val *root = yyjson_doc_get_root(d), *k, *v, *p, *o, *a; size_t i, n, i2, n2, i3, n3;
    yyjson_obj_foreach(yg(root, "areaNames"), i, n, k, v) r.names.push_back({ys(k), ys(v)});
    yyjson_arr_foreach(yg(root, "performances"), i, n, p) {
      r.event_id += yu(yg(p, "eventId")); r.perf_id += yu(yg(p, "id"));
      yyjson_arr_foreach(yg(p, "prices"), i2, n2, o) r.amount += yu(yg(o, "amount"));
      yyjson_arr_foreach(yg(p, "seatCategories"), i2, n2, o)
        yyjson_arr_foreach(yg(o, "areas"), i3, n3, a) { r.area_id += yu(yg(a, "areaId")); r.n_area++; }
      r.start += yu(yg(p, "start"));
    }
    yyjson_doc_free(d);
  }
  static void sd(Ctx &c, R &r) {
    dom::element doc = c.sd();
    for (auto f : dom::object(doc["areaNames"])) r.names.push_back({f.key, sv(f.value)});
    for (dom::object p : doc["performances"]) {
      r.event_id += uint64_t(p["eventId"]); r.perf_id += uint64_t(p["id"]);
      for (dom::object o : p["prices"]) r.amount += uint64_t(o["amount"]);
      for (dom::object o : p["seatCategories"])
        for (dom::object a : o["areas"]) { r.area_id += uint64_t(a["areaId"]); r.n_area++; }
      r.start += uint64_t(p["start"]);
    }
  }
};

// 5. canada: sum all coordinates
struct CanR { double x = 0, y = 0; u64 n = 0; bool operator==(const CanR &o) const { return memcmp(&x, &o.x, 8) == 0 && memcmp(&y, &o.y, 8) == 0 && n == o.n; } };
struct Canada {
  typedef CanR R;
  static void od(Ctx &c, R &r) {
    yyjson_od_doc ld; CHECK(yyjson_od_iterate(c.odp, c.in->buf, c.in->len, &ld) == 0); yyjson_od_doc *d = &ld; yyjson_od_iter root = odo(yyjson_od_doc_root(d));
    yyjson_od_iter fs = oda(odf(root, "features")); yyjson_od_val f, ring, pt, x;
    while (yyjson_od_arr_next(&fs, &f)) {
      yyjson_od_iter fo = odo(f); yyjson_od_iter g = odo(odf(fo, "geometry"));
      yyjson_od_iter rings = oda(odf(g, "coordinates"));
      while (yyjson_od_arr_next(&rings, &ring)) {
        yyjson_od_iter pts = oda(ring);
        while (yyjson_od_arr_next(&pts, &pt)) {
          yyjson_od_iter xy = oda(pt);
          CHECK(yyjson_od_arr_next(&xy, &x)); r.x += odd(x);
          CHECK(yyjson_od_arr_next(&xy, &x)); r.y += odd(x);
          CHECK(!yyjson_od_arr_next(&xy, &x)); r.n++;
        }
      }
    }
    odend(d);
  }
  static void sj(Ctx &c, R &r) {
    auto doc = c.sj();
    for (ondemand::object f : doc["features"]) {
      for (ondemand::array ring : f["geometry"]["coordinates"])
        for (ondemand::array pt : ring) {
          auto it = pt.begin(); r.x += double(*it); ++it; r.y += double(*it); ++it; CHECK(it == pt.end()); r.n++;
        }
    }
  }
  static void yy(Ctx &c, R &r) {
    yyjson_doc *d = c.yy(); yyjson_val *f, *ring, *pt; size_t i, n, i2, n2, i3, n3;
    yyjson_arr_foreach(yg(yyjson_doc_get_root(d), "features"), i, n, f)
      yyjson_arr_foreach(yg(yg(f, "geometry"), "coordinates"), i2, n2, ring)
        yyjson_arr_foreach(ring, i3, n3, pt) { CHECK(yyjson_arr_size(pt) == 2); r.x += yd(yyjson_arr_get_first(pt)); r.y += yd(yyjson_arr_get_last(pt)); r.n++; }
    yyjson_doc_free(d);
  }
  static void sd(Ctx &c, R &r) {
    dom::element doc = c.sd();
    for (dom::object f : doc["features"])
      for (dom::array ring : f["geometry"]["coordinates"])
        for (dom::array pt : ring) { CHECK(pt.size() == 2); r.x += double(pt.at(0)); r.y += double(pt.at(1)); r.n++; }
  }
};

// 6. gsoc-2018: per project name, sponsor.name, author.name
struct GsR { sv a, b, c; bool operator==(const GsR &o) const { return a == o.a && b == o.b && c == o.c; } };
struct Gsoc {
  typedef std::vector<GsR> R;
  static void od(Ctx &c, R &r) {
    yyjson_od_doc ld; CHECK(yyjson_od_iterate(c.odp, c.in->buf, c.in->len, &ld) == 0); yyjson_od_doc *d = &ld; yyjson_od_iter root = odo(yyjson_od_doc_root(d)); yyjson_od_str k; yyjson_od_val v;
    while (yyjson_od_obj_next(&root, &k, &v)) {
      yyjson_od_iter p = odo(v); GsR g; g.a = ods(odf(p, "name"));
      { yyjson_od_iter s = odo(odf(p, "sponsor")); g.b = ods(odf(s, "name")); }
      { yyjson_od_iter s = odo(odf(p, "author")); g.c = ods(odf(s, "name")); }
      r.push_back(g);
    }
    odend(d);
  }
  static void sj(Ctx &c, R &r) {
    auto doc = c.sj();
    for (auto f : doc.get_object()) {
      ondemand::object p = f.value(); GsR g; g.a = p["name"]; g.b = p["sponsor"]["name"]; g.c = p["author"]["name"]; r.push_back(g);
    }
  }
  static void yy(Ctx &c, R &r) {
    yyjson_doc *d = c.yy(); yyjson_val *k, *p; size_t i, n;
    yyjson_obj_foreach(yyjson_doc_get_root(d), i, n, k, p) { GsR g; g.a = ys(yg(p, "name")); g.b = ys(yg(yg(p, "sponsor"), "name")); g.c = ys(yg(yg(p, "author"), "name")); r.push_back(g); }
    yyjson_doc_free(d);
  }
  static void sd(Ctx &c, R &r) {
    dom::element doc = c.sd();
    for (auto f : dom::object(doc)) { dom::object p = f.value; GsR g; g.a = p["name"]; g.b = p["sponsor"]["name"]; g.c = p["author"]["name"]; r.push_back(g); }
  }
};

// 7. numbers: sum array of doubles
struct NumR { double s = 0; u64 n = 0; bool operator==(const NumR &o) const { return memcmp(&s, &o.s, 8) == 0 && n == o.n; } };
struct Numbers {
  typedef NumR R;
  static void od(Ctx &c, R &r) {
    yyjson_od_doc ld; CHECK(yyjson_od_iterate(c.odp, c.in->buf, c.in->len, &ld) == 0); yyjson_od_doc *d = &ld; yyjson_od_iter a = oda(yyjson_od_doc_root(d)); yyjson_od_val v;
    while (yyjson_od_arr_next(&a, &v)) { r.s += odd(v); r.n++; }
    odend(d);
  }
  static void sj(Ctx &c, R &r) { auto doc = c.sj(); for (double x : doc.get_array()) { r.s += x; r.n++; } }
  static void yy(Ctx &c, R &r) { yyjson_doc *d = c.yy(); yyjson_val *v; size_t i, n; yyjson_arr_foreach(yyjson_doc_get_root(d), i, n, v) { r.s += yd(v); r.n++; } yyjson_doc_free(d); }
  static void sd(Ctx &c, R &r) { dom::element doc = c.sd(); for (double x : dom::array(doc)) { r.s += x; r.n++; } }
};

// 8. update-center: plugins -> name, version, #dependencies
struct UcR { sv name, ver; u64 ndep; bool operator==(const UcR &o) const { return name == o.name && ver == o.ver && ndep == o.ndep; } };
struct Update {
  typedef std::vector<UcR> R;
  static void od(Ctx &c, R &r) {
    yyjson_od_doc ld; CHECK(yyjson_od_iterate(c.odp, c.in->buf, c.in->len, &ld) == 0); yyjson_od_doc *d = &ld; yyjson_od_iter root = odo(yyjson_od_doc_root(d));
    yyjson_od_iter pl = odo(odf(root, "plugins")); yyjson_od_str k; yyjson_od_val v, x;
    while (yyjson_od_obj_next(&pl, &k, &v)) {
      yyjson_od_iter p = odo(v); UcR u; u.ndep = 0;
      yyjson_od_iter dp = oda(odf(p, "dependencies")); while (yyjson_od_arr_next(&dp, &x)) u.ndep++;
      u.name = ods(odf(p, "name")); u.ver = ods(odf(p, "version")); r.push_back(u);
    }
    odend(d);
  }
  static void sj(Ctx &c, R &r) {
    auto doc = c.sj();
    for (auto f : doc["plugins"].get_object()) {
      ondemand::object p = f.value(); UcR u; u.ndep = 0;
      for (auto x : p["dependencies"]) { (void)x; u.ndep++; }
      u.name = p["name"]; u.ver = p["version"]; r.push_back(u);
    }
  }
  static void yy(Ctx &c, R &r) {
    yyjson_doc *d = c.yy(); yyjson_val *k, *p; size_t i, n;
    yyjson_obj_foreach(yg(yyjson_doc_get_root(d), "plugins"), i, n, k, p) { UcR u; u.ndep = yyjson_arr_size(yg(p, "dependencies")); u.name = ys(yg(p, "name")); u.ver = ys(yg(p, "version")); r.push_back(u); }
    yyjson_doc_free(d);
  }
  static void sd(Ctx &c, R &r) {
    dom::element doc = c.sd();
    for (auto f : dom::object(doc["plugins"])) { dom::object p = f.value; UcR u; u.ndep = dom::array(p["dependencies"]).size(); u.name = p["name"]; u.ver = p["version"]; r.push_back(u); }
  }
};

// 9. full traversal: every value visited and decoded
struct WalkR { u64 obj = 0, arr = 0, str = 0, sbytes = 0, num = 0, t = 0, f = 0, nul = 0, keys = 0; double sum = 0;
  bool operator==(const WalkR &o) const { return obj == o.obj && arr == o.arr && str == o.str && sbytes == o.sbytes && num == o.num && t == o.t && f == o.f && nul == o.nul && keys == o.keys && memcmp(&sum, &o.sum, 8) == 0; } };
static void odwalk(yyjson_od_val v, WalkR &r) {
  yyjson_od_iter it; yyjson_od_val c; yyjson_od_str k; bool b;
  switch (yyjson_od_get_type(&v)) {
    case YYJSON_OD_TYPE_OBJ: r.obj++; CHECK(yyjson_od_get_obj(&v, &it)); while (yyjson_od_obj_next(&it, &k, &c)) { r.keys++; odwalk(c, r); } break;
    case YYJSON_OD_TYPE_ARR: r.arr++; CHECK(yyjson_od_get_arr(&v, &it)); while (yyjson_od_arr_next(&it, &c)) odwalk(c, r); break;
    case YYJSON_OD_TYPE_STR: r.str++; r.sbytes += ods(v).size(); break;
    case YYJSON_OD_TYPE_NUM: r.num++; r.sum += odd(v); break;
    case YYJSON_OD_TYPE_BOOL: CHECK(yyjson_od_get_bool(&v, &b)); (b ? r.t : r.f)++; break;
    case YYJSON_OD_TYPE_NULL: CHECK(yyjson_od_is_null(&v)); r.nul++; break;
    default: CHECK(0);
  }
}
// idiomatic simdjson On Demand traversal (cf. simdjson's recursive_print_json)
static void sjwalk(ondemand::value v, WalkR &r) {
  switch (v.type()) {
    case ondemand::json_type::object: r.obj++; for (ondemand::field f : v.get_object()) { r.keys++; sjwalk(f.value(), r); } break;
    case ondemand::json_type::array: r.arr++; for (ondemand::value x : v.get_array()) sjwalk(x, r); break;
    case ondemand::json_type::string: r.str++; r.sbytes += std::string_view(v.get_string()).size(); break;
    case ondemand::json_type::number: r.num++; r.sum += double(v.get_double()); break;
    case ondemand::json_type::boolean: (bool(v.get_bool()) ? r.t : r.f)++; break;
    case ondemand::json_type::null: CHECK(v.is_null()); r.nul++; break;
    default: CHECK(0);
  }
}
static void yywalk(yyjson_val *v, WalkR &r) {
  yyjson_val *k, *c; size_t i, n;
  switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_OBJ: r.obj++; yyjson_obj_foreach(v, i, n, k, c) { r.keys++; yywalk(c, r); } break;
    case YYJSON_TYPE_ARR: r.arr++; yyjson_arr_foreach(v, i, n, c) yywalk(c, r); break;
    case YYJSON_TYPE_STR: r.str++; r.sbytes += yyjson_get_len(v); break;
    case YYJSON_TYPE_NUM: r.num++; r.sum += yyjson_get_num(v); break;
    case YYJSON_TYPE_BOOL: (yyjson_get_bool(v) ? r.t : r.f)++; break;
    case YYJSON_TYPE_NULL: r.nul++; break;
    default: CHECK(0);
  }
}
static void sdwalk(dom::element v, WalkR &r) {
  switch (v.type()) {
    case dom::element_type::OBJECT: r.obj++; for (auto f : dom::object(v)) { r.keys++; sdwalk(f.value, r); } break;
    case dom::element_type::ARRAY: r.arr++; for (auto x : dom::array(v)) sdwalk(x, r); break;
    case dom::element_type::STRING: r.str++; r.sbytes += sv(v).size(); break;
    case dom::element_type::INT64: case dom::element_type::UINT64: case dom::element_type::DOUBLE: case dom::element_type::BIGINT: r.num++; r.sum += double(v); break;
    case dom::element_type::BOOL: (bool(v) ? r.t : r.f)++; break;
    case dom::element_type::NULL_VALUE: r.nul++; break;
  }
}
struct Walk {
  typedef WalkR R;
  static void od(Ctx &c, R &r) { yyjson_od_doc ld; CHECK(yyjson_od_iterate(c.odp, c.in->buf, c.in->len, &ld) == 0); yyjson_od_doc *d = &ld; odwalk(yyjson_od_doc_root(d), r); CHECK(yyjson_od_doc_finish(d)); }
  static void sj(Ctx &c, R &r) { auto doc = c.sj(); ondemand::value root = doc.get_value(); sjwalk(root, r); CHECK(doc.at_end()); }
  static void yy(Ctx &c, R &r) { yyjson_doc *d = c.yy(); yywalk(yyjson_doc_get_root(d), r); yyjson_doc_free(d); }
  static void sd(Ctx &c, R &r) { sdwalk(c.sd(), r); }
};

// =================== sampling profiler (OD_PROF=impl index, OD_PROF_IT) ===================
static unsigned long g_ips[4000000]; static volatile int g_ni;
static void prof_h(int, siginfo_t *, void *c) { ucontext_t *u = (ucontext_t *)c; if (g_ni < 4000000) g_ips[g_ni++] = u->uc_mcontext.gregs[REG_RIP]; }
template <class T> static void prof(Ctx &c, int m, int it) {
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = prof_h; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, 0);
  struct itimerval t = {{0, 100}, {0, 100}}; setitimer(ITIMER_PROF, &t, 0);
  for (int k = 0; k < it; k++) { typename T::R x; switch (m) { case 0: T::od(c, x); break; case 1: T::sj(c, x); break; case 2: T::yy(c, x); break; default: T::sd(c, x); } }
  t.it_value.tv_usec = 0; t.it_interval.tv_usec = 0; setitimer(ITIMER_PROF, &t, 0);
  FILE *o = fopen("ips.txt", "w"); for (int k = 0; k < g_ni; k++) fprintf(o, "%lx\n", g_ips[k]); fclose(o); fprintf(stderr, "%d samples\n", g_ni);
  exit(0);
}

// =================== driver ===================
static const char *IMPL[4] = {"yyjson_od", "simdjson_od", "yyjson_dom", "simdjson_dom"};
template <class T> static void run(Ctx &c, const char *task, const Input &in, int rounds, FILE *raw) {
  c.in = &in;
  if (getenv("OD_ICNT")) { int m = atoi(getenv("OD_ICNT")); typename T::R x0, x1;
    switch (m) { case 0: T::od(c, x0); break; default: T::sj(c, x0); }
    __asm__ __volatile__("int3");
    switch (m) { case 0: T::od(c, x1); break; default: T::sj(c, x1); }
    __asm__ __volatile__("int3"); exit(0); }
  if (getenv("OD_PROF")) prof<T>(c, atoi(getenv("OD_PROF")), atoi(getenv("OD_PROF_IT") ? getenv("OD_PROF_IT") : "2000"));
  typename T::R res[4];
  T::od(c, res[0]); T::sj(c, res[1]); T::yy(c, res[2]); T::sd(c, res[3]);
  bool same = res[0] == res[1] && res[0] == res[2] && res[0] == res[3];
  if (!same) { fprintf(stderr, "RESULT MISMATCH task %s file %s: od==sj %d od==yy %d od==sd %d\n", task, in.name.c_str(), res[0] == res[1], res[0] == res[2], res[0] == res[3]); exit(2); }
  int it = (int)std::max<size_t>(1, (size_t)200000000 / in.len / 4); // ~50 MB per sample
  std::vector<double> t[4];
  for (int r = 0; r < rounds; r++)
    for (int m = 0; m < 4; m++) {
      typename T::R x;
      auto t0 = std::chrono::steady_clock::now();
      for (int k = 0; k < it; k++) {
        x = typename T::R();
        switch (m) { case 0: T::od(c, x); break; case 1: T::sj(c, x); break; case 2: T::yy(c, x); break; default: T::sd(c, x); }
      }
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / it;
      t[m].push_back(s);
      if (raw) fprintf(raw, "%s\t%s\t%s\t%d\t%.9f\n", task, in.name.c_str(), IMPL[m], r, s);
    }
  double best[4], med[4];
  for (int m = 0; m < 4; m++) { std::sort(t[m].begin(), t[m].end()); best[m] = t[m][0]; med[m] = t[m][t[m].size() / 2]; }
  auto g = [&](double s) { return in.len / s / 1e9; };
  printf("| %-16s | %-18s | %6.2f | %6.2f | **%5.3f** | %6.2f | %6.2f | %5.3f | %5.3f | %5.3f |\n", task, in.name.c_str(),
         g(best[0]), g(best[1]), best[1] / best[0], g(best[2]), g(best[3]), best[2] / best[0], best[3] / best[0], med[1] / med[0]);
  fflush(stdout);
}

int main(int argc, char **argv) {
  CHECK(argc >= 2);
  std::string dir = argv[1]; int rounds = argc > 2 ? atoi(argv[2]) : 15; const char *flt = argc > 3 ? argv[3] : "";
  FILE *raw = fopen("bench_od_raw.tsv", "w");
  Ctx c;
  Input tw = load(dir + "/twitter.json"), ci = load(dir + "/citm_catalog.json"), ca = load(dir + "/canada.json"),
        gs = load(dir + "/gsoc-2018.json"), nu = load(dir + "/numbers.json"), uc = load(dir + "/update-center.json");
  printf("| task | file | yyjson_od GB/s | simdjson_od GB/s | od ratio (yy/sj) | yyjson_dom GB/s | simdjson_dom GB/s | yy_dom/yy_od | sj_dom/yy_od | od ratio (median) |\n");
  printf("|---|---|---|---|---|---|---|---|---|---|\n");
#define RUN(T, nm, in) if (strstr((std::string(nm) + ":" + (in).name).c_str(), flt)) run<T>(c, nm, in, rounds, raw)
  RUN(PartialTweets, "partial_tweets", tw);
  RUN(TopTweet, "top_tweet", tw);
  RUN(DistinctUserId, "distinct_user_id", tw);
  RUN(Citm, "citm_sums", ci);
  RUN(Canada, "canada_coords", ca);
  RUN(Gsoc, "gsoc_fields", gs);
  RUN(Numbers, "numbers_sum", nu);
  RUN(Update, "update_center", uc);
  RUN(Walk, "walk", tw); RUN(Walk, "walk", ci); RUN(Walk, "walk", ca); RUN(Walk, "walk", gs); RUN(Walk, "walk", uc); RUN(Walk, "walk", nu);
  fclose(raw);
  return 0;
}
