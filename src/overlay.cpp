// The in-game menu. Three-finger tap opens or closes it. It is drawn with our own GL right
// before each frame is presented, saving and restoring every piece of GL state it touches:
// the engine caches GL state (fuseGL::P3DStateMan) and would draw the next frame wrong.
// Coordinates are surface pixels, origin top-left, the same space touch events arrive in.
#include <GLES2/gl2.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "rr2.h"
#include "font.inc"

// ---- the menu: a main page of sections; add rows to a page's list ----
static void close_menu();
static void go(int page);
using S = std::string;
struct Item { S (*label)(); void (*tap)(); bool confirm = false; };  // confirm: needs a second tap
struct Page { const char* title; std::vector<Item> items; };
static const Item kBack = {[] { return S("< Back"); }, [] { go(0); }};
// The physics lab is a developer tool: its row appears in Settings only with the "lab" setting
// on (rr2.cfg "lab=1", or adb shell setprop debug.rr2.lab 1).
static std::vector<Page> with_lab(std::vector<Page> p) {
    if (setting("lab", 0)) p[1].items.insert(p[1].items.end() - 1, Item{
        [] { return S("Physics lab (log): ") + (lab() ? "ON" : "OFF"); },
        [] { on_game_thread([] { set_lab(!lab()); }); }});
    return p;
}
static const std::vector<Page> kPages = with_lab({
    {"RECKLESS RACING 2", {
        {[] { return S("Settings  >"); }, [] { go(1); }},
        {[] { return S("Cheats  >"); }, [] { go(2); }},
        {[] { return S("Save data  >"); }, [] { go(3); }},
        {[] { return S("Close"); }, [] { close_menu(); }},
    }},
    {"SETTINGS", {
        {[] { return "Frame rate: " + std::to_string(tick_rate()) + " FPS"; },
         [] { set_tick_rate(tick_rate() == 30 ? 60 : 30); }},
        {[] { return "Grip fix: " + std::to_string(grip_fix()) + "%"; },
         [] { set_grip_fix((grip_fix() / 10 * 10 + 10) % 110); }},  // snaps old 25%-step values
        {[] { return "Traction fix: " + std::to_string(traction_fix()) + "%"; },
         [] { set_traction_fix((traction_fix() / 5 * 5 + 5) % 105); }},
        {[] { return S("Supersampling: ") + (supersampling() ? "ON" : "OFF"); },
         [] { set_supersampling(!supersampling()); }},
        {[] { return S("Gamepad mode: ") + (gamepad_mode() ? "ON" : "OFF"); },
         [] { set_gamepad_mode(!gamepad_mode()); }},
        kBack,
    }},
    {"CHEATS", {
        {[] { return "Give $100,000" + (last_cash() < 0 ? S() : " (now $" + std::to_string(last_cash()) + ")"); },
         [] { on_game_thread([] { cheat_cash(100000); }); }},
        {[] { return S("Unlock all cars and tracks"); }, [] { on_game_thread(cheat_unlock_all); }},
        kBack,
    }},
    {"SAVE DATA", {
        {[] { return "Back up save (last: " + backup_info() + ")"; }, [] { on_game_thread(backup_save); }},
        {[] { return S("Restore backup (closes game)"); }, [] { on_game_thread(restore_save); }, true},
        {[] { return S("Reset save (closes game)"); }, [] { on_game_thread(reset_save); }, true},
        kBack,
    }},
});
static int max_rows() { size_t n = 0; for (auto& p : kPages) n = std::max(n, p.items.size()); return int(n); }

static std::atomic<bool> g_open;
static std::atomic<int> g_page, g_armed{-1};  // current page; row waiting for its confirming tap
static std::atomic<int> g_surface_w, g_surface_h;
static std::atomic<int> g_x, g_y, g_w, g_row;  // layout from the last draw, for hit-testing
static void go(int page) { g_page = page, g_armed = -1; }
static void close_menu() { g_open = false, go(0); }

