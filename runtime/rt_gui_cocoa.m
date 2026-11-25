#import <Cocoa/Cocoa.h>
#include "thrum_rt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    GCMD_CLEAR,
    GCMD_RECT,
    GCMD_ROUND_RECT,
    GCMD_STROKE_RECT,
    GCMD_CIRCLE,
    GCMD_LINE,
    GCMD_TEXT,
    GCMD_CLIP,
    GCMD_UNCLIP,
} TGuiCmdKind;

typedef struct {
    TGuiCmdKind kind;
    double x, y, w, h;
    double rad, lw;
    double r, g, b, a;
    int align; /* 0:left 1:center 2:right 3:mid 4:lmid 5:rmid */
    bool bold;
    bool mono;
    char* text;
} TGuiCmd;

typedef struct {
    TGuiCmd* items;
    size_t len;
    size_t cap;
} TGuiCmdBuf;

typedef struct {
    int kind; /* 0:quit 1:down 2:up 3:move 4:drag 5:key 6:resize 7:wheel */
    double x, y;
    double dx, dy;
    int button;
    int clicks;
    bool cmd;
    bool shift;
    char* key;
    char* typed;
} TGuiEv;

typedef struct TGuiState TGuiState;

@interface ThrumGuiView : NSView {
@public
    TGuiState* _st;
}
@end

@interface ThrumWinDelegate : NSObject <NSWindowDelegate> {
@public
    TGuiState* _st;
}
- (void)requestQuit:(id)sender;
@end

struct TGuiState {
    double w, h;
    double bg_r, bg_g, bg_b;
    bool open;
    bool headless;
    TGuiCmdBuf staging;
    TGuiCmdBuf disp;
    TGuiEv* events;
    size_t ev_len;
    size_t ev_cap;
    NSWindow* win;
    ThrumGuiView* view;
    ThrumWinDelegate* del;
};

static void cmd_buf_clear(TGuiCmdBuf* b) {
    for (size_t i = 0; i < b->len; i++) {
        free(b->items[i].text);
    }
    b->len = 0;
}

static void cmd_buf_free(TGuiCmdBuf* b) {
    cmd_buf_clear(b);
    free(b->items);
    b->items = NULL;
    b->cap = 0;
}

static void cmd_buf_push(TGuiCmdBuf* b, TGuiCmd cmd) {
    if (b->len >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->items = (TGuiCmd*)realloc(b->items, b->cap * sizeof(TGuiCmd));
    }
    b->items[b->len++] = cmd;
}

static void push_ev(TGuiState* st, TGuiEv ev) {
    if (!st) return;
    if (st->ev_len >= st->ev_cap) {
        st->ev_cap = st->ev_cap ? st->ev_cap * 2 : 64;
        st->events = (TGuiEv*)realloc(st->events, st->ev_cap * sizeof(TGuiEv));
    }
    st->events[st->ev_len++] = ev;
}

static void events_clear(TGuiState* st) {
    for (size_t i = 0; i < st->ev_len; i++) {
        free(st->events[i].key);
        free(st->events[i].typed);
    }
    st->ev_len = 0;
}

static NSFont* make_gui_font(double size, bool bold, bool mono) {
    NSFontWeight w = bold ? NSFontWeightBold : NSFontWeightMedium;
    if (mono) {
        return [NSFont monospacedSystemFontOfSize:(CGFloat)size weight:(bold ? NSFontWeightBold : NSFontWeightRegular)];
    }
    return [NSFont systemFontOfSize:(CGFloat)size weight:w];
}

static double clamp255(double v) {
    if (v < 0.0) return 0.0;
    if (v > 255.0) return 255.0;
    return v / 255.0;
}

static NSColor* mk_color(double r, double g, double b, double a) {
    return [NSColor colorWithSRGBRed:clamp255(r) green:clamp255(g) blue:clamp255(b) alpha:clamp255(a)];
}

