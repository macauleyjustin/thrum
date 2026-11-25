#import <Foundation/Foundation.h>
#include "thrum_rt.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* method;
    const char* url;
    const char* body;
    size_t body_len;
    TMap* headers;
} TNetReq;

static const char* need_str_arg(TRuntime* rt, TVal v, const char* what, int line, int col) {
    if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_STR) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s needs text, got %s", what, thrum_tname(v));
        thrum_error(rt, line, col, msg, NULL);
    }
    return ((TStr*)v.as.obj)->data;
}

static TVal make_err_resp(TRuntime* rt, const char* url, const char* msg) {
    T_FRAME(rt);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* m = (TMap*)mp.as.obj;
    thrum_map_set_cs(rt, m, "ok", thrum_bool(false));
    thrum_map_set_cs(rt, m, "status", thrum_num_dimless(0.0));
    thrum_map_set_cs(rt, m, "url", thrum_cstr(rt, url ? url : ""));
    thrum_map_set_cs(rt, m, "content_type", thrum_cstr(rt, ""));
    T_VAR(rt, hm, thrum_map(rt));
    thrum_map_set_cs(rt, m, "headers", hm);
    thrum_map_set_cs(rt, m, "body", thrum_cstr(rt, ""));
    thrum_map_set_cs(rt, m, "error", thrum_cstr(rt, msg ? msg : "request failed"));
    T_END_FRAME(rt);
    return mp;
}

static TVal prod_net_req(TRuntime* rt, void* ctx) {
    TNetReq* r = (TNetReq*)ctx;
    if (!r->url || (strncmp(r->url, "http://", 7) != 0 && strncmp(r->url, "https://", 8) != 0)) {
        return make_err_resp(rt, r->url, "only http:// and https:// URLs are supported");
    }

    __block NSInteger status_code = 0;
    __block NSString* final_url = nil;
    __block NSString* content_type = nil;
    __block NSString* body_str = nil;
    __block NSString* err_str = nil;
    __block NSMutableDictionary* resp_headers = nil;

    @autoreleasepool {
        NSString* url_ns = [NSString stringWithUTF8String:r->url];
        NSURL* nsurl = url_ns ? [NSURL URLWithString:url_ns] : nil;
        if (!nsurl) {
            return make_err_resp(rt, r->url, "invalid URL");
        }
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsurl
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:12.0];
        [req setHTTPMethod:[NSString stringWithUTF8String:r->method ? r->method : "GET"]];
        [req setValue:@"Mozilla/5.0 (Macintosh; Apple Silicon Mac OS X) ThrumBrowser/0.2"
            forHTTPHeaderField:@"User-Agent"];
        [req setValue:@"text/html,application/xhtml+xml,text/plain,application/json;q=0.9,*/*;q=0.8"
            forHTTPHeaderField:@"Accept"];

        if (r->headers) {
            for (size_t i = 0; i < r->headers->order_len; i++) {
                TVal kv = r->headers->order_keys[i];
                TVal vv;
                if (thrum_map_get(r->headers, kv, &vv) &&
                    kv.kind == TV_OBJ && kv.as.obj->kind == OBJ_STR &&
                    vv.kind == TV_OBJ && vv.as.obj->kind == OBJ_STR) {
                    NSString* hk = [NSString stringWithUTF8String:((TStr*)kv.as.obj)->data];
                    NSString* hv = [NSString stringWithUTF8String:((TStr*)vv.as.obj)->data];
                    if (hk && hv) [req setValue:hv forHTTPHeaderField:hk];
                }
            }
        }
        if (r->body && r->body_len > 0) {
            [req setHTTPBody:[NSData dataWithBytes:r->body length:r->body_len]];
        }

        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = 12.0;
        cfg.timeoutIntervalForResource = 20.0;
        NSURLSession* sess = [NSURLSession sessionWithConfiguration:cfg];
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        NSURLSessionDataTask* task = [sess dataTaskWithRequest:req
            completionHandler:^(NSData* data, NSURLResponse* resp, NSError* error) {
                if (error) {
                    err_str = [[error localizedDescription] copy];
                }
                if (resp) {
                    NSURL* ru = [resp URL];
                    if (ru) final_url = [[ru absoluteString] copy];
                    if ([resp MIMEType]) content_type = [[resp MIMEType] copy];
                    if ([resp isKindOfClass:[NSHTTPURLResponse class]]) {
                        NSHTTPURLResponse* hr = (NSHTTPURLResponse*)resp;
                        status_code = [hr statusCode];
                        NSDictionary* hdrs = [hr allHeaderFields];
                        resp_headers = [[NSMutableDictionary alloc] initWithCapacity:[hdrs count]];
                        for (id k in hdrs) {
                            NSString* ks = [[NSString stringWithFormat:@"%@", k] lowercaseString];
                            NSString* vs = [NSString stringWithFormat:@"%@", [hdrs objectForKey:k]];
                            [resp_headers setObject:vs forKey:ks];
                        }
                    }
                }
                if (data && [data length] > 0) {
                    NSString* s = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
                    if (!s) s = [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
                    if (!s) s = [[NSString alloc] initWithData:data encoding:NSASCIIStringEncoding];
                    body_str = s;
                }
                dispatch_semaphore_signal(sem);
            }];
        [task resume];
        long wait_res = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)22 * NSEC_PER_SEC));
        if (wait_res != 0) {
            [task cancel];
            err_str = [@"request timed out" copy];
        }
        [sess invalidateAndCancel];
    }

    T_FRAME(rt);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* m = (TMap*)mp.as.obj;
    bool ok = (err_str == nil && status_code >= 200 && status_code < 400);
    thrum_map_set_cs(rt, m, "ok", thrum_bool(ok));
    thrum_map_set_cs(rt, m, "status", thrum_num_dimless((double)status_code));
    thrum_map_set_cs(rt, m, "url", thrum_cstr(rt, final_url ? [final_url UTF8String] : r->url));
    thrum_map_set_cs(rt, m, "content_type", thrum_cstr(rt, content_type ? [content_type UTF8String] : ""));

    T_VAR(rt, hm, thrum_map(rt));
    TMap* hmap = (TMap*)hm.as.obj;
    if (resp_headers) {
        NSArray* ks = [[resp_headers allKeys] sortedArrayUsingSelector:@selector(compare:)];
        for (NSString* k in ks) {
            NSString* v = [resp_headers objectForKey:k];
            T_VAR(rt, kv, thrum_cstr(rt, [k UTF8String]));
            T_VAR(rt, vv, thrum_cstr(rt, [v UTF8String]));
            thrum_map_set(rt, hmap, kv, vv);
        }
    }
    thrum_map_set_cs(rt, m, "headers", hm);
    thrum_map_set_cs(rt, m, "body", thrum_cstr(rt, body_str ? [body_str UTF8String] : ""));
    thrum_map_set_cs(rt, m, "error", thrum_cstr(rt, err_str ? [err_str UTF8String] : ""));

    @autoreleasepool {
        [final_url release];
        [content_type release];
        [body_str release];
        [err_str release];
        [resp_headers release];
    }
    T_END_FRAME(rt);
    return mp;
}