void overlay_surface(int w, int h) { g_surface_w = w, g_surface_h = h; }

// ---- input (UI thread). Returns true if the event is the menu's, not the game's ----
bool overlay_touch(int x, int y, int packed) {
    int action = packed & 0xff, pointer = packed >> 16;  // MotionEvent action, pointer id + 1
    static bool multi;                                   // >1 finger since the last first-down
    if (action == 0) multi = false;
    if (action == 5) multi = true;
    if (action == 5 && pointer == 3) return g_open = !g_open, true;  // third finger
    if (!g_open) return false;
    if (action == 1 && !multi) {  // single-finger tap released: hit-test the rows
        auto& items = kPages[g_page].items;
        int row = (y - g_y) / std::max(1, int(g_row)) - 1;  // row 0 is the title
        bool hit = x >= g_x && x < g_x + g_w && y >= g_y && row >= 0 && row < int(items.size());
        if (hit && items[row].confirm && g_armed != row) g_armed = row;  // first tap only arms it
        else if (hit) g_armed = -1, items[row].tap();
        else g_armed = -1;
    }
    return action != 1 && action != 6 && action != 3;  // releases always reach the game
}

// ---- drawing (game thread, GL context current) ----
static GLuint g_prog, g_tex;
static GLint g_u_screen, g_u_color;
static std::vector<float> g_verts;  // x, y, u, v per vertex

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

static void init_gl() {
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER,
        "attribute vec2 a_pos; attribute vec2 a_uv; uniform vec2 u_screen; varying vec2 v_uv;"
        "void main() { v_uv = a_uv;"
        "  gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0); }"));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER,
        "precision mediump float; uniform sampler2D u_tex; uniform vec4 u_color; varying vec2 v_uv;"
        "void main() { gl_FragColor = vec4(u_color.rgb, u_color.a * texture2D(u_tex, v_uv).a); }"));
    glBindAttribLocation(g_prog, 0, "a_pos");
    glBindAttribLocation(g_prog, 1, "a_uv");
    glLinkProgram(g_prog);
    g_u_screen = glGetUniformLocation(g_prog, "u_screen");
    g_u_color = glGetUniformLocation(g_prog, "u_color");

    std::vector<unsigned char> alpha(kFontW * kFontH);
    for (int i = 0; i < kFontW * kFontH; i++) alpha[i] = (kFontBits[i >> 3] >> (i & 7)) & 1 ? 255 : 0;
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, kFontW, kFontH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, alpha.data());
    for (GLenum p : {GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER}) glTexParameteri(GL_TEXTURE_2D, p, GL_NEAREST);
    for (GLenum p : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T}) glTexParameteri(GL_TEXTURE_2D, p, GL_CLAMP_TO_EDGE);
}

// Queue a quad showing atlas cell `c` (char 127 = solid) at x, y with size w, h.
static void quad(float x, float y, float w, float h, int c) {
    int cell = std::clamp(c, 32, 127) - 32;
    float u0 = float(cell % 16 * kFontCellW) / kFontW, v0 = float(cell / 16 * kFontCellH) / kFontH;
    float u1 = u0 + float(kFontCellW) / kFontW, v1 = v0 + float(kFontCellH) / kFontH;
    float q[] = {x, y, u0, v0, x + w, y, u1, v0, x, y + h, u0, v1,
                 x + w, y, u1, v0, x + w, y + h, u1, v1, x, y + h, u0, v1};
    g_verts.insert(g_verts.end(), q, q + 24);
}

static void flush(float r, float g, float b, float a) {
    glUniform4f(g_u_color, r, g, b, a);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, g_verts.data());
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, g_verts.data() + 2);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(g_verts.size() / 4));
    g_verts.clear();
}

static void text_centered(const std::string& s, float cx, float cy, float h) {
    float w = h * kFontCellW / kFontCellH;
    for (size_t i = 0; i < s.size(); i++) quad(cx - w * s.size() / 2 + w * i, cy - h / 2, w, h, s[i]);
}