static void draw_disp_cmds(TGuiState* st, NSRect bounds) {
    if (!st) return;
    [mk_color(st->bg_r, st->bg_g, st->bg_b, 255.0) setFill];
    NSRectFill(bounds);
    int clip_depth = 0;

    for (size_t i = 0; i < st->disp.len; i++) {
        TGuiCmd* c = &st->disp.items[i];
        switch (c->kind) {
            case GCMD_CLEAR:
                [mk_color(c->r, c->g, c->b, 255.0) setFill];
                NSRectFill(bounds);
                break;
            case GCMD_CLIP:
                [NSGraphicsContext saveGraphicsState];
                NSRectClip(NSMakeRect(c->x, c->y, c->w, c->h));
                clip_depth++;
                break;
            case GCMD_UNCLIP:
                if (clip_depth > 0) {
                    [NSGraphicsContext restoreGraphicsState];
                    clip_depth--;
                }
                break;
            case GCMD_RECT:
                [mk_color(c->r, c->g, c->b, c->a) setFill];
                NSRectFillUsingOperation(NSMakeRect(c->x, c->y, c->w, c->h), NSCompositingOperationSourceOver);
                break;
            case GCMD_ROUND_RECT: {
                [mk_color(c->r, c->g, c->b, c->a) setFill];
                NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c->x, c->y, c->w, c->h)
                                                                  xRadius:c->rad yRadius:c->rad];
                [p fill];
                break;
            }
            case GCMD_STROKE_RECT: {
                [mk_color(c->r, c->g, c->b, c->a) setStroke];
                NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(c->x, c->y, c->w, c->h)
                                                                  xRadius:c->rad yRadius:c->rad];
                [p setLineWidth:c->lw];
                [p stroke];
                break;
            }
            case GCMD_CIRCLE: {
                [mk_color(c->r, c->g, c->b, c->a) setFill];
                NSBezierPath* p = [NSBezierPath bezierPathWithOvalInRect:
                    NSMakeRect(c->x - c->rad, c->y - c->rad, c->rad * 2.0, c->rad * 2.0)];
                [p fill];
                break;
            }
            case GCMD_LINE: {
                [mk_color(c->r, c->g, c->b, c->a) setStroke];
                NSBezierPath* p = [NSBezierPath bezierPath];
                [p moveToPoint:NSMakePoint(c->x, c->y)];
                [p lineToPoint:NSMakePoint(c->w, c->h)];
                [p setLineWidth:c->lw];
                [p setLineCapStyle:NSLineCapStyleRound];
                [p stroke];
                break;
            }
            case GCMD_TEXT: {
                if (!c->text || !c->text[0]) break;
                NSString* s = [NSString stringWithUTF8String:c->text];
                if (!s) break;
                NSFont* font = make_gui_font(c->rad, c->bold, c->mono);
                NSColor* col = mk_color(c->r, c->g, c->b, c->a);
                NSDictionary* attrs = [NSDictionary dictionaryWithObjectsAndKeys:
                    font, NSFontAttributeName,
                    col, NSForegroundColorAttributeName,
                    nil];
                NSSize sz = [s sizeWithAttributes:attrs];
                double dx = c->x;
                double dy = c->y;
                if (c->align == 1 || c->align == 3) dx = c->x - sz.width * 0.5;
                else if (c->align == 2 || c->align == 5) dx = c->x - sz.width;
                if (c->align == 3 || c->align == 4 || c->align == 5) dy = c->y - sz.height * 0.5;
                [s drawAtPoint:NSMakePoint(dx, dy) withAttributes:attrs];
                break;
            }
        }
    }
    while (clip_depth > 0) {
        [NSGraphicsContext restoreGraphicsState];
        clip_depth--;
    }
}

@implementation ThrumGuiView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (_st) draw_disp_cmds(_st, [self bounds]);
}

- (void)pushMouse:(NSEvent*)ev kind:(int)k button:(int)btn {
    if (!_st) return;
    NSPoint p = [self convertPoint:[ev locationInWindow] fromView:nil];
    TGuiEv ge = {0};
    ge.kind = k;
    ge.x = p.x;
    ge.y = p.y;
    ge.button = btn;
    ge.clicks = (int)[ev clickCount];
    push_ev(_st, ge);
}