static TVal net_get(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx;
    const char* url = need_str_arg(rt, argv[0], "net.get", line, col);
    TMap* hdrs = NULL;
    if (argc > 1 && argv[1].kind != TV_NIL) {
        if (argv[1].kind != TV_OBJ || argv[1].as.obj->kind != OBJ_MAP) {
            thrum_error(rt, line, col, "net.get headers must be a map", NULL);
        }
        hdrs = (TMap*)argv[1].as.obj;
    }
    TNetReq req = {"GET", url, NULL, 0, hdrs};
    return thrum_journal_get(rt, "net.get", url, prod_net_req, &req);
}

static TVal net_post(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx;
    const char* url = need_str_arg(rt, argv[0], "net.post", line, col);
    const char* body = need_str_arg(rt, argv[1], "net.post body", line, col);
    TMap* hdrs = NULL;
    if (argc > 2 && argv[2].kind != TV_NIL) {
        if (argv[2].kind != TV_OBJ || argv[2].as.obj->kind != OBJ_MAP) {
            thrum_error(rt, line, col, "net.post headers must be a map", NULL);
        }
        hdrs = (TMap*)argv[2].as.obj;
    }
    TNetReq req = {"POST", url, body, strlen(body), hdrs};
    return thrum_journal_get(rt, "net.post", url, prod_net_req, &req);
}

TVal thrum_cap_net_create(TRuntime* rt, int line, int col) {
    (void)line; (void)col;
    TCap* c = thrum_alloc_cap(rt, "net", NULL, NULL, NULL);
    thrum_cap_add_method(rt, c, "get", net_get, 1, 2);
    thrum_cap_add_method(rt, c, "post", net_post, 2, 3);
    return thrum_obj((TObj*)c);
}