static void draw_menu(int W, int H) {
    auto& page = kPages[g_page];
    int rows = int(page.items.size()), row = H / (max_rows() + 3);
    int w = std::max(W * 6 / 10, row * 8), x = (W - w) / 2, y = (H - row * (rows + 1)) / 2;
    g_x = x, g_y = y, g_w = w, g_row = row;
    quad(0, 0, W, H, 127), flush(0, 0, 0, 0.45f);                              // dim the game
    quad(x, y, w, row * (rows + 1), 127), flush(0.08f, 0.08f, 0.1f, 0.92f);  // panel
    for (int i = 0; i < rows; i++) quad(x + row / 8, y + row * (i + 1) + row / 8, w - row / 4, row * 3 / 4, 127);
    flush(0.22f, 0.24f, 0.3f, 1);                                             // buttons
    text_centered(page.title, W / 2.0f, y + row / 2.0f, row * 0.45f);
    flush(1, 0.75f, 0.2f, 1);                                                 // title
    for (int i = 0; i < rows; i++)
        text_centered(i == g_armed ? S("Tap again to confirm") : page.items[i].label(), W / 2.0f, y + row * (i + 1.5f), row * 0.4f);
    flush(1, 1, 1, 1);
}

// Small banner at the top while the physics lab runs, so the active configuration is visible.
static void draw_label(const std::string& s, int W, int H) {
    float h = H / 28.0f, w = h * kFontCellW / kFontCellH * s.size();
    quad(W / 2.0f - w / 2 - h / 2, h * 0.5f, w + h, h * 1.5f, 127), flush(0, 0, 0, 0.5f);
    text_centered(s, W / 2.0f, h * 1.25f, h), flush(1, 0.9f, 0.3f, 1);
}

void overlay_draw() {
    std::string label = lab_label();
    if (!g_open && label.empty()) return;
    // save everything we are about to change
    GLint prog, abuf, fbo, active, tex, vp[4], bsrc, bdst, bsrca, bdsta, beq, beqa, unpack;
    GLboolean mask[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog), glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo), glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_VIEWPORT, vp), glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc), glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrca), glGetIntegerv(GL_BLEND_DST_ALPHA, &bdsta);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq), glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &beqa);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST};
    GLboolean was[5];
    for (int i = 0; i < 5; i++) was[i] = glIsEnabled(caps[i]);
    struct { GLint on, size, type, norm, stride, buf; void* p; } attr[2];
    for (int i = 0; i < 2; i++) {
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr[i].on);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attr[i].size);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attr[i].type);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attr[i].norm);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attr[i].stride);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attr[i].buf);
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attr[i].p);
    }

    // draw
    if (!g_prog) init_gl();
    int W = g_surface_w ? int(g_surface_w) : vp[0] + vp[2], H = g_surface_h ? int(g_surface_h) : vp[1] + vp[3];
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    for (int i = 1; i < 5; i++) glDisable(caps[i]);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g_prog);
    glUniform2f(g_u_screen, float(W), float(H));
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0), glEnableVertexAttribArray(1);
    g_open ? draw_menu(W, H) : draw_label(label, W, H);

    // restore
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, attr[i].buf);
        glVertexAttribPointer(i, attr[i].size, attr[i].type, GLboolean(attr[i].norm), attr[i].stride, attr[i].p);
        attr[i].on ? glEnableVertexAttribArray(i) : glDisableVertexAttribArray(i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, abuf);
    glBindTexture(GL_TEXTURE_2D, tex);
    glActiveTexture(active);
    glUseProgram(prog);
    for (int i = 0; i < 5; i++) was[i] ? glEnable(caps[i]) : glDisable(caps[i]);
    glBlendEquationSeparate(beq, beqa);
    glBlendFuncSeparate(bsrc, bdst, bsrca, bdsta);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}