- (void)mouseDown:(NSEvent*)ev { [self pushMouse:ev kind:1 button:1]; }
- (void)rightMouseDown:(NSEvent*)ev { [self pushMouse:ev kind:1 button:2]; }
- (void)mouseUp:(NSEvent*)ev { [self pushMouse:ev kind:2 button:1]; }
- (void)rightMouseUp:(NSEvent*)ev { [self pushMouse:ev kind:2 button:2]; }
- (void)mouseMoved:(NSEvent*)ev { [self pushMouse:ev kind:3 button:0]; }
- (void)mouseDragged:(NSEvent*)ev { [self pushMouse:ev kind:4 button:1]; }

- (void)scrollWheel:(NSEvent*)ev {
    if (!_st) return;
    NSPoint p = [self convertPoint:[ev locationInWindow] fromView:nil];
    double dy = [ev scrollingDeltaY];
    double dx = [ev scrollingDeltaX];
    if (![ev hasPreciseScrollingDeltas]) {
        dy *= 18.0;
        dx *= 18.0;
    }
    TGuiEv ge = {0};
    ge.kind = 7;
    ge.x = p.x;
    ge.y = p.y;
    ge.dx = dx;
    ge.dy = dy;
    push_ev(_st, ge);
}

- (void)keyDown:(NSEvent*)ev {
    if (!_st) return;
    NSString* chars = [ev charactersIgnoringModifiers];
    if (!chars || [chars length] == 0) return;
    unichar uc = [chars characterAtIndex:0];
    const char* name = NULL;
    char utf8[16] = {0};
    if (uc == NSUpArrowFunctionKey) name = "up";
    else if (uc == NSDownArrowFunctionKey) name = "down";
    else if (uc == NSLeftArrowFunctionKey) name = "left";
    else if (uc == NSRightArrowFunctionKey) name = "right";
    else if (uc == NSPageUpFunctionKey) name = "pageup";
    else if (uc == NSPageDownFunctionKey) name = "pagedown";
    else if (uc == NSHomeFunctionKey) name = "home";
    else if (uc == NSEndFunctionKey) name = "end";
    else if (uc == '\r' || uc == '\n') name = "enter";
    else if (uc == ' ') name = "space";
    else if (uc == 0x1b) name = "esc";
    else if (uc == '\t') name = "tab";
    else if (uc == 0x7f || uc == 0x08 || uc == NSBackspaceCharacter || uc == NSDeleteFunctionKey) name = "backspace";
    else {
        const char* raw = [[chars lowercaseString] UTF8String];
        if (raw && raw[0]) {
            snprintf(utf8, sizeof(utf8), "%s", raw);
            name = utf8;
        }
    }
    if (!name) return;
    NSEventModifierFlags mods = [ev modifierFlags];
    TGuiEv ge = {0};
    ge.kind = 5;
    ge.key = strdup(name);
    ge.cmd = (mods & NSEventModifierFlagCommand) != 0;
    ge.shift = (mods & NSEventModifierFlagShift) != 0;
    NSString* full = [ev characters];
    const char* ts = (full && [full length] > 0) ? [full UTF8String] : NULL;
    ge.typed = ts ? strdup(ts) : strdup("");
    push_ev(_st, ge);
}
@end

@implementation ThrumWinDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (_st) {
        _st->open = false;
        TGuiEv ge = {0};
        ge.kind = 0;
        push_ev(_st, ge);
    }
    return YES;
}

- (void)windowDidResize:(NSNotification*)note {
    (void)note;
    if (_st && _st->view) {
        NSSize sz = [_st->view bounds].size;
        _st->w = sz.width;
        _st->h = sz.height;
        TGuiEv ge = {0};
        ge.kind = 6;
        ge.x = sz.width;
        ge.y = sz.height;
        push_ev(_st, ge);
    }
}

- (void)requestQuit:(id)sender {
    (void)sender;
    if (_st) {
        _st->open = false;
        TGuiEv ge = {0};
        ge.kind = 0;
        push_ev(_st, ge);
    }
}
@end

