#include <resea/ipc.h>
#include <resea/io.h>
#include <resea/printf.h>
#include <string.h>

#define BLANK_CHAR    0x0f20 /* whitespace */
#define SCREEN_HEIGHT 25
#define SCREEN_WIDTH  80

// Colors.

static uint16_t *screen = NULL;

static void move_cursor(unsigned y, unsigned x) {
    uint16_t pos = y * SCREEN_WIDTH + x;
    io_out8(0x3d4, 0x0f);
    io_out8(0x3d5, pos & 0xff);
    io_out8(0x3d4, 0x0e);
    io_out8(0x3d5, (pos >> 8) & 0xff);
}

static void clear(void) {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            screen[y * SCREEN_WIDTH + x] = BLANK_CHAR;
        }
    }
}

static void scroll(void) {
    // Scroll by one line.
    for (int from = 1; from < SCREEN_HEIGHT; from++) {
        int dst = (from - 1) * SCREEN_WIDTH;
        int src = from * SCREEN_WIDTH;
        memcpy(&screen[dst], &screen[src], sizeof(uint16_t) * SCREEN_WIDTH);
    }

    // Clear the last line.
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        screen[(SCREEN_HEIGHT - 1) * SCREEN_WIDTH + x] = BLANK_CHAR;
    }
}

static void draw_char(unsigned y, unsigned x, char ch, enum textscreen_color fg, enum textscreen_color bg) {
    if (y >= SCREEN_HEIGHT || x >= SCREEN_WIDTH) {
        return;
    }

    screen[y * SCREEN_WIDTH + x] = (bg << 12) | (fg << 8) | ch;
}

void main(void) {
    TRACE("starting...");

    paddr_t paddr;
    screen = io_alloc_pages(1, 0xb8000, &paddr);

    ASSERT_OK(ipc_serve("display"));

    // The mainloop: receive and handle messages.
    INFO("ready");
    while (true) {
        struct message m;
        error_t err = ipc_recv(IPC_ANY, &m);
        ASSERT_OK(err);

        switch (m.type) {
            case TEXTSCREEN_DRAW_CHAR_MSG:
                draw_char(m.textscreen_draw_char.y, m.textscreen_draw_char.x, m.textscreen_draw_char.ch,
                          m.textscreen_draw_char.fg_color, m.textscreen_draw_char.bg_color);
                break;
            case TEXTSCREEN_MOVE_CURSOR_MSG:
                move_cursor(m.textscreen_move_cursor.y, m.textscreen_move_cursor.x);
                break;
            case TEXTSCREEN_CLEAR_MSG:
                clear();
                break;
            case TEXTSCREEN_SCROLL_MSG:
                scroll();
                break;
            case TEXTSCREEN_GET_SIZE_MSG: {
                m.type = TEXTSCREEN_GET_SIZE_REPLY_MSG;
                m.textscreen_get_size_reply.width = SCREEN_WIDTH;
                m.textscreen_get_size_reply.height = SCREEN_HEIGHT;
                ipc_reply(m.src, &m);
                break;
            }
            default:
                WARN("unknown message (type=%d)", m.type);
        }
    }
}