static void ensure_menu(ThrumWinDelegate* del) {
    static bool built = false;
    NSMenu* menubar = [NSMenu new];
    NSMenuItem* appItem = [NSMenuItem new];
    [menubar addItem:appItem];
    NSMenu* appMenu = [NSMenu new];
    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit" action:@selector(requestQuit:) keyEquivalent:@"q"];
    [quitItem setTarget:del];
    [appMenu addItem:quitItem];
    [appItem setSubmenu:appMenu];
    [NSApp setMainMenu:menubar];
    if (!built) built = true;
}

static void gui_close_window(TGuiState* st) {
    if (!st) return;
    @autoreleasepool {
        if (st->view) st->view->_st = NULL;
        if (st->del) st->del->_st = NULL;
        if (st->win) {
            [st->win setDelegate:nil];
            [st->win orderOut:nil];
            [st->win release];
            st->win = nil;
        }
        if (st->view) { [st->view release]; st->view = nil; }
        if (st->del) { [st->del release]; st->del = nil; }
    }
    st->open = false;
}

static void gui_cleanup(TRuntime* rt, void* state) {
    (void)rt;
    gui_close_window((TGuiState*)state);
}

static void gui_free_state(void* state) {
    TGuiState* st = (TGuiState*)state;
    if (!st) return;
    gui_close_window(st);
    cmd_buf_free(&st->staging);
    cmd_buf_free(&st->disp);
    events_clear(st);
    free(st->events);
    free(st);
}

static const char* need_str(TRuntime* rt, TVal v, const char* what, int line, int col) {
    if (v.kind != TV_OBJ || v.as.obj->kind != OBJ_STR) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s needs text, got %s", what, thrum_tname(v));
        thrum_error(rt, line, col, msg, NULL);
    }
    return ((TStr*)v.as.obj)->data;
}

static TVal gui_open(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TGuiState* st = (TGuiState*)ctx;
    const char* title = need_str(rt, argv[0], "gui.open title", line, col);
    double w = thrum_need_num(rt, argv[1], line, col);
    double h = thrum_need_num(rt, argv[2], line, col);
    if (w < 64.0) w = 64.0;
    if (h < 64.0) h = 64.0;

    if (st->win) gui_close_window(st);
    st->w = w;
    st->h = h;
    st->open = true;

    const char* hl = getenv("THRUM_GUI_HEADLESS");
    st->headless = (hl && strcmp(hl, "1") == 0);
    if (st->headless) return thrum_nil();

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect rect = NSMakeRect(0, 0, w, h);
        NSWindowStyleMask mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        NSWindow* win = [[NSWindow alloc] initWithContentRect:rect
                                                    styleMask:mask
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        [win setReleasedWhenClosed:NO];
        [win setTitle:[NSString stringWithUTF8String:title]];
        [win setMinSize:NSMakeSize(960, 600)];
        [win setAcceptsMouseMovedEvents:YES];

        ThrumGuiView* view = [[ThrumGuiView alloc] initWithFrame:rect];
        view->_st = st;
        ThrumWinDelegate* del = [[ThrumWinDelegate alloc] init];
        del->_st = st;

        [win setContentView:view];
        [win makeFirstResponder:view];
        [win setDelegate:del];
        [win center];
        ensure_menu(del);
        [win makeKeyAndOrderFront:nil];
        if (@available(macOS 14.0, *)) {
            [NSApp activate];
        } else {
            #pragma clang diagnostic push
            #pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [NSApp activateIgnoringOtherApps:YES];
            #pragma clang diagnostic pop
        }
        [NSApp finishLaunching];

        st->win = win;
        st->view = view;
        st->del = del;
    }
    return thrum_nil();
}

static TVal gui_is_open(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)rt; (void)argc; (void)argv; (void)line; (void)col;
    return thrum_bool(((TGuiState*)ctx)->open);
}

static TVal gui_close(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)rt; (void)argc; (void)argv; (void)line; (void)col;
    gui_close_window((TGuiState*)ctx);
    return thrum_nil();
}

static TVal gui_size(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    TGuiState* st = (TGuiState*)ctx;
    T_FRAME(rt);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* m = (TMap*)mp.as.obj;
    thrum_map_set_cs(rt, m, "width", thrum_num_dimless(st->w));
    thrum_map_set_cs(rt, m, "height", thrum_num_dimless(st->h));
    T_END_FRAME(rt);
    return mp;
}

static void pump_cocoa_events(TGuiState* st, double wait_sec) {
    if (!st || st->headless || !st->win) return;
    @autoreleasepool {
        NSDate* limit = (wait_sec > 0.0) ? [NSDate dateWithTimeIntervalSinceNow:wait_sec] : nil;
        NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:limit
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES];
        if (ev) {
            [NSApp sendEvent:ev];
            [NSApp updateWindows];
            while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:nil
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES]) != nil) {
                [NSApp sendEvent:ev];
            }
            [NSApp updateWindows];
        }
    }
}

static TVal gui_poll(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc; (void)argv; (void)line; (void)col;
    TGuiState* st = (TGuiState*)ctx;
    pump_cocoa_events(st, 0.0);

    T_FRAME(rt);
    T_VAR(rt, out, thrum_list(rt, st->ev_len));
    TList* l = (TList*)out.as.obj;
    for (size_t i = 0; i < st->ev_len; i++) {
        TGuiEv* e = &st->events[i];
        T_VAR(rt, mp, thrum_map(rt));
        TMap* m = (TMap*)mp.as.obj;
        const char* kstr = "quit";
        if (e->kind == 1) kstr = "down";
        else if (e->kind == 2) kstr = "up";
        else if (e->kind == 3) kstr = "move";
        else if (e->kind == 4) kstr = "drag";
        else if (e->kind == 5) kstr = "key";
        else if (e->kind == 6) kstr = "resize";
        else if (e->kind == 7) kstr = "wheel";
        thrum_map_set_cs(rt, m, "kind", thrum_cstr(rt, kstr));
        if (e->kind >= 1 && e->kind <= 4) {
            thrum_map_set_cs(rt, m, "x", thrum_num_dimless(e->x));
            thrum_map_set_cs(rt, m, "y", thrum_num_dimless(e->y));
            thrum_map_set_cs(rt, m, "button", thrum_num_dimless((double)e->button));
            thrum_map_set_cs(rt, m, "clicks", thrum_num_dimless((double)e->clicks));
        } else if (e->kind == 5) {
            thrum_map_set_cs(rt, m, "key", thrum_cstr(rt, e->key ? e->key : ""));
            thrum_map_set_cs(rt, m, "text", thrum_cstr(rt, e->typed ? e->typed : ""));
            thrum_map_set_cs(rt, m, "cmd", thrum_bool(e->cmd));
            thrum_map_set_cs(rt, m, "shift", thrum_bool(e->shift));
        } else if (e->kind == 6) {
            thrum_map_set_cs(rt, m, "width", thrum_num_dimless(e->x));
            thrum_map_set_cs(rt, m, "height", thrum_num_dimless(e->y));
        } else if (e->kind == 7) {
            thrum_map_set_cs(rt, m, "x", thrum_num_dimless(e->x));
            thrum_map_set_cs(rt, m, "y", thrum_num_dimless(e->y));
            thrum_map_set_cs(rt, m, "dx", thrum_num_dimless(e->dx));
            thrum_map_set_cs(rt, m, "dy", thrum_num_dimless(e->dy));
        }
        thrum_list_push(rt, l, mp);
    }
    events_clear(st);
    T_END_FRAME(rt);
    return out;
}

static TVal gui_wait(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TGuiState* st = (TGuiState*)ctx;
    double ms = thrum_need_num(rt, argv[0], line, col);
    if (ms < 0.0) ms = 0.0;
    if (st->headless || !st->win) return thrum_nil();
    if (st->ev_len == 0) {
        pump_cocoa_events(st, ms / 1000.0);
    }
    return thrum_nil();
}

static TVal gui_begin(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)rt; (void)argc; (void)argv; (void)line; (void)col;
    TGuiState* st = (TGuiState*)ctx;
    cmd_buf_clear(&st->staging);
    return thrum_nil();
}

static TVal gui_clear(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmd c = {0};
    c.kind = GCMD_CLEAR;
    c.r = thrum_need_num(rt, argv[0], line, col);
    c.g = thrum_need_num(rt, argv[1], line, col);
    c.b = thrum_need_num(rt, argv[2], line, col);
    c.a = 255.0;
    st->bg_r = c.r; st->bg_g = c.g; st->bg_b = c.b;
    cmd_buf_push(&st->staging, c);
    return thrum_nil();
}

static TVal gui_rect(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmd c = {0};
    c.kind = GCMD_RECT;
    c.x = thrum_need_num(rt, argv[0], line, col);
    c.y = thrum_need_num(rt, argv[1], line, col);
    c.w = thrum_need_num(rt, argv[2], line, col);
    c.h = thrum_need_num(rt, argv[3], line, col);
    c.r = thrum_need_num(rt, argv[4], line, col);
    c.g = thrum_need_num(rt, argv[5], line, col);
    c.b = thrum_need_num(rt, argv[6], line, col);
    c.a = (argc > 7) ? thrum_need_num(rt, argv[7], line, col) : 255.0;
    cmd_buf_push(&st->staging, c);
    return thrum_nil();
}

static TVal gui_round_rect(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmd c = {0};
    c.kind = GCMD_ROUND_RECT;
    c.x = thrum_need_num(rt, argv[0], line, col);
    c.y = thrum_need_num(rt, argv[1], line, col);
    c.w = thrum_need_num(rt, argv[2], line, col);
    c.h = thrum_need_num(rt, argv[3], line, col);
    c.rad = thrum_need_num(rt, argv[4], line, col);
    c.r = thrum_need_num(rt, argv[5], line, col);
    c.g = thrum_need_num(rt, argv[6], line, col);
    c.b = thrum_need_num(rt, argv[7], line, col);
    c.a = (argc > 8) ? thrum_need_num(rt, argv[8], line, col) : 255.0;
    cmd_buf_push(&st->staging, c);
    return thrum_nil();
}

static TVal gui_stroke_rect(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmd c = {0};
    c.kind = GCMD_STROKE_RECT;
    c.x = thrum_need_num(rt, argv[0], line, col);
    c.y = thrum_need_num(rt, argv[1], line, col);
    c.w = thrum_need_num(rt, argv[2], line, col);
    c.h = thrum_need_num(rt, argv[3], line, col);
    c.rad = thrum_need_num(rt, argv[4], line, col);
    c.lw = thrum_need_num(rt, argv[5], line, col);
    c.r = thrum_need_num(rt, argv[6], line, col);
    c.g = thrum_need_num(rt, argv[7], line, col);
    c.b = thrum_need_num(rt, argv[8], line, col);
    c.a = (argc > 9) ? thrum_need_num(rt, argv[9], line, col) : 255.0;
    cmd_buf_push(&st->staging, c);
    return thrum_nil();
}

static TVal gui_circle(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmd c = {0};
    c.kind = GCMD_CIRCLE;
    c.x = thrum_need_num(rt, argv[0], line, col);
    c.y = thrum_need_num(rt, argv[1], line, col);
    c.rad = thrum_need_num(rt, argv[2], line, col);
    c.r = thrum_need_num(rt, argv[3], line, col);
    c.g = thrum_need_num(rt, argv[4], line, col);
    c.b = thrum_need_num(rt, argv[5], line, col);
    c.a = (argc > 6) ? thrum_need_num(rt, argv[6], line, col) : 255.0;
    cmd_buf_push(&st->staging, c);
    return thrum_nil();
}

static TVal gui_line(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmd c = {0};
    c.kind = GCMD_LINE;
    c.x = thrum_need_num(rt, argv[0], line, col);
    c.y = thrum_need_num(rt, argv[1], line, col);
    c.w = thrum_need_num(rt, argv[2], line, col);
    c.h = thrum_need_num(rt, argv[3], line, col);
    c.lw = thrum_need_num(rt, argv[4], line, col);
    c.r = thrum_need_num(rt, argv[5], line, col);
    c.g = thrum_need_num(rt, argv[6], line, col);
    c.b = thrum_need_num(rt, argv[7], line, col);
    c.a = (argc > 8) ? thrum_need_num(rt, argv[8], line, col) : 255.0;
    cmd_buf_push(&st->staging, c);
    return thrum_nil();
}

static TVal gui_text(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmd c = {0};
    c.kind = GCMD_TEXT;
    c.x = thrum_need_num(rt, argv[0], line, col);
    c.y = thrum_need_num(rt, argv[1], line, col);
    const char* s = need_str(rt, argv[2], "gui.text", line, col);
    c.text = strdup(s);
    c.rad = thrum_need_num(rt, argv[3], line, col);
    c.r = thrum_need_num(rt, argv[4], line, col);
    c.g = thrum_need_num(rt, argv[5], line, col);
    c.b = thrum_need_num(rt, argv[6], line, col);
    c.align = 0;
    if (argc > 7) {
        const char* al = need_str(rt, argv[7], "gui.text align", line, col);
        if (strcmp(al, "center") == 0) c.align = 1;
        else if (strcmp(al, "right") == 0) c.align = 2;
        else if (strcmp(al, "mid") == 0) c.align = 3;
        else if (strcmp(al, "lmid") == 0) c.align = 4;
        else if (strcmp(al, "rmid") == 0) c.align = 5;
    }
    c.bold = (argc > 8) ? thrum_truthy(argv[8]) : false;
    c.a = (argc > 9) ? thrum_need_num(rt, argv[9], line, col) : 255.0;
    c.mono = (argc > 10) ? thrum_truthy(argv[10]) : false;
    cmd_buf_push(&st->staging, c);
    return thrum_nil();
}

static TVal gui_clip(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmd c = {0};
    c.kind = GCMD_CLIP;
    c.x = thrum_need_num(rt, argv[0], line, col);
    c.y = thrum_need_num(rt, argv[1], line, col);
    c.w = thrum_need_num(rt, argv[2], line, col);
    c.h = thrum_need_num(rt, argv[3], line, col);
    cmd_buf_push(&st->staging, c);
    return thrum_nil();
}

static TVal gui_unclip(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)rt; (void)argc; (void)argv; (void)line; (void)col;
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmd c = {0};
    c.kind = GCMD_UNCLIP;
    cmd_buf_push(&st->staging, c);
    return thrum_nil();
}

static TVal gui_measure(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx;
    const char* s = need_str(rt, argv[0], "gui.measure", line, col);
    double size = thrum_need_num(rt, argv[1], line, col);
    bool bold = (argc > 2) ? thrum_truthy(argv[2]) : false;
    bool mono = (argc > 3) ? thrum_truthy(argv[3]) : false;
    double w = 0.0, h = size * 1.25;
    @autoreleasepool {
        NSString* ns = [NSString stringWithUTF8String:s];
        if (ns) {
            NSFont* font = make_gui_font(size, bold, mono);
            NSDictionary* attrs = [NSDictionary dictionaryWithObjectsAndKeys:font, NSFontAttributeName, nil];
            NSSize sz = [ns sizeWithAttributes:attrs];
            w = sz.width;
            h = sz.height;
        }
    }
    T_FRAME(rt);
    T_VAR(rt, mp, thrum_map(rt));
    TMap* m = (TMap*)mp.as.obj;
    thrum_map_set_cs(rt, m, "width", thrum_num_dimless(w));
    thrum_map_set_cs(rt, m, "height", thrum_num_dimless(h));
    T_END_FRAME(rt);
    return mp;
}

static TVal gui_clipboard(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc; (void)argv; (void)line; (void)col;
    TVal out = thrum_cstr(rt, "");
    @autoreleasepool {
        NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        if (s) {
            const char* u = [s UTF8String];
            if (u) out = thrum_cstr(rt, u);
        }
    }
    return out;
}

static TVal gui_set_clipboard(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)ctx; (void)argc;
    const char* s = need_str(rt, argv[0], "gui.set_clipboard", line, col);
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        NSString* ns = [NSString stringWithUTF8String:s];
        if (ns) [pb setString:ns forType:NSPasteboardTypeString];
    }
    return thrum_nil();
}

static TVal gui_commit(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)rt; (void)argc; (void)argv; (void)line; (void)col;
    TGuiState* st = (TGuiState*)ctx;
    TGuiCmdBuf tmp = st->disp;
    st->disp = st->staging;
    st->staging = tmp;
    cmd_buf_clear(&st->staging);

    if (!st->headless && st->win && st->view) {
        @autoreleasepool {
            [st->view setNeedsDisplay:YES];
            [st->win displayIfNeeded];
        }
    }
    return thrum_nil();
}

static TVal gui_screenshot(TRuntime* rt, void* ctx, int argc, TVal* argv, int line, int col) {
    (void)argc;
    TGuiState* st = (TGuiState*)ctx;
    const char* path = need_str(rt, argv[0], "gui.screenshot path", line, col);
    int w = (int)ceil(st->w > 0.0 ? st->w : 800.0);
    int h = (int)ceil(st->h > 0.0 ? st->h : 600.0);
    int scale = 2;
    int pxw = w * scale;
    int pxh = h * scale;

    @autoreleasepool {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:pxw
                          pixelsHigh:pxh
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSCalibratedRGBColorSpace
                         bytesPerRow:0
                        bitsPerPixel:0];
        NSGraphicsContext* base = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        CGContextRef cg = [base CGContext];
        CGContextSaveGState(cg);
        CGContextTranslateCTM(cg, 0.0, (CGFloat)pxh);
        CGContextScaleCTM(cg, (CGFloat)scale, -(CGFloat)scale);
        NSGraphicsContext* flipped = [NSGraphicsContext graphicsContextWithCGContext:cg flipped:YES];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:flipped];
        draw_disp_cmds(st, NSMakeRect(0, 0, (CGFloat)w, (CGFloat)h));
        [NSGraphicsContext restoreGraphicsState];
        CGContextRestoreGState(cg);

        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                        properties:[NSDictionary dictionary]];
        BOOL ok = [png writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
        [rep release];
        if (!ok) {
            thrum_error(rt, line, col, "failed to write screenshot PNG", NULL);
        }
    }
    return thrum_nil();
}

TVal thrum_cap_gui_create(TRuntime* rt, int line, int col) {
    (void)line; (void)col;
    TGuiState* st = (TGuiState*)calloc(1, sizeof(TGuiState));
    st->w = 800.0;
    st->h = 600.0;
    st->bg_r = 20.0; st->bg_g = 86.0; st->bg_b = 54.0;
    TCap* c = thrum_alloc_cap(rt, "gui", gui_cleanup, gui_free_state, st);
    thrum_cap_add_method(rt, c, "open", gui_open, 3, 3);
    thrum_cap_add_method(rt, c, "is_open", gui_is_open, 0, 0);
    thrum_cap_add_method(rt, c, "close", gui_close, 0, 0);
    thrum_cap_add_method(rt, c, "size", gui_size, 0, 0);
    thrum_cap_add_method(rt, c, "poll", gui_poll, 0, 0);
    thrum_cap_add_method(rt, c, "wait", gui_wait, 1, 1);
    thrum_cap_add_method(rt, c, "begin", gui_begin, 0, 0);
    thrum_cap_add_method(rt, c, "clear", gui_clear, 3, 3);
    thrum_cap_add_method(rt, c, "rect", gui_rect, 7, 8);
    thrum_cap_add_method(rt, c, "round_rect", gui_round_rect, 8, 9);
    thrum_cap_add_method(rt, c, "stroke_rect", gui_stroke_rect, 9, 10);
    thrum_cap_add_method(rt, c, "circle", gui_circle, 6, 7);
    thrum_cap_add_method(rt, c, "line", gui_line, 8, 9);
    thrum_cap_add_method(rt, c, "text", gui_text, 7, 11);
    thrum_cap_add_method(rt, c, "clip", gui_clip, 4, 4);
    thrum_cap_add_method(rt, c, "unclip", gui_unclip, 0, 0);
    thrum_cap_add_method(rt, c, "measure", gui_measure, 2, 4);
    thrum_cap_add_method(rt, c, "clipboard", gui_clipboard, 0, 0);
    thrum_cap_add_method(rt, c, "set_clipboard", gui_set_clipboard, 1, 1);
    thrum_cap_add_method(rt, c, "commit", gui_commit, 0, 0);
    thrum_cap_add_method(rt, c, "screenshot", gui_screenshot, 1, 1);
    return thrum_obj((TObj*)c);
}
